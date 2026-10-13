# 使用说明书（USAGE）

> i.MX6ULL / OV5640 V4L2 MMAP 采集 → TCP 图传 → PC 显示
>
> 本文是**按顺序照做就能跑通**的流程说明。细节原理见 `../README.md`，
> 性能测试的完整方法论见 `PERFORMANCE.md`。
>
> **本文所有 shell 命令都假设：工作目录 = 项目根目录**
> （即 `/home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp`）。
> 文档路径类引用则以 `performance_evaluation/` 为基准，见 §11。
>
> **当前协议版本为 2**（`include/frame_proto.h` 的 `FRAME_VERSION`）。
> 版本 1 与 2 的 `timestamp_us` 线上编码不同，**不能混用**，两端必须同时重编。

---

## 0. 先搞清楚数据流（决定谁先启动）

```
┌──────────────────────────────┐            ┌───────────────────────────────┐
│  i.MX6ULL 开发板 (TCP 客户端) │            │  PC / 开发机 (TCP 服务端)      │
│                              │            │                               │
│  /dev/videoN ──V4L2 MMAP──┐  │            │  ┌── SDL2 窗口 (可选 -d)       │
│                           ▼  │            │  │                            │
│  (可选 -l) /dev/fb0 ◄── 帧数据 │            │  ├── 落盘 raw/.ppm (可 --no-save)
│                           │  │  40B 帧头   │  │                            │
│                    imx6ull_sender ─────────┼──► pc_receiver                │
│                           │  │  + 裸图像   │  │                            │
│                           └──┘  │           │  └── CSV 统计 (--stats-dir)   │
└──────────────────────────────┘            └───────────────────────────────┘
```

**三条必须记住的规则：**

1. **PC 端先启动**。板端是客户端，PC 端是服务端；板端 `connect()` 时如果 PC 还没在监听，板端会直接失败退出。
2. 板端**不重连**。对端断开后帧号会不连续，继续发只会让 PC 端收到混乱数据，所以程序选择发完即停。
3. 一次只服务一个客户端（`accept()` 一次即止，`backlog=1`）。

---

## 1. 一次性环境准备

### 1.1 开发机（编译/接收）

| 依赖 | 用途 | 检查命令 |
|---|---|---|
| 交叉工具链 `gcc-linaro-4.9.4-2017.01` | 编译板端 | `ls /home/why/zdyz/a/env/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-gcc` |
| `gcc` | 编译 PC 端 | `gcc --version` |
| `libsdl2-dev` | PC 端实时显示窗口（**可选**） | `pkg-config --modversion sdl2` |
| `python3` | CSV 分析 | `python3 --version` |
| `coreutils` 的 `timeout` | 测试脚本超时护栏 | `command -v timeout` |

若缺 SDL2：

```bash
sudo apt install libsdl2-dev
```

> 没装 SDL2 也能编译运行，只是 `pc_receiver` 失去 `-d` 显示窗口能力
> （Makefile 用 `pkg-config --cflags sdl2` 探测，探测不到就不定义 `ENABLE_SDL_DISPLAY`）。

### 1.2 开发板（NFS 启动）

确认 NFS 已挂载，且 rootfs 挂载点存在：

```bash
mount | grep nfs
ls -d /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15
```

### 1.3 网络

板子与 PC 必须能互通，且**知道 PC 的 IP**（板端 `-s` 参数要填它）：

```bash
# PC 上查自己的 IP
ip -4 addr show | grep inet
```

---

## 2. 编译（两个独立目标，各存日志）

在项目根目录执行：

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp
make clean

# 板端 (ARM 交叉编译)
timeout 300 make arm      2>&1 | tee performance_evaluation/logs/build_arm.log

# PC 端 (本机 gcc, 含 SDL2)
timeout 300 make pc       2>&1 | tee performance_evaluation/logs/build_pc.log

