#!/bin/sh
#
# board_benchmark.sh - 板端基准测试矩阵 (在 i.MX6ULL 上运行)
#
# 需要在开发板上执行 (busybox sh 兼容, 不用 bash 特性)。
# 每个用例都带 --duration 与 --io-timeout, 保证一定会结束, 不会挂死。
#
# 用法 (板端):
#     cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15
#     sh board_benchmark.sh /dev/video1 192.168.1.66 60
#
# 参数:
#     $1 视频设备节点        (默认 /dev/video1)
#     $2 PC 服务器 IP        (默认 192.168.1.66)
#     $3 每用例时长秒数      (默认 60)
#     $4 输出目录            (默认 /lib/modules/4.1.15/logs/camera_sender/bench)
#
# 所有日志/CSV 统一落在 /lib/modules/4.1.15/logs/<项目名>/ 下, 不再散在 4.1.15 根目录。
#

DEV="${1:-/dev/video1}"
PCIP="${2:-192.168.1.66}"
DUR="${3:-60}"
OUT="${4:-/lib/modules/4.1.15/logs/camera_sender/bench}"
PORT=5000

BIN="$(dirname "$0")/imx6ull_sender"
[ -x "$BIN" ] || BIN="./imx6ull_sender"

echo "=================================================================="
echo " board_benchmark.sh"
echo "   device   : $DEV"
echo "   pc       : $PCIP:$PORT"
echo "   duration : ${DUR}s per case"
echo "   outdir   : $OUT"
echo "   bin      : $BIN"
echo "=================================================================="

if [ ! -x "$BIN" ]; then
    echo "[ERROR] 找不到可执行文件 $BIN"
    exit 1
fi

mkdir -p "$OUT"

# ------------------------------------------------------------------
# 0. 环境信息 (必须记录, 否则数据无法复现)
# ------------------------------------------------------------------
echo "--- 环境信息 ---"
uname -a
cat /proc/cpuinfo | grep -E 'model name|Processor|BogoMIPS' | head -4
echo "内核: $(uname -r)"
echo "网卡:"
if command -v ip >/dev/null 2>&1; then
    ip -o link show 2>/dev/null | head -5
fi
if command -v ethtool >/dev/null 2>&1; then
    for n in eth0 eth1; do
        ethtool "$n" 2>/dev/null | grep -E 'Speed|Duplex' | sed "s/^/  $n: /"
    done
else
    echo "  (无 ethtool, 请手动确认链路速率: 100Mbps vs 10Mbps)"
fi

# 链路速率探测 (用 ping 粗判, 只作参考)
if command -v ping >/dev/null 2>&1; then
    echo "--- ping $PCIP (4 次) ---"
    ping -c 4 "$PCIP" 2>&1 | tail -3
fi

# 摄像头能力 / 格式协商结果的唯一依据:
# 跑 1 帧, 从 imx6ull_sender 的启动日志读 S_FMT / REQBUFS / G_PARM 的实际返回值。
# (项目自带的 v4l2_info / v4l2_format_test 已于 2026-10-13 删除)
echo "--- 能力/格式探测 (跑 1 帧读启动日志) ---"
if [ -x "$BIN" ]; then
    "$BIN" -d "$DEV" --capture-only -n 1 2>&1 | tee "$OUT/probe_format.txt"
    echo "  -> 完整输出: $OUT/probe_format.txt"
else
    echo "[WARN] 找不到 $BIN, 跳过能力探测 (强烈建议先手动跑一次)"
fi

# ------------------------------------------------------------------
# 1. capture-only: 纯采集, 不碰网络 (帧率上限基准)
# ------------------------------------------------------------------
echo ""
echo "=== [1/4] capture-only ${DUR}s ==="
"$BIN" -d "$DEV" --capture-only \
    --duration "$DUR" --warmup 30 --quiet \
    --stats-dir "$OUT/capture_only" \
    > "$OUT/capture_only.log" 2>&1
echo "exit=$? log=$OUT/capture_only.log"

# ------------------------------------------------------------------
# 2. capture+tcp: 采集 + 发送 (默认 YUYV; 若 S_FMT 不接受请换 -f)
# ------------------------------------------------------------------
echo ""
echo "=== [2/4] capture+tcp ${DUR}s (YUYV) ==="
"$BIN" -d "$DEV" -s "$PCIP" -p "$PORT" \
    --duration "$DUR" --warmup 30 --quiet --io-timeout 5 \
    --stats-dir "$OUT/capture_tcp" \
    > "$OUT/capture_tcp.log" 2>&1
echo "exit=$? log=$OUT/capture_tcp.log"

# ------------------------------------------------------------------
# 3. capture+tcp+lcd: 再加本地 LCD 显示, 看显示开销
# ------------------------------------------------------------------
echo ""
echo "=== [3/4] capture+tcp+display ${DUR}s ==="
"$BIN" -d "$DEV" -s "$PCIP" -p "$PORT" -l \
    --duration "$DUR" --warmup 30 --quiet --io-timeout 5 \
    --stats-dir "$OUT/capture_tcp_display" \
    > "$OUT/capture_tcp_display.log" 2>&1
