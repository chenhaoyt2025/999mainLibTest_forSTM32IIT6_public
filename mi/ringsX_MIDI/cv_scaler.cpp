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
// Filtering and scaling of ADC values + input calibration.

#include "ringsX_MIDI/cv_scaler.h"

#include <algorithm>

#include "stmlib/dsp/dsp.h"
#include "stmlib/system/storage.h"
#include "stmlib/utils/random.h"

#include "ringsX_MIDI/dsp/part.h"
#include "ringsX_MIDI/dsp/patch.h"

namespace rings {
  
using namespace std;
using namespace stmlib;

/* static */
ChannelSettings CvScaler::channel_settings_[ADC_CHANNEL_LAST] = {
  { LAW_LINEAR, true, 1.00f },  // ADC_CHANNEL_CV_FREQUENCY
  { LAW_LINEAR, true, 0.1f },  // ADC_CHANNEL_CV_STRUCTURE
  { LAW_LINEAR, true, 0.1f },  // ADC_CHANNEL_CV_BRIGHTNESS
  { LAW_LINEAR, true, 0.05f },  // ADC_CHANNEL_CV_DAMPING
  { LAW_LINEAR, true, 0.01f },  // ADC_CHANNEL_CV_POSITION
  { LAW_LINEAR, false, 1.00f },  // ADC_CHANNEL_CV_V_OCT
  { LAW_LINEAR, false, 0.01f },  // ADC_CHANNEL_POT_FREQUENCY
  { LAW_LINEAR, false, 0.01f },  // ADC_CHANNEL_POT_STRUCTURE
  { LAW_LINEAR, false, 0.01f },  // ADC_CHANNEL_POT_BRIGHTNESS
  { LAW_LINEAR, false, 0.01f },  // ADC_CHANNEL_POT_DAMPING
  { LAW_LINEAR, false, 0.01f },  // ADC_CHANNEL_POT_POSITION
  { LAW_QUARTIC_BIPOLAR, false, 0.005f },  // ADC_CHANNEL_ATTENUVERTER_FREQUENCY
  { LAW_QUADRATIC_BIPOLAR, false, 0.005f },  //ADC_CHANNEL_ATTENUVERTER_STRUCTURE,
  { LAW_QUADRATIC_BIPOLAR, false, 0.005f },  // ADC_CHANNEL_ATTENUVERTER_BRIGHTNESS,
  { LAW_QUADRATIC_BIPOLAR, false, 0.005f },  // ADC_CHANNEL_ATTENUVERTER_DAMPING,
  { LAW_LINEAR, false, 0.01f },  // ADC_CHANNEL_ATTENUVERTER_POSITION (Now dedicated Reverb Pot)
};

void CvScaler::Init(CalibrationData* calibration_data) {
  calibration_data_ = calibration_data;

  adc_.Init();
  trigger_input_.Init();

  transpose_ = 0.0f;
  
  fill(&adc_lp_[0], &adc_lp_[ADC_CHANNEL_LAST], 0.0f);
  
  normalization_probe_.Init();
  normalization_detector_exciter_.Init(0.01f, 0.5f);
  normalization_detector_trigger_.Init(0.05f, 0.9f);
  normalization_detector_v_oct_.Init(0.01f, 0.5f);
  
  inhibit_strum_ = 0;
  fm_cv_ = 0.0f;
  
  normalization_probe_enabled_ = true;
  normalization_probe_forced_state_ = false;
}

void CvScaler::DetectAudioNormalization(Codec::Frame* in, size_t size) {
  int32_t count = 0;
  short* input_samples = &in->r;
  for (size_t i = 0; i < size; i += 8) {
    short s = input_samples[i * 2];
    if (s > 50 && s < 1500) {
      ++count;
    } else if (s > -1500 && s < -50) {
      --count;
    }
  }
  float y = static_cast<float>(count) / static_cast<float>(size >> 3);
  float x = normalization_probe_value_[1] ? -1.0f : 1.0f;
  
  normalization_detector_exciter_.Process(x, y);
  if (normalization_detector_exciter_.normalized()) {
    for (size_t i = 0; i < size; ++i) {
      input_samples[i * 2] = 0;
    }
  }
}

void CvScaler::DetectNormalization() {
  if (normalization_probe_value_[0] == trigger_input_.DummyRead()) {
    normalization_detector_trigger_.Process(1.0f, 1.0f);
  } else {
    normalization_detector_trigger_.Process(1.0f, -1.0f);
  }
  
  float x = adc_.float_value(ADC_CHANNEL_CV_V_OCT) - calibration_data_->normalization_detection_threshold;
  float y = normalization_probe_value_[0] ? -1.0f : 1.0f;
  if (x > -0.5f && x < 0.5f) {
    x = x < 0.0f ? -1.0f : 1.0f;
    normalization_detector_v_oct_.Process(x, y);
  } else {
    normalization_detector_v_oct_.Process(0.0f, y);
  }
  
  normalization_probe_value_[1] = normalization_probe_value_[0];
  normalization_probe_value_[0] = Random::GetWord() >> 31;
  bool new_state = normalization_probe_enabled_
      ? normalization_probe_value_[0]
      : normalization_probe_forced_state_;
  normalization_probe_.Write(new_state);
}

#define ATTENUVERT(destination, NAME, min_val, max_val) \
  { \
    float cv = GetMidiParameter(ADC_CHANNEL_CV_ ## NAME); \
    float pot = GetMidiParameter(ADC_CHANNEL_POT_ ## NAME); \
    float attenuverter = GetMidiParameter(ADC_CHANNEL_ATTENUVERTER_ ## NAME); \
    if (midi_control_active[ADC_CHANNEL_POT_ ## NAME]) { \
      pot = midi_parameters[ADC_CHANNEL_POT_ ## NAME]; \
    } \
    float value = cv * attenuverter + pot; \
    CONSTRAIN(value, min_val, max_val); \
    destination = value; \
  }

extern float midi_parameters[ADC_CHANNEL_LAST];
extern bool midi_control_active[ADC_CHANNEL_LAST];
extern bool midi_pot_latched[ADC_CHANNEL_LAST];

float CvScaler::GetMidiParameter(int adc_channel) {
  float physical_pot = adc_lp_[adc_channel];
  
  if (midi_control_active[adc_channel]) {
    if (!midi_pot_latched[adc_channel]) {
      // 检查物理电位器是否捕获（Catch-up）了 MIDI 值
      // 阈值设为 0.05 (约 5%)
      if (fabsf(physical_pot - midi_parameters[adc_channel]) < 0.05f) {
        midi_pot_latched[adc_channel] = true;
        midi_control_active[adc_channel] = false; // 交还控制权
      }
    }
    
    if (midi_control_active[adc_channel]) {
      return midi_parameters[adc_channel];
    }
  }
  return physical_pot;
}

void CvScaler::Read(int model, Patch* patch, PerformanceState* performance_state) {
  // Process all CVs / pots.
  for (size_t i = 0; i < ADC_CHANNEL_LAST; ++i) {
    const ChannelSettings& settings = channel_settings_[i];
    float value = adc_.float_value(i);
    if (settings.remove_offset) {
      value = calibration_data_->offset[i] - value;
    }
    switch (settings.law) {
      case LAW_QUADRATIC_BIPOLAR:
        {
          value = value - 0.5f;
          float value2 = value * value * 4.0f * 3.3f;
          value = value < 0.0f ? -value2 : value2;
        }
        break;

      case LAW_QUARTIC_BIPOLAR:
        {
          value = value - 0.5f;
          float value2 = value * value * 4.0f;
          float value4 = value2 * value2 * 3.3f;
          value = value < 0.0f ? -value4 : value4;
        }
        break;

      default:
        break;
    }
    adc_lp_[i] += settings.lp_coefficient * (value - adc_lp_[i]);
  }
  
  ATTENUVERT(patch->structure, STRUCTURE, 0.0f, 0.9995f);
  ATTENUVERT(patch->brightness, BRIGHTNESS, 0.0f, 1.0f);
  ATTENUVERT(patch->damping, DAMPING, 0.0f, 1.0f);
  
  if (model >= 7) {
    // Audrey 模式功能互换：
    float cv_pos = GetMidiParameter(ADC_CHANNEL_CV_POSITION);
    
    // 大旋钮 + CV 现在控制混响 (Reverb)
    float reverb_pot = GetMidiParameter(ADC_CHANNEL_POT_POSITION);
    if (midi_control_active[ADC_CHANNEL_POT_POSITION]) {
      reverb_pot = midi_parameters[ADC_CHANNEL_POT_POSITION];
    }
    float reverb = reverb_pot + cv_pos;
    CONSTRAIN(reverb, 0.0f, 1.0f);
    // 混响大旋钮最小从 30% 开始
    patch->reverb_amount = 0.3f + reverb * 0.7f;
    
    // 小旋钮 + CV 现在控制 Position (反馈/调制量)
    float pos = GetMidiParameter(ADC_CHANNEL_ATTENUVERTER_POSITION) + cv_pos;
    CONSTRAIN(pos, 0.0f, 1.0f);
    patch->position = pos;
  } else {
    // 原版模式：恢复 Position 小旋钮作为物理输入 CV 的衰减器 (Attenuverter)
    ATTENUVERT(patch->position, POSITION, 0.0f, 1.0f);
    patch->reverb_amount = 0.0f; 
  }
  
  float fm = GetMidiParameter(ADC_CHANNEL_CV_FREQUENCY) * 48.0f;
  float error = fm - fm_cv_;
  if (fabs(error) >= 0.8f) {
    fm_cv_ = fm;
  } else {
    fm_cv_ += 0.02f * error;
  }
  performance_state->fm = fm_cv_ * GetMidiParameter(ADC_CHANNEL_ATTENUVERTER_FREQUENCY);
  CONSTRAIN(performance_state->fm, -48.0f, 48.0f);
  
  // Audrey 模式频率范围更宽 (8 个八度 vs 5 个八度)
  float pot_freq = GetMidiParameter(ADC_CHANNEL_POT_FREQUENCY);
  if (midi_control_active[ADC_CHANNEL_POT_FREQUENCY]) {
    pot_freq = midi_parameters[ADC_CHANNEL_POT_FREQUENCY];
  }
  float frequency_range = (model >= 7) ? 96.0f : 60.0f;
  float transpose = frequency_range * pot_freq;
  float hysteresis = transpose - transpose_ > 0.0f ? -0.3f : +0.3f;
  transpose_ = static_cast<int32_t>(transpose + hysteresis + 0.5f);
  
  float note = calibration_data_->pitch_offset;
  note += GetMidiParameter(ADC_CHANNEL_CV_V_OCT) * calibration_data_->pitch_scale;
  
  performance_state->note = note;
  // 允许 HOHO 模式起始频率更低 (0.0f vs 12.0f)
  float base_note = (model >= 7) ? 0.0f : 12.0f;
  performance_state->tonic = base_note + transpose_;
  
  DetectNormalization();
  
  // Strumming / internal exciter triggering logic.
  bool internal_strum = normalization_detector_trigger_.normalized();
  bool internal_exciter = normalization_detector_exciter_.normalized();
  bool internal_note = normalization_detector_v_oct_.normalized();
  performance_state->internal_exciter = internal_exciter;
  performance_state->internal_strum = internal_strum;
  performance_state->internal_note = internal_note;
  performance_state->strum = trigger_input_.rising_edge();
  
  if (performance_state->internal_note) {
    // Remove quantization when nothing is plugged in the V/OCT input.
    performance_state->note = 0.0f;
    performance_state->tonic = 12.0f + transpose;
  }
  
  // Hysteresis on chord.
  float chord = calibration_data_->offset[ADC_CHANNEL_CV_STRUCTURE] - \
      adc_.float_value(ADC_CHANNEL_CV_STRUCTURE);
  if (midi_control_active[ADC_CHANNEL_CV_STRUCTURE]) chord = midi_parameters[ADC_CHANNEL_CV_STRUCTURE] - 0.5f;
  
  float attenuverter = GetMidiParameter(ADC_CHANNEL_ATTENUVERTER_STRUCTURE);
  float pot = GetMidiParameter(ADC_CHANNEL_POT_STRUCTURE);
  chord = chord * attenuverter + pot;
  chord *= static_cast<float>(kNumChords - 1);
  hysteresis = chord - chord_ > 0.0f ? -0.1f : +0.1f;
  chord_ = static_cast<int32_t>(chord + hysteresis + 0.5f);
  CONSTRAIN(chord_, 0, kNumChords - 1);
  performance_state->chord = chord_;
  
  adc_.Convert();
  trigger_input_.Read();
}

}  // namespace rings
