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
