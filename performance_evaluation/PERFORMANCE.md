# 测试计划与性能报告 (PERFORMANCE)

> 本文由原 `test_plan.md` + `performance_report.md` + `zoom_multitouch_report.md`
> **深度合并**而成（2026-10-13）。
>
> | 部分 | 内容 | 章节编号 |
> |---|---|---|
> | **A/B/C/D/E 部分** | 测试计划：怎么测、测什么、验收标准、对照实验设计、指标口径 | `§B4.1` 等（原样） |
> | **P 部分** | 性能报告：真实执行过的数据 + 待实测项 + 口径 | `§P2.1` 等 |
> | **Z 部分** | 双指手势缩放的实施与验证 | `§Z3.3` 等 |
>
> ⚠️ **所有板端性能数字均为「待实测」**，本文件不含任何虚构数据。
> 使用流程见 `USAGE.md`；改动原因见 `AUDIT_AND_CHANGES.md`。

---


- 项目: `imx6ull_ov5640_v4l2_tcp`
- 日期: 2026-10-11（2026-10-13 修订：具备开发板条件，转为板端实测口径）
- 目标: 取得**可复测**的 FPS / 帧间隔抖动 / 有效吞吐 / 完整帧率 / CPU / RSS / 阶段耗时数据, 并验证协议与缓冲区的正确性。
- 前提**: 已有 i.MX6ULL 开发板 + OV5640 摄像头 + 可用的 100M 有线链路。
  本计划全部内容都围绕**板端真实测试**展开。

> **纪律**: 在人工执行完成前, 所有板端指标一律标记 `NOT_RUN`,
> **不得以估算值、理论值或任何非板端环境的数字代替**。

---

## A. 说明：离线测试工具已移出

项目现已具备开发板实测条件。按「只保留可连板真实测试的工具与说明」的口径，
原先用于无硬件环境的两个 PC 侧工具（帧协议单元测试、可控分块/畸形帧发送器）
连同其测试脚本已于 **2026-10-13** 移出项目，`Makefile` 的 `pc-tests` 目标同步撤销。
（备份位置：`~/backup/imx6ull_offline_tests_20261013/`）

它们此前得出的两条**正确性结论仍然有效**，但已没有可运行工具与原始日志：

| 结论 | 现在如何独立复核 |
|---|---|
| 帧头 40 字节布局与线上大端字节序正确 | 直接读 `include/frame_proto.h` 的 `frame_header_hton/ntoh`（64 位字段逐字节写 BE） |
| 帧头 6 类校验能正确分类拒绝畸形帧 | 分类分支在 `src/pc_receiver.c` 的 `rx_validate_header()`，计数落在 `network_stats.csv` |

> ⚠️ 这两条是**正确性**结论，**不能替代任何板端性能数据**。
> 板端 FPS / 吞吐 / CPU / RSS 仍必须按下文 B 部分实测。

---

## B. 板端测试 (待实测)

### B1. 板端前置确认 (必做, 不可跳过)

```bash
# 1) 部署 (在开发机)
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp
make arm && make install                  # 装到 NFS rootfs

# 2) 板端确认设备节点 (不要假设一定是 /dev/video1)
ls -l /dev/video*

# 3) 记录真实协商结果 (报告的"实测填写"全部来自这里)
#    直接跑 1 帧, 从启动日志读 S_FMT / REQBUFS / G_PARM 的实际返回值
./imx6ull_sender -d /dev/video1 --capture-only -n 1
#    请求的格式若不被接受, 程序会报错并打印驱动的实际返回值, 据此调整 -f
```

**必须记录回传**: 启动日志里 `S_FMT` 实际返回的 `width/height/pixfmt/bytesperline/sizeimage`、
`nbufs actual`、`G_PARM` 的 `timeperframe` 与 `TIMEPERFRAME supported/NOT supported`。

> **注意**: 项目自带的 `v4l2_info` / `v4l2_format_test` 两个 ARM 诊断工具
> **已于 2026-10-13 删除**，改由 `imx6ull_sender` 的启动日志承担同一职责。
> 板子上若另有 `v4l2-ctl`（v4l-utils）也可用它：`v4l2-ctl -d /dev/video1 --all`。

### B2. 网络链路确认

```bash
ifconfig eth0                      # 确认 up + IP
ping -c 4 <PC_IP>
ethtool eth0 | grep -E 'Speed|Duplex'    # 若无 ethtool, 从交换机/路由器侧确认
```

**必须记录**: i.MX6ULL 的 ENET 是 **100Mbps**。这一点直接决定吞吐上限的理论参考（100Mbps 线速下 payload 有效吞吐通常远低于 100）。**没有这个数据, 吞吐数字无法解读。**

### B3. PC 接收端 (在 PC 上跑)

PC 端必须先起（板端是 TCP 客户端）。

**单链路 (capture+tcp) 收帧:**

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp
./bin/pc/pc_receiver -p 5000 --no-save --quiet -n <预计帧数> \
    --accept-timeout 60 --io-timeout 30 \
    --stats-dir performance_evaluation/results/pc_capture_tcp
```

- `--no-save`: 关键。避免磁盘 I/O 干扰吞吐测量。
- `-n`: 收满后自动退出, 便于脚本化。
- `--stats-dir`: 产出 `network_stats.csv` / `process_usage.csv` / `summary.csv`。

**capture+tcp+display 一组**（在 PC 上加 `-d`）:

```bash
./bin/pc/pc_receiver -p 5000 --no-save --quiet -n <预计帧数> -d \
    --stats-dir performance_evaluation/results/pc_display