```

**一次编译全部：**

```bash
timeout 600 make allboth 2>&1 | tee performance_evaluation/logs/build_after.log
```

**验收标准**：`echo $?` 为 0，且日志里**没有 warning**：

```bash
grep -ci warning performance_evaluation/logs/build_*.log   # 应全为 0
```

### 2.1 产物一览

| 路径 | 运行位置 | 说明 |
|---|---|---|
| `bin/arm/imx6ull_sender` | 板端 | **主程序**：采集 +（可选）LCD + TCP 发送 |
| `bin/arm/imx6ull_sender` 的**启动日志** | 板端 | 承担原诊断工具的职责：打印 `S_FMT`/`REQBUFS`/`G_PARM` 的实际返回值 |
| `bin/pc/pc_receiver` | PC | **主程序**：TCP 接收 +（可选）SDL2 + 落盘 + 统计 |

### 2.2 换工具链（可选）

```bash
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
# 或直接改 Makefile 里的 TOOLCHAIN_DIR 与 CROSS_COMPILE
```

---

## 3. 部署到板子（NFS）

```bash
make install
```

安装到 `DESTDIR = /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15`，包含：

- `imx6ull_sender`


把测试脚本也一起拷过去（板端做性能测试要用）：

```bash
cp performance_evaluation/scripts/board_benchmark.sh \
   performance_evaluation/scripts/analyze_stats.py \
   /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/
```

> ⚠️ **协议升级提醒**：本次协议为 **v2**。如果板子上还有旧的
> `imx6ull_sender`（v1），PC 端会报 `bad_version` 并退出。
> **必须两端同时重新编译部署。**

---

## 4. 上板确认（**这步不要跳过**）

SSH / 串口登录板子后：

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15

# ① 确认设备节点 —— 不要假设一定是 /dev/video1
ls -l /dev/video*

# ② 确认驱动 / 格式 / 帧率 —— 直接看 imx6ull_sender 的启动日志
#    它会打印 S_FMT 实际返回的 width/height/pixfmt/bytesperline/sizeimage
#    以及 G_PARM 的 timeperframe; 跑 1 帧就退出即可
./imx6ull_sender -d /dev/video1 --capture-only -n 1

#    若请求的格式不被驱动接受, 它会**报错并打印驱动的实际返回值**,
#    例如: S_FMT returned 0x32315659, but 0x56595559 was requested
#    据此把 -f 改成它支持的那个格式再跑一遍

# ③ 确认网络
ifconfig eth0
ping -c 4 <PC_IP>

# ④ 确认链路速率（i.MX6ULL 的 ENET 是 100Mbps）
ethtool eth0 | grep -E 'Speed|Duplex'    # 无 ethtool 就从交换机侧确认
```

**必须从启动日志里记录下来（后续所有配置与数据都以此为准）：**

| 要记的项 | 日志里的哪一行 |
|---|---|
| 驱动名是否为 `mx6s-csi` | 初始化信息 |
| **`S_FMT` 实际返回**的 `width / height / pixfmt / bytesperline / sizeimage` | `[INFO] ... pixfmt=...` 那段 |
| **`REQBUFS` 实际给的 buffer 数**（驱动可以改小） | `nbufs actual = N` |
| **`G_PARM` 的 `timeperframe`** 与驱动是否声明支持 | `timeperframe = a/b (x.xx fps), TIMEPERFRAME supported/NOT supported` |
| 网卡速率 | `ethtool` |

> **不要预设 640×480@30**。以 `S_FMT` / `G_PARM` 的**实际返回值**为准。
>
> 📌 项目自带的两个诊断工具（`v4l2_info` / `v4l2_format_test`）**已于 2026-10-13 删除**，
> 上面用 `imx6ull_sender` 的启动日志替代。若板子上另有 `v4l2-ctl`（v4l-utils），
> 也可以用它查能力：`v4l2-ctl -d /dev/video1 --all`。

---

## 5. 最小跑通（先确认能出图，再谈性能）

### 5.1 第 1 步：PC 端启动接收（先起！）

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp

# 保存帧 + SDL2 实时显示
./bin/pc/pc_receiver -p 5000 -o ./frames -d --accept-timeout 120
```

终端会打印：

```
[INFO] Listening on port 5000 (fd=3)
[INFO] Waiting for connection...
```

`--accept-timeout 120` 表示 120 秒内没人连就自己退出，**避免脚本无脑挂着**。
交互使用可以不加（默认一直等）。

### 5.2 第 2 步：板端启动发送

在**另一个终端**登录板子：

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15

# 用第 4 步确认过的格式！
./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 -w 640 -h 480 -f YUYV -n 100
```

