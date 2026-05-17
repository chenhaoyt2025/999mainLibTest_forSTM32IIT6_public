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

#include "ringsX_MIDI/drivers/adc.h"
#include "ringsX_MIDI/drivers/codec.h"
#include "ringsX_MIDI/drivers/debug_pin.h"
#include "ringsX_MIDI/drivers/debug_port.h"
#include "ringsX_MIDI/drivers/system.h"
#include "ringsX_MIDI/drivers/version.h"
#include "ringsX_MIDI/dsp/part.h"
#include "ringsX_MIDI/dsp/strummer.h"
#include "ringsX_MIDI/dsp/string_synth_part.h"
#include "ringsX_MIDI/cv_scaler.h"
#include "ringsX_MIDI/settings.h"
#include "ringsX_MIDI/ui.h"
#include "ringsX_MIDI/drivers/midi_port.h"
#include "ringsX_MIDI/midi_parser.h"
#include "stmlib/system/system_clock.h" // 用于串口心跳信号的计时

// #define PROFILE_INTERRUPT 1

using namespace rings;
using namespace stmlib;

uint16_t reverb_buffer[32768] __attribute__ ((section (".ccmdata")));

Codec codec;
CvScaler cv_scaler;
DebugPort debug_port;
Part part;
Settings settings;
StringSynthPart string_synth;
Strummer strummer;
Ui ui;

rings::MidiPort midi_port;
MidiParser midi_parser;

namespace rings {
float midi_parameters[ADC_CHANNEL_LAST];
bool midi_control_active[ADC_CHANNEL_LAST];
bool midi_pot_latched[ADC_CHANNEL_LAST];
}

// Default interrupt handlers.
extern "C" {

int __errno;

void NMI_Handler() { }
void HardFault_Handler() { while (1); }
void MemManage_Handler() { while (1); }
void BusFault_Handler() { while (1); }
void UsageFault_Handler() { while (1); }
void SVC_Handler() { }
void DebugMon_Handler() { }
void PendSV_Handler() { }

bool midi_note_triggered = false;
uint32_t midi_button_timer[2];
bool midi_enabled = false; // 默认关闭 CC 1-5 和 MIDI Note

void SysTick_Handler() {
  ui.Poll();
  midi_port.Poll();
  
  for (int i = 0; i < 2; ++i) {
    if (midi_button_timer[i] > 0) {
      if (--midi_button_timer[i] == 0) {
        stmlib::Event e;
        e.control_type = stmlib::CONTROL_SWITCH;
        e.control_id = i;
        e.data = 1; // Released
        ui.queue()->AddEvent(stmlib::CONTROL_SWITCH, i, 1);
      }
    }
  }

  while (midi_port.readable()) {
    uint8_t midibyte = midi_port.Read();
    
    MidiParser::Message msg;
    if (midi_parser.ProcessByte(midibyte, &msg)) {
      // CC 110: MIDI Channel (Global, skip filter)
      if (msg.type == MIDI_MESSAGE_CC && msg.control == 110) {
        uint8_t new_chan = msg.value;
        if (new_chan > 16) new_chan = 16;
        settings.mutable_state()->midi_channel = new_chan;
        ui.SaveState();
        continue;
      }

      // Filter by Channel
      uint8_t target_chan = settings.state().midi_channel;
      if (target_chan != 0 && msg.channel != (target_chan - 1)) {
        continue;
      }

      if (msg.type == MIDI_MESSAGE_CC) {
        if (msg.control == 9) {
          midi_enabled = msg.value >= 64;
          continue;
        }

        int parameter_index = -1;
        switch (msg.control) {
          case 1: parameter_index = ADC_CHANNEL_POT_FREQUENCY; break;
          case 2: parameter_index = ADC_CHANNEL_POT_STRUCTURE; break;
          case 3: parameter_index = ADC_CHANNEL_POT_BRIGHTNESS; break;
          case 4: parameter_index = ADC_CHANNEL_POT_DAMPING; break;
          case 5: parameter_index = ADC_CHANNEL_POT_POSITION; break;
          case 11: // Left Button
            if (msg.value >= 64) {
              ui.queue()->AddEvent(stmlib::CONTROL_SWITCH, 0, 0); // Pressed
              midi_button_timer[0] = 50; // 50ms 后释放
            }
            break;
          case 12: // Right Button
            if (msg.value >= 64) {
              ui.queue()->AddEvent(stmlib::CONTROL_SWITCH, 1, 0); // Pressed
              midi_button_timer[1] = 50; // 50ms 后释放
            }
            break;
          case 110: // MIDI Channel
            if (msg.value >= 1 && msg.value <= 16) {
              settings.mutable_state()->midi_channel = msg.value - 1;
              ui.SaveState();
            }
            break;
        }
        if (parameter_index != -1 && midi_enabled) {
          midi_parameters[parameter_index] = static_cast<float>(msg.value) / 127.0f;
          midi_control_active[parameter_index] = true;
          midi_pot_latched[parameter_index] = false; // 进入 Catch-up 模式
        }
      } else if (msg.type == MIDI_MESSAGE_NOTE_ON && midi_enabled) {
        midi_parameters[ADC_CHANNEL_CV_V_OCT] = static_cast<float>(msg.note - 60) / 12.0f;
        midi_control_active[ADC_CHANNEL_CV_V_OCT] = true;
        // 移除自动触发逻辑，仅作为 CV 注入 (V/OCT)
      }
    }
  }

  if (settings.freshly_baked()) {
    if (debug_port.readable()) {
      uint8_t command = debug_port.Read();
      uint8_t response = ui.HandleFactoryTestingRequest(command);
      debug_port.Write(response);
    }
  }
}

}