echo "exit=$? log=$OUT/capture_tcp_display.log"

# ------------------------------------------------------------------
# 4. 长时稳定性 (可选, 时间预算足够时打开)
#    检查帧率衰减 / RSS 增长 / fd 泄漏
# ------------------------------------------------------------------
LONG_DUR=$((DUR * 5))
echo ""
echo "=== [4/4] 长时稳定性 ${LONG_DUR}s (capture+tcp) ==="
echo "     (若时间不够, 可 Ctrl+C 中止; 已采集的部分仍然有效)"
FD_BEFORE=$(ls /proc/$$/fd 2>/dev/null | wc -l)
"$BIN" -d "$DEV" -s "$PCIP" -p "$PORT" \
    --duration "$LONG_DUR" --warmup 60 --quiet --io-timeout 5 \
    --stats-dir "$OUT/longrun" \
    > "$OUT/longrun.log" 2>&1
echo "exit=$? log=$OUT/longrun.log"

# ------------------------------------------------------------------
# 5. 对照实验 (每次只改一个变量)
#
#    5a: 缓冲数量 3 vs 6  -- 验证"串行循环下缓冲不足导致溢出"的假设
#    5b: 帧率 15fps       -- 验证驱动是否接受 S_PARM
#    5c: LCD R/B 交换     -- 目视对比 (需接屏且人工观察, 故只打印提示)
# ------------------------------------------------------------------
EXP_DUR=$((DUR / 2))
[ "$EXP_DUR" -lt 10 ] && EXP_DUR=10

echo ""
echo "=== [5a] 对照实验: buffers=6 (基线见 [2] 的 buffers=3) ${EXP_DUR}s ==="
"$BIN" -d "$DEV" -s "$PCIP" -p "$PORT" --buffers 6 \
    --duration "$EXP_DUR" --warmup 30 --quiet --io-timeout 5 \
    --stats-dir "$OUT/exp_buf6" \
    > "$OUT/exp_buf6.log" 2>&1
echo "exit=$? log=$OUT/exp_buf6.log"
echo "    判读: 与 [2] 比较 seq_gap_events / poll_timeout / fps_overall"
echo "          明显改善 -> 缓冲不足; 几乎无差别 -> 发送耗时本身超过帧周期"

echo ""
echo "=== [5b] 对照实验: --set-fps 15 ${EXP_DUR}s ==="
"$BIN" -d "$DEV" -s "$PCIP" -p "$PORT" --set-fps 15 \
    --duration "$EXP_DUR" --warmup 30 --quiet --io-timeout 5 \
    --stats-dir "$OUT/exp_fps15" \
    > "$OUT/exp_fps15.log" 2>&1
echo "exit=$? log=$OUT/exp_fps15.log"
echo "    判读 (关键, 决定简历怎么写):"
grep -E 'S_PARM|timeperframe|TIMEPERFRAME' "$OUT/exp_fps15.log" | sed 's/^/      /'
echo "      - G_PARM 读回 15fps 且实测 FPS≈15 -> 可写'支持帧率协商'"
echo "      - G_PARM 仍是默认值               -> 只能写'实测帧率', 不能写'支持帧率配置'"

echo ""
echo "=== [5c] 对照实验: LCD R/B 交换 (需目视) ==="
echo "    手工执行并观察屏幕颜色 (两次都接 LCD):"
echo "      A) $BIN -d $DEV -s $PCIP -l -n 300"
echo "      B) $BIN -d $DEV -s $PCIP -l --lcd-swap-rb -n 300"
echo "    判读: A 偏蓝绿且 B 正常 -> 确认 CSI R/B 交换"
echo "          A 正常且 B 偏蓝绿 -> 该通路无 R/B 交换, 需修正 README 结论"

# ------------------------------------------------------------------
# 6. 资源清理检查
# ------------------------------------------------------------------
echo ""
echo "=== 资源清理检查 ==="
echo "--- /dev/video* 是否仍被占用 (应为空) ---"
if command -v fuser >/dev/null 2>&1; then
    fuser "$DEV" 2>&1 || echo "  OK: 无人占用 $DEV"
else
    echo "  (无 fuser; 请手动确认程序已退出: ps | grep imx6ull_sender)"
fi
echo "--- 残留进程 (应为空) ---"
ps 2>/dev/null | grep -E 'imx6ull_sender' | grep -v grep || echo "  OK: 无残留进程"
echo "--- dmesg 尾部 (看 CSI 驱动是否有报错) ---"
dmesg 2>/dev/null | tail -8

echo ""
echo "=================================================================="
echo " 采集完成。请把以下内容回传:"
echo "   $OUT/ 整个目录 (含所有 csv 与 log)"
echo "   $OUT/probe_format.txt (真实协商的格式/分辨率)"
echo "   dmesg 尾部输出"
echo "=================================================================="
