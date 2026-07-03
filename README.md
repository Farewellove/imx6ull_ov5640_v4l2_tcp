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
│   ├── v4l2_info.c        # 摄像头能力查询工具
│   ├── v4l2_capture.c     # V4L2 MMAP 采集实现
│   ├── tcp_socket.c       # TCP Socket 实现 (send_all/recv_all)
│   ├── fb_display.c       # Framebuffer LCD 显示实现
│   ├── imx6ull_sender.c   # 板端采集 + LCD显示 + TCP 发送
│   └── pc_receiver.c      # PC 端接收 + 保存
├── Makefile
└── README.md
```

## 1. 项目功能

- **v4l2_info**: 查询摄像头完整能力信息（QUERYCAP, G_FMT, G_PARM, ENUM_FMT, ENUM_FRAMESIZES, ENUM_FRAMEINTERVALS）
- **imx6ull_sender**: 在 i.MX6ULL 板端通过 V4L2 MMAP 采集 OV5640 图像，可选同步显示到开发板 LCD，并以自定义帧协议通过 TCP 发送到 PC
- **pc_receiver**: PC 端作为 TCP Server 接收图像帧，保存为 raw 文件；对 RGB565 额外保存 PPM 格式方便直接查看

### 支持的像素格式

| 格式   | V4L2 常量           | 说明                         |
|--------|---------------------|------------------------------|
| RGB565 | V4L2_PIX_FMT_RGB565 | 16-bit 彩色，保存 PPM 可查看 |
| YUYV   | V4L2_PIX_FMT_YUYV   | YUV 4:2:2，保存 raw         |
| MJPEG  | V4L2_PIX_FMT_MJPEG  | Motion JPEG，保存 .mjpg     |

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
./imx6ull_sender -d /dev/video1 -s 192.168.1.100 -p 5000 -w 640 -h 480 -f RGB565

# 同时显示到开发板 LCD (需连接 4.3"/7" RGB LCD)
./imx6ull_sender -d /dev/video1 -s 192.168.1.100 -p 5000 -f RGB565 -l

# MJPEG 格式 (带宽低，适合无线)
./imx6ull_sender -d /dev/video1 -s 192.168.1.100 -p 5000 -f MJPEG

# 低分辨率 YUYV
./imx6ull_sender -d /dev/video1 -s 192.168.1.100 -w 320 -h 240 -f YUYV

# 发送 100 帧后自动停止
./imx6ull_sender -d /dev/video1 -s 192.168.1.100 -n 100
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
- 支持 RGB565 直接写入和 YUYV→RGB565 软转换
- 图像小于 LCD → 居中显示（四周留黑）
- 图像大于 LCD → 居中裁剪（截取中间区域）
- 不影响 TCP 发送，显示和网络传输并行进行

## 4. PC 端运行方法

### 4.1 启动接收程序

```bash
# 在 PC 端先启动接收 (默认端口 5000)
./pc_receiver -p 5000 -o ./frames
```

### 4.2 接收结果

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

### 4.3 参数说明

| 参数 | 说明         | 默认值     |
|------|-------------|-----------|
| -p   | 监听端口    | 5000      |
| -o   | 保存目录    | ./frames  |

## 5. 推荐测试流程

### 5.1 完整测试步骤

1. **PC 端**: 先启动接收程序
   ```bash
   ./pc_receiver -p 5000 -o ./test_frames
   ```

2. **板端**: 先查询摄像头能力
   ```bash
   ./v4l2_info /dev/video1
   ```

3. **板端**: 启动采集发送
   ```bash
   # 不启用 LCD
   ./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 -f RGB565

   # 启用 LCD 同步显示
   ./imx6ull_sender -d /dev/video1 -s <PC_IP> -p 5000 -f RGB565 -l
   ```

4. **验证**: 在 PC 端查看收到的帧
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

**原因**: 通常是传感器没有正确上电或 MIPI/CSI 时钟没有配置好。

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

### 9.6 图像颜色不对

**常见情况和解决方法**:

1. **YUYV 当成 RGB565 显示**: 修改 -f 参数匹配实际格式
2. **UYVY vs YUYV 字节序反了**: 确认传感器输出的 YUV 顺序，可能需要修改 / 添加 UYVY 支持
3. **RGB565 字节序 (RGB vs BGR)**: 修改 `save_rgb565_as_ppm` 中的颜色转换
4. **色彩空间不对**: 检查 `VIDIOC_S_FMT` 后驱动返回的实际格式，程序会打印

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

| 命令            | 说明                                   |
|----------------|---------------------------------------|
| `make`         | 编译板端程序 (默认)                     |
| `make arm`     | 只编译板端 v4l2_info + imx6ull_sender |
| `make pc`      | 只编译 PC 端 pc_receiver               |
| `make allboth` | 同时编译 ARM 和 PC                     |
| `make install` | 安装板端程序到 NFS rootfs              |
| `make clean`   | 清理所有编译产物                        |
| `make help`    | 显示帮助                               |

## 11. 平台信息

- **目标平台**: NXP i.MX6ULL (Cortex-A7)
- **摄像头**: OV5640 (MIPI CSI / DVP)
- **内核版本**: Linux 4.1.15
- **V4L2 驱动**: mx6s_csi + ov5640_camera
- **设备节点**: /dev/video0 或 /dev/video1
- **LCD 接口**: /dev/fb0 (Linux framebuffer)
- **交叉编译器**: gcc-linaro-4.9.4-2017.01 (arm-linux-gnueabihf)

## 12. 许可

本项目仅用于学习和测试目的。
