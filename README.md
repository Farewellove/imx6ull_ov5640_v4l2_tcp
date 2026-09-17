# i.MX6ULL OV5640 V4L2 TCP Image Transmission System

基于 i.MX6ULL 的 V4L2 摄像头采集、本地 LCD 显示与网络图传系统。

## 项目结构

```
imx6ull_ov5640_v4l2_tcp/
├── include/
│   ├── frame_proto.h      # TCP 帧协议定义
│   ├── v4l2_capture.h     # V4L2 MMAP 采集 API
│   ├── tcp_socket.h       # TCP Socket 封装 API
│   └── fb_display.h       # Framebuffer LCD 显示 API
├── src/
│   ├── v4l2_info.c        # 摄像头能力查询工具 (⚠ 未被 Makefile 编译, 见下)
│   ├── v4l2_capture.c     # V4L2 MMAP 采集实现
│   ├── tcp_socket.c       # TCP Socket 实现 (send_all/recv_all)
│   ├── fb_display.c       # Framebuffer LCD 显示实现
│   ├── imx6ull_sender.c   # 板端采集 + LCD显示 + TCP 发送
│   └── pc_receiver.c      # PC 端接收 + 保存
├── tools/
│   ├── v4l2_info.c        # 摄像头能力查询工具 (实际编译的是这一份)
│   └── v4l2_format_test.c # 格式协商测试 (TRY_FMT/S_FMT/G_FMT)
├── Makefile
└── README.md
```

> **关于 `v4l2_info` 的两份源码**: `src/v4l2_info.c` 与 `tools/v4l2_info.c`
> 内容相近，但 **Makefile 只编译 `tools/` 下那一份**
> （规则 `$(ARM_DIR)/v4l2_info_tool.o: $(TOOLSDIR)/v4l2_info.c`），
> 产出 `bin/arm/v4l2_info`。`src/v4l2_info.c` 目前是历史遗留、不参与构建，
> 修改它不会有任何效果。实际使用的能力查询工具输出以 `tools/` 版本为准
> （额外解析 colorspace/field，信息更全）。

## 1. 项目功能

- **v4l2_info**: 查询摄像头完整能力信息（QUERYCAP, G_FMT, G_PARM, ENUM_FMT, ENUM_FRAMESIZES, ENUM_FRAMEINTERVALS）
- **imx6ull_sender**: 在 i.MX6ULL 板端通过 V4L2 MMAP 采集 OV5640 图像，可选同步显示到开发板 LCD，并以自定义帧协议通过 TCP 发送到 PC
- **pc_receiver**: PC 端作为 TCP Server 接收图像帧，保存为 raw 文件；可选 SDL2 实时显示窗口 (`-d`)，支持多格式自动转换、`--force-*` 手动覆盖和按键切换调试

### 支持的像素格式

| 格式     | V4L2 常量             | 说明                              |
|----------|-----------------------|-----------------------------------|
| RGB565   | V4L2_PIX_FMT_RGB565   | 16-bit 彩色, little-endian       |
| RGB565X  | V4L2_PIX_FMT_RGB565X  | 16-bit 彩色, big-endian          |
| BGR565   | (项目内部)             | 16-bit 彩色, R/B 交换, LE        |
| BGR565X  | (项目内部)             | 16-bit 彩色, R/B 交换, BE        |
| YUYV     | V4L2_PIX_FMT_YUYV     | YUV 4:2:2, YUYV 顺序             |
| UYVY     | V4L2_PIX_FMT_UYVY     | YUV 4:2:2, UYVY 顺序             |
| MJPEG    | V4L2_PIX_FMT_MJPEG    | Motion JPEG 压缩                 |

> **重要**: S_FMT 之后使用驱动**实际返回**的 pixfmt，即使与用户请求不同。发送端和接收端均以实际格式为准。

## 2. 编译方法

### 2.1 编译板端程序 (ARM)

```bash
# 默认编译板端程序 (使用内置 arm-linux-gnueabihf- 工具链)
make

# 等价于
make arm
```

输出目录 `bin/arm/`：
- `bin/arm/v4l2_info`
- `bin/arm/imx6ull_sender`

### 2.2 编译 PC 端程序

```bash
# 使用本机 gcc 编译
make pc
```

输出目录 `bin/pc/`：
- `bin/pc/pc_receiver`

### 2.3 同时编译两端

