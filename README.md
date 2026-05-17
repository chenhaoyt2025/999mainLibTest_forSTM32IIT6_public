# 999mainLibTest_forSTM32IIT6

## Overview
- Single PMOD AK4619 audio on IIT6 (`4 in / 4 out`, SAI1 full-duplex).
- LTDC RGB LCD (`800x480`) and GT911 touch UI.
- Rings/Plaits test integration (DaisySP + Mutable DSP code in this project tree).

## Current Behavior (Code-Accurate)
- UI scenes (9): `SCOPE`, `RINGS_CTRL`, `PLAITS_CTRL`, `RINGS_VCV_UI`, `PLAITS_VCV_UI`, `VECTOR`, `SPECTRUM`, `NEON`, `ORBIT`.
- Touch: long-press the bottom area to switch scene.

Audio policy:
- `RINGS_CTRL` / `RINGS_VCV_UI` -> `RINGS(square)` self-sound.
- `PLAITS_CTRL` / `PLAITS_VCV_UI` -> `PLAITS(square)` self-sound.
- All other scenes -> `LOOP(4in->4out)`.

## Serial Commands
- `l/L`: set `LOOP`
- `t/T`: set `TONE(1k pulse)`
- `r/R`: set `RINGS(square)`
- `p/P`: set `PLAITS(square)`
- `x/X`: rotate loop input map
- `g/G`: toggle rings/plaits gate
- `.`: `STOP`

Note:
- Scene-based audio policy is applied continuously, so scene mapping has priority over manual serial mode switching.

## Build Option
- File: `build_config.mk`
- `USE_PANEL_BACKGROUNDS ?= 0`
  - `0`: no panel background images (default, smaller firmware)
  - `1`: compile/use panel backgrounds (`rings/plaits/scope`)

## Hardware Pins
- AK4619 I2C1: `PB8/PB9`
- SAI1: `PC1` (SDO/RX), `PF6` (SDI/TX), `PF7` (MCLK), `PF8` (BICK), `PF9` (LRCK)
- UART1: `PB14/PB15`
- Touch (GT911 soft-I2C): `PI11` (SCL), `PI8` (SDA), `PG3` (INT), `PH4` (RST)

## Build and Flash
```bash
cd /Users/hoho_mini/Documents/stm32H743IIT6/999mainLibTest_forSTM32IIT6
make clean && make -j4
dfu-util -a 0 -s 0x08000000:leave -D build/TDM_AK4619_DaisySP.bin
```

## License
This project is licensed under the MIT License. See [LICENSE](LICENSE).

## TDM Driver References
- `PMOD/eurorack-pmod-master/gateware`
- `daisy_seed_tdm_eurorack_pmod`