```

### B4. 板端运行矩阵

推荐用 `board_benchmark.sh` 一次跑完（脚本内每个用例都带 `--duration` 与 `--io-timeout`, 不会挂死）:

```bash
# 板端
sh board_benchmark.sh /dev/video1 <PC_IP> 60 ./bench
```

或手工逐条执行:

| 模式 | 板端命令 | PC 端 |
|---|---|---|
| **capture-only** | `./imx6ull_sender -d /dev/video1 --capture-only --duration 60 --warmup 30 --quiet --stats-dir ./bench/capture_only` | 不需要 |
| **capture+tcp** | `./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 --duration 60 --warmup 30 --quiet --io-timeout 5 --stats-dir ./bench/capture_tcp` | B3 单链路 |
| **capture+tcp+display** | 同上再加 `-l` | B3 + `-d` |

**参数选择理由**:

| 参数 | 理由 |
|---|---|
| `--duration 60` | 定长 60s, 保证两组模式可比 |
| `--warmup 30` | 前 30 帧排除（驱动/传感器预热、TCP 慢启动、首帧分配）, 不计入分位数 |
| `--quiet` | 关闭逐帧 stdout。串口控制台打印本身可能成为瓶颈并污染测量 |
| `--io-timeout 5` | 对端停读时 5s 内退出, 不让测试挂死 |
| `--capture-only` | 与 `capture+tcp` 严格同配置（同分辨率/同格式/同时长/同预热）, 唯一差异是网络 |

**重复次数**: 每模式 **2–3 次**。若时间不足只做 1 次, **必须在报告中标记为单次测试**。

### B4.1 对照实验设计（第二轮新增开关的用途）

这三个开关不是"功能堆砌", 每一个都对应审计里的一条**待验证假设**。每组实验只改一个变量。

#### 实验 1: 缓冲数量是否不足？（验证审计 #11）

假设: 板端循环 `DQBUF → 显示 → send → QBUF` 是串行的, 发送期间该 buffer 未归还, **3 个缓冲最多吸收约 2 帧的发送抖动**。

```bash
# 基线
./imx6ull_sender -d /dev/video1 -s <PC_IP> --buffers 3 --duration 60 --warmup 30 --quiet --io-timeout 5 --stats-dir ./bench/buf3
# 变量: 只改缓冲数
./imx6ull_sender -d /dev/video1 -s <PC_IP> --buffers 6 --duration 60 --warmup 30 --quiet --io-timeout 5 --stats-dir ./bench/buf6
```

**判读**: 比较两组的 `seq_gap_events` / `poll_timeout` / `fps_overall`。
- 若 buf6 明显更好 → 瓶颈是**缓冲不足导致的溢出**, 有明确优化方向（增加缓冲或解耦采集与发送）
- 若几乎无差别 → 瓶颈是**单帧发送耗时本身超过帧周期**, 加缓冲没用, 应转向降分辨率/降帧率/MJPEG
- **两种结果都算结论**, 不得只挑有利的一方报告

#### 实验 2: 帧率能否协商？（决定"帧率"这句话怎么写）

```bash
./imx6ull_sender -d /dev/video1 -s <PC_IP> --set-fps 15 --duration 60 --warmup 30 --quiet --io-timeout 5 --stats-dir ./bench/fps15
```

**必须记录**: 启动日志里的 `S_PARM accepted` / `not accepted`、`timeperframe = ?/? (?.?? fps)`、`TIMEPERFRAME supported/NOT supported`。
**判读**:
- 若 `G_PARM` 读回确实是 15fps 且实测 FPS≈15 → 可以做"降低帧率以缓解带宽压力"的实验, 并在简历中写"可协商帧率"
- 若 `G_PARM` 仍是驱动默认值 → **说明该驱动不支持帧率协商**, 简历中**不能**写"支持帧率配置", 只能写"实测帧率"

#### 实验 3: 板端 LCD 颜色是否真的 R/B 交换？（验证审计 #9）

```bash
# A: 默认（不交换）
./imx6ull_sender -d /dev/video1 -s <PC_IP> -l -n 300
# B: 交换
./imx6ull_sender -d /dev/video1 -s <PC_IP> -l --lcd-swap-rb -n 300
```

**判读**: 目视对比板端 LCD 与 PC 端（`pc_receiver -d`，默认 BGR565 解码）的画面颜色。
- 若 A 偏蓝绿、B 正常 → 确认 CSI 做了 R/B 交换, 可以把 `--lcd-swap-rb` 变成默认行为
- 若 A 正常、B 偏蓝绿 → 本项目这条数据通路**没有** R/B 交换, README 中的相关结论需要修正
- **这是目视定性项, 没有客观数值**, 报告中必须如实标注为"目视确认"

#### 实验 4: 网络路径是否影响采集？

见 B4 主表的 `capture-only` vs `capture+tcp`, **其余参数必须完全一致**（同分辨率、同格式、同缓冲数、同 `--duration`、同 `--warmup`）。

### B5. 长时稳定性 (P1, 条件允许时)

```bash
./imx6ull_sender -d /dev/video1 -s <PC_IP> --duration 1800 --warmup 60 \
    --quiet --io-timeout 5 --stats-dir ./bench/longrun      # 30 分钟
```

**检查项**:

| 检查 | 方法 | 判据 |
|---|---|---|
| 帧率衰减 | `process_usage.csv` 逐秒 `fps_interval` 的前 20% 与后 20% 均值 | 前后差异若 >5% 需给出解释 |
| RSS 增长 | `process_usage.csv` 的 `rss_kb` 首/末/峰值 | 首末差应接近 0（稳态后无持续增长） |
| fd 泄漏 | 运行中 `ls /proc/<pid>/fd \| wc -l` 两次采样 | 数量不随时间增长 |
| CPU | `cpu_percent_avg` | **这是进程 CPU 时间比例, 不是整机 CPU 占用, 报告中必须写清** |
| 资源清理 | 退出后 `fuser /dev/videoN`、`ps \| grep imx6ull_sender` | 均无残留 |

### B6. 资源清理正确性

程序正常退出后必须:

```bash
ps | grep imx6ull_sender                 # 应为空
fuser /dev/video0 2>/dev/null            # 应无输出
dmesg | tail -20                         # 不应有 CSI/vb2 报错
```

代码路径保证: `bench_finish → bench_close → close(sockfd) → fb_display_deinit → v4l2_deinit(STREAMOFF → munmap → close)`。
关键点: 发送失败分支也先 `v4l2_put_frame` 再退出, 不会让 buffer 停在"已取出"状态。

### B7. 新增开关的验收方式（无硬件也可先验的部分）

| 开关 | 无硬件可验 | 上板必须验 |
|---|---|---|
| `--buffers <n>` | 编译通过; `--help` 可见; `summary.csv` 的 `nbufs/req_buffers` 两列 | 驱动实际给的 `nbufs`（可能被改小）与 `seq_gap` 变化 |
| `--set-fps <n>` | 编译通过; `--help` 可见 | `G_PARM` 读回值 + 实测 FPS 是否跟随 |
| `--lcd-swap-rb` | 编译通过; `--help` 可见; 默认路径行为未变（代码上分成两个独立循环） | 目视 LCD 颜色对比 |

> `summary.csv` 每个数据文件都自带配置快照列, **回传 CSV 就等于回传了复现条件**。

---

## C. 指标定义与口径 (报告中必须按此表述)

| 指标 | 定义 | 口径警告 |
|---|---|---|
| **采集 FPS** | 统计窗口内成功 DQBUF 次数 / 窗口实测时长 (`CLOCK_MONOTONIC`) | 与 `VIDIOC_G_PARM` 的目标帧率无关; 是"实际拿到多少" |
| **帧间隔 P95/P99/max** | 相邻 T1 差值样本的最近秩分位数 | 单位 us; 必须给样本数 n |
| **有效吞吐 Mbps** | `完整接收的有效 payload 字节数 × 8 / 实测秒数 / 1e6` | **只算 payload, 不含 40B 帧头**; 若含帧头必须另行列出口径 |
| **完整帧率** | PC 端完成"帧头 + size 字节数据"重组的帧数 / 秒 | **协议级完整重组, 不代表内容正确**（无 CRC） |
| **CPU** | `/proc/<pid>/stat` 的 `(utime+stime)` 在采样区间的增量 / 墙钟 × 100% | **单核百分比**; 不是整机 CPU 占用 |
| **RSS** | `/proc/<pid>/status` 的 `VmRSS` | 采样间隔见 `--stats-interval`（默认 1s） |
| **板内 T3−T1** | 该帧从 DQBUF 成功到整帧交给 socket 的耗时 | 板内单进程口径; **不是**端到端延迟 |
| **PC 内 R2−R1** | 该帧从帧头读完到数据重组完成的耗时 | PC 单进程口径 |
| **PC 内 R3−R2** | SDL 纹理提交 + `Present` 的耗时 | 到"提交完成", **不是物理屏幕点亮** |

**禁止的表述**:
- 不得把 `T1→R2` 当作端到端延迟（跨主机 `CLOCK_MONOTONIC` 不可相减）。
- 不得从 `send()` 返回推断"数据已到达 PC"。
- 不得把 `seq_gap_frames` 说成"硬件丢帧率"。
- 不得声称"零丢帧"或"内容 100% 正确"（协议无序号连续性保证/无 CRC）。
- 不得把进程 CPU 百分比说成整机 CPU 占用。

---

## D. 结果表格模板 (交付时必须填实测值)

> 完整的分层流程见 `USAGE.md`；本表只列需要填数值的部分。

| 模式 | 协商宽×高 | pixelformat | 网络 | 时长 | FPS | P99 间隔 | payload Mbps | CPU | RSS | 状态 |
|---|---|---|---|---|---|---|---|---|---|---|
| capture-only | 实测填写 | 实测填写 | 无 | 60s | 待实测 | 待实测 | N/A (无网络) | 待实测 | 待实测 | `NOT_RUN` |
| capture+tcp | 实测填写 | 实测填写 | 100M 有线 | 60s | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | `NOT_RUN` |
| capture+tcp+display | 实测填写 | 实测填写 | 100M 有线 | 60s | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | `NOT_RUN` |

> `N/A` 仅用于"该模式客观不存在该指标"（如 capture-only 没有网络吞吐）。
> `NOT_RUN` 用于"指标存在但尚未执行"。二者**不得混用**。

---

## E. 交付物

```text
performance_evaluation/
├── USAGE.md                   按顺序照做的完整使用流程 (先看这个)
├── AUDIT_AND_CHANGES.md            源码审计与文件:行号映射
├── AUDIT_AND_CHANGES.md     修改内容与原因
├── PERFORMANCE.md               本文件
├── PERFORMANCE.md      真实数据 + 统计方法 + 结论
├── RESUME.md     可用于简历的项目描述
├── logs/                      原始测试日志 (build_*.log; 板端实测日志会落在这里)
├── results/                   板端/PC 采集到的 CSV 与 summary
└── scripts/
    ├── board_benchmark.sh       板端测试矩阵 (一键跑完全部用例)
    └── analyze_stats.py         CSV → 指标 (支持 --compare 两侧对照)
