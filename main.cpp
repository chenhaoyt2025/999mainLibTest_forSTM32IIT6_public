#include "daisy_seed.h"
#include "daisysp.h"
#include "ringsX_MIDI/dsp/part.h"
#include "ringsX_MIDI/dsp/patch.h"
#include "ringsX_MIDI/dsp/performance_state.h"
#include "plaits_adapter.h"
#include "assets_gen/rings_panel.h"
#include "assets_gen/plaits_panel.h"
#include "assets_gen/scope_panel.h"
#include "assets_gen/vcv_knob_big.h"
#include "assets_gen/vcv_knob_small.h"
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
extern "C"
{
#include "BSP/LTDC/lcd_rgb.h"
#include "BSP/LTDC/sdram.h"
#include "BSP/LTDC/touch_800x480.h"
}

using namespace daisy;
using namespace daisysp;

DaisySeed   hw;
SaiHandle   sai1_tdm;
AK4619      codec_1;
UartHandler debug_uart;
bool        g_uart_ready = false;

constexpr size_t  kSai1TdmSlots        = 4;
constexpr size_t  kAudioOutChannels     = 4;
constexpr size_t  kAudioInChannels      = 4;
constexpr size_t  kModePrintMs          = 2000;
constexpr uint8_t kCodecAddrCandidates[] = {0x10, 0x11};
constexpr size_t  kCodecProbeRetries    = 12;
constexpr size_t  kCodecProbeDelayMs    = 25;
constexpr bool    kUiOnlyVcv            = false;

enum class StreamMode
{
    LOOP_4IN_4OUT,
    TEST_TONE_1K,
    RINGS_TEST_SQUARE,
    PLAITS_TEST_SQUARE,
};

enum class RunMode
{
    ACTIVE,
    STOP,
};

enum class UiScene
{
    SCOPE = 0,
    RINGS_CTRL,
    PLAITS_CTRL,
    RINGS_VCV_UI,
    PLAITS_VCV_UI,
    VECTOR,
    SPECTRUM,
    NEON,
    ORBIT,
    COUNT,
};

volatile uint32_t g_callback_count = 0;
StreamMode        g_stream_mode    = StreamMode::LOOP_4IN_4OUT;
RunMode           g_run_mode       = RunMode::STOP;
UiScene           g_ui_scene       = UiScene::SCOPE;
size_t            g_in_channels    = kAudioInChannels;
size_t            g_out_channels   = kAudioOutChannels;
volatile uint8_t  g_usb_cmd_byte   = 0;
volatile bool     g_usb_cmd_pending = false;
volatile float    g_monitor_l = 0.0f;
volatile float    g_monitor_r = 0.0f;

constexpr uint16_t kVisHistory = 320;
float              g_vis_l[kVisHistory];
float              g_vis_r[kVisHistory];
uint16_t           g_vis_wr       = 0;
float              g_spec_env[8]  = {0.0f};
float              g_prev_l1      = 0.0f;
float              g_prev_l2      = 0.0f;
float              g_prev_l3      = 0.0f;
float              g_prev_r1      = 0.0f;
float              g_prev_r2      = 0.0f;
float              g_prev_r3      = 0.0f;
float              g_phase_l      = 0.0f;
float              g_phase_r      = 0.0f;
uint8_t            g_prev_touch   = 0;
uint32_t           g_last_draw_ms = 0;
uint32_t           g_last_touch_toggle_ms = 0;
uint32_t           g_bottom_press_start_ms = 0;
bool               g_mode_switched_on_press = false;
bool               g_touch_demo_layout_ready = false;
bool               g_touch_paint_layout_ready = false;
bool               g_touch_tone_ctrl_layout_ready = false;
float              g_scope_x_scale_norm = 0.0f;
float              g_scope_y_scale_norm = 0.0f;
float              g_scope_x_pos_norm   = 0.5f;
float              g_scope_y_pos_norm   = 0.5f;
float              g_scope_time_norm    = 0.56f;
float              g_scope_thresh_norm  = 0.5f;
bool               g_scope_lissajous    = false;
bool               g_scope_trig_enabled = true;
bool               g_scope_liss_latch   = false;
bool               g_scope_trig_latch   = false;
bool               g_paint_prev_down = false;
bool               g_paint_clear_latched = false;
uint16_t           g_paint_prev_x = 0u;
uint16_t           g_paint_prev_y = 0u;
uint8_t            g_paint_color_idx = 0u;
constexpr uint32_t kUiDrawPeriodMs = 50;
constexpr uint32_t kTouchSwitchHoldMs = 120;
constexpr uint32_t kTouchSwitchDebounceMs = 500;
constexpr uint16_t kTouchSwitchYMin = 420;
constexpr uint16_t kSceneX = 0;
constexpr uint16_t kSceneY = 40;
constexpr uint16_t kSceneW = 360;
constexpr uint16_t kSceneH = 220;
constexpr uint16_t kSceneYEnd = static_cast<uint16_t>(kSceneY + kSceneH);
constexpr uint16_t kSceneXEnd = static_cast<uint16_t>(kSceneX + kSceneW);

float             g_tone_phase         = 0.0f;
uint32_t          g_pattern_sample_pos = 0;
volatile float    g_tone_freq_hz       = 1000.0f;
volatile float    g_tone_amp           = 0.20f;
float             g_tone_ctrl_knob_norm = 0.5f;
float             g_tone_ctrl_vol_norm  = 0.20f;
constexpr uint32_t kSliderRecMaxMs     = 10000u;
constexpr uint32_t kSliderRecStepMs    = kUiDrawPeriodMs;
constexpr size_t   kSliderRecMaxSamples
    = static_cast<size_t>((kSliderRecMaxMs + kSliderRecStepMs - 1u) / kSliderRecStepMs);
float              g_slider_rec_buf[kSliderRecMaxSamples] = {0.0f};
size_t             g_slider_rec_len         = 0u;
size_t             g_slider_play_idx        = 0u;
uint32_t           g_slider_last_step_ms    = 0u;
bool               g_slider_rec_active      = false;
bool               g_slider_play_active     = false;
bool               g_slider_touch_prev      = false;
struct RingsUiCache
{
    bool    valid          = false;
    float   pitch_norm     = 0.0f;
    float   structure_norm = 0.0f;
    float   brightness_norm = 0.0f;
    float   damping_norm   = 0.0f;
    float   feedback_norm  = 0.0f;
    float   reverb_norm    = 0.0f;
    bool    gate           = false;
    uint8_t mode           = 0u;
};
struct PlaitsUiCache
{
    bool    valid           = false;
    float   pitch_norm      = 0.0f;
    float   timbre_norm     = 0.0f;
    float   morph_norm      = 0.0f;
    float   harmonics_norm  = 0.0f;
    float   model_norm      = 0.0f;
    bool    gate            = false;
    uint8_t model_index     = 0u;
    uint8_t morph_mode      = 0u;
};
RingsUiCache g_rings_ui_cache;
PlaitsUiCache g_plaits_ui_cache;
float              g_rings_pitch_norm       = 0.45f;
float              g_rings_structure_norm   = 0.45f;
float              g_rings_brightness_norm  = 0.55f;
float              g_rings_damping_norm     = 0.50f;
float              g_rings_feedback_norm    = 0.35f;
float              g_rings_reverb_mix_norm  = 0.25f;
uint8_t            g_rings_audrey_mode      = 0u;
bool               g_rings_gate             = true;
bool               g_rings_gate_btn_latch   = false;
bool               g_rings_mode_btn_latch   = false;
float              g_rings_car_phase        = 0.0f;
float              g_rings_mod_phase        = 0.0f;
float              g_rings_env              = 0.0f;
float              g_rings_fb_state         = 0.0f;
float              g_rings_rev_state_l      = 0.0f;
float              g_rings_rev_state_r      = 0.0f;
float              g_plaits_pitch_norm      = 0.60f;
float              g_plaits_timbre_norm     = 0.40f;
float              g_plaits_morph_norm      = 0.35f;
float              g_plaits_harmonics_norm  = 0.50f;
float              g_plaits_model_norm      = 0.00f;
uint8_t            g_plaits_model_index     = 0u;
uint8_t            g_plaits_morph_mode      = 0u;
bool               g_plaits_gate            = true;
bool               g_plaits_gate_btn_latch  = false;
bool               g_plaits_model_btn_latch = false;
bool               g_plaits_morph_btn_latch = false;
constexpr float   kTwoPi               = 6.28318530718f;
constexpr float   kToneSampleRate      = 48000.0f;
constexpr uint32_t kToneCycleSamples   = static_cast<uint32_t>(kToneSampleRate * 2.0f);
constexpr uint32_t kToneGuardSamples   = static_cast<uint32_t>(kToneSampleRate * 0.25f);
constexpr uint32_t kToneActiveSamples  = kToneCycleSamples - (kToneGuardSamples * 2);
constexpr uint32_t kTonePulseWidthSamples
    = static_cast<uint32_t>(kToneSampleRate * 0.06f);

constexpr size_t kMiMaxBlock = 256;
rings::Part             g_rings_part;
rings::Patch            g_rings_patch;
rings::PerformanceState g_rings_perf;
uint16_t                g_rings_reverb_buffer[32768];
bool                    g_rings_dsp_ready = false;
float                   g_rings_in[kMiMaxBlock];
float                   g_rings_out_buf[kMiMaxBlock];
float                   g_rings_aux_buf[kMiMaxBlock];
float                   g_plaits_out_l[kMiMaxBlock];
float                   g_plaits_out_r[kMiMaxBlock];
float                   g_rings_strum_phase = 0.0f;
bool                    g_rings_prev_gate = false;

static const char* StreamModeName(StreamMode mode)
{
    switch(mode)
    {
        case StreamMode::LOOP_4IN_4OUT: return "LOOP(4in->4out)";
        case StreamMode::TEST_TONE_1K: return "TONE(1k pulse)";
        case StreamMode::RINGS_TEST_SQUARE: return "RINGS(square)";
        case StreamMode::PLAITS_TEST_SQUARE: return "PLAITS(square)";
        default: return "UNKNOWN";
    }
}

static const char* RunModeName(RunMode mode)
{
    return mode == RunMode::ACTIVE ? "ACTIVE" : "STOP";
}

static const char* UiSceneName(UiScene scene)
{
    switch(scene)
    {
        case UiScene::SCOPE: return "SCOPE";
        case UiScene::RINGS_CTRL: return "RINGS_CTRL";
        case UiScene::PLAITS_CTRL: return "PLAITS_CTRL";
        case UiScene::RINGS_VCV_UI: return "RINGS_VCV_UI";
        case UiScene::PLAITS_VCV_UI: return "PLAITS_VCV_UI";
        case UiScene::VECTOR: return "VECTOR";
        case UiScene::SPECTRUM: return "SPECTRUM";
        case UiScene::NEON: return "NEON";
        case UiScene::ORBIT: return "ORBIT";
        default: return "SCOPE";
    }
}

static const char* RingsAudreyModeName(uint8_t mode)
{
    switch(mode % 3u)
    {
        case 0: return "AUDREY A";
        case 1: return "AUDREY B";
        default: return "AUDREY C";
    }
}

static const char* PlaitsMorphModeName(uint8_t mode)
{
    switch(mode % 5u)
    {
        case 0: return "OCTAVE";
        case 1: return "LPG DECAY";
        case 2: return "LPG COLOR";
        case 3: return "DJ EQ";
        default: return "ABS CV";
    }
}

static float AbsFast(float value)
{
    return value < 0.0f ? -value : value;
}

static float Clampf(float value, float lo, float hi)
{
    if(value < lo)
        return lo;
    if(value > hi)
        return hi;
    return value;
}

static bool DiffEnough(float a, float b, float eps = 0.002f)
{
    return AbsFast(a - b) > eps;
}

static uint16_t U16Min(uint16_t a, uint16_t b)
{
    return (a < b) ? a : b;
}

static float Wrap01(float value)
{
    while(value >= 1.0f)
        value -= 1.0f;
    while(value < 0.0f)
        value += 1.0f;
    return value;
}

static float SawFromPhase(float phase)
{
    return (2.0f * phase) - 1.0f;
}

static float TriFromPhase(float phase)
{
    float distance = phase - 0.5f;
    if(distance < 0.0f)
        distance = -distance;
    return 1.0f - (4.0f * distance);
}

static float SoftClipLocal(float x)
{
    const float ax = AbsFast(x);
    return x / (1.0f + ax);
}

static bool TouchAdjustVSlider(int16_t tx,
                               int16_t ty,
                               int16_t x,
                               int16_t y0,
                               int16_t y1,
                               int16_t w,
                               float&  value)
{
    if(tx < (x - 12) || tx > (x + w + 12) || ty < y0 || ty > y1)
        return false;
    const float norm = 1.0f - (static_cast<float>(ty - y0) / static_cast<float>(y1 - y0));
    value            = Clampf(norm, 0.0f, 1.0f);
    return true;
}

static void DrawVSlider(int16_t      x,
                        int16_t      y0,
                        int16_t      y1,
                        int16_t      w,
                        float        value,
                        const char*  name,
                        uint32_t     fill_color)
{
    const uint16_t h = static_cast<uint16_t>(y1 - y0);
    const int16_t  sy
        = static_cast<int16_t>(y1 - Clampf(value, 0.0f, 1.0f) * static_cast<float>(y1 - y0));

    LCD_SetColor(0xff233043);
    LCD_DrawRect(static_cast<uint16_t>(x - 2), static_cast<uint16_t>(y0 - 2), static_cast<uint16_t>(w + 4), static_cast<uint16_t>(h + 4));
    LCD_SetColor(0xff0D1724);
    LCD_FillRect(static_cast<uint16_t>(x), static_cast<uint16_t>(y0), static_cast<uint16_t>(w), h);
    LCD_SetColor(fill_color);
    LCD_FillRect(static_cast<uint16_t>(x), static_cast<uint16_t>(sy), static_cast<uint16_t>(w), static_cast<uint16_t>(y1 - sy));
    LCD_SetColor(0xffF0F4FF);
    LCD_FillRect(static_cast<uint16_t>(x - 4), static_cast<uint16_t>(sy - 4), static_cast<uint16_t>(w + 8), 8u);

    LCD_SetColor(LCD_WHITE);
    LCD_SetTextFont(&CH_Font16);
    LCD_DisplayString(static_cast<uint16_t>(x - 10), static_cast<uint16_t>(y1 + 10), const_cast<char*>(name));
}

static bool TouchAdjustKnob(int16_t tx, int16_t ty, int16_t cx, int16_t cy, int16_t radius, float& value)
{
    const int32_t dx = static_cast<int32_t>(tx) - static_cast<int32_t>(cx);
    const int32_t dy = static_cast<int32_t>(ty) - static_cast<int32_t>(cy);
    const int32_t rr = static_cast<int32_t>(radius + 14);
    if((dx * dx + dy * dy) > (rr * rr))
        return false;

    constexpr float kStart = -2.35619449f; // -135deg
    constexpr float kEnd   = 2.35619449f;  // +135deg
    const float angle = atan2f(static_cast<float>(cy - ty), static_cast<float>(tx - cx));
    float       norm  = (angle - kStart) / (kEnd - kStart);
    norm              = Clampf(norm, 0.0f, 1.0f);
    value             = norm;
    return true;
}

