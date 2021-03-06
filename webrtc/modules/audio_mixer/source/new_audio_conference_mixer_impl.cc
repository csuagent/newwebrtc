/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/audio_mixer/source/new_audio_conference_mixer_impl.h"

#include <algorithm>
#include <functional>

#include "webrtc/modules/audio_conference_mixer/source/audio_frame_manipulator.h"
#include "webrtc/modules/audio_mixer/include/audio_mixer_defines.h"
#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/modules/utility/include/audio_frame_operations.h"
#include "webrtc/system_wrappers/include/critical_section_wrapper.h"
#include "webrtc/system_wrappers/include/trace.h"

namespace webrtc {
namespace {

class SourceFrame {
 public:
  SourceFrame(MixerAudioSource* p, AudioFrame* a, bool m, bool was_mixed_before)
      : audio_source_(p),
        audio_frame_(a),
        muted_(m),
        was_mixed_before_(was_mixed_before) {
    if (!muted_) {
      energy_ = CalculateEnergy(*a);
    }
  }

  // a.shouldMixBefore(b) is used to select mixer participants.
  bool shouldMixBefore(const SourceFrame& other) const {
    if (muted_ != other.muted_) {
      return other.muted_;
    }

    auto our_activity = audio_frame_->vad_activity_;
    auto other_activity = other.audio_frame_->vad_activity_;

    if (our_activity != other_activity) {
      return our_activity == AudioFrame::kVadActive;
    }

    return energy_ > other.energy_;
  }