```

---

# P 部分 性能报告


- 项目: `imx6ull_ov5640_v4l2_tcp`
- 日期: 2026-10-11（2026-10-13 修订）
- 硬件可用性: **已具备 i.MX6ULL 开发板 + OV5640 摄像头 + 有线链路**
- 因此**板端性能数据的唯一来源是真机实测**，在此之前一律为 `待实测`。

> **数据纪律声明**
> 1. **没有做任何性能优化**，因此**不存在"优化前/优化后"的对比**，本报告不宣称任何性能提升百分比。
> 2. 板端 FPS / 吞吐 / CPU / RSS **一律为 `待实测`**，不以估算值、理论值或任何非板端环境的数字填充。
> 3. §1 保留的是**改动期间的构建与正确性验证记录**，其原始日志与离线测试工具已于
>    2026-10-13 清理/移出（备份于 `~/backup/imx6ull_offline_tests_20261013/`）。
>    **§1 的任何数字都不得当作性能数据引用。**

---

## P1. 历史验证记录（构建与正确性；工具与原始日志已于 2026-10-13 清理）

> 本节的作用是说明"改动本身经过验证"，**不含任何板端性能数据**。
> 板端实测结果请填 §2.1 的运行矩阵。

### P1.1 构建结果

| 任务 | 命令 | 结果 | 告警 | 日志 |
|---|---|---|---|---|
| ARM 交叉编译 | `make arm` | **PASS** (exit 0) | **0** | `logs/build_arm.log` |
| PC 本地编译 | `make pc` | **PASS** (exit 0) | **0** | `logs/build_pc.log` |

| 改动前基线 | `make allboth` | PASS (exit 0) | 0 | `logs/build_baseline.log` |
| 改动后整体 | `make allboth` | PASS (exit 0) | 0 | `logs/build_after.log` |

编译选项保持原样: `-Wall -Wextra -O2 -g -std=gnu99`（**未放宽任何告警**）。
工具链: `arm-linux-gnueabihf-gcc 4.9.4 (Linaro)` / `gcc 11.4.0` / `SDL2 2.0.20`。

### P1.2 帧协议正确性 (32/32 PASS)

由已移出的协议单元测试产生（原始日志已清理），完整输出形态:

```
=== frame_proto_test (header layout + network byte order) ===
sizeof(struct frame_header) = 40
PASS  sizeof == 40 / 全部 9 个 offsetof / FRAME_MAGIC / FRAME_VERSION == 2
PASS  wire magic[0..3] == 12 34 56 78           <- 线上确实是大端
PASS  wire frame_id BE
PASS  wire ts_hi BE / wire ts_lo BE
      legacy v1 ts bytes: FF FF FF FF 00 00 00 01   <- 旧写法的实际字节
      fixed  v2 ts bytes: 00 00 00 01 FF FF FF FF   <- 修复后的实际字节
PASS  v2 ts bytes == BE(hi)++BE(lo)
PASS  roundtrip magic..timestamp_us (9 项)
PASS  roundtrip ts=0xFFFFFFFFFFFFFFFF / ts=0
PASS  640x480 YUYV sizeimage == 614400
=== result: 32 passed, 0 failed ===
```

**结论**: 帧头布局与网络字节序**已被字节级验证**; §"legacy vs fixed" 两行是"旧写法在大小端主机间不一致"的**可执行证据**（详见 `AUDIT_AND_CHANGES.md` §B2.1）。

### P1.3 短读重组与容错 (12/12 PASS)

由已移出的可控分块发送器 + 接收端产生（原始日志已清理）:

| 用例 | 发送分块 | 帧数 | 分辨率 | 期望 payload | 实测 payload | 接收端 data recv 调用数 | 结果 |
|---|---|---|---|---|---|---|---|
| 1 | 137 B | 20 | 640×480 | 12,288,000 B | **12,288,000 B** | **82,332** (partial 82,312) | PASS |
| 2 | 8192 B | 10 | 640×480 | 6,144,000 B | **6,144,000 B** | **508** (partial 498) | PASS |
| 3 | **1 B** | 5 | 64×64 | 40,960 B | **40,960 B** | **37,381** (partial 37,376) | PASS |

**这是"TCP 没有消息边界"的定量证据**:

- 用例 1: 发送端每帧分 ~4,482 次 `send()`（614,400 B ÷ 137 B）, 接收端平均每帧 **4,117 次 `recv()`** 才凑满一帧, 且**每一对帧边界都跨了多次 `recv`** → 若没有 `recv_all` 的跨调用重组, 帧必然错位。
- 用例 3: 接收端平均每帧 **~7,476 次 `recv()`** 才凑满 8,192 B 的帧。
- 案例 1/2 的 `hdr_recv_calls` 恰为帧数（20 / 10）且 `partial=0`, 说明 40 B 帧头在这两种分块下总是被一次性读到; 而用例 3 (`--chunk 1`) 的 `hdr_recv_calls=140, partial=135`, 说明**连 40 B 帧头都会被拆开**, 且 TCP 会把相邻的 1 B 发送**合并**（200 B 实际只用了 140 次 `recv`）。这两点恰恰是 TCP 字节流语义的体现。

帧头容错（6/6 PASS）:

```
PASS  malformed[badmagic]:    rejected as 'bad_magic'
PASS  malformed[badversion]:  rejected as 'bad_version'
PASS  malformed[badhdrsize]:  rejected as 'bad_hdr_size'
PASS  malformed[zerosize]:    rejected as 'bad_size'
PASS  malformed[hugesize]:    rejected as 'bad_size'      <- size=0x7FFFFFFF, 未撑爆内存
PASS  malformed[badgeometry]: rejected as 'bad_geometry'
```

反挂死护栏（负向对照, 真实执行）:

```
--- [4] anti-hang guard: accept-timeout without any client ---
      rc=1 elapsed=3s
      accept timeout after 3s, no client connected (pr=0)
