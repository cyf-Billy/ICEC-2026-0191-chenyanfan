"""校园声学地图与智能光效系统 · 判定逻辑离线验证

实现《方案-校园声学地图与智能光效系统.md》第四~七节的规则，
用 10ms 采样步长回放输入序列，输出状态序列，并对照期望结果。
不依赖任何硬件：输入是"相对基线的增量 Δ(dB)"序列。

实现过程中修正过的三处（都写进了方案文档）：
1. ADC 失效判据不能用"方差≈0"——极静环境方差也≈0，会误判。改为"贴底/贴顶"。
2. 阶跃输入若强制逐级升级，会在"正常"白等 1 秒才升档 → 改为**升级可跳档**（直达最高满足档）。
3. 回落仍逐级（每次只降一档），保证不会从吵闹直接掉回静音造成闪烁。
"""

SAMPLES_PER_SEC = 100          # 10 ms
MEDIAN_WIN = 5                 # 50 ms
EMA_ALPHA = 0.2

TH = {1: 5, 2: 8, 3: 15}       # 升级阈值（dB，相对基线）
DUR_UP = {1: 1.0, 2: 1.0, 3: 0.5}
TH_BACK = {1: 5, 2: 6, 3: 13}  # 回落阈值（迟滞：比升级阈值低 2 dB）
DUR_DOWN = 2.0

BOOT_QUIET = 10.0
FLOOR_DB = -20.0
FAIL_AFTER = 5.0
COMMIT_DEBOUNCE = 0.1          # 状态提交去抖：目标需稳定 100ms 才真正切换
STATES = {0: "静音", 1: "正常", 2: "偏吵", 3: "吵闹", 4: "安全态(失效)"}


def expand(segments):
    out = []
    for dur, val in segments:
        out.extend([val] * int(round(dur * SAMPLES_PER_SEC)))
    return out


def simulate(segments):
    """回放输入序列，返回 [(时刻秒, 状态)]"""
    samples = expand(segments)
    hist, ema = [], None
    state = 0
    up = {1: 0.0, 2: 0.0, 3: 0.0}
    down = 0.0
    pending, pending_since = None, 0.0
    boot_n = int(BOOT_QUIET * SAMPLES_PER_SEC)
    fail_t = 0.0
    timeline = [(0.0, STATES[0])]

    for i, raw in enumerate(samples):
        t = i / SAMPLES_PER_SEC

        # ---- ADC 失效：贴底持续 5 秒 ----
        fail_t = fail_t + 0.01 if raw is None else 0.0
        if fail_t >= FAIL_AFTER:
            if state != 4:
                state = 4
                timeline.append((round(t, 2), STATES[4]))
            continue
        if raw is None:
            continue

        # ---- 下限截断 → 中位数 → EMA ----
        hist.append(max(raw, FLOOR_DB))
        if len(hist) > MEDIAN_WIN:
            hist.pop(0)
        med = sorted(hist)[len(hist) // 2]
        ema = med if ema is None else EMA_ALPHA * med + (1 - EMA_ALPHA) * ema

        if i < boot_n:
            continue

        # ---- 各档独立计时（不因状态切换而清零）----
        for k in (1, 2, 3):
            up[k] = up[k] + 0.01 if ema >= TH[k] else 0.0
        down += 0.01

        # ---- 升级：直达满足条件的最高档（可跳档）----
        target = state
        for k in (1, 2, 3):
            if k > state and up[k] >= DUR_UP[k]:
                target = max(target, k)

        # ---- 回落：每次只降一档，且需迟滞 + 2 秒 ----
        if target == state and down >= DUR_DOWN and state > 0 and ema < TH_BACK[state]:
            target = state - 1

        # ---- 状态提交去抖：目标稳定 100ms 才切换，避免 60ms 的过渡态闪烁 ----
        if target != state:
            if pending != target:
                pending, pending_since = target, t
            elif t - pending_since >= COMMIT_DEBOUNCE:
                state = target
                down = 0.0
                pending = None
                timeline.append((round(t, 2), STATES[state]))
        else:
            pending = None

    return timeline


def states_of(tl):
    return [s for _, s in tl]


B = [(BOOT_QUIET, 0.0)]

CASES = [
    ("1 单点冲击不误报", B + [(1.0, 0.0), (0.2, 20.0), (3.0, 0.0)], ["静音"]),
    ("2 持续 9dB 直达偏吵", B + [(2.0, 9.0)], ["静音", "偏吵"]),
    ("3 只持续 0.5s 不升级", B + [(0.5, 9.0), (3.0, 0.0)], ["静音"]),
    ("4 边界 9/7 抖动不闪烁", B + [(2.0, 9.0)] + [(0.1, 9.0), (0.1, 7.0)] * 10, ["静音", "偏吵"]),
    ("5 逐级升到吵闹", B + [(2.0, 6.0), (2.0, 10.0), (2.0, 20.0)], ["静音", "正常", "偏吵", "吵闹"]),
    ("6 突变直达吵闹、回落逐级", B + [(2.0, 20.0), (3.0, 10.0), (3.0, 7.0), (5.0, 0.0)],
     ["静音", "吵闹", "偏吵", "正常", "静音"]),
    ("7 极静不误判为失效", B + [(6.0, -30.0)], ["静音"]),
    ("8 ADC 贴底 → 安全态", B + [(6.0, None)], ["静音", "安全态(失效)"]),
    ("9 开机静默期生效（10s 内不动）", [(3.0, 25.0), (10.0, 25.0)], ["静音", "吵闹"]),
]


def main():
    passed = failed = 0
    for name, segs, expected in CASES:
        tl = simulate(segs)
        got = states_of(tl)
        ok = got == expected
        print(f"[{'PASS' if ok else 'FAIL'}] {name}")
        print(f"       期望: {' -> '.join(expected)}")
        print(f"       实际: {' -> '.join(got)}")
        print(f"       时刻: {tl}")
        passed, failed = (passed + 1, failed) if ok else (passed, failed + 1)
    print(f"\n通过 {passed} / {len(CASES)}，失败 {failed}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