```bash
make allboth
```

### 2.4 更换交叉编译工具链

```bash
# 覆盖默认工具链
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-

# 或修改 Makefile 中的 TOOLCHAIN_DIR 和 CROSS_COMPILE 变量
```

### 2.5 安装到 NFS

```bash
# 将板端程序安装到 NFS rootfs (用于开发板 NFS 启动)
make install
```

默认安装路径: `/home/why/zdyz/nfs/rootfs/lib/modules/4.1.15`

### 2.6 清理

```bash
make clean
```

## 3. i.MX6ULL 板端运行方法

### 3.1 确认摄像头设备

```bash
# 查看 V4L2 设备
ls -l /dev/video*

# 查询摄像头能力
./v4l2_info /dev/video1
```

### 3.2 启动发送程序

```bash
# 基本用法: 采集 + TCP 发送
./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 -w 640 -h 480 -f RGB565

# 同时显示到开发板 LCD (需连接 4.3"/7" RGB LCD)
./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 -f RGB565 -l

# MJPEG 格式 (带宽低，适合无线)
./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 -f MJPEG

# 低分辨率 YUYV
./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -w 320 -h 240 -f YUYV

# 发送 100 帧后自动停止
./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -n 100
```

### 3.3 参数说明

| 参数 | 说明                           | 默认值          |
|------|-------------------------------|-----------------|
| -d   | V4L2 设备节点                  | /dev/video1     |
| -s   | PC 服务器 IP (必填)            | -               |
| -p   | TCP 端口                       | 5000            |
| -w   | 图像宽度                       | 640             |
| -h   | 图像高度                       | 480             |
| -f   | 像素格式 RGB565/YUYV/MJPEG    | RGB565          |
| -n   | 发送帧数，0=一直发             | 0               |
| -l   | 启用本地 LCD 显示 (/dev/fb0)   | 关闭            |

### 3.4 LCD 显示说明

使用 `-l` 参数时，程序自动将采集画面同步显示到开发板 LCD：

- 通过 `/dev/fb0` Linux framebuffer 接口，无需 Qt/OpenCV/SDL
- 支持 YUYV→XRGB8888 (BT.601 定点转换) 和 RGB565→XRGB8888 (解包+位扩展) 写入
- 仅支持 32bpp framebuffer (XRGB8888), 其他位深初始化失败
- 目标行步长使用 fix.line_length, 源行步长使用 V4L2 bytesperline (避免行尾 padding 错行)
- 图像小于 LCD → 居中显示（四周留黑）
- 图像大于 LCD → 居中裁剪（截取中间区域）
- 不影响 TCP 发送，显示和网络传输并行进行

## 4. PC 端运行方法

### 4.1 启动接收程序

```bash
# 保存帧到磁盘 (默认)
./pc_receiver -p 5000 -o ./frames

# 保存帧 + SDL2 实时显示窗口 (推荐)
./pc_receiver -p 5000 -o ./frames -d

# 只显示不保存
./pc_receiver -p 5000 -d
```

### 4.2 SDL2 实时显示 (-d)

使用 `-d` 参数会弹出一个 SDL2 窗口，实时显示摄像头画面：

- **默认解码**: BGR565 LE（i.MX6ULL CSI R/B 通道交换）
- **多格式支持**: RGB565/RGB565X/BGR565/BGR565X/YUYV/UYVY 自动转换为 RGB888
- **窗口标题实时显示**: `hdr=RGB565_LE | disp=BGR565_LE`
- **按键切换调试**:
  - `1` = RGB565_LE, `2` = RGB565X_BE, `3` = BGR565_LE(DEFAULT), `4` = BGR565X_BE
  - `5` = YUYV, `6` = UYVY, `0` = 使用帧头格式
  - `ESC` / `Q` = 关闭窗口退出
- **窗口缩放**: 支持拖拽缩放，画面自适应
- **依赖**: 需要安装 `libsdl2-dev`

```bash
# 安装 SDL2 (一次性)
sudo apt install libsdl2-dev
```

### 4.3 指定解码格式 (--display-format / --swap-rb)

当颜色异常时，使用以下参数手动指定解码格式：