`-n 100` 表示发 100 帧后自动停止（调试期建议加上，避免忘了关）。

**成功标志**：

- 板端：`[FRAME 0] hdr=YUYV, 640x480, 614400B, ts=...`
- PC 端：`[FRAME 0] 640x480 614400B hdr=YUYV ts=...` + `-> PPM: ...`

PC 端 `./frames/` 下会生成 `frame_000000.yuv` 与 `frame_000000.ppm`，
PPM 可以直接用图片查看器打开肉眼比对颜色。

### 5.3 第 3 步：确认颜色正确

**PC 端窗口里按键切换解码器，哪个颜色正常就用哪个：**

| 按键 | 格式 | 按键 | 格式 |
|---|---|---|---|
| `1` | RGB565_LE | `5` | YUYV |
| `2` | RGB565X_BE | `6` | UYVY |
| `3` | BGR565_LE | `7` | YVYU |
| `4` | BGR565X_BE | `8` | VYUY |
| `0` | auto（按帧头） | `ESC`/`Q` | 退出 |

**默认是 `auto`（按帧头 `hdr.pixfmt` 解码）。**
若画面偏蓝绿，通常是 i.MX6ULL CSI 交换了 R/B，试 `3`（BGR565_LE）。

找到正确解码器后固定下来（下次启动直接生效）：

```bash
./bin/pc/pc_receiver -p 5000 -o ./frames -d --display-format bgr565
```

### 5.4 第 4 步：板端 LCD 显示（需接屏）

```bash
# 基本
./imx6ull_sender -d /dev/video1 -s <PC_IP> -l -n 300

# 若板端 LCD 偏蓝绿而 PC 端正常 -> 打开 R/B 交换 (不需重新编译)
./imx6ull_sender -d /dev/video1 -s <PC_IP> -l --lcd-swap-rb -n 300
```

`--lcd-swap-rb` **只影响 LCD 写入，不影响发给 PC 的原始数据**。
LCD 初始化失败不会致命，会打印 WARN 后继续发送。

---

## 6. 完整参数表

### 6.1 板端 `imx6ull_sender`

| 参数 | 默认 | 说明 |
|---|---|---|
| `-d <dev>` | `/dev/video1` | 视频设备节点 |
| `-s <ip>` | 必填 | PC 的 IP（除非用 `--capture-only`） |
| `-p <port>` | `5000` | TCP 端口 |
| `-w <w>` / `-h <h>` | `640` / `480` | 请求分辨率 |
| `-f <fmt>` / `--pixfmt <fmt>` | `YUYV` | `YUYV`/`UYVY`/`RGB565`/`MJPEG` |
| `--yuv-order <o>` | 无 | `yuyv`/`uyvy`/`yvyu`/`vyuy`，**只改帧头标签，不做字节重排** |
| `-n <frames>` | `0`（不限） | 发送帧数上限 |
| `-l` | 关 | 启用 `/dev/fb0` LCD 显示 |
| `--lcd-swap-rb` | 关 | LCD 写入 RGB565 时交换 R/B |
| `--buffers <n>` | `3` | `REQBUFS` 数量（1–8） |
| `--set-fps <n>` | 不请求 | 经 `VIDIOC_S_PARM` 请求帧率 |
| `--io-timeout <sec>` | 不设 | `SO_SNDTIMEO`，对端停读时不再永久阻塞 |
| `--stats-dir <dir>` | 关 | **开启统计**，写 CSV |
| `--duration <sec>` | `0`（不限） | 跑满指定秒数自动停止 |
| `--warmup <frames>` | `0` | 前 N 帧不计入分位数 |
| `--stats-interval <sec>` | `1` | CPU/RSS 周期采样间隔 |
| `--capture-only` / `--no-tcp` | 关 | 完全跳过 TCP（纯采集基准） |
| `--quiet` | 关 | 关闭逐帧 stdout |
| `--touch` | 关 | **启用双指手势缩放**，自动扫描 `/dev/input/event0..9` 找 MT 触摸设备 |
| `--touch-dev <dev>` | — | 指定触摸设备节点（如 `/dev/input/event1`） |
| `--touch-replay <evlog>` | — | 回放合成 MT 事件日志（离线验证，不读真实设备） |
| `--zoom-min <f>` / `--zoom-max <f>` | `1.0` / `2.0` | 缩放范围 |
| `--zoom-fixed <f>` | — | 固定倍率、不读触摸，**强制走缩放渲染路径**（用于性能矩阵） |
| `--touch-swap-xy` / `--touch-invert-x` / `--touch-invert-y` | 关 | 触摸方向微调（装配方向不一致时） |