  MixerAudioSource* audio_source_;
  AudioFrame* audio_frame_;
  bool muted_;
  uint32_t energy_;
  bool was_mixed_before_;
};

// Mix |frame| into |mixed_frame|, with saturation protection and upmixing.
// These effects are applied to |frame| itself prior to mixing. Assumes that
// |mixed_frame| always has at least as many channels as |frame|. Supports
// stereo at most.
//
// TODO(andrew): consider not modifying |frame| here.
void MixFrames(AudioFrame* mixed_frame, AudioFrame* frame, bool use_limiter) {
  RTC_DCHECK_GE(mixed_frame->num_channels_, frame->num_channels_);
  if (use_limiter) {
    // Divide by two to avoid saturation in the mixing.
    // This is only meaningful if the limiter will be used.
    *frame >>= 1;
  }
  if (mixed_frame->num_channels_ > frame->num_channels_) {
    // We only support mono-to-stereo.
    RTC_DCHECK_EQ(mixed_frame->num_channels_, static_cast<size_t>(2));
    RTC_DCHECK_EQ(frame->num_channels_, static_cast<size_t>(1));
    AudioFrameOperations::MonoToStereo(frame);
  }

  *mixed_frame += *frame;
}

// Return the max number of channels from a |list| composed of AudioFrames.
size_t MaxNumChannels(const AudioFrameList* list) {
  size_t max_num_channels = 1;
  for (AudioFrameList::const_iterator iter = list->begin(); iter != list->end();
       ++iter) {
    max_num_channels = std::max(max_num_channels, (*iter).frame->num_channels_);
  }
  return max_num_channels;
}

}  // namespace

MixerAudioSource::MixerAudioSource() : _mixHistory(new NewMixHistory()) {}

MixerAudioSource::~MixerAudioSource() {
  delete _mixHistory;
}

bool MixerAudioSource::IsMixed() const {
  return _mixHistory->IsMixed();
}

NewMixHistory::NewMixHistory() : _isMixed(0) {}

NewMixHistory::~NewMixHistory() {}

bool NewMixHistory::IsMixed() const {
  return _isMixed;
}

bool NewMixHistory::WasMixed() const {
  // Was mixed is the same as is mixed depending on perspective. This function
  // is for the perspective of NewAudioConferenceMixerImpl.
  return IsMixed();
}

int32_t NewMixHistory::SetIsMixed(const bool mixed) {
  _isMixed = mixed;
  return 0;
}

void NewMixHistory::ResetMixedStatus() {
  _isMixed = false;
}

NewAudioConferenceMixer* NewAudioConferenceMixer::Create(int id) {
  NewAudioConferenceMixerImpl* mixer = new NewAudioConferenceMixerImpl(id);
  if (!mixer->Init()) {
    delete mixer;
    return NULL;
  }
  return mixer;
}

NewAudioConferenceMixerImpl::NewAudioConferenceMixerImpl(int id)
    : _id(id),
      _minimumMixingFreq(kLowestPossible),
      _outputFrequency(kDefaultFrequency),
      _sampleSize(0),
      audio_source_list_(),
      additional_audio_source_list_(),
      num_mixed_audio_sources_(0),
      use_limiter_(true),
      _timeStamp(0) {
  thread_checker_.DetachFromThread();
}

bool NewAudioConferenceMixerImpl::Init() {
  _crit.reset(CriticalSectionWrapper::CreateCriticalSection());
  if (_crit.get() == NULL)
    return false;

  _cbCrit.reset(CriticalSectionWrapper::CreateCriticalSection());
  if (_cbCrit.get() == NULL)
    return false;

  Config config;
  config.Set<ExperimentalAgc>(new ExperimentalAgc(false));
  _limiter.reset(AudioProcessing::Create(config));
  if (!_limiter.get())
    return false;

  if (SetOutputFrequency(kDefaultFrequency) == -1)
    return false;

  if (_limiter->gain_control()->set_mode(GainControl::kFixedDigital) !=
      _limiter->kNoError)
    return false;

  // We smoothly limit the mixed frame to -7 dbFS. -6 would correspond to the
  // divide-by-2 but -7 is used instead to give a bit of headroom since the
  // AGC is not a hard limiter.
  if (_limiter->gain_control()->set_target_level_dbfs(7) != _limiter->kNoError)
    return false;

  if (_limiter->gain_control()->set_compression_gain_db(0) !=
      _limiter->kNoError)
    return false;

  if (_limiter->gain_control()->enable_limiter(true) != _limiter->kNoError)
    return false;

  if (_limiter->gain_control()->Enable(true) != _limiter->kNoError)
    return false;

  return true;
}

void NewAudioConferenceMixerImpl::Mix(AudioFrame* audio_frame_for_mixing) {
  size_t remainingAudioSourcesAllowedToMix = kMaximumAmountOfMixedAudioSources;
  RTC_DCHECK(thread_checker_.CalledOnValidThread());
  AudioFrameList mixList;
  AudioFrameList additionalFramesList;
  std::map<int, MixerAudioSource*> mixedAudioSourcesMap;
  {
    CriticalSectionScoped cs(_cbCrit.get());

    int32_t lowFreq = GetLowestMixingFrequency();
    // SILK can run in 12 kHz and 24 kHz. These frequencies are not
    // supported so use the closest higher frequency to not lose any
    // information.
    // TODO(aleloi): this is probably more appropriate to do in
    //                GetLowestMixingFrequency().
    if (lowFreq == 12000) {
      lowFreq = 16000;
    } else if (lowFreq == 24000) {
      lowFreq = 32000;
    }
    if (lowFreq <= 0) {
      return;
    } else {
      switch (lowFreq) {
        case 8000:
          if (OutputFrequency() != kNbInHz) {
            SetOutputFrequency(kNbInHz);
          }
          break;
        case 16000:
          if (OutputFrequency() != kWbInHz) {
            SetOutputFrequency(kWbInHz);
          }
          break;
        case 32000:
          if (OutputFrequency() != kSwbInHz) {
            SetOutputFrequency(kSwbInHz);
          }
          break;
        case 48000:
          if (OutputFrequency() != kFbInHz) {
            SetOutputFrequency(kFbInHz);
          }
          break;
        default:
          RTC_NOTREACHED();
          return;
      }
    }

    mixList = UpdateToMix(remainingAudioSourcesAllowedToMix);
    remainingAudioSourcesAllowedToMix -= mixList.size();
    GetAdditionalAudio(&additionalFramesList);
  }

  // TODO(aleloi): it might be better to decide the number of channels
  //                with an API instead of dynamically.

  // Find the max channels over all mixing lists.
  const size_t num_mixed_channels =
      std::max(MaxNumChannels(&mixList), MaxNumChannels(&additionalFramesList));

  audio_frame_for_mixing->UpdateFrame(
      -1, _timeStamp, NULL, 0, _outputFrequency, AudioFrame::kNormalSpeech,
      AudioFrame::kVadPassive, num_mixed_channels);

  _timeStamp += static_cast<uint32_t>(_sampleSize);

  use_limiter_ = num_mixed_audio_sources_ > 1 &&
                 _outputFrequency <= AudioProcessing::kMaxNativeSampleRateHz;

  // We only use the limiter if it supports the output sample rate and
  // we're actually mixing multiple streams.
  MixFromList(audio_frame_for_mixing, mixList, _id, use_limiter_);

  {
    CriticalSectionScoped cs(_crit.get());
    MixAnonomouslyFromList(audio_frame_for_mixing, additionalFramesList);

    if (audio_frame_for_mixing->samples_per_channel_ == 0) {
      // Nothing was mixed, set the audio samples to silence.
      audio_frame_for_mixing->samples_per_channel_ = _sampleSize;
      audio_frame_for_mixing->Mute();
    } else {
      // Only call the limiter if we have something to mix.
      LimitMixedAudio(audio_frame_for_mixing);
    }
  }
  return;
}

int32_t NewAudioConferenceMixerImpl::SetOutputFrequency(
    const Frequency& frequency) {
  CriticalSectionScoped cs(_crit.get());

  _outputFrequency = frequency;
  _sampleSize =
      static_cast<size_t>((_outputFrequency * kProcessPeriodicityInMs) / 1000);

  return 0;
}

NewAudioConferenceMixer::Frequency
NewAudioConferenceMixerImpl::OutputFrequency() const {
  CriticalSectionScoped cs(_crit.get());
  return _outputFrequency;
}

int32_t NewAudioConferenceMixerImpl::SetMixabilityStatus(
    MixerAudioSource* audio_source,
    bool mixable) {
  if (!mixable) {
    // Anonymous audio sources are in a separate list. Make sure that the
    // audio source is in the _audioSourceList if it is being mixed.
    SetAnonymousMixabilityStatus(audio_source, false);
  }
  size_t numMixedAudioSources;
  {
    CriticalSectionScoped cs(_cbCrit.get());
    const bool isMixed = IsAudioSourceInList(*audio_source, audio_source_list_);
    // API must be called with a new state.
    if (!(mixable ^ isMixed)) {
      WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                   "Mixable is aready %s", isMixed ? "ON" : "off");
      return -1;
    }
    bool success = false;
    if (mixable) {
      success = AddAudioSourceToList(audio_source, &audio_source_list_);
    } else {
      success = RemoveAudioSourceFromList(audio_source, &audio_source_list_);
    }
    if (!success) {
      WEBRTC_TRACE(kTraceError, kTraceAudioMixerServer, _id,
                   "failed to %s audio_source", mixable ? "add" : "remove");
      RTC_NOTREACHED();
      return -1;
    }

    size_t numMixedNonAnonymous = audio_source_list_.size();
    if (numMixedNonAnonymous > kMaximumAmountOfMixedAudioSources) {
      numMixedNonAnonymous = kMaximumAmountOfMixedAudioSources;
    }
    numMixedAudioSources =
        numMixedNonAnonymous + additional_audio_source_list_.size();
  }
  // A MixerAudioSource was added or removed. Make sure the scratch
  // buffer is updated if necessary.
  // Note: The scratch buffer may only be updated in Process().
  CriticalSectionScoped cs(_crit.get());
  num_mixed_audio_sources_ = numMixedAudioSources;
  return 0;
}

