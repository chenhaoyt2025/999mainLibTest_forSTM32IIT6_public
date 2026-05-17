// Copyright 2014 Emilie Gillet.
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

#ifndef RINGS_HILBERT_DSP_HILBERT_TRANSFORM_H_
#define RINGS_HILBERT_DSP_HILBERT_TRANSFORM_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/dsp.h"

namespace rings {

const float lut_ap_poles[] = {
   9.999174437e-01,  9.997160329e-01,  9.993897602e-01,  9.987952776e-01,
   9.976718129e-01,  9.955280098e-01,  9.914315323e-01,  9.836199785e-01,
   9.688016569e-01,  9.409767040e-01,  8.897147107e-01,  7.984785110e-01,
   6.454684139e-01,  4.118108699e-01,  9.725667152e-02, -2.775386379e-01,
  -7.176356738e-01,
};

class HilbertTransform {
 public:
  HilbertTransform() { }
  ~HilbertTransform() { }
  
  void Init() {
    for (int32_t i = 0; i < 17; ++i) {
      filter_[i].Init(-lut_ap_poles[i]);
    }
  }
  
  inline void Process(float in, float* i_out, float* q_out) {
    for (int32_t i = 0; i < 17; ++i) {
      float* destination = i & 1 ? q_out : i_out;
      float source = i <= 1 ? in : *destination;
      *destination = filter_[i].Process(source);
    }
  }
  
  void Process(const float* in, float* i_out, float* q_out, size_t size) {
    for (int32_t i = 0; i < 17; ++i) {
      float* destination = i & 1 ? q_out : i_out;
      const float* source = i <= 1 ? in : destination;
      filter_[i].Process(source, destination, size);
    }
  }
  
 private:
  class AllPassFilter {
   public:
    AllPassFilter() { }
    ~AllPassFilter() { }

    void Init(float coefficient) {
      x_ = 0.0f;
      y_ = 0.0f;
      coefficient_ = coefficient;
    }

    inline float Process(float in) {
      float y = coefficient_ * (in - y_) + x_;
      x_ = in;
      y_ = y;
      return y;
    }

    inline void Process(const float* in, float* out, size_t size) {
      const float coefficient = coefficient_;
      float xp = x_;
      float yp = y_;
      while (size--) {
        float x = *in++;
        float y = coefficient * (x - yp) + xp;
        *out++ = y;
        xp = x;
        yp = y;
      }
      x_ = xp;
      y_ = yp;
    }

   private:
    float x_;
    float y_;
    float coefficient_;

    DISALLOW_COPY_AND_ASSIGN(AllPassFilter);
  };

  AllPassFilter filter_[17];

  DISALLOW_COPY_AND_ASSIGN(HilbertTransform);
};

}  // namespace rings

#endif  // RINGS_HILBERT_DSP_HILBERT_TRANSFORM_H_