> 手势缩放需要 `-l`（只作用于本地显示）；没有 LCD 时会告警并忽略。
> **触摸设备未连接时不影响采集与图传** —— 只告警并关闭缩放。详见 §12。

> **严格的格式检查**：`S_FMT` 若返回了与 `-f` 不同的格式，程序会**报错退出**，
> 而不是静默按错误格式发数据。看到
> `[ERROR] S_FMT returned ..., but ... was requested` 时，
> 报错信息里**已经带了驱动的实际返回值**，把 `-f` 改成那个格式即可。

### 6.2 PC 端 `pc_receiver`

| 参数 | 默认 | 说明 |
|---|---|---|
| `-p <port>` | `5000` | 监听端口 |
| `-o <dir>` | `./frames` | 帧文件输出目录 |
| `-d` | 关 | 启用 SDL2 实时显示 |
| `-n <frames>` / `--max-frames <n>` | `0`（不限） | 收满 N 帧后自动退出 |
| `--display-format <f>` | 无（auto） | 强制解码格式 |
| `--swap-rb` | 无 | 等价 `--display-format bgr565` |
| `--yuv-order <o>` | 无 | YUV 字节顺序覆盖 |
| `--no-save` | 落盘 | **不写帧文件**（测吞吐时必加，去掉磁盘 I/O 干扰） |
| `--quiet` | 关 | 关闭逐帧 stdout |
| `--accept-timeout <sec>` | `0`（一直等） | 无人连接则自行退出，**自动化测试必加** |
| `--io-timeout <sec>` | 不设 | `SO_RCVTIMEO`/`SO_SNDTIMEO` |
| `--stats-dir <dir>` | 关 | **开启统计**，写 CSV |
| `--stats-interval <sec>` | `1` | CPU/RSS 周期采样间隔 |

---

## 7. 性能测试流程（P0）

### 7.1 一键跑完（推荐）

**终端 A（PC 端）：** 板端会连过来，所以先起接收端。

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp
mkdir -p performance_evaluation/results/pc_capture_tcp
./bin/pc/pc_receiver -p 5000 --no-save --quiet \
    --accept-timeout 900 --io-timeout 60 \
    --stats-dir performance_evaluation/results/pc_capture_tcp
```

**终端 B（板端）：**

```bash
cd /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15
sh board_benchmark.sh /dev/video1 <PC_IP> 60 ./bench
```

脚本会依次跑：

| 步骤 | 内容 |
|---|---|
| 0 | 环境信息（`uname`/网卡/`ping`/帧缓冲位深） |
| 1 | `capture-only` 60s（纯采集基准，不碰网络） |
| 2 | `capture+tcp` 60s |
| 3 | `capture+tcp+display` 60s（加 `-l`） |
| 4 | 长时稳定性 300s（时间不够可 Ctrl+C，已采部分仍有效） |
| 5a | 对照实验：`--buffers 6`（与步骤 2 的 buffers=3 比较） |
| 5b | 对照实验：`--set-fps 15`（验证驱动是否接受帧率协商） |
| 5c | 提示手工做 LCD R/B 目视对比 |
| 6 | 资源清理检查（`fuser` / `ps` / `dmesg`） |

每个用例都带 `--duration` 与 `--io-timeout`，**不会挂死**。

### 7.2 手工逐条执行（想精确控制时）

```bash
# 纯采集基准 (对照组, 与下一组除网络外参数必须完全一致)
./imx6ull_sender -d /dev/video1 --capture-only \
    --duration 60 --warmup 30 --quiet \
    --stats-dir ./bench/capture_only

