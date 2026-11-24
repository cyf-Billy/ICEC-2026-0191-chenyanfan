# firmware / 阶段二源码

本目录按考核规范「02-20Days-Challenge/：阶段二源码、process_logs/ 与 iterations/」设置。

> **定位**：这是阶段二之外的**自主扩展装置**——项目五是「拆别人的极简机」（逆向），本装置是「从零做一台」（正向）。两者共用同一套动手能力与硬件基础（同一批采购的 ESP32 / 麦克风 / 灯带）。

## 当前状态（2026-11-24 更新）

| 项 | 状态 |
| --- | --- |
| 代码 | ✅ v2.0，已修 2 个 bug（失效误判、计时偏差），见《烧录操作卡》第 7 节 |
| 编译 | ✅ 经典 ESP32 304973 B（23%）｜ESP32-C3 323938 B（24%） |
| 开发环境 | ✅ **Arduino IDE 2.3.11**（`D:\ICEC\tools\arduino-ide-2.3.11`，双击 `D:\ICEC\打开Arduino IDE.cmd` 启动）+ arduino-cli 1.6.0 |
| 逻辑验证 | ✅ `logic_check.py` 9/9 通过 |
| 烧录准备 | ✅ **已配齐**：esptool 5.3.1 + AI Thinker ESP32-CAM 板型 + CH340 驱动（已安装） |
| 上机 | ⏳ 仍缺**板子 + 数据线**；接线表与烧录步骤见 `烧录操作卡-声光装置.md` |

## 目录结构

```text
firmware/
├─ README.md                 ← 本文件
└─ noise_light/              ← 声光提示装置（延展项目）
   ├─ noise_light.ino        ← 主程序：采样 → 滤波 → 判档 → 灯效
   ├─ config.h               ← 全部阈值与时间参数
   └─ logic_check.py         ← 判定逻辑离线验证（9 条用例）
```

## 当前状态（2026-11-24）

| 项 | 状态 |
| --- | --- |
| 逻辑设计 | ✅ 完成（判定四要素、状态机、7 类边界处理）|
| 逻辑验证 | ✅ `logic_check.py` 9 条用例 **9/9 通过** |
| 固件实现 | ✅ 完成，`arduino-cli compile` 编译通过 |
| 编译占用 | flash **322832 B / 24%**，RAM **14728 B / 4%**（`esp32:esp32:esp32c3`）|
| 上机验证 | ⬜ 未做（器件未接）|

## 接线表（按芯片自动切换引脚）

`config.h` 里按芯片做了条件编译，两种板子的脚位不同——**尤其经典 ESP32 的 GPIO6~11 接 SPI Flash，绝不能当普通 IO 用**，接到那里会直接导致程序跑飞。

| 信号 | 经典 ESP32（CH340 板） | ESP32-C3 | 说明 |
| --- | --- | --- | --- |
| MAX9814 OUT | **GPIO34**（ADC1_CH6，仅输入） | **GPIO2**（ADC1_CH2） | 模拟输入，不要接带内部上拉的脚 |
| WS2812B DIN | **GPIO2** | **GPIO8** | 串 470Ω 电阻，两端共地 |
| WS2812B 5V | 5V（独立供电） | 5V（独立供电） | 30 灯全亮约 1.8 A，不要从板子 3V3 取 |
| WS2812B GND | GND | GND | **必须与主控共地** |
| MAX9814 VDD | 3V3 | 3V3 | 模块自带 AGC |
| MAX9814 GND | GND | GND | — |

## 编译验证结果（2026-11-24）

| 目标 | FQBN | flash | RAM |
| --- | --- | --- | --- |
| 经典 ESP32 | `esp32:esp32:esp32` | 302189 B（23%） | 23172 B（7%）|
| ESP32-C3 | `esp32:esp32:esp32c3` | 322832 B（24%） | 14728 B（4%）|

两种目标均编译通过，说明引脚条件编译生效。



| 依赖 | 版本 | 来源 |
| --- | --- | --- |
| 开发板核心 | `esp32:esp32` **3.3.12** | 镜像 `https://arduino.me/packages/esp32.json` |
| 灯带库 | `Adafruit NeoPixel` **1.15.5** | Library Manager |

第三方库不提交源码，只在此记录版本；换机器时按上表重装即可。

## 编译与上传

```powershell
# 编译
arduino-cli compile --fqbn esp32:esp32:esp32c3 .\noise_light

# 上传（端口按实际改）
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32c3 .\noise_light
```

## 实现与设计的对应关系

| 设计文档中的规则 | 实现位置 |
| --- | --- |
| 判据用"相对基线的增量" | `loop()` 中 `delta = sm - baseline` |
| 基线取 20 分位、30 分钟滑窗 | `updateBaseline()` |
| 三层滤波（中位数/EMA/迟滞）| `median5()`、`ema()`、`classify()` |
| 升级可跳档、回落逐级 | `classify()` 中 `target` 的取值顺序 |
| 状态提交去抖 100 ms | `classify()` 的 `pending` / `pendingSince` |
| ADC 贴底贴顶判失效 | `adcSaturated()` + `failTimer` |
| 开机 10 秒静默期 | `now - bootStart < BOOT_QUIET_MS` |
| 灯效语义（颜色 + 节奏，≤2 Hz）| `stateColor()`、`render()` |

## 还没验证的部分（上机才知）

| 项 | 为什么必须上机 | 预期验证方式 |
| --- | --- | --- |
| MAX9814 实际直流偏置 | 标称 1.25 V，但供电 3.3 V 时可能偏离 | 串口打印原始 ADC 均值，确认 `MIC_DC_OFFSET` 是否需要改 |
| `ADCD_FAIL_RATIO` 阈值 | 贴底判据依赖实际噪声底 | 断开麦克风，看是否 5 秒内进安全态 |
| `DB_FLOOR` 与 dB 映射 | RMS→dB 的标定系数是估算值 | 与声级计并排对比，做单点偏移校准 |
| 灯带电流与电源 | 30 灯全亮约 1.8 A，USB 供电可能不足 | 全白测试，看是否掉压重启 |

> 这四项都属于「参数标定」，不是「逻辑错误」——逻辑已经用 9 条用例验证过，上机只需要调数字。

## 约定

- 本目录只放**自己写的**代码，第三方库通过上表记录版本，不把库源码提交进来
- 每次提交对应一次可编译的改动，提交信息写清「改了什么、为什么」
