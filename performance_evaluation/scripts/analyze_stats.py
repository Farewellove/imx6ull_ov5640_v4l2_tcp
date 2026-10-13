#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze_stats.py - 从收集到的 CSV 计算可复测的性能指标

只依赖 Python 标准库 (无 numpy/pandas), 保证在板端 busybox 环境或任意 PC 上
都能跑。所有输出都带数据来源与样本数, 不做任何插值或平滑 —— 报告里出现的
每个数字都必须能在这份脚本的输出里找到。

输入 (由 --stats-dir 生成):
    板端: capture_frames.csv, process_usage.csv, summary.csv
    PC  : network_stats.csv, process_usage.csv, summary.csv

用法:
    python3 analyze_stats.py <stats_dir> [--mode capture-only|capture+tcp]
    python3 analyze_stats.py --compare <board_dir> <pc_dir>

设计说明 (为什么不用平均值糊过去):
    - 帧率有两个口径: 全程平均 fps 和 稳态 fps (剔除预热)。
    - 抖动必须看 P95/P99, 平均值会掩盖偶发卡顿。
    - 吞吐 (Mbps) 只算有效 payload, 并单独给出是否含帧头, 避免两种口径混用。
    - 分位数用最近秩法 (与 C 端 metrics.c 完全一致), 保证脚本与程序结论一致。