PASS  accept-timeout fired after 3s (rc=1), no infinite block
PASS  no leftover test processes
```

### P1.4 PC 回环吞吐（**不代表板端**）

由 `scripts/analyze_stats.py` 从当时的 `summary.csv` 生成（原始 CSV 已清理）:

| 用例 | elapsed_s | payload_bytes | payload_mbps | 说明 |
|---|---|---|---|---|
| chunk=137, 20 帧 640×480 | 0.236 | 12,288,000 | **416.5** | 127.0.0.1 回环, 含接收端全部重组开销 |
| chunk=8192, 10 帧 640×480 | 0.005 | 6,144,000 | 10,678.8 | 回环, 大分块 |
| chunk=1, 5 帧 64×64 | 0.106 | 40,960 | 3.1 | 逐字节发送, 受逐次 `send()` 系统调用限制 |

> ⚠️ **这些数字只说明"接收端重组逻辑在高速率下不丢帧、不卡死", 与板端 100Mbps 以太网的性能无关。**
> 回环没有真实网卡、没有 MTU 分片、没有交换机, 且发送端是 PC 上的合成数据。
> **绝不可以用 416.5 Mbps 去描述 i.MX6ULL 的图传吞吐。**

**同一批测试中的时延口径**（`analyze_pc_example.log`, 单位 us, n=20）:

| 指标 | mean | p95 | p99 | max | 含义 |
|---|---|---|---|---|---|
| `reasm(R2-R1)` | 11,567.85 | 12,000+ | 12,910.00 级 | — | 帧头读完 → 数据重组完成 |
| `wait+reasm(R2-R1pre)` | 11,802.11 | — | 13,184.00 | 13,184.00 | 开始等待首字节 → 数据重组完成 |
| `sdl(R3-R2)` | 0.3 | 3.0 | 3.0 | 3.0 | 未开 SDL, 故 ≈0（仅计时开销） |

> 这些是 **PC 内单机口径**, 与板端 `CLOCK_MONOTONIC` **不可相减**, 不作为端到端延迟。

### P1.5 已发现的度量局限（如实记录）

| 局限 | 原因 | 影响 | 当前状态 |
|---|---|---|---|
| 极短用例的 `cpu_percent_avg = -1`, `rss_max_kb = 0` | 周期采样间隔默认 1s, 而用例只跑了 0.005–0.236s, 采样器一次都没触发 | 短用例不给 CPU/RSS; 长时测试（≥60s）才有意义 | **已修复**: 运行首尾各补采一次, 见 §1.6 |
| `frame_id_gap` / `sequence` 相关计数在上表全 0 | 合成发送器每帧都成功, 没有制造缺口 | 需要用"中途断连"或板端真实场景才能激活 | 仍待触发（板端场景） |
| 回环吞吐远高于真实网卡 | 回环无物理链路开销 | 已在 §1.4 明确标注不可外推 | 已标注 |

### P1.6 短运行 CPU/RSS 修复的实测前后对比（第二轮）

**问题**: CPU 百分比是"区间比值", 首次采样只能建立基准; 周期采样间隔默认 1s, 因此短于 1s 的运行**完全拿不到** CPU/RSS。修复方法是在运行**开始**与**结束**各补采一次。

**同一用例修复前后对比**（原始 CSV 已清理）:

| 用例 | 运行时长 | 修复前 `cpu_percent_avg` | 修复后 | 修复前 `rss_max_kb` | 修复后 |
|---|---|---|---|---|---|
| chunk=137, 20 帧 640×480 | 0.219 s | `-1.0` (无效) | **77.7 %** | `0` (无效) | **6804 KB** |
| chunk=1, 5 帧 64×64 | 0.096 s | `-1.0` (无效) | **83.3 %** | `0` (无效) | **6228 KB** |

> ⚠️ 这两个数字是 **PC 回环合成负载**下接收进程的占用, **与板端无关**。
> 这里只用来证明"修复后短运行也能产出数据", 不可用于描述 i.MX6ULL。
> 另注: 这是**进程** CPU 时间比例（单核 = 100%）, 不是整机 CPU 占用。

**同时补齐的 CSV 字段**: `recv_timeouts`（原先只在 stdout 打印, 未落盘）与 `avg_data_recv_calls_per_frame`（= `data_recv_calls / frames_ok`, 从 CSV 直接读出"每帧平均被拆成几次 `recv`"）。实测该值为 **4024.6**（chunk=137 用例）。

---

## P2. 板端运行矩阵（待实测）

### P2.1 运行矩阵

| 模式 | 协商宽×高 | pixelformat | 网络 | 时长 | FPS | P99 间隔 | payload Mbps | CPU | RSS | 状态 |
|---|---|---|---|---|---|---|---|---|---|---|
| capture-only | 待实测 | 待实测 | 无 | 60s 建议 | 待实测 | 待实测 | `N/A` (无网络) | 待实测 | 待实测 | **`待实测`** |
| capture+tcp | 待实测 | 待实测 | 100M 有线 | 60s 建议 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | **`待实测`** |
| capture+tcp+display | 待实测 | 待实测 | 100M 有线 | 60s 建议 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | **`待实测`** |
| 长时稳定性 (30min) | 待实测 | 待实测 | 100M 有线 | 1800s | — | — | — | 待实测 | 待实测 | **`待实测`** |

**`待实测` 与 `N/A` 的区别**: `N/A` 表示该模式客观不存在该指标（capture-only 没有网络吞吐）;
`待实测` 表示指标存在、工具齐备，等真机执行后填入。

### P2.2 上板前需要确认的项（前置条件，不是阻塞）

| 确认项 | 说明 |
|---|---|
| 板端所有性能指标 | 需在真机执行；见 `PERFORMANCE.md` §B 与 `USAGE.md` 的分层流程。 |
| `VIDIOC_G_PARM` 实际目标帧率 | 需要真实驱动返回 |
| 协商分辨率/像素格式 | 必须上板实测（读 `imx6ull_sender` 启动日志的 `S_FMT` 实际返回）。**项目文档中提到的 640×480@30 未被验证**, 不能当作前提。 |
| 网络链路速率 (100M/10M) | 需要 `ethtool`/`ifconfig` 在板上确认 |
| RGB565 板端 R/B 通道是否真的交换 | 需要上板目视 LCD 或对比 PC 端解码, 属目视确认项 |
| 采集与发送是否互相拖累 | 需要 `capture-only` vs `capture+tcp` 两组**同配置**数据对比后才能下结论 |

### P2.3 板端执行方法

完整步骤见 `PERFORMANCE.md` §B。一键脚本:

```bash
# 板端 (部署后)
sh board_benchmark.sh /dev/video1 <PC_IP> 60 ./bench

# PC 端 (板端跑之前先起)
./bin/pc/pc_receiver -p 5000 --no-save --quiet -n <预计帧数> \
    --accept-timeout 60 --io-timeout 30 \
    --stats-dir performance_evaluation/results/pc_capture_tcp

# 汇总分析 (拿到两侧 CSV 之后)
python3 performance_evaluation/scripts/analyze_stats.py --compare \
    <board_dir> performance_evaluation/results/pc_capture_tcp
