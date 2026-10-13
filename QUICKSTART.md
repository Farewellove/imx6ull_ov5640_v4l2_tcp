# QUICKSTART — 可直接复制粘贴

> 纯命令，按顺序执行。**PC IP 已固定为 `192.168.1.66`**（网卡 `enp8s0`）。
> 需要解释时看 `performance_evaluation/USAGE.md`；总入口 `performance_evaluation/README.md`。
>
> **协议为 v2**：板端与 PC 端必须同时重编重装，否则 PC 端报 `bad_version`。

---

## 0. 一次性准备（PC）

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && make clean && timeout 600 make allboth && timeout 300 make install && cp performance_evaluation/scripts/board_benchmark.sh performance_evaluation/scripts/board_zoom_bench.sh /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/ && echo "=== BUILD + INSTALL OK (上面输出里若有 warning 要处理) ==="
```

只编不装（快速检查）：

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && make clean && timeout 600 make allboth
```

> 这一步没有用 `tee`/管道，是为了让构建失败能中断后续命令（管道会把 make 的退出码吞掉）。
> 想留日志就把 `> performance_evaluation/logs/build_after.log 2>&1` 加在 `make allboth` 之后。

---

## 1. 板端确认（必做：记下 4 个值）

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ls -l /dev/video* && ping -c 2 192.168.1.66 && ./imx6ull_sender -d /dev/video1 --capture-only -n 1
```

从输出抄下：`S_FMT` 实际 `width/height/pixfmt`、`nbufs actual`、`timeperframe`、`[FB] Screen: ...bpp=`。

若报 `S_FMT returned XXX, but YYY was requested`，改用 `-f` 适配（见文末速查）。

---

## 2. 跑通（PC 先起！）

**终端 A（PC）**

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && ./bin/pc/pc_receiver -p 5000 -o ./frames -d --accept-timeout 300
```

**终端 B（板端）**

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 -n 300
```

---

## 3. 双指缩放目视确认

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 --capture-only -l --touch --quiet
```

依次做：**双指张开（放大）→ 合拢（缩小）→ 平移 → 三指（复位）**。

方向不对的备选（逐个试）：

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 --capture-only -l --touch --quiet --touch-swap-xy
```

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 --capture-only -l --touch --quiet --touch-invert-x
```

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 --capture-only -l --touch --quiet --touch-invert-y
```

---

## 4. 基础性能矩阵（PC 先起；约 4 分钟）

**终端 A（PC）**

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && mkdir -p performance_evaluation/results/pc_bench && ./bin/pc/pc_receiver -p 5000 --no-save --quiet --accept-timeout 900 --io-timeout 60 --stats-dir performance_evaluation/results/pc_bench
```

**终端 B（板端）**

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && sh board_benchmark.sh /dev/video1 192.168.1.66 60 ./bench
```

---

## 5. 缩放矩阵 1.0 / 1.5 / 2.0×（PC 先起；约 4 分钟）

**终端 A（PC）**

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && mkdir -p performance_evaluation/results/pc_zoom && ./bin/pc/pc_receiver -p 5000 --no-save --quiet --accept-timeout 900 --io-timeout 60 --stats-dir performance_evaluation/results/pc_zoom
```

**终端 B（板端）**

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && sh board_zoom_bench.sh /dev/video1 192.168.1.66 60 ./bench_zoom
```

> 结果落在 NFS 上，PC 侧路径：`/home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/bench*`

---

## 6. 分析（PC）

单档：

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && python3 performance_evaluation/scripts/analyze_stats.py /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/bench_zoom/zoom_2.0
```

三档关键指标一次看完：

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && for z in 1.0 1.5 2.0; do echo "########## zoom ${z}x ##########"; python3 performance_evaluation/scripts/analyze_stats.py /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/bench_zoom/zoom_$z | grep -E 'render|display_fps|frames_rendered|gesture|frames_ok|gap|poll'; done
```

两侧对照（板端 vs PC）：

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && python3 performance_evaluation/scripts/analyze_stats.py --compare /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/bench/capture_tcp performance_evaluation/results/pc_bench
```

`summary.csv` 关键列（该文件共 61 列，下面是任务书要的）：

| 列 | 名称 | 含义 |
|---|---|---|
| 6 | `fps_overall` | 采集 FPS |
| 20 / 21 / 22 | `payload_mbps` / `cpu_percent_avg` / `rss_max_kb` | 吞吐 / CPU / RSS |
| 43 | `display_fps` | **实际写进 LCD 的帧率** |
| 44 / 45 | `fit_render_frames` / `zoom_render_frames` | 两条渲染路径各跑多少帧 |
| 50 | `zoom_final` | 结束时倍率 |
| 51–55 | `pinch_out` / `pinch_in` / `pan_frames` / `zoom_resets` / `touch_syn_dropped` | 手势计数 |
| **56–59** | `render_mean_us` / `p95` / `p99` / `max` | **缩放渲染耗时（任务书要求）** |
| **60–61** | `gesture_mean_us` / `gesture_p99_us` | **手势处理耗时** |