# 采集 + 发送
./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 \
    --duration 60 --warmup 30 --quiet --io-timeout 5 \
    --stats-dir ./bench/capture_tcp

# 采集 + 发送 + LCD
./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 -l \
    --duration 60 --warmup 30 --quiet --io-timeout 5 \
    --stats-dir ./bench/capture_tcp_display
```

**参数为什么这么选：**

| 参数 | 理由 |
|---|---|
| `--duration 60` | 定长，保证各组可比 |
| `--warmup 30` | 排除驱动/传感器预热、TCP 慢启动 |
| `--quiet` | 逐帧串口打印本身可能成为瓶颈，污染测量 |
| `--io-timeout 5` | 对端停读时 5s 内退出，不挂死 |
| `--capture-only` | 与 `capture+tcp` 严格同配置，**唯一差异是网络** |

**重复次数**：每模式 2–3 次。时间不够只做 1 次的话，**必须在报告里标注"单次测试"**。

### 7.3 产出的 CSV

| 文件 | 位置 | 内容 |
|---|---|---|
| `capture_frames.csv` | 板端 `--stats-dir` | 逐帧：`sequence`/`seq_gap`/`bytesused`/`poll_us`/`lcd_us`/`send_us`/`t3t1_us`/时间戳 |
| `process_usage.csv` | 两端 | 周期采样：`cpu_percent`/`rss_kb`/`fps_interval` |
| `summary.csv` | 两端 | 汇总 + **配置快照**（分辨率/格式/缓冲数/帧率/LCD 开关） |
| `network_stats.csv` | PC 端 | 逐帧：重组耗时/`payload_bytes`/`frame_id` 缺口 |

> `summary.csv` 末尾自带 `nbufs/req_buffers/req_fps/fps_num/fps_den/tf_supported/width/height/pixfmt/lcd/lcd_swap_rb`，
> **把 CSV 拷出来就等于把复现条件一起带走了。**

---

## 8. 结果分析

```bash
# 单侧分析
python3 performance_evaluation/scripts/analyze_stats.py <某次测试的 stats 目录>

# 两侧对照（板端 bench/capture_tcp 与 PC 端目录）
python3 performance_evaluation/scripts/analyze_stats.py --compare \
    <bench/capture_tcp 目录> \
    performance_evaluation/results/pc_capture_tcp
