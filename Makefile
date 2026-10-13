# Makefile for imx6ull_ov5640_v4l2_tcp
#
# 用法:
#   make          - 编译板端程序 (等同于 make arm)
#   make arm      - 只编译板端 imx6ull_sender
#   make pc       - 只编译 PC 端程序
#   make allboth  - 同时编译 ARM + PC
#   make install  - 安装板端程序到 NFS
#   make clean    - 清理所有编译产物
#
# 输出目录:
#   bin/arm/imx6ull_sender     - V4L2 采集 + TCP 发送 (板端)
#   bin/pc/pc_receiver         - TCP 接收保存 + SDL2 显示 (PC 端)

# ---- 交叉编译工具链配置 ----
TOOLCHAIN_DIR := /home/why/zdyz/a/env/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf
PATH := $(TOOLCHAIN_DIR)/bin:$(PATH)

CROSS_COMPILE ?= arm-linux-gnueabihf-

# ARM 交叉编译器
ARM_CC      := $(CROSS_COMPILE)gcc
ARM_STRIP   := $(CROSS_COMPILE)strip

# PC 本机编译器
PC_CC       := gcc
PC_STRIP    := strip

RM      := rm -f
MKDIR   := mkdir -p

# ---- 目录 ----
SRCDIR   := src
INCDIR   := include
# 注: 板端诊断工具 (tools/v4l2_info.c, tools/v4l2_format_test.c) 已于
#     2026-10-13 删除 (备份 ~/backup/imx6ull_cleanup_20261013/tools/)。
#     上板确认格式改为直接看 imx6ull_sender 的启动日志, 见 USAGE.md §4。
ARM_DIR  := bin/arm
PC_DIR   := bin/pc
DESTDIR  := /home/why/zdyz/nfs/rootfs/lib/modules/4.1.15

# ---- 编译选项 ----
CFLAGS   := -Wall -Wextra -O2 -g
CFLAGS   += -I$(INCDIR)
CFLAGS   += -std=gnu99

# 板端额外选项
ARM_CFLAGS := $(CFLAGS)
# 兼容较老的 Linux 4.1.15 内核头文件
# ARM_CFLAGS += -I/path/to/linux-4.1.15/include/uapi
# ARM_CFLAGS += -I/path/to/linux-4.1.15/include

# 通用链接选项
# -lm: gesture_zoom.c 用 sqrt() 算双指距离 (无法用整数近似替代, 因为缩放
#      倍率必须与距离成比例)。glibc 的 libm 在目标 rootfs 上必然存在。
LDFLAGS := -lm

# PC 端额外选项
PC_CFLAGS := $(CFLAGS) -DENABLE_SDL_DISPLAY $(shell pkg-config --cflags sdl2 2>/dev/null)
PC_LDFLAGS := $(shell pkg-config --libs sdl2 2>/dev/null) -lm

# ---- 公共源文件 (ARM 和 PC 各自编译一份) ----
# metrics.c: 统计模块 (帧间隔分位数 / CPU / RSS / CSV), 板端与 PC 端共用
COMMON_SRCS := $(SRCDIR)/v4l2_capture.c $(SRCDIR)/tcp_socket.c $(SRCDIR)/metrics.c

# ---- 显示与触摸源文件 ----
# fb_display.c   : framebuffer 绘制 (16/32bpp) + 最近邻缩放渲染
# touch_input.c  : evdev MT Protocol B 读取 (支持离线回放)
# gesture_zoom.c : 双指手势 → 缩放视图
# 板端主程序使用; PC 侧离线缩放测试工具也复用同一份代码 (不重写一份),
# 这样离线测出来的渲染耗时与越界检查才真正代表生产路径。
DISPLAY_SRCS := $(SRCDIR)/fb_display.c $(SRCDIR)/touch_input.c $(SRCDIR)/gesture_zoom.c

# ---- ARM 目标 (主程序) ----
ARM_COMMON_OBJS  := $(patsubst $(SRCDIR)/%.c, $(ARM_DIR)/%.o, $(COMMON_SRCS))
ARM_DISPLAY_OBJS := $(patsubst $(SRCDIR)/%.c, $(ARM_DIR)/%.o, $(DISPLAY_SRCS))