```bash
# 默认 BGR565 (i.MX6ULL CSI R/B 已交换的推荐模式)
./pc_receiver -p 5000 -d

# 显式指定 BGR565
./pc_receiver -p 5000 -d --display-format bgr565
./pc_receiver -p 5000 -d --swap-rb

# 切换回 RGB565 (标准顺序)
./pc_receiver -p 5000 -d --display-format rgb565

# 尝试其他格式
./pc_receiver -p 5000 -d --display-format rgb565x
./pc_receiver -p 5000 -d --display-format yuyv
```

> PPM 保存使用与 SDL 显示**相同**的解码器，保证窗口和文件颜色一致。

### 4.4 接收结果

收到的文件保存在 `./frames/` 目录下：

```
frames/
├── frame_000000.rgb565    # RGB565 raw 数据
├── frame_000000.ppm       # PPM 图片 (可直接用图片查看器打开)
├── frame_000001.rgb565
├── frame_000001.ppm
├── ...
```

如果发送的是 MJPEG:
```
frames/
├── frame_000000.mjpg      # 可直接用图片查看器打开
├── frame_000001.mjpg
├── ...
```

如果发送的是 YUYV:
```
frames/
├── frame_000000.yuyv      # YUYV raw 数据
├── frame_000001.yuyv
├── ...
```

### 4.5 参数说明

| 参数              | 说明                      | 默认值    |
|-------------------|--------------------------|-----------|
| -p                | 监听端口                   | 5000      |
| -o                | 保存目录                   | ./frames  |
| -d                | 启用 SDL2 实时显示          | 关闭      |
| --force-rgb565    | 强制按 RGB565 LE 解析      | -         |
| --force-rgb565x   | 强制按 RGB565X BE 解析     | -         |
| --force-bgr565    | 强制按 BGR565 LE 解析      | -         |
| --force-bgr565x   | 强制按 BGR565X BE 解析     | -         |
| --force-yuyv      | 强制按 YUYV 解析           | -         |
| --force-uyvy      | 强制按 UYVY 解析           | -         |

> 这些 --force-* 参数也可通过 SDL 窗口按键 `1`-`6` 实时切换。

## 5. 推荐测试流程

### 5.1 快速开始 (默认 YUYV)

```bash
# 1. PC 端: 启动接收 (自动检测帧头格式)
./pc_receiver -p 5000 -o ./test_frames -d

# 2. 板端: 启动发送 (默认 YUYV)
./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000
```

### 5.2 YUV 字节顺序测试

当使用 YUYV 格式时，如果 PC 端颜色异常 (偏紫/偏绿)，可能是 YUV 字节顺序不匹配:

```bash
# 板端指定 YUV 字节顺序 (默认 yuyv)
./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 --yuv-order uyvy
./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 --yuv-order yvyu

# PC 端也可指定 YUV 顺序覆盖
./pc_receiver -p 5000 -d --yuv-order uyvy
./pc_receiver -p 5000 -d --yuv-order yvyu
```

> 当前驱动准备切回 YUYV 全链路，若 S_FMT 返回 YUYV 则直接使用。

### 5.3 OV5640 颜色异常排查 (板端诊断)

#### Step 1: 查询摄像头能力

```bash
./v4l2_info /dev/video1
```

观察输出中的 `ENUM_FMT` 部分，确认驱动支持哪些格式。

#### Step 2: 测试格式协商

```bash
./v4l2_format_test /dev/video1 640 480 rgb565
./v4l2_format_test /dev/video1 640 480 yuyv
./v4l2_format_test /dev/video1 640 480 uyvy
```

观察 `S_FMT` 和 `G_FMT` 返回的格式：
- 如果请求 `rgb565` 返回 `RGBP` → RGB565 可用
- 如果请求 `yuyv` 返回 `RGBP` → YUYV 不支持，驱动回退到 RGB565
- 如果请求 `yuyv` 返回 `YUYV` → YUYV 可用

#### Step 3: 板端启动 sender 观察日志

```bash
./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 --pixfmt rgb565
```

关键日志行：
```
[INFO] Capture using driver actual format:
[INFO]   Format       : RGB565 (LE) (0x50424752)
[INFO]   Resolution   : 640x480
[INFO]   BytesPerLine : 1280
[INFO]   SizeImage    : 614400
[INFO]   NOTE: i.MX6ULL CSI may swap R/B. PC use: pc_receiver -d (default BGR565)
```

#### Step 4: PC 端测试不同解码方式

