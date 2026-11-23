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

#define LED_COUNT     30      // 灯带灯珠数

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
#define BASELINE_WINDOW   30      // 30 个 1 分钟中位数
#define BASELINE_PERCENTILE 0.20f // 取 20 分位，抗偶发长噪声污染

// ---------- 开机 ----------
#define BOOT_QUIET_MS     10000   // 前 10 秒只学基线，不判档

// ---------- 失效检测 ----------
#define FAIL_AFTER_MS     5000    // 贴底/贴顶持续 5 秒判失效
#define ADC_FAIL_RATIO    0.005f  // 低于 0.5% 或高于 99.5% 视为贴底/贴顶

// ---------- 灯效 ----------
#define LED_BRIGHTNESS    60      // 0-255，限制亮度避免扰民
#define BLINK_MAX_HZ      2.0f    // 闪烁频率上限，超过会引起不适

// ---------- 状态 ----------
enum SoundState { ST_QUIET = 0, ST_NORMAL, ST_NOISY, ST_LOUD, ST_SAFE };

#endif