ARM_SENDER_BIN := $(ARM_DIR)/imx6ull_sender
ARM_SENDER_OBJ := $(ARM_DIR)/imx6ull_sender.o

ARM_BINS := $(ARM_SENDER_BIN)

# ---- PC 目标 ----
PC_COMMON_OBJS := $(patsubst $(SRCDIR)/%.c, $(PC_DIR)/%.o, $(COMMON_SRCS))

PC_RECEIVER_BIN := $(PC_DIR)/pc_receiver
PC_RECEIVER_OBJ := $(PC_DIR)/pc_receiver.o

PC_BINS := $(PC_RECEIVER_BIN)

# ---- 顶层目标 ----
.PHONY: all arm pc install clean help

# 默认只编译板端程序
all: arm

# 同时编译板端与 PC 端 (全部用于连板真实测试)
allboth: arm pc
	@echo "================================================================"
	@echo "  All builds complete!"
	@echo "================================================================"

# ---- ARM 编译 ----
arm: $(ARM_DIR) $(ARM_BINS)
	@echo "================================================================"
	@echo "  ARM build complete!"
	@echo "================================================================"
	@ls -lh $(ARM_BINS)
	@echo "================================================================"

$(ARM_DIR):
	$(MKDIR) $(ARM_DIR)

# ARM 目标文件规则
$(ARM_DIR)/%.o: $(SRCDIR)/%.c
	@echo "  [ARM] CC    $<"
	$(ARM_CC) $(ARM_CFLAGS) -c -o $@ $<

$(ARM_SENDER_BIN): $(ARM_SENDER_OBJ) $(ARM_COMMON_OBJS) $(ARM_DISPLAY_OBJS)
	@echo "  [ARM] LD    $@"
	$(ARM_CC) $(ARM_CFLAGS) -o $@ $^ $(LDFLAGS)

# ---- PC 编译 ----
pc: $(PC_DIR) $(PC_BINS)
	@echo "================================================================"
	@echo "  PC build complete!"
	@echo "================================================================"
	@ls -lh $(PC_BINS)
	@echo "================================================================"

$(PC_DIR):
	$(MKDIR) $(PC_DIR)

# PC 目标文件规则
$(PC_DIR)/%.o: $(SRCDIR)/%.c
	@echo "  [PC]  CC    $<"
	$(PC_CC) $(PC_CFLAGS) -c -o $@ $<

$(PC_RECEIVER_BIN): $(PC_RECEIVER_OBJ) $(PC_COMMON_OBJS)
	@echo "  [PC]  LD    $@"
	$(PC_CC) $(PC_CFLAGS) -o $@ $^ $(PC_LDFLAGS)

# ---- 安装 (板端程序 + 诊断工具) ----
install: $(ARM_BINS)
	@echo "  INSTALL to $(DESTDIR)/"
	@$(MKDIR) $(DESTDIR)
	@install -m 755 $(ARM_BINS) $(DESTDIR)/
	@echo "  Install complete:"
	@ls -lh $(DESTDIR)/imx6ull_sender

# ---- 清理 ----
clean:
	$(RM) -r $(ARM_DIR)/*.o $(ARM_BINS)
	$(RM) -r $(ARM_DIR)
	$(RM) -r $(PC_DIR)/*.o $(PC_BINS)
	$(RM) -r $(PC_DIR)
	@echo "[INFO] Clean done"

# ---- 帮助 ----
help:
	@echo "Usage:"
	@echo "  make            - 编译板端程序 (ARM)"
	@echo "  make arm        - 只编译板端 imx6ull_sender"
	@echo "  make pc         - 只编译 PC 端 pc_receiver"
	@echo "  make allboth    - 同时编译 ARM + PC"
	@echo "  make install    - 安装板端程序到 NFS ($(DESTDIR))"
	@echo "  make clean      - 清理所有编译产物"