bool NewAudioConferenceMixerImpl::MixabilityStatus(
    const MixerAudioSource& audio_source) const {
  CriticalSectionScoped cs(_cbCrit.get());
  return IsAudioSourceInList(audio_source, audio_source_list_);
}

int32_t NewAudioConferenceMixerImpl::SetAnonymousMixabilityStatus(
    MixerAudioSource* audio_source,
    bool anonymous) {
  CriticalSectionScoped cs(_cbCrit.get());
  if (IsAudioSourceInList(*audio_source, additional_audio_source_list_)) {
    if (anonymous) {
      return 0;
    }
    if (!RemoveAudioSourceFromList(audio_source,
                                   &additional_audio_source_list_)) {
      WEBRTC_TRACE(kTraceError, kTraceAudioMixerServer, _id,
                   "unable to remove audio_source from anonymous list");
      RTC_NOTREACHED();
      return -1;
    }
    return AddAudioSourceToList(audio_source, &audio_source_list_) ? 0 : -1;
  }
  if (!anonymous) {
    return 0;
  }
  const bool mixable =
      RemoveAudioSourceFromList(audio_source, &audio_source_list_);
  if (!mixable) {
    WEBRTC_TRACE(
        kTraceWarning, kTraceAudioMixerServer, _id,
        "audio_source must be registered before turning it into anonymous");
    // Setting anonymous status is only possible if MixerAudioSource is
    // already registered.
    return -1;
  }
  return AddAudioSourceToList(audio_source, &additional_audio_source_list_)
             ? 0
             : -1;
}

