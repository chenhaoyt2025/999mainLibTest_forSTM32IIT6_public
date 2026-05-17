# TDM_AK4619_DaisySP (IIT6)

当前工程目标：  
- `PMOD1` 单 AK4619 音频（`4in/4out` loop + `tone`）  
- `LTDC/RGB` 屏幕显示（`800x480`）  
- `GT911` 触摸（5个显示模式切换）

## 关键结论（本次已验证）

- IIT6 这块屏在本项目里应走 **LTDC/RGB**，不是 `lcd.c` 那种 `FMC 8080` 读 ID 流程。  
- 之前 `lcd_id=0xFFFF` 是 8080 路径不通，不是变体 `libdaisy` 坏。  
- 触摸/显示驱动放在项目内 `BSP/LTDC`，不改 `libdaisy` 库本体。

## 当前功能

- 音频模式：
  - `l` / `L`：`LOOP(4in->4out)`
  - `t` / `T`：`TONE(1k pulse)`
  - `.`：`STOP`
- 显示模式（共5种）：
  - `SCOPE` / `VECTOR` / `SPECTRUM` / `NEON` / `ORBIT`
  - 触摸底部区域可循环切换

## 当前引脚（工程实际使用）

- `SAI1 TDM`：`PC1/PF6/PF7/PF8/PF9`
- `AK4619 I2C`：`PB8/PB9`
- `UART1`：`PB14/PB15`
- `LTDC/RGB + SDRAM`：由 `BSP/LTDC/lcd_rgb.c` 与 `libdaisy` SDRAM 初始化共同提供
- `GT911 触摸`（软件I2C）：
  - `SCL=PI11`
  - `SDA=PI8`
  - `INT=PG3`
  - `RST=PH4`

## 冲突与注意事项

- 不要再同时接入 `BSP/LCD/lcd.c`（8080/FMC方案）和 `BSP/LTDC/lcd_rgb.c`。  
- `libdaisy` 已有 `HAL_SDRAM_MspInit`、`HAL_TIM_Base_MspInit`，所以本工程不链接 standalone 版 `sdram.c/lcd_pwm.c`，避免重复定义。  

## 构建

```bash
cd /Users/hoho_mini/Documents/stm32H743IIT6/TDM_AK4619_DaisySP
make clean && make
```