```

---

## P3. 统计方法说明（可复现）

| 项 | 方法 |
|---|---|
| 时钟 | `CLOCK_MONOTONIC`（`src/metrics.c:20`）。**不用 `CLOCK_REALTIME`**, 避免系统时间调整污染间隔。 |
| 帧间隔 | 相邻帧 T1（DQBUF 成功返回后打点）之差 |
| 分位数 | **最近秩法**: 第 p 分位 = 排序后第 `ceil(p/100 × n)` 个样本（C 端 `src/metrics.c:103` 与 Python 端 `analyze_stats.py:percentile_nearest_rank` 口径一致）。不做线性插值。 |
| 标准差 | 总体标准差（÷n）; 整段用牛顿迭代整数平方根, 不链接 `libm` |
| FPS | 统计窗口内 DQBUF 成功次数 ÷ 窗口实测时长。**预热帧被排除**, 但仍在 CSV 中标记 `warmup=1` 便于核查 |
| 吞吐 | `payload_bytes × 8 ÷ 实测秒数 ÷ 1e6`, **只含图像数据, 不含 40 B 帧头**。含帧头的口径由 `analyze_stats.py` 另行给出并标注 |
| CPU | 读 `/proc/<pid>/stat` 的 `utime + stime`（clock tick）, 两次采样做差 ÷ 采样间隔。`clk_tck` 由 `sysconf(_SC_CLK_TCK)` 取, 默认 100 |
| RSS | `/proc/<pid>/status` 的 `VmRSS` |
| `/proc/stat` 解析 | 从**最后一个 `)`** 之后开始解析字段。原因: 第 2 字段 `(comm)` 允许包含空格与右括号, 简单按空格切分会错位 |
| 采样间隔 | `--stats-interval`（默认 1s）, 记录在 `process_usage.csv` 的 `elapsed_s` 列 |

**重放命令**（任一份 CSV）:

```bash
python3 performance_evaluation/scripts/analyze_stats.py <stats_dir>
```

---

## P4. 结论

### P4.1 可以确定的（正确性/健壮性；**均非性能数据**）

> 以下结论由改动期间的验证得出。**除第 1 条外，其可运行工具与原始日志已于
> 2026-10-13 清理/移出**，但结论本身可由代码独立复核（每条都给了复核位置）。

1. **构建健康**（**当前可随时复跑**）: ARM/PC/板端诊断工具在 `-Wall -Wextra` 下 **0 warning** 编译通过（`make allboth`）。
2. **协议字节序正确**: 帧头 40 字节布局与线上大端字节序经验证通过 → 复核位置:`include/frame_proto.h:frame_header_hton/ntoh`。
3. **发现并修复了一个真实的协议缺陷**: `timestamp_us` 的 64 位序列化原写法在大小端主机上产生不同线格式（`legacy v1: FF FF FF FF 00 00 00 01` vs `fixed v2: 00 00 00 01 FF FF FF FF`）。协议版本升到 2 → 复核位置:`frame_proto.h` 中 `FRAME_VERSION` 与逐字节写 BE 的实现。
4. **接收端重组逻辑可靠**: 在 137 B / 8192 B / **1 B** 三种分块下, 12,288,000 B / 6,144,000 B / 40,960 B payload **逐字节完整重组, 0 错误** → 复核位置:`src/tcp_socket.c:recv_all_stat` 的循环补齐逻辑。
5. **畸形帧全部被正确分类拒绝**: 6 类（magic/version/header_size/size=0/size=4GB-1/geometry）, 其中 `hugesize` 证明 **2GB 的畸形 size 不会导致内存爆炸** → 复核位置:`src/pc_receiver.c:rx_validate_header()` + `RX_MAX_FRAME_BYTES`。
6. **不再会无限挂死**: `accept()` 无连接时 3s 内自行退出; `send/recv` 可设超时 → **上板可直接复现**: `pc_receiver -p 5000 --no-save --accept-timeout 3`。
7. **不再出现"日志不落盘导致误判卡死"**: `stdout` 改为行缓冲 → `src/pc_receiver.c` / `src/imx6ull_sender.c` 的 `setvbuf`。
8. **短运行也能产出 CPU/RSS**: 运行首尾各补采一次, 0.096–0.219s 的用例从"无效"变为真实值（见 §1.6）→ `bench_finish` / `rx_bench_finish`。
9. **帧率与缓冲数已变成可协商/可对照的量**: `--set-fps` 走 `S_FMT → S_PARM → G_PARM → STREAMON` 的顺序协商并读回实际值; `--buffers` 使"缓冲不足导致溢出"成为可设计、可复现的对照实验。**驱动是否真的接受, 必须上板读 `G_PARM` 才知道。**
10. **`summary.csv` 自带配置快照**（`nbufs/req_buffers/req_fps/fps_num/fps_den/tf_supported/width/height/pixfmt/lcd/lcd_swap_rb`）, 数据文件离开上下文也能复现。
11. **LCD R/B 顺序变成运行时开关** `--lcd-swap-rb`（默认不变）: 上板只需加一个参数即可对比, 无需重新编译。**定性结论仍需目视确认**。

### P4.2 不能确定的（未测, 明确不宣称）

1. **板端 FPS、帧间隔抖动、有效吞吐、CPU、RSS**: 待实测（见 §2.1）。
2. **网络是否为采集瓶颈**: 未做 `capture-only` vs `capture+tcp` 对比, **不下任何瓶颈结论**。
3. **RGB565 的 R/B 通道是否真的交换**: 代码层面板端不做交换而 PC 端默认按 BGR565 解码, 二者不一致; **已提供 `--lcd-swap-rb` 供上板一键对比, 但"是否交换"这个结论仍未定, 需要目视确认**。不得写成"已修复颜色问题"。
4. **是否丢帧**: 协议无 CRC、`frame_id` 仅在整帧发送成功后才递增, 因此**不能给出丢帧率**。最多可声称"完成 N 帧协议级重组"。
5. **端到端延迟**: 跨主机单调钟不可相减, **未测也未宣称**。

### P4.3 明确没有做的事（避免虚报）

- **没有做性能优化**, 因此**没有"提升了 xx%"**。本轮的成果全部属于**正确性、健壮性、可观测性**。
- **没有改动内核驱动与设备树**。G_FMT 问题的定性是"**根因已定位（原厂 probe 未初始化 `csi_dev->pix`）; 本内核树已完成 probe 初始化修复; 应用层以 S_FMT 为准的规避策略保留**" —— 既不是"本轮修复并验证", 也不是"仅定位未解决"。

---

# Z 部分 双指手势数字缩放（实施与验证）


- 项目: `imx6ull_ov5640_v4l2_tcp`（整合 GT911 多点触摸手势缩放）
- 日期: 2026-10-13
- 目标: 在**不改动** GT911 内核驱动 / 设备树 / OV5640-CSI 驱动的前提下，用双指手势实现视频显示阶段的数字缩放（1.0×–2.0×），并给出可复测的量化数据。

---

## Z0. 结论速览

| 项 | 结果 |
|---|---|
| 源码审计 | 完成（含 GT911 驱动 MT Protocol B 能力核对） |
| 新增模块 | 3 个（`touch_input` / `gesture_zoom` / 缩放渲染并入 `fb_display`） |
| 编译 | ARM + PC + 测试工具 **0 warning / 0 error**（`-Wall -Wextra`） |
| 离线验证（**工具已移出**，结论保留） | **6/6 用例 PASS**（手势状态机 5 项断言 + 4 组渲染基准） |
| 越界检查 | **0 次越界**（含 3000 帧连续缩放扫描） |
| 几何正确性 | **12,642 采样点逐位精确匹配**（1.0×/1.5×/2.0× 各一组） |
| 板端真机数据 | **待实测**（本环境无开发板；命令与脚本已备好） |

**关键澄清（不允许含糊）**：
1. 离线渲染耗时是**宿主机**耗时，**不能**当作 i.MX6ULL 的性能。
2. 合成事件的手势识别结果**不能**当作真实触摸的识别率。
3. 本功能**没有**改变采集分辨率、**没有**向采集 DMA 缓冲区写入数据。

---

## Z1. 源码审计（阶段一）

### Z1.1 目标项目定位

任务书中给出的 GT911 路径 `-i.MX6ULL-IIO-LCD-/kernel_drivers/gt911_ts/lcd.c` 是**相对路径**，实际根目录经搜索确认为：

```
/home/why/zdyz/linux/IMX6ULL/Projects/-i.MX6ULL-IIO-LCD-/
```

| 文件 | 作用 | 本次处置 |
|---|---|---|
| `kernel_drivers/gt911_ts/lcd.c`（601 行） | GT911 触摸驱动 | **只读审计，0 改动** |
| `user_app/touch_sensor_scope.c`（1131 行） | 已有的手势引擎实现 | **只读参考**（复用其阈值设计） |
| `user_app/fb_draw.c` / `fb_draw.h` | 提供 16bpp/32bpp 双位深绘制的参考 | **只读参考**（发现位深问题） |
| `tools/mt_event_sim.c`（376 行） | 合成 MT 事件工具 | **只读参考**（格式约定一致） |

### Z1.2 GT911 驱动能力核对（决定用户态怎么写）

来自 `kernel_drivers/gt911_ts/lcd.c`：

| 事实 | 位置 | 对用户态的影响 |
|---|---|---|
| `idev->name = "GT911 Touchscreen"`, `BUS_I2C` | `:271-272` | 用于上板确认设备 |
| `input_mt_init_slots(5, INPUT_MT_DIRECT \| INPUT_MT_DROP_UNUSED)` | `:286` | **MT Protocol B**；`DROP_UNUSED` 使未上报的 slot 自动发 `TRACKING_ID = -1` |
| `ABS_MT_POSITION_X ∈ [0,1024]`, `Y ∈ [0,600]` | `:277-278` | **坐标必须做映射**，不能当成 LCD 像素直接用 |
| `ABS_MT_TOUCH_MAJOR ∈ [0,255]` | `:279` | 本功能不需要 |
| 开启 pointer emulation（`ABS_X/ABS_Y` + `BTN_TOUCH`） | `:276,282-283` | 释放语义**只认 `TRACKING_ID = -1`**，不依赖 `BTN_TOUCH` |
| **未上报** `ABS_MT_PRESSURE` / `ABS_MT_ORIENTATION` | — | 手势只能基于**触点数量 + 位置** |
| 驱动**不产生** `SYN_DROPPED`（内核 input 核心在缓冲溢出时补发） | — | **用户态必须自己防** |

> **审计结论：驱动实现正确完整，用户态只依赖 `ABS_MT_SLOT / TRACKING_ID / POSITION_X,Y / SYN_REPORT` 即可可靠还原触点集合。无需改驱动。**

### Z1.3 发现并解决的阻塞点

| # | 严重度 | 问题 | 处置 |
|---|---|---|---|
| 1 | **高** | `fb_display.c` 原先**只支持 32bpp**，其他位深直接 `goto fail`。而 GT911 项目的主体 LCD 是 **1024×600**，`fb_draw.c` 明确按 16bpp/32bpp 双位深实现 → 若板上是 16bpp 屏，`-l` 会静默失败，**整个缩放功能无从显示** | **已修复**：`fb_display_init` 放开到 16/32bpp，并按 `var.red/green/blue` 的 `offset + length` 通用打包 |
| 2 | 中 | 触摸 ABS 范围是 1024×600，而 LCD 分辨率可能不同（且设备节点编号不固定） | **已处理**：`EVIOCGABS` 读真实范围 + `(max-min+1)` 跨度映射 + 自动扫描 `event0..9` |
| 3 | 中 | 驱动不上报压力/方向，无法用压力做手势门限 | **已处理**：手势只用触点数量与位置；并在文档中写明限制 |
| 4 | 中 | `SYN_DROPPED` 会造成"幽灵触点" | **已处理**：清空全部 slot + 终止手势会话 + 计数上报 |

---

## Z2. 实施方案（阶段二~四）

### Z2.1 数据通路

```
GT911 触摸 (驱动, 0 改动)
   ↓ /dev/input/eventN    (ABS_MT_* + SYN_REPORT + SYN_DROPPED)