float in[kMaxBlockSize];
float out[kMaxBlockSize];
float aux[kMaxBlockSize];

const float kNoiseGateThreshold = 0.00003f;
float in_level = 0.0f;

void FillBuffer(Codec::Frame* input, Codec::Frame* output, size_t size) {
#ifdef PROFILE_INTERRUPT
  TIC
#endif  // PROFILE_INTERRUPT
  PerformanceState performance_state;
  Patch patch;
  
  cv_scaler.DetectAudioNormalization(input, size);
  cv_scaler.Read(part.model(), &patch, &performance_state);

  if (midi_note_triggered) {
    performance_state.strum = true;
    midi_note_triggered = false;
  }
  
  if (settings.state().easter_egg) {
    for (size_t i = 0; i < size; ++i) {
      in[i] = static_cast<float>(input[i].r) / 32768.0f;
    }
    strummer.Process(NULL, size, &performance_state);
    string_synth.Process(performance_state, patch, in, out, aux, size);
  } else {
    // Apply noise gate.
    for (size_t i = 0; i < size; ++i) {
      float in_sample = static_cast<float>(input[i].r) / 32768.0f;
      float error, gain;
      error = in_sample * in_sample - in_level;
      in_level += error * (error > 0.0f ? 0.1f : 0.0001f);
      gain = in_level <= kNoiseGateThreshold 
            ? (1.0f / kNoiseGateThreshold) * in_level : 1.0f;
      in[i] = gain * in_sample;
    }
    strummer.Process(in, size, &performance_state);
    part.Process(performance_state, patch, in, out, aux, size);
  }
  
  for (size_t i = 0; i < size; ++i) {
    output[i].l = Clip16(static_cast<int32_t>(out[i] * 32768.0f));
    output[i].r = Clip16(static_cast<int32_t>(aux[i] * 32768.0f));
  }
  ui.set_strumming_flag(performance_state.strum);
#ifdef PROFILE_INTERRUPT
  TOC
#endif  // PROFILE_INTERRUPT
}

void Init() {
  System sys;
  Version version;
  
  sys.Init(true);
  version.Init();

  strummer.Init(0.01f, kSampleRate / kMaxBlockSize);
  part.Init(reverb_buffer);
  string_synth.Init(reverb_buffer);

  settings.Init();
  cv_scaler.Init(settings.mutable_calibration_data());
  ui.Init(&settings, &cv_scaler, &part, &string_synth);
  
  if (!codec.Init(!version.revised(), kSampleRate)) {
    ui.Panic();
  }
  if (!codec.Start(kMaxBlockSize, &FillBuffer)) {
    ui.Panic();
  }
  codec.set_line_input_gain(22);

  midi_port.Init();

  // 强制开启调试串口，用于监控 Audrey II 的运行状态
#ifdef PROFILE_INTERRUPT
  DebugPin::Init();
#else
  // debug_port.Init(); // ！！！冲突点：DebugPort 使用与 MIDI 相同的 USART1 (PA9/PA10)
#endif  // PROFILE_INTERRUPT

  sys.StartTimers();
}

int main(void) {
  Init();
  uint32_t last_print = 0;
  while (1) {
    ui.DoEvents();
    // 心跳信号：每隔 1000ms 向串口发送一个“.”，确认 CPU 仍在正常运行
    if (system_clock.milliseconds() - last_print > 1000) {
      debug_port.Write('.');
      last_print = system_clock.milliseconds();
    }
  }
}