```

输出包含：FPS、帧间隔 mean/p50/p95/p99/max/stddev、payload Mbps、
`sequence` 跳变、短收发分片数、CPU/RSS、板内阶段耗时，以及错误计数。

### 8.1 指标口径（写报告/面试时必须按这个说）

| 指标 | 口径 | **不能这么说** |
|---|---|---|
| 采集 FPS | 成功 DQBUF 次数 ÷ 实测时长（`CLOCK_MONOTONIC`） | 不等于 `G_PARM` 的目标帧率 |
| 有效吞吐 | `payload 字节 × 8 ÷ 实测秒数 ÷ 1e6`，**不含 40B 帧头** | 别把含帧头的口径混进来 |
| CPU | **进程** CPU 时间比例（单核 = 100%），运行首尾两次 `/proc` 采样做差 | 不是整机 CPU 占用 |
| 板内 `T3-T1` | 该帧 DQBUF → 整帧交给 socket | **不是**端到端延迟 |
| PC 内 `R2-R1` | 帧头读完 → 数据重组完成 | 跨主机单调钟**不可相减** |
| `frame_id` 缺口 | 发送端未完成整帧协议发送的帧数 | **不等于**硬件丢帧 |
| 完整帧数 | 协议级完整重组帧数 | 协议**无 CRC**，不能说"内容 100% 正确" |

---

## 9. 排错手册

### 9.1 板端

| 现象 | 原因 | 处理 |
|---|---|---|
| `open /dev/video1 failed` | 节点不存在/权限 | `ls -l /dev/video*`；`chmod 666 /dev/videoN`；`dmesg \| grep -i csi` |
| `[ERROR] S_FMT returned XXX, but YYY was requested` | 驱动不支持请求格式 | 报错里 `XXX` 就是驱动实际给的格式，把 `-f` 改成它 |
| `VIDIOC_STREAMON failed` | 传感器未上电 / MCLK 未输出 / 排线松 | `dmesg \| grep -i 'ov5640\|csi'`；注意本项目 **MCLK 引脚与闪光灯 LED 共用**，被切到 GPIO 时摄像头无时钟 |
| `poll timeout (2000 ms)` 偶发 | STREAMON 后传感器预热 | 正常警告。**连续**超时说明传感器没出图，查 CSI 中断错误与 `csi_id`/`mux_id` |
| `DQBUF EAGAIN, will retry` | 非阻塞模式下暂无数据 | 正常。确认 `open` 用了 `O_NONBLOCK`，且 `poll` 返回 `POLLIN` 后才 `DQBUF` |
| `[ERROR] send data failed` / `send timed out` | PC 端断开或停读 | PC 端是否还活着；`--io-timeout` 设了吗 |
| `[ERROR] accept timeout after Ns`（PC 端） | 板端没连上 | 检查 IP/端口/防火墙；板端 `ping` PC |
| `TCP connect failed` | PC 端没启动 | **PC 端必须先启动** |

### 9.2 PC 端

| 现象 | 原因 | 处理 |
|---|---|---|
| `bind: Address already in use` | 端口被占 | `ss -ltnp \| grep 5000`，换 `-p` 或杀掉旧进程 |
| `[ERROR] header rejected: bad_version` | **板端还是旧协议 v1** | 重新编译部署板端（协议已升 v2） |
| `[ERROR] header rejected: bad_magic` | 流错位 | 两端分辨率/格式不一致；或中间有代理改写了数据 |
| `[ERROR] header rejected: bad_size` | 帧头 `size` 不合理 | 同样先查两端格式一致性 |
| 画面全黑 / 全是灰 | 解码器选错 | 窗口里按 `1`–`8` 逐个试；或用 `--display-format` |
| 画面偏蓝绿 | CSI 交换了 R/B | `--swap-rb` 或按 `3` |
| `[SDL] Init failed` | 无显示环境 / 未装 SDL2 | 去掉 `-d`；或 `sudo apt install libsdl2-dev` |

### 9.3 性能异常

| 现象 | 排查方向 |
|---|---|
| FPS 远低于预期 | 看 `summary.csv` 的 `interval_p99_us`（是抖动还是普遍慢）；看 `poll_timeout` |
| `seq_gap_events` 很多 | 用 `--buffers 6` 做对照；若改善说明缓冲不足，若不变说明发送耗时超过帧周期 |
| `send_us` 很大 | 网络带宽/拥塞；试 `-f MJPEG` 或降分辨率/降帧率 |
| PC 端 `recv_timeouts > 0` | 对端（板端）卡住或网断 |
| CPU 100% | 是**进程**单核百分比；`--quiet` 下仍高说明瓶颈在转换/发送 |
| 帧率随时间衰减 | 看 `process_usage.csv` 的 `fps_interval` 与 `rss_kb` 是否单调增长（泄漏） |

### 9.4 测试过程挂住不动

本项目在三个层面都做了超时护栏，正常不会无限挂：

| 层 | 机制 |
|---|---|
| **脚本级** | `board_benchmark.sh` 的每个用例都由 `timeout --signal=TERM --kill-after=2 <N>` 包住 |
| **程序级（连接）** | `pc_receiver --accept-timeout <s>`：没人连接就自己退出，不会永久阻塞在 `accept()` |
| **程序级（收发）** | `pc_receiver --io-timeout <s>` / `imx6ull_sender --io-timeout <s>`：对端挂起时 `recv`/`send` 到期返回 `EAGAIN`，不再永久阻塞 |
| **程序级（时长）** | 板端每个用例都带 `--duration`，到点自动结束 |

若仍异常，手工检查残留并清理：

```bash
ps -eo pid,cmd | grep -E 'pc_receiver|imx6ull_sender' | grep -v grep
ss -ltnp | grep 5000
fuser /dev/video1
```

> 最常见的挂住原因不是程序，而是 **PC 端没先启动**：板端 `connect()` 失败会直接退出，
> 但 PC 端会一直等在 `accept()`。加 `--accept-timeout` 就能避免误判成"程序卡死"。

---

## 10. 红线（写报告 / 简历 / 面试时不许越界）

| ❌ 不能说 | 原因 |
|---|---|
| "优化后性能提升 xx%" | 本项目**没有做过性能优化**，无可比前后数据 |
| "零丢帧" / "内容 100% 正确" | 协议**无 CRC**，无帧内序号连续性保证 |
| "端到端延迟 X ms" | 跨主机单调钟不可相减；`send()` 返回不代表数据到达 |
| "整机 CPU 占用 X%" | 采到的是**进程** CPU 时间比例 |
| "支持 640×480@30" | **未实测**，必须以板上 `S_FMT`/`G_PARM` 实测值为准 |
| "网络是瓶颈" | 必须给出 `capture-only` vs `capture+tcp` 的同配置对照 |
| "已修复板端 LCD 颜色问题" | 只提供了 `--lcd-swap-rb` **开关**，是否交换需目视确认 |
| "支持帧率配置" | 只实现了 `S_PARM` 请求 + `G_PARM` 读回，**驱动是否接受必须上板确认** |

---

## 11. 文档索引

**约定**：下表「路径」列以 **`performance_evaluation/`** 为基准（本文所在目录）。
`PERF` = `performance_evaluation/`，`ROOT` = 项目根目录。

| 路径 | 等价于（相对项目根） | 用途 |
|---|---|---|
| `../QUICKSTART.md` | `ROOT/QUICKSTART.md` | **纯命令清单**（可直接复制粘贴，PC IP 已写死） |
| `README.md` | `PERF/README.md` | **总索引**：一页纸结论 + 文档导航 + 快速命令 + 关键文件对照 |
| `USAGE.md` | `PERF/USAGE.md` | **本文件**：按顺序照做的使用流程 |
| `AUDIT_AND_CHANGES.md` | `PERF/AUDIT_AND_CHANGES.md` | **§A 源码审计**（文件:行号映射 + 问题清单 + GT911 集成审计）；**§B 改动记录**（改了什么/为什么/怎么验证） |
| `PERFORMANCE.md` | `PERF/PERFORMANCE.md` | **测试计划**（§A–§E：怎么测、验收标准、对照实验）+ **§P 性能报告** + **§Z 双指缩放验证** + 指标口径 |
| `RESUME.md` | `PERF/RESUME.md` | 简历/面试口径（含红线清单） |
| `scripts/` | `PERF/scripts/` | 板端测试与分析脚本 |
| `logs/` | `PERF/logs/` | 编译与测试日志 |
| `results/` | `PERF/results/` | 板端/PC 采集到的 CSV |
| `../README.md` | `ROOT/README.md` | 原理、协议细节、格式说明、历史排查记录 |
| `../docs/项目源码面试审计报告.md` | `ROOT/docs/...` | 源码级审计 + 面试问答准备 |
| `../docs/设备树修改面试审计报告.md` | `ROOT/docs/...` | 设备树/内核侧审计 |

> 第 2–8 章的命令里出现 `performance_evaluation/logs/...` 这类写法，是因为那些命令
> **在项目根目录执行**；与上表的路径基准不同，不要混淆。

---

## 12. 双指手势数字缩放（GT911）

在原有采集 / 图传 / LCD 显示之上，整合 GT911 电容触摸，用**双指手势**对本地显示做
数字缩放（1.0×–2.0×）。**不改采集分辨率、不向采集缓冲区写入数据**，缩放只发生在写显存这一步。

### 12.1 前置确认（不要跳过）

```bash
# 板端: 确认触摸设备与名称
ls -l /dev/input/
cat /proc/bus/input/devices | grep -B2 -A6 -i gt911
#   期望看到: Name="GT911 Touchscreen"  Bus=I2C
#   设备节点编号在不同板子上不固定, 所以程序默认自动扫描 event0..9