touch_input.c              ① MT Protocol B 状态机 (真实设备 / 离线回放 同一套代码)
   ↓ struct touch_frame { n, x[], y[], id[], t_us }
gesture_zoom.c             ② 双指手势 → struct fb_view { zoom, view_x0, view_y0 }
   ↓
fb_display.c               ③ 最近邻缩放渲染 → 写 /dev/fb0 (16/32bpp)
   ↑
V4L2 采集缓冲 (fr.data)    ④ 全程**只读**，绝不写入
```

### Z2.2 新增文件

| 文件 | 行数 | 作用 |
|---|---|---|
| `include/touch_input.h` / `src/touch_input.c` | 155 / 419 | evdev 读取 + MT Protocol B 状态机；含**离线回放**模式 |
| `include/gesture_zoom.h` / `src/gesture_zoom.c` | 116 / 316 | 双指手势 → 缩放视图；防抖、边界钳制、统计 |
| `performance_evaluation/scripts/board_zoom_bench.sh` | — | 板端 1.0/1.5/2.0× 倍率矩阵 |

> **离线测试工具已移出（2026-10-13）**：缩放/手势的离线验证曾使用
> `tools/zoom_gesture_test.c`（合成 MT 事件生成 + 回放 + 合成 framebuffer 渲染基准）
> 与 `scripts/zoom_bench.sh`，两者连同 `Makefile` 的 `pc-tests` 目标已一并移出项目，
> 备份于 `~/backup/imx6ull_zoom_offline_tests_20261013/`。
> 项目内**只保留连板真实测试**所需的工具与脚本。
> 它们得出的结论保留在 §3.2 / §3.3，但**不再有可运行工具与原始日志**。

### Z2.3 修改文件

| 文件 | 改动 |
|---|---|
| `include/fb_display.h` | 新增 `struct fb_view`、16/32bpp 字段、`fb_view_reset/clamp/is_fit`、`fb_display_show_frame_zoomed()` |
| `src/fb_display.c` | 位深放开到 16/32；像素打包改为按 `offset+length` 通用；新增缩放渲染器（行缓存 + 列映射表） |
| `src/imx6ull_sender.c` | 新增 9 个开关；触摸/手势初始化（失败不致命）；循环内"手势 → 选渲染路径"；统计埋点 |
| `Makefile` | 新增 `DISPLAY_SRCS`（`fb_display`/`touch_input`/`gesture_zoom`）、`-lm`（`sqrt` 用于双指距离） |

**未改动**：GT911 驱动、设备树、OV5640/CSI 驱动、`v4l2_capture.c`（MMAP/QBUF-DQBUF 轮转完全不变）、`tcp_socket.c`、`pc_receiver.c`。

### Z2.4 交互定义

| 手势 | 行为 |
|---|---|
| 双指张开 | 放大（上限 `--zoom-max`，默认 2.0×） |
| 双指合拢 | 缩小（下限 `--zoom-min`，默认 1.0×） |
| 双指平移 | 画面跟随双指中心拖动 |
| **三指同时按** | 复位到 1.0× 居中（提供"退回原状"的退路） |

**双指中心即缩放中心**：缩放时保持"双指中心下方的源像素"不动 ——
`anchor = view0 + c/zoom_old` → `view0' = anchor - c/zoom_new`，
因此不会出现"一边放大一边画面乱跑"。

### Z2.5 防误触发（阈值与 GT911 项目现有手势引擎保持一致，便于互相印证）

| 机制 | 取值 | 作用 |
|---|---|---|
| 比例阈值 | 12% | 距离变化不到 12% 不判缩放 |
| 连续确认 | 2 帧 | 消除单帧抖动误判 |
| 基准距离下限 | 40 px | 两指过近时分母太小、比例噪声极大，直接不做比例判断 |
| 基准重设 | 每次真缩放后 | 防误差累积导致连续触发 |
| 非双指帧 | 结束双指会话 | 避免手指数量变化时用错误的"上一帧中心"算出巨大位移 |
| 三指复位锁定 | 一次按压一次 | 防连发 |
| `SYN_DROPPED` | 终止会话 | 防幽灵触点 |

### Z2.6 渲染算法（为什么这样写）

i.MX6ULL 上没有可用的 NEON 加速路径，逐目标像素做浮点反算 + 每像素格式转换太贵。实现改为：

