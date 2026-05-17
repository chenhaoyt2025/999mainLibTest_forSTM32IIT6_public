// Copyright 2015 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Group of voices.

#include "ringsX_MIDI/dsp/part.h"

#include "stmlib/dsp/units.h"

#include "ringsX_MIDI/resources.h"

namespace rings {

using namespace std;
using namespace stmlib;

void Part::Init(uint16_t* reverb_buffer) {
  active_voice_ = 0;
  
  feedback_l_ = 0.0f;
  feedback_r_ = 0.0f;
  
  for (int32_t i = 0; i < 2; ++i) {
    fb_lpf_[i].Init();
    fb_hpf_[i].Init();
  }

  fill(&note_[0], &note_[kMaxPolyphony], 0.0f);
  
  bypass_ = false;
  polyphony_ = 1;
  model_ = RESONATOR_MODEL_MODAL;
  dirty_ = true;
  
  for (int32_t i = 0; i < kMaxPolyphony; ++i) {
    excitation_filter_[i].Init();
    plucker_[i].Init();
    dc_blocker_[i].Init(1.0f - 10.0f / kSampleRate);
  }
  
  reverb_.Init(reverb_buffer);
  limiter_.Init();

  note_filter_.Init(
      kSampleRate / kMaxBlockSize,
      0.001f,  // Lag time with a sharp edge on the V/Oct input or trigger.
      0.010f,  // Lag time after the trigger has been received.
      0.050f,  // Time to transition from reactive to filtered.
      0.004f); // Prevent a sharp edge to partly leak on the previous voice.
}

void Part::ConfigureResonators() {
  if (!dirty_) {
    return;
  }
  
  switch (model_) {
    case RESONATOR_MODEL_MODAL:
      {
        int32_t resolution = 64 / polyphony_ - 4;
        for (int32_t i = 0; i < polyphony_; ++i) {
          resonator_[i].Init();
          resonator_[i].set_resolution(resolution);
        }
      }
      break;
    
    case RESONATOR_MODEL_SYMPATHETIC_STRING:
    case RESONATOR_MODEL_STRING:
    case RESONATOR_MODEL_WESTERN_CHORDS:
    case RESONATOR_MODEL_STRING_AND_REVERB:
    case RESONATOR_MODEL_AUDREY_A:
    case RESONATOR_MODEL_AUDREY_B:
    case RESONATOR_MODEL_AUDREY_C:
      {
        float lfo_frequencies[kNumStrings] = {
          0.5f, 0.4f, 0.35f, 0.23f, 0.211f, 0.2f, 0.171f
        };
        for (int32_t i = 0; i < kNumStrings; ++i) {
          bool has_dispersion = model_ == RESONATOR_MODEL_STRING || \
              model_ == RESONATOR_MODEL_STRING_AND_REVERB || \
              model_ >= RESONATOR_MODEL_AUDREY_A;
          string_[i].Init(has_dispersion);

          float f_lfo = float(kMaxBlockSize) / float(kSampleRate);
          f_lfo *= lfo_frequencies[i];
          lfo_[i].Init<COSINE_OSCILLATOR_APPROXIMATE>(f_lfo);
        }
        for (int32_t i = 0; i < polyphony_; ++i) {
          plucker_[i].Init();
        }
      }
      break;
    
    case RESONATOR_MODEL_FM_VOICE:
    case RESONATOR_MODEL_FM_VOICE_2:
      {
        for (int32_t i = 0; i < polyphony_; ++i) {
          fm_voice_[i].Init();
        }
      }
      break;

    case RESONATOR_MODEL_HILBERT:
      {
        int32_t resolution = 64 / polyphony_ - 4;
        for (int32_t i = 0; i < polyphony_; ++i) {
          hilbert_resonator_[i].Init();
          hilbert_resonator_[i].set_resolution(resolution);
        }
      }
      break;
    
    default:
      break;
  }

  if (active_voice_ >= polyphony_) {
    active_voice_ = 0;
  }
  dirty_ = false;
}

#ifdef BRYAN_CHORDS

// Chord table by Bryan Noll:
float chords[kMaxPolyphony][11][8] = {
  {
    { -12.0f, -0.01f, 0.0f,  0.01f, 0.02f, 11.98f, 11.99f, 12.0f }, // OCT
    { -12.0f, -5.0f,  0.0f,  6.99f, 7.0f,  11.99f, 12.0f,  19.0f }, // 5
    { -12.0f, -5.0f,  0.0f,  5.0f,  7.0f,  11.99f, 12.0f,  17.0f }, // sus4
    { -12.0f, -5.0f,  0.0f,  3.0f,  7.0f,   3.01f, 12.0f,  19.0f }, // m 
    { -12.0f, -5.0f,  0.0f,  3.0f,  7.0f,   3.01f, 10.0f,  19.0f }, // m7
    { -12.0f, -5.0f,  0.0f,  3.0f, 14.0f,   3.01f, 10.0f,  19.0f }, // m9
    { -12.0f, -5.0f,  0.0f,  3.0f,  7.0f,   3.01f, 10.0f,  17.0f }, // m11
    { -12.0f, -5.0f,  0.0f,  2.0f,  7.0f,   9.0f,  16.0f,  19.0f }, // 69
    { -12.0f, -5.0f,  0.0f,  4.0f,  7.0f,  11.0f,  14.0f,  19.0f }, // M9
    { -12.0f, -5.0f,  0.0f,  4.0f,  7.0f,  11.0f,  10.99f, 19.0f }, // M7
    { -12.0f, -5.0f,  0.0f,  4.0f,  7.0f,  11.99f, 12.0f,  19.0f } // M
  },
  { 
    { -12.0f, 0.0f,  0.01f, 12.0f }, // OCT
    { -12.0f, 6.99f, 7.0f,  12.0f }, // 5
    { -12.0f, 5.0f,  7.0f,  12.0f }, // sus4
    { -12.0f, 3.0f, 11.99f, 12.0f }, // m 
    { -12.0f, 3.0f, 10.0f,  12.0f }, // m7
    { -12.0f, 3.0f, 10.0f,  14.0f }, // m9
    { -12.0f, 3.0f, 10.0f,  17.0f }, // m11
    { -12.0f, 2.0f,  9.0f,  16.0f }, // 69
    { -12.0f, 4.0f, 11.0f,  14.0f }, // M9
    { -12.0f, 4.0f,  7.0f,  11.0f }, // M7
    { -12.0f, 4.0f,  7.0f,  12.0f }, // M
  },
  {
    { 0.0f, -12.0f },
    { 0.0f, 2.0f },
    { 0.0f, 3.0f },
    { 0.0f, 4.0f },
    { 0.0f, 5.0f },
    { 0.0f, 7.0f },
    { 0.0f, 9.0f },
    { 0.0f, 10.0f },
    { 0.0f, 11.0f },
    { 0.0f, 12.0f },
    { -12.0f, 12.0f }
  },
  {
    { 0.0f, -12.0f },
    { 0.0f, 2.0f },
    { 0.0f, 3.0f },
    { 0.0f, 4.0f },
    { 0.0f, 5.0f },
    { 0.0f, 7.0f },
    { 0.0f, 9.0f },
    { 0.0f, 10.0f },
    { 0.0f, 11.0f },
    { 0.0f, 12.0f },
    { -12.0f, 12.0f }
  }
};

#else

// Original chord table
float chords[kMaxPolyphony][11][8] = {
  {
    { -12.0f, 0.0f, 0.01f, 0.02f, 0.03f, 11.98f, 11.99f, 12.0f },
    { -12.0f, 0.0f, 3.0f,  3.01f, 7.0f,  9.99f,  10.0f,  19.0f },
    { -12.0f, 0.0f, 3.0f,  3.01f, 7.0f,  11.99f, 12.0f,  19.0f },
    { -12.0f, 0.0f, 3.0f,  3.01f, 7.0f,  13.99f, 14.0f,  19.0f },
    { -12.0f, 0.0f, 3.0f,  3.01f, 7.0f,  16.99f, 17.0f,  19.0f },
    { -12.0f, 0.0f, 6.98f, 6.99f, 7.0f,  12.00f, 18.99f, 19.0f },
    { -12.0f, 0.0f, 3.99f, 4.0f,  7.0f,  16.99f, 17.0f,  19.0f },
    { -12.0f, 0.0f, 3.99f, 4.0f,  7.0f,  13.99f, 14.0f,  19.0f },
    { -12.0f, 0.0f, 3.99f, 4.0f,  7.0f,  11.99f, 12.0f,  19.0f },
    { -12.0f, 0.0f, 3.99f, 4.0f,  7.0f,  10.99f, 11.0f,  19.0f },
    { -12.0f, 0.0f, 4.99f, 5.0f,  7.0f,  11.99f, 12.0f,  17.0f }
  },
  { 
    { -12.0f, 0.0f, 0.01f, 12.0f },
    { -12.0f, 3.0f, 7.0f,  10.0f },
    { -12.0f, 3.0f, 7.0f,  12.0f },
    { -12.0f, 3.0f, 7.0f,  14.0f },
    { -12.0f, 3.0f, 7.0f,  17.0f },
    { -12.0f, 7.0f, 12.0f, 19.0f },
    { -12.0f, 4.0f, 7.0f,  17.0f },
    { -12.0f, 4.0f, 7.0f,  14.0f },
    { -12.0f, 4.0f, 7.0f,  12.0f },
    { -12.0f, 4.0f, 7.0f,  11.0f },
    { -12.0f, 5.0f, 7.0f,  12.0f },
  },
  {
    { 0.0f, -12.0f },
    { 0.0f, 0.01f },
    { 0.0f, 2.0f },
    { 0.0f, 3.0f },
    { 0.0f, 4.0f },
    { 0.0f, 5.0f },
    { 0.0f, 7.0f },
    { 0.0f, 10.0f },
    { 0.0f, 11.0f },
    { 0.0f, 12.0f },
    { -12.0f, 12.0f }
  },
  {
    { 0.0f, -12.0f },
    { 0.0f, 0.01f },
    { 0.0f, 2.0f },
    { 0.0f, 3.0f },
    { 0.0f, 4.0f },
    { 0.0f, 5.0f },
    { 0.0f, 7.0f },
    { 0.0f, 10.0f },
    { 0.0f, 11.0f },
    { 0.0f, 12.0f },
    { -12.0f, 12.0f }
  }
};

#endif  // BRYAN_CHORDS

void Part::ComputeSympatheticStringsNotes(
    float tonic,
    float note,
    float parameter,
    float* destination,
    size_t num_strings) {
  float notes[9] = {
      tonic,
      note - 12.0f,
      note - 7.01955f,
      note,
      note + 7.01955f,
      note + 12.0f,
      note + 19.01955f,
      note + 24.0f,
      note + 24.0f };
  const float detunings[4] = {
      0.013f,
      0.011f,
      0.007f,
      0.017f
  };
  
  if (parameter >= 2.0f) {
    // Quantized chords
    int32_t chord_index = parameter - 2.0f;
    const float* chord = chords[polyphony_ - 1][chord_index];
    for (size_t i = 0; i < num_strings; ++i) {
      destination[i] = chord[i] + note;
    }
    return;
  }

  size_t num_detuned_strings = (num_strings - 1) >> 1;
  size_t first_detuned_string = num_strings - num_detuned_strings;
  
  for (size_t i = 0; i < first_detuned_string; ++i) {
    float note = 3.0f;
    if (i != 0) {
      note = parameter * 7.0f;
      parameter += (1.0f - parameter) * 0.2f;
    }
    
    MAKE_INTEGRAL_FRACTIONAL(note);
    note_fractional = Squash(note_fractional);

    float a = notes[note_integral];
    float b = notes[note_integral + 1];
    
    note = a + (b - a) * note_fractional;
    destination[i] = note;
    if (i + first_detuned_string < num_strings) {
      destination[i + first_detuned_string] = destination[i] + detunings[i & 3];
    }
  }
}

void Part::RenderModalVoice(
    int32_t voice,
    const PerformanceState& performance_state,
    const Patch& patch,
    float frequency,
    float filter_cutoff,
    size_t size) {
  // Internal exciter is a pulse, pre-filter.
  if (performance_state.internal_exciter &&
      voice == active_voice_ &&
      performance_state.strum) {
    resonator_input_[0] += 0.25f * SemitonesToRatio(
        filter_cutoff * filter_cutoff * 24.0f) / filter_cutoff;
  }
  
  // Process through filter.
  excitation_filter_[voice].Process<FILTER_MODE_LOW_PASS>(
      resonator_input_, resonator_input_, size);

  Resonator& r = resonator_[voice];
  r.set_frequency(frequency);
  r.set_structure(patch.structure);
  r.set_brightness(patch.brightness * patch.brightness);
  r.set_position(patch.position);
  r.set_damping(patch.damping);
  r.Process(resonator_input_, out_buffer_, aux_buffer_, size);
}

void Part::RenderFMVoice(
    int32_t voice,
    const PerformanceState& performance_state,
    const Patch& patch,
    float frequency,
    float filter_cutoff,
    size_t size) {
  FMVoice& v = fm_voice_[voice];
  if (performance_state.internal_exciter &&
      voice == active_voice_ &&
      performance_state.strum) {
    v.TriggerInternalEnvelope();
  }

  v.set_frequency(frequency);
  v.set_ratio(patch.structure);
  v.set_brightness(patch.brightness);
  v.set_feedback_amount(patch.position);
  v.set_position(/*patch.position*/ 0.0f);
  v.set_damping(patch.damping);
  
  float noise_amount = 0.0f;
  if (model_ >= 7) {
    noise_amount = (model_ == RESONATOR_MODEL_AUDREY_C) ? 0.012f : 0.045f;
  }
  v.set_noise_amount(noise_amount);
  v.Process(resonator_input_, out_buffer_, aux_buffer_, size);
}

void Part::RenderStringVoice(
    int32_t voice,
    const PerformanceState& performance_state,
    const Patch& patch,
    float frequency,
    float filter_cutoff,
    size_t size) {
  // Compute number of strings and frequency.
  int32_t num_strings = 1;
  float frequencies[kNumStrings];

  if (model_ == RESONATOR_MODEL_SYMPATHETIC_STRING ||
      model_ == RESONATOR_MODEL_WESTERN_CHORDS) {
    num_strings = 2 * kMaxPolyphony / polyphony_;
    float parameter = model_ == RESONATOR_MODEL_SYMPATHETIC_STRING
        ? patch.structure
        : 2.0f + performance_state.chord;
    ComputeSympatheticStringsNotes(
        performance_state.tonic + performance_state.fm,
        performance_state.tonic + note_[voice] + performance_state.fm,
        parameter,
        frequencies,
        num_strings);
    for (int32_t i = 0; i < num_strings; ++i) {
      frequencies[i] = SemitonesToRatio(frequencies[i] - 69.0f) * a3;
    }
  } else {
    frequencies[0] = frequency;
  }

  if (voice == active_voice_) {
    const float gain = 1.0f / Sqrt(static_cast<float>(num_strings) * 2.0f);
    for (size_t i = 0; i < size; ++i) {
      resonator_input_[i] *= gain;
    }
  }

  // Process external input.
  excitation_filter_[voice].Process<FILTER_MODE_LOW_PASS>(
      resonator_input_, resonator_input_, size);

  // Add noise burst.
  if (performance_state.internal_exciter) {
    if (voice == active_voice_ && performance_state.strum) {
      plucker_[voice].Trigger(frequency, filter_cutoff * 8.0f, patch.position);
    }
    plucker_[voice].Process(noise_burst_buffer_, size);
    for (size_t i = 0; i < size; ++i) {
      resonator_input_[i] += noise_burst_buffer_[i];
    }
  }
  dc_blocker_[voice].Process(resonator_input_, size);
  
  fill(&out_buffer_[0], &out_buffer_[size], 0.0f);
  fill(&aux_buffer_[0], &aux_buffer_[size], 0.0f);
  
  float structure = patch.structure;
  float dispersion = structure < 0.24f
      ? (structure - 0.24f) * 4.166f
      : (structure > 0.26f ? (structure - 0.26f) * 1.35135f : 0.0f);
  
  for (int32_t string = 0; string < num_strings; ++string) {
    int32_t i = voice + string * polyphony_;
    String& s = string_[i];
    float lfo_value = lfo_[i].Next();
    
    float brightness = patch.brightness;
    float damping = patch.damping;
    float position = patch.position;
    float glide = 1.0f;
    float string_index = static_cast<float>(string) / static_cast<float>(num_strings);
    const float* input = resonator_input_;
    
    if (model_ == RESONATOR_MODEL_STRING_AND_REVERB) {
      damping *= (2.0f - damping);
    }
    
    // When the internal exciter is used, string 0 is the main
    // source, the other strings are vibrating by sympathetic resonance.
    // When the internal exciter is not used, all strings are vibrating
    // by sympathetic resonance.
    if (string > 0 && performance_state.internal_exciter) {
      brightness *= (2.0f - brightness);
      brightness *= (2.0f - brightness);
      damping = 0.7f + patch.damping * 0.27f;
      float amount = (0.5f - fabs(0.5f - patch.position)) * 0.9f;
      position = patch.position + lfo_value * amount;
      glide = SemitonesToRatio((brightness - 1.0f) * 36.0f);
      input = sympathetic_resonator_input_;
    }
    
    s.set_dispersion(dispersion);
    s.set_frequency(frequencies[string], glide);
    s.set_brightness(brightness);
    s.set_position(position);
    s.set_damping(damping + string_index * (0.95f - damping));
    s.Process(input, out_buffer_, aux_buffer_, size);
    
    if (string == 0) {
      // Was 0.1f, Ben Wilson -> 0.2f
      float gain = 0.2f / static_cast<float>(num_strings);
      for (size_t i = 0; i < size; ++i) {
        float sum = out_buffer_[i] - aux_buffer_[i];
        sympathetic_resonator_input_[i] = gain * sum;
      }
    }
  }
}

const int32_t kPingPattern[] = {
  1, 0, 2, 1, 0, 2, 1, 0
};

void Part::Process(
    const PerformanceState& performance_state,
    const Patch& patch,
    const float* in,
    float* out,
    float* aux,
    size_t size) {

  // Copy inputs to outputs when bypass mode is enabled.
  if (bypass_) {
    copy(&in[0], &in[size], &out[0]);
    copy(&in[0], &in[size], &aux[0]);
    return;
  }
  
  // 使用 patch.reverb_amount (由 Position 小旋钮控制) 作为全局混响量
  float global_reverb = patch.reverb_amount;
  
  if (model_ >= 7) {
    // HOHO 模式：混响感更强，且映射更陡峭
    global_reverb = global_reverb * (1.2f - 0.2f * global_reverb);
    CONSTRAIN(global_reverb, 0.0f, 1.0f);
  }

  // 对于原版 Mode 5 (String & Reverb)，如果 rever_amount 为 0 (表示非 HOHO 模式)，
  // 则恢复原版逻辑：使用 damping 控制混响量。
  if (model_ == RESONATOR_MODEL_STRING_AND_REVERB && global_reverb <= 0.001f) {
    global_reverb = patch.damping * 0.8f;
  }
  
  // 如果复音数为 4，为了保证 CPU 不超载，稍微压缩一下混响的最大范围。
  // 这种压缩比较平滑，保留了深度，但避开了 CPU 挂起的临界点。
  int32_t effective_polyphony = polyphony_;
  if (model_ >= 7) {
    effective_polyphony = 2; // HOHO 模式固定双复音
  } else if (model_ == RESONATOR_MODEL_STRING_AND_REVERB && effective_polyphony > 2) {
    effective_polyphony = 2; // 原版模式 5 锁定最高双复音
  }

  if (effective_polyphony > 2) {
    global_reverb *= 0.85f;
  }
  
  // HOHO 模式 (7-9) 混响更强大。
  if (model_ >= 7) {
    // 移除强制倍增，让干湿比更线性，允许在最小处听到纯净的干声
    global_reverb = global_reverb * 0.95f; 
  }
  
  // 恢复深邃的混响设置，因为 HOHO 模式已锁定 2 复音，CPU 资源充足
  reverb_.set_amount(global_reverb);
  reverb_.set_diffusion(0.625f);
  reverb_.set_time(model_ >= 7 ? 0.5f + 0.49f * global_reverb : 0.35f + 0.63f * global_reverb);
  reverb_.set_input_gain(model_ >= 7 ? 0.35f : 0.2f); // 显著增加混响输入增益
  reverb_.set_lp(0.3f + 0.5f * global_reverb);

  ConfigureResonators();
  
  note_filter_.Process(
      performance_state.note,
      performance_state.strum);

  if (performance_state.strum) {
    note_[active_voice_] = note_filter_.stable_note();
    if (effective_polyphony > 1 && effective_polyphony & 1) {
      active_voice_ = kPingPattern[step_counter_ % 8];
      step_counter_ = (step_counter_ + 1) % 8;
    } else {
      active_voice_ = (active_voice_ + 1) % effective_polyphony;
    }
  }
  
  note_[active_voice_] = note_filter_.note();
  
  fill(&out[0], &out[size], 0.0f);
  fill(&aux[0], &aux[size], 0.0f);
  for (int32_t voice = 0; voice < effective_polyphony; ++voice) {
    // Compute MIDI note value, frequency, and cutoff frequency for excitation
    // filter.
    float cutoff = patch.brightness * (2.0f - patch.brightness);
    float note = note_[voice] + performance_state.tonic + performance_state.fm;
    float frequency = SemitonesToRatio(note - 69.0f) * a3;
    float filter_cutoff_range = performance_state.internal_exciter
      ? frequency * SemitonesToRatio((cutoff - 0.5f) * 96.0f)
      : 0.4f * SemitonesToRatio((cutoff - 1.0f) * 108.0f);
    float filter_cutoff = min(voice == active_voice_
      ? filter_cutoff_range
      : (10.0f / kSampleRate), 0.499f);
    float filter_q = performance_state.internal_exciter ? 1.5f : 0.8f;
    if (model_ >= 7) {
      filter_q = performance_state.internal_exciter ? 2.5f : 1.25f;
    }

    // Process input with excitation filter. Inactive voices receive silence.
    excitation_filter_[voice].set_f_q<FREQUENCY_DIRTY>(filter_cutoff, filter_q);
    
    // HOHO 模式不锁静音，允许自激。B 模式可以作为一个干净版本或带有些许不同的特性
    bool is_silent_mode = false;

    if (voice == active_voice_ && !is_silent_mode) {
      copy(&in[0], &in[size], &resonator_input_[0]);
      
      // HOHO 模式注入微弱噪声，确保在没输入信号时也能自激发出声。
      // C 模式底噪调低，防止静止时太吵。
      if (model_ >= 7) {
        float noise_gain = (model_ == RESONATOR_MODEL_AUDREY_C) ? 0.0004f : 0.0012f;
        for (size_t i = 0; i < size; ++i) {
          resonator_input_[i] += (stmlib::Random::GetFloat() - 0.5f) * noise_gain;
        }
      }

      // Audrey 系列引擎：整合低通、高通和非线性失真。
      // Structure 旋钮对应 "Body" 参数，控制回授量和驱动增益。
      if (model_ == RESONATOR_MODEL_AUDREY_A) {
        float lp_cutoff = 0.05f + patch.position * 0.9f; 
        float hp_cutoff = 0.005f + (1.0f - patch.position) * 0.1f;
        
        // 显著增大驱动和回授：让 Audrey A 更加暴躁和容易回授
        float drive = 1.5f + patch.structure * 5.5f; 
        float fb_amount = patch.structure * 6.5f; 
        
        fb_lpf_[0].set_f_q<FREQUENCY_DIRTY>(lp_cutoff, 0.5f);
        fb_lpf_[1].set_f_q<FREQUENCY_DIRTY>(lp_cutoff, 0.5f);
        fb_hpf_[0].set_f_q<FREQUENCY_DIRTY>(hp_cutoff, 0.5f);
        fb_hpf_[1].set_f_q<FREQUENCY_DIRTY>(hp_cutoff, 0.5f);
        
        float fb_l = fb_lpf_[0].Process<FILTER_MODE_LOW_PASS>(feedback_l_);
        fb_l = fb_hpf_[0].Process<FILTER_MODE_HIGH_PASS>(fb_l);
        float fb_r = fb_lpf_[1].Process<FILTER_MODE_LOW_PASS>(feedback_r_);
        fb_r = fb_hpf_[1].Process<FILTER_MODE_HIGH_PASS>(fb_r);
        
        fb_l = SoftLimit(fb_l * drive);
        fb_r = SoftLimit(fb_r * drive);

        for (size_t i = 0; i < size; ++i) {
          resonator_input_[i] += (fb_l + fb_r) * 0.5f * fb_amount;
        }
      }
    } else {
      fill(&resonator_input_[0], &resonator_input_[size], 0.0f);
    }
    
    if (is_silent_mode) {
      fill(&out_buffer_[0], &out_buffer_[size], 0.0f);
      fill(&aux_buffer_[0], &aux_buffer_[size], 0.0f);
    } else if (model_ == RESONATOR_MODEL_MODAL) {
      RenderModalVoice(
          voice, performance_state, patch, frequency, filter_cutoff, size);
    } else if (model_ == RESONATOR_MODEL_FM_VOICE || 
               model_ == RESONATOR_MODEL_FM_VOICE_2 ||
               model_ == RESONATOR_MODEL_AUDREY_C) {
      RenderFMVoice(
          voice, performance_state, patch, frequency, filter_cutoff, size);
    } else if (model_ == RESONATOR_MODEL_HILBERT) {
      // Hilbert Resonator processing
      hilbert_resonator_[voice].set_frequency(frequency / kSampleRate);
      hilbert_resonator_[voice].set_structure(patch.structure);
      hilbert_resonator_[voice].set_brightness(patch.brightness);
      hilbert_resonator_[voice].set_damping(patch.damping);
      hilbert_resonator_[voice].set_position(patch.position);
      hilbert_resonator_[voice].Process(
          &resonator_input_[0], out_buffer_, aux_buffer_, size);
    } else {
      RenderStringVoice(
          voice, performance_state, patch, frequency, filter_cutoff, size);
    }
    
    if (polyphony_ == 1) {
      // Send the two sets of harmonics / pickups to individual outputs.
      for (size_t i = 0; i < size; ++i) {
        out[i] += out_buffer_[i];
        aux[i] += aux_buffer_[i];
      }
    } else {
      // Dispatch odd/even voices to individual outputs.
      float* destination = voice & 1 ? aux : out;
      for (size_t i = 0; i < size; ++i) {
        destination[i] += out_buffer_[i] - aux_buffer_[i];
      }
    }
  }
  
  // 只有具备混响特性的模式（原版 Reverb 模式 和 HOHO 模式）才启用混响处理
  bool has_reverb = (model_ == RESONATOR_MODEL_STRING_AND_REVERB || 
                   model_ == RESONATOR_MODEL_AUDREY_A || 
                   model_ == RESONATOR_MODEL_AUDREY_C);

  if (has_reverb) {
    // 混叠/平移逻辑：原本用于模式 5。
    // 对于 HOHO Audrey C，我们跳过这个强制混叠逻辑，保留原版红色模型的立体声宽度。
    if (model_ != RESONATOR_MODEL_AUDREY_C) {
      float side_mix = (1.0f - patch.position);
      if (model_ == RESONATOR_MODEL_AUDREY_A || model_ == RESONATOR_MODEL_AUDREY_B) {
        // 显著减少混叠量 (降为原来的 20%)，保留更宽的立体声场
        side_mix *= 0.2f; 
      }
      for (size_t i = 0; i < size; ++i) {
        float l = out[i];
        float r = aux[i];
        out[i] = l * (1.0f - side_mix) + r * side_mix;
        aux[i] = r * (1.0f - side_mix) + l * side_mix;
      }
    }
    
    if (global_reverb > 0.02f) {
      reverb_.Process(out, aux, size);
    }
    
    // 为具备混响特性的模式更新回授信号
    float fb_l = 0.0f;
    float fb_r = 0.0f;
    for (size_t i = 0; i < size; ++i) {
      fb_l += out[i];
      fb_r += aux[i];
    }
    fb_l /= size;
    fb_r /= size;
    
    // Safety: prevent feedback blow-up and NaN.
    if (fb_l > 1.0f) fb_l = 1.0f;
    else if (fb_l < -1.0f) fb_l = -1.0f;
    else if (!(fb_l >= -1.0f && fb_l <= 1.0f)) fb_l = 0.0f; // NaN check
    
    if (fb_r > 1.0f) fb_r = 1.0f;
    else if (fb_r < -1.0f) fb_r = -1.0f;
    else if (!(fb_r >= -1.0f && fb_r <= 1.0f)) fb_r = 0.0f; // NaN check

    feedback_l_ = fb_l;
    feedback_r_ = fb_r;
  }
  
  // Audio rate loop: ensure resonator_input_ doesn't explode
  for (size_t i = 0; i < size; ++i) {
    if (resonator_input_[i] > 5.0f) resonator_input_[i] = 5.0f;
    else if (resonator_input_[i] < -5.0f) resonator_input_[i] = -5.0f;
    else if (!(resonator_input_[i] >= -5.0f && resonator_input_[i] <= 5.0f)) resonator_input_[i] = 0.0f;
  }
  
  // Apply limiter to string output.
  limiter_.Process(out, aux, size, model_gains_[model_]);
}

/* static */
float Part::model_gains_[] = {
  1.4f,  // MODAL (0)
  1.0f,  // SYMPATHETIC (1)
  1.4f,  // STRING (2)
  0.7f,  // FM (3)
  1.0f,  // CHORDS (4)
  1.4f,  // REVERB (5)
  0.7f,  // FM2 (6)
  1.4f,  // AUDREY_A (7)
  1.4f,  // AUDREY_B (8)
  1.4f,  // AUDREY_C (9)
};

}  // namespace rings