"""

import csv
import os
import sys
import math


# ---------------------------------------------------------------- 工具
def read_csv(path):
    if not os.path.isfile(path):
        return None
    with open(path, "r", newline="") as f:
        r = csv.reader(f)
        rows = [row for row in r]
    if not rows:
        return None
    header = rows[0]
    out = []
    for row in rows[1:]:
        if not row or all(c.strip() == "" for c in row):
            continue
        out.append(dict(zip(header, row)))
    return {"header": header, "rows": out}


def as_float(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def as_int(v, default=None):
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return default


def percentile_nearest_rank(sorted_vals, p):
    """最近秩法, 与 src/metrics.c 的 metrics_summary 口径一致"""
    n = len(sorted_vals)
    if n == 0:
        return None
    idx = int(math.ceil(p / 100.0 * n))
    if idx < 1:
        idx = 1
    if idx > n:
        idx = n
    return sorted_vals[idx - 1]


def stats_us(vals):
    if not vals:
        return None
    s = sorted(vals)
    n = len(s)
    mean = sum(s) / float(n)
    var = sum((x - mean) ** 2 for x in s) / float(n)
    return {
        "n": n,
        "min": s[0],
        "mean": mean,
        "p50": percentile_nearest_rank(s, 50),
        "p95": percentile_nearest_rank(s, 95),
        "p99": percentile_nearest_rank(s, 99),
        "max": s[-1],
        "stddev": math.sqrt(var),
    }


def fmt(v, unit="", nd=2):
    if v is None:
        return "NA"
    return ("%%.%df%s" % (nd, unit)) % v


def fmt_us(v):
    if v is None:
        return "NA"
    if v >= 1000.0:
        return "%.2f ms" % (v / 1000.0)
    return "%.1f us" % v


# ---------------------------------------------------------------- 板端
def analyze_board(d, label="board"):
    print("=" * 72)
    print("板端采集统计: %s" % d)
    print("=" * 72)

    fr = read_csv(os.path.join(d, "capture_frames.csv"))
    su = read_csv(os.path.join(d, "summary.csv"))
    pu = read_csv(os.path.join(d, "process_usage.csv"))

    if fr is None:
        print("  [SKIP] 没有 capture_frames.csv (可能未开启 --stats-dir, 或未上板运行)")
        return None

    rows = fr["rows"]
    # 只统计非预热帧 (warmup=0)
    warm = [r for r in rows if as_int(r.get("warmup"), 1) == 1]
    body = [r for r in rows if as_int(r.get("warmup"), 1) == 0]

    print("  总行数(含预热)   : %d" % len(rows))
    print("  预热帧           : %d" % len(warm))
    print("  统计窗口帧数     : %d" % len(body))
    if not body:
        print("  [WARN] 统计窗口为空, 无法给分位数 (检查 --warmup 是否过大)")
        return None

    # 帧间隔: 用 t1_ns 差 (与 C 端口径一致), 但 CSV 里没有直接列 t1 差。
    # 用相邻行 t1_ns 差重算一遍, 作为独立校验。
    iv = []
    prev = None
    for r in body:
        t1 = as_int(r.get("t1_ns"))
        if t1 is None:
            continue
        if prev is not None and t1 > prev:
            iv.append((t1 - prev) / 1000.0)   # us
        prev = t1

    # 全程 fps 用首末 t1
    t_first = as_int(body[0].get("t1_ns"))
    t_last = as_int(body[-1].get("t1_ns"))
    elapsed_s = (t_last - t_first) / 1e9 if (t_first and t_last and t_last > t_first) else None
    fps_window = (len(body) / elapsed_s) if elapsed_s else None

    # 也读 summary.csv 里的官方数值做交叉核对
    s = None
    if su and su["rows"]:
        s = su["rows"][0]

    print()
    print("  --- 帧率 ---")
    if s:
        print("  mode             : %s" % s.get("mode", "NA"))
        print("  elapsed_s        : %s" % s.get("elapsed_s", "NA"))
        print("  frames_ok        : %s" % s.get("frames_ok", "NA"))
        print("  fps_overall      : %s   (程序口径: frames_ok/实测时长)" % s.get("fps_overall", "NA"))
    if fps_window:
        print("  稳态 fps (本脚本): %.3f   (统计窗口 %d 帧 / %.3f s)" % (fps_window, len(body), elapsed_s))

    print()
    print("  --- 帧间隔抖动 (由 t1_ns 重算, 与 metrics.c 独立核对) ---")
    st = stats_us(iv)
    if st:
        print("  n=%d  mean=%s  p50=%s  p95=%s  p99=%s  max=%s  stddev=%s" % (
            st["n"], fmt_us(st["mean"]), fmt_us(st["p50"]), fmt_us(st["p95"]),
            fmt_us(st["p99"]), fmt_us(st["max"]), fmt_us(st["stddev"])))
        if s:
            print("  程序汇总口径: interval_mean=%s p95=%s p99=%s max=%s (us)" % (
                s.get("interval_mean_us", "NA"), s.get("interval_p95_us", "NA"),
                s.get("interval_p99_us", "NA"), s.get("interval_max_us", "NA")))

    print()
    print("  --- 正确性/可靠性计数器 ---")
    if s:
        for k in ("poll_timeout", "dqbuf_eagain", "dqbuf_eintr", "dqbuf_error",
                  "buf_flag_error", "seq_gap_events", "seq_gap_frames",
                  "bytesused_min", "bytesused_max",
                  "frames_sent", "frames_send_fail",
                  "payload_bytes", "header_bytes", "payload_mbps"):
            if k in s:
                print("  %-16s : %s" % (k, s[k]))
        print("  注: seq_gap_frames 是驱动 sequence 语义, 不等于硬件丢帧率")

    print()
    print("  --- 板内阶段耗时 (CLOCK_MONOTONIC, 不是端到端延迟) ---")
    if s:
        for k in ("send_mean_us", "send_p99_us", "t3t1_mean_us", "t3t1_p99_us"):
            if k in s:
                print("  %-16s : %s us" % (k, s[k]))
    # 用逐帧 CSV 重算 lcd/send
    for col, name in (("lcd_us", "lcd_copy(T2-T1)"), ("send_us", "send(T3-T2)"),
                      ("t3t1_us", "onboard(T3-T1)")):
        vals = [as_float(r.get(col)) for r in body]
        vals = [v for v in vals if v is not None]
        if vals and max(vals) > 0:
            stx = stats_us(vals)
            print("  %-16s : mean=%s p95=%s p99=%s max=%s (n=%d)" % (
                name, fmt_us(stx["mean"]), fmt_us(stx["p95"]),
                fmt_us(stx["p99"]), fmt_us(stx["max"]), stx["n"]))

    print()
    print("  --- 进程资源 (采样自 /proc/<pid>/{stat,status}) ---")
    if s:
        print("  cpu_percent_avg  : %s (%% of one core)" % s.get("cpu_percent_avg", "NA"))
        print("  rss_max_kb       : %s" % s.get("rss_max_kb", "NA"))
    if pu and pu["rows"]:
        rss = [as_float(r.get("rss_kb")) for r in pu["rows"]]
        rss = [x for x in rss if x is not None]
        cpu = [as_float(r.get("cpu_percent")) for r in pu["rows"]]
        cpu = [x for x in cpu if x is not None]
        print("  samples          : %d" % len(pu["rows"]))
        if rss:
            print("  rss 首/末/峰值   : %.0f / %.0f / %.0f KB  (增长 %.0f KB)" % (
                rss[0], rss[-1], max(rss), rss[-1] - rss[0]))
        if cpu:
            print("  cpu mean/max     : %.1f%% / %.1f%%" % (sum(cpu) / len(cpu), max(cpu)))
    else:
        print("  [SKIP] 没有 process_usage.csv")
    return s


# ---------------------------------------------------------------- PC
def analyze_pc(d):
    print("=" * 72)
    print("PC 接收统计: %s" % d)
    print("=" * 72)

    nf = read_csv(os.path.join(d, "network_stats.csv"))
    su = read_csv(os.path.join(d, "summary.csv"))
    pu = read_csv(os.path.join(d, "process_usage.csv"))

    if nf is None:
        print("  [SKIP] 没有 network_stats.csv")
        return None

    rows = nf["rows"]
    body = rows
    s = su["rows"][0] if (su and su["rows"]) else None

    print("  完整重组帧数     : %d" % len(body))
    if not body:
        print("  [WARN] 没有收到完整帧")
        return s

    print()
    print("  --- 吞吐 ---")
    if s:
        print("  elapsed_s        : %s" % s.get("elapsed_s", "NA"))
        print("  payload_bytes    : %s" % s.get("payload_bytes", "NA"))
        print("  header_bytes     : %s" % s.get("header_bytes", "NA"))
        print("  payload_mbps     : %s   (只含 payload, 不含帧头)" % s.get("payload_mbps", "NA"))
        pb = as_float(s.get("payload_bytes"))
        hb = as_float(s.get("header_bytes"))
        el = as_float(s.get("elapsed_s"))
        if pb is not None and hb is not None and el:
            print("  payload+hdr Mbps : %.3f   (含帧头的另一种口径, 勿与上一行混用)"
                  % ((pb + hb) * 8.0 / el / 1e6))

    print()
    print("  --- 完整性 / 错误计数 ---")
    if s:
        for k in ("frames_ok", "frames_written", "bad_magic", "bad_version",
                  "bad_hdr_size", "bad_size", "bad_geometry", "session_end",
                  "frame_id_gap_events", "frame_id_gap_frames",
                  "recv_timeouts"):
            if k in s:
                print("  %-20s : %s" % (k, s[k]))
        print("  注: frame_id 缺口 = 发送端未完成整帧协议发送的帧数, 不是硬件丢帧")
        print("  注: 协议无 CRC, 因此只能声称'协议级完整重组 N 帧', 不能声称'内容 100%% 正确'")

    print()
    print("  --- 短读重组证据 (TCP 无消息边界) ---")
    if s:
        dc = as_int(s.get("data_recv_calls"))
        dp = as_int(s.get("data_recv_partial"))
        hc = as_int(s.get("hdr_recv_calls"))
        print("  hdr  recv: calls=%s partial=%s" % (hc, s.get("hdr_recv_partial")))
        print("  data recv: calls=%s partial=%s" % (dc, dp))
        if dc and len(body):
            print("  平均每帧 data recv 次数: %.1f  (>=1 即说明发生了跨调用重组)"
                  % (dc / float(len(body))))

    print()
    print("  --- PC 内部耗时 (本机单调钟, 与板端不可比) ---")
    for col, name in (("reasm_us", "reasm(R2-R1)"),
                      ("payload_reasm_us", "wait+reasm(R2-R1pre)"),
                      ("sdl_us", "sdl(R3-R2)")):
        vals = [as_float(r.get(col)) for r in body]
        vals = [v for v in vals if v is not None]
        if vals:
            stx = stats_us(vals)
            print("  %-22s : mean=%s p95=%s p99=%s max=%s (n=%d)" % (
                name, fmt_us(stx["mean"]), fmt_us(stx["p95"]),
                fmt_us(stx["p99"]), fmt_us(stx["max"]), stx["n"]))

    iv = []
    prev = None
    for r in body:
        r2 = as_int(r.get("r2_ns"))
        if r2 is None:
            continue
        if prev is not None and r2 > prev:
            iv.append((r2 - prev) / 1000.0)
        prev = r2
    if iv:
        stx = stats_us(iv)
        print("  %-22s : mean=%s p95=%s p99=%s max=%s" % (
            "帧间隔(R2差)", fmt_us(stx["mean"]), fmt_us(stx["p95"]),
            fmt_us(stx["p99"]), fmt_us(stx["max"])))

    print()
    print("  --- 进程资源 ---")
    if s:
        print("  cpu_percent_avg  : %s (%% of one core; 由运行首尾两次采样做差得到)"
              % s.get("cpu_percent_avg", "NA"))
        print("  rss_max_kb       : %s" % s.get("rss_max_kb", "NA"))
    if pu and pu["rows"]:
        rss = [as_float(r.get("rss_kb")) for r in pu["rows"]]
        rss = [x for x in rss if x is not None]
        if rss:
            print("  周期采样 (%d 点): rss 首/末/峰值 = %.0f / %.0f / %.0f KB" % (
                len(pu["rows"]), rss[0], rss[-1], max(rss)))
    else:
        print("  注: 运行时长不足一个采样间隔时没有周期采样点, "
              "CPU/RSS 只来自首尾两次采样 (仍然有效)")
    return s


# ---------------------------------------------------------------- main
def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    if argv[1] == "--compare":
        if len(argv) < 4:
            print("用法: analyze_stats.py --compare <board_dir> <pc_dir>")
            return 2
        b = analyze_board(argv[2])
        print()
        p = analyze_pc(argv[3])
        print()
        print("=" * 72)
        print("对照结论 (只列有数据支持的部分)")
        print("=" * 72)
        if b and p:
            print("  板端 frames_ok=%s / PC frames_ok=%s" % (b.get("frames_ok"), p.get("frames_ok")))
            bo = as_float(b.get("fps_overall"))
            pf = as_float(p.get("frames_ok"))
            pel = as_float(p.get("elapsed_s"))
            if bo and pf and pel:
                print("  板端全程 fps=%.3f  vs  PC 接收 fps=%.3f" % (bo, pf / pel))
                print("  -> 两者差值反映的是网络/协议层完成率, 不是硬件丢帧率")
            print("  板端 payload_mbps=%s  vs  PC payload_mbps=%s" % (
                b.get("payload_mbps"), p.get("payload_mbps")))
        else:
            print("  [INCOMPLETE] 有一侧缺数据, 不做任何结论")
        return 0

    d = argv[1]
    if os.path.isfile(os.path.join(d, "capture_frames.csv")):
        analyze_board(d)
    elif os.path.isfile(os.path.join(d, "network_stats.csv")):
        analyze_pc(d)
    else:
        print("目录 %s 里既没有 capture_frames.csv 也没有 network_stats.csv" % d)
        return 2

    print()
    print("提示: 用 --compare <board_dir> <pc_dir> 生成两侧对照")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