bool NewAudioConferenceMixerImpl::AnonymousMixabilityStatus(
    const MixerAudioSource& audio_source) const {
  CriticalSectionScoped cs(_cbCrit.get());
  return IsAudioSourceInList(audio_source, additional_audio_source_list_);
}

int32_t NewAudioConferenceMixerImpl::SetMinimumMixingFrequency(Frequency freq) {
  // Make sure that only allowed sampling frequencies are used. Use closest
  // higher sampling frequency to avoid losing information.
  if (static_cast<int>(freq) == 12000) {
    freq = kWbInHz;
  } else if (static_cast<int>(freq) == 24000) {
    freq = kSwbInHz;
  }

  if ((freq == kNbInHz) || (freq == kWbInHz) || (freq == kSwbInHz) ||
      (freq == kLowestPossible)) {
    _minimumMixingFreq = freq;
    return 0;
  } else {
    WEBRTC_TRACE(kTraceError, kTraceAudioMixerServer, _id,
                 "SetMinimumMixingFrequency incorrect frequency: %i", freq);
    RTC_NOTREACHED();
    return -1;
  }
}

// Check all AudioFrames that are to be mixed. The highest sampling frequency
// found is the lowest that can be used without losing information.
int32_t NewAudioConferenceMixerImpl::GetLowestMixingFrequency() const {
  const int audioSourceListFrequency =
      GetLowestMixingFrequencyFromList(audio_source_list_);
  const int anonymousListFrequency =
      GetLowestMixingFrequencyFromList(additional_audio_source_list_);
  const int highestFreq = (audioSourceListFrequency > anonymousListFrequency)
                              ? audioSourceListFrequency
                              : anonymousListFrequency;
  // Check if the user specified a lowest mixing frequency.
  if (_minimumMixingFreq != kLowestPossible) {
    if (_minimumMixingFreq > highestFreq) {
      return _minimumMixingFreq;
    }
  }
  return highestFreq;
}

int32_t NewAudioConferenceMixerImpl::GetLowestMixingFrequencyFromList(
    const MixerAudioSourceList& mixList) const {
  int32_t highestFreq = 8000;
  for (MixerAudioSourceList::const_iterator iter = mixList.begin();
       iter != mixList.end(); ++iter) {
    const int32_t neededFrequency = (*iter)->NeededFrequency(_id);
    if (neededFrequency > highestFreq) {
      highestFreq = neededFrequency;
    }
  }
  return highestFreq;
}