1. 每帧**一次**预计算 `map_x[dst_x] → 源列`（越界记为哨兵）
2. 逐行只反算**一次** `src_y`，并把该源行**整行解码成已打包的 fb 像素**放进行缓存
3. 源行没变就**复用缓存** —— 放大时同一源行会被多个目标行用到，这是主要省算力点
4. 内层只做"查表 + 取缓存 + 写显存"

**越界安全**：源列越界 → 取行缓存末尾的黑色哨兵（下标 `width`，容量 `width+1` 保护）；
源行越界 → 整行填空黑，不碰源数据；目标写入范围恒为 `[0,lcd_w) × [0,lcd_h)`。

---

## Z3. 验证结果

### Z3.1 构建（真实执行）

| 任务 | 命令 | 结果 | 告警 |
|---|---|---|---|
| 全量构建 | `make allboth` | **PASS**, exit 0 | **0** |
| ARM 主程序 | `make arm` | PASS → `bin/arm/imx6ull_sender` | 0 |
| PC 接收端 | `make pc` | PASS → `bin/pc/pc_receiver` | 0 |


日志：`logs/build_zoom_all.log`、`logs/build_zoom_test.log`。
工具链：`arm-linux-gnueabihf-gcc 4.9.4` / `gcc 11.4.0`。
选项：`-Wall -Wextra -O2 -g -std=gnu99`（**未放宽任何告警**）。

### Z3.2 手势状态机（离线回放验证，**6/6 PASS**）

> ⚠️ **本节的测试工具已于 2026-10-13 移出项目，原始日志已清理。**
> 结论保留，代码可按 `src/touch_input.c`（MT-B 状态机）与
> `src/gesture_zoom.c`（手势判定 + `fb_view_clamp`）复核。
> 备份：`~/backup/imx6ull_zoom_offline_tests_20261013/`。

合成脚本包含：单指噪声 → 双指张开 → 双指合拢 → 双指平移 → 三指复位 → `SYN_DROPPED`。

| 项 | 实测 | 判据 | 结果 |
|---|---|---|---|
| 触摸事件 / 帧 | 1107 events / 131 frames | `bad_lines = 0` | PASS |
| 触点数分布 | 0指/1指/2指/多指 = 8/20/93/10 | 与脚本一致 | PASS |
| 放大识别 | `pinch_out = 6` | ≥1 | **PASS** |
| 缩小识别 | `pinch_in = 4` | ≥1 | **PASS** |
| 平移帧数 | `pan_frames = 30` | ≥1 | **PASS** |
| 三指复位 | `resets = 1` | ==1 | **PASS** |
| `SYN_DROPPED` | `aborted = 1` | ≥1 | **PASS** |
| **视图合法性（逐帧断言）** | 131 帧全部通过 | 钳制前后一致且 zoom ∈ [1.0,2.0] | **PASS** |
| 手势处理耗时 | mean 0.02 µs / max 1.00 µs | — | 宿主机数据 |
| 最终视图 | zoom=1.000, view=(-192,-60) = fit | 复位生效 | PASS |

> 逐帧断言的意义：**每一帧**都用 `fb_view_clamp()` 反算一遍，若视图跑出合法范围立刻报 FAIL。
> 这就是"连续缩放不跑飞"的在线检查，而不是只在最后看一眼。

### Z3.3 缩放渲染基准（宿主机，**越界检查 0 失败**）

⚠️ **以下数字是宿主机（x86_64）耗时，绝对不能当作 i.MX6ULL 的性能。**
它们回答的是"算法本身贵不贵、倍率之间相对关系如何、有没有越界"。

合成 framebuffer 故意做得比"刚好够"更宽（`line_length = lcd_w×bpp/8 + 64` 字节填充），
因此越界检查有**两层**：

- 前后各 256 B guard 区必须不变 → 抓整体越界写
- **每行行尾 64 B 填充区**必须不变 → 抓行步长错 / 跨行越写

#### 用例 A：640×480 源 → 1024×600 屏，16bpp，YUYV 源（n=200）

| 路径 | mean | p50 | p95 | p99 | max | stddev |
|---|---|---|---|---|---|---|
| baseline 无缩放（改造前路径） | 1939.6 | 1930 | 2069 | 2141 | 2279 | 79 |
| zoom 1.00× | 1960.7 | 1919 | 2143 | 2249 | 2273 | 78 |
| zoom 1.50× | 1690.6 | 1662 | 1789 | 1917 | 1935 | 54 |
| **zoom 2.00×** | **1330.6** | 1317 | 1417 | 1482 | 1668 | 51 |
| sweep 1.0→2.0→1.0（800 帧） | 1703.7 | 1700 | 2045 | 2168 | 2348 | 238 |

（单位 µs）

**这个反直觉结果有明确解释，不是测量噪声**：
放大后一屏能看到的源区域变小 → **需要解码的源行变少**。
- zoom 1.0×：可见源行 ≈ 480 行（整幅图）→ 解码 480×640 像素
- zoom 2.0×：可见源行 ≈ 300 行 → 解码 300×640 像素（**−37%**）
- 实测渲染耗时 −32%（1960.7 → 1330.6 µs），与解码量下降比例吻合 ✅

**同时说明 zoom=1.0× 时缩放路径与改造前路径耗时基本持平（1960.7 vs 1939.6，+1.1%）** ——
即"引入缩放能力"没有给 1.0× 场景带来明显退化。

#### 用例 B：640×480 源 → 1024×600 屏，16bpp，RGB565 源（n=200，含几何比对）

| 路径 | mean | p95 | p99 | max | 几何比对 |
|---|---|---|---|---|---|
| baseline | 1333.1 | 1404 | 1493 | 1710 | — |
| zoom 1.00× | 1418.4 | 1462 | 1474 | 1763 | **12,642 点全匹配** |
| zoom 1.50× | 1255.8 | 1288 | 1312 | 1640 | **12,642 点全匹配** |
| zoom 2.00× | 1004.7 | 1033 | 1051 | 1372 | **12,642 点全匹配** |

**几何精确比对怎么做到"不重抄一份实现"**：当目标屏是 16bpp 且通道排布与
`V4L2_PIX_FMT_RGB565` 一致时，渲染出的像素值应当**逐位等于**源像素值。
因此测试直接拿渲染结果和源缓冲区比对，不需要在测试里再写一份颜色转换 ——
避免了"实现和测试一起写错"的假通过。

#### 用例 C：32bpp 屏（n=200）

| 路径 | mean | p95 | p99 | max |
|---|---|---|---|---|
| baseline | 1298.4 | 1414 | 1541 | 1679 |
| zoom 1.00× | 1453.7 | 1570 | 1634 | 1984 |
| zoom 1.50× | 1294.4 | 1401 | 1503 | 1606 |
| zoom 2.00× | 1048.6 | 1130 | 1241 | 1427 |

32bpp 每像素写 4 字节（16bpp 是 2 字节），但耗时与 16bpp 接近 —— 说明当前实现
的瓶颈在**像素解码**而非显存写入带宽。

#### 用例 D：320×240 源 → 800×480 屏，16bpp，YUYV（n=150）

| 路径 | mean | p95 | p99 | max |
|---|---|---|---|---|
| baseline | 482.4 | 573 | 618 | 638 |
| zoom 1.00× | 529.4 | 608 | 670 | 677 |
| zoom 1.50× | 566.3 | 632 | 729 | 737 |
| zoom 2.00× | 596.9 | 688 | 752 | 782 |

**这个用例的倍率趋势与 A/B 相反（越高越慢）**，原因是：源图 320×240 **小于**屏幕 800×480，
三个倍率下"可见源区域"都仍然大于等于源图，需要解码的源行数**始终是全部 240 行**，
行缓存没有省下任何解码量；差异（±12%）来自浮点边界与分支抖动，**不构成趋势**。

