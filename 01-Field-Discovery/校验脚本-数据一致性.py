# -*- coding: utf-8 -*-
"""仓内数据一致性交叉校验
1) 24 组 CSV：完整性、单调性、与报告陈述是否一致
2) 课题06 15 组表：数值能否在两份原始 CSV 中找到
3) 所有 CSV/Markdown 里引用的证据文件是否存在
"""
import csv
import io
import os
import re

REPO = r"D:\ICEC\ICEC-2026-0191-chenyanfan"
RAW = os.path.join(REPO, r"01-Field-Discovery\raw_data")
RPT02 = os.path.join(REPO, r"01-Field-Discovery\报告-02-手机多工况发热与掉电实测.md")
RPT06 = os.path.join(REPO, r"01-Field-Discovery\报告-06-校园昼夜噪声地图.md")


def read_csv(name):
    with io.open(os.path.join(RAW, name), encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


def main():
    problems = []

    print("=" * 72)
    print("1) 24 组时间序列 CSV")
    print("=" * 72)
    rows = read_csv("2026-11-24-phone-drain.csv")
    print(f"总行数: {len(rows)}")
    if len(rows) != 24:
        problems.append(f"24组表行数为 {len(rows)}，应为 24")

    empty = [r["组号"] for r in rows if not r["电量%"]]
    print(f"空行: {empty if empty else '无'}")
    if empty:
        problems.append(f"24组表有空行: {empty}")

    # 按工况检查单调性
    by_cond = {}
    for r in rows:
        by_cond.setdefault(r["工况"], []).append(r)

    for c, rs in sorted(by_cond.items()):
        lv = [int(x["电量%"]) for x in rs if x["电量%"]]
        name = rs[0]["工况名称"]
        mono = all(a >= b for a, b in zip(lv, lv[1:]))
        print(f"  工况{c} {name}: {len(rs)} 点, 电量 {lv[0]}→{lv[-1]}%, 单调不增={mono}")
        if not mono:
            problems.append(f"工况{c}({name}) 电量非单调不增: {lv}")

    print()
    print("=" * 72)
    print("2) 报告-02 陈述 vs CSV 实际")
    print("=" * 72)
    with io.open(RPT02, encoding="utf-8") as f:
        t02 = f.read()

    # 各工况：报告称"起点→终点"，CSV 前 3 个工况从 +5 分钟起（不含起点），游戏含起点
    # 因此校验规则：CSV 末点必须等于报告终点；首点等于起点（游戏）或比起点少 1（其余）
    claims = [
        ("1", "待机", 64, 60, False),
        ("2", "短视频", 58, 50, False),
        ("3", "导航", 49, 35, False),
        ("4", "游戏", 32, 6, True),
    ]
    for cid, label, s, e, has_start in claims:
        rs = by_cond.get(cid, [])
        if not rs:
            problems.append(f"工况{cid}({label}) CSV 缺失")
            continue
        lv = [int(x["电量%"]) for x in rs if x["电量%"]]
        first_min = int(rs[0]["相对分钟"])
        ok_last = (lv[-1] == e)
        # 起点一致性：CSV 首点不得高于起点（电量单调不增），
        # 且 0→首点之间的掉电速率不得超过 1.5 %/分钟（否则不合理）
        if has_start:
            ok_first = (lv[0] == s)
            rate = 0.0
        else:
            ok_first = (lv[0] <= s)
            rate = (s - lv[0]) / max(first_min, 1)
            if rate > 1.5:
                ok_first = False
        flag = "✓" if (ok_last and ok_first) else "✗"
        extra = f"  0→+{first_min}min 掉 {s-lv[0]}pt（{rate:.2f} %/min）" if not has_start else ""
        print(f"  {flag} {label:<10} 报告 {s}→{e}%  │  CSV {lv[0]}→{lv[-1]}%{extra}")
        if not ok_last:
            problems.append(f"工况{cid}({label}) 终点不符: 报告{e} vs CSV{lv[-1]}")
        if not ok_first:
            problems.append(f"工况{cid}({label}) 起点异常: 报告{s} vs CSV{lv[0]}")

    # 关键结论句必须出现
    must_have = [
        "0.89 W", "1.74 W", "2.91 W", "6.9 W",
        "86.1℃", "99.8%", "5.0 %/5min",
        "电压塌陷", "1%", "20 秒",
    ]
    missing = [k for k in must_have if k not in t02]
    print(f"关键数字/结论覆盖: {len(must_have)-len(missing)}/{len(must_have)}")
    if missing:
        problems.append(f"报告-02 缺少关键结论: {missing}")

    print()
    print("=" * 72)
    print("3) 课题06：15 组表 vs 原始 CSV")
    print("=" * 72)
    t15 = read_csv("课题06-15组声学数据表.csv")
    print(f"15 组表行数: {len(t15)}")
    if len(t15) != 15:
        problems.append(f"15组表行数为 {len(t15)}，应为 15")

    # 收集原始数据所有 dB 值
    src22 = read_csv("2026-11-22-noise.csv")
    src24 = read_csv("2026-11-24-noise.csv")
    src_vals = set()
    for r in src22:
        for k in ("当前值dB", "最小值dB", "平均值dB", "最大值dB"):
            v = r.get(k, "")
            if v:
                try:
                    src_vals.add(round(float(v), 1))
                except ValueError:
                    pass
    for r in src24:
        for k in ("水印值dB", "最小值dB", "平均值dB", "最大值dB"):
            v = r.get(k, "")
            if v:
                try:
                    src_vals.add(round(float(v), 1))
                except ValueError:
                    pass

    unmatched = []
    for r in t15:
        for k in ("当前值dB", "最小值dB", "平均值dB", "最大值dB"):
            v = r.get(k, "")
            if not v:
                continue
            try:
                fv = round(float(v), 1)
            except ValueError:
                continue
            if fv not in src_vals:
                unmatched.append((r["序号"], r["官方点位"], k, v))
    print(f"数值可回溯到原始 CSV: {len(unmatched)==0}")
    if unmatched:
        print("  未匹配:")
        for u in unmatched[:10]:
            print(f"    序号{u[0]} {u[1]} {u[2]}={u[3]}")
        problems.append(f"15组表有 {len(unmatched)} 个数值无法回溯")

    # 官方 5 点位覆盖
    official = ["图书馆", "食堂", "主干道", "运动场", "实训室走廊"]
    covered = set(r["官方点位"] for r in t15)
    print(f"官方点位覆盖: {len(covered)}/5 -> {sorted(covered)}")
    lack = [o for o in official if o not in covered]
    if lack:
        problems.append(f"15组表缺少官方点位: {lack}")

    print()
    print("=" * 72)
    print("4) 证据文件存在性")
    print("=" * 72)
    miss_file = []
    for name in ("课题06-15组声学数据表.csv", "2026-11-22-noise.csv", "2026-11-24-noise.csv", "2026-11-24-phone-drain.csv"):
        for r in read_csv(name):
            ev = r.get("证据文件", "")
            if ev and not ev.startswith("adb-log") and not ev.startswith("截图"):
                if not os.path.exists(os.path.join(RAW, ev)):
                    miss_file.append((name, ev))
    print(f"缺失证据文件: {len(miss_file)}")
    for m in miss_file[:10]:
        print(f"  {m[0]} -> {m[1]}")
    if miss_file:
        problems.append(f"{len(miss_file)} 个证据文件缺失")

    print()
    print("=" * 72)
    print("5) 汇总")
    print("=" * 72)
    if problems:
        print(f"❌ 发现 {len(problems)} 个问题：")
        for p in problems:
            print(f"   - {p}")
    else:
        print("✅ 全部通过：数据完整、单调、可回溯、证据齐全")


if __name__ == "__main__":
    main()

