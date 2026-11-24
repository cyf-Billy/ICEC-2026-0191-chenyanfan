// 声光提示装置 · 主程序
//
// 实现《方案-校园声学地图与智能光效系统.md》第四~七节验证过的判定逻辑：
//   采样 → 去偏置 → RMS → 相对 dB → 中位数 → EMA → 基线学习 → 判档 → 状态机 → 灯效
//
// 规则要点（与 logic_check.py 的 9 条用例一一对应）：
//   1. 判据是"相对本点位基线的增量"，不是绝对分贝
//   2. 升级可跳档（直接进最高满足档），回落逐级
//   3. 状态提交有 100ms 去抖，避免阈值穿越时间差造成的闪烁
//   4. ADC 贴底/贴顶持续 5 秒 → 进入安全态，不假装正常
//
// v2 修订（2026-11-24）：
//   [修 bug] 原失效检测用"均值偏离偏置量"判断，麦克风正常工作（均值≈偏置）时
//            偏离接近 0，会被误判为"贴底"。改为统计"贴轨样本占比" +
//            "窗口内是否有任何波动"，只在真正短路/断路时才进安全态。
//   [修 bug] 原计时用"每次循环累加 WINDOW_MS"，实际循环耗时约 20ms（采样 10ms +
//            delay 10ms），导致驻留时间被低估约一半。改为用 millis() 实测差值。
//   [改] ADC 分辨率与衰减显式设置，经典 ESP32 与 C3 默认值不同，避免换板后量程变化。
//   [加] 串口调试命令：b=重学基线 s=打印状态 t=灯带自检 d=切换详细日志
//   [加] 开机灯带自检（红→绿→蓝），没有串口也能确认接线

#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "config.h"

#define FW_VERSION "2.0"

Adafruit_NeoPixel strip(LED_COUNT, PIN_LED, NEO_GRB + NEO_KHZ800);

static float medianBuf[MEDIAN_WIN];
static int   medianIdx = 0;

static float emaDb = 0.0f;
static bool  emaInit = false;

static float baseline = 0.0f;          // 当前基线（相对 dB 的零点）
static float minuteBuf[BASELINE_WINDOW];
static int   minuteIdx = 0;
static int   minuteCount = 0;
static uint32_t minuteStart = 0;       // 按真实时间累积"分钟均值"

static SoundState state = ST_QUIET;
static SoundState pending = ST_QUIET;
static uint32_t pendingSince = 0;
static uint32_t stateSince = 0;

static uint32_t upTimer[4] = {0, 0, 0, 0};   // 各档累计满足时长（ms）
static uint32_t downTimer = 0;               // 连续不满足时长（ms）

static uint32_t bootStart = 0;
static uint32_t failTimer = 0;
static uint32_t lastTick = 0;

static bool verbose = true;

// ---------------- ADC 初始化 ----------------

void adcInit() {
  analogReadResolution(12);
#if defined(ADC_ATTEN_DB_12)
  analogSetPinAttenuation(PIN_MIC, ADC_ATTEN_DB_12);   // 0~3.3V 量程
#elif defined(ADC_11db)
  analogSetPinAttenuation(PIN_MIC, ADC_11db);          // 旧核心的等价写法
#endif
}

// ---------------- 采样 ----------------

// 读一个 10ms 窗口的 RMS，返回以满量程归一后的 dB（负值）
float readRmsDb() {
  int32_t sum = 0;
  for (int i = 0; i < WINDOW_SAMPLES; i++) {
    int v = analogRead(PIN_MIC) - MIC_DC_OFFSET;
    sum += (int32_t)v * v;
    delayMicroseconds(1000 / 8);   // 8 kHz
  }
  float rms = sqrtf((float)sum / WINDOW_SAMPLES);
  if (rms < 1.0f) return DB_FLOOR;
  float db = 20.0f * log10f(rms / 2048.0f);
  return db < DB_FLOOR ? DB_FLOOR : db;
}

// 失效检测（v2 修正）
//   贴轨：样本值 < 64 或 > 4031（约 1.5% / 98.5% 满量程）算贴轨
//   判据：贴轨样本占比 > 90%  →  短路/断路，传感器没有有效信号
//   补充：窗口内所有样本完全相同且贴在轨上 → 也判失效
//   注意：麦克风正常工作时均值在偏置附近，这属于"正常"，绝不能判失效
bool adcFailDetect() {
  const int N = 32;
  int lo = 0, hi = 0, same = 0;
  int first = analogRead(PIN_MIC);
  for (int i = 0; i < N; i++) {
    int v = analogRead(PIN_MIC);
    if (v < 64) lo++;
    if (v > 4031) hi++;
    if (v == first) same++;
    delayMicroseconds(100);
  }
  float railRatio = (float)(lo + hi) / N;
  if (railRatio > 0.9f) return true;
  if (same == N && (first < 64 || first > 4031)) return true;
  return false;
}

