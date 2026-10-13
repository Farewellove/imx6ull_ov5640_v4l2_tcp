/*
 * metrics.h - 用户态性能统计模块
 *
 * 提供一套与业务无关的量化工具, 供板端 imx6ull_sender 和 PC 端 pc_receiver
 * 共用, 默认不参与任何业务逻辑 —— 只有在命令行显式开启统计时才被调用,
 * 因此不影响原有功能与默认输出。
 *
 * 内容:
 *   1. 单调时钟 (CLOCK_MONOTONIC): 所有间隔测量只能用单调时钟, 不能用
 *      CLOCK_REALTIME (会被 NTP/手动改时间影响, 得到负间隔或跳变)。
 *   2. 样本直方图 metrics_hist: 可增长数组, 记录帧间隔/阶段耗时等原始样本,
 *      最后统一算 min/mean/p50/p95/p99/max (最近秩法, 非插值)。
 *   3. 进程资源采样: 读 /proc/<pid>/stat 与 /proc/<pid>/status, 得到
 *      CPU 时间比例 (按采样区间做差) 和 RSS。
 *
 * 兼容 Linux 4.1.15 / GCC 4.9, 不依赖 libm (自带整数平方根)。
 */

#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/* ================================================================
 * 1. 单调时钟
 * ================================================================ */

/* 单调时钟当前值 (纳秒) */
uint64_t metrics_now_ns(void);

/* 单调时钟当前值 (微秒) */
uint64_t metrics_now_us(void);

/* ================================================================
 * 2. 样本直方图与分位数
 *
 * 朴素的"只存和/最大值"无法得到 P95/P99, 所以这里保存全部原始样本。
 * 一帧一个样本, 60 秒 @30fps 只有 1800 个 uint64 (14KB), 完全可接受。
 * ================================================================ */

struct metrics_summary {
	uint64_t count;     /* 参与统计的样本数 */
	double   min_us;    /* 最小 (微秒) */
	double   mean_us;   /* 平均 */
	double   p50_us;    /* 中位数 */
	double   p95_us;    /* 95 分位 */
	double   p99_us;    /* 99 分位 */
	double   max_us;    /* 最大 */
	double   stddev_us; /* 标准差 (衡量抖动) */
};

struct metrics_hist {
	uint64_t *samples;   /* 样本数组 (微秒) */
	size_t    n;         /* 已存样本数 */
	size_t    cap;       /* 当前容量 */
	uint64_t  dropped;   /* 内存分配失败被丢弃的样本数 (不静默) */
};

void metrics_hist_init(struct metrics_hist *h);
int  metrics_hist_add(struct metrics_hist *h, uint64_t value_us);
void metrics_hist_reset(struct metrics_hist *h);
void metrics_hist_free(struct metrics_hist *h);

/*
 * 计算 min/mean/p50/p95/p99/max/stddev。
 * 使用最近秩(nearest-rank)法: 第 p 分位取排序后第 ceil(p/100*n) 个样本,
 * 不做线性插值 —— 对"最差 1% 帧的耗时"这类问题上, 取真实存在的样本更有意义。
 * 内部会对样本做一份拷贝后排序, 不破坏原始顺序。
 */
int metrics_summary(const struct metrics_hist *h, struct metrics_summary *out);

/* 打印一行摘要 (前缀 label), 便于日志 grep */
void metrics_print_summary(const char *label, const struct metrics_summary *s);

/* ================================================================
 * 3. 进程资源采样 (/proc)
 * ================================================================ */

struct metrics_proc_usage {
	int      valid;         /* 1 = 本次采样成功 */
	uint64_t rss_kb;        /* 常驻内存集 (KB), 来自 VmRSS */
	uint64_t vsz_kb;        /* 虚拟内存 (KB), 来自 VmSize */
	uint64_t threads;       /* 线程数 */
	double   cpu_ticks_sec; /* 累计 CPU 时间 (utime+stime, 秒) */
	double   cpu_percent;   /* 区间内 CPU 占用百分比 (单核=100%) */
	double   interval_sec;  /* 距上次采样的墙钟秒数 */
};

/*
 * CPU 占用是"区间比值", 单次快照没有意义, 必须保存上一次的 CPU 时间,
 * 所以用一个 tracker 记住状态。
 */
struct metrics_proc_tracker {
	int      inited;
	uint64_t last_cpu_ticks;  /* 上一次 utime+stime (clock tick 数) */
	uint64_t last_ns;         /* 上一次采样时刻 (单调纳秒) */
	long     clk_tck;         /* sysconf(_SC_CLK_TCK), 一般 100 */
};

void metrics_proc_tracker_init(struct metrics_proc_tracker *t);

/*
 * 采样一次。第一次调用只建立基准, cpu_percent 记为 0。
 * pid 传 0 表示当前进程。
 */
int metrics_proc_tracker_sample(struct metrics_proc_tracker *t, pid_t pid,
                                struct metrics_proc_usage *out);

/* ================================================================
 * 4. CSV 输出辅助
 * ================================================================ */

/*
 * 打开一个 CSV 文件并写表头。
 * 返回 FILE*, 失败返回 NULL。调用者负责 fclose。
 */
FILE *metrics_csv_open(const char *path, const char *header);

#endif /* METRICS_H */
