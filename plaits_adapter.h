#pragma once

#include <cstddef>
#include <cstdint>

void PlaitsAdapterInit();
bool PlaitsAdapterReady();

void PlaitsAdapterRender(float        pitch_norm,
                         float        timbre_norm,
                         float        morph_norm,
                         float        harmonics_norm,
                         uint8_t      model_index,
                         uint8_t      morph_mode,
                         bool         gate_on,
                         std::size_t  size,
                         float*       out_l,
                         float*       out_r);
