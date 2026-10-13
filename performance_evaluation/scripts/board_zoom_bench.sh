#!/bin/sh
#
# board_zoom_bench.sh - 板端固定倍率缩放渲染基准 (1.0x / 1.5x / 2.0x)
#
# 与 board_benchmark.sh 的区别: 那个测采集/图传基线 (capture-only / capture+tcp),
# 这个只测"显示阶段的缩放渲染开销", 三档之间只差 --zoom-fixed 一个变量。
#
# 需要在开发板上执行 (busybox sh 兼容, 不用 bash 特性)。
# 每个用例都带 --duration 与 --io-timeout, 保证一定会结束, 不会挂死。
#
# 用法 (板端):
#     cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15
#     sh board_zoom_bench.sh /dev/video1 192.168.1.66 60
#
# 参数:
#     $1 视频设备节点        (默认 /dev/video1)
#     $2 PC 服务器 IP        (默认 192.168.1.66)
#     $3 每档时长秒数        (默认 60)
#     $4 输出目录            (默认 /lib/modules/4.1.15/logs/camera_sender/zoom)
#
# 前置条件 (缺一不可, 否则数据不可用):
#     1) PC 端 pc_receiver 已在监听 (板端是客户端, 连不上会直接退出):
#        ./bin/pc/pc_receiver -p 5000 --no-save --quiet \
#            --accept-timeout 900 --io-timeout 60 \
#            --stats-dir performance_evaluation/results/pc_zoom
#     2) 板端有 LCD (/dev/fb0): 没有 LCD 时 --zoom-fixed 会被程序忽略,
#        render_* 全是 -1。
#
# 输出:
#     $OUT/zoom_off/  zoom_1.0/  zoom_1.5/  zoom_2.0/
#         capture_frames.csv  process_usage.csv  summary.csv
#     $OUT/probe_format.txt   (真实协商的分辨率/格式)
#
# 注意: 子目录名用的是"点号" (zoom_1.5), 不是下划线。
#

DEV="${1:-/dev/video1}"
PCIP="${2:-192.168.1.66}"
DUR="${3:-60}"
OUT="${4:-/lib/modules/4.1.15/logs/camera_sender/zoom}"
PORT=5000

BIN="$(dirname "$0")/imx6ull_sender"
[ -x "$BIN" ] || BIN="./imx6ull_sender"

echo "=================================================================="
echo " board_zoom_bench.sh"
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

# 缩放只作用于本地显示: 没有 framebuffer 时程序会忽略 --zoom-fixed,
# render_* 列会全是 -1, 这一轮就没有意义了。
if [ ! -e /dev/fb0 ]; then
    echo "[WARN] 没有 /dev/fb0: --zoom-fixed 会被忽略, render_* 将是 -1"
    echo "       仍继续跑, 但这一轮数据不能用于渲染耗时结论。"
fi

mkdir -p "$OUT"

# ------------------------------------------------------------------
# 0. 环境信息 + 格式探测 (必须记录, 否则数据无法复现)
# ------------------------------------------------------------------
echo "--- 环境信息 ---"
uname -a
echo "内核: $(uname -r)"
cat /proc/cpuinfo 2>/dev/null | grep -E 'model name|Processor|BogoMIPS' | head -4

echo "--- 能力/格式探测 (跑 1 帧读启动日志) ---"
"$BIN" -d "$DEV" --capture-only -n 1 2>&1 | tee "$OUT/probe_format.txt"
echo "  -> 完整输出: $OUT/probe_format.txt"

