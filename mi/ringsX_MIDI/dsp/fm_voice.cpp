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
// FM Voice.

#include "ringsX_MIDI/dsp/fm_voice.h"

#include <cmath>

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/dsp/units.h"

#include "ringsX_MIDI/resources.h"

namespace rings {

using namespace stmlib;

void FMVoice::Init() {
  set_frequency(220.0f / kSampleRate);
  set_ratio(0.5f);
  set_brightness(0.5f);
  set_damping(0.5f);
  set_position(0.5f);
  set_feedback_amount(0.0f);
  set_noise_amount(0.0f);
  
  previous_carrier_frequency_ = carrier_frequency_;
  previous_modulator_frequency_ = carrier_frequency_;
  previous_brightness_ = brightness_;
  previous_damping_ = damping_;
  previous_feedback_amount_ = feedback_amount_;
  
  amplitude_envelope_ = 0.0f;
  brightness_envelope_ = 0.0f;
  
  carrier_phase_ = 0;
  modulator_phase_ = 0;
  seed_ = 0x12345678;
  gain_ = 0.0f;
  fm_amount_ = 0.0f;
  
  follower_.Init(
      8.0f / kSampleRate,
      160.0f / kSampleRate,
      1600.0f / kSampleRate);
}

void FMVoice::Process(const float* in, float* out, float* aux, size_t size) {
  // Interpolate between the "oscillator" behaviour and the "FMLPGed thing"
  // behaviour.
  float envelope_amount = damping_ < 0.9f ? 1.0f : (1.0f - damping_) * 10.0f;
  float amplitude_rt60 = 0.1f * SemitonesToRatio(damping_ * 96.0f) * kSampleRate;
  float amplitude_decay = 1.0f - powf(0.001f, 1.0f / amplitude_rt60);

  float brightness_rt60 = 0.1f * SemitonesToRatio(damping_ * 84.0f) * kSampleRate;
  float brightness_decay = 1.0f - powf(0.001f, 1.0f / brightness_rt60);
  
  float ratio = Interpolate(lut_fm_frequency_quantizer, ratio_, 128.0f);
  float modulator_frequency = carrier_frequency_ * SemitonesToRatio(ratio);
  
  if (modulator_frequency > 0.5f) {
    modulator_frequency = 0.5f;
  }
  
  // 分割点调整为 0.25。
  // 0.0 -> 0.25 映射到 -1.0 -> 0.0 (相位反馈)
  // 0.25 -> 1.0 映射到 0.0 -> 1.0 (调制反馈)
  // 此时物理中点 0.5 恰好处于调制反馈 travel 的 1/3 处 (0.25/0.75)
  float feedback = feedback_amount_ < 0.25f
      ? (feedback_amount_ - 0.25f) * 4.0f
      : (feedback_amount_ - 0.25f) * 1.3333333f;
  
  ParameterInterpolator carrier_increment(
      &previous_carrier_frequency_, carrier_frequency_, size);
  ParameterInterpolator modulator_increment(
      &previous_modulator_frequency_, modulator_frequency, size);
  ParameterInterpolator brightness(
      &previous_brightness_, brightness_, size);
  ParameterInterpolator feedback_amount(
      &previous_feedback_amount_, feedback, size);

  uint32_t carrier_phase = carrier_phase_;
  uint32_t modulator_phase = modulator_phase_;
  float previous_sample = previous_sample_;
  
  while (size--) {
    // Envelope follower and internal envelope.
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    float noise = static_cast<int32_t>(seed_) * 2.3283064e-10f * noise_amount_;
    
    float amplitude_envelope, brightness_envelope;
    follower_.Process(
        *in++ + noise,
        &amplitude_envelope,
        &brightness_envelope);
    
    brightness_envelope *= 2.0f * amplitude_envelope * (2.0f - amplitude_envelope);
    
    SLOPE(amplitude_envelope_, amplitude_envelope, 0.05f, amplitude_decay);
    SLOPE(brightness_envelope_, brightness_envelope, 0.01f, brightness_decay);
    
    // Compute envelopes.
    float brightness_value = brightness.Next();
    brightness_value *= brightness_value;
    float fm_amount_min = brightness_value < 0.5f
        ? 0.0f
        : brightness_value * 2.0f - 1.0f;
    float fm_amount_max = brightness_value < 0.5f
        ? 2.0f * brightness_value
        : 1.0f;
    float fm_envelope = 0.5f + envelope_amount * (brightness_envelope_ - 0.5f);
    float fm_amount = (fm_amount_min + fm_amount_max * fm_envelope) * 2.0f;
    SLEW(fm_amount_, fm_amount, 0.005f + fm_amount_max * 0.015f);

    // FM synthesis in itself
    float phase_feedback = feedback < 0.0f ? 1.2f * feedback * feedback : 0.0f;
    modulator_phase += static_cast<uint32_t>(4294967296.0f * \
      modulator_increment.Next() * (1.0f + previous_sample * phase_feedback));
    carrier_phase += static_cast<uint32_t>(4294967296.0f * \
        carrier_increment.Next());

    float feedback_val = feedback_amount.Next();
    float modulator_fb = feedback_val > 0.0f ? 1.0f * feedback_val * feedback_val : 0.0f;
    float modulator = SineFm(modulator_phase, modulator_fb * previous_sample + noise);
    float carrier = SineFm(carrier_phase, fm_amount_ * modulator + noise);
    ONE_POLE(previous_sample, carrier, 0.1f);

    // Compute amplitude envelope.
    // 在 Audrey 模式下 (noise_amount > 0)，即使没有包络也保持一个基础增益 (0.2f)，防止死寂
    float base_gain = noise_amount_ > 0.0f ? 0.2f : 0.0f;
    float gain = base_gain + (1.0f - base_gain) * (1.0f + envelope_amount * (amplitude_envelope_ - 1.0f));
    
    // 低频补偿：当频率低于 200Hz 时，额外增加增益以强化 40-60Hz 的表现
    if (noise_amount_ > 0.0f) {
      float bass_compensation = 1.0f;
      if (carrier_frequency_ < 0.004f) { // ~192Hz
        bass_compensation = 1.0f + (0.004f - carrier_frequency_) * 250.0f; // 低频最高补偿约 2 倍
        if (bass_compensation > 2.0f) bass_compensation = 2.0f;
      }
      gain *= bass_compensation;
    }
    
    ONE_POLE(gain_, gain, 0.005f + 0.045f * fm_amount_);
    
    *out++ = (carrier + 0.5f * modulator) * gain_;
    *aux++ = 0.5f * modulator * gain_;
  }
  carrier_phase_ = carrier_phase;
  modulator_phase_ = modulator_phase;
  previous_sample_ = previous_sample;
}

}  // namespace rings
