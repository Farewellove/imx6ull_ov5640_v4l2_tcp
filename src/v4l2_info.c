/*
 * v4l2_info.c - 摄像头能力查询工具
 *
 * 用法: ./v4l2_info /dev/video1
 *
 * 打印:
 *   - VIDIOC_QUERYCAP
 *   - VIDIOC_G_FMT
 *   - VIDIOC_G_PARM
 *   - VIDIOC_ENUM_FMT
 *   - VIDIOC_ENUM_FRAMESIZES
 *   - VIDIOC_ENUM_FRAMEINTERVALS
 *
 * 本文件只是个薄壳: 打开设备, 然后调用 v4l2_capture.c 里的
 * v4l2_print_capabilities() 完成全部打印。能力查询的实现只有那一份,
 * 主程序也能复用, 避免两边查询逻辑不一致。
 *
 * !! 注意: 本文件当前**没有被 Makefile 编译** !!
 *
 *   Makefile 里 bin/arm/v4l2_info 是从 tools/v4l2_info.c 编出来的
 *   ($(ARM_DIR)/v4l2_info_tool.o 规则), 规则中没有出现 src/v4l2_info.c。
 *   也就是说实际下发到板子的 v4l2_info 是 tools/ 下那一份 (功能更全,
 *   带 colorspace/field 解析), 本文件目前是历史遗留, 改了不会生效。
 *
 *   若要启用本文件, 需要在 Makefile 中加一条编译 + 链接规则, 并把
 *   v4l2_capture.o 一起链进去 (因为用到了 v4l2_print_capabilities)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "v4l2_capture.h"

int main(int argc, char *argv[])
{
    const char *devname;
    int fd;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video_device>\n", argv[0]);
        fprintf(stderr, "Example: %s /dev/video1\n", argv[0]);
        return 1;
    }
    devname = argv[1];

    fd = open(devname, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        perror("open");
        fprintf(stderr, "Failed to open %s\n", devname);
        return 1;
    }

    printf("================================================================\n");
    printf("  V4L2 Camera Information Tool\n");
    printf("  Device: %s\n", devname);
    printf("================================================================\n\n");

    v4l2_print_capabilities(fd);

    close(fd);
    return 0;
}