// ---------------- 滤波 ----------------

float median5(float x) {
  medianBuf[medianIdx] = x;
  medianIdx = (medianIdx + 1) % MEDIAN_WIN;
  float tmp[MEDIAN_WIN];
  memcpy(tmp, medianBuf, sizeof(tmp));
  for (int i = 0; i < MEDIAN_WIN; i++)
    for (int j = i + 1; j < MEDIAN_WIN; j++)
      if (tmp[j] < tmp[i]) { float t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
  return tmp[MEDIAN_WIN / 2];
}

float ema(float x) {
  if (!emaInit) { emaDb = x; emaInit = true; }
  else          { emaDb = EMA_ALPHA * x + (1 - EMA_ALPHA) * emaDb; }
  return emaDb;
}

// ---------------- 基线（20 分位，抗偶发噪声）----------------

void updateBaseline(float db, uint32_t now) {
  static float minuteAcc = 0;
  static int   minuteCnt = 0;
  static bool  minuteInit = false;

  if (!minuteInit) { minuteStart = now; minuteInit = true; }
  minuteAcc += db;
  minuteCnt++;

  if (now - minuteStart >= 60000UL) {          // 满 1 分钟
    minuteBuf[minuteIdx] = minuteAcc / minuteCnt;
    minuteIdx = (minuteIdx + 1) % BASELINE_WINDOW;
    if (minuteCount < BASELINE_WINDOW) minuteCount++;
    minuteAcc = 0; minuteCnt = 0; minuteStart = now;

    if (verbose) {
      Serial.printf("[base] minute #%d = %.1f dB\n", minuteCount,
                    minuteBuf[(minuteIdx + BASELINE_WINDOW - 1) % BASELINE_WINDOW]);
    }
  }

  if (minuteCount < BASELINE_MIN_MINUTES) return;
  float tmp[BASELINE_WINDOW];
  memcpy(tmp, minuteBuf, sizeof(float) * minuteCount);
  for (int i = 0; i < minuteCount; i++)
    for (int j = i + 1; j < minuteCount; j++)
      if (tmp[j] < tmp[i]) { float t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
  int idx = (int)(minuteCount * BASELINE_PERCENTILE);
  if (idx >= minuteCount) idx = minuteCount - 1;
  baseline = tmp[idx];
}

void resetBaseline() {
  minuteCount = 0; minuteIdx = 0;
  baseline = 0.0f;
  Serial.println("[base] reset, relearning...");
}

// ---------------- 判档 ----------------

SoundState classify(float delta, uint32_t dt) {
  upTimer[1] = (delta >= TH_NORMAL) ? upTimer[1] + dt : 0;
  upTimer[2] = (delta >= TH_NOISY)  ? upTimer[2] + dt : 0;
  upTimer[3] = (delta >= TH_LOUD)   ? upTimer[3] + dt : 0;
  downTimer  = downTimer + dt;

  // 升级：直达最高满足档
  SoundState target = state;
  if (state < ST_NORMAL && upTimer[1] >= DUR_UP_NORMAL) target = ST_NORMAL;
  if (state < ST_NOISY  && upTimer[2] >= DUR_UP_NOISY)  target = ST_NOISY;
  if (state < ST_LOUD   && upTimer[3] >= DUR_UP_LOUD)   target = ST_LOUD;
  if (state == ST_SAFE) target = ST_NORMAL;      // 失效恢复后从正常开始

  // 回落：每次只降一档，需迟滞 + 驻留
  if (target == state && downTimer >= DUR_DOWN && state > ST_QUIET) {
    float backTh = (state == ST_LOUD)  ? TH_LOUD  - HYST
                 : (state == ST_NOISY) ? TH_NOISY - HYST
                                       : TH_NORMAL - HYST;
    if (delta < backTh) target = (SoundState)(state - 1);
  }

  // 状态提交去抖
  uint32_t now = millis();
  if (target != state) {
    if (pending != target) { pending = target; pendingSince = now; }
    else if (now - pendingSince >= COMMIT_DEBOUNCE) {
      if (verbose) {
        Serial.printf("[state] %d -> %d (delta=%.1f)\n", state, (int)target, delta);
      }
      state = target;
      stateSince = now;
      downTimer = 0;
      pending = state;
    }
  } else {
    pending = state;
  }
  return state;
}

// ---------------- 灯效 ----------------

uint32_t stateColor(SoundState s) {
  switch (s) {
    case ST_QUIET:  return strip.Color(0, 20, 80);     // 深蓝
    case ST_NORMAL: return strip.Color(0, 120, 60);    // 青绿
    case ST_NOISY:  return strip.Color(160, 120, 0);   // 黄
    case ST_LOUD:   return strip.Color(200, 20, 20);   // 红
    default:        return strip.Color(80, 80, 80);    // 白（安全态）
  }
}

void render(SoundState s, uint32_t now) {
  uint32_t c = stateColor(s);
  float t = (now - stateSince) / 1000.0f;

  switch (s) {
    case ST_QUIET:      // 慢呼吸
      strip.fill(strip.Color(0, 20, (uint8_t)(60 + 40 * sinf(t * 6.28f))));
      break;
    case ST_NORMAL:     // 常亮
      strip.fill(c);
      break;
    case ST_NOISY:      // 0.5 Hz 缓闪
      strip.fill((fmodf(t, 2.0f) < 1.0f) ? c : strip.Color(0, 0, 0));
      break;
    case ST_LOUD:       // 2 Hz 上限脉冲
      strip.fill((fmodf(t, 0.5f) < 0.25f) ? c : strip.Color(20, 0, 0));
      break;
    default:            // 安全态白灯
      strip.fill(strip.Color(80, 80, 80));
      break;
  }
  strip.show();
}

// 开机自检：红→绿→蓝，每次 300ms
void ledSelfTest() {
  const uint32_t cols[3] = {strip.Color(80, 0, 0), strip.Color(0, 80, 0), strip.Color(0, 0, 80)};
  for (int i = 0; i < 3; i++) {
    strip.fill(cols[i]);
    strip.show();
    delay(300);
  }
  strip.clear();
  strip.show();
}

// ---------------- 串口命令 ----------------

void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case 'b': resetBaseline(); break;
      case 's':
        Serial.printf("[status] v%s state=%d baseline=%.2f ema=%.2f minutes=%d\n",
                      FW_VERSION, (int)state, baseline, emaDb, minuteCount);
        break;
      case 't': ledSelfTest(); break;
      case 'd':
        verbose = !verbose;
        Serial.printf("[log] verbose=%d\n", verbose);
        break;
      default: break;
    }
  }
}

