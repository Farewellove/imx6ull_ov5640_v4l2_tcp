/*
 * imx6ull_sender.c - i.MX6ULL 板端采集发送程序
 *
 * 默认采集格式: YUYV (V4L2_PIX_FMT_YUYV)
 * 使用 S_FMT 驱动实际返回的 pixfmt 构造帧头 (ctx.pixfmt)
 *
 * 用法:
 *   ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000
 *   ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 --pixfmt yuyv
 *   ./imx6ull_sender -d /dev/video1 -s 192.168.1.66 -p 5000 --yuv-order uyvy
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "frame_proto.h"
#include "v4l2_capture.h"
#include "tcp_socket.h"
#include "fb_display.h"
#include "metrics.h"
#include "touch_input.h"
#include "gesture_zoom.h"

/*
 * 采集循环的运行标志。
 * 必须是 volatile sig_atomic_t: 在信号处理函数里被改写, 主循环里被读,
 * 用 sig_atomic_t 保证读写是单指令、不会被中断撕裂, volatile 阻止
 * 编译器把它优化进寄存器 (否则循环可能永远看不到变化)。
 */
static volatile sig_atomic_t g_running = 1;

/*
 * SIGINT/SIGTERM 处理: 只置标志位就返回, 不在信号上下文里做清理。
 * 真正的清理 (关 socket、STREAMOFF、munmap) 交给主循环退出后按顺序做
 * —— 信号处理函数里能安全调用的函数非常有限, 在那里做清理容易出问题。
 */
static void signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
	fprintf(stderr, "\n[INFO] Received signal, exiting...\n");
}

/* 命令行格式名 → FOURCC (-f / --pixfmt)。返回 0 表示无法识别 */
static uint32_t name_to_pixfmt(const char *name)
{
	if (strcasecmp(name, "RGB565")  == 0) return V4L2_PIX_FMT_RGB565;
	if (strcasecmp(name, "RGB565X") == 0) return V4L2_PIX_FMT_RGB565X;
	if (strcasecmp(name, "BGR565")  == 0) return PIX_FMT_BGR565;
	if (strcasecmp(name, "BGR565X") == 0) return PIX_FMT_BGR565X;
	if (strcasecmp(name, "YUYV")    == 0) return V4L2_PIX_FMT_YUYV;
	if (strcasecmp(name, "UYVY")    == 0) return V4L2_PIX_FMT_UYVY;
	if (strcasecmp(name, "YVYU")    == 0) return PIX_FMT_YVYU;
	if (strcasecmp(name, "VYUY")    == 0) return PIX_FMT_VYUY;
	if (strcasecmp(name, "MJPEG")   == 0) return V4L2_PIX_FMT_MJPEG;
	return 0;
}

/*
 * YUV 字节序名 → 内部 FOURCC (--yuv-order)。
 * 用于覆盖 S_FMT 的返回值: 当驱动报告 YUYV 但实际字节排列不对时,
 * 用它改掉帧头里声明的 pixfmt, 让 PC 端按正确的顺序解码。
 * 注意这**只改帧头标签**, 板端不做任何字节重排 (数据原样发出)。
 */
static uint32_t yuv_order_to_pixfmt(const char *name)
{
	if (strcasecmp(name, "yuyv") == 0) return V4L2_PIX_FMT_YUYV;
	if (strcasecmp(name, "uyvy") == 0) return V4L2_PIX_FMT_UYVY;
	if (strcasecmp(name, "yvyu") == 0) return PIX_FMT_YVYU;
	if (strcasecmp(name, "vyuy") == 0) return PIX_FMT_VYUY;
	return 0;
}

/*
 * 发送帧头: 先拷到局部结构里逐字段 htonl, 再一次性 send_all。
 *
 * 为什么要拷一份而不是就地改 hdr: hdr 是主循环里的变量, 就地转成网络序
 * 会让后续打印/复用拿到错的值。用局部结构既不破坏调用者数据, 也保证
 * 发出的 40 字节里所有多字节字段都是大端。
 *
 * timestamp_us 是 64 位, 而 htonl 只管 32 位: 拆成高 32 位和低 32 位
 * 分别转换, 再拼回一个 uint64_t。
 *
 * 返回 send_all 的结果 (0 成功 / -1 失败)。
 *
 * st 可为 NULL 表示不统计分片情况; 传非 NULL 时把本次 send 的调用次数/
 * 分片次数累加进去 (用于量化"TCP 短发送")。
 */
static int send_frame_header(int sockfd, const struct frame_header *hdr,
                             struct io_stats *st)
{
	struct frame_header net_hdr;

	/* 序列化统一走 frame_proto.h 的 frame_header_hton, 与接收端共用
	 * 同一套实现, 避免两端各自 htonl 写出不一致的线格式 */
	frame_header_hton(hdr, &net_hdr);

	return send_all_stat(sockfd, &net_hdr, sizeof(net_hdr), st);
}

enum {
	OPT_PIXFMT = 256,
	OPT_YUV_ORDER,
	OPT_STATS_DIR,
	OPT_DURATION,
	OPT_WARMUP,
	OPT_STATS_INTERVAL,
	OPT_CAPTURE_ONLY,
	OPT_QUIET,
	OPT_IO_TIMEOUT,
	OPT_BUFFERS,
	OPT_SET_FPS,
	OPT_LCD_SWAP_RB,
	OPT_TOUCH,
	OPT_TOUCH_DEV,
	OPT_TOUCH_REPLAY,
	OPT_ZOOM_MIN,
	OPT_ZOOM_MAX,
	OPT_ZOOM_FIXED,
	OPT_TOUCH_SWAP_XY,
	OPT_TOUCH_INVERT_X,
	OPT_TOUCH_INVERT_Y,
	OPT_TOUCH_DEBUG
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -d <dev>          Video device, default /dev/video1\n"
		"  -s <ip>           Server IP address (required unless --capture-only)\n"
		"  -p <port>         TCP port, default 5000\n"
		"  -w <width>        Image width, default 640\n"
		"  -h <height>       Image height, default 480\n"
		"  -f <fmt>          Pixel format: YUYV(default), UYVY, RGB565, MJPEG\n"
		"  --pixfmt <fmt>    Same as -f\n"
		"  --yuv-order <o>   YUV byte order: yuyv(default), uyvy, yvyu, vyuy\n"
		"  -n <frames>       Number of frames, 0=unlimited (default 0)\n"
		"  -l                Enable LCD display via /dev/fb0\n"
		"  --buffers <n>     REQBUFS count (default 3, max 8). More buffers\n"
		"                    absorb more send jitter in the serial loop.\n"
		"  --set-fps <n>     Request frame rate via VIDIOC_S_PARM (0=don't).\n"
		"                    Applied after S_FMT and before STREAMON.\n"
		"  --lcd-swap-rb     Swap R/B when writing RGB565 to LCD.\n"
		"                    i.MX6ULL CSI may swap R/B: if the board LCD looks\n"
		"                    blue-green tinted, turn this on (no rebuild).\n"
		"\n"
		"Touch zoom options (GT911 双指手势数字缩放, 默认关闭):\n"
		"  --touch           Enable two-finger gesture zoom. Auto-scans\n"
		"                    /dev/input/event0..9 for an MT touchscreen.\n"
		"  --touch-dev <dev> Use an explicit device node (e.g. /dev/input/event1)\n"
		"  --touch-replay <evlog>\n"
		"                    Replay a synthetic MT event log instead of a real\n"
		"                    device (offline verification).\n"
		"  --zoom-min <f>    Min zoom, default 1.0\n"
		"  --zoom-max <f>    Max zoom, default 2.0\n"
		"  --zoom-fixed <f>  Force a fixed zoom, no touch needed (for the\n"
		"                    1.0x/1.5x/2.0x benchmark matrix)\n"
		"  --touch-swap-xy   Swap touch X/Y (panel mounting orientation)\n"
		"  --touch-debug     Print every raw touch frame (finger count and\n"
		"                    coordinates) for the first 300 frames. Use this\n"
		"                    to verify the panel really reports 2 fingers\n"
		"                    before blaming the gesture logic.\n"
		"  --touch-invert-x / --touch-invert-y\n"
		"                    Invert a touch axis if the mapping is mirrored\n"
		"\n"
		"Benchmark options (default OFF, 不影响原有功能):\n"
		"  --stats-dir <dir>   Enable stats, write capture_frames.csv /\n"
		"                      process_usage.csv into <dir>\n"
		"  --duration <sec>    Stop after <sec> seconds (0=unlimited)\n"
		"  --warmup <frames>   Exclude first <frames> from interval stats\n"
		"  --stats-interval <sec>  Periodic resource sample interval (default 1)\n"
		"  --capture-only      Skip TCP entirely (capture-only mode)\n"
		"  --quiet             Suppress per-frame stdout (reduces console load)\n"
		"  --io-timeout <sec>  SO_SNDTIMEO: abort instead of blocking forever\n"
		"                      when the receiver stops reading\n"
		"\n"
		"Examples:\n"
		"  %s -d /dev/video1 -s 192.168.1.66 -p 5000\n"
		"  %s -d /dev/video1 -s 192.168.1.66 -p 5000 --yuv-order uyvy\n"
		"  %s -d /dev/video1 --capture-only --duration 60 --quiet "
		"--stats-dir ./bench/capture_only\n"
		"  %s -d /dev/video1 -s 192.168.1.66 --duration 60 --quiet "
		"--stats-dir ./bench/capture_tcp\n",
		prog, prog, prog, prog, prog);
}