static void DrawKnob(int16_t cx, int16_t cy, int16_t radius, float value, const char* name, uint32_t color)
{
    constexpr float kStart = -2.35619449f;
    constexpr float kEnd   = 2.35619449f;
    const float theta = kStart + Clampf(value, 0.0f, 1.0f) * (kEnd - kStart);
    const int16_t px  = static_cast<int16_t>(cx + cosf(theta) * (radius - 6));
    const int16_t py  = static_cast<int16_t>(cy - sinf(theta) * (radius - 6));

    LCD_SetColor(0xff223148);
    LCD_DrawCircle(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), static_cast<uint16_t>(radius + 1));
    LCD_SetColor(0xff6A8FB8);
    LCD_DrawCircle(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), static_cast<uint16_t>(radius - 1));
    LCD_SetColor(color);
    LCD_DrawLine(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), static_cast<uint16_t>(px), static_cast<uint16_t>(py));
    LCD_SetColor(0xffEAF2FF);
    LCD_FillCircle(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), 4u);

    if(name == nullptr || name[0] == '\0')
        return;
    LCD_SetColor(LCD_WHITE);
    LCD_SetTextFont(&CH_Font16);
    LCD_DisplayString(static_cast<uint16_t>(cx - radius), static_cast<uint16_t>(cy + radius + 10), const_cast<char*>(name));
}

static void DrawRgb565Image(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t* image)
{
    if(image == nullptr || w == 0 || h == 0)
        return;
    if(x < 0 || y < 0)
        return;
    if((x + static_cast<int16_t>(w)) > static_cast<int16_t>(LCD_Width)
       || (y + static_cast<int16_t>(h)) > static_cast<int16_t>(LCD_Height))
        return;

    uint16_t* framebuffer = reinterpret_cast<uint16_t*>(LCD_MemoryAdd);
    for(uint16_t row = 0; row < h; ++row)
    {
        uint16_t*       dst = framebuffer + (static_cast<uint32_t>(y + row) * LCD_Width + x);
        const uint16_t* src = image + static_cast<uint32_t>(row) * w;
        memcpy(dst, src, static_cast<size_t>(w) * sizeof(uint16_t));
    }
}

static uint16_t Rgb332To565(uint8_t c)
{
    const uint8_t r = static_cast<uint8_t>((c >> 5) & 0x07u);
    const uint8_t g = static_cast<uint8_t>((c >> 2) & 0x07u);
    const uint8_t b = static_cast<uint8_t>(c & 0x03u);
    const uint16_t rr = static_cast<uint16_t>((r * 31u + 3u) / 7u);
    const uint16_t gg = static_cast<uint16_t>((g * 63u + 3u) / 7u);
    const uint16_t bb = static_cast<uint16_t>((b * 31u + 1u) / 3u);
    return static_cast<uint16_t>((rr << 11) | (gg << 5) | bb);
}

static void DrawRgb8Image(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint8_t* image)
{
    if(image == nullptr || w == 0 || h == 0)
        return;
    if(x < 0 || y < 0)
        return;
    if((x + static_cast<int16_t>(w)) > static_cast<int16_t>(LCD_Width)
       || (y + static_cast<int16_t>(h)) > static_cast<int16_t>(LCD_Height))
        return;

    uint16_t* framebuffer = reinterpret_cast<uint16_t*>(LCD_MemoryAdd);
    for(uint16_t row = 0; row < h; ++row)
    {
        uint16_t* dst = framebuffer + (static_cast<uint32_t>(y + row) * LCD_Width + x);
        const uint8_t* src = image + static_cast<uint32_t>(row) * w;
        for(uint16_t col = 0; col < w; ++col)
            dst[col] = Rgb332To565(src[col]);
    }
}

static uint16_t BlendRgb565(uint16_t bg, uint8_t sr, uint8_t sg, uint8_t sb, uint8_t a)
{
    const uint8_t br = static_cast<uint8_t>(((bg >> 11) & 0x1f) * 255 / 31);
    const uint8_t bgc = static_cast<uint8_t>(((bg >> 5) & 0x3f) * 255 / 63);
    const uint8_t bb = static_cast<uint8_t>((bg & 0x1f) * 255 / 31);
    const uint16_t inva = static_cast<uint16_t>(255u - a);
    const uint8_t rr = static_cast<uint8_t>((sr * a + br * inva) / 255u);
    const uint8_t rg = static_cast<uint8_t>((sg * a + bgc * inva) / 255u);
    const uint8_t rb = static_cast<uint8_t>((sb * a + bb * inva) / 255u);
    return static_cast<uint16_t>(((rr * 31u / 255u) << 11) | ((rg * 63u / 255u) << 5)
                                 | (rb * 31u / 255u));
}

static void DrawRotatedRgbaSprite(const uint8_t* rgba,
                                  uint16_t       w,
                                  uint16_t       h,
                                  int16_t        cx,
                                  int16_t        cy,
                                  float          radians)
{
    if(rgba == nullptr || w == 0 || h == 0)
        return;
    uint16_t* framebuffer = reinterpret_cast<uint16_t*>(LCD_MemoryAdd);
    const float c = cosf(radians);
    const float s = sinf(radians);
    const float sx_center = (static_cast<float>(w) - 1.0f) * 0.5f;
    const float sy_center = (static_cast<float>(h) - 1.0f) * 0.5f;
    const int16_t x0 = static_cast<int16_t>(cx - static_cast<int16_t>(w / 2u));
    const int16_t y0 = static_cast<int16_t>(cy - static_cast<int16_t>(h / 2u));

    for(uint16_t dy = 0; dy < h; ++dy)
    {
        const int16_t py = static_cast<int16_t>(y0 + static_cast<int16_t>(dy));
        if(py < 0 || py >= static_cast<int16_t>(LCD_Height))
            continue;
        for(uint16_t dx = 0; dx < w; ++dx)
        {
            const int16_t px = static_cast<int16_t>(x0 + static_cast<int16_t>(dx));
            if(px < 0 || px >= static_cast<int16_t>(LCD_Width))
                continue;

            const float tx = static_cast<float>(dx) - sx_center;
            const float ty = static_cast<float>(dy) - sy_center;
            const float src_xf = tx * c + ty * s + sx_center;
            const float src_yf = -tx * s + ty * c + sy_center;
            const int   src_x  = static_cast<int>(src_xf + 0.5f);
            const int   src_y  = static_cast<int>(src_yf + 0.5f);
            if(src_x < 0 || src_x >= static_cast<int>(w) || src_y < 0
               || src_y >= static_cast<int>(h))
                continue;

            const size_t src_idx = (static_cast<size_t>(src_y) * w + static_cast<size_t>(src_x)) * 4u;
            const uint8_t sr = rgba[src_idx + 0u];
            const uint8_t sg = rgba[src_idx + 1u];
            const uint8_t sb = rgba[src_idx + 2u];
            const uint8_t sa = rgba[src_idx + 3u];
            if(sa == 0u)
                continue;
            uint16_t& dst = framebuffer[static_cast<uint32_t>(py) * LCD_Width
                                        + static_cast<uint32_t>(px)];
            if(sa == 255u)
                dst = static_cast<uint16_t>(((sr * 31u / 255u) << 11) | ((sg * 63u / 255u) << 5)
                                            | (sb * 31u / 255u));
            else
                dst = BlendRgb565(dst, sr, sg, sb, sa);
        }
    }
}

static void VisPushSample(float left, float right)
{
    float d1l = left - g_prev_l1;
    float d2l = d1l - (g_prev_l1 - g_prev_l2);
    float d3l = d2l - ((g_prev_l1 - g_prev_l2) - (g_prev_l2 - g_prev_l3));
    float d1r = right - g_prev_r1;
    float d2r = d1r - (g_prev_r1 - g_prev_r2);
    float d3r = d2r - ((g_prev_r1 - g_prev_r2) - (g_prev_r2 - g_prev_r3));
    float v[8];

    g_vis_l[g_vis_wr] = left;
    g_vis_r[g_vis_wr] = right;
    g_vis_wr          = static_cast<uint16_t>((g_vis_wr + 1u) % kVisHistory);

    v[0] = 0.8f * AbsFast((left + right) * 0.5f);
    v[1] = 1.0f * AbsFast(left - right);
    v[2] = 1.8f * AbsFast(d1l);
    v[3] = 1.8f * AbsFast(d1r);
    v[4] = 3.5f * AbsFast(d2l);
    v[5] = 3.5f * AbsFast(d2r);
    v[6] = 6.0f * AbsFast(d3l);
    v[7] = 6.0f * AbsFast(d3r);

    for(size_t i = 0; i < 8; ++i)
    {
        if(v[i] > 1.2f)
            v[i] = 1.2f;
        g_spec_env[i] += (v[i] - g_spec_env[i]) * 0.10f;
    }

    g_prev_l3 = g_prev_l2;
    g_prev_l2 = g_prev_l1;
    g_prev_l1 = left;
    g_prev_r3 = g_prev_r2;
    g_prev_r2 = g_prev_r1;
    g_prev_r1 = right;
}

static void DrawModeHeader(const char* title, uint32_t bg, uint32_t fg)
{
    LCD_SetBackColor(bg);
    LCD_SetColor(fg);
    LCD_FillRect(0, 0, LCD_Width, 34);
    LCD_SetColor(LCD_WHITE);
    LCD_SetTextFont(&CH_Font24);
    LCD_DisplayString(8, 6, const_cast<char*>(title));
}

static void ClearVisualArea(uint32_t color)
{
    LCD_SetColor(color);
    LCD_FillRect(0, 40, LCD_Width, 330);
}