// ---------------- 主流程 ----------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[boot] noise_light v%s chip=%s\n", FW_VERSION,
                BOARD_IS_C3 ? "ESP32-C3" : "ESP32");
  Serial.printf("[boot] PIN_MIC=%d PIN_LED=%d LED_COUNT=%d\n", PIN_MIC, PIN_LED, LED_COUNT);

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();

  ledSelfTest();
  adcInit();

  bootStart = millis();
  lastTick = bootStart;
  Serial.printf("[boot] learning baseline for %d ms, keep quiet\n", BOOT_QUIET_MS);
}

void loop() {
  uint32_t now = millis();
  uint32_t dt = now - lastTick;
  lastTick = now;
  if (dt > 200) dt = 200;      // 防长时间阻塞导致计时跳变

  pollSerial();

  // 失效检测：贴轨持续 5 秒才进安全态
  if (adcFailDetect()) {
    failTimer += dt;
  } else {
    failTimer = 0;
  }
  if (failTimer >= FAIL_AFTER_MS) {
    if (state != ST_SAFE) {
      Serial.println("[fail] ADC saturated, enter SAFE state (check wiring)");
      state = ST_SAFE;
      stateSince = now;
    }
    render(state, now);
    delay(WINDOW_MS);
    return;
  }

  float db = readRmsDb();
  float sm = ema(median5(db));
  updateBaseline(sm, now);

  // 开机静默期：只学基线，不判档
  if (now - bootStart < BOOT_QUIET_MS) {
    render(ST_QUIET, now);
    delay(WINDOW_MS);
    return;
  }

  float delta = sm - baseline;
  SoundState s = classify(delta, dt);
  render(s, now);

  static uint32_t lastLog = 0;
  if (verbose && now - lastLog > 1000) {
    lastLog = now;
    Serial.printf("db=%.1f base=%.1f delta=%.1f state=%d\n", sm, baseline, delta, s);
  }

  delay(WINDOW_MS);
}