# ------------------------------------------------------------------
# 各档用同一个函数跑: 除 --zoom-fixed 外所有参数保持一致
#   $1 = 输出子目录名, $2 = 传给 --zoom-fixed 的倍率 (空 = 不缩放基线)
# ------------------------------------------------------------------
run_case() {
    _dir="$1"
    _z="$2"
    if [ -n "$_z" ]; then
        _zopt="--zoom-fixed $_z"
        _tag="zoom ${_z}x"
    else
        _zopt=""
        _tag="no-zoom baseline (fit 路径)"
    fi

    echo ""
    echo "=== $_tag ${DUR}s -> $OUT/$_dir ==="
    # 不用 timeout 兜底: BusyBox 的 timeout 只支持 -t/-s, 没有 GNU 的
    # -k/位置参数写法 (实测 "timeout: invalid option -- 'k'" 会让用例
    # 直接空跑)。程序自带 --duration + --io-timeout, 一定会自己结束。
    "$BIN" -d "$DEV" -s "$PCIP" -p "$PORT" -l \
        $_zopt \
        --duration "$DUR" --warmup 30 --quiet --io-timeout 5 \
        --stats-dir "$OUT/$_dir" \
        > "$OUT/$_dir.log" 2>&1
    echo "exit=$? log=$OUT/$_dir.log"

    # 立刻回显本档的关键结果, 免得跑完 4 档才发现某一档没生效
    if [ -f "$OUT/$_dir/summary.csv" ]; then
        echo "  --- $_dir 关键指标 ---"
        awk -F, '
            NR==1 { for (i=1; i<=NF; i++) h[$i]=i; next }
            {
                printf "  采集FPS=%s 显示FPS=%s mbps=%s cpu=%s rss_kb=%s\n", \
                    $h["fps_overall"], $h["display_fps"], $h["payload_mbps"], \
                    $h["cpu_percent_avg"], $h["rss_max_kb"]
                printf "  render mean/p95/p99/max=%s/%s/%s/%s us\n", \
                    $h["render_mean_us"], $h["render_p95_us"], \
                    $h["render_p99_us"], $h["render_max_us"]
                printf "  gesture mean/p99=%s/%s us\n", \
                    $h["gesture_mean_us"], $h["gesture_p99_us"]
                printf "  render_path: fit=%s zoomed=%s frames_rendered=%s zoom_final=%s\n", \
                    $h["fit_render_frames"], $h["zoom_render_frames"], \
                    $h["frames_rendered"], $h["zoom_final"]
            }' "$OUT/$_dir/summary.csv"
    else
        echo "  [WARN] 没有生成 $OUT/$_dir/summary.csv (用例失败或被中断)"
        tail -5 "$OUT/$_dir.log" 2>/dev/null | sed 's/^/      /'
    fi
}

# ------------------------------------------------------------------
# 1. 无缩放基线: 走 fit(无缩放) 渲染路径, 作为渲染耗时的对照
# ------------------------------------------------------------------
run_case "zoom_off" ""

# ------------------------------------------------------------------
# 2. 固定倍率三档: 1.0 / 1.5 / 2.0
#    --zoom-fixed >0 时程序强制走缩放渲染路径 (否则 1.0x 恰好等于 fit 视图,
#    会退化成无缩放路径, 就测不到缩放渲染代码本身)
# ------------------------------------------------------------------
run_case "zoom_1.0" "1.0"
run_case "zoom_1.5" "1.5"
run_case "zoom_2.0" "2.0"

# ------------------------------------------------------------------
# 3. 逐帧渲染路径抽查 (确认 zoomed 路径真的被走到了)
# ------------------------------------------------------------------
echo ""
echo "=== render_path 分布抽查 (0=fit 无缩放, 1=缩放渲染) ==="
for z in 1.0 1.5 2.0; do
    f="$OUT/zoom_$z/capture_frames.csv"
    if [ -f "$f" ]; then
        awk -F, -v z="$z" '
            NR==1 { for (i=1; i<=NF; i++) if ($i=="render_path") c=i; next }
            { if (c) n[$c]++ }
            END {
                printf "  zoom_%sx: ", z
                for (k in n) printf "render_path=%s:%d 帧 ", k, n[k]
                printf "\n"
            }' "$f"
    else
        echo "  zoom_${z}x: [SKIP] 没有 $f"
    fi
done

# ------------------------------------------------------------------
# 4. 资源清理检查
# ------------------------------------------------------------------
echo ""
echo "=== 资源清理检查 ==="
if command -v fuser >/dev/null 2>&1; then
    fuser "$DEV" 2>&1 || echo "  OK: 无人占用 $DEV"
fi
ps 2>/dev/null | grep -E 'imx6ull_sender' | grep -v grep || echo "  OK: 无残留进程"
dmesg 2>/dev/null | tail -6

echo ""
echo "=================================================================="
echo " 采集完成。请把 $OUT/ 整个目录回传 (含所有 csv 与 log)。"
echo ""
echo " PC 侧分析 (注意子目录名是点号, 不是下划线):"
echo "   python3 performance_evaluation/scripts/analyze_stats.py \\"
echo "       /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/logs/camera_sender/zoom/zoom_2.0"
echo " 判读: render_p99_us 与帧间隔量级相当 -> 渲染是瓶颈, 显示帧率会掉"
echo "=================================================================="