static void DrawScopeScene()
{
    constexpr int16_t kPanelX = 0;
    constexpr int16_t kPanelY = static_cast<int16_t>((LCD_Height - scope_panel_h) / 2u);
    constexpr int16_t kPanelW = static_cast<int16_t>(scope_panel_w);
    constexpr int16_t kPanelH = static_cast<int16_t>(scope_panel_h);
    LCD_SetColor(0xffE2E2E2);
    LCD_FillRect(0, 0, LCD_Width, LCD_Height);
    DrawRgb8Image(kPanelX, kPanelY, scope_panel_w, scope_panel_h, scope_panel_rgb8);

    constexpr float kOrigWmm = 66.04f;
    constexpr float kOrigHmm = 128.69331f;
    constexpr float kRotWmm  = kOrigHmm;
    constexpr float kRotHmm  = kOrigWmm;
    constexpr float kScaleX  = static_cast<float>(kPanelW) / kRotWmm;
    constexpr float kScaleY  = static_cast<float>(kPanelH) / kRotHmm;
    auto PX = [&](float ox, float oy) -> int16_t {
        const float rx = oy;
        return static_cast<int16_t>(kPanelX + rx * kScaleX + 0.5f);
    };
    auto PY = [&](float ox, float oy) -> int16_t {
        const float ry = kOrigWmm - ox;
        return static_cast<int16_t>(kPanelY + ry * kScaleY + 0.5f);
    };

    const int16_t kDispX = static_cast<int16_t>(kPanelX + static_cast<int16_t>(kPanelW * 0.43f));
    const int16_t kDispY = static_cast<int16_t>(kPanelY + static_cast<int16_t>(kPanelH * 0.07f));
    const int16_t kDispW = static_cast<int16_t>(kPanelW * 0.53f);
    const int16_t kDispH = static_cast<int16_t>(kPanelH * 0.54f);

    constexpr int16_t kKnobR = 14;
    constexpr int16_t kBtnR  = 8;

    const int16_t kLissX   = PX(8.643f, 80.58f);
    const int16_t kLissY   = PY(8.643f, 80.58f);
    const int16_t kTrigX   = PX(57.397f, 80.58f);
    const int16_t kTrigY   = PY(57.397f, 80.58f);
    const int16_t kXScaleX = PX(24.897f, 80.58f);
    const int16_t kXScaleY = PY(24.897f, 80.58f);
    const int16_t kYScaleX = PX(41.147f, 80.58f);
    const int16_t kYScaleY = PY(41.147f, 80.58f);
    const int16_t kTimeX   = PX(8.643f, 96.82f);
    const int16_t kTimeY   = PY(8.643f, 96.82f);
    const int16_t kXPosX   = PX(24.897f, 96.82f);
    const int16_t kXPosY   = PY(24.897f, 96.82f);
    const int16_t kYPosX   = PX(41.147f, 96.82f);
    const int16_t kYPosY   = PY(41.147f, 96.82f);
    const int16_t kThreshX = PX(57.397f, 96.82f);
    const int16_t kThreshY = PY(57.397f, 96.82f);

    auto GainFromNorm = [](float norm) -> float {
        const int step = static_cast<int>(Clampf(floorf(norm * 8.0f + 0.5f), 0.0f, 8.0f));
        return powf(2.0f, static_cast<float>(step)) / 10.0f;
    };
    auto VoltFromNorm = [](float norm) -> float { return (Clampf(norm, 0.0f, 1.0f) * 20.0f) - 10.0f; };
    auto PlotY = [&](float v, float off, float gain) -> int16_t {
        float y = ((v + off) * gain * -0.5f + 0.5f) * static_cast<float>(kDispH - 1);
        y = Clampf(y, 0.0f, static_cast<float>(kDispH - 1));
        return static_cast<int16_t>(kDispY + static_cast<int16_t>(y));
    };
    auto PlotX = [&](float v, float off, float gain) -> int16_t {
        float x = ((v + off) * gain * 0.5f + 0.5f) * static_cast<float>(kDispW - 1);
        x = Clampf(x, 0.0f, static_cast<float>(kDispW - 1));
        return static_cast<int16_t>(kDispX + static_cast<int16_t>(x));
    };

    float xscale_norm = g_scope_x_scale_norm;
    float yscale_norm = g_scope_y_scale_norm;
    const bool touch_active = (touchInfo.flag != 0u) && (touchInfo.num > 0u);
    bool liss_hit = false;
    bool trig_hit = false;
    if(touch_active)
    {
        const int16_t tx = static_cast<int16_t>(touchInfo.x[0]);
        const int16_t ty = static_cast<int16_t>(touchInfo.y[0]);
        if(ty < static_cast<int16_t>(kTouchSwitchYMin))
        {
            TouchAdjustKnob(tx, ty, kXScaleX, kXScaleY, kKnobR, xscale_norm);
            TouchAdjustKnob(tx, ty, kYScaleX, kYScaleY, kKnobR, yscale_norm);
            TouchAdjustKnob(tx, ty, kTimeX, kTimeY, kKnobR, g_scope_time_norm);
            TouchAdjustKnob(tx, ty, kXPosX, kXPosY, kKnobR, g_scope_x_pos_norm);
            TouchAdjustKnob(tx, ty, kYPosX, kYPosY, kKnobR, g_scope_y_pos_norm);
            TouchAdjustKnob(tx, ty, kThreshX, kThreshY, kKnobR, g_scope_thresh_norm);

            const int32_t dlx = static_cast<int32_t>(tx) - static_cast<int32_t>(kLissX);
            const int32_t dly = static_cast<int32_t>(ty) - static_cast<int32_t>(kLissY);
            const int32_t dtx = static_cast<int32_t>(tx) - static_cast<int32_t>(kTrigX);
            const int32_t dty = static_cast<int32_t>(ty) - static_cast<int32_t>(kTrigY);
            liss_hit = (dlx * dlx + dly * dly) <= (kBtnR * kBtnR);
            trig_hit = (dtx * dtx + dty * dty) <= (kBtnR * kBtnR);
        }
    }
    g_scope_x_scale_norm = Clampf(floorf(Clampf(xscale_norm, 0.0f, 1.0f) * 8.0f + 0.5f) / 8.0f, 0.0f, 1.0f);
    g_scope_y_scale_norm = Clampf(floorf(Clampf(yscale_norm, 0.0f, 1.0f) * 8.0f + 0.5f) / 8.0f, 0.0f, 1.0f);
    g_scope_time_norm    = Clampf(g_scope_time_norm, 0.0f, 1.0f);
    g_scope_x_pos_norm   = Clampf(g_scope_x_pos_norm, 0.0f, 1.0f);
    g_scope_y_pos_norm   = Clampf(g_scope_y_pos_norm, 0.0f, 1.0f);
    g_scope_thresh_norm  = Clampf(g_scope_thresh_norm, 0.0f, 1.0f);

    if(liss_hit && !g_scope_liss_latch)
    {
        g_scope_lissajous = !g_scope_lissajous;
        g_scope_liss_latch = true;
    }
    else if(!liss_hit)
    {
        g_scope_liss_latch = false;
    }
    if(trig_hit && !g_scope_trig_latch)
    {
        g_scope_trig_enabled = !g_scope_trig_enabled;
        g_scope_trig_latch = true;
    }
    else if(!trig_hit)
    {
        g_scope_trig_latch = false;
    }

    const float gain_x = GainFromNorm(g_scope_x_scale_norm);
    const float gain_y = GainFromNorm(g_scope_y_scale_norm);
    const float off_x  = VoltFromNorm(g_scope_x_pos_norm);
    const float off_y  = VoltFromNorm(g_scope_y_pos_norm);
    const float thr_v  = VoltFromNorm(g_scope_thresh_norm);
    const uint16_t visible = static_cast<uint16_t>(72u + static_cast<uint16_t>(g_scope_time_norm * 184.0f));
    const uint16_t span = U16Min(static_cast<uint16_t>(visible), kVisHistory);

    LCD_SetColor(0xff000000);
    LCD_FillRect(static_cast<uint16_t>(kDispX),
                 static_cast<uint16_t>(kDispY),
                 static_cast<uint16_t>(kDispW),
                 static_cast<uint16_t>(kDispH));
    LCD_SetColor(0xff2C3A46);
    LCD_DrawRect(kDispX, kDispY, static_cast<uint16_t>(kDispW), static_cast<uint16_t>(kDispH));
    for(uint16_t i = 1; i < 5u; ++i)
    {
        const uint16_t gy = static_cast<uint16_t>(kDispY + (kDispH - 1) * i / 4u);
        LCD_SetColor(0xff2A333C);
        LCD_DrawLine(kDispX, gy, static_cast<uint16_t>(kDispX + kDispW - 1), gy);
    }

    if(g_scope_lissajous)
    {
        int16_t px = kDispX + kDispW / 2;
        int16_t py = kDispY + kDispH / 2;
        LCD_SetColor(0xffE8D34A);
        for(uint16_t i = 0; i < span; ++i)
        {
            const uint16_t idx = static_cast<uint16_t>((g_vis_wr + kVisHistory - span + i) % kVisHistory);
            const float vx = g_vis_l[idx] * 5.0f;
            const float vy = g_vis_r[idx] * 5.0f;
            const int16_t x = PlotX(vx, off_x, gain_x);
            const int16_t y = PlotY(vy, off_y, gain_y);
            if(i > 0u)
                LCD_DrawLine(static_cast<uint16_t>(px), static_cast<uint16_t>(py), static_cast<uint16_t>(x), static_cast<uint16_t>(y));
            px = x;
            py = y;
        }
    }
    else
    {
        int16_t pyx = kDispY + kDispH / 2;
        int16_t pyy = kDispY + kDispH / 2;
        for(uint16_t x = 0u; x < static_cast<uint16_t>(kDispW); ++x)
        {
            const uint16_t si = static_cast<uint16_t>((static_cast<uint32_t>(x) * span) / static_cast<uint16_t>(kDispW));
            const uint16_t idx = static_cast<uint16_t>((g_vis_wr + kVisHistory - span + si) % kVisHistory);
            const float vx = g_vis_l[idx] * 5.0f;
            const float vy = g_vis_r[idx] * 5.0f;
            const int16_t xx = static_cast<int16_t>(kDispX + x);
            const int16_t yx = PlotY(vx, off_x, gain_x);
            const int16_t yy = PlotY(vy, off_y, gain_y);
            if(x > 0u)
            {
                LCD_SetColor(0xffE8D34A);
                LCD_DrawLine(static_cast<uint16_t>(xx - 1), static_cast<uint16_t>(pyx), static_cast<uint16_t>(xx), static_cast<uint16_t>(yx));
                LCD_SetColor(0xff52C7FF);
                LCD_DrawLine(static_cast<uint16_t>(xx - 1), static_cast<uint16_t>(pyy), static_cast<uint16_t>(xx), static_cast<uint16_t>(yy));
            }
            pyx = yx;
            pyy = yy;
        }

        const int16_t ty = PlotY(thr_v, off_x, gain_x);
        LCD_SetColor(g_scope_trig_enabled ? 0xffF0F0F0 : 0xff606060);
        LCD_DrawLine(kDispX, static_cast<uint16_t>(ty), static_cast<uint16_t>(kDispX + kDispW - 1), static_cast<uint16_t>(ty));
        LCD_FillRect(static_cast<uint16_t>(kDispX + kDispW - 12), static_cast<uint16_t>(ty - 4), 10u, 8u);
    }

    DrawKnob(kXScaleX, kXScaleY, kKnobR, g_scope_x_scale_norm, "", 0xffE8D34A);
    DrawKnob(kYScaleX, kYScaleY, kKnobR, g_scope_y_scale_norm, "", 0xff52C7FF);
    DrawKnob(kTimeX, kTimeY, kKnobR, g_scope_time_norm, "", 0xffA9B6C8);
    DrawKnob(kXPosX, kXPosY, kKnobR, g_scope_x_pos_norm, "", 0xffE8D34A);
    DrawKnob(kYPosX, kYPosY, kKnobR, g_scope_y_pos_norm, "", 0xff52C7FF);
    DrawKnob(kThreshX, kThreshY, kKnobR, g_scope_thresh_norm, "", 0xffD5D5D5);

    LCD_SetColor(g_scope_lissajous ? 0xffF0F0F0 : 0xff505050);
    LCD_FillCircle(static_cast<uint16_t>(kLissX), static_cast<uint16_t>(kLissY), 5u);
    LCD_SetColor(g_scope_trig_enabled ? 0xffF0F0F0 : 0xff505050);
    LCD_FillCircle(static_cast<uint16_t>(kTrigX), static_cast<uint16_t>(kTrigY), 5u);
}

static void DrawVectorScene()
{
    int16_t cx = static_cast<int16_t>(LCD_Width / 2u);
    int16_t cy = 210;

    ClearVisualArea(0xff202020);
    LCD_SetColor(0xff4040A0);
    LCD_DrawLine(0, static_cast<uint16_t>(cy), LCD_Width - 1u, static_cast<uint16_t>(cy));
    LCD_DrawLine(static_cast<uint16_t>(cx), 40, static_cast<uint16_t>(cx), 369);

    LCD_SetColor(LCD_CYAN);
    for(uint16_t i = 0u; i < 260u; ++i)
    {
        uint16_t idx = static_cast<uint16_t>((g_vis_wr + kVisHistory - 260u + i) % kVisHistory);
        int16_t  x   = static_cast<int16_t>(cx + (g_vis_l[idx] * 120.0f));
        int16_t  y   = static_cast<int16_t>(cy - (g_vis_r[idx] * 120.0f));
        if(x >= 0 && x < static_cast<int16_t>(LCD_Width) && y >= 40 && y < 370)
            LCD_DrawPoint(static_cast<uint16_t>(x), static_cast<uint16_t>(y), LCD_CYAN);
    }
}

static void DrawSpectrumScene()
{
    static const uint32_t cols[8]
        = {LCD_CYAN, LCD_BLUE, LCD_GREEN, LCD_YELLOW, 0xffFF6A00, LCD_RED, LCD_MAGENTA, LCD_WHITE};
    uint16_t barw = static_cast<uint16_t>(LCD_Width / 8u);
    uint16_t y0   = 44u;
    uint16_t h    = 320u;

    ClearVisualArea(0xff101010);
    for(uint16_t i = 0u; i < 8u; ++i)
    {
        float    v  = g_spec_env[i];
        uint16_t x0 = static_cast<uint16_t>(i * barw + 2u);
        uint16_t bh = 0u;
        uint16_t y1 = 0u;
        uint16_t y2 = 0u;

        if(v > 1.0f)
            v = 1.0f;
        if(v < 0.0f)
            v = 0.0f;

        bh = static_cast<uint16_t>(v * static_cast<float>(h));
        y1 = static_cast<uint16_t>(y0 + h);
        y2 = static_cast<uint16_t>(y1 - bh);

        LCD_SetColor(0xff303050);
        LCD_DrawRect(static_cast<uint16_t>(x0 - 1u), y0, static_cast<uint16_t>(barw - 4u), h);
        if(y2 < y1)
        {
            LCD_SetColor(cols[i]);
            LCD_FillRect(x0, y2, static_cast<uint16_t>(barw - 5u), static_cast<uint16_t>(y1 - y2));
        }
    }
}

static void DrawNeonScene()
{
    uint16_t w     = static_cast<uint16_t>(LCD_Width - 8u);
    uint16_t x0    = 4u;
    uint16_t y_mid = 205u;
    int16_t  p0    = static_cast<int16_t>(y_mid);
    int16_t  p1    = static_cast<int16_t>(y_mid);

    ClearVisualArea(0xff080020);
    w = U16Min(w, kVisHistory);
    for(uint16_t x = 1u; x < w; ++x)
    {
        uint16_t idx = static_cast<uint16_t>((g_vis_wr + kVisHistory - w + x) % kVisHistory);
        float    a   = g_vis_l[idx];
        float    b   = g_vis_r[idx];
        int16_t  y0  = static_cast<int16_t>(y_mid) - static_cast<int16_t>((a + b) * 54.0f);
        int16_t  y1  = static_cast<int16_t>(y_mid) - static_cast<int16_t>((a - b) * 54.0f);
        if(y0 < 42)
            y0 = 42;
        if(y0 > 368)
            y0 = 368;
        if(y1 < 42)
            y1 = 42;
        if(y1 > 368)
            y1 = 368;
        LCD_SetColor(0xff00E5FF);
        LCD_DrawLine(static_cast<uint16_t>(x0 + x - 1u),
                     static_cast<uint16_t>(p0),
                     static_cast<uint16_t>(x0 + x),
                     static_cast<uint16_t>(y0));
        LCD_SetColor(0xffFF4DDB);
        LCD_DrawLine(static_cast<uint16_t>(x0 + x - 1u),
                     static_cast<uint16_t>(p1),
                     static_cast<uint16_t>(x0 + x),
                     static_cast<uint16_t>(y1));
        p0 = y0;
        p1 = y1;
    }
}

static void DrawOrbitScene()
{
    uint16_t cx    = static_cast<uint16_t>(LCD_Width / 2u);
    uint16_t cy    = 205u;
    uint16_t r0    = static_cast<uint16_t>(48u + (g_spec_env[2] * 48.0f));
    uint16_t r1    = static_cast<uint16_t>(92u + (g_spec_env[5] * 64.0f));
    uint16_t pidx  = static_cast<uint16_t>(g_phase_l * static_cast<float>(kVisHistory));
    uint16_t pidx2 = static_cast<uint16_t>(g_phase_r * static_cast<float>(kVisHistory));
    if(pidx >= kVisHistory)
        pidx = static_cast<uint16_t>(kVisHistory - 1u);
    if(pidx2 >= kVisHistory)
        pidx2 = static_cast<uint16_t>(kVisHistory - 1u);

    ClearVisualArea(0xff060A16);
    LCD_SetColor(0xff404860);
    LCD_DrawCircle(cx, cy, 48u);
    LCD_DrawCircle(cx, cy, 96u);
    LCD_DrawCircle(cx, cy, 140u);

    for(uint16_t i = 0u; i < 64u; ++i)
    {
        uint16_t idx = static_cast<uint16_t>((pidx + i) % kVisHistory);
        int16_t  x = static_cast<int16_t>(cx) + static_cast<int16_t>(g_vis_l[idx] * static_cast<float>(r1));
        int16_t  y = static_cast<int16_t>(cy) + static_cast<int16_t>(g_vis_r[idx] * static_cast<float>(r0));
        if(x > 2 && x < static_cast<int16_t>(LCD_Width - 2u) && y > 42 && y < 368)
            LCD_DrawPoint(static_cast<uint16_t>(x), static_cast<uint16_t>(y), 0xff9CC2FF);
    }

    {
        int16_t x0 = static_cast<int16_t>(cx) + static_cast<int16_t>(g_vis_l[pidx] * static_cast<float>(r1));
        int16_t y0 = static_cast<int16_t>(cy) + static_cast<int16_t>(g_vis_r[pidx] * static_cast<float>(r0));
        int16_t x1 = static_cast<int16_t>(cx) + static_cast<int16_t>(g_vis_l[pidx2] * 138.0f);
        int16_t y1 = static_cast<int16_t>(cy) + static_cast<int16_t>(g_vis_r[pidx2] * 138.0f);
        LCD_SetColor(0xffFFB347);
        LCD_FillCircle(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), 4u);
        LCD_SetColor(0xff7DFF7D);
        LCD_FillCircle(static_cast<uint16_t>(x1), static_cast<uint16_t>(y1), 3u);
    }
}

static void DrawTouchCoordDemoScene()
{
    if(!g_touch_demo_layout_ready)
    {
        LCD_SetColor(0xff333333);
        LCD_SetBackColor(0xffB9EDF8);
        LCD_Clear();
        LCD_SetTextFont(&CH_Font24);
        LCD_DisplayString(32, 20, const_cast<char*>("Touch Coord Demo"));
        LCD_DisplayString(32, 58, const_cast<char*>("IIT6 800x480 GT911"));

        LCD_SetTextFont(&CH_Font20);
        LCD_DisplayString(44, 130, const_cast<char*>("X1:       Y1:"));
        LCD_DisplayString(44, 180, const_cast<char*>("X2:       Y2:"));
        LCD_DisplayString(44, 230, const_cast<char*>("X3:       Y3:"));
        LCD_DisplayString(44, 280, const_cast<char*>("X4:       Y4:"));
        LCD_DisplayString(44, 330, const_cast<char*>("X5:       Y5:"));
        LCD_SetColor(LCD_RED);
        g_touch_demo_layout_ready = true;
    }

    for(uint8_t i = 0; i < 5; ++i)
    {
        const uint16_t y = static_cast<uint16_t>(130u + static_cast<uint16_t>(i) * 50u);
        LCD_DisplayNumber(110, y, static_cast<int32_t>(touchInfo.x[i]), 4);
        LCD_DisplayNumber(260, y, static_cast<int32_t>(touchInfo.y[i]), 4);
    }
}

