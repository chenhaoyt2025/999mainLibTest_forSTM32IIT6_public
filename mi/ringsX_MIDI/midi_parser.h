// Copyright 2024 GitHub Copilot.
//
// Author: GitHub Copilot
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
// Simple MIDI CC parser.

#ifndef PLAITS_MIDI_PARSER_H_
#define PLAITS_MIDI_PARSER_H_

#include "stmlib/stmlib.h"

namespace rings {

enum MidiParserState {
  MIDI_PARSER_STATE_IDLE,
  MIDI_PARSER_STATE_CC_BYTE_1,
  MIDI_PARSER_STATE_CC_BYTE_2,
  MIDI_PARSER_STATE_NOTE_ON_BYTE_1,
  MIDI_PARSER_STATE_NOTE_ON_BYTE_2,
  MIDI_PARSER_STATE_NOTE_OFF_BYTE_1,
  MIDI_PARSER_STATE_NOTE_OFF_BYTE_2,
  MIDI_PARSER_STATE_PB_BYTE_1,
  MIDI_PARSER_STATE_PB_BYTE_2,
};

enum MessageType {
  MIDI_MESSAGE_NONE,
  MIDI_MESSAGE_CC,
  MIDI_MESSAGE_NOTE_ON,
  MIDI_MESSAGE_NOTE_OFF,
  MIDI_MESSAGE_PITCH_BEND,
};

class MidiParser {
 public:
  MidiParser() { }
  ~MidiParser() { }

  void Init() {
    state_ = MIDI_PARSER_STATE_IDLE;
    running_status_ = 0;
  }

  struct Message {
    MessageType type;
    uint8_t channel;
    union {
      uint8_t data1;
      uint8_t control;
      uint8_t note;
    };
    union {
      uint8_t data2;
      uint8_t value;
      uint8_t velocity;
    };
    uint16_t pitch_bend;
  };

  bool ProcessByte(uint8_t byte, Message* message) {
    if (byte >= 0xF8) { // Real-time message
      return false;
    }

    if (byte >= 0x80) { // Status byte
      uint8_t command = byte & 0xF0;
      running_status_ = byte;
      if (command == 0x80) state_ = MIDI_PARSER_STATE_NOTE_OFF_BYTE_1;
      else if (command == 0x90) state_ = MIDI_PARSER_STATE_NOTE_ON_BYTE_1;
      else if (command == 0xB0) state_ = MIDI_PARSER_STATE_CC_BYTE_1;
      else if (command == 0xE0) state_ = MIDI_PARSER_STATE_PB_BYTE_1;
      else {
        state_ = MIDI_PARSER_STATE_IDLE;
        running_status_ = 0;
      }
      return false;
    }

    // Data byte
    switch (state_) {
      case MIDI_PARSER_STATE_NOTE_OFF_BYTE_1:
        last_data1_ = byte;
        state_ = MIDI_PARSER_STATE_NOTE_OFF_BYTE_2;
        break;
      case MIDI_PARSER_STATE_NOTE_OFF_BYTE_2:
        message->type = MIDI_MESSAGE_NOTE_OFF;
        message->channel = running_status_ & 0x0F;
        message->data1 = last_data1_;
        message->data2 = byte;
        state_ = MIDI_PARSER_STATE_NOTE_OFF_BYTE_1;
        return true;

      case MIDI_PARSER_STATE_NOTE_ON_BYTE_1:
        last_data1_ = byte;
        state_ = MIDI_PARSER_STATE_NOTE_ON_BYTE_2;
        break;
      case MIDI_PARSER_STATE_NOTE_ON_BYTE_2:
        message->type = byte == 0 ? MIDI_MESSAGE_NOTE_OFF : MIDI_MESSAGE_NOTE_ON;
        message->channel = running_status_ & 0x0F;
        message->data1 = last_data1_;
        message->data2 = byte;
        state_ = MIDI_PARSER_STATE_NOTE_ON_BYTE_1;
        return true;

      case MIDI_PARSER_STATE_CC_BYTE_1:
        last_data1_ = byte;
        state_ = MIDI_PARSER_STATE_CC_BYTE_2;
        break;
      case MIDI_PARSER_STATE_CC_BYTE_2:
        message->type = MIDI_MESSAGE_CC;
        message->channel = running_status_ & 0x0F;
        message->data1 = last_data1_;
        message->data2 = byte;
        state_ = MIDI_PARSER_STATE_CC_BYTE_1;
        return true;

      case MIDI_PARSER_STATE_PB_BYTE_1:
        last_data1_ = byte;
        state_ = MIDI_PARSER_STATE_PB_BYTE_2;
        break;
      case MIDI_PARSER_STATE_PB_BYTE_2:
        message->type = MIDI_MESSAGE_PITCH_BEND;
        message->channel = running_status_ & 0x0F;
        message->data1 = last_data1_;
        message->data2 = byte;
        message->pitch_bend = last_data1_ | (byte << 7);
        state_ = MIDI_PARSER_STATE_PB_BYTE_1;
        return true;

      default:
        break;
    }
    return false;
  }

 private:
  MidiParserState state_;
  uint8_t running_status_;
  uint8_t last_data1_;

  DISALLOW_COPY_AND_ASSIGN(MidiParser);
};

}  // namespace plaits

#endif  // PLAITS_MIDI_PARSER_H_
