// 声光提示装置 · 参数配置
// 所有阈值与时间参数集中在这里，改行为只改这个文件
//
// 注意：引脚按芯片条件编译。经典 ESP32 与 ESP32-C3 的可用 IO 不同，
// 尤其经典 ESP32 的 GPIO6~11 接 SPI Flash，绝不能用作普通 IO。

#ifndef CONFIG_H
#define CONFIG_H

// ---------- 芯片识别 ----------
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
  #define BOARD_IS_C3 1
#else
  #define BOARD_IS_C3 0
#endif

// ---------- 硬件引脚 ----------
#if BOARD_IS_C3
  #define PIN_MIC    2      // C3：GPIO2 = ADC1_CH2
  #define PIN_LED    8      // C3：GPIO8 可做普通 IO
#else
  #define PIN_MIC    34     // 经典 ESP32：GPIO34 = ADC1_CH6，仅输入，适合接传感器
  #define PIN_LED    2      // 经典 ESP32：GPIO2 板载 LED，安全
#endif

// ---------- 灯带（WS2812B）----------
#define LED_COUNT     30      // 灯带灯珠数
#define LED_BRIGHTNESS 40     // 0-255，全局亮度上限（见下方功率预算）

// ---------- 功率预算（上机前必看）----------
// 单颗 WS2812B 全白满亮约 60 mA；PWM 亮度近似线性缩放。
//   本配置：30 颗 × 60 mA × (40/255) ≈ 282 mA
//   + ESP32-CAM 自身约 150~250 mA（摄像头不初始化）
//   ≈ 总计 430~530 mA @ 5V
//
// 这已经接近 USB-TTL 转接座和 AMS1117 线性稳压器的余量上限。
// 因此提供两档安全档位，按实际供电能力在编译时二选一：
//
//   POWER_TIER 1 = USB-TTL 转接座供电（最保守）
//       12 颗 / 亮度 30  → 约 85 mA
//   POWER_TIER 2 = 独立 5V 电源给灯带（推荐，共地）
//       30 颗 / 亮度 40  → 约 282 mA
//   POWER_TIER 3 = 大电流 5V 电源（≥2A）
//       30 颗 / 亮度 120 → 约 847 mA
//
// 默认用 TIER 1，保证"第一次上机不会因电流不足而复位/闪烁"。
#define POWER_TIER    1

#if POWER_TIER == 1
  #undef  LED_COUNT
  #define LED_COUNT      12
  #undef  LED_BRIGHTNESS
  #define LED_BRIGHTNESS 30
#elif POWER_TIER == 2
  #undef  LED_COUNT
  #define LED_COUNT      30
  #undef  LED_BRIGHTNESS
  #define LED_BRIGHTNESS 40
#elif POWER_TIER == 3
  #undef  LED_COUNT
  #define LED_COUNT      30
  #undef  LED_BRIGHTNESS
  #define LED_BRIGHTNESS 120
#endif

// 编译期功率告警（预处理器只支持整数，故用整数 mA 估算）
#define WS2812_MA_PER_LED  60                    // 每颗全白满亮约 60 mA
#define LED_BUDGET_MA      ((LED_COUNT * WS2812_MA_PER_LED * LED_BRIGHTNESS) / 255)
#if LED_BUDGET_MA > 900
  #warning "LED power budget > 900 mA: use external 5V supply and common ground!"
#endif

// ---------- 采样 ----------
#define SAMPLE_RATE       8000    // Hz
#define WINDOW_MS         10      // 计算窗口
#define WINDOW_SAMPLES    (SAMPLE_RATE / 1000 * WINDOW_MS)   // 80 点
#define MIC_DC_OFFSET     2048    // MAX9814 输出 1.25V 偏置，12 位 ADC 约 2048
#define DB_FLOOR          (-20.0f) // 下限截断，防止 -inf 污染滤波

// ---------- 滤波 ----------
#define MEDIAN_WIN        5       // 5 点中位数（50 ms）
#define EMA_ALPHA         0.2f    // 一阶低通，时间常数约 250 ms

// ---------- 判档（相对基线的增量，dB）----------
#define TH_NORMAL         5.0f    // 静音 → 正常
#define TH_NOISY          8.0f    // 正常 → 偏吵
#define TH_LOUD           15.0f   // 偏吵 → 吵闹
// 回落阈值 = 升级阈值 - 2 dB（迟滞）
#define HYST              2.0f

// ---------- 驻留时间（毫秒）----------
#define DUR_UP_NORMAL     1000
#define DUR_UP_NOISY      1000
#define DUR_UP_LOUD       500
#define DUR_DOWN          2000
#define COMMIT_DEBOUNCE   100     // 状态提交去抖，防 60ms 过渡态闪烁

// ---------- 基线学习 ----------
#define BASELINE_WINDOW        30      // 环形缓冲：30 个"分钟均值"
#define BASELINE_MIN_MINUTES   3       // 至少攒够 3 分钟才启用基线判档
#define BASELINE_PERCENTILE 0.20f     // 取 20 分位，抗偶发长噪声污染
#define BASELINE_MINUTE_MS     60000UL // 一个"分钟均值"的真实时长

// ---------- 开机 ----------
#define BOOT_QUIET_MS     10000   // 前 10 秒只学基线，不判档

// ---------- 失效检测 ----------
#define FAIL_AFTER_MS     5000    // 贴底/贴顶持续 5 秒判失效
#define ADC_FAIL_RATIO    0.005f  // 低于 0.5% 或高于 99.5% 视为贴底/贴顶

// ---------- 灯效 ----------
// LED_BRIGHTNESS 已在上方「灯带」小节按 POWER_TIER 定义，此处不再重复定义
#define BLINK_MAX_HZ      2.0f    // 闪烁频率上限，超过会引起不适

// ---------- 状态 ----------
enum SoundState { ST_QUIET = 0, ST_NORMAL, ST_NOISY, ST_LOUD, ST_SAFE };

#endif