```bash
# 默认 BGR565 (推荐)
./pc_receiver -p 5000 -o ./frames -d --display-format bgr565

# 标准 RGB565
./pc_receiver -p 5000 -o ./frames -d --display-format rgb565

# 或在 SDL 窗口中按键 1-6 实时切换
```

#### Step 5: 已确认的颜色现象

| 解码方式      | 画面表现                   | 结论                  |
|--------------|---------------------------|----------------------|
| RGB565_LE    | 结构正确但整体偏蓝绿         | R/B 通道交换           |
| **BGR565_LE** | **结构正确且颜色最接近正常** ✅ | **当前推荐**           |
| RGB565X_BE   | 颜色严重异常                | 不是大端字节序          |
| BGR565X_BE   | 颜色严重异常                | 不是大端字节序          |
| YUYV         | 严重紫绿噪点                | 数据不是 YUV 4:2:2     |
| UYVY         | 严重紫绿噪点                | 数据不是 YUV 4:2:2     |

**结论**: 驱动报告 RGB565，但 i.MX6ULL CSI 硬件交换了 R/B 通道。
PC 端默认使用 BGR565_LE 解码即可获得正常颜色。

### 5.3 当前驱动已知限制

经过完整测试确认，当前 mx6s-csi + OV5640 驱动存在以下行为：

| 现象 | 说明 |
|------|------|
| VIDIOC_G_FMT 返回无效值 | pixelformat=0, bytesperline=0。**不可靠，不能作为格式依据。** |
| VIDIOC_S_FMT 校验不严格 | 接受未在 ENUM_FMT 中列出的格式 (如 UYVY)，不能仅凭 S_FMT 判断 sensor 实际输出。 |
| ENUM_FMT 列出 RGBP/JPEG/YUYV | 未列出 UYVY，说明 UYVY 请求即使 S_FMT 接受也可能是假成功。 |
| RGB565_LE 显示偏蓝绿 | i.MX6ULL CSI R/B 通道交换，需 BGR565_LE 解码。 |

**当前推荐**:
- 板端 sender 使用默认 YUYV: `./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000`
- PC 端自动检测帧头格式: `./pc_receiver -p 5000 -o ./frames -d`
- 若 YUV 字节顺序不匹配: `--yuv-order uyvy` / `--yuv-order yvyu`
- 驱动层 YUYV 修复后将不再需要 BGR565 workaround

### 5.3 验证收到的帧

```bash
ls -lh test_frames/
eog test_frames/frame_000000.ppm   # GNOME
display test_frames/frame_000000.ppm  # ImageMagick
```

### 5.2 带宽参考

| 格式   | 分辨率   | 每帧大小    | 15fps 带宽 | 30fps 带宽 |
|--------|---------|-------------|-----------|-----------|
| RGB565 | 640x480 | 614,400 B   | ~9.2 MB/s | ~18.4 MB/s|
| YUYV   | 640x480 | 614,400 B   | ~9.2 MB/s | ~18.4 MB/s|
| MJPEG  | 640x480 | ~30-150 KB  | ~0.5-2 MB/s| ~1-4 MB/s|

> 带宽不够时优先使用 MJPEG 或降低分辨率。

## 6. V4L2 采集流程说明

本项目的 V4L2 采集严格遵循标准的 MMAP 流式 I/O 流程：

```
1. open("/dev/video1")              # 打开设备
2. VIDIOC_QUERYCAP                  # 查询设备能力
3. VIDIOC_S_FMT                     # 设置图像格式 (宽/高/像素格式)
4. VIDIOC_REQBUFS (count=3)         # 申请 MMAP 缓冲区
5. VIDIOC_QUERYBUF (x3)             # 查询每个 buffer 的 offset 和 length
6. mmap (x3)                        # 将每个 buffer 映射到用户空间
7. VIDIOC_QBUF (x3)                 # 将 buffer 全部入队
8. VIDIOC_STREAMON                  # 开启视频流
9. [循环采集]
   a. poll(fd, POLLIN)              # 等待帧就绪
   b. VIDIOC_DQBUF                  # 取出就绪的 buffer
   c. 读取数据 (直接通过 mmap 指针)
   d. [可选] fb_display_show_frame  # 显示到 LCD
   e. TCP send                      # 发送到 PC
   f. VIDIOC_QBUF                   # 将 buffer 重新入队
10. VIDIOC_STREAMOFF                # 关闭视频流
11. [可选] fb_display_deinit        # 关闭 LCD
12. munmap (x3)                     # 解除内存映射
13. close(fd)                       # 关闭设备
```

