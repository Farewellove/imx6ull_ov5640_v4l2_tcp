# Makefile for imx6ull_ov5640_v4l2_tcp
#
# 用法:
#   make          - 编译板端程序 (等同于 make arm)
#   make arm      - 只编译板端 imx6ull_sender
#   make arm-tools- 只编译板端诊断工具
#   make pc       - 只编译 PC 端程序
#   make allboth  - 同时编译 ARM + PC + tools
#   make install  - 安装板端程序到 NFS
#   make clean    - 清理所有编译产物
#
# 输出目录:
#   bin/arm/imx6ull_sender     - V4L2 采集 + TCP 发送 (板端)
#   bin/arm/v4l2_info          - 摄像头能力查询 (诊断工具)
#   bin/arm/v4l2_format_test   - 格式协商测试 (诊断工具)
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
TOOLSDIR := tools
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
LDFLAGS :=

# PC 端额外选项
PC_CFLAGS := $(CFLAGS) -DENABLE_SDL_DISPLAY $(shell pkg-config --cflags sdl2 2>/dev/null)
PC_LDFLAGS := $(shell pkg-config --libs sdl2 2>/dev/null) -lm

# ---- 公共源文件 (ARM 和 PC 各自编译一份) ----
COMMON_SRCS := $(SRCDIR)/v4l2_capture.c $(SRCDIR)/tcp_socket.c

# ---- ARM 目标 (主程序) ----
ARM_COMMON_OBJS := $(patsubst $(SRCDIR)/%.c, $(ARM_DIR)/%.o, $(COMMON_SRCS))

ARM_SENDER_BIN := $(ARM_DIR)/imx6ull_sender
ARM_SENDER_OBJ := $(ARM_DIR)/imx6ull_sender.o

# fb_display 仅板端使用, 不加入 PC 编译
ARM_FB_DISPLAY_OBJ := $(ARM_DIR)/fb_display.o

ARM_BINS := $(ARM_SENDER_BIN)

# ---- ARM 诊断工具 (独立编译, 不依赖 v4l2_capture module) ----
ARM_V4L2_INFO_BIN   := $(ARM_DIR)/v4l2_info
ARM_V4L2_INFO_OBJ   := $(ARM_DIR)/v4l2_info_tool.o
ARM_FMT_TEST_BIN    := $(ARM_DIR)/v4l2_format_test
ARM_FMT_TEST_OBJ    := $(ARM_DIR)/v4l2_format_test.o

ARM_TOOLS := $(ARM_V4L2_INFO_BIN) $(ARM_FMT_TEST_BIN)

# ---- PC 目标 ----
PC_COMMON_OBJS := $(patsubst $(SRCDIR)/%.c, $(PC_DIR)/%.o, $(COMMON_SRCS))

PC_RECEIVER_BIN := $(PC_DIR)/pc_receiver
PC_RECEIVER_OBJ := $(PC_DIR)/pc_receiver.o

PC_BINS := $(PC_RECEIVER_BIN)

# ---- 顶层目标 ----
.PHONY: all arm arm-tools pc install clean help

# 默认只编译板端程序
all: arm

# 同时编译板端、PC 端和诊断工具
allboth: arm pc arm-tools
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

$(ARM_SENDER_BIN): $(ARM_SENDER_OBJ) $(ARM_COMMON_OBJS) $(ARM_FB_DISPLAY_OBJ)
	@echo "  [ARM] LD    $@"
	$(ARM_CC) $(ARM_CFLAGS) -o $@ $^ $(LDFLAGS)

# ---- ARM 诊断工具编译 (独立编译, 不依赖 v4l2_capture.o) ----
arm-tools: $(ARM_DIR) $(ARM_TOOLS)
	@echo "================================================================"
	@echo "  ARM tools build complete!"
	@echo "================================================================"
	@ls -lh $(ARM_TOOLS)
	@echo "================================================================"

$(ARM_DIR)/v4l2_info_tool.o: $(TOOLSDIR)/v4l2_info.c
	@echo "  [ARM] CC    $< (tool)"
	$(ARM_CC) $(ARM_CFLAGS) -c -o $@ $<

$(ARM_V4L2_INFO_BIN): $(ARM_V4L2_INFO_OBJ)
	@echo "  [ARM] LD    $@ (tool)"
	$(ARM_CC) $(ARM_CFLAGS) -o $@ $^ $(LDFLAGS)

$(ARM_DIR)/v4l2_format_test.o: $(TOOLSDIR)/v4l2_format_test.c
	@echo "  [ARM] CC    $< (tool)"
	$(ARM_CC) $(ARM_CFLAGS) -c -o $@ $<

$(ARM_FMT_TEST_BIN): $(ARM_FMT_TEST_OBJ)
	@echo "  [ARM] LD    $@ (tool)"
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
install: $(ARM_BINS) $(ARM_TOOLS)
	@echo "  INSTALL to $(DESTDIR)/"
	@$(MKDIR) $(DESTDIR)
	@install -m 755 $(ARM_BINS) $(ARM_TOOLS) $(DESTDIR)/
	@echo "  Install complete:"
	@ls -lh $(DESTDIR)/imx6ull_sender $(DESTDIR)/v4l2_info $(DESTDIR)/v4l2_format_test

# ---- 清理 ----
clean:
	$(RM) -r $(ARM_DIR)/*.o $(ARM_BINS) $(ARM_TOOLS)
	$(RM) -r $(ARM_DIR)
	$(RM) -r $(PC_DIR)/*.o $(PC_BINS)
	$(RM) -r $(PC_DIR)
	@echo "[INFO] Clean done"

# ---- 帮助 ----
help:
	@echo "Usage:"
	@echo "  make            - 编译板端程序 (ARM)"
	@echo "  make arm        - 只编译板端 imx6ull_sender"
	@echo "  make arm-tools  - 编译板端诊断工具 v4l2_info + v4l2_format_test"
	@echo "  make pc         - 只编译 PC 端 pc_receiver"
	@echo "  make allboth    - 同时编译 ARM + PC + tools"
	@echo "  make install    - 安装板端程序到 NFS ($(DESTDIR))"
	@echo "  make clean      - 清理所有编译产物"
