#!/bin/sh
#
# board_zoom_bench.sh - 板端双指手势缩放性能矩阵 (在 i.MX6ULL 上运行)
#
# 目的: 取得 1.0x / 1.5x / 2.0x 三档固定倍率下的**真实板端**数据:
#         采集 FPS、显示 FPS、缩放渲染耗时 (mean/P95/P99/max)、
#         进程 CPU/RSS、以及是否出现丢帧/越界异常。
#
# 为什么用 --zoom-fixed 而不是人手做手势:
#   --zoom-fixed 让程序不读触摸, 直接固定在某一倍率并**强制走缩放渲染路径**,
#   于是三档之间只差一个变量 (倍率), 数据才可比。手势模式的次数与时机由人
#   决定, 无法做定长对比, 只能作为"功能是否正常"的定性验证。
#
# 用法 (板端, deploy 后):
#     sh board_zoom_bench.sh /dev/video1 192.168.1.66 60 ./bench
#
#   $1 视频设备节点   (默认 /dev/video1)
#   $2 PC 服务器 IP   (默认 192.168.1.66)
#   $3 每档时长(秒)   (默认 60)
#   $4 输出目录       (默认 ./bench)
#
# 运行前请先在 PC 端起接收:
#   ./bin/pc/pc_receiver -p 5000 --no-save --quiet \
#       --accept-timeout 900 --io-timeout 60 \
#       --stats-dir performance_evaluation/results/pc_zoom
#

DEV="${1:-/dev/video1}"
PCIP="${2:-192.168.1.66}"
DUR="${3:-60}"
OUT="${4:-./bench}"
PORT=5000

BIN="$(dirname "$0")/imx6ull_sender"
[ -x "$BIN" ] || BIN="./imx6ull_sender"

echo "=================================================================="
echo " board_zoom_bench.sh"
echo "   device   : $DEV"
echo "   pc       : $PCIP:$PORT"
echo "   duration : ${DUR}s per case"
echo "   outdir   : $OUT"
echo "=================================================================="

if [ ! -x "$BIN" ]; then
    echo "[ERROR] 找不到可执行文件 $BIN"
    exit 1
fi

mkdir -p "$OUT"

# ------------------------------------------------------------------
# 0. 环境与触摸设备确认 (不确认就测, 数据无法解释)
# ------------------------------------------------------------------
echo "--- 环境 ---"
uname -a
echo "--- framebuffer 实际格式 (决定 16 还是 32bpp 路径) ---"
if [ -r /sys/class/graphics/fb0/virtual_size ]; then
    echo "  virtual_size: $(cat /sys/class/graphics/fb0/virtual_size)"
    echo "  bits_per_pixel: $(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null || echo '?')"
    echo "  stride: $(cat /sys/class/graphics/fb0/stride 2>/dev/null || echo '?')"
fi
echo "--- input 设备 ---"
ls -l /dev/input/ 2>/dev/null
echo "  提示: GT911 的 name 应为 \"GT911 Touchscreen\" (BUS_I2C)"
if [ -r /proc/bus/input/devices ]; then
    grep -A3 -i 'gt911\|touch' /proc/bus/input/devices 2>/dev/null | head -20
fi

# ------------------------------------------------------------------
# 1/2/3. 固定倍率矩阵: 1.0x / 1.5x / 2.0x
#        每档只改倍率, 其余 (分辨率/格式/时长/预热/是否 LCD/是否 TCP) 完全一致
# ------------------------------------------------------------------
for Z in 1.0 1.5 2.0; do
    echo ""
    echo "=== [zoom ${Z}x] 采集+发送+LCD+缩放 ${DUR}s ==="
    "$BIN" -d "$DEV" -s "$PCIP" -p "$PORT" -l \
        --zoom-fixed "$Z" \
        --duration "$DUR" --warmup 30 --quiet --io-timeout 5 \
        --stats-dir "$OUT/zoom_${Z}" \
        > "$OUT/zoom_${Z}.log" 2>&1
    echo "exit=$? log=$OUT/zoom_${Z}.log"
    echo "--- 关键行 ---"
    grep -E 'FB\] Screen|ZOOM\] fixed|display_fps|frames_rendered|render\(zoom\)|gesture|seq_gap|poll_timeout' \
        "$OUT/zoom_${Z}.log" | sed 's/^/    /'
done

# ------------------------------------------------------------------
# 4. 对照组: 无缩放 (改造前的显示路径), 用于确认"1.0x 没有退化"
# ------------------------------------------------------------------
echo ""
echo "=== [baseline] 采集+发送+LCD, 不启用缩放 ${DUR}s ==="
"$BIN" -d "$DEV" -s "$PCIP" -p "$PORT" -l \
    --duration "$DUR" --warmup 30 --quiet --io-timeout 5 \
    --stats-dir "$OUT/baseline_noscale" \
    > "$OUT/baseline_noscale.log" 2>&1
echo "exit=$? log=$OUT/baseline_noscale.log"
grep -E 'FB\] Screen|display_fps|lcd_stage|render' "$OUT/baseline_noscale.log" | sed 's/^/    /'

# ------------------------------------------------------------------
# 5. 手势模式 (定性): 人工做张开/合拢/平移/三指复位, 看计数与是否跟手
# ------------------------------------------------------------------
echo ""
echo "=== [gesture] 手势模式 (人工操作) ==="
echo "    手动执行 (观察屏幕, 依次做: 双指张开 -> 合拢 -> 平移 -> 三指复位):"
echo "      $BIN -d $DEV -s $PCIP -p $PORT -l --touch \\"
echo "          --zoom-min 1.0 --zoom-max 2.0 --quiet --io-timeout 5 \\"
echo "          --stats-dir $OUT/gesture"
echo "    判读: ① 张开变大/合拢变小, 到 1.0x 不再变小"
echo "          ② 缩放围绕双指中心, 画面不跑飞"
echo "          ③ 双指平移画面跟随"
echo "          ④ 三指复位回 1.0x 居中"
echo "          ⑤ 退出时的 'pinch_out/pinch_in/pan_frames/resets' 计数与操作次数吻合"
echo "          ⑥ 若方向不对, 试 --touch-swap-xy / --touch-invert-x / --touch-invert-y"

# ------------------------------------------------------------------
# 6. 资源清理检查
# ------------------------------------------------------------------
echo ""
echo "=== 资源清理检查 ==="
ps 2>/dev/null | grep -E 'imx6ull_sender' | grep -v grep || echo "  OK: 无残留进程"
if command -v fuser >/dev/null 2>&1; then
    fuser "$DEV" 2>&1 || echo "  OK: 无人占用 $DEV"
fi
echo "--- dmesg 尾部 (看 input / csi 是否报错) ---"
dmesg 2>/dev/null | tail -10

echo ""
echo "=================================================================="
echo " 采集完成。请回传:"
echo "   $OUT/ 整个目录 (含 summary.csv / process_usage.csv / capture_frames.csv)"
echo "   各档启动日志中的 '[FB] Screen: ...' 与 '[ZOOM] fixed zoom' 行"
echo "   PC 端 results/pc_zoom/ 目录"
echo "=================================================================="
