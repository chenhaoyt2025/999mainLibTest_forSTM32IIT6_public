#include "plaits_adapter.h"

#include "plaits/dsp/dsp.h"
#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"

namespace
{
constexpr std::size_t kMaxBlock = 256;

plaits::Voice       g_voice;
plaits::Patch       g_patch;
plaits::Modulations g_mod;
char                g_pool[32768];
bool                g_ready      = false;

plaits::Voice::Frame g_frames[kMaxBlock];

float Clamp01(float value)
{
    if(value < 0.0f)
        return 0.0f;
    if(value > 1.0f)
        return 1.0f;
    return value;
}
} // namespace

void PlaitsAdapterInit()
{
    stmlib::BufferAllocator allocator(g_pool, sizeof(g_pool));
    g_voice.Init(&allocator);
    g_ready = true;
}

bool PlaitsAdapterReady()
{
    return g_ready;
}

void PlaitsAdapterRender(float        pitch_norm,
                         float        timbre_norm,
                         float        morph_norm,
                         float        harmonics_norm,
                         uint8_t      model_index,
                         uint8_t      morph_mode,
                         bool         gate_on,
                         std::size_t  size,
                         float*       out_l,
                         float*       out_r)
{
    if(!g_ready || out_l == nullptr || out_r == nullptr)
        return;

    if(size > kMaxBlock)
        size = kMaxBlock;

    pitch_norm     = Clamp01(pitch_norm);
    timbre_norm    = Clamp01(timbre_norm);
    morph_norm     = Clamp01(morph_norm);
    harmonics_norm = Clamp01(harmonics_norm);
    model_index    = static_cast<uint8_t>(model_index % 24u);

    float note = 36.0f + pitch_norm * 48.0f;
    if(morph_mode == 0u)
    {
        const int octave = static_cast<int>(pitch_norm * 4.0f) - 2;
        note += static_cast<float>(octave * 12);
    }

    g_patch.note                        = note;
    g_patch.harmonics                   = harmonics_norm;
    g_patch.timbre                      = timbre_norm;
    g_patch.morph                       = morph_norm;
    g_patch.frequency_modulation_amount = 0.0f;
    g_patch.timbre_modulation_amount    = 0.0f;
    g_patch.morph_modulation_amount     = 0.0f;
    g_patch.harmonics_modulation_amount = 0.0f;
    g_patch.engine                      = static_cast<int>(model_index);
    g_patch.decay                       = 0.40f + 0.55f * morph_norm;
    g_patch.lpg_colour                  = 0.25f + 0.65f * timbre_norm;
    g_patch.dj_eq                       = 0.5f;
    g_patch.engine_absolute_mode        = (morph_mode == 4u);

    if(morph_mode == 1u)
        g_patch.decay = 0.05f + 0.90f * morph_norm;
    else if(morph_mode == 2u)
        g_patch.lpg_colour = 0.02f + 0.96f * morph_norm;
    else if(morph_mode == 3u)
        g_patch.dj_eq = morph_norm;

    g_mod.engine            = static_cast<float>(g_patch.engine) / 23.0f;
    g_mod.note              = 0.0f;
    g_mod.frequency         = 0.0f;
    g_mod.harmonics         = 0.0f;
    g_mod.timbre            = 0.0f;
    g_mod.morph             = 0.0f;
    g_mod.level             = 1.0f;
    g_mod.frequency_patched = false;
    g_mod.timbre_patched    = false;
    g_mod.morph_patched     = false;
    g_mod.level_patched     = false;
    g_mod.trigger_patched   = false;
    g_mod.trigger           = 0.0f;

    std::size_t processed = 0;
    while(processed < size)
    {
        std::size_t chunk = size - processed;
        if(chunk > plaits::kBlockSize)
            chunk = plaits::kBlockSize;

        g_voice.Render(g_patch, g_mod, g_frames, chunk);
        for(std::size_t i = 0; i < chunk; ++i)
        {
            const float l = static_cast<float>(g_frames[i].out) / 32768.0f;
            const float r = static_cast<float>(g_frames[i].aux) / 32768.0f;
            const float a = gate_on ? 0.85f : 0.0f;
            out_l[processed + i] = l * a;
            out_r[processed + i] = r * a;
        }
        processed += chunk;
    }
}