AudioFrameList NewAudioConferenceMixerImpl::UpdateToMix(
    size_t maxAudioFrameCounter) const {
  AudioFrameList result;
  std::vector<SourceFrame> audioSourceMixingDataList;

  // Get audio source audio and put it in the struct vector.
  for (MixerAudioSource* audio_source : audio_source_list_) {
    auto audio_frame_with_info = audio_source->GetAudioFrameWithMuted(
        _id, static_cast<int>(_outputFrequency));

    auto audio_frame_info = audio_frame_with_info.audio_frame_info;
    AudioFrame* audio_source_audio_frame = audio_frame_with_info.audio_frame;

    if (audio_frame_info == MixerAudioSource::AudioFrameInfo::kError) {
      WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                   "failed to GetAudioFrameWithMuted() from participant");
      continue;
    }
    audioSourceMixingDataList.emplace_back(
        audio_source, audio_source_audio_frame,
        audio_frame_info == MixerAudioSource::AudioFrameInfo::kMuted,
        audio_source->_mixHistory->WasMixed());
  }

  // Sort frames by sorting function.
  std::sort(audioSourceMixingDataList.begin(), audioSourceMixingDataList.end(),
            std::mem_fn(&SourceFrame::shouldMixBefore));

  // Go through list in order and put things in mixList.
  for (SourceFrame& p : audioSourceMixingDataList) {
    // Filter muted.
    if (p.muted_) {
      p.audio_source_->_mixHistory->SetIsMixed(false);
      continue;
    }

    // Add frame to result vector for mixing.
    bool is_mixed = false;
    if (maxAudioFrameCounter > 0) {
      --maxAudioFrameCounter;
      if (!p.was_mixed_before_) {
        RampIn(*p.audio_frame_);
      }
      result.emplace_back(p.audio_frame_, false);
      is_mixed = true;
    }

    // Ramp out unmuted.
    if (p.was_mixed_before_ && !is_mixed) {
      RampOut(*p.audio_frame_);
      result.emplace_back(p.audio_frame_, false);
    }

    p.audio_source_->_mixHistory->SetIsMixed(is_mixed);
  }
  return result;
}

void NewAudioConferenceMixerImpl::GetAdditionalAudio(
    AudioFrameList* additionalFramesList) const {
  WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
               "GetAdditionalAudio(additionalFramesList)");
  // The GetAudioFrameWithMuted() callback may result in the audio source being
  // removed from additionalAudioFramesList_. If that happens it will
  // invalidate any iterators. Create a copy of the audio sources list such
  // that the list of participants can be traversed safely.
  MixerAudioSourceList additionalAudioSourceList;
  additionalAudioSourceList.insert(additionalAudioSourceList.begin(),
                                   additional_audio_source_list_.begin(),
                                   additional_audio_source_list_.end());

  for (MixerAudioSourceList::const_iterator audio_source =
           additionalAudioSourceList.begin();
       audio_source != additionalAudioSourceList.end(); ++audio_source) {
    auto audio_frame_with_info =
        (*audio_source)->GetAudioFrameWithMuted(_id, _outputFrequency);
    auto ret = audio_frame_with_info.audio_frame_info;
    AudioFrame* audio_frame = audio_frame_with_info.audio_frame;
    if (ret == MixerAudioSource::AudioFrameInfo::kError) {
      WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                   "failed to GetAudioFrameWithMuted() from audio_source");
      continue;
    }
    if (audio_frame->samples_per_channel_ == 0) {
      // Empty frame. Don't use it.
      continue;
    }
    additionalFramesList->push_back(FrameAndMuteInfo(
        audio_frame, ret == MixerAudioSource::AudioFrameInfo::kMuted));
  }
}

bool NewAudioConferenceMixerImpl::IsAudioSourceInList(
    const MixerAudioSource& audio_source,
    const MixerAudioSourceList& audioSourceList) const {
  WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
               "IsAudioSourceInList(audio_source,audioSourceList)");
  for (MixerAudioSourceList::const_iterator iter = audioSourceList.begin();
       iter != audioSourceList.end(); ++iter) {
    if (&audio_source == *iter) {
      return true;
    }
  }
  return false;
}