# 板端: 确认 LCD 实际位深 (决定走 16 还是 32bpp 渲染路径)
cat /sys/class/graphics/fb0/bits_per_pixel
```

### 12.2 启用缩放（先本地验证，不发网络）

```bash
# 板端
./imx6ull_sender -d /dev/video1 --capture-only -l --touch --quiet
```

启动时应看到：

```
[TOUCH] opened /dev/input/event1: name="GT911 Touchscreen"
[TOUCH] abs range: X[0..1023] Y[0..599] -> LCD 1024x600
[FB] Screen: 1024x600, bpp=16, line_length=2112
[ZOOM] image 640x480 -> LCD 1024x600
[ZOOM] zoom range : 1.00x .. 2.00x
[ZOOM] initial    : zoom=1.00 view=(-192.0, -60.0) [fit: 走无缩放快速路径]
```

**目视逐项确认**：

| 操作 | 期望 |
|---|---|
| 双指张开 | 放大，且**以双指中心为中心**（不是屏幕中心） |
| 双指合拢 | 缩小，到 1.0× 后不再变小 |
| 双指平移 | 画面跟随双指移动 |
| 三指同时按 | 复位到 1.0× 居中 |
| 松手后再动 | 缩放状态**保持**，不会自己跳回去 |

方向不对时加：`--touch-swap-xy` / `--touch-invert-x` / `--touch-invert-y`。

### 12.3 固定倍率性能矩阵（1.0× / 1.5× / 2.0×）

用 `--zoom-fixed` 让三档之间**只差一个变量**，否则手势的时机与次数不可控，数据不可比。

```bash
# PC 端先起接收
./bin/pc/pc_receiver -p 5000 --no-save --quiet --accept-timeout 900 --io-timeout 60 \
    --stats-dir performance_evaluation/results/pc_zoom