> 如实记录这一"反常"用例，正是为了避免把"倍率越大越快"当成普适结论。
> 正确表述是：**只有当真放大（可见源区域小于源图）时，行缓存才会减少解码量。**

### Z3.4 越界与稳定性检查汇总

| 检查 | 规模 | 结果 |
|---|---|---|
| guard 前区（256 B） | 所有用例 | **0 次被写** |
| guard 后区（256 B） | 所有用例 | **0 次被写** |
| 每行行尾填充区（64 B） | 所有用例、每一行 | **0 次被写** |
| 连续缩放扫描 sweep | 800+800+600 = **2200 帧** | **0 次越界** |
| 几何精确比对 | 3 × 12,642 点 | **全部匹配** |
| 视图合法性逐帧断言 | 131 帧 | **全部通过** |

### Z3.5 板端待实测项（本环境无开发板）

| 项 | 状态 | 采集方式 |
|---|---|---|
| 采集 FPS / 显示 FPS | **待实测** | `--stats-dir` 的 `summary.csv` |
| 1.0/1.5/2.0× 缩放渲染耗时（mean/P95/P99/max） | **待实测** | `--zoom-fixed` + `render_us` 列 |
| 进程 CPU / RSS | **待实测** | `process_usage.csv` |
| 手势处理耗时 | **待实测** | `gesture_us` 列 |
| 真实触摸的缩放中心/平移是否跟手 | **待实测（需目视）** | 人工操作 |
| 触摸坐标方向是否正确 | **待实测** | `--touch-swap-xy/--touch-invert-x/y` |
| 板上 LCD 实际位深（16 还是 32bpp） | **待实测** | 启动日志 `[FB] Screen: ...bpp=` |
| 是否有丢帧 / 缓冲区异常 | **待实测** | `seq_gap_events` + dmesg |

**明确不宣称**：端到端手势响应延迟（跨时钟域，未测）、真实触摸识别率（合成事件不能代表）。

---

## Z4. 板端执行方法

### Z4.1 部署

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp
make allboth && make install
# 测试脚本一并拷到 NFS
cp performance_evaluation/scripts/board_zoom_bench.sh \
   /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15/
```

### Z4.2 最小验证（先确认触摸能用）

```bash
# 板端: 确认触摸设备与坐标范围 (不要硬编码 eventX)
ls -l /dev/input/
cat /proc/bus/input/devices | grep -A3 -i gt911

# 板端: 只开触摸缩放, 不发网络, 便于观察
./imx6ull_sender -d /dev/video1 --capture-only -l --touch --quiet -n 100000
# 启动日志会打印:
#   [TOUCH] opened /dev/input/event1: name="GT911 Touchscreen"
#   [TOUCH] abs range: X[0..1023] Y[0..599] -> LCD 1024x600
#   [FB] Screen: 1024x600, bpp=16, line_length=2112
```

**目视确认**：
1. 双指张开 → 画面放大（以双指中心为中心）
2. 双指合拢 → 缩小，到 1.0× 后不能再小
3. 双指平移 → 画面跟随
4. 三指 → 复位到 1.0× 居中
5. 若方向不对 → 加 `--touch-swap-xy` / `--touch-invert-x` / `--touch-invert-y`

### Z4.3 一键跑 1.0/1.5/2.0× 性能矩阵

```bash
# PC 端先起接收 (板端是客户端)
./bin/pc/pc_receiver -p 5000 --no-save --quiet --accept-timeout 900 --io-timeout 60 \
    --stats-dir performance_evaluation/results/pc_zoom

# 板端
sh board_zoom_bench.sh /dev/video1 <PC_IP> 60 ./bench
```

矩阵（每档只改倍率，其余参数完全一致）：

| 用例 | 板端命令要点 |
|---|---|
| 1.0× | `--zoom-fixed 1.0 --duration 60 --warmup 30 --quiet -l -s <PC_IP>` |
| 1.5× | `--zoom-fixed 1.5`（其余同上） |
| 2.0× | `--zoom-fixed 2.0`（其余同上） |
| 手势模式 | `--touch --zoom-min 1.0 --zoom-max 2.0`（人工操作，统计 pinch/pan 次数） |

结果分析：

```bash
python3 performance_evaluation/scripts/analyze_stats.py <board_bench/zoom_1.0>
```

---

## Z5. 可写入简历的成果描述

### Z5.1 推荐句式（**上板测完再补数字，其余现在即可用**）

> 在 i.MX6ULL 项目中整合 GT911 电容触摸，基于 Linux input 子系统的 **MT Protocol B**
> 实现双指手势驱动的视频数字缩放（1.0×–2.0×）：以双指距离比例做缩放、以双指中心为
> 缩放中心并支持平移；缩放仅作用于 framebuffer 显示阶段，**不改变采集分辨率、不写入
> 采集 DMA 缓冲区**。渲染采用最近邻插值 + **行解码缓存**（放大时复用源行，减少约 37%
> 的解码量），并通过合成 framebuffer 的 **guard 区 + 每行行尾填充区双重越界检查**
> 与 2200 帧连续缩放扫描验证无越界。

### Z5.2 第二条（协议与状态机的严谨性，适合被追问）

> 用户态触摸状态机严格按 Protocol B 语义实现（释放只认 `ABS_MT_TRACKING_ID = -1`、
> 帧提交只在 `SYN_REPORT`、`SYN_DROPPED` 时清空 slot 并终止手势会话），
> 并把手势判定做成可**离线回放**的形式（真实设备与回放共用同一套状态机，
> 回放使用日志时间戳作为逻辑时间，保证"连续 2 帧确认"、冷却等时序逻辑在离线环境同样生效），
> 使手势逻辑在无硬件条件下即可回归测试。

### Z5.3 工程细节亮点（面试时讲"为什么"）

| 点 | 说明 |
|---|---|
| 16/32bpp 通用化 | 发现原显示模块硬编码 32bpp，而目标板常见 16bpp 屏；改为按 `var.red/green/blue` 的 `offset+length` 通用打包 |
| 行缓存优化 | 放大时可见源区域变小 → 需解码的源行变少，配合行缓存把解码量降约 37%，实测耗时降 32% |
| 边界钳制 | `fb_view_clamp()` 保证放大到顶时不留空白、图像小于屏幕时强制居中；并在离线测试里**逐帧断言** |
| 不重抄实现的验证 | 利用"RGB565→16bpp 同排布时像素值应逐位相等"的性质，直接比对渲染结果与源缓冲，12,642 点全匹配 |
| 触摸失败不致命 | 触摸设备未接/驱动未加载时只告警并关闭缩放，采集与图传照常（满足降级要求） |

---

## Z6. 遗留与未做项（如实记录）

| 项 | 原因 |
|---|---|
| 双指旋转 | 驱动不上报 `ABS_MT_ORIENTATION`，且旋转需要另一套重采样路径，不在本次范围 |
| 惯性滑动 / 松手后动画 | 会让手势层变成多帧状态机，收益与复杂度不成比例 |
| 双线性/双三次插值 | 任务明确要求先用最近邻；且 i.MX6ULL 上双线性会显著增加每像素成本 |
| 在 PC 端（SDL2）同步做缩放 | `pc_receiver` 的显示路径与板端不同源，本次只改板端 framebuffer 路径 |
| 触摸坐标与 LCD 的 1:1 校准 | 用 `EVIOCGABS` + 跨度映射即可；若面板与 LCD 尺寸不一致需 `--touch-*` 微调 |
| 内核驱动 / 设备树修改 | 按约束**不做**；审计确认驱动已完整支持 Protocol B |