/* ================================================================
 * 板端基准测试状态 (--stats-dir 开启后才使用)
 *
 * 设计原则: 所有字段默认零值 = 统计关闭, 主循环在统计关闭时走原来的
 * 代码路径, 保证不开启统计时行为与改动前完全一致。
 * ================================================================ */
struct cap_bench {
	int      enabled;
	int      capture_only;   /* --capture-only: 不发网络 */
	int      quiet;          /* --quiet: 不每帧 printf */
	double   duration_s;     /* 0 = 不限时 */
	double   interval_s;     /* 周期采样间隔 */
	uint32_t warmup;         /* 预热帧数, 不计入分位数 */
	char     dir[256];

	FILE    *fp_frames;
	FILE    *fp_proc;

	uint64_t t_start_ns;
	uint64_t next_sample_ns;

	/* 采集侧计数 */
	uint64_t frames_ok;         /* DQBUF 成功次数 */
	uint64_t frames_reported;   /* 计入统计的帧数 (扣除预热) */
	uint64_t frames_warmup;     /* 预热期帧数 */
	uint64_t poll_timeout;
	uint64_t dqbuf_eagain;
	uint64_t dqbuf_eintr;
	uint64_t dqbuf_error;
	uint64_t buf_flag_error;    /* 带 V4L2_BUF_FLAG_ERROR 的帧数 */
	uint64_t seq_gap_events;    /* sequence 跳变次数 */
	uint64_t seq_gap_total;     /* 累计跳变帧数 (按驱动序号估算) */
	int      seq_prev_valid;
	uint32_t seq_prev;
	uint32_t seq_first;
	uint32_t seq_last;
	uint64_t bytesused_min;
	uint64_t bytesused_max;

	/* TCP 侧计数 */
	uint64_t frames_sent;          /* 帧头+数据都发完的帧数 */
	uint64_t frames_send_failed;
	uint64_t payload_bytes;        /* 成功交给 socket 的 payload 字节 */
	uint64_t header_bytes;
	struct io_stats hdr_io;        /* 帧头收发分片统计 */
	struct io_stats data_io;       /* 帧数据收发分片统计 */

	/* 直方图 (微秒) */
	struct metrics_hist h_interval;  /* T1 间隔 (= 帧间隔) */
	struct metrics_hist h_poll;      /* poll 等待耗时 */
	struct metrics_hist h_lcd;       /* T2 - T1 (显示阶段总耗时) */
	struct metrics_hist h_render;    /* 帧渲染耗时 (不含触摸解析) */
	struct metrics_hist h_gesture;   /* 触摸读取 + 手势判定耗时 */

	/* 缩放/触摸 (GT911 双指手势) */
	int      touch_enabled;
	int      touch_replay;
	uint64_t fit_render_frames;     /* 走无缩放快速路径的帧数 */
	uint64_t zoom_render_frames;    /* 走缩放渲染的帧数 */
	uint64_t frames_rendered;       /* 真正写入 LCD 的帧数 */
	double   zoom_min, zoom_max, zoom_final;
	uint64_t pinch_out, pinch_in, pan_frames, zoom_resets;
	uint64_t touch_syn_dropped;
	struct metrics_hist h_send;      /* T3 - T2 */
	struct metrics_hist h_t3t1;      /* 板内 T3 - T1 */

	/* 进程资源 */
	struct metrics_proc_tracker ptrack;
	double   cpu_sum;
	uint64_t cpu_samples;
	double   cpu_last;
	uint64_t rss_max_kb;

	/* 内部: 上一帧 T1, 用于算帧间隔 */
	uint64_t prev_t1_ns;
	int      prev_t1_valid;

	/* 配置快照 —— 写进 summary.csv, 让每个数据文件自带复现条件,
	 * 避免"过几天忘了这份 CSV 是什么分辨率/多少缓冲跑出来的" */
	uint32_t width, height, pixfmt;
	const char *pixfmt_name;
	int      nbufs_actual;     /* 驱动实际给的 buffer 数 */
	int      req_buffers;      /* 命令行请求的 buffer 数 (0=默认) */
	uint32_t req_fps;          /* 命令行请求的帧率 (0=未请求) */
	uint32_t fps_num;          /* G_PARM 实际 numerator */
	uint32_t fps_den;          /* G_PARM 实际 denominator */
	int      fps_supported;    /* 驱动是否声明 TIMEPERFRAME */
	int      lcd_swap_rb;
	int      lcd_enabled;
};

static struct cap_bench g_bench;

static int bench_start(struct cap_bench *b)
{
	char path[320];

	if (!b->enabled)
		return 0;

	if (b->dir[0] == '\0')
		snprintf(b->dir, sizeof(b->dir), ".");

	if (mkdir(b->dir, 0755) < 0 && errno != EEXIST) {
		perror("mkdir stats dir");
		return -1;
	}

	snprintf(path, sizeof(path), "%s/capture_frames.csv", b->dir);
	b->fp_frames = metrics_csv_open(path,
		"frame_index,frames_ok,frame_id,sequence,seq_gap,bytesused,"
		"buf_index,flags,warmup,poll_us,lcd_us,send_us,t3t1_us,"
		"v4l2_ts_us,t1_ns,t2_ns,t3_ns,tcp_enabled,sent,"
		/* 缩放/手势 (追加在末尾) */
		"gesture_us,render_us,zoom,view_x0,view_y0,render_path");
	if (b->fp_frames == NULL)
		return -1;

	snprintf(path, sizeof(path), "%s/process_usage.csv", b->dir);
	b->fp_proc = metrics_csv_open(path,
		"elapsed_s,cpu_percent,rss_kb,vsz_kb,threads,"
		"frames_ok,fps_interval,frames_sent,payload_bytes");
	if (b->fp_proc == NULL)
		return -1;

	metrics_proc_tracker_init(&b->ptrack);
	b->t_start_ns = metrics_now_ns();
	b->next_sample_ns = b->t_start_ns +
	                    (uint64_t)(b->interval_s * 1e9);
	b->bytesused_min = (uint64_t)-1;
	b->seq_first = 0;
	b->seq_last = 0;

	/*
	 * 立刻建立一次 CPU 基准。
	 *
	 * metrics 的 CPU 百分比是"区间比值", 第一次采样只能建立基准。如果不
	 * 在这里建, 那么运行时长不足一个采样间隔的测试 (例如 --duration 5)
	 * 会得到 cpu_percent = -1、rss = 0, 白白丢掉数据。这里先采一次,
	 * bench_finish 里再补采一次, 任何时长就都有 CPU/RSS 了。
	 */
	{
		struct metrics_proc_usage pu0;
		if (metrics_proc_tracker_sample(&b->ptrack, getpid(),
		                                &pu0) == 0 && pu0.valid)
			b->rss_max_kb = pu0.rss_kb;
	}

	printf("[BENCH] stats enabled: dir=%s, duration=%.0fs, warmup=%u, "
	       "interval=%.1fs, tcp=%s, quiet=%d\n",
	       b->dir, b->duration_s, b->warmup, b->interval_s,
	       b->capture_only ? "off" : "on", b->quiet);
	printf("[BENCH] config: %ux%u pixfmt=%s nbufs=%d req_bufs=%d "
	       "req_fps=%u actual_fps=%u/%u tf_supported=%d "
	       "tcp=%d lcd=%d lcd_swap_rb=%d\n",
	       b->width, b->height, b->pixfmt_name,
	       b->nbufs_actual, b->req_buffers, b->req_fps,
	       b->fps_num, b->fps_den, b->fps_supported,
	       b->capture_only ? 0 : 1, b->lcd_enabled, b->lcd_swap_rb);
	return 0;
}

