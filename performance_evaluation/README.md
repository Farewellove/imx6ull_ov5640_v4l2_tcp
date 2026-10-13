# 交付材料总索引

> **这是 `performance_evaluation/` 的唯一入口。** 只看这一页就能知道：
> 项目做了什么、哪些结论已经有证据、哪些必须上板测、每一份文档回答什么问题。
>
> 路径基准：本文中的相对路径以 `performance_evaluation/` 为基准；
> 命令一律在**项目根目录**执行（`/home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp`）。

---

## 0. 这个项目是什么

i.MX6ULL + OV5640 的 V4L2 采集 →（可选 LCD 显示 + GT911 双指手势缩放）→ 自定义帧协议 TCP 图传 → PC 端接收/显示/落盘。

```
OV5640 ──V4L2 MMAP──┬─► 40B 帧头 + 裸图像 ──TCP──► pc_receiver ──┬─► 落盘 raw/PPM
                    │                                            └─► SDL2 窗口
                    └─► /dev/fb0 显示 ◄── 双指手势数字缩放 ◄── GT911 (/dev/input)
```

**两端必须同时编译部署**：帧协议为 **版本 2**，与版本 1 的 64 位时间戳线上编码不同，不兼容。

| 端 | 程序 |
|---|---|
| 板端（ARM，TCP 客户端） | `bin/arm/imx6ull_sender` |
| PC 端（gcc，TCP 服务端） | `bin/pc/pc_receiver` |
| 板端确认 | 由 `imx6ull_sender` 的**启动日志**给出（`S_FMT`/`REQBUFS`/`G_PARM` 实际返回值） |

---

## 1. 核心结论一页纸

### 1.1 已有证据、可以直接讲

| 结论 | 复核位置 |
|---|---|
| 编译：ARM + PC + 诊断工具在 `-Wall -Wextra` 下 **0 warning** | `logs/build_after.log`；`make allboth` 可随时复跑 |
| 协议字节序缺陷（真 bug）已修：64 位时间戳改为逐字节大端 | `include/frame_proto.h:frame_header_hton/ntoh` |
| 帧头 **6 类校验**能正确分类拒绝畸形帧（含 2GB size 不撑爆内存） | `src/pc_receiver.c:rx_validate_header()` + `RX_MAX_FRAME_BYTES` |
| 接收端跨 `recv()` 重组可靠（137B / 8KB / **1B** 分块逐字节完整重组） | `src/tcp_socket.c:recv_all_stat` |
| 三层超时护栏消除无限阻塞（`accept` / `send` / `recv`） | `src/pc_receiver.c`（accept 前 `poll`）、`--io-timeout` |
| 短运行也能产出 CPU/RSS（运行首尾各补采一次） | `bench_finish` / `rx_bench_finish` |
| 显示支持 **16/32bpp 双位深**，按 `var` 的 `offset+length` 通用打包 | `src/fb_display.c:fb_pack_rgb` |
| 双指手势 → 1.0×–2.0× 数字缩放，**不改采集分辨率、不写采集缓冲区** | `src/gesture_zoom.c`、`src/touch_input.c` |
| 缩放渲染连续 2200 帧扫描 + guard/行尾填充双重检查 **0 越界** | `src/fb_display.c:fb_display_show_frame_zoomed` |

### 1.2 必须上板实测（**当前一律为"待实测"**）

采集 FPS、显示 FPS、有效吞吐、帧间隔 P95/P99、进程 CPU/RSS、
1.0/1.5/2.0× 缩放渲染耗时、手势处理耗时、长时稳定性、真实手势跟手性（目视）。

采集命令见 §4.3，一键脚本 `scripts/board_benchmark.sh` 与 `scripts/board_zoom_bench.sh`。

### 1.3 红线（写报告 / 简历 / 面试时不许说）

| ❌ | 原因 |
|---|---|
| "优化后提升 xx%" | 本项目**没做过性能优化**，不存在前后对比 |
| "零丢帧" / "内容 100% 正确" | 协议**无 CRC** |
| "端到端延迟 X ms" | 跨主机单调钟不可相减 |
| "整机 CPU 占用" | 采到的是**进程** CPU 时间比例 |
| "手势识别率 100%" | 离线用的是**合成事件**，不代表真实触摸 |
| "缩放渲染耗时 X µs"（宿主机数字） | 宿主机 ≠ i.MX6ULL |
| "支持双指旋转 / 惯性滑动" | **未实现** |