static void DrawTouchPaintScene()
{
    static const uint32_t kPaintColors[] = {
        LCD_CYAN, LCD_YELLOW, LCD_MAGENTA, LCD_GREEN, 0xffFF7A00, LCD_WHITE};

    if(!g_touch_paint_layout_ready)
    {
        LCD_SetColor(0xff101820);
        LCD_SetBackColor(0xff101820);
        LCD_Clear();

        LCD_SetColor(0xff2A2A2A);
        LCD_FillRect(0, 372, LCD_Width, 108);
        LCD_SetColor(0xff4A4A4A);
        LCD_DrawLine(0, 372, LCD_Width - 1u, 372);

        LCD_SetColor(LCD_WHITE);
        LCD_SetTextFont(&CH_Font16);
        LCD_DisplayString(10, 382, const_cast<char*>("Paint: 1-finger draw / 2-finger clear"));
        LCD_DisplayString(10, 408, const_cast<char*>("Hold touch at bottom to switch mode"));
        LCD_DisplayString(10, 434, const_cast<char*>("Pen color changes automatically"));

        g_touch_paint_layout_ready = true;
        g_paint_prev_down          = false;
        g_paint_clear_latched      = false;
    }

    const bool touch_active = (touchInfo.flag != 0u) && (touchInfo.num > 0u);
    if(!touch_active)
    {
        g_paint_prev_down = false;
        g_paint_clear_latched = false;
        return;
    }

    if(touchInfo.num >= 2u)
    {
        if(!g_paint_clear_latched)
        {
            LCD_SetColor(0xff101820);
            LCD_FillRect(0, 34, LCD_Width, 338);
            g_paint_clear_latched = true;
            g_paint_prev_down     = false;
        }
        return;
    }
    g_paint_clear_latched = false;

    uint16_t x = touchInfo.x[0];
    uint16_t y = touchInfo.y[0];
    if(x >= LCD_Width)
        x = static_cast<uint16_t>(LCD_Width - 1u);
    if(y >= kTouchSwitchYMin)
        return;

    LCD_SetColor(kPaintColors[g_paint_color_idx % (sizeof(kPaintColors) / sizeof(kPaintColors[0]))]);
    if(g_paint_prev_down)
    {
        LCD_DrawLine(g_paint_prev_x, g_paint_prev_y, x, y);
    }
    else
    {
        LCD_DrawPoint(x, y, kPaintColors[g_paint_color_idx % (sizeof(kPaintColors) / sizeof(kPaintColors[0]))]);
    }
    g_paint_prev_x = x;
    g_paint_prev_y = y;
    g_paint_prev_down = true;
    g_paint_color_idx++;
}

static float MidiToFreq(float midi_note)
{
    return 440.0f * powf(2.0f, (midi_note - 69.0f) / 12.0f);
}

static void InitMiDsp()
{
    g_rings_part.Init(g_rings_reverb_buffer);
    g_rings_part.set_polyphony(2);
    g_rings_part.set_model(rings::RESONATOR_MODEL_AUDREY_A);
    g_rings_dsp_ready = true;
    PlaitsAdapterInit();
}

static void UpdateRingsDspParams()
{
    g_rings_patch.structure     = Clampf(g_rings_structure_norm, 0.0f, 1.0f);
    g_rings_patch.brightness    = Clampf(g_rings_brightness_norm, 0.0f, 1.0f);
    g_rings_patch.damping       = Clampf(g_rings_damping_norm, 0.0f, 1.0f);
    g_rings_patch.position      = Clampf(g_rings_feedback_norm, 0.0f, 1.0f);
    g_rings_patch.reverb_amount = Clampf(g_rings_reverb_mix_norm, 0.0f, 1.0f);

    g_rings_perf.internal_exciter = true;
    g_rings_perf.internal_strum   = true;
    g_rings_perf.internal_note    = true;
    g_rings_perf.chord            = 0;
    g_rings_perf.fm               = 0.0f;
    g_rings_perf.tonic            = 36.0f;
    g_rings_perf.note             = 24.0f + g_rings_pitch_norm * 36.0f;

    const rings::ResonatorModel md[3] = {rings::RESONATOR_MODEL_AUDREY_A,
                                         rings::RESONATOR_MODEL_AUDREY_B,
                                         rings::RESONATOR_MODEL_AUDREY_C};
    g_rings_part.set_model(md[g_rings_audrey_mode % 3u]);

    if(g_rings_gate && !g_rings_prev_gate)
    {
        g_rings_perf.strum = true;
    }
    else
    {
        g_rings_strum_phase += (g_rings_gate ? 3.0f : 0.6f) / kToneSampleRate;
        if(g_rings_strum_phase >= 1.0f)
        {
            g_rings_strum_phase -= 1.0f;
            g_rings_perf.strum = g_rings_gate;
        }
        else
        {
            g_rings_perf.strum = false;
        }
    }
    g_rings_prev_gate = g_rings_gate;
}


static void DrawToneControlScene()
{
    constexpr int16_t  kKnobCx      = 190;
    constexpr int16_t  kKnobCy      = 220;
    constexpr int16_t  kKnobR       = 82;
    constexpr int16_t  kSliderX     = 520;
    constexpr int16_t  kSliderY0    = 90;
    constexpr int16_t  kSliderY1    = 350;
    constexpr int16_t  kSliderW     = 34;
    constexpr float    kKnobStart   = (5.0f * kTwoPi) / 8.0f; // 225 deg
    constexpr float    kKnobSweep   = (3.0f * kTwoPi) / 4.0f; // 270 deg
    char               line[96];

    if(!g_touch_tone_ctrl_layout_ready)
    {
        LCD_SetColor(0xff10161F);
        LCD_SetBackColor(0xff10161F);
        LCD_Clear();
        LCD_SetTextFont(&CH_Font24);
        LCD_SetColor(LCD_WHITE);
        LCD_DisplayString(18, 12, const_cast<char*>("Touch Tone Control"));
        LCD_SetTextFont(&CH_Font16);
        LCD_DisplayString(18, 46, const_cast<char*>("Knob: pitch (C2..C7)  Slider: volume"));
        LCD_DisplayString(18, 70, const_cast<char*>("Bottom hold: switch mode"));

        LCD_SetColor(0xff2C3E55);
        LCD_DrawCircle(static_cast<uint16_t>(kKnobCx), static_cast<uint16_t>(kKnobCy), static_cast<uint16_t>(kKnobR));
        LCD_DrawCircle(static_cast<uint16_t>(kKnobCx), static_cast<uint16_t>(kKnobCy), static_cast<uint16_t>(kKnobR - 1));
        LCD_SetColor(0xff1D2634);
        LCD_FillCircle(static_cast<uint16_t>(kKnobCx), static_cast<uint16_t>(kKnobCy), static_cast<uint16_t>(kKnobR - 8));

        LCD_SetColor(0xff2C3E55);
        LCD_DrawRect(static_cast<uint16_t>(kSliderX - 2),
                     static_cast<uint16_t>(kSliderY0 - 2),
                     static_cast<uint16_t>(kSliderW + 4),
                     static_cast<uint16_t>(kSliderY1 - kSliderY0 + 4));
        LCD_SetColor(0xff1D2634);
        LCD_FillRect(static_cast<uint16_t>(kSliderX),
                     static_cast<uint16_t>(kSliderY0),
                     static_cast<uint16_t>(kSliderW),
                     static_cast<uint16_t>(kSliderY1 - kSliderY0));
        g_touch_tone_ctrl_layout_ready = true;
    }

    const uint32_t now_ms      = System::GetNow();
    const bool     touch_active = (touchInfo.flag != 0u) && (touchInfo.num > 0u);
    bool           slider_touch = false;
    if(touch_active)
    {
        const int16_t tx = static_cast<int16_t>(touchInfo.x[0]);
        const int16_t ty = static_cast<int16_t>(touchInfo.y[0]);
        if(ty < static_cast<int16_t>(kTouchSwitchYMin))
        {
            const int16_t dx = tx - kKnobCx;
            const int16_t dy = ty - kKnobCy;
            const int32_t d2 = static_cast<int32_t>(dx) * static_cast<int32_t>(dx)
                               + static_cast<int32_t>(dy) * static_cast<int32_t>(dy);
            if(d2 <= static_cast<int32_t>(kKnobR + 18) * static_cast<int32_t>(kKnobR + 18))
            {
                float a = atan2f(static_cast<float>(kKnobCy - ty),
                                 static_cast<float>(tx - kKnobCx)); // [ -pi, pi ]
                if(a < 0.0f)
                    a += kTwoPi;
                float rel = a - kKnobStart;
                if(rel < 0.0f)
                    rel += kTwoPi;
                if(rel > kKnobSweep)
                    rel = kKnobSweep;
                g_tone_ctrl_knob_norm = rel / kKnobSweep;
            }

            if(tx >= (kSliderX - 24) && tx <= (kSliderX + kSliderW + 24)
               && ty >= kSliderY0 && ty <= kSliderY1)
            {
                float n = 1.0f - (static_cast<float>(ty - kSliderY0)
                                  / static_cast<float>(kSliderY1 - kSliderY0));
                g_tone_ctrl_vol_norm = Clampf(n, 0.0f, 1.0f);
                slider_touch          = true;
            }
        }
    }

    if(slider_touch)
    {
        if(!g_slider_touch_prev)
        {
            g_slider_rec_active   = true;
            g_slider_play_active  = false;
            g_slider_rec_len      = 0u;
            g_slider_play_idx     = 0u;
            g_slider_last_step_ms = 0u;
        }

        if(g_slider_rec_active
           && (g_slider_last_step_ms == 0u
               || (now_ms - g_slider_last_step_ms) >= kSliderRecStepMs))
        {
            if(g_slider_rec_len < kSliderRecMaxSamples)
                g_slider_rec_buf[g_slider_rec_len++] = g_tone_ctrl_vol_norm;
            g_slider_last_step_ms = now_ms;

            if(g_slider_rec_len >= kSliderRecMaxSamples)
            {
                g_slider_rec_active   = false;
                g_slider_play_active  = true;
                g_slider_play_idx     = 0u;
                g_slider_last_step_ms = 0u;
            }
        }
    }
    else
    {
        if(g_slider_touch_prev && g_slider_rec_active)
        {
            g_slider_rec_active   = false;
            g_slider_play_active  = g_slider_rec_len > 0u;
            g_slider_play_idx     = 0u;
            g_slider_last_step_ms = 0u;
        }

        if(g_slider_play_active && g_slider_rec_len > 0u
           && (g_slider_last_step_ms == 0u
               || (now_ms - g_slider_last_step_ms) >= kSliderRecStepMs))
        {
            g_tone_ctrl_vol_norm = g_slider_rec_buf[g_slider_play_idx];
            g_slider_play_idx++;
            if(g_slider_play_idx >= g_slider_rec_len)
                g_slider_play_idx = 0u;
            g_slider_last_step_ms = now_ms;
        }
    }
    g_slider_touch_prev = slider_touch;

    g_tone_ctrl_knob_norm = Clampf(g_tone_ctrl_knob_norm, 0.0f, 1.0f);
    g_tone_ctrl_vol_norm  = Clampf(g_tone_ctrl_vol_norm, 0.0f, 1.0f);

    const float midi_note = 36.0f + (g_tone_ctrl_knob_norm * 60.0f); // C2..C7
    const float freq      = MidiToFreq(midi_note);
    g_tone_freq_hz        = freq;
    g_tone_amp            = g_tone_ctrl_vol_norm;
    g_stream_mode         = StreamMode::TEST_TONE_1K;
    g_run_mode            = RunMode::ACTIVE;

    LCD_SetColor(0xff10161F);
    LCD_FillRect(18, 372, 760, 98);
    LCD_SetColor(LCD_WHITE);
    LCD_SetTextFont(&CH_Font16);
    snprintf(line,
             sizeof(line),
             "Freq: %.1f Hz  MIDI: %.1f  Volume: %.2f",
             static_cast<double>(freq),
             static_cast<double>(midi_note),
             static_cast<double>(g_tone_ctrl_vol_norm));
    LCD_DisplayString(20, 384, line);
    snprintf(line,
             sizeof(line),
             "Slider: %s  RecLen: %.2fs / 10.00s",
             g_slider_rec_active ? "REC" : (g_slider_play_active ? "PLAY" : "IDLE"),
             static_cast<double>(static_cast<float>(g_slider_rec_len)
                                 * (static_cast<float>(kSliderRecStepMs) / 1000.0f)));
    LCD_DisplayString(20, 410, line);
    LCD_DisplayString(20, 436, const_cast<char*>("Serial: l(loop) / t(tone) still available"));

    LCD_SetColor(0xff1D2634);
    LCD_FillCircle(static_cast<uint16_t>(kKnobCx), static_cast<uint16_t>(kKnobCy), static_cast<uint16_t>(kKnobR - 12));
    const float  a  = kKnobStart + g_tone_ctrl_knob_norm * kKnobSweep;
    const int16_t px = static_cast<int16_t>(kKnobCx + cosf(a) * static_cast<float>(kKnobR - 18));
    const int16_t py = static_cast<int16_t>(kKnobCy - sinf(a) * static_cast<float>(kKnobR - 18));
    LCD_SetColor(0xff00D4FF);
    LCD_DrawLine(static_cast<uint16_t>(kKnobCx), static_cast<uint16_t>(kKnobCy),
                 static_cast<uint16_t>(px), static_cast<uint16_t>(py));
    LCD_FillCircle(static_cast<uint16_t>(kKnobCx), static_cast<uint16_t>(kKnobCy), 8u);

    const int16_t sy = static_cast<int16_t>(
        kSliderY1 - g_tone_ctrl_vol_norm * static_cast<float>(kSliderY1 - kSliderY0));
    LCD_SetColor(0xff1D2634);
    LCD_FillRect(static_cast<uint16_t>(kSliderX), static_cast<uint16_t>(kSliderY0),
                 static_cast<uint16_t>(kSliderW), static_cast<uint16_t>(kSliderY1 - kSliderY0));
    LCD_SetColor(0xff2ECC71);
    LCD_FillRect(static_cast<uint16_t>(kSliderX),
                 static_cast<uint16_t>(sy),
                 static_cast<uint16_t>(kSliderW),
                 static_cast<uint16_t>(kSliderY1 - sy));
    LCD_SetColor(0xffF5F5F5);
    LCD_FillRect(static_cast<uint16_t>(kSliderX - 6),
                 static_cast<uint16_t>(sy - 5),
                 static_cast<uint16_t>(kSliderW + 12),
                 10u);

}