static void bench_close(struct cap_bench *b)
{
	if (b->fp_frames) { fclose(b->fp_frames); b->fp_frames = NULL; }
	if (b->fp_proc)   { fclose(b->fp_proc);   b->fp_proc = NULL; }
}

/*
 * 记录一帧的统计。
 *
 * 时间轴定义 (全部是 CLOCK_MONOTONIC, 单位 ns):
 *   t_poll : 进入 poll 前的时刻
 *   t1     : DQBUF 成功返回后立即取 (= 帧到用户态的时刻)  ★ 帧间隔用 t1 差
 *   t2     : 必要的本地处理 (LCD 转换/写入) 之后
 *   t3     : 整帧 (帧头 + 数据) 全部交给 socket 之后
 *
 * 注意 t3 只代表"内核 socket 缓冲区收下了", 不代表数据已到达 PC。
 * 跨主机的单调时钟不能相减, 所以这里只报板内 t3-t1, 不声称端到端延迟。
 */
static void bench_frame_row(struct cap_bench *b,
                            uint64_t frame_index, uint32_t frame_id,
                            const struct v4l2_frame *fr,
                            uint64_t expected_seq,
                            uint64_t t1, uint64_t t2, uint64_t t3,
                            uint64_t poll_us, uint64_t lcd_us,
                            uint64_t send_us, uint64_t t3t1_us,
                            uint64_t gesture_us, uint64_t render_us,
                            const struct fb_view *view, int render_path,
                            int warm, int sent)
{
	double zoom = 1.0, vx = 0.0, vy = 0.0;

	if (!b->enabled || b->fp_frames == NULL)
		return;

	if (view != NULL) {
		zoom = view->zoom;
		vx   = view->view_x0;
		vy   = view->view_y0;
	}

	fprintf(b->fp_frames,
		"%llu,%llu,%u,%u,%lld,%zu,%d,0x%08X,%d,"
		"%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,"
		/* 缩放/手势相关列 (追加在末尾, 不影响既有列号) */
		"%llu,%llu,%.6f,%.3f,%.3f,%d\n",
		(unsigned long long)frame_index,
		(unsigned long long)b->frames_ok,
		frame_id,
		fr->sequence,
		(long long)((int64_t)fr->sequence - (int64_t)expected_seq),
		fr->bytesused,
		fr->index,
		fr->flags,
		warm ? 1 : 0,
		(unsigned long long)poll_us,
		(unsigned long long)lcd_us,
		(unsigned long long)send_us,
		(unsigned long long)t3t1_us,
		(unsigned long long)fr->timestamp_us,
		(unsigned long long)t1,
		(unsigned long long)t2,
		(unsigned long long)t3,
		b->capture_only ? 0 : 1,
		sent,
		(unsigned long long)gesture_us,
		(unsigned long long)render_us,
		zoom, vx, vy,
		render_path);
}

/* 周期资源采样 + 写 process_usage.csv */
static void bench_periodic(struct cap_bench *b, uint64_t now_ns,
                           uint64_t frames_ok, uint64_t frames_sent)
{
	struct metrics_proc_usage pu;
	static uint64_t last_frames = 0;
	double elapsed, fps_int;

	if (!b->enabled || b->fp_proc == NULL)
		return;
	if (now_ns < b->next_sample_ns)
		return;

	elapsed = (double)(now_ns - b->t_start_ns) / 1e9;
	fps_int = (double)(frames_ok - last_frames) / b->interval_s;
	last_frames = frames_ok;

	if (metrics_proc_tracker_sample(&b->ptrack, getpid(), &pu) == 0 && pu.valid) {
		b->cpu_sum += pu.cpu_percent;
		b->cpu_samples++;
		b->cpu_last = pu.cpu_percent;
		if (pu.rss_kb > b->rss_max_kb)
			b->rss_max_kb = pu.rss_kb;
		fprintf(b->fp_proc, "%.3f,%.1f,%llu,%llu,%llu,%llu,%.2f,%llu,%llu\n",
		        elapsed, pu.cpu_percent,
		        (unsigned long long)pu.rss_kb,
		        (unsigned long long)pu.vsz_kb,
		        (unsigned long long)pu.threads,
		        (unsigned long long)frames_ok, fps_int,
		        (unsigned long long)frames_sent,
		        (unsigned long long)b->payload_bytes);
	}

	b->next_sample_ns += (uint64_t)(b->interval_s * 1e9);
}

/* 是否已到 --duration 指定的运行时长 */
static int bench_time_up(struct cap_bench *b, uint64_t now_ns)
{
	if (!b->enabled || b->duration_s <= 0.0)
		return 0;
	return (double)(now_ns - b->t_start_ns) / 1e9 >= b->duration_s;
}