bool NewAudioConferenceMixerImpl::AddAudioSourceToList(
    MixerAudioSource* audio_source,
    MixerAudioSourceList* audioSourceList) const {
  WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
               "AddAudioSourceToList(audio_source, audioSourceList)");
  audioSourceList->push_back(audio_source);
  // Make sure that the mixed status is correct for new MixerAudioSource.
  audio_source->_mixHistory->ResetMixedStatus();
  return true;
}

bool NewAudioConferenceMixerImpl::RemoveAudioSourceFromList(
    MixerAudioSource* audio_source,
    MixerAudioSourceList* audioSourceList) const {
  WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
               "RemoveAudioSourceFromList(audio_source, audioSourceList)");
  for (MixerAudioSourceList::iterator iter = audioSourceList->begin();
       iter != audioSourceList->end(); ++iter) {
    if (*iter == audio_source) {
      audioSourceList->erase(iter);
      // AudioSource is no longer mixed, reset to default.
      audio_source->_mixHistory->ResetMixedStatus();
      return true;
    }
  }
  return false;
}

int32_t NewAudioConferenceMixerImpl::MixFromList(
    AudioFrame* mixedAudio,
    const AudioFrameList& audioFrameList,
    int32_t id,
    bool use_limiter) {
  WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, id,
               "MixFromList(mixedAudio, audioFrameList)");
  if (audioFrameList.empty())
    return 0;

  uint32_t position = 0;

  if (audioFrameList.size() == 1) {
    mixedAudio->timestamp_ = audioFrameList.front().frame->timestamp_;
    mixedAudio->elapsed_time_ms_ =
        audioFrameList.front().frame->elapsed_time_ms_;
  } else {
    // TODO(wu): Issue 3390.
    // Audio frame timestamp is only supported in one channel case.
    mixedAudio->timestamp_ = 0;
    mixedAudio->elapsed_time_ms_ = -1;
  }

  for (AudioFrameList::const_iterator iter = audioFrameList.begin();
       iter != audioFrameList.end(); ++iter) {
    if (position >= kMaximumAmountOfMixedAudioSources) {
      WEBRTC_TRACE(
          kTraceMemory, kTraceAudioMixerServer, id,
          "Trying to mix more than max amount of mixed audio sources:%d!",
          kMaximumAmountOfMixedAudioSources);
      // Assert and avoid crash
      RTC_NOTREACHED();
      position = 0;
    }
    if (!iter->muted) {
      MixFrames(mixedAudio, iter->frame, use_limiter);
    }

    position++;
  }

  return 0;
}

// TODO(andrew): consolidate this function with MixFromList.
int32_t NewAudioConferenceMixerImpl::MixAnonomouslyFromList(
    AudioFrame* mixedAudio,
    const AudioFrameList& audioFrameList) const {
  WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
               "MixAnonomouslyFromList(mixedAudio, audioFrameList)");

  if (audioFrameList.empty())
    return 0;

  for (AudioFrameList::const_iterator iter = audioFrameList.begin();
       iter != audioFrameList.end(); ++iter) {
    if (!iter->muted) {
      MixFrames(mixedAudio, iter->frame, use_limiter_);
    }
  }
  return 0;
}

bool NewAudioConferenceMixerImpl::LimitMixedAudio(
    AudioFrame* mixedAudio) const {
  if (!use_limiter_) {
    return true;
  }

  // Smoothly limit the mixed frame.
  const int error = _limiter->ProcessStream(mixedAudio);

  // And now we can safely restore the level. This procedure results in
  // some loss of resolution, deemed acceptable.
  //
  // It's possible to apply the gain in the AGC (with a target level of 0 dbFS
  // and compression gain of 6 dB). However, in the transition frame when this
  // is enabled (moving from one to two audio sources) it has the potential to
  // create discontinuities in the mixed frame.
  //
  // Instead we double the frame (with addition since left-shifting a
  // negative value is undefined).
  *mixedAudio += *mixedAudio;

  if (error != _limiter->kNoError) {
    WEBRTC_TRACE(kTraceError, kTraceAudioMixerServer, _id,
                 "Error from AudioProcessing: %d", error);
    RTC_NOTREACHED();
    return false;
  }
  return true;
}
}  // namespace webrtc