static void DrawRingsCtrlScene()
{
    constexpr float kScale = 800.0f / 380.0f;
    constexpr int16_t kPanelY = static_cast<int16_t>((LCD_Height - rings_panel_h) / 2);
    auto RX = [](float y) -> int16_t { return static_cast<int16_t>(y * kScale + 0.5f); };
    auto RY = [&](float x) -> int16_t { return static_cast<int16_t>((210.0f - x) * kScale + 0.5f) + kPanelY; };
    constexpr float kRogan3Half = 25.921875f;
    constexpr float kRogan1Half = 19.84180f;

    constexpr int16_t kBtnY    = 40;
    constexpr int16_t kBtnH    = 34;
    constexpr int16_t kGateX   = 8;
    constexpr int16_t kGateW   = 120;
    constexpr int16_t kModeAX  = 138;
    constexpr int16_t kModeW   = 88;
    constexpr int16_t kModeGap = 12;
    constexpr int16_t kPolyX   = 430;
    constexpr int16_t kPolyW   = 80;
    constexpr int16_t kResoX   = 520;
    constexpr int16_t kResoW   = 80;
    constexpr int16_t kKRBig   = 44;
    constexpr int16_t kKRSmall = 33;
    const int16_t     kKFreqX  = RX(72.0f + kRogan3Half);
    const int16_t     kKStruX  = RX(72.0f + kRogan3Half);
    const int16_t     kKBriX   = RX(158.0f + kRogan1Half);
    const int16_t     kKDmpX   = RX(158.0f + kRogan1Half);
    const int16_t     kKPosX   = RX(158.0f + kRogan1Half);
    const int16_t     kKRevX   = RX(229.0f + kRogan1Half);
    const int16_t     kFreqY   = RY(29.0f + kRogan3Half);
    const int16_t     kStruY   = RY(126.0f + kRogan3Half);
    const int16_t     kBriY    = RY(13.0f + kRogan1Half);
    const int16_t     kDmpY    = RY(83.0f + kRogan1Half);
    const int16_t     kPosY    = RY(154.0f + kRogan1Half);
    const int16_t     kRevY    = RY(57.0f + kRogan1Half);
    char              line[192];

    if(!g_touch_tone_ctrl_layout_ready)
    {
        LCD_SetColor(0xff111111);
        LCD_SetBackColor(0xff111111);
        LCD_Clear();
        DrawRgb565Image(0,
                        static_cast<int16_t>((LCD_Height - rings_panel_h) / 2),
                        rings_panel_w,
                        rings_panel_h,
                        rings_panel_rgb565);
        LCD_SetTextFont(&CH_Font16);
        LCD_SetColor(0xff101010);
        LCD_DisplayString(8, 8, const_cast<char*>("RINGS"));
        g_touch_tone_ctrl_layout_ready = true;
    }

    const bool touch_active = (touchInfo.flag != 0u) && (touchInfo.num > 0u);
    bool       gate_hit     = false;
    bool       mode_hit     = false;
    uint8_t    mode_sel     = g_rings_audrey_mode;
    if(touch_active)
    {
        const int16_t tx = static_cast<int16_t>(touchInfo.x[0]);
        const int16_t ty = static_cast<int16_t>(touchInfo.y[0]);
        if(ty < static_cast<int16_t>(kTouchSwitchYMin))
        {
            TouchAdjustKnob(tx, ty, kKFreqX, kFreqY, kKRBig, g_rings_pitch_norm);
            TouchAdjustKnob(tx, ty, kKStruX, kStruY, kKRBig, g_rings_structure_norm);
            TouchAdjustKnob(tx, ty, kKBriX, kBriY, kKRSmall, g_rings_brightness_norm);
            TouchAdjustKnob(tx, ty, kKDmpX, kDmpY, kKRSmall, g_rings_damping_norm);
            TouchAdjustKnob(tx, ty, kKPosX, kPosY, kKRSmall, g_rings_feedback_norm);
            TouchAdjustKnob(tx, ty, kKRevX, kRevY, kKRSmall, g_rings_reverb_mix_norm);

            if(tx >= kGateX && tx <= (kGateX + kGateW) && ty >= kBtnY && ty <= (kBtnY + kBtnH))
                gate_hit = true;

            for(uint8_t i = 0; i < 3u; ++i)
            {
                const int16_t bx = static_cast<int16_t>(kModeAX + i * (kModeW + kModeGap));
                if(tx >= bx && tx <= (bx + kModeW) && ty >= kBtnY && ty <= (kBtnY + kBtnH))
                {
                    mode_hit = true;
                    mode_sel = i;
                }
            }
        }
    }

    if(gate_hit && !g_rings_gate_btn_latch)
    {
        g_rings_gate           = !g_rings_gate;
        g_rings_gate_btn_latch = true;
    }
    else if(!gate_hit)
    {
        g_rings_gate_btn_latch = false;
    }

    if(mode_hit && !g_rings_mode_btn_latch)
    {
        g_rings_audrey_mode    = static_cast<uint8_t>(mode_sel % 3u);
        g_rings_mode_btn_latch = true;
    }
    else if(!mode_hit)
    {
        g_rings_mode_btn_latch = false;
    }

    g_stream_mode = StreamMode::RINGS_TEST_SQUARE;
    g_run_mode    = kUiOnlyVcv ? RunMode::STOP : RunMode::ACTIVE;

    const bool rings_ui_dirty
        = !g_rings_ui_cache.valid || touch_active
          || DiffEnough(g_rings_ui_cache.pitch_norm, g_rings_pitch_norm)
          || DiffEnough(g_rings_ui_cache.structure_norm, g_rings_structure_norm)
          || DiffEnough(g_rings_ui_cache.brightness_norm, g_rings_brightness_norm)
          || DiffEnough(g_rings_ui_cache.damping_norm, g_rings_damping_norm)
          || DiffEnough(g_rings_ui_cache.feedback_norm, g_rings_feedback_norm)
          || DiffEnough(g_rings_ui_cache.reverb_norm, g_rings_reverb_mix_norm)
          || (g_rings_ui_cache.gate != g_rings_gate)
          || (g_rings_ui_cache.mode != g_rings_audrey_mode);
    if(!rings_ui_dirty)
        return;

    DrawRgb565Image(0,
                    static_cast<int16_t>((LCD_Height - rings_panel_h) / 2),
                    rings_panel_w,
                    rings_panel_h,
                    rings_panel_rgb565);
    DrawKnob(kKFreqX, kFreqY, kKRBig, g_rings_pitch_norm, "", 0xff4CC3FF);
    DrawKnob(kKStruX, kStruY, kKRBig, g_rings_structure_norm, "", 0xff4CF6FF);
    DrawKnob(kKBriX, kBriY, kKRSmall, g_rings_brightness_norm, "", 0xffFFD166);
    DrawKnob(kKDmpX, kDmpY, kKRSmall, g_rings_damping_norm, "", 0xff8DEB7A);
    DrawKnob(kKPosX, kPosY, kKRSmall, g_rings_feedback_norm, "", 0xffFF8FA3);
    DrawKnob(kKRevX, kRevY, kKRSmall, g_rings_reverb_mix_norm, "", 0xffC5A3FF);

    LCD_SetColor(g_rings_gate ? 0xff2ECC71 : 0xffAA3344);
    LCD_SetColor(LCD_WHITE);
    LCD_DrawRect(static_cast<uint16_t>(kGateX), static_cast<uint16_t>(kBtnY), static_cast<uint16_t>(kGateW), static_cast<uint16_t>(kBtnH));
    LCD_SetTextFont(&CH_Font16);
    LCD_DisplayString(static_cast<uint16_t>(kGateX + 16), static_cast<uint16_t>(kBtnY + 10),
                      g_rings_gate ? const_cast<char*>("GATE ON") : const_cast<char*>("GATE OFF"));

    for(uint8_t i = 0; i < 3u; ++i)
    {
        const int16_t bx = static_cast<int16_t>(kModeAX + i * (kModeW + kModeGap));
        LCD_SetColor((g_rings_audrey_mode == i) ? 0xff3B6BFF : 0xff1F2B42);
        LCD_SetColor(LCD_WHITE);
        LCD_DrawRect(static_cast<uint16_t>(bx), static_cast<uint16_t>(kBtnY), static_cast<uint16_t>(kModeW), static_cast<uint16_t>(kBtnH));
        LCD_DisplayString(static_cast<uint16_t>(bx + 12), static_cast<uint16_t>(kBtnY + 10),
                          (i == 0u) ? const_cast<char*>("AUD A")
                                     : ((i == 1u) ? const_cast<char*>("AUD B")
                                                   : const_cast<char*>("AUD C")));
    }

    LCD_SetColor(0xff2A3A58);
    LCD_SetColor(LCD_WHITE);
    LCD_DrawRect(static_cast<uint16_t>(kPolyX), static_cast<uint16_t>(kBtnY), static_cast<uint16_t>(kPolyW), static_cast<uint16_t>(kBtnH));
    LCD_DrawRect(static_cast<uint16_t>(kResoX), static_cast<uint16_t>(kBtnY), static_cast<uint16_t>(kResoW), static_cast<uint16_t>(kBtnH));
    LCD_DisplayString(static_cast<uint16_t>(kPolyX + 14), static_cast<uint16_t>(kBtnY + 10), const_cast<char*>("POLY"));
    LCD_DisplayString(static_cast<uint16_t>(kResoX + 14), static_cast<uint16_t>(kBtnY + 10), const_cast<char*>("RESO"));

    const float pitch_note = 30.0f + (g_rings_pitch_norm * 54.0f);
    const float freq       = MidiToFreq(pitch_note);
    LCD_SetColor(0xff101010);
    LCD_SetBackColor(0xffE6E6E6);
    snprintf(line,
             sizeof(line),
             "Mode=%s  Freq=%.1fHz  Body=%.2f Nerv=%.2f Decay=%.2f Fb=%.2f Rev=%.2f",
             RingsAudreyModeName(g_rings_audrey_mode),
             static_cast<double>(freq),
             static_cast<double>(g_rings_structure_norm),
             static_cast<double>(g_rings_brightness_norm),
             static_cast<double>(g_rings_damping_norm),
             static_cast<double>(g_rings_feedback_norm),
             static_cast<double>(g_rings_reverb_mix_norm));
    LCD_DisplayString(8, 458, line);

    g_rings_ui_cache.valid           = true;
    g_rings_ui_cache.pitch_norm      = g_rings_pitch_norm;
    g_rings_ui_cache.structure_norm  = g_rings_structure_norm;
    g_rings_ui_cache.brightness_norm = g_rings_brightness_norm;
    g_rings_ui_cache.damping_norm    = g_rings_damping_norm;
    g_rings_ui_cache.feedback_norm   = g_rings_feedback_norm;
    g_rings_ui_cache.reverb_norm     = g_rings_reverb_mix_norm;
    g_rings_ui_cache.gate            = g_rings_gate;
    g_rings_ui_cache.mode            = g_rings_audrey_mode;
}