/* 结束时打印汇总 + 写 summary */
static void bench_finish(struct cap_bench *b, uint64_t t_end_ns)
{
	struct metrics_summary s;
	double elapsed;
	FILE *fp;
	char path[320];
	uint64_t n;

	if (!b->enabled)
		return;

	elapsed = (double)(t_end_ns - b->t_start_ns) / 1e9;

	/*
	 * 结束时补采一次资源。
	 *
	 * 与 bench_start 的基准采样配对: 即使整个运行时长不足一个采样间隔
	 * (周期采样一次都没触发), 这里也能得到有效的 CPU 百分比与 RSS。
	 * 不补的话, 短时测试会输出 cpu_percent=-1、rss=0, 等于白丢数据。
	 */
	{
		struct metrics_proc_usage pu;
		if (metrics_proc_tracker_sample(&b->ptrack, getpid(), &pu) == 0 &&
		    pu.valid) {
			if (pu.interval_sec > 0.0) {
				b->cpu_sum += pu.cpu_percent;
				b->cpu_samples++;
			}
			if (pu.rss_kb > b->rss_max_kb)
				b->rss_max_kb = pu.rss_kb;
		}
	}

	printf("\n================ BENCHMARK SUMMARY ================\n");
	printf("mode             : %s\n",
	       b->capture_only ? "capture-only" : "capture+tcp");
	printf("elapsed_s        : %.3f\n", elapsed);
	printf("frames_ok        : %llu\n",
	       (unsigned long long)b->frames_ok);
	printf("frames_reported  : %llu (warmup excluded %llu)\n",
	       (unsigned long long)b->frames_reported,
	       (unsigned long long)b->frames_warmup);
	if (elapsed > 0.0)
		printf("fps_overall      : %.3f\n",
		       (double)b->frames_ok / elapsed);
	printf("poll_timeout     : %llu\n", (unsigned long long)b->poll_timeout);
	printf("dqbuf_eagain     : %llu\n", (unsigned long long)b->dqbuf_eagain);
	printf("dqbuf_eintr      : %llu\n", (unsigned long long)b->dqbuf_eintr);
	printf("dqbuf_error      : %llu\n", (unsigned long long)b->dqbuf_error);
	printf("buf_flag_error   : %llu\n",
	       (unsigned long long)b->buf_flag_error);
	printf("sequence         : first=%u last=%u\n",
	       b->seq_first, b->seq_last);
	printf("seq_gap_events   : %llu\n",
	       (unsigned long long)b->seq_gap_events);
	printf("seq_gap_frames   : %llu (driver-sequence based)\n",
	       (unsigned long long)b->seq_gap_total);
	if (b->bytesused_min == (uint64_t)-1)
		b->bytesused_min = 0;
	printf("bytesused        : min=%llu max=%llu\n",
	       (unsigned long long)b->bytesused_min,
	       (unsigned long long)b->bytesused_max);
	printf("frames_sent      : %llu\n",
	       (unsigned long long)b->frames_sent);
	printf("frames_send_fail : %llu\n",
	       (unsigned long long)b->frames_send_failed);
	printf("payload_bytes    : %llu\n",
	       (unsigned long long)b->payload_bytes);
	printf("header_bytes     : %llu\n",
	       (unsigned long long)b->header_bytes);
	if (elapsed > 0.0)
		printf("payload_mbps     : %.3f (payload only, excl. header)\n",
		       (double)b->payload_bytes * 8.0 / elapsed / 1e6);
	printf("send_calls       : hdr=%llu data=%llu\n",
	       (unsigned long long)b->hdr_io.calls,
	       (unsigned long long)b->data_io.calls);
	printf("send_partial     : hdr=%llu data=%llu (short send rounds)\n",
	       (unsigned long long)b->hdr_io.partial,
	       (unsigned long long)b->data_io.partial);
	if (b->cpu_samples > 0)
		printf("cpu_percent_avg  : %.1f (%% of one core, %llu samples)\n",
		       b->cpu_sum / (double)b->cpu_samples,
		       (unsigned long long)b->cpu_samples);
	printf("rss_max_kb       : %llu\n",
	       (unsigned long long)b->rss_max_kb);

	/* ---- 显示与缩放 (GT911 双指手势) ---- */
	printf("--- display / zoom ---\n");
	printf("frames_rendered  : %llu\n",
	       (unsigned long long)b->frames_rendered);
	if (elapsed > 0.0)
		printf("display_fps      : %.3f (frames actually written to LCD)\n",
		       (double)b->frames_rendered / elapsed);
	printf("render_path      : fit(no-scale)=%llu  zoomed=%llu\n",
	       (unsigned long long)b->fit_render_frames,
	       (unsigned long long)b->zoom_render_frames);
	printf("touch_enabled    : %d%s\n", b->touch_enabled,
	       b->touch_replay ? " (replay)" : "");
	if (b->touch_enabled) {
		printf("zoom             : min=%.2f max=%.2f final=%.3f\n",
		       b->zoom_min, b->zoom_max, b->zoom_final);
		printf("gestures         : pinch_out=%llu pinch_in=%llu "
		       "pan_frames=%llu resets=%llu\n",
		       (unsigned long long)b->pinch_out,
		       (unsigned long long)b->pinch_in,
		       (unsigned long long)b->pan_frames,
		       (unsigned long long)b->zoom_resets);
		printf("touch_syn_dropped: %llu\n",
		       (unsigned long long)b->touch_syn_dropped);
	}
	if (b->h_interval.dropped || b->h_send.dropped)
		printf("WARN             : histogram dropped samples "
		       "(interval=%llu send=%llu)\n",
		       (unsigned long long)b->h_interval.dropped,
		       (unsigned long long)b->h_send.dropped);

	n = (uint64_t)b->frames_reported;
	if (n > 0) {
		if (metrics_summary(&b->h_interval, &s) == 0)
			metrics_print_summary("frame_interval", &s);
		if (metrics_summary(&b->h_poll, &s) == 0)
			metrics_print_summary("poll_wait", &s);
		if (metrics_summary(&b->h_lcd, &s) == 0)
			metrics_print_summary("lcd_stage(T2-T1)", &s);
		if (metrics_summary(&b->h_render, &s) == 0)
			metrics_print_summary("render(zoom)", &s);
		if (b->touch_enabled && metrics_summary(&b->h_gesture, &s) == 0)
			metrics_print_summary("gesture", &s);
		if (metrics_summary(&b->h_send, &s) == 0)
			metrics_print_summary("send(T3-T2)", &s);
		if (metrics_summary(&b->h_t3t1, &s) == 0)
			metrics_print_summary("onboard(T3-T1)", &s);
	} else {
		printf("WARN             : no frames in statistical window, "
		       "percentiles NOT available\n");
	}
	printf("===================================================\n\n");

	/* 机器可读汇总, 供脚本聚合 */
	snprintf(path, sizeof(path), "%s/summary.csv", b->dir);
	fp = metrics_csv_open(path,
		"mode,elapsed_s,frames_ok,frames_reported,warmup,fps_overall,"
		"poll_timeout,dqbuf_eagain,dqbuf_eintr,dqbuf_error,buf_flag_error,"
		"seq_gap_events,seq_gap_frames,bytesused_min,bytesused_max,"
		"frames_sent,frames_send_fail,payload_bytes,header_bytes,"
		"payload_mbps,cpu_percent_avg,rss_max_kb,"
		"interval_mean_us,interval_p95_us,interval_p99_us,interval_max_us,"
		"send_mean_us,send_p99_us,t3t1_mean_us,t3t1_p99_us,"
		/* 配置快照: 让 CSV 自带复现条件 */
		"nbufs,req_buffers,req_fps,fps_num,fps_den,tf_supported,"
		"width,height,pixfmt,lcd,lcd_swap_rb,"
		/* 缩放/手势 (追加在末尾) */
		"frames_rendered,display_fps,fit_render_frames,zoom_render_frames,"
		"touch_enabled,touch_replay,zoom_min,zoom_max,zoom_final,"
		"pinch_out,pinch_in,pan_frames,zoom_resets,touch_syn_dropped,"
		"render_mean_us,render_p95_us,render_p99_us,render_max_us,"
		"gesture_mean_us,gesture_p99_us");
	if (fp != NULL) {
		struct metrics_summary si, ss, st, sr, sg;
		int oki = (metrics_summary(&b->h_interval, &si) == 0);
		int oks = (b->capture_only ? 0 : (metrics_summary(&b->h_send, &ss) == 0));
		int okt = (b->capture_only ? 0 : (metrics_summary(&b->h_t3t1, &st) == 0));
		int okr = (metrics_summary(&b->h_render, &sr) == 0);
		int okg = (metrics_summary(&b->h_gesture, &sg) == 0);
		fprintf(fp,
			"%s,%.3f,%llu,%llu,%u,%.3f,%llu,%llu,%llu,%llu,%llu,"
			"%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%.1f,%llu,"
			"%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
			"%d,%d,%u,%u,%u,%d,%u,%u,0x%08X,%d,%d,"
			/* 缩放/手势 */
			"%llu,%.3f,%llu,%llu,%d,%d,%.2f,%.2f,%.3f,"
			"%llu,%llu,%llu,%llu,%llu,"
			"%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
			b->capture_only ? "capture-only" : "capture+tcp",
			elapsed,
			(unsigned long long)b->frames_ok,
			(unsigned long long)b->frames_reported,
			b->warmup,
			elapsed > 0.0 ? (double)b->frames_ok / elapsed : 0.0,
			(unsigned long long)b->poll_timeout,
			(unsigned long long)b->dqbuf_eagain,
			(unsigned long long)b->dqbuf_eintr,
			(unsigned long long)b->dqbuf_error,
			(unsigned long long)b->buf_flag_error,
			(unsigned long long)b->seq_gap_events,
			(unsigned long long)b->seq_gap_total,
			(unsigned long long)b->bytesused_min,
			(unsigned long long)b->bytesused_max,
			(unsigned long long)b->frames_sent,
			(unsigned long long)b->frames_send_failed,
			(unsigned long long)b->payload_bytes,
			(unsigned long long)b->header_bytes,
			elapsed > 0.0
			  ? (double)b->payload_bytes * 8.0 / elapsed / 1e6 : 0.0,
			b->cpu_samples > 0 ? b->cpu_sum / (double)b->cpu_samples : -1.0,
			(unsigned long long)b->rss_max_kb,
			oki ? si.mean_us : -1.0, oki ? si.p95_us : -1.0,
			oki ? si.p99_us : -1.0, oki ? si.max_us : -1.0,
			oks ? ss.mean_us : -1.0, oks ? ss.p99_us : -1.0,
			okt ? st.mean_us : -1.0, okt ? st.p99_us : -1.0,
			b->nbufs_actual, b->req_buffers, b->req_fps,
			b->fps_num, b->fps_den, b->fps_supported,
			b->width, b->height, b->pixfmt,
			b->lcd_enabled, b->lcd_swap_rb,
			(unsigned long long)b->frames_rendered,
			elapsed > 0.0 ? (double)b->frames_rendered / elapsed : 0.0,
			(unsigned long long)b->fit_render_frames,
			(unsigned long long)b->zoom_render_frames,
			b->touch_enabled, b->touch_replay,
			b->zoom_min, b->zoom_max, b->zoom_final,
			(unsigned long long)b->pinch_out,
			(unsigned long long)b->pinch_in,
			(unsigned long long)b->pan_frames,
			(unsigned long long)b->zoom_resets,
			(unsigned long long)b->touch_syn_dropped,
			okr ? sr.mean_us : -1.0, okr ? sr.p95_us : -1.0,
			okr ? sr.p99_us : -1.0, okr ? sr.max_us : -1.0,
			okg ? sg.mean_us : -1.0, okg ? sg.p99_us : -1.0);
		fclose(fp);
		printf("[BENCH] summary written: %s\n", path);
	}

	metrics_hist_free(&b->h_interval);
	metrics_hist_free(&b->h_poll);
	metrics_hist_free(&b->h_lcd);
	metrics_hist_free(&b->h_render);
	metrics_hist_free(&b->h_gesture);
	metrics_hist_free(&b->h_send);
	metrics_hist_free(&b->h_t3t1);
}