---

## 2. 文档导航

| 文档 | 回答什么问题 | 什么时候看 |
|---|---|---|
| **`../QUICKSTART.md`** | **纯命令清单**（可直接复制粘贴，PC IP 已写死） | **只想赶快跑起来** |
| **`USAGE.md`** | **按顺序照做就能跑通**：环境→编译→部署→上板确认→跑通→性能测试→排错；§12 是双指缩放 | **第一次上手先看这个**（带解释版） |
| `AUDIT_AND_CHANGES.md` | **§A** 源码结构与"问题在哪"（文件↔行号映射 + 问题清单 + GT911 集成审计）；**§B** 改了什么、为什么这么改、怎么验证 | 想知道改动的依据、或准备被追问细节 |
| `PERFORMANCE.md` | **§A–§E** 测什么、怎么测、验收标准、4 组对照实验、指标口径；**§P** 真实执行过的数据 + 待实测项；**§Z** 双指手势缩放的实施与验证 | 要复现数据、设计实验、或引用数字 |
| `RESUME.md` | 简历/面试口径：能写什么、**不能写什么** | 写简历/准备面试 |
| `../README.md` | 原理、协议细节、格式协商、历史排查记录 | 需要底层细节 |
| `../docs/项目源码面试审计报告.md`<br>`../docs/设备树修改面试审计报告.md` | 源码侧与设备树侧的面试审计 | 面试准备 |

> **合并历史（2026-10-13）**：原 7 份文档已深度合并为 3 份 ——
> `source_audit.md` + `modification_report.md` → `AUDIT_AND_CHANGES.md`；
> `test_plan.md` + `performance_report.md` + `zoom_multitouch_report.md` → `PERFORMANCE.md`；
> `resume_improvements.md` → `RESUME.md`。
> 章节号加了部件前缀（`§A6.1` / `§B2.1` / `§P1.3` / `§Z3.3`），全文编号唯一，可直接互引。

---

## 3. 按目的选择阅读顺序

| 你的目的 | 读这几份，按这个顺序 |
|---|---|
| **只想赶快跑（要现成命令）** | `../QUICKSTART.md`（纯命令，可直接粘贴） |
| **只想跑起来（带解释）** | `USAGE.md` §1–§5 |
| **要复现性能数据** | `USAGE.md` §7 → `PERFORMANCE.md` §B → `scripts/board_benchmark.sh` |
| **要做双指缩放实验** | `USAGE.md` §12 → `PERFORMANCE.md` §Z4 |
| **准备面试** | `RESUME.md` → `AUDIT_AND_CHANGES.md` §A6 → `../docs/*.md` |
| **想审代码改动是否合理** | `AUDIT_AND_CHANGES.md` → `AUDIT_AND_CHANGES.md` |

---

## 4. 快速命令

### 4.1 编译

```bash
cd /home/why/zdyz/linux/IMX6ULL/Projects/imx6ull_ov5640_v4l2_tcp

make arm          # 板端 imx6ull_sender
make pc           # PC 端 pc_receiver
make allboth      # 全部
make install      # 安装板端程序到 NFS
```
验收：exit 0 且 **0 warning**。

### 4.2 最小跑通

```bash
# PC 端先起（它是服务端！）
./bin/pc/pc_receiver -p 5000 -o ./frames -d --accept-timeout 120

# 板端后起（客户端）
./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 -f YUYV -n 100
```

### 4.3 性能测试（板端）

```bash
# 终端A (PC)
./bin/pc/pc_receiver -p 5000 --no-save --quiet \
    --accept-timeout 900 --io-timeout 60 \
    --stats-dir performance_evaluation/results/pc_capture_tcp

# 终端B (板端)
sh board_benchmark.sh /dev/video1 <PC_IP> 60 ./bench          # 基础矩阵
sh board_zoom_bench.sh /dev/video1 <PC_IP> 60 ./bench_zoom    # 缩放 1.0/1.5/2.0×

# 分析
python3 performance_evaluation/scripts/analyze_stats.py --compare <board_dir> <pc_dir>
```