## 7. TCP 帧协议说明

### 7.1 数据格式

```
+-----------------+------------------+
|  frame_header   |  raw image data  |
|  (40 bytes)     |  (size bytes)    |
+-----------------+------------------+
```

### 7.2 帧头定义

```c
struct frame_header {
    uint32_t magic;         // 魔数: 0x12345678 (网络字节序)
    uint32_t version;       // 协议版本: 1
    uint32_t header_size;   // 帧头大小: sizeof(struct frame_header) = 40
    uint32_t width;         // 图像宽度 (网络字节序)
    uint32_t height;        // 图像高度 (网络字节序)
    uint32_t pixfmt;        // V4L2 像素格式常量 (网络字节序)
    uint32_t size;          // 图像数据长度 (网络字节序)
    uint32_t frame_id;      // 帧序号，从 0 递增 (网络字节序)
    uint64_t timestamp_us;  // 采集时间戳，微秒 (网络字节序)
};
```

### 7.3 字节序说明

所有多字节字段（magic, version, header_size, width, height, pixfmt, size, frame_id, timestamp_us）均使用**网络字节序 (Big-Endian)** 传输。

- 发送端: 调用 `htonl()` / `htons()` 转换后再发送
- 接收端: 接收后调用 `ntohl()` / `ntohs()` 转换回主机字节序

这样确保 i.MX6ULL (ARM, little-endian) 和 PC (x86_64, little-endian) 以及任何大端平台之间都能正常通信。

### 7.4 像素格式值

| 格式   | 十六进制值     | 说明      |
|--------|--------------|-----------|
| RGB565 | 0x50424752   | 每像素 2 字节 |
| YUYV   | 0x56595559   | 每像素 2 字节 |
| MJPEG  | 0x47504A4D   | JPEG 压缩   |

## 8. Framebuffer LCD 显示说明

### 8.1 实现原理

通过 Linux framebuffer 接口 `/dev/fb0` 直接操作显存，不做任何额外依赖：

1. `open("/dev/fb0")` → 打开 framebuffer 设备
2. `FBIOGET_VSCREENINFO` → 获取屏幕分辨率、bpp
3. `FBIOGET_FSCREENINFO` → 获取行偏移、显存大小
4. `mmap` → 映射显存到用户空间
5. 每帧写入 → 直接拷贝/转换到 mmap 地址

### 8.2 支持的图像格式

| 输入格式 | 处理方式                     |
|---------|----------------------------|
| RGB565  | 直接 memcpy 到 framebuffer  |
| YUYV    | YUYV→RGB565 软转换后写入     |
| MJPEG   | 不支持，跳过显示             |

### 8.3 YUYV → RGB565 转换

使用 ITU-R BT.601 定点公式：

```
R = (298*(Y-16) + 409*(V-128) + 128) >> 8
G = (298*(Y-16) - 100*(U-128) - 208*(V-128) + 128) >> 8
B = (298*(Y-16) + 516*(U-128) + 128) >> 8
```

### 8.4 显示策略

| 条件           | 策略               |
|---------------|-------------------|
| 图像 ≤ LCD    | 居中显示，四周留黑  |
| 图像 > LCD    | 居中裁剪中间区域    |

> i.MX6ULL 开发板典型 LCD: 480x272 (4.3") 或 800x480 (7")，均为 RGB565 格式。

## 9. 常见问题

### 9.1 open /dev/video1 failed

**原因**: 设备节点不存在或权限不足。

**排查**:
```bash
# 确认设备节点
ls -l /dev/video*

# 检查内核日志
dmesg | grep -i "video\|ov5640\|csi"

# 确认驱动已加载
lsmod | grep ov5640

# 临时授权
chmod 666 /dev/video1
```

### 9.2 VIDIOC_S_FMT failed

**原因**: 驱动不支持请求的像素格式或分辨率。

**排查**:
```bash
# 先用 v4l2_info 查询设备支持哪些格式
./v4l2_info /dev/video1
```

然后选择一个驱动支持的格式和分辨率。OV5640 通常支持:
- 640x480 RGB565/YUYV
- 320x240 RGB565/YUYV
- 1280x720 YUYV
- 2592x1944 MJPEG (需要驱动暴露该 mode)