int main(int argc, char *argv[])
{
	const char *devname    = "/dev/video1";
	const char *server_ip  = NULL;
	int port               = 5000;
	uint32_t req_width     = 640;
	uint32_t req_height    = 480;
	uint32_t req_pixfmt    = V4L2_PIX_FMT_YUYV;  /* 默认 YUYV */
	uint32_t yuv_override  = 0;    /* --yuv-order 覆盖 */
	int max_frames         = 0;
	int enable_lcd         = 0;
	int send_timeout_s     = 0;   /* --io-timeout: SO_SNDTIMEO (秒) */
	int req_buffers        = 0;   /* --buffers: REQBUFS 数量, 0=默认 3 */
	uint32_t req_fps       = 0;   /* --set-fps: 请求帧率, 0=不设置 */
	int lcd_swap_rb        = 0;   /* --lcd-swap-rb: LCD RGB565 R/B 交换 */
	/* 触摸 / 手势缩放 */
	const char *touch_dev   = NULL;   /* --touch [dev], NULL = 未开启 */
	int      touch_enabled  = 0;
	const char *touch_replay = NULL;  /* --touch-replay <evlog> */
	double   zoom_min       = 1.0;
	double   zoom_max       = 2.0;
	double   zoom_fixed     = 0.0;    /* --zoom-fixed: >0 时固定倍率且不读触摸 */
	int      touch_swap_xy  = 0, touch_inv_x = 0, touch_inv_y = 0;
	int      touch_debug    = 0;   /* --touch-debug: 打印原始触摸帧 */
	int      touch_dbg_frames = 0; /* 已打印的调试帧数 (上限 300) */
	struct touch_ctx tctx;
	struct zoom_state zstate;
	int      touch_ok       = 0;
	int      force_zoomed   = 0;   /* 是否按视图在"无缩放/缩放"两条渲染路径间切换 */
	int opt, opt_index;

	struct v4l2_context ctx;
	struct fb_display fb;
	int sockfd = -1;
	uint32_t frame_id = 0;
	int ret;

	static const struct option long_options[] = {
		{"pixfmt",         required_argument, NULL, OPT_PIXFMT},
		{"yuv-order",      required_argument, NULL, OPT_YUV_ORDER},
		{"stats-dir",      required_argument, NULL, OPT_STATS_DIR},
		{"duration",       required_argument, NULL, OPT_DURATION},
		{"warmup",         required_argument, NULL, OPT_WARMUP},
		{"stats-interval", required_argument, NULL, OPT_STATS_INTERVAL},
		{"capture-only",   no_argument,       NULL, OPT_CAPTURE_ONLY},
		{"no-tcp",         no_argument,       NULL, OPT_CAPTURE_ONLY},
		{"quiet",          no_argument,       NULL, OPT_QUIET},
		{"io-timeout",     required_argument, NULL, OPT_IO_TIMEOUT},
		{"buffers",        required_argument, NULL, OPT_BUFFERS},
		{"set-fps",        required_argument, NULL, OPT_SET_FPS},
		{"lcd-swap-rb",    no_argument,       NULL, OPT_LCD_SWAP_RB},
		{"touch",          no_argument,       NULL, OPT_TOUCH},
		{"touch-dev",      required_argument, NULL, OPT_TOUCH_DEV},
		{"touch-replay",   required_argument, NULL, OPT_TOUCH_REPLAY},
		{"zoom-min",       required_argument, NULL, OPT_ZOOM_MIN},
		{"zoom-max",       required_argument, NULL, OPT_ZOOM_MAX},
		{"zoom-fixed",     required_argument, NULL, OPT_ZOOM_FIXED},
		{"touch-swap-xy",  no_argument,       NULL, OPT_TOUCH_SWAP_XY},
		{"touch-invert-x", no_argument,       NULL, OPT_TOUCH_INVERT_X},
		{"touch-invert-y", no_argument,       NULL, OPT_TOUCH_INVERT_Y},
		{"touch-debug",   no_argument,       NULL, OPT_TOUCH_DEBUG},
		{NULL, 0, NULL, 0}
	};

	/*
	 * stdout 行缓冲。
	 *
	 * 默认 stdout 只在连接终端时行缓冲, 重定向到文件/管道就变成块缓冲
	 * (4096B), 日志会长时间不落盘 —— 自动化脚本用 `prog > log &` 起进程
	 * 后再去 grep 日志判断状态就会一直读不到, 看起来像"卡死"。改成行缓冲
	 * 后, 每行都及时可见; 统计模式用 --quiet 时没有逐帧打印, 不受影响。
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* 统计默认关闭: g_bench 全零 = 原行为 */
	memset(&g_bench, 0, sizeof(g_bench));
	g_bench.interval_s = 1.0;

	while ((opt = getopt_long(argc, argv, "d:s:p:w:h:f:n:l",
	                          long_options, &opt_index)) != -1) {
		switch (opt) {
		case 'd': devname = optarg; break;
		case 's': server_ip = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'w': req_width = (uint32_t)atoi(optarg); break;
		case 'h': req_height = (uint32_t)atoi(optarg); break;
		case 'f':
		case OPT_PIXFMT:
			req_pixfmt = name_to_pixfmt(optarg);
			if (req_pixfmt == 0) {
				fprintf(stderr, "[ERROR] Unknown format: %s\n"
				        "Supported: YUYV, UYVY, RGB565, MJPEG\n",
				        optarg);
				return 1;
			}
			break;
		case OPT_YUV_ORDER:
			yuv_override = yuv_order_to_pixfmt(optarg);
			if (yuv_override == 0) {
				fprintf(stderr, "[ERROR] Unknown yuv order: %s\n"
				        "Supported: yuyv, uyvy, yvyu, vyuy\n",
				        optarg);
				return 1;
			}
			printf("[INFO] YUV byte order override: %s\n", optarg);
			break;
		case 'n': max_frames = atoi(optarg); break;
		case 'l': enable_lcd = 1; break;
		case OPT_STATS_DIR:
			g_bench.enabled = 1;
			snprintf(g_bench.dir, sizeof(g_bench.dir), "%s", optarg);
			break;
		case OPT_DURATION:
			g_bench.duration_s = atof(optarg);
			if (g_bench.duration_s < 0.0)
				g_bench.duration_s = 0.0;
			break;
		case OPT_WARMUP:
			g_bench.warmup = (uint32_t)atoi(optarg);
			break;
		case OPT_STATS_INTERVAL:
			g_bench.interval_s = atof(optarg);
			if (g_bench.interval_s <= 0.0)
				g_bench.interval_s = 1.0;
			break;
		case OPT_CAPTURE_ONLY:
			g_bench.capture_only = 1;
			break;
		case OPT_QUIET:
			g_bench.quiet = 1;
			break;
		case OPT_IO_TIMEOUT:
			send_timeout_s = atoi(optarg);
			if (send_timeout_s < 0)
				send_timeout_s = 0;
			break;
		case OPT_BUFFERS:
			req_buffers = atoi(optarg);
			if (req_buffers < 0)
				req_buffers = 0;
			break;
		case OPT_SET_FPS:
			req_fps = (uint32_t)atoi(optarg);
			break;
		case OPT_LCD_SWAP_RB:
			lcd_swap_rb = 1;
			break;
		case OPT_TOUCH:
			touch_enabled = 1;
			break;
		case OPT_TOUCH_DEV:
			touch_dev = optarg;
			touch_enabled = 1;
			break;
		case OPT_TOUCH_REPLAY:
			touch_replay = optarg;
			touch_enabled = 1;
			break;
		case OPT_ZOOM_MIN:
			zoom_min = atof(optarg);
			break;
		case OPT_ZOOM_MAX:
			zoom_max = atof(optarg);
			break;
		case OPT_ZOOM_FIXED:
			zoom_fixed = atof(optarg);
			break;
		case OPT_TOUCH_SWAP_XY:  touch_swap_xy = 1; break;
		case OPT_TOUCH_INVERT_X: touch_inv_x   = 1; break;
		case OPT_TOUCH_INVERT_Y: touch_inv_y   = 1; break;
		case OPT_TOUCH_DEBUG:   touch_debug   = 1; break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (server_ip == NULL && !g_bench.capture_only) {
		fprintf(stderr, "[ERROR] Server IP (-s) is required "
		        "(or use --capture-only)\n\n");
		usage(argv[0]);
		return 1;
	}

	printf("================================================================\n");
	printf("  i.MX6ULL OV5640 V4L2 TCP Sender\n");
	printf("================================================================\n");
	printf("  Device    : %s\n", devname);
	if (g_bench.capture_only)
		printf("  Mode      : capture-only (TCP disabled)\n");
	else
		printf("  Server    : %s:%d\n", server_ip, port);
	printf("  Requested : %s %ux%u (0x%08X)\n",
	       pixfmt_to_string(req_pixfmt), req_width, req_height, req_pixfmt);
	if (yuv_override)
		printf("  YUV Order : %s\n", pixfmt_to_string(yuv_override));
	if (req_buffers > 0)
		printf("  Buffers   : %d (requested)\n", req_buffers);
	if (req_fps > 0)
		printf("  FPS req   : %u (via VIDIOC_S_PARM)\n", req_fps);
	if (lcd_swap_rb)
		printf("  LCD swap  : R/B swapped for RGB565\n");
	printf("================================================================\n\n");

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* 1. V4L2 初始化 (S_FMT 后 ctx.pixfmt = 驱动实际格式;
	 *    --buffers / --set-fps 在这里一并协商, 顺序为
	 *    S_FMT → S_PARM → G_PARM → REQBUFS → STREAMON) */
	printf("[INFO] Initializing V4L2 capture...\n");
	{
		struct v4l2_init_params ip;
		memset(&ip, 0, sizeof(ip));
		ip.devname = devname;
		ip.width   = req_width;
		ip.height  = req_height;
		ip.pixfmt  = req_pixfmt;
		ip.nbufs   = req_buffers;
		ip.fps     = req_fps;

		ret = v4l2_init_ex(&ctx, &ip);
		if (ret < 0) {
			fprintf(stderr, "[ERROR] V4L2 init failed\n");
			return 1;
		}
	}

	/* 记录配置快照 (写进 summary.csv, 使数据文件自带复现条件) */
	g_bench.width         = ctx.width;
	g_bench.height        = ctx.height;
	g_bench.pixfmt        = ctx.pixfmt;
	g_bench.pixfmt_name   = pixfmt_to_string(ctx.pixfmt);
	g_bench.nbufs_actual  = ctx.nbufs;
	g_bench.req_buffers   = req_buffers;
	g_bench.req_fps       = req_fps;
	g_bench.fps_num       = ctx.fps_num;
	g_bench.fps_den       = ctx.fps_den;
	g_bench.fps_supported = ctx.fps_supported;
	g_bench.lcd_swap_rb   = lcd_swap_rb;

	/*
	 * 把 S_FMT 的实际返回结果打出来 —— 这是帧头里 pixfmt/width/height
	 * 的唯一来源, 后面所有环节 (PC 端解码、LCD 转换) 都以它为准。
	 */
	printf("[INFO] ========================================\n");
	printf("[INFO] === S_FMT result (frame_header source) ===\n");
	printf("[INFO]   accepted pixfmt = %s (0x%08X / '%.4s')\n",
	       pixfmt_to_string(ctx.pixfmt), ctx.pixfmt, (char *)&ctx.pixfmt);
	printf("[INFO]   requested       = %s (0x%08X)\n",
	       pixfmt_to_string(req_pixfmt), req_pixfmt);
	printf("[INFO]   width           = %u\n", ctx.width);
	printf("[INFO]   height          = %u\n", ctx.height);
	printf("[INFO]   bytesperline    = %u\n", ctx.bytesperline);
	printf("[INFO]   sizeimage       = %u\n", ctx.sizeimage);

	/*
	 * 严格检查: S_FMT 必须原样接受请求的格式, 否则直接退出。
	 *
	 * 注意这里和 v4l2_capture.c 里的策略不同 —— 那边把格式被替换当作
	 * 警告 (只是个采集库, 由调用者决定怎么办); 这边是最终应用, 格式
	 * 被悄悄换掉会导致 PC 端按错误的解码器解释数据 (典型症状是颜色
	 * 全错或画面撕裂), 与其发出去一堆花屏, 不如启动时就报错退出,
	 * 让用户显式换个支持的格式。
	 */
	if (ctx.pixfmt != req_pixfmt) {
		fprintf(stderr,
			"[ERROR] S_FMT returned %s (0x%08X), but %s (0x%08X) was "
			"requested.\n"
			"        Driver does not support the requested format!\n",
			pixfmt_to_string(ctx.pixfmt), ctx.pixfmt,
			pixfmt_to_string(req_pixfmt), req_pixfmt);
		v4l2_deinit(&ctx);
		return 1;
	}

	/*
	 * YUV order 覆盖: 仅影响 frame_header 中的 pixfmt 标签。
	 * 只在驱动确实返回了 YUV422 时才有意义 (对 RGB565 用 --yuv-order
	 * 没道理), 所以这里用 pixfmt_is_yuv422 把无效组合忽略掉。
	 */
	if (yuv_override && pixfmt_is_yuv422(ctx.pixfmt)) {
		printf("[INFO]   yuv-order override: %s -> %s (header only)\n",
		       pixfmt_to_string(ctx.pixfmt),
		       pixfmt_to_string(yuv_override));
	}

	/* 实测 buffer 数量与帧率 —— 报告里的"缓冲数/帧率"必须以这里为准,
	 * 而不是命令行请求值 (驱动可以改小 count, 也可以不接受 S_PARM) */
	printf("[INFO]   nbufs actual    = %d\n", ctx.nbufs);
	if (ctx.fps_num > 0 && ctx.fps_den > 0)
		printf("[INFO]   timeperframe    = %u/%u (%.2f fps), TIMEPERFRAME %s\n",
		       ctx.fps_num, ctx.fps_den,
		       (double)ctx.fps_den / (double)ctx.fps_num,
		       ctx.fps_supported ? "supported" : "NOT supported");
	else
		printf("[INFO]   timeperframe    = unavailable (G_PARM no value)\n");

	/* LCD 只认 YUYV/RGB565, 其他格式仍能发出去, 只是本地不显示 */
	if (enable_lcd && !pixfmt_is_displayable(ctx.pixfmt))
		printf("[WARN] LCD display may not support pixfmt=%s\n",
		       pixfmt_to_string(ctx.pixfmt));
	printf("[INFO] ========================================\n");

	/*
	 * 2. 连接 TCP
	 * 放在 V4L2 初始化之后: 先确认摄像头能正常工作, 再去占用对方的
	 * 连接。失败要回收已申请的 V4L2 资源, 否则驱动侧缓冲区一直占着。
	 * capture-only 模式 (--capture-only) 完全跳过这一步, 用于测纯采集
	 * 的帧率上限, 作为"网络是否影响采集"的对照组。
	 */
	if (!g_bench.capture_only) {
		sockfd = tcp_client_connect(server_ip, port);
		if (sockfd < 0) {
			fprintf(stderr, "[ERROR] TCP connect failed\n");
			v4l2_deinit(&ctx);
			return 1;
		}
		/*
		 * 对端停止读取 (PC 卡住/被挂起) 时, socket 发送缓冲写满, send()
		 * 会无限阻塞, 采集循环随之停摆, 长时测试 (--duration) 就永远不
		 * 结束。设 SO_SNDTIMEO 后 send 到期返回 EAGAIN, 循环按发送失败
		 * 计数并正常退出, 缓冲区也会归还驱动。
		 */
		if (send_timeout_s > 0) {
			tcp_set_timeouts(sockfd, 0, send_timeout_s);
			printf("[INFO] socket send timeout: %d s\n", send_timeout_s);
		}
	}

	/*
	 * 3. LCD (可选, -l)
	 * 本地显示失败不算致命: 把 enable_lcd 置 0 后继续跑, 图传功能
	 * 不受影响 —— 板子接不接屏都不该影响发图, 这样调试时能少一个
	 * 变量。后续循环里所有 LCD 调用都由 enable_lcd 守着。
	 */
	if (enable_lcd) {
		if (fb_display_init(&fb, "/dev/fb0") < 0) {
			fprintf(stderr, "[WARN] LCD init failed, "
			        "continuing without display\n");
			enable_lcd = 0;
		} else if (lcd_swap_rb) {
			/* i.MX6ULL CSI 输出 RGB565 时可能已交换 R/B, 板端
			 * 直显会偏蓝绿; 此开关只影响 LCD 写入, 不影响发出去
			 * 的原始数据 */
			fb_display_set_swap_rb(&fb, 1);
			printf("[FB] R/B swap enabled for RGB565 -> LCD "
			       "(network payload unchanged)\n");
		}
	}

	/*
	 * 3.5 触摸手势缩放初始化 (可选)
	 *
	 * 顺序要求: 必须在 fb_display_init 之后 —— 手势层需要 LCD 的**真实**
	 * 分辨率, 才能把手势坐标映射到屏幕并确定可见源区域。
	 *
	 * 失败一律不致命: 触摸屏没接、驱动没加载、设备节点变了, 都只告警并
	 * 关闭缩放, 采集与图传照常运行 (满足"触摸设备未连接时保持现有功能")。
	 */
	{
		int force_zoomed_local;

		memset(&tctx, 0, sizeof(tctx));
		memset(&zstate, 0, sizeof(zstate));

		if ((touch_enabled || zoom_fixed > 0.0) && !enable_lcd) {
			fprintf(stderr, "[WARN] --touch/--zoom 只在本地显示上有意义, "
			        "当前没有可用 LCD, 缩放被忽略\n");
			touch_enabled = 0;
		}

		if (touch_enabled && touch_replay != NULL)
			touch_ok = (touch_open_replay(&tctx, touch_replay,
			                             enable_lcd ? fb.screen_width  : 1024,
			                             enable_lcd ? fb.screen_height : 600) == 0);
		else if (touch_enabled)
			touch_ok = (touch_open(&tctx, touch_dev,
			                       enable_lcd ? fb.screen_width  : 1024,
			                       enable_lcd ? fb.screen_height : 600) == 0);

		if (touch_enabled && !touch_ok) {
			fprintf(stderr, "[WARN] touch init failed, "
			        "continuing without gesture zoom\n");
			touch_enabled = 0;
		}
		if (touch_ok)
			touch_set_orientation(&tctx, touch_swap_xy,
			                      touch_inv_x, touch_inv_y);

		/*
		 * 缩放状态机。图像尺寸取 S_FMT 的实际结果 (ctx.width/height),
		 * 与采集分辨率完全一致 —— 缩放只在显示阶段对像素重采样, 不改
		 * 采集分辨率, 也不写采集缓冲区。
		 */
		zoom_state_init(&zstate, ctx.width, ctx.height,
		                enable_lcd ? fb.screen_width  : 1024,
		                enable_lcd ? fb.screen_height : 600,
		                zoom_min, zoom_max);

		if (touch_ok) {
			touch_print_info(&tctx);
			zoom_state_print_info(&zstate);
		}

		/* --zoom-fixed: 固定倍率、不读触摸, 用于 1.0/1.5/2.0 基准对比 */
		if (zoom_fixed > 0.0 && zoom_state_set_zoom(&zstate, zoom_fixed) == 0)
			printf("[ZOOM] fixed zoom = %.2fx (强制走缩放渲染路径, "
			       "不读触摸)\n", zstate.view.zoom);

		g_bench.touch_enabled    = touch_ok;
		g_bench.touch_replay     = (touch_replay != NULL);
		g_bench.zoom_min         = zstate.min_zoom;
		g_bench.zoom_max         = zstate.max_zoom;
		g_bench.touch_syn_dropped = tctx.syn_dropped_cnt;

		/*
		 * 是否需要"按视图判断渲染路径":
		 *   - touch_ok          : 用户随时可能改缩放
		 *   - zoom_fixed > 0    : 强制走缩放路径, 否则 1.0x 恰好是 fit
		 *                         视图, 会退化成无缩放路径, 就测不到
		 *                         缩放渲染代码本身
		 * 都没开启时保持原来的单一无缩放路径 (默认行为不变)。
		 */
		force_zoomed_local = touch_ok || (zoom_fixed > 0.0);
		force_zoomed = force_zoomed_local;
	}

	/*
	 * 4. 采集 + 发送主循环
	 *
	 * 每轮处理一帧: DQBUF 取帧 → (可选) 触摸手势 → (可选) 本地显示
	 * → 发帧头 → 发数据 → QBUF 归还缓冲。三个缓冲区轮转, 所以发送期间
	 * 驱动还能继续往另一个缓冲写。
	 *
	 * max_frames == 0 表示不限帧数, 一直发到 Ctrl+C (改 g_running)。
	 *
	 * 统计说明 (--stats-dir 开启后):
	 *   - 帧间隔/抖动用 T1 (DQBUF 返回) 之差。
	 *   - 发送耗时 T3-T2 只到"交给本机 socket", 不是端到端延迟。
	 *   - gesture_us 是本帧触摸解析+手势判定耗时, render_us 是写显存耗时。
	 *   - 关闭统计时所有 bench_* 都是空操作, 行为与改动前一致。
	 */
	printf("[INFO] Starting capture loop (Ctrl+C to stop)...\n");

	g_bench.lcd_enabled = enable_lcd;

	if (bench_start(&g_bench) < 0) {
		fprintf(stderr, "[ERROR] benchmark init failed\n");
		if (sockfd >= 0) close(sockfd);
		if (enable_lcd) fb_display_deinit(&fb);
		v4l2_deinit(&ctx);
		return 1;
	}

	{
		uint64_t captured = 0;   /* DQBUF 成功次数, 用于 -n 限制与索引 */

		while (g_running &&
		       (max_frames == 0 || (int)captured < max_frames)) {
			struct v4l2_frame fr;
			struct frame_header hdr;
			uint32_t header_pixfmt;
			uint64_t t_poll, t1, t2, t3, t_gest;
			uint64_t poll_us, lcd_us, send_us, t3t1_us;
			uint64_t gesture_us = 0, render_us = 0;
			uint64_t expected_seq;
			int render_path = 0;   /* 0 = 无缩放 blit, 1 = 缩放渲染 */
			int warm, sent = 0, send_failed = 0;

			/* 超时 2000ms: 给足驱动的出帧间隔, 又不至于无限期卡死 */
			t_poll = metrics_now_ns();
			ret = v4l2_get_frame_info(&ctx, &fr, 2000);
			if (ret < 0) {
				/* 超时/暂无数据不是错误, 直接再等下一帧 */
				if (ret == -ETIMEDOUT) { g_bench.poll_timeout++; continue; }
				if (ret == -EAGAIN)    { g_bench.dqbuf_eagain++; continue; }
				/* 被信号打断 -> 用户按了 Ctrl+C, 正常退出 */
				if (ret == -EINTR)     { g_bench.dqbuf_eintr++; break; }
				g_bench.dqbuf_error++;
				fprintf(stderr,
				        "[ERROR] v4l2_get_frame_info failed (ret=%d)\n",
				        ret);
				break;
			}
			t1 = metrics_now_ns();     /* 成功 DQBUF 之后的打点 */

			captured++;
			g_bench.frames_ok++;

			/* 预热期帧不计入分位数统计, 但仍写 CSV 并标记 warmup=1 */
			warm = (g_bench.frames_ok <= g_bench.warmup);
			if (warm)
				g_bench.frames_warmup++;
			else
				g_bench.frames_reported++;

			/* V4L2_BUF_FLAG_ERROR: 该缓冲内容不可信, 计数但不丢弃
			 * (丢弃与否由业务决定; 这里只如实上报) */
			if (fr.flags & V4L2_BUF_FLAG_ERROR)
				g_bench.buf_flag_error++;

			/* driver sequence 跳变检测 */
			expected_seq = g_bench.seq_prev_valid
			               ? (uint64_t)g_bench.seq_prev + 1 : fr.sequence;
			if (!g_bench.seq_prev_valid) {
				g_bench.seq_first = fr.sequence;
				g_bench.seq_prev_valid = 1;
			} else if (fr.sequence != expected_seq && !warm) {
				g_bench.seq_gap_events++;
				/* 只在序号前进时累计缺失帧数; 回绕/乱序不猜数 */
				if (fr.sequence > expected_seq)
					g_bench.seq_gap_total +=
						(uint64_t)(fr.sequence - expected_seq);
			}
			g_bench.seq_prev = fr.sequence;
			g_bench.seq_last = fr.sequence;

			if (!warm) {
				if (fr.bytesused < g_bench.bytesused_min)
					g_bench.bytesused_min = fr.bytesused;
				if (fr.bytesused > g_bench.bytesused_max)
					g_bench.bytesused_max = fr.bytesused;
			}

			/*
			 * ---- 触摸读取 + 双指手势判定 ----
			 *
			 * 放在显示之前: 本帧的缩放视图应当先更新再渲染, 否则画面
			 * 会滞后一帧, 快速捏合时能看出"跟手不跟"。
			 *
			 * guard 上限 64: 万一触摸事件爆发 (或回放文件异常大), 也
			 * 不能把采集循环拖住 —— 触摸只是控制信号, 落后就丢。
			 */
			t_gest = metrics_now_ns();
			if (touch_ok) {
				struct touch_frame tf;
				int guard = 0;

				while (touch_read_frame(&tctx, &tf) == 1) {
					zoom_state_update(&zstate, &tf);

					/* --touch-debug: 触摸是控制信号, 打印只用于定位
					 * "面板到底报了几个触点", 上限 300 帧避免刷屏 */
					if (touch_debug && touch_dbg_frames < 300) {
						int k;

						touch_dbg_frames++;
						printf("[TF#%d] n=%d raw=%d moved=%d drop=%d",
						       touch_dbg_frames, tf.n,
						       tf.n_raw, tf.n_moved,
						       tf.syn_dropped);
						for (k = 0; k < tf.n && k < 5; k++)
							printf(" (%d,%d)",
							       tf.x[k], tf.y[k]);
						printf("\n");
					}

					if (++guard >= 64)
						break;
				}
			}
			gesture_us = (metrics_now_ns() - t_gest) / 1000ULL;

			/*
			 * ---- 本地显示 ----
			 *
			 * 两条路径, 保证默认行为不变:
			 *   - 视图等价于"1.0x 居中" (或未开启缩放) → 原来的无缩放
			 *     blit, 输出与改造前逐像素一致
			 *   - 视图被手势改变 (或 --zoom-fixed) → 最近邻缩放渲染
			 *
			 * --zoom-fixed 必须无条件走缩放路径: 它的用途是给 1.0x 档
			 * 也测到缩放渲染代码本身。原来写的 `force_zoomed &&
			 * !zoom_state_is_fit()` 会被 1.0x 恰好等于 fit 视图这一点
			 * 退化掉 (实测 zoom_1.0 档 zoomed=0, 全部走了 fit), 与上面
			 * 注释宣称的行为不符。
			 * 触摸模式下仍然按视图判断: 1.0x 居中视图走 fit 省性能。
			 *
			 * 缩放只发生在写显存这一步: 采集分辨率不变, 采集缓冲区
			 * (fr.data) 全程只读, 不向其写入任何数据。
			 */
			if (enable_lcd) {
				uint64_t t_r0 = metrics_now_ns();

				if (zoom_fixed > 0.0 ||
				    (touch_ok && !zoom_state_is_fit(&zstate))) {
					fb_display_show_frame_zoomed(&fb, fr.data,
					                             ctx.width, ctx.height,
					                             ctx.bytesperline,
					                             ctx.pixfmt,
					                             &zstate.view);
					g_bench.zoom_render_frames++;
					render_path = 1;
				} else {
					fb_display_show_frame(&fb, fr.data, ctx.width,
					                      ctx.height, ctx.bytesperline,
					                      ctx.pixfmt);
					g_bench.fit_render_frames++;
				}
				render_us = (metrics_now_ns() - t_r0) / 1000ULL;
				g_bench.frames_rendered++;
				t2 = metrics_now_ns();
			} else {
				t2 = t1;
			}
			lcd_us = (t2 - t1) / 1000ULL;
			poll_us = (t1 - t_poll) / 1000ULL;

			/* 帧头: 使用 yuv_override (如果设置) 或 ctx.pixfmt */
			header_pixfmt = (yuv_override && pixfmt_is_yuv422(ctx.pixfmt))
			                ? yuv_override : ctx.pixfmt;

			/*
			 * 组装帧头。width/height/pixfmt 全部取 S_FMT 的实际结果,
			 * size 取 DQBUF 报的 bytesused (不是 ctx.sizeimage ——
			 * MJPEG 这类变长格式每帧大小都不同, 只能以后者为准)。
			 */
			memset(&hdr, 0, sizeof(hdr));
			hdr.magic        = FRAME_MAGIC;
			hdr.version      = FRAME_VERSION;
			hdr.header_size  = sizeof(hdr);
			hdr.width        = ctx.width;
			hdr.height       = ctx.height;
			hdr.pixfmt       = header_pixfmt;
			hdr.size         = (uint32_t)fr.bytesused;
			hdr.frame_id     = frame_id;
			hdr.timestamp_us = fr.timestamp_us;

			/*
			 * 发送失败 (对端断开等) 必须先把 buffer 还给驱动再退出,
			 * 否则 v4l2_deinit 的 STREAMOFF 时该 buffer 仍处于已取出
			 * 状态。这里选择发完即停, 不做重连 —— 断线后帧号会不连续,
			 * 继续发只会让 PC 端收到混乱的数据。
			 */
			if (g_bench.capture_only) {
				t3 = t2;      /* 不发网络, T3 无意义, 与 T2 对齐 */
			} else {
				if (send_frame_header(sockfd, &hdr,
				                      &g_bench.hdr_io) < 0) {
					fprintf(stderr,
					        "[ERROR] send header failed (id=%u)\n",
					        frame_id);
					g_bench.frames_send_failed++;
					send_failed = 1;
				} else if (send_all_stat(sockfd, fr.data,
				                         fr.bytesused,
				                         &g_bench.data_io) < 0) {
					fprintf(stderr,
					        "[ERROR] send data failed (id=%u)\n",
					        frame_id);
					g_bench.frames_send_failed++;
					send_failed = 1;
				} else {
					sent = 1;
					g_bench.frames_sent++;
					g_bench.payload_bytes += fr.bytesused;
					g_bench.header_bytes += sizeof(hdr);
				}
				t3 = metrics_now_ns();
			}
			send_us = (t3 - t2) / 1000ULL;
			t3t1_us = (t3 - t1) / 1000ULL;

			/* ---- 分位数样本 (仅统计窗口内) ---- */
			if (!warm) {
				if (g_bench.prev_t1_valid)
					metrics_hist_add(&g_bench.h_interval,
					                 (t1 - g_bench.prev_t1_ns) / 1000ULL);
				metrics_hist_add(&g_bench.h_poll, poll_us);
				if (enable_lcd) {
					metrics_hist_add(&g_bench.h_lcd, lcd_us);
					/* 渲染耗时单独统计: 这才是 1.0/1.5/2.0x
					 * 对比里"缩放渲染耗时"的口径 (不含触摸解析) */
					metrics_hist_add(&g_bench.h_render, render_us);
					if (touch_ok)
						metrics_hist_add(&g_bench.h_gesture,
						                 gesture_us);
				}
				if (!g_bench.capture_only) {
					metrics_hist_add(&g_bench.h_send, send_us);
					metrics_hist_add(&g_bench.h_t3t1, t3t1_us);
				}
			}
			g_bench.prev_t1_ns = t1;
			g_bench.prev_t1_valid = 1;

			if (!g_bench.quiet)
				printf("[FRAME %u] hdr=%s, %ux%u, %zuB, ts=%lu us%s\n",
				       frame_id, pixfmt_to_string(header_pixfmt),
				       ctx.width, ctx.height,
				       fr.bytesused, (unsigned long)fr.timestamp_us,
				       g_bench.capture_only ? " (no-tcp)" : "");

			bench_frame_row(&g_bench, captured - 1, frame_id, &fr,
			                expected_seq, t1, t2, t3,
			                poll_us, lcd_us, send_us, t3t1_us,
			                gesture_us, render_us, &zstate.view, render_path,
			                warm, sent);

			bench_periodic(&g_bench, t3, captured, g_bench.frames_sent);

			if (sent)
				frame_id++;

			/* 用完立刻归还, 保持驱动侧始终有 buffer 可写 */
			v4l2_put_frame(&ctx, fr.index);

			if (send_failed)
				break;
			if (bench_time_up(&g_bench, metrics_now_ns()))
				break;
		}
	}

	printf("[INFO] Sent %u frames total\n", frame_id);

	/*
	 * 把缩放/手势的最终状态快照进统计里, 再让 bench_finish 输出 ——
	 * 否则 summary.csv 里的 zoom_final / pinch_* 全是 0, 数据无法自证
	 * "这份 CSV 是在什么倍率、发生了什么手势的情况下跑出来的"。
	 *
	 * 必须无条件快照: --zoom-fixed 基准矩阵下 touch_ok == 0 (不读触摸),
	 * 但视图状态依然有效; 只在 touch_ok 时赋值会让这一列在基准矩阵里
	 * 永远是 0.000, 看不出这一档到底跑的是几倍。
	 */
	g_bench.zoom_final        = zstate.view.zoom;
	g_bench.pinch_out         = zstate.pinch_out;
	g_bench.pinch_in          = zstate.pinch_in;
	g_bench.pan_frames        = zstate.pan_frames;
	g_bench.zoom_resets       = zstate.resets;
	g_bench.touch_syn_dropped = tctx.syn_dropped_cnt;
	if (touch_ok || zoom_fixed > 0.0)
		zoom_state_print_stats(&zstate);

	/* 5. 清理: 顺序与初始化相反 (touch → socket → LCD → V4L2) */
	bench_finish(&g_bench, metrics_now_ns());
	bench_close(&g_bench);
	touch_close(&tctx);
	if (sockfd >= 0) close(sockfd);
	if (enable_lcd) fb_display_deinit(&fb);
	v4l2_deinit(&ctx);
	printf("[INFO] Done.\n");
	return 0;
}