### 4.4 双指缩放（板端）

```bash
# 先只本地验证（不发网络）
./imx6ull_sender -d /dev/video1 --capture-only -l --touch --quiet
# 目视：双指张开=放大、合拢=缩小、双指平移=拖动、三指=复位
# 方向不对：--touch-swap-xy / --touch-invert-x / --touch-invert-y
```

---

## 5. 关键文件 ↔ 功能对照

想改某个功能，直接看这几个文件：

| 功能 | 文件 | 关键函数 |
|---|---|---|
| V4L2 采集（MMAP / QBUF-DQBUF 轮转 / 帧率协商） | `src/v4l2_capture.c` | `v4l2_init_ex`、`v4l2_get_frame_info`、`v4l2_put_frame` |
| 帧协议（40B 帧头 / 网络字节序） | `include/frame_proto.h` | `frame_header_hton` / `_ntoh` |
| TCP 收发与短读重组 | `src/tcp_socket.c` | `send_all_stat`、`recv_all_stat`、`tcp_set_timeouts` |
| 板端主流程（采集→手势→显示→发送） | `src/imx6ull_sender.c` | `main` 主循环、`bench_frame_row`、`bench_finish` |
| 帧缓冲显示（16/32bpp） | `src/fb_display.c` | `fb_display_init`、`fb_display_show_frame` |
| **数字缩放渲染** | `src/fb_display.c` | `fb_display_show_frame_zoomed`、`fb_view_reset/clamp/is_fit` |
| **触摸读取（MT Protocol B）** | `src/touch_input.c` | `touch_open`、`touch_read_frame` |
| **双指手势判定** | `src/gesture_zoom.c` | `zoom_state_update`、`zoom_state_set_zoom` |
| 统计（分位数/CPU/RSS/CSV） | `src/metrics.c` | `metrics_hist_add`、`metrics_summary`、`metrics_csv_open` |
| PC 端接收与校验 | `src/pc_receiver.c` | `rx_validate_header`、`rx_bench_finish` |

---

## 6. 材料清单与状态

| 目录 / 文件 | 内容 |
|---|---|
| `README.md` | 本文件（总索引） |
| `USAGE.md` | 使用流程（含 §12 双指缩放） |
| `AUDIT_AND_CHANGES.md` / `AUDIT_AND_CHANGES.md` | 审计 / 改动说明 |
| `PERFORMANCE.md` / `PERFORMANCE.md` | 测试计划 / 数据报告 |
| `PERFORMANCE.md` | 缩放功能实施与验证 |
| `RESUME.md` | 简历口径与红线 |
| `scripts/board_benchmark.sh` | 板端基础性能矩阵 |
| `scripts/board_zoom_bench.sh` | 板端 1.0/1.5/2.0× 缩放矩阵 |
| `scripts/analyze_stats.py` | CSV → 指标（支持 `--compare` 两侧对照） |
| `logs/` | 编译日志（板端实测日志跑完会落在这里） |
| `results/` | 板端/PC 采集到的 CSV（当前为空，待实测填充） |

**已移出项目的离线测试工具**（备份于 `~/backup/`，结论保留在文档中，不再有可运行工具与原始日志）：

| 曾用工具 | 移出时间 | 备份位置 |
|---|---|---|
| `frame_proto_test`、`chunked_frame_sender`、`run_pc_offline_tests.sh` | 2026-10-13 | `~/backup/imx6ull_offline_tests_20261013/` |
| `zoom_gesture_test`、`zoom_bench.sh` | 2026-10-13 | `~/backup/imx6ull_zoom_offline_tests_20261013/` |
| `src/v4l2_info.c`（死文件）、失效日志 | 2026-10-13 | `~/backup/imx6ull_cleanup_20261013/` |
| `tools/v4l2_info.c`、`tools/v4l2_format_test.c`（板端诊断工具） | 2026-10-13 | `~/backup/imx6ull_cleanup_20261013/tools/` |
