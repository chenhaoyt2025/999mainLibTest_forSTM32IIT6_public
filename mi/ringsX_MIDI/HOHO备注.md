# ringsX_MIDI 开发备注

**作者**: HOHO
**创建时间**: 2026年3月

---

## 1. 核心改进 (Unified RingsX)
本项目整合了以下几种模式，并引入了 MIDI 控制支持：
1.  **原版 Rings**: 完整保留所有复音（1/2/4）和原版算法。
2.  **Audrey II**: 暴力反馈与非线性失真模式 (Port Synthux Academy)。
3.  **Hilbert Shifter**: 实验性频移与相位偏移模式。

---

## 2. MIDI 映射表 (CC Mapping)

### 2.1 全局控制开关
| CC 编号 | 功能描述 | 默认状态 |
| :--- | :--- | :--- |
| **CC 9** | **MIDI Function Switch** | **OFF** (仅响应 CC 11/12/110 和 Thru) |

### 2.2 基础参数控制 (CC 1-5)
*需 CC 9 >= 64 开启*。直接映射到面板大旋钮参数。

| CC 编号 | 对应面板参数 | Audrey 模式下的功能 (model >= 7) |
| :--- | :--- | :--- |
| **CC 1** | **Frequency** | 选定模式的基础频率 (Pitch) |
| **CC 2** | **Structure** | Body 参数 (FM结构与反馈延迟基频) |
| **CC 3** | **Brightness** | Nervousness (调制频率与不稳定性) |
| **CC 4** | **Damping** | Decay (声振时长) |
| **CC 5** | **Position** | Reverb Mix (混响干湿比) |

### 2.3 按键与设置控制 (CC 11-12, 110)
*始终有效 (不受 CC 9 限制)*。

| CC 编号 | 功能描述 | 备注 |
| :--- | :--- | :--- |
| **CC 11** | **Left Button** | 控制复音数 (>= 64 视为触发一次点击) |
| **CC 12** | **Right Button** | 切换算法模式 (>= 64 视为触发一次点击) |
| **CC 110** | **MIDI Channel** | 全局设置并保存 MIDI 通道到 Flash (1-16) |

### 2.4 其他 MIDI 行为
*   **MIDI Thru**: 自动将接收到的 MIDI 原始数据从 TX 端口转发出去 (始终开启)。
*   **MIDI Note**: *需 CC 9 开启*。仅作为 V/OCT CV 注入，不触发内建 Strum。
*   **Catch-up**: 物理旋钮需跨越 MIDI 值才能重新获得控制权。

---

## 3. 开发注意事项
*   **存储逻辑**: 只有 MIDI 通道 (CC 110) 会触发 Flash 写入。
*   **隔离规范**: 必须使用 `ringsX_MIDI/` 目录下的驱动和代码。