# 板端一键跑完 1.0/1.5/2.0x + 无缩放对照组
sh board_zoom_bench.sh /dev/video1 <PC_IP> 60 ./bench

# 分析
python3 performance_evaluation/scripts/analyze_stats.py ./bench/zoom_1.5
```

要看的列（都在 `summary.csv` 与 `capture_frames.csv` 里）：

| 指标 | 位置 | 含义 |
|---|---|---|
| `display_fps` | `summary.csv` | 真正写进 LCD 的帧率（区别于采集 FPS） |
| `render_mean_us` / `render_p95_us` / `render_p99_us` / `render_max_us` | `summary.csv` | **缩放渲染耗时**（不含触摸解析） |
| `gesture_mean_us` / `gesture_p99_us` | `summary.csv` | 触摸解析 + 手势判定耗时 |
| `render_path` | `capture_frames.csv` | 每帧走的路径：0=无缩放 blit，1=缩放渲染 |
| `zoom` / `view_x0` / `view_y0` | `capture_frames.csv` | 每帧的实际视图 |
| `zoom_render_frames` / `fit_render_frames` | `summary.csv` | 两条路径各跑了多少帧 |

### 12.4 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| `[WARN] touch init failed`，但采集图传正常 | 触摸设备/驱动不在 | 这是设计内的降级行为；查 `ls /dev/input/`、`dmesg \| grep -i gt911` |
| `[WARN] --touch/--zoom 只在本地显示上有意义` | 忘了加 `-l` | 加 `-l` |
| 缩放方向反了 / 上下左右颠倒 | 面板装配方向不同 | `--touch-swap-xy` / `--touch-invert-x` / `--touch-invert-y` |
| 手势不响应 | 触点数识别不对 | 启动日志的 `abs range` 是否合理；确认驱动是 Protocol B |
| 画面偏蓝绿 | CSI 交换 R/B | 加 `--lcd-swap-rb` |
| 放大到顶后画面出现空白条 | 不应发生（`fb_view_clamp` 保证） | 记录现场并回报；离线测试里已有逐帧断言覆盖 |
| 缩放后帧率明显下降 | 渲染耗时超过帧周期 | 看 `render_p99_us` 与帧间隔对比；必要时降低采集分辨率 |
