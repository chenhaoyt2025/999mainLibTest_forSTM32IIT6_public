# 999mainLibTest_forSTM32IIT6

## Overview / 概览
- Single PMOD AK4619 audio on IIT6 (`4 in / 4 out`, SAI1 full-duplex).
- LTDC RGB LCD (`800x480`) + GT911 touch UI.
- Rings/Plaits test integration (DaisySP + Mutable DSP code in project tree).

- IIT6 单 PMOD AK4619 音频（`4进4出`，SAI1 全双工）。
- LTDC RGB 屏（`800x480`）+ GT911 触摸界面。
- 集成 Rings/Plaits 测试（DaisySP + 工程内 Mutable DSP 代码）。

## Current Behavior (Code-Accurate) / 当前行为（与代码一致）
- UI scenes (9): `SCOPE`, `RINGS_CTRL`, `PLAITS_CTRL`, `RINGS_VCV_UI`, `PLAITS_VCV_UI`, `VECTOR`, `SPECTRUM`, `NEON`, `ORBIT`.
- Touch: long-press bottom area to switch scene.

- 界面模式共 9 个：`SCOPE`、`RINGS_CTRL`、`PLAITS_CTRL`、`RINGS_VCV_UI`、`PLAITS_VCV_UI`、`VECTOR`、`SPECTRUM`、`NEON`、`ORBIT`。
- 触摸：按住底部区域可切换模式。

- Audio policy:
- `RINGS_CTRL` / `RINGS_VCV_UI` -> `RINGS(square)` self-sound.
- `PLAITS_CTRL` / `PLAITS_VCV_UI` -> `PLAITS(square)` self-sound.
- All other scenes -> `LOOP(4in->4out)`.

- 音频策略：
- `RINGS_CTRL` / `RINGS_VCV_UI` 使用 `RINGS(square)` 自发声。
- `PLAITS_CTRL` / `PLAITS_VCV_UI` 使用 `PLAITS(square)` 自发声。
- 其余所有场景统一使用 `LOOP(4in->4out)`。

## Serial Commands / 串口命令
- `l/L`: set `LOOP`
- `t/T`: set `TONE(1k pulse)`
- `r/R`: set `RINGS(square)`
- `p/P`: set `PLAITS(square)`
- `x/X`: rotate loop input map
- `g/G`: toggle rings/plaits gate
- `.`: `STOP`

- `l/L`：切到 `LOOP`
- `t/T`：切到 `TONE(1k pulse)`
- `r/R`：切到 `RINGS(square)`
- `p/P`：切到 `PLAITS(square)`
- `x/X`：循环输入映射旋转
- `g/G`：切换 rings/plaits gate
- `.`：停止

Note / 说明:
- Scene-based audio policy is applied continuously, so scene audio mapping has priority.
- 场景音频策略会持续生效，因此场景映射优先级更高。

## Build Option / 编译开关
- File: `build_config.mk`
- `USE_PANEL_BACKGROUNDS ?= 0`
  - `0`: no panel background images (default, smaller firmware)
  - `1`: compile/use panel backgrounds (`rings/plaits/scope`)

- 文件：`build_config.mk`
- `USE_PANEL_BACKGROUNDS ?= 0`
  - `0`：不编译/不使用底图（默认，固件更小）
  - `1`：编译并使用底图（`rings/plaits/scope`）

## Hardware Pins / 硬件引脚
- AK4619 I2C1: `PB8/PB9`
- SAI1: `PC1`(SDO/RX), `PF6`(SDI/TX), `PF7`(MCLK), `PF8`(BICK), `PF9`(LRCK)
- UART1: `PB14/PB15`
- Touch (GT911 soft-I2C): `PI11`(SCL), `PI8`(SDA), `PG3`(INT), `PH4`(RST)

## Build & Flash / 编译与烧录
```bash
cd /Users/hoho_mini/Documents/stm32H743IIT6/999mainLibTest_forSTM32IIT6
make clean && make -j4
dfu-util -a 0 -s 0x08000000:leave -D build/TDM_AK4619_DaisySP.bin
```