如果 S_FMT 返回了与你请求不同的格式，程序会打印错误并退出，说明驱动不支持请求的格式。

### 9.3 VIDIOC_STREAMON failed

**原因**: 通常是传感器没有正确上电或 CSI 时钟 (MCLK) 没有配置好。注意本项目 MCLK 引脚与闪光灯 LED 共用, 引脚被切到 GPIO 时摄像头无时钟。

**排查**:
```bash
dmesg | grep -i "ov5640\|mipi\|csi\|stream"

# 常见原因:
# 1. 摄像头排线没接好
# 2. XCLK 没有输出 (检查 CCM 时钟配置)
# 3. I2C 通信失败 (传感器未检测到)
# 4. 设备树中 pinctrl 配置错误
```

### 9.4 poll timeout

**原因**: 摄像头没有输出帧数据。

**排查**:
- 程序打印 `[WARN] poll timeout (2000 ms)` 是正常的警告，不会崩溃
- 可能是 STREAMON 后传感器需要几帧预热
- 连续超时说明传感器没有输出，检查:
  - `dmesg` 看是否有 CSI 中断错误
  - 确认设备树中 `csi_id` 和 `mux_id` 配置正确
  - 确认 `mx6s_csi` 捕获端口没有搞反 (CSI0 vs CSI1)

### 9.5 DQBUF 阻塞

**原因**: DQBUF 在 O_NONBLOCK 模式下返回 EAGAIN，程序会打印 `[WARN] DQBUF EAGAIN, will retry` 并继续 poll。

**排查**:
- 确认 `open()` 时使用了 `O_NONBLOCK` 标志
- 确认 poll 返回 POLLIN 后才调用 DQBUF
- 如果是阻塞在 DQBUF (没有 O_NONBLOCK 的旧情况)，给 open 加上 `O_NONBLOCK`

### 9.6 图像颜色异常排查

这是最常见的显示问题。按以下步骤逐步排查：

#### Step 1: 确认板端驱动实际输出的格式

板端 `imx6ull_sender` 启动时查看日志输出：

```
[INFO] Capture using driver actual format:
[INFO]   Format       : YUYV (4:2:2) (0x56595559)
```

如果显示 `YUYV` 或 `UYVY`，说明驱动实际返回的不是 RGB565。

#### Step 2: 确认 PC 端收到的格式

PC 端 `pc_receiver` 每帧会打印 `hdr=XXXX`：

```
[FRAME 0] ... hdr=YUYV (4:2:2), ts=...
```

#### Step 3: 根据症状选择排查方案

| 症状                           | 可能原因              | 解决方法                                    |
|-------------------------------|----------------------|---------------------------------------------|
| 画面轮廓正确但整体蓝绿/偏绿      | 实际为 YUYV/UYVY     | `--force-yuyv` 或 `--force-uyvy`            |
| 画面红蓝反了 (天空偏红)         | BGR565 字节序        | `--force-bgr565` 或 SDL 窗口按 `3`           |
| 颜色严重异常但轮廓正确           | RGB565X BE 字节序    | `--force-rgb565x` 或 SDL 窗口按 `2`          |
| 画面全是灰/紫色噪点             | 字节序完全反了         | 依次尝试 `4`, `5`, `6` 按键                  |
| 画面颜色偏暗/偏亮               | RGB565→RGB888 范围   | 检查 BT.601 转换公式 (通常正确)              |

#### Step 4: 使用 SDL 按键实时切换

启动 `pc_receiver -d` 后，在画面窗口中按键快速切换：

```bash
# 按 1 → RGB565 (默认)
# 按 5 → YUYV (最常见的实际格式)
# 按 6 → UYVY
# 按 2 → RGB565X (big-endian)
# 按 3 → BGR565 (红蓝交换)
# 按 4 → BGR565X
# 按 0 → 恢复帧头中的格式
```

哪个按键画面颜色正常，就说明原始数据是哪种格式。找到正确格式后可：
- 使用 `--force-*` 参数指定 (每次启动生效)
- 或修改板端 `-f` 参数匹配实际格式 (根本解决)

#### Step 5: 命令行快速试错

