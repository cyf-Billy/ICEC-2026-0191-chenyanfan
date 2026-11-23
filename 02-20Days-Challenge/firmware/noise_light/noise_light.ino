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

#include <Adafruit_NeoPixel.h>
#include "config.h"

Adafruit_NeoPixel strip(LED_COUNT, PIN_LED, NEO_GRB + NEO_KHZ800);

static float medianBuf[MEDIAN_WIN];
static int   medianIdx = 0;

static float emaDb = 0.0f;
static bool  emaInit = false;

static float baseline = 0.0f;          // 当前基线（相对 dB 的零点）
static float minuteBuf[BASELINE_WINDOW];
static int   minuteIdx = 0;
static int   minuteCount = 0;

static SoundState state = ST_QUIET;
static SoundState pending = ST_QUIET;
static uint32_t pendingSince = 0;
static uint32_t stateSince = 0;

static float upTimer[4] = {0, 0, 0, 0};
static float downTimer = 0.0f;

static uint32_t bootStart = 0;
static uint32_t failTimer = 0;

// ---------------- 采样 ----------------

float readRmsDb() {
  int32_t sum = 0;
  for (int i = 0; i < WINDOW_SAMPLES; i++) {
    int v = analogRead(PIN_MIC) - MIC_DC_OFFSET;
    sum += (int32_t)v * v;
    delayMicroseconds(1000 / 8);   // 8 kHz 粗采样
  }
  float rms = sqrtf((float)sum / WINDOW_SAMPLES);
  if (rms < 1.0f) return DB_FLOOR;
  float db = 20.0f * log10f(rms / 2048.0f);   // 满量程归一
  return db < DB_FLOOR ? DB_FLOOR : db;
}

// 检测 ADC 贴底/贴顶：只有短路或断路才会整窗饱和
bool adcSaturated() {
  static int32_t acc = 0;
  acc = 0;
  for (int i = 0; i < 16; i++) acc += analogRead(PIN_MIC) - MIC_DC_OFFSET;
  float ratio = fabsf((float)acc / 16.0f) / 2048.0f;
  return ratio < ADC_FAIL_RATIO || ratio > (1.0f - ADC_FAIL_RATIO);
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

// ---------------- 基线（20 分位）----------------

void updateBaseline(float db) {
  static float minuteAcc = 0;
  static int   minuteCnt = 0;
  minuteAcc += db;
  minuteCnt++;

  if (minuteCnt >= 6000) {            // 约 1 分钟的 10ms 窗口
    float m = minuteAcc / minuteCnt;
    minuteAcc = 0; minuteCnt = 0;
    minuteBuf[minuteIdx] = m;
    minuteIdx = (minuteIdx + 1) % BASELINE_WINDOW;
    if (minuteCount < BASELINE_WINDOW) minuteCount++;
  }

  if (minuteCount < 3) return;        // 前 3 分钟不做基线判断
  float tmp[BASELINE_WINDOW];
  memcpy(tmp, minuteBuf, sizeof(float) * minuteCount);
  for (int i = 0; i < minuteCount; i++)
    for (int j = i + 1; j < minuteCount; j++)
      if (tmp[j] < tmp[i]) { float t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
  baseline = tmp[(int)(minuteCount * BASELINE_PERCENTILE)];
}

// ---------------- 判档 ----------------

SoundState classify(float delta, uint32_t now) {
  // 各档独立计时，不因状态切换清零
  upTimer[1] = (delta >= TH_NORMAL) ? upTimer[1] + WINDOW_MS : 0;
  upTimer[2] = (delta >= TH_NOISY)  ? upTimer[2] + WINDOW_MS : 0;
  upTimer[3] = (delta >= TH_LOUD)   ? upTimer[3] + WINDOW_MS : 0;
  downTimer += WINDOW_MS;

  // 升级：直达最高满足档
  SoundState target = state;
  if (state < ST_NORMAL && upTimer[1] >= DUR_UP_NORMAL) target = ST_NORMAL;
  if (state < ST_NOISY  && upTimer[2] >= DUR_UP_NOISY)  target = ST_NOISY;
  if (state < ST_LOUD   && upTimer[3] >= DUR_UP_LOUD)   target = ST_LOUD;
  if (state == ST_SAFE) target = ST_NORMAL;    // 失效恢复后从正常开始

  // 回落：每次只降一档，需迟滞 + 2 秒
  if (target == state && downTimer >= DUR_DOWN && state > ST_QUIET) {
    float backTh = (state == ST_LOUD)  ? TH_LOUD  - HYST
                 : (state == ST_NOISY) ? TH_NOISY - HYST
                                       : TH_NORMAL - HYST;
    if (delta < backTh) target = (SoundState)(state - 1);
  }

  // 状态提交去抖
  if (target != state) {
    if (pending != target) { pending = target; pendingSince = now; }
    else if (now - pendingSince >= COMMIT_DEBOUNCE) {
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
    case ST_QUIET:      // 慢呼吸 1 Hz
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
    default:            // 安全态白灯慢呼吸
      strip.fill(strip.Color(80, 80, 80));
      break;
  }
  strip.show();
}

// ---------------- 主流程 ----------------

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();
  bootStart = millis();
  Serial.println("[boot] noise_light start, learning baseline for 10s");
}

void loop() {
  uint32_t now = millis();

  // 失效检测：贴底/贴顶持续
  if (adcSaturated()) {
    failTimer += WINDOW_MS;
  } else {
    failTimer = 0;
  }
  if (failTimer >= FAIL_AFTER_MS) {
    state = ST_SAFE;
    render(state, now);
    delay(WINDOW_MS);
    return;
  }

  float db = readRmsDb();
  float sm = ema(median5(db));
  updateBaseline(sm);

  // 开机静默期：只学基线，不判档
  if (now - bootStart < BOOT_QUIET_MS) {
    render(ST_QUIET, now);
    delay(WINDOW_MS);
    return;
  }

  float delta = sm - baseline;
  SoundState s = classify(delta, now);
  render(s, now);

  static uint32_t lastLog = 0;
  if (now - lastLog > 1000) {
    lastLog = now;
    Serial.printf("db=%.1f base=%.1f delta=%.1f state=%d\n", sm, baseline, delta, s);
  }

  delay(WINDOW_MS);
}
