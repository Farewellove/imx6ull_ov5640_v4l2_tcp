#!/bin/sh
#
# pc_bench_serve.sh - PC 端按用例顺序逐个起 pc_receiver
#
# 为什么需要它:
#   pc_receiver 一次只服务一个客户端 (accept 一次, 收完就退出)。
#   而板端 board_benchmark.sh 有 5 个带网络的用例, 只起一个接收端的话
#   后面几个用例全部 "Connection refused" (上一轮实测就是这么废掉的)。
#   这里逐个用例各起一次, 每个用例一份独立的 stats 目录, 方便对照。
#
# 用法 (PC 端, 在项目根目录):
#   sh performance_evaluation/scripts/pc_bench_serve.sh pc_bench \
#       capture_tcp capture_tcp_display longrun exp_buf6 exp_fps15
#
#   sh performance_evaluation/scripts/pc_bench_serve.sh pc_zoom \
#       zoom_off zoom_1.0 zoom_1.5 zoom_2.0
#
# 参数:
#   $1   stats 目录名 (performance_evaluation/results/<$1>/)
#   $2.. 用例名列表, 顺序必须与板端脚本的用例顺序一致
#
# 环境变量: PORT (默认 5000), ACCEPT_TIMEOUT (默认 200 秒)
#
# 注意: 板端第一个用例 capture-only 不发网络, 这一轮 PC 端会空等到
#       板端第二个用例连上, 属正常 (ACCEPT_TIMEOUT 要大于单用例时长)。
#

PREFIX="${1:-pc_bench}"
[ $# -gt 0 ] && shift

BIN=./bin/pc/pc_receiver
PORT="${PORT:-5000}"
ACCEPT_TIMEOUT="${ACCEPT_TIMEOUT:-200}"

# 没给用例名时的默认列表 (与 board_benchmark.sh 的带网络用例顺序一致)
if [ $# -eq 0 ]; then
    set -- capture_tcp capture_tcp_display longrun exp_buf6 exp_fps15
fi

if [ ! -x "$BIN" ]; then
    echo "[ERROR] 找不到 $BIN (请先 make pc)"
    exit 1
fi

echo "=================================================================="
echo " pc_bench_serve.sh"
echo "   bin        : $BIN"
echo "   port       : $PORT"
echo "   accept     : ${ACCEPT_TIMEOUT}s per case"
echo "   stats root : performance_evaluation/results/$PREFIX/"
echo "   cases      : $*"
echo "=================================================================="

rc=0
for c in "$@"; do
    echo ""
    echo "=== [PC] waiting for case: $c ==="
    "$BIN" -p "$PORT" --no-save --quiet \
        --accept-timeout "$ACCEPT_TIMEOUT" --io-timeout 60 \
        --stats-dir "performance_evaluation/results/$PREFIX/$c"
    ec=$?
    echo "--- case $c finished (exit=$ec) ---"
    [ "$ec" -ne 0 ] && rc=$ec
done

echo ""
echo "=================================================================="
echo " 全部用例结束 (rc=$rc)。stats 在 performance_evaluation/results/$PREFIX/"
echo "=================================================================="
exit $rc