三档一次拉出关键指标：

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/bench_zoom && for z in 1.0 1.5 2.0; do s=zoom_$z/summary.csv; printf "[zoom %s] 采集FPS=%s 显示FPS=%s mbps=%s cpu=%s rss_kb=%s | render mean/p95/p99/max=%s/%s/%s/%s us | gesture mean/p99=%s/%s us\n" "$z" "$(cut -d, -f6 $s|sed -n 2p)" "$(cut -d, -f43 $s|sed -n 2p)" "$(cut -d, -f20 $s|sed -n 2p)" "$(cut -d, -f21 $s|sed -n 2p)" "$(cut -d, -f22 $s|sed -n 2p)" "$(cut -d, -f56 $s|sed -n 2p)" "$(cut -d, -f57 $s|sed -n 2p)" "$(cut -d, -f58 $s|sed -n 2p)" "$(cut -d, -f59 $s|sed -n 2p)" "$(cut -d, -f60 $s|sed -n 2p)" "$(cut -d, -f61 $s|sed -n 2p)"; done
```

完整表头对照：

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/bench_zoom && head -1 zoom_1.0/summary.csv | tr ',' '\n' | nl
```

逐帧原始数据（缩放渲染耗时的分布来源，最后 6 列是 `gesture_us,render_us,zoom,view_x0,view_y0,render_path`）：

```bash
ls -l /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/bench_zoom/zoom_1.5/ && head -3 /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/bench_zoom/zoom_1.5/capture_frames.csv
```

渲染路径分布抽查（`render_path` = 1 表示走了缩放渲染）：

```bash
awk -F, 'NR==1{for(i=1;i<=NF;i++) if($i=="render_path") c=i; next} {n[$c]++} END{for(k in n) print "render_path="k" : "n[k]" 帧"}' /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/bench_zoom/zoom_1.5/capture_frames.csv
```

---

## 7. 回填报告

把实测值填进 `performance_evaluation/PERFORMANCE.md` 的 **§D 结果表格模板**：

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && sed -n "$(grep -n '^## D\.' performance_evaluation/PERFORMANCE.md | cut -d: -f1),+20p" performance_evaluation/PERFORMANCE.md
```

同时把 §P2.1「运行矩阵」里的 `待实测` 换成实测值：

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp && grep -n '待实测' performance_evaluation/PERFORMANCE.md | head -20
```

---

## 速查

| 现象 | 处理 |
|---|---|
| `S_FMT returned XXX, but YYY was requested` | 把 `-f` 改成 `XXX` 对应格式（`YUYV`/`UYVY`/`RGB565`/`MJPEG`） |
| `TCP connect failed` / 板端秒退 | PC 端没先起，或 IP/端口不对 |
| `bad_version`（PC 端） | 板端还是旧协议 v1，重跑第 0 步 |
| `Address already in use` | 换端口 `-p 5001`，或 `ss -ltnp \| grep 5000` 杀掉旧进程 |
| `[WARN] touch init failed` | 触摸设备/驱动不在；采集与图传不受影响 |
| 画面偏蓝绿 | 板端加 `--lcd-swap-rb`；PC 窗口按 `3` |
| 缩放方向反了 | `--touch-swap-xy` / `--touch-invert-x` / `--touch-invert-y` |
| 脚本没反应 | 确认 PC 端 `pc_receiver` 已在监听（板端是客户端，连不上会直接退出） |

| 参数 | 为什么必须加 |
|---|---|
| `--no-save` | 去掉磁盘写入，吞吐/耗时才干净 |
| `--quiet` | 串口逐帧打印会污染测量 |
| `--duration` / `-n` | 定长，各组可比；避免忘了关 |
| `--warmup 30` | 排除驱动预热与 TCP 慢启动 |
| `--io-timeout` | 对端停读时不永久阻塞 |

---

## 附录：单条命令速查（不用脚本）

```bash
# 纯采集基准（不发网络）
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 --capture-only --duration 60 --warmup 30 --quiet --stats-dir ./bench/capture_only

# 采集 + 发送
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 --duration 60 --warmup 30 --quiet --io-timeout 5 --stats-dir ./bench/capture_tcp

# 采集 + 发送 + LCD
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 -l --duration 60 --warmup 30 --quiet --io-timeout 5 --stats-dir ./bench/capture_tcp_display

# 固定 2.0x 缩放（不读触摸，只改一个变量）
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 -l --zoom-fixed 2.0 --duration 60 --warmup 30 --quiet --io-timeout 5 --stats-dir ./bench/zoom_2.0

# 对照实验：加大缓冲
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 --buffers 6 --duration 60 --warmup 30 --quiet --io-timeout 5 --stats-dir ./bench/buf6

# 对照实验：请求 15fps
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15 && ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 --set-fps 15 --duration 60 --warmup 30 --quiet --io-timeout 5 --stats-dir ./bench/fps15
```