```bash
# 默认就是 BGR565（i.MX6ULL CSI R/B 交换），通常不需要额外参数
./pc_receiver -p 5000 -o ./frames -d

# 如果颜色仍然不对，依次尝试:
./pc_receiver -p 5000 -o ./frames -d --display-format rgb565   # 标准 RGB565
./pc_receiver -p 5000 -o ./frames -d --display-format rgb565x  # BE 字节序
./pc_receiver -p 5000 -o ./frames -d --display-format yuyv     # YUYV
./pc_receiver -p 5000 -o ./frames -d --display-format uyvy     # UYVY
```

#### Step 6: 已确认的 i.MX6ULL CSI 颜色问题

经过对 6 种解码模式的完整测试，结论如下：

| 模式 | 结果 | 说明 |
|------|------|------|
| RGB565 LE  | 画面正常，但蓝绿色偏 | R/B 通道交换 |
| RGB565X BE | 颜色严重异常 | 不是大端 |
| **BGR565 LE** | **颜色最接近正常** ✅ | **默认解码模式** |
| BGR565X BE | 颜色严重异常 | 不是大端 |
| YUYV       | 紫色噪点 | 不是 YUV 数据 |
| UYVY       | 紫色噪点 | 不是 YUV 数据 |

**结论**: 数据本质是 16-bit 小端 RGB565，但 i.MX6ULL CSI 接口交换了 R/B 通道。
PC 端默认使用 **BGR565 LE** 解码即可获得正常颜色。

#### Step 7: 推荐运行命令

```bash
# PC 端 (默认 BGR565，直接使用)
./pc_receiver -p 5000 -o ./frames -d

# 或显式指定
./pc_receiver -p 5000 -o ./frames -d --display-format bgr565
./pc_receiver -p 5000 -o ./frames -d --swap-rb
```

> **关键原则**: 
> 1. 不要假设驱动返回了你请求的格式。板端日志打印驱动**实际** pixfmt。
> 2. i.MX6ULL CSI 输出 RGB565 时 R/B 通道交换，PC 端用 BGR565 解码。

### 9.7 网络带宽不够

**解决方法**:
1. 使用 MJPEG 格式: `-f MJPEG`
2. 降低分辨率: `-w 320 -h 240`
3. 降低采集帧率: 通过 `VIDIOC_S_PARM` 设置目标帧率（需在驱动侧支持）
4. 使用千兆以太网（i.MX6ULL 的 ENET 是 100M，需确认网络环境）
5. 如果使用 WiFi，信号质量和距离影响很大

**带宽估算**:
- RGB565 640x480 @ 15fps ≈ 9.2 MB/s ≈ 73.6 Mbps
- MJPEG 640x480 @ 15fps ≈ 0.5–2 MB/s ≈ 4–16 Mbps

### 9.8 LCD 无法打开 (/dev/fb0)

**原因**: framebuffer 设备不存在或驱动未加载。

**排查**:
```bash
# 确认 fb 设备
ls -l /dev/fb0

# 检查内核是否启用了 framebuffer
dmesg | grep -i "fb\|mxsfb"

# 确认设备树中 LCD 配置正确
```

程序在 LCD 初始化失败时会打印 `[WARN] LCD init failed, continuing without display` 并继续运行（不影响 TCP 发送）。

## 10. Makefile 目标速查

| 命令             | 说明                                            |
|-----------------|------------------------------------------------|
| `make`          | 编译板端程序 (imx6ull_sender)                   |
| `make arm`      | 只编译板端 imx6ull_sender                       |
| `make arm-tools`| 编译诊断工具 v4l2_info + v4l2_format_test       |
| `make pc`       | 只编译 PC 端 pc_receiver                        |
| `make allboth`  | 同时编译 ARM + PC + tools                       |
| `make install`  | 安装板端程序 + 工具到 NFS rootfs                 |
| `make clean`    | 清理所有编译产物                                 |
| `make help`     | 显示帮助                                        |

## 11. 平台信息

- **目标平台**: NXP i.MX6ULL (Cortex-A7)
- **摄像头**: OV5640 (8-bit 并行 DVP 接口, i.MX6ULL 无 MIPI CSI 控制器)
- **内核版本**: Linux 4.1.15
- **V4L2 驱动**: mx6s_capture (驱动名 mx6s-csi) + ov5640_camera
- **设备节点**: /dev/video0 或 /dev/video1
- **LCD 接口**: /dev/fb0 (Linux framebuffer)
- **交叉编译器**: gcc-linaro-4.9.4-2017.01 (arm-linux-gnueabihf)

## 12. 许可

本项目仅用于学习和测试目的。