static void DrawPlaitsCtrlScene()
{
    constexpr float kScale = 800.0f / 128.69331f;
    constexpr int16_t kPanelY = static_cast<int16_t>((LCD_Height - plaits_panel_h) / 2);
    auto PX = [](float y_mm) -> int16_t { return static_cast<int16_t>(y_mm * kScale + 0.5f); };
    auto PY = [&](float x_mm) -> int16_t { return static_cast<int16_t>((60.959999f - x_mm) * kScale + 0.5f) + kPanelY; };
    constexpr float kRogan3HalfMm = 8.7800f;
    constexpr float kRogan1HalfMm = 6.7200f;

    constexpr int16_t kBtnY    = 40;
    constexpr int16_t kBtnH    = 34;
    constexpr int16_t kGateX   = 8;
    constexpr int16_t kGateW   = 118;
    constexpr int16_t kModeBtnX = 136;
    constexpr int16_t kModeBtnW = 170;
    constexpr int16_t kMorphBtnX = 316;
    constexpr int16_t kMorphBtnW = 170;
    constexpr int16_t kModelPlusX = 496;
    constexpr int16_t kModelW = 80;
    constexpr int16_t kKRBig   = 44;
    constexpr int16_t kKRSmall = 33;
    const int16_t     kKFreqX  = PX(20.21088f + kRogan3HalfMm);
    const int16_t     kKHarmX  = PX(20.21088f + kRogan3HalfMm);
    const int16_t     kKTimX   = PX(49.6562f + kRogan1HalfMm);
    const int16_t     kKMorX   = PX(49.6562f + kRogan1HalfMm);
    const int16_t     kKModelX = PX(14.6539f + kRogan1HalfMm);
    const int16_t     kFreqY   = PY(3.1577f + kRogan3HalfMm);
    const int16_t     kHarmY   = PY(39.3327f + kRogan3HalfMm);
    const int16_t     kTimY    = PY(4.04171f + kRogan1HalfMm);
    const int16_t     kMorY    = PY(42.71716f + kRogan1HalfMm);
    const int16_t     kModelY  = PY(27.7f + kRogan1HalfMm);
    char              line[192];

    if(!g_touch_tone_ctrl_layout_ready)
    {
        LCD_SetColor(0xff111111);
        LCD_SetBackColor(0xff111111);
        LCD_Clear();
        DrawRgb565Image(0,
                        static_cast<int16_t>((LCD_Height - plaits_panel_h) / 2),
                        plaits_panel_w,
                        plaits_panel_h,
                        plaits_panel_rgb565);
        LCD_SetTextFont(&CH_Font16);
        LCD_SetColor(0xff101010);
        LCD_DisplayString(8, 8, const_cast<char*>("PLAITS"));
        g_touch_tone_ctrl_layout_ready = true;
    }

    g_plaits_model_index = static_cast<uint8_t>(Clampf(g_plaits_model_norm, 0.0f, 1.0f) * 23.0f + 0.5f);

    const bool touch_active = (touchInfo.flag != 0u) && (touchInfo.num > 0u);
    bool       gate_hit     = false;
    bool       model_mode_hit = false;
    bool       morph_mode_hit = false;
    bool       model_minus_hit = false;
    bool       model_plus_hit  = false;
    if(touch_active)
    {
        const int16_t tx = static_cast<int16_t>(touchInfo.x[0]);
        const int16_t ty = static_cast<int16_t>(touchInfo.y[0]);
        if(ty < static_cast<int16_t>(kTouchSwitchYMin))
        {
            TouchAdjustKnob(tx, ty, kKFreqX, kFreqY, kKRBig, g_plaits_pitch_norm);
            TouchAdjustKnob(tx, ty, kKHarmX, kHarmY, kKRBig, g_plaits_harmonics_norm);
            TouchAdjustKnob(tx, ty, kKTimX, kTimY, kKRSmall, g_plaits_timbre_norm);
            TouchAdjustKnob(tx, ty, kKMorX, kMorY, kKRSmall, g_plaits_morph_norm);
            TouchAdjustKnob(tx, ty, kKModelX, kModelY, kKRSmall, g_plaits_model_norm);

            if(tx >= kGateX && tx <= (kGateX + kGateW) && ty >= kBtnY && ty <= (kBtnY + kBtnH))
                gate_hit = true;
            if(tx >= kModeBtnX && tx <= (kModeBtnX + kModeBtnW) && ty >= kBtnY && ty <= (kBtnY + kBtnH))
                model_mode_hit = true;
            if(tx >= kMorphBtnX && tx <= (kMorphBtnX + kMorphBtnW) && ty >= kBtnY && ty <= (kBtnY + kBtnH))
                morph_mode_hit = true;
            if(tx >= kModelPlusX && tx <= (kModelPlusX + kModelW) && ty >= kBtnY && ty <= (kBtnY + kBtnH))
                model_plus_hit = true;
            if(tx >= static_cast<int16_t>(kModelPlusX + kModelW + 12)
               && tx <= static_cast<int16_t>(kModelPlusX + 2 * kModelW + 12)
               && ty >= kBtnY && ty <= (kBtnY + kBtnH))
                model_minus_hit = true;
        }
    }

    if(gate_hit && !g_plaits_gate_btn_latch)
    {
        g_plaits_gate           = !g_plaits_gate;
        g_plaits_gate_btn_latch = true;
    }
    else if(!gate_hit)
    {
        g_plaits_gate_btn_latch = false;
    }

    if(model_mode_hit && !g_plaits_model_btn_latch)
    {
        g_plaits_model_index = static_cast<uint8_t>((g_plaits_model_index + 1u) % 24u);
        g_plaits_model_norm = static_cast<float>(g_plaits_model_index) / 23.0f;
        g_plaits_model_btn_latch = true;
    }
    else if(!model_mode_hit)
    {
        g_plaits_model_btn_latch = false;
    }

    if(morph_mode_hit && !g_plaits_morph_btn_latch)
    {
        g_plaits_morph_mode = static_cast<uint8_t>((g_plaits_morph_mode + 1u) % 5u);
        g_plaits_morph_btn_latch = true;
    }
    else if(!morph_mode_hit)
    {
        g_plaits_morph_btn_latch = false;
    }

    if(model_plus_hit)
    {
        g_plaits_model_index = static_cast<uint8_t>((g_plaits_model_index + 1u) % 24u);
        g_plaits_model_norm = static_cast<float>(g_plaits_model_index) / 23.0f;
    }
    if(model_minus_hit)
    {
        g_plaits_model_index = static_cast<uint8_t>((g_plaits_model_index + 23u) % 24u);
        g_plaits_model_norm = static_cast<float>(g_plaits_model_index) / 23.0f;
    }

    g_stream_mode = StreamMode::PLAITS_TEST_SQUARE;
    g_run_mode    = kUiOnlyVcv ? RunMode::STOP : RunMode::ACTIVE;

    const bool plaits_ui_dirty
        = !g_plaits_ui_cache.valid || touch_active
          || DiffEnough(g_plaits_ui_cache.pitch_norm, g_plaits_pitch_norm)
          || DiffEnough(g_plaits_ui_cache.timbre_norm, g_plaits_timbre_norm)
          || DiffEnough(g_plaits_ui_cache.morph_norm, g_plaits_morph_norm)
          || DiffEnough(g_plaits_ui_cache.harmonics_norm, g_plaits_harmonics_norm)
          || DiffEnough(g_plaits_ui_cache.model_norm, g_plaits_model_norm)
          || (g_plaits_ui_cache.gate != g_plaits_gate)
          || (g_plaits_ui_cache.model_index != g_plaits_model_index)
          || (g_plaits_ui_cache.morph_mode != g_plaits_morph_mode);
    if(!plaits_ui_dirty)
        return;

    DrawRgb565Image(0,
                    static_cast<int16_t>((LCD_Height - plaits_panel_h) / 2),
                    plaits_panel_w,
                    plaits_panel_h,
                    plaits_panel_rgb565);
    DrawKnob(kKFreqX, kFreqY, kKRBig, g_plaits_pitch_norm, "", 0xffFF9AD5);
    DrawKnob(kKHarmX, kHarmY, kKRBig, g_plaits_harmonics_norm, "", 0xffFFD166);
    DrawKnob(kKTimX, kTimY, kKRSmall, g_plaits_timbre_norm, "", 0xffB38BFF);
    DrawKnob(kKMorX, kMorY, kKRSmall, g_plaits_morph_norm, "", 0xff6CDBFF);
    DrawKnob(kKModelX, kModelY, kKRSmall, g_plaits_model_norm, "", 0xff7EF29A);

    LCD_SetColor(g_plaits_gate ? 0xffAA66FF : 0xff59306D);
    LCD_SetColor(LCD_WHITE);
    LCD_DrawRect(static_cast<uint16_t>(kGateX), static_cast<uint16_t>(kBtnY), static_cast<uint16_t>(kGateW), static_cast<uint16_t>(kBtnH));
    LCD_DisplayString(static_cast<uint16_t>(kGateX + 14), static_cast<uint16_t>(kBtnY + 10),
                      g_plaits_gate ? const_cast<char*>("GATE ON") : const_cast<char*>("GATE OFF"));

    LCD_SetColor(0xff2C2444);
    LCD_SetColor(LCD_WHITE);
    LCD_DrawRect(static_cast<uint16_t>(kModeBtnX), static_cast<uint16_t>(kBtnY), static_cast<uint16_t>(kModeBtnW), static_cast<uint16_t>(kBtnH));
    snprintf(line, sizeof(line), "MODEL NEXT (%02u)", static_cast<unsigned>(g_plaits_model_index));
    LCD_DisplayString(static_cast<uint16_t>(kModeBtnX + 10), static_cast<uint16_t>(kBtnY + 10), line);

    LCD_SetColor(0xff2F234C);
    LCD_SetColor(LCD_WHITE);
    LCD_DrawRect(static_cast<uint16_t>(kMorphBtnX), static_cast<uint16_t>(kBtnY), static_cast<uint16_t>(kMorphBtnW), static_cast<uint16_t>(kBtnH));
    snprintf(line, sizeof(line), "MORPH MODE: %s", PlaitsMorphModeName(g_plaits_morph_mode));
    LCD_DisplayString(static_cast<uint16_t>(kMorphBtnX + 10), static_cast<uint16_t>(kBtnY + 10), line);

    LCD_SetColor(0xff173349);
    LCD_SetColor(LCD_WHITE);
    LCD_DrawRect(static_cast<uint16_t>(kModelPlusX), static_cast<uint16_t>(kBtnY), static_cast<uint16_t>(kModelW), static_cast<uint16_t>(kBtnH));
    LCD_DrawRect(static_cast<uint16_t>(kModelPlusX + kModelW + 12), static_cast<uint16_t>(kBtnY), static_cast<uint16_t>(kModelW), static_cast<uint16_t>(kBtnH));
    LCD_DisplayString(static_cast<uint16_t>(kModelPlusX + 36), static_cast<uint16_t>(kBtnY + 10), const_cast<char*>("+"));
    LCD_DisplayString(static_cast<uint16_t>(kModelPlusX + kModelW + 46), static_cast<uint16_t>(kBtnY + 10), const_cast<char*>("-"));

    const float pitch_note = 36.0f + (g_plaits_pitch_norm * 60.0f);
    const float freq       = MidiToFreq(pitch_note);
    LCD_SetColor(0xff101010);
    LCD_SetBackColor(0xffE6E6E6);
    snprintf(line,
             sizeof(line),
             "Model=%u  Freq=%.1fHz  Timbre=%.2f Morph=%.2f Harm=%.2f  MorphMode=%s",
             static_cast<unsigned>(g_plaits_model_index),
             static_cast<double>(freq),
             static_cast<double>(g_plaits_timbre_norm),
             static_cast<double>(g_plaits_morph_norm),
             static_cast<double>(g_plaits_harmonics_norm),
             PlaitsMorphModeName(g_plaits_morph_mode));
    LCD_DisplayString(8, 458, line);

    g_plaits_ui_cache.valid          = true;
    g_plaits_ui_cache.pitch_norm     = g_plaits_pitch_norm;
    g_plaits_ui_cache.timbre_norm    = g_plaits_timbre_norm;
    g_plaits_ui_cache.morph_norm     = g_plaits_morph_norm;
    g_plaits_ui_cache.harmonics_norm = g_plaits_harmonics_norm;
    g_plaits_ui_cache.model_norm     = g_plaits_model_norm;
    g_plaits_ui_cache.gate           = g_plaits_gate;
    g_plaits_ui_cache.model_index    = g_plaits_model_index;
    g_plaits_ui_cache.morph_mode     = g_plaits_morph_mode;
}

static void DrawRingsVcvUiScene()
{
    g_run_mode = RunMode::STOP;
    constexpr float kScale = 800.0f / 380.0f;
    constexpr int16_t kPanelY = static_cast<int16_t>((LCD_Height - rings_panel_h) / 2u);
    auto RX = [](float y_mm) -> int16_t { return static_cast<int16_t>(y_mm * kScale + 0.5f); };
    auto RY = [&](float x_mm) -> int16_t { return static_cast<int16_t>((210.0f - x_mm) * kScale + 0.5f) + kPanelY; };
    constexpr float kRogan3Half = 25.921875f;
    constexpr float kRogan1Half = 19.84180f;
    constexpr int16_t kKRBig = 44;
    constexpr int16_t kKRSmall = 34;
    const int16_t     kKFreqX  = RX(72.0f + kRogan3Half);
    const int16_t     kKStruX  = RX(72.0f + kRogan3Half);
    const int16_t     kKBriX   = RX(158.0f + kRogan1Half);
    const int16_t     kKDmpX   = RX(158.0f + kRogan1Half);
    const int16_t     kKPosX   = RX(158.0f + kRogan1Half);
    const int16_t     kFreqY   = RY(29.0f + kRogan3Half);
    const int16_t     kStruY   = RY(126.0f + kRogan3Half);
    const int16_t     kBriY    = RY(13.0f + kRogan1Half);
    const int16_t     kDmpY    = RY(83.0f + kRogan1Half);
    const int16_t     kPosY    = RY(154.0f + kRogan1Half);
    const int16_t     kPolyX   = RX(244.5f);
    const int16_t     kPolyY   = RY(175.0f);
    const int16_t     kResoX   = RX(244.5f);
    const int16_t     kResoY   = RY(140.0f);
    constexpr int16_t kBtnR    = 18;
    constexpr float   kStart   = -2.35619449f;
    constexpr float   kEnd     = 2.35619449f;
    char              line[160];

    const bool touch_active = (touchInfo.flag != 0u) && (touchInfo.num > 0u);
    bool       poly_hit     = false;
    bool       reso_hit     = false;
    if(touch_active)
    {
        const int16_t tx = static_cast<int16_t>(touchInfo.x[0]);
        const int16_t ty = static_cast<int16_t>(touchInfo.y[0]);
        if(ty < static_cast<int16_t>(kTouchSwitchYMin))
        {
            TouchAdjustKnob(tx, ty, kKFreqX, kFreqY, kKRBig, g_rings_pitch_norm);
            TouchAdjustKnob(tx, ty, kKStruX, kStruY, kKRBig, g_rings_structure_norm);
            TouchAdjustKnob(tx, ty, kKBriX, kBriY, kKRSmall, g_rings_brightness_norm);
            TouchAdjustKnob(tx, ty, kKDmpX, kDmpY, kKRSmall, g_rings_damping_norm);
            TouchAdjustKnob(tx, ty, kKPosX, kPosY, kKRSmall, g_rings_feedback_norm);
            const int32_t pdx = static_cast<int32_t>(tx) - static_cast<int32_t>(kPolyX);
            const int32_t pdy = static_cast<int32_t>(ty) - static_cast<int32_t>(kPolyY);
            const int32_t rdx = static_cast<int32_t>(tx) - static_cast<int32_t>(kResoX);
            const int32_t rdy = static_cast<int32_t>(ty) - static_cast<int32_t>(kResoY);
            poly_hit = (pdx * pdx + pdy * pdy) <= (kBtnR * kBtnR);
            reso_hit = (rdx * rdx + rdy * rdy) <= (kBtnR * kBtnR);
        }
    }
    if(poly_hit && !g_rings_mode_btn_latch)
    {
        g_rings_audrey_mode = static_cast<uint8_t>((g_rings_audrey_mode + 1u) % 3u);
        g_rings_mode_btn_latch = true;
    }
    else if(!poly_hit)
    {
        g_rings_mode_btn_latch = false;
    }
    if(reso_hit && !g_rings_gate_btn_latch)
    {
        g_rings_gate = !g_rings_gate;
        g_rings_gate_btn_latch = true;
    }
    else if(!reso_hit)
    {
        g_rings_gate_btn_latch = false;
    }

    DrawRgb565Image(0,
                    static_cast<int16_t>((LCD_Height - rings_panel_h) / 2),
                    rings_panel_w,
                    rings_panel_h,
                    rings_panel_rgb565);
    const float freq_angle = kStart + Clampf(g_rings_pitch_norm, 0.0f, 1.0f) * (kEnd - kStart);
    const float stru_angle = kStart + Clampf(g_rings_structure_norm, 0.0f, 1.0f) * (kEnd - kStart);
    const float bri_angle = kStart + Clampf(g_rings_brightness_norm, 0.0f, 1.0f) * (kEnd - kStart);
    const float dmp_angle = kStart + Clampf(g_rings_damping_norm, 0.0f, 1.0f) * (kEnd - kStart);
    const float pos_angle = kStart + Clampf(g_rings_feedback_norm, 0.0f, 1.0f) * (kEnd - kStart);

    DrawRotatedRgbaSprite(vcv_knob_big_rgba, vcv_knob_big_w, vcv_knob_big_h, kKFreqX, kFreqY, freq_angle);
    DrawRotatedRgbaSprite(vcv_knob_big_rgba, vcv_knob_big_w, vcv_knob_big_h, kKStruX, kStruY, stru_angle);
    DrawRotatedRgbaSprite(vcv_knob_small_rgba, vcv_knob_small_w, vcv_knob_small_h, kKBriX, kBriY, bri_angle);
    DrawRotatedRgbaSprite(vcv_knob_small_rgba, vcv_knob_small_w, vcv_knob_small_h, kKDmpX, kDmpY, dmp_angle);
    DrawRotatedRgbaSprite(vcv_knob_small_rgba, vcv_knob_small_w, vcv_knob_small_h, kKPosX, kPosY, pos_angle);

    LCD_SetColor(g_rings_audrey_mode != 0u ? 0xffF2F2F2 : 0xff606060);
    LCD_FillCircle(static_cast<uint16_t>(kPolyX), static_cast<uint16_t>(kPolyY), 5u);
    LCD_SetColor(g_rings_gate ? 0xffF2F2F2 : 0xff606060);
    LCD_FillCircle(static_cast<uint16_t>(kResoX), static_cast<uint16_t>(kResoY), 5u);

    LCD_SetColor(0xff101010);
    LCD_SetBackColor(0xffE6E6E6);
    snprintf(line, sizeof(line), "RINGS_VCV_UI  F=%.2f S=%.2f B=%.2f D=%.2f P=%.2f",
             static_cast<double>(g_rings_pitch_norm),
             static_cast<double>(g_rings_structure_norm),
             static_cast<double>(g_rings_brightness_norm),
             static_cast<double>(g_rings_damping_norm),
             static_cast<double>(g_rings_feedback_norm));
    LCD_DisplayString(8, 458, line);
}

static void DrawPlaitsVcvUiScene()
{
    g_run_mode = RunMode::STOP;
    constexpr float kScale = 800.0f / 128.69331f;
    constexpr int16_t kPanelY = static_cast<int16_t>((LCD_Height - plaits_panel_h) / 2u);
    auto PX = [](float y_mm) -> int16_t { return static_cast<int16_t>(y_mm * kScale + 0.5f); };
    auto PY = [&](float x_mm) -> int16_t { return static_cast<int16_t>((60.959999f - x_mm) * kScale + 0.5f) + kPanelY; };
    constexpr float kRogan3HalfMm = 8.7800f;
    constexpr float kRogan1HalfMm = 6.7200f;
    constexpr int16_t kKRBig = 44;
    constexpr int16_t kKRSmall = 34;
    const int16_t     kKFreqX  = PX(20.21088f + kRogan3HalfMm);
    const int16_t     kKHarmX  = PX(20.21088f + kRogan3HalfMm);
    const int16_t     kKTimX   = PX(49.6562f + kRogan1HalfMm);
    const int16_t     kKMorX   = PX(49.6562f + kRogan1HalfMm);
    const int16_t     kFreqY   = PY(3.1577f + kRogan3HalfMm);
    const int16_t     kHarmY   = PY(39.3327f + kRogan3HalfMm);
    const int16_t     kTimY    = PY(4.04171f + kRogan1HalfMm);
    const int16_t     kMorY    = PY(42.71716f + kRogan1HalfMm);
    const int16_t     kModelBtn1X = PX(15.8f);
    const int16_t     kModelBtn2X = PX(22.0f);
    const int16_t     kModelBtnY  = PY(27.5f);
    constexpr int16_t kModelBtnR = 11;
    constexpr float   kStart = -2.35619449f;
    constexpr float   kEnd   = 2.35619449f;
    char              line[160];

    g_plaits_model_index = static_cast<uint8_t>(Clampf(g_plaits_model_norm, 0.0f, 1.0f) * 23.0f + 0.5f);
    const bool touch_active = (touchInfo.flag != 0u) && (touchInfo.num > 0u);
    bool       model_up_hit = false;
    bool       model_dn_hit = false;
    if(touch_active)
    {
        const int16_t tx = static_cast<int16_t>(touchInfo.x[0]);
        const int16_t ty = static_cast<int16_t>(touchInfo.y[0]);
        if(ty < static_cast<int16_t>(kTouchSwitchYMin))
        {
            TouchAdjustKnob(tx, ty, kKFreqX, kFreqY, kKRBig, g_plaits_pitch_norm);
            TouchAdjustKnob(tx, ty, kKHarmX, kHarmY, kKRBig, g_plaits_harmonics_norm);
            TouchAdjustKnob(tx, ty, kKTimX, kTimY, kKRSmall, g_plaits_timbre_norm);
            TouchAdjustKnob(tx, ty, kKMorX, kMorY, kKRSmall, g_plaits_morph_norm);
            const int32_t dx1 = static_cast<int32_t>(tx) - static_cast<int32_t>(kModelBtn1X);
            const int32_t dy1 = static_cast<int32_t>(ty) - static_cast<int32_t>(kModelBtnY);
            const int32_t dx2 = static_cast<int32_t>(tx) - static_cast<int32_t>(kModelBtn2X);
            const int32_t dy2 = static_cast<int32_t>(ty) - static_cast<int32_t>(kModelBtnY);
            model_up_hit = (dx1 * dx1 + dy1 * dy1) <= (kModelBtnR * kModelBtnR);
            model_dn_hit = (dx2 * dx2 + dy2 * dy2) <= (kModelBtnR * kModelBtnR);
        }
    }
    if(model_up_hit && !g_plaits_model_btn_latch)
    {
        g_plaits_model_index = static_cast<uint8_t>((g_plaits_model_index + 1u) % 24u);
        g_plaits_model_norm = static_cast<float>(g_plaits_model_index) / 23.0f;
        g_plaits_model_btn_latch = true;
    }
    else if(model_dn_hit && !g_plaits_model_btn_latch)
    {
        g_plaits_model_index = static_cast<uint8_t>((g_plaits_model_index + 23u) % 24u);
        g_plaits_model_norm = static_cast<float>(g_plaits_model_index) / 23.0f;
        g_plaits_model_btn_latch = true;
    }
    else if(!model_up_hit && !model_dn_hit)
    {
        g_plaits_model_btn_latch = false;
    }

    DrawRgb565Image(0,
                    static_cast<int16_t>((LCD_Height - plaits_panel_h) / 2),
                    plaits_panel_w,
                    plaits_panel_h,
                    plaits_panel_rgb565);
    const float freq_angle = kStart + Clampf(g_plaits_pitch_norm, 0.0f, 1.0f) * (kEnd - kStart);
    const float harm_angle = kStart + Clampf(g_plaits_harmonics_norm, 0.0f, 1.0f) * (kEnd - kStart);
    const float tim_angle  = kStart + Clampf(g_plaits_timbre_norm, 0.0f, 1.0f) * (kEnd - kStart);
    const float mor_angle  = kStart + Clampf(g_plaits_morph_norm, 0.0f, 1.0f) * (kEnd - kStart);

    DrawRotatedRgbaSprite(vcv_knob_big_rgba, vcv_knob_big_w, vcv_knob_big_h, kKFreqX, kFreqY, freq_angle);
    DrawRotatedRgbaSprite(vcv_knob_big_rgba, vcv_knob_big_w, vcv_knob_big_h, kKHarmX, kHarmY, harm_angle);
    DrawRotatedRgbaSprite(vcv_knob_small_rgba, vcv_knob_small_w, vcv_knob_small_h, kKTimX, kTimY, tim_angle);
    DrawRotatedRgbaSprite(vcv_knob_small_rgba, vcv_knob_small_w, vcv_knob_small_h, kKMorX, kMorY, mor_angle);

    LCD_SetColor(model_up_hit ? 0xffF2F2F2 : 0xff6A6A6A);
    LCD_FillCircle(static_cast<uint16_t>(kModelBtn1X), static_cast<uint16_t>(kModelBtnY), 5u);
    LCD_SetColor(model_dn_hit ? 0xffF2F2F2 : 0xff6A6A6A);
    LCD_FillCircle(static_cast<uint16_t>(kModelBtn2X), static_cast<uint16_t>(kModelBtnY), 5u);

    LCD_SetColor(0xff101010);
    LCD_SetBackColor(0xffE6E6E6);
    snprintf(line, sizeof(line), "PLAITS_VCV_UI  Model=%u P=%.2f H=%.2f T=%.2f M=%.2f",
             static_cast<unsigned>(g_plaits_model_index),
             static_cast<double>(g_plaits_pitch_norm),
             static_cast<double>(g_plaits_harmonics_norm),
             static_cast<double>(g_plaits_timbre_norm),
             static_cast<double>(g_plaits_morph_norm));
    LCD_DisplayString(8, 458, line);
}

static void SceneApplyStyle()
{
    g_touch_demo_layout_ready = false;
    g_touch_paint_layout_ready = false;
    g_touch_tone_ctrl_layout_ready = false;
    g_rings_ui_cache.valid = false;
    g_plaits_ui_cache.valid = false;
    memset(g_vis_l, 0, sizeof(g_vis_l));
    memset(g_vis_r, 0, sizeof(g_vis_r));
    g_vis_wr = 0;
    LCD_SetColor(LCD_BLACK);
    LCD_Clear();
    switch(g_ui_scene)
    {
        case UiScene::SCOPE: break;
        case UiScene::VECTOR: DrawModeHeader("TLQ VECTORSCOPE", LCD_BLACK, LCD_BLUE); break;
        case UiScene::SPECTRUM: DrawModeHeader("TLQ SPECTRUM", LCD_BLACK, LCD_CYAN); break;
        case UiScene::NEON: DrawModeHeader("TLQ NEON WAVE", LCD_BLACK, 0xffB060FF); break;
        case UiScene::ORBIT: DrawModeHeader("TLQ ORBIT", LCD_BLACK, 0xff5FC7FF); break;
        case UiScene::RINGS_CTRL: break;
        case UiScene::PLAITS_CTRL: break;
        case UiScene::RINGS_VCV_UI: break;
        case UiScene::PLAITS_VCV_UI: break;
        default: break;
    }
}

static void DrawTouchOverlay()
{
    char line[128];
    LCD_SetColor(0xff101010);
    LCD_FillRect(0, 372, LCD_Width, 108);
    LCD_SetColor(0xff404040);
    LCD_DrawRect(8, 376, LCD_Width - 16, 96);

    LCD_SetColor(LCD_WHITE);
    LCD_SetTextFont(&CH_Font16);
    snprintf(line, sizeof(line), "Mode:%s  TouchNum:%u",
             UiSceneName(g_ui_scene),
             static_cast<unsigned>(touchInfo.num));
    LCD_DisplayString(14, 386, line);

    snprintf(line,
             sizeof(line),
             "X0:%u Y0:%u  X1:%u Y1:%u",
             static_cast<unsigned>(touchInfo.x[0]),
             static_cast<unsigned>(touchInfo.y[0]),
             static_cast<unsigned>(touchInfo.x[1]),
             static_cast<unsigned>(touchInfo.y[1]));
    LCD_DisplayString(14, 412, line);

    LCD_DisplayString(14, 438, const_cast<char*>("Touch bottom area: switch mode"));
}

static void RenderUiFrame(float left, float right)
{
    VisPushSample(left, right);
    g_phase_l = Wrap01(g_phase_l + 0.0031f);
    g_phase_r = Wrap01(g_phase_r + 0.0023f);

    switch(g_ui_scene)
    {
        case UiScene::SCOPE: DrawScopeScene(); break;
        case UiScene::VECTOR: DrawVectorScene(); break;
        case UiScene::SPECTRUM: DrawSpectrumScene(); break;
        case UiScene::NEON: DrawNeonScene(); break;
        case UiScene::ORBIT: DrawOrbitScene(); break;
        case UiScene::RINGS_CTRL: DrawRingsCtrlScene(); break;
        case UiScene::PLAITS_CTRL: DrawPlaitsCtrlScene(); break;
        case UiScene::RINGS_VCV_UI: DrawRingsVcvUiScene(); break;
        case UiScene::PLAITS_VCV_UI: DrawPlaitsVcvUiScene(); break;
        default: break;
    }
    if(g_ui_scene == UiScene::SCOPE
       || g_ui_scene == UiScene::RINGS_CTRL
       || g_ui_scene == UiScene::PLAITS_CTRL
       || g_ui_scene == UiScene::RINGS_VCV_UI
       || g_ui_scene == UiScene::PLAITS_VCV_UI)
        return;

    static uint8_t overlay_div = 0;
    overlay_div++;
    if(overlay_div >= 3u)
    {
        overlay_div = 0;
        DrawTouchOverlay();
    }
}

static void UartPrint(const char* format, ...)
{
    char    buffer[192];
    va_list va;
    va_start(va, format);
    const int written = vsnprintf(buffer, sizeof(buffer), format, va);
    va_end(va);
    if(written <= 0)
        return;

    size_t len = static_cast<size_t>(written);
    if(len >= sizeof(buffer))
        len = sizeof(buffer) - 1;

    if(g_uart_ready)
        debug_uart.BlockingTransmit(reinterpret_cast<uint8_t*>(buffer), len, 10);

    char line[192];
    size_t copy_len = len;
    while(copy_len > 0
          && (buffer[copy_len - 1] == '\n' || buffer[copy_len - 1] == '\r'))
    {
        copy_len--;
    }
    if(copy_len >= sizeof(line))
        copy_len = sizeof(line) - 1;
    memcpy(line, buffer, copy_len);
    line[copy_len] = '\0';
    hw.PrintLine("%s", line);
}

static void HandleControlByte(uint8_t c)
{
    if(kUiOnlyVcv)
    {
        UartPrint("[CTRL] UI-only build, audio command ignored: %c\r\n", c);
        return;
    }
    if(c == '.')
    {
        g_run_mode = RunMode::STOP;
        UartPrint("[CTRL] stop(.) -> %s\r\n", RunModeName(g_run_mode));
    }
    else if(c == 't' || c == 'T')
    {
        g_stream_mode = StreamMode::TEST_TONE_1K;
        g_run_mode    = RunMode::ACTIVE;
        UartPrint("[CTRL] -> %s in=%u out=%u\r\n",
                  StreamModeName(g_stream_mode),
                  static_cast<unsigned>(g_in_channels),
                  static_cast<unsigned>(g_out_channels));
    }
    else if(c == 'l' || c == 'L')
    {
        g_stream_mode = StreamMode::LOOP_4IN_4OUT;
        g_run_mode    = RunMode::ACTIVE;
        UartPrint("[CTRL] -> %s in=%u out=%u\r\n",
                  StreamModeName(g_stream_mode),
                  static_cast<unsigned>(g_in_channels),
                  static_cast<unsigned>(g_out_channels));
    }
    else if(c == 'r' || c == 'R')
    {
        g_stream_mode = StreamMode::RINGS_TEST_SQUARE;
        g_run_mode    = RunMode::ACTIVE;
        UartPrint("[CTRL] -> %s\r\n", StreamModeName(g_stream_mode));
    }
    else if(c == 'p' || c == 'P')
    {
        g_stream_mode = StreamMode::PLAITS_TEST_SQUARE;
        g_run_mode    = RunMode::ACTIVE;
        UartPrint("[CTRL] -> %s\r\n", StreamModeName(g_stream_mode));
    }
    else if(c == 'g' || c == 'G')
    {
        g_rings_gate  = !g_rings_gate;
        g_plaits_gate = !g_plaits_gate;
        UartPrint("[CTRL] gate rings=%u plaits=%u\r\n",
                  static_cast<unsigned>(g_rings_gate ? 1 : 0),
                  static_cast<unsigned>(g_plaits_gate ? 1 : 0));
    }
    else
    {
        g_stream_mode = g_stream_mode == StreamMode::LOOP_4IN_4OUT
                            ? StreamMode::TEST_TONE_1K
                            : StreamMode::LOOP_4IN_4OUT;
        g_run_mode    = RunMode::ACTIVE;
        UartPrint("[CTRL] toggle -> %s in=%u out=%u\r\n",
                  StreamModeName(g_stream_mode),
                  static_cast<unsigned>(g_in_channels),
                  static_cast<unsigned>(g_out_channels));
    }
}

static void UsbControlRx(uint8_t* buff, uint32_t* len)
{
    if(buff == nullptr || len == nullptr || *len == 0)
        return;

    for(uint32_t i = 0; i < *len; i++)
    {
        const uint8_t c = buff[i];
        if(c == '\r' || c == '\n' || c == 0)
            continue;
        g_usb_cmd_byte    = c;
        g_usb_cmd_pending = true;
    }
}

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    g_callback_count++;
    if(size > kMiMaxBlock)
        size = kMiMaxBlock;

    float last_l = 0.0f;
    float last_r = 0.0f;

    if(g_run_mode == RunMode::STOP)
    {
        for(size_t i = 0; i < size; i++)
            for(size_t ch = 0; ch < g_out_channels; ch++)
                out[ch][i] = 0.0f;
        g_monitor_l = 0.0f;
        g_monitor_r = 0.0f;
        return;
    }

    if(g_stream_mode == StreamMode::LOOP_4IN_4OUT)
    {
        for(size_t i = 0; i < size; i++)
        {
            for(size_t ch = 0; ch < g_out_channels; ch++)
                out[ch][i] = in[ch][i];
            last_l = in[0][i];
            last_r = in[1][i];
        }
    }
    else if(g_stream_mode == StreamMode::TEST_TONE_1K)
    {
        for(size_t i = 0; i < size; i++)
        {
            const float tone = g_tone_amp * sinf(g_tone_phase);
            g_tone_phase += kTwoPi * g_tone_freq_hz / kToneSampleRate;
            if(g_tone_phase >= kTwoPi)
                g_tone_phase -= kTwoPi;

            for(size_t ch = 0; ch < g_out_channels; ch++)
            {
                const uint32_t pulse_count = static_cast<uint32_t>(ch + 1);
                bool           gate_on     = false;
                if(g_pattern_sample_pos >= kToneGuardSamples
                   && g_pattern_sample_pos < (kToneCycleSamples - kToneGuardSamples))
                {
                    const uint32_t rel     = g_pattern_sample_pos - kToneGuardSamples;
                    const uint32_t slot    = kToneActiveSamples / pulse_count;
                    const uint32_t in_slot = slot > 0 ? (rel % slot) : 0;
                    const uint32_t width
                        = (slot / 2) < kTonePulseWidthSamples ? (slot / 2)
                                                               : kTonePulseWidthSamples;
                    gate_on = (width > 0 && in_slot < width);
                }
                out[ch][i] = gate_on ? tone : 0.0f;
            }
            last_l = out[0][i];
            last_r = out[1][i];

            g_pattern_sample_pos++;
            if(g_pattern_sample_pos >= kToneCycleSamples)
                g_pattern_sample_pos = 0;
        }
    }
    else if(g_stream_mode == StreamMode::RINGS_TEST_SQUARE && g_rings_dsp_ready)
    {
        UpdateRingsDspParams();
        size_t processed = 0;
        while(processed < size)
        {
            size_t chunk = size - processed;
            if(chunk > rings::kMaxBlockSize)
                chunk = rings::kMaxBlockSize;

            for(size_t i = 0; i < chunk; i++)
            {
                const size_t src = processed + i;
                g_rings_in[i] = 0.5f * (in[0][src] + in[1][src]);
            }

            g_rings_part.Process(g_rings_perf,
                                 g_rings_patch,
                                 g_rings_in,
                                 g_rings_out_buf,
                                 g_rings_aux_buf,
                                 chunk);

            for(size_t i = 0; i < chunk; i++)
            {
                const size_t dst = processed + i;
                const float  l   = g_rings_out_buf[i];
                const float  r   = g_rings_aux_buf[i];
                out[0][dst]      = l;
                out[1][dst]      = r;
                out[2][dst]      = l;
                out[3][dst]      = r;
                last_l           = l;
                last_r           = r;
            }
            processed += chunk;
        }
    }
    else if(g_stream_mode == StreamMode::PLAITS_TEST_SQUARE)
    {
        if(PlaitsAdapterReady())
        {
            PlaitsAdapterRender(g_plaits_pitch_norm,
                                g_plaits_timbre_norm,
                                g_plaits_morph_norm,
                                g_plaits_harmonics_norm,
                                g_plaits_model_index,
                                g_plaits_morph_mode,
                                g_plaits_gate,
                                size,
                                g_plaits_out_l,
                                g_plaits_out_r);
            for(size_t i = 0; i < size; i++)
            {
                const float l = g_plaits_out_l[i];
                const float r = g_plaits_out_r[i];
                out[0][i]     = l;
                out[1][i]     = r;
                out[2][i]     = l;
                out[3][i]     = r;
                last_l        = l;
                last_r        = r;
            }
        }
        else
        {
            for(size_t i = 0; i < size; i++)
                for(size_t ch = 0; ch < g_out_channels; ch++)
                    out[ch][i] = 0.0f;
        }
    }
    else
    {
        for(size_t i = 0; i < size; i++)
            for(size_t ch = 0; ch < g_out_channels; ch++)
                out[ch][i] = 0.0f;
    }

    g_monitor_l = last_l;
    g_monitor_r = last_r;
}

static bool InitCodecI2c(I2CHandle&                    i2c,
                         I2CHandle::Config::Peripheral periph,
                         Pin                           scl,
                         Pin                           sda,
                         const char*                   tag,
                         AK4619&                       codec)
{
    I2CHandle::Config cfg;
    cfg.periph         = periph;
    cfg.mode           = I2CHandle::Config::Mode::I2C_MASTER;
    cfg.pin_config.scl = scl;
    cfg.pin_config.sda = sda;

    const I2CHandle::Config::Speed speeds[] = {I2CHandle::Config::Speed::I2C_400KHZ,
                                               I2CHandle::Config::Speed::I2C_100KHZ};

    for(size_t s = 0; s < sizeof(speeds) / sizeof(speeds[0]); s++)
    {
        cfg.speed = speeds[s];
        if(i2c.Init(cfg) != I2CHandle::Result::OK)
            continue;

        for(size_t retry = 0; retry < kCodecProbeRetries; retry++)
        {
            uint8_t detected_addr = 0;
            uint8_t reg00         = 0;
            for(size_t i = 0; i < sizeof(kCodecAddrCandidates); i++)
            {
                const uint8_t addr = kCodecAddrCandidates[i];
                if(i2c.ReadDataAtAddress(addr, 0x00, 1, &reg00, 1, 80)
                   == I2CHandle::Result::OK)
                {
                    detected_addr = addr;
                    break;
                }
            }

            if(detected_addr != 0)
            {
                codec.SetAddress(detected_addr);
                UartPrint("%s codec addr=0x%02X reg00=0x%02X speed=%s retry=%u\r\n",
                          tag,
                          static_cast<unsigned>(detected_addr),
                          static_cast<unsigned>(reg00),
                          cfg.speed == I2CHandle::Config::Speed::I2C_400KHZ ? "400k"
                                                                            : "100k",
                          static_cast<unsigned>(retry));
                return codec.Init(i2c) == AK4619::Result::OK;
            }

            System::Delay(kCodecProbeDelayMs);
        }
    }

    UartPrint("%s codec probe failed (try 0x10/0x11, 400k/100k)\r\n", tag);
    return false;
}

[[noreturn]] static void BlinkFailCode(uint8_t code)
{
    if(code == 0)
        code = 1;
    while(1)
    {
        for(uint8_t i = 0; i < code; i++)
        {
            hw.SetLed(true);
            System::Delay(140);
            hw.SetLed(false);
            System::Delay(140);
        }
        System::Delay(700);
    }
}

static bool InitDebugUart()
{
    UartHandler::Config uart_cfg;
    uart_cfg.periph        = UartHandler::Config::Peripheral::USART_1;
    uart_cfg.mode          = UartHandler::Config::Mode::TX_RX;
    uart_cfg.baudrate      = 115200;
    uart_cfg.pin_config.tx = Pin(PORTB, 14);
    uart_cfg.pin_config.rx = Pin(PORTB, 15);
    g_uart_ready           = (debug_uart.Init(uart_cfg) == UartHandler::Result::OK);
    return g_uart_ready;
}

static bool InitSai1Fixed()
{
    SaiHandle::Config sai1_cfg;
    sai1_cfg.periph          = SaiHandle::Config::Peripheral::SAI_1;
    sai1_cfg.sr              = SaiHandle::Config::SampleRate::SAI_48KHZ;
    sai1_cfg.bit_depth       = SaiHandle::Config::BitDepth::SAI_32BIT;
    sai1_cfg.tdm_slots       = kSai1TdmSlots;
    sai1_cfg.a_enabled       = true;
    sai1_cfg.b_enabled       = true;
    sai1_cfg.a_sync          = SaiHandle::Config::Sync::SLAVE;
    sai1_cfg.b_sync          = SaiHandle::Config::Sync::MASTER;
    sai1_cfg.a_dir           = SaiHandle::Config::Direction::RECEIVE;
    sai1_cfg.b_dir           = SaiHandle::Config::Direction::TRANSMIT;
    sai1_cfg.pin_config.mclk = Pin(PORTF, 7);
    sai1_cfg.pin_config.fs   = Pin(PORTF, 9);
    sai1_cfg.pin_config.sck  = Pin(PORTF, 8);
    sai1_cfg.pin_config.sa   = Pin(PORTC, 1);
    sai1_cfg.pin_config.sb   = Pin(PORTF, 6);
    return sai1_tdm.Init(sai1_cfg) == SaiHandle::Result::OK;
}

static bool InitAudioSingleSai()
{
    if(!InitSai1Fixed())
        return false;

    AudioHandle::Config audio_cfg;
    audio_cfg.blocksize       = 48;
    audio_cfg.samplerate      = SaiHandle::Config::SampleRate::SAI_48KHZ;
    audio_cfg.postgain        = 1.0f;
    audio_cfg.input_channels  = kAudioInChannels;
    audio_cfg.output_channels = kAudioOutChannels;

    g_in_channels  = kAudioInChannels;
    g_out_channels = kAudioOutChannels;

    if(hw.audio_handle.Init(audio_cfg, sai1_tdm) != AudioHandle::Result::OK)
        return false;

    g_callback_count = 0;
    g_stream_mode    = StreamMode::LOOP_4IN_4OUT;
    g_run_mode       = RunMode::ACTIVE;

    // libdaisy DMA IRQ priorities default to 0. Lower SAI DMA IRQ priority so
    // touch/UI tasks are not starved when audio is running.
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 6, 0); // SAI1 A
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 6, 0); // SAI1 B
    return hw.audio_handle.Start(AudioCallback) == AudioHandle::Result::OK;
}

static void ConfigureLcdFramebufferMpuRegion()
{
    MPU_Region_InitTypeDef mpu_cfg;
    HAL_MPU_Disable();

    mpu_cfg.Enable           = MPU_REGION_ENABLE;
    mpu_cfg.Number           = MPU_REGION_NUMBER7;
    mpu_cfg.BaseAddress      = 0xC0000000;
    mpu_cfg.Size             = MPU_REGION_SIZE_32MB;
    mpu_cfg.SubRegionDisable = 0x00;
    mpu_cfg.TypeExtField     = MPU_TEX_LEVEL0;
    mpu_cfg.AccessPermission = MPU_REGION_FULL_ACCESS;
    mpu_cfg.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
    mpu_cfg.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    mpu_cfg.IsCacheable      = MPU_ACCESS_CACHEABLE;
    mpu_cfg.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&mpu_cfg);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

static bool ConfigureLtdcClock()
{
    return true;
}

int main(void)
{
    hw.Configure();
    hw.Init(true);
    hw.StartLog(false);
    System::Delay(300);

    InitDebugUart();
    ConfigureLcdFramebufferMpuRegion();
    if(!ConfigureLtdcClock())
        UartPrint("LTDC clock config failed\r\n");
    MX_LTDC_Init();
    LCD_DisplayDirection(Direction_H);
    LCD_SetBackColor(LCD_BLACK);
    LCD_SetColor(LCD_WHITE);
    LCD_Clear();
    LCD_SetTextFont(&CH_Font24);
    LCD_DisplayString(8, 8, (char*)"IIT6 LTDC RGB OK");
    LCD_SetTextFont(&CH_Font16);
    LCD_DisplayString(8, 40, (char*)(kUiOnlyVcv ? "VCV UI ONLY MODE" : "PMOD1 AUDIO RUNNING"));
    uint8_t touch_ok = Touch_Init();
    if(touch_ok == 0)
    {
        UartPrint("Touch init failed\r\n");
    }
    else
    {
        UartPrint("Touch init ok\r\n");
    }
    SceneApplyStyle();
    if(!kUiOnlyVcv)
        InitMiDsp();
    UartPrint("LTDC init ok w=%u h=%u\r\n",
              static_cast<unsigned>(LCD_Width),
              static_cast<unsigned>(LCD_Height));

    UartPrint("\r\nIIT6 PMOD1 only boot\r\n");
    UartPrint("PMOD2 removed: release PA0/PA1/PA2/PG9/PD11/PB6/PB7 for LCD/other use\r\n");
    UartPrint("LCD/QSPI path enabled in this build\r\n");
    UartPrint("Build mode: %s\r\n", kUiOnlyVcv ? "UI-only (no audio init)" : "Audio+UI");

    hw.usb_handle.SetReceiveCallback(UsbControlRx, UsbHandle::FS_INTERNAL);
    UartPrint("Control: bottom hold switch scene; serial cmds reserved\r\n");
    if(!kUiOnlyVcv)
    {
        UartPrint("codec1=I2C_1(PB8/PB9)\r\n");
        UartPrint("SAI1 full-duplex: SDO=PC1(RX), SDI=PF6(TX), MCLK/BICK/LRCK=PF7/PF8/PF9\r\n");
        I2CHandle codec_i2c_1;
        bool      codec_1_ok = InitCodecI2c(codec_i2c_1,
                                            I2CHandle::Config::Peripheral::I2C_1,
                                            Pin(PORTB, 8),
                                            Pin(PORTB, 9),
                                            "I2C_1",
                                            codec_1);
        if(!codec_1_ok)
        {
            UartPrint("AK4619 init failed on I2C_1\r\n");
            BlinkFailCode(1);
        }

        UartPrint("AK4619 init ok (single I2C)\r\n");

        if(!InitAudioSingleSai())
        {
            UartPrint("Audio init failed\r\n");
            while(1) {}
        }

        UartPrint("Mode LOOP(4in->4out) started ltdc=%ux%u\r\n",
                  static_cast<unsigned>(LCD_Width),
                  static_cast<unsigned>(LCD_Height));
    }

    uint32_t last_print_ms    = System::GetNow();
    uint32_t last_print_count = 0;
    uint32_t last_led_ms      = last_print_ms;
    uint32_t last_touch_ms    = last_print_ms;
    bool     led_on           = false;

    while(1)
    {
        const uint32_t now = System::GetNow();

        if(g_usb_cmd_pending)
        {
            const uint8_t c = g_usb_cmd_byte;
            g_usb_cmd_pending = false;
            HandleControlByte(c);
        }

        if(now - last_touch_ms >= 20)
        {
            last_touch_ms = now;
            Touch_Scan();
            const bool touch_active = (touchInfo.flag != 0u) && (touchInfo.num > 0u);
            bool       bottom_touch = false;
            if(touch_active)
            {
                bottom_touch = touchInfo.x[0] < LCD_Width && touchInfo.y[0] >= kTouchSwitchYMin
                               && touchInfo.y[0] < LCD_Height;
            }
            if(bottom_touch)
            {
                if(g_bottom_press_start_ms == 0u)
                    g_bottom_press_start_ms = now;
                if(!g_mode_switched_on_press
                   && (now - g_bottom_press_start_ms) >= kTouchSwitchHoldMs
                   && (now - g_last_touch_toggle_ms) >= kTouchSwitchDebounceMs)
                {
                    uint8_t next = (static_cast<uint8_t>(g_ui_scene) + 1u)
                                   % static_cast<uint8_t>(UiScene::COUNT);
                    g_ui_scene              = static_cast<UiScene>(next);
                    g_last_touch_toggle_ms  = now;
                    g_mode_switched_on_press = true;
                    SceneApplyStyle();
                    UartPrint("UI mode -> %s\r\n", UiSceneName(g_ui_scene));
                }
            }
            else
            {
                g_bottom_press_start_ms   = 0u;
                g_mode_switched_on_press  = false;
            }
            g_prev_touch = touch_active ? 1u : 0u;
        }

        if(now - g_last_draw_ms >= kUiDrawPeriodMs)
        {
            g_last_draw_ms = now;
            float left     = g_monitor_l;
            float right    = g_monitor_r;
            if(touchInfo.flag != 0u
               && g_ui_scene != UiScene::SCOPE
               && g_ui_scene != UiScene::RINGS_CTRL
               && g_ui_scene != UiScene::PLAITS_CTRL
               && g_ui_scene != UiScene::RINGS_VCV_UI
               && g_ui_scene != UiScene::PLAITS_VCV_UI)
            {
                left  = (static_cast<float>(touchInfo.x[0])
                        / static_cast<float>(LCD_Width > 1 ? (LCD_Width - 1) : 1))
                       * 2.0f
                       - 1.0f;
                right = (static_cast<float>(touchInfo.y[0])
                         / static_cast<float>(LCD_Height > 1 ? (LCD_Height - 1) : 1))
                        * 2.0f
                        - 1.0f;
            }
            left  = Clampf(left, -1.0f, 1.0f);
            right = Clampf(right, -1.0f, 1.0f);
            RenderUiFrame(left, right);
        }

        if(now - last_print_ms >= kModePrintMs)
        {
            const uint32_t cb_now   = g_callback_count;
            const uint32_t cb_delta = cb_now - last_print_count;
            const uint32_t cb_ps
                = static_cast<uint32_t>((static_cast<uint64_t>(cb_delta) * 1000u)
                                        / kModePrintMs);
            last_print_count = cb_now;
            last_print_ms    = now;

            UartPrint("diag mode=%s run=%s cb/s=%lu cb_total=%lu in=%u out=%u ui_only=%u\r\n",
                      StreamModeName(g_stream_mode),
                      RunModeName(g_run_mode),
                      static_cast<unsigned long>(cb_ps),
                      static_cast<unsigned long>(cb_now),
                      static_cast<unsigned>(g_in_channels),
                      static_cast<unsigned>(g_out_channels),
                      static_cast<unsigned>(kUiOnlyVcv ? 1u : 0u));
            UartPrint("diag ltdc=%ux%u\r\n",
                      static_cast<unsigned>(LCD_Width),
                      static_cast<unsigned>(LCD_Height));
        }

        if(now - last_led_ms >= 500)
        {
            last_led_ms = now;
            led_on      = !led_on;
            hw.SetLed(led_on);
        }
        System::Delay(1);
    }
}
