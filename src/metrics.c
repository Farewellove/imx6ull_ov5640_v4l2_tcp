/*
 * metrics.c - 用户态性能统计模块实现
 *
 * 只依赖 POSIX 与 /proc, 不依赖 libm (自带整数平方根), 便于交叉编译到
 * 只有 busybox 的 i.MX6ULL 板端。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "metrics.h"

/* ================================================================
 * 单调时钟
 * ================================================================ */
uint64_t metrics_now_ns(void)
{
	struct timespec ts;

	/* CLOCK_MONOTONIC: 不受系统时间调整影响, 只用于测量间隔 */
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t metrics_now_us(void)
{
	return metrics_now_ns() / 1000ULL;
}

/* ================================================================
 * 整数平方根 (牛顿迭代), 避免为了一个 stddev 而链接 libm
 * ================================================================ */
static uint64_t isqrt_u64(uint64_t v)
{
	uint64_t x, y;

	if (v == 0)
		return 0;
	x = v;
	y = (x + 1) / 2;
	while (y < x) {
		x = y;
		y = (x + v / x) / 2;
	}
	return x;
}

/* ================================================================
 * 样本直方图
 * ================================================================ */
void metrics_hist_init(struct metrics_hist *h)
{
	memset(h, 0, sizeof(*h));
}

void metrics_hist_reset(struct metrics_hist *h)
{
	h->n = 0;
	h->dropped = 0;
}

void metrics_hist_free(struct metrics_hist *h)
{
	free(h->samples);
	memset(h, 0, sizeof(*h));
}

int metrics_hist_add(struct metrics_hist *h, uint64_t value_us)
{
	if (h->n == h->cap) {
		size_t newcap = (h->cap == 0) ? 1024 : h->cap * 2;
		uint64_t *p = (uint64_t *)realloc(h->samples,
		                                  newcap * sizeof(uint64_t));
		if (p == NULL) {
			/* 分配失败: 记 dropped, 让统计结果里能看出样本缺失,
			 * 而不是静默地少统计几个样本 */
			h->dropped++;
			return -1;
		}
		h->samples = p;
		h->cap = newcap;
	}
	h->samples[h->n++] = value_us;
	return 0;
}

/* qsort 比较函数: uint64 升序 */
static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	if (x < y) return -1;
	if (x > y) return 1;
	return 0;
}

int metrics_summary(const struct metrics_hist *h, struct metrics_summary *out)
{
	uint64_t *sorted;
	uint64_t sum = 0;
	size_t i, sz;
	double mean, var;
	size_t idx;

	if (h->n == 0) {
		memset(out, 0, sizeof(*out));
		return -1;
	}

	sz = h->n * sizeof(uint64_t);
	sorted = (uint64_t *)malloc(sz);
	if (sorted == NULL)
		return -1;
	memcpy(sorted, h->samples, sz);

	for (i = 0; i < h->n; i++)
		sum += h->samples[i];
	mean = (double)sum / (double)h->n;

	qsort(sorted, h->n, sizeof(uint64_t), cmp_u64);

	out->count  = h->n;
	out->min_us = (double)sorted[0];
	out->max_us = (double)sorted[h->n - 1];
	out->mean_us = mean;

	/* 最近秩法: 第 p 分位 = 排序后第 ceil(p/100 * n) 个 (1-based) */
	idx = (size_t)((h->n * 50 + 99) / 100);  if (idx < 1) idx = 1;
	out->p50_us = (double)sorted[idx - 1];
	idx = (size_t)((h->n * 95 + 99) / 100);  if (idx < 1) idx = 1;
	out->p95_us = (double)sorted[idx - 1];
	idx = (size_t)((h->n * 99 + 99) / 100);  if (idx < 1) idx = 1;
	out->p99_us = (double)sorted[idx - 1];

	/* 总体标准差 */
	var = 0.0;
	for (i = 0; i < h->n; i++) {
		double d = (double)h->samples[i] - mean;
		var += d * d;
	}
	var /= (double)h->n;
	out->stddev_us = (double)isqrt_u64((uint64_t)(var + 0.5));

	free(sorted);
	return 0;
}

void metrics_print_summary(const char *label, const struct metrics_summary *s)
{
	printf("[METRICS] %-22s n=%llu min=%.1f mean=%.1f p50=%.1f "
	       "p95=%.1f p99=%.1f max=%.1f stddev=%.1f (us)\n",
	       label, (unsigned long long)s->count,
	       s->min_us, s->mean_us, s->p50_us,
	       s->p95_us, s->p99_us, s->max_us, s->stddev_us);
}

/* ================================================================
 * 进程资源采样
 * ================================================================ */
void metrics_proc_tracker_init(struct metrics_proc_tracker *t)
{
	memset(t, 0, sizeof(*t));
	t->clk_tck = sysconf(_SC_CLK_TCK);
	if (t->clk_tck <= 0)
		t->clk_tck = 100;   /* 内核默认 */
}

/*
 * 解析 /proc/<pid>/stat。
 *
 * 坑: 第 2 个字段 comm 是"(进程名)", 里面可以有空格和右括号, 所以不能
 * 简单地按空格切分。正确做法是找最后一个 ')' 之后的第一个非空格字符,
 * 从那里开始才是第 3 个字段 (state)。
 * 需要的字段: utime(14) stime(15) num_threads(20)。
 */
static int parse_proc_stat(pid_t pid, uint64_t *cpu_ticks, uint64_t *threads)
{
	char path[64];
	char buf[1024];
	FILE *fp;
	char *p, *end;
	int field;
	uint64_t utime = 0, stime = 0, nthr = 0;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	fp = fopen(path, "r");
	if (fp == NULL)
		return -1;
	if (fgets(buf, sizeof(buf), fp) == NULL) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	p = strrchr(buf, ')');
	if (p == NULL)
		return -1;
	p++;                     /* 跳过 ')' */
	while (*p == ' ')
		p++;

	/* p 现在指向 state, 即第 3 个字段 */
	field = 3;
	while (*p != '\0') {
		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		end = p;
		while (*end != '\0' && *end != ' ')
			end++;

		if (field == 14) {
			utime = strtoull(p, NULL, 10);
		} else if (field == 15) {
			stime = strtoull(p, NULL, 10);
		} else if (field == 20) {
			nthr = strtoull(p, NULL, 10);
			break;      /* 后面不需要了 */
		}
		p = end;
		field++;
	}

	*cpu_ticks = utime + stime;
	*threads = nthr;
	return 0;
}

/* 从 /proc/<pid>/status 读 VmRSS / VmSize */
static int parse_proc_status(pid_t pid, uint64_t *rss_kb, uint64_t *vsz_kb)
{
	char path[64];
	char line[256];
	FILE *fp;

	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	fp = fopen(path, "r");
	if (fp == NULL)
		return -1;

	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strncmp(line, "VmRSS:", 6) == 0)
			*rss_kb = strtoull(line + 6, NULL, 10);
		else if (strncmp(line, "VmSize:", 7) == 0)
			*vsz_kb = strtoull(line + 7, NULL, 10);
	}
	fclose(fp);
	return 0;
}

int metrics_proc_tracker_sample(struct metrics_proc_tracker *t, pid_t pid,
                                struct metrics_proc_usage *out)
{
	uint64_t cpu_ticks = 0, threads = 0;
	uint64_t now_ns;

	memset(out, 0, sizeof(*out));

	if (pid == 0)
		pid = getpid();

	if (parse_proc_stat(pid, &cpu_ticks, &threads) < 0)
		return -1;
	if (parse_proc_status(pid, &out->rss_kb, &out->vsz_kb) < 0)
		return -1;

	now_ns = metrics_now_ns();
	out->threads = threads;
	out->cpu_ticks_sec = (double)cpu_ticks / (double)t->clk_tck;
	out->valid = 1;

	if (!t->inited) {
		/* 第一次只建立基准, 不产出 CPU 百分比 */
		t->inited = 1;
		t->last_cpu_ticks = cpu_ticks;
		t->last_ns = now_ns;
		out->cpu_percent = 0.0;
		out->interval_sec = 0.0;
		return 0;
	}

	{
		double d_wall = (double)(now_ns - t->last_ns) / 1e9;
		double d_cpu  = (double)(cpu_ticks - t->last_cpu_ticks)
		                / (double)t->clk_tck;

		out->interval_sec = d_wall;
		if (d_wall > 0.0)
			out->cpu_percent = d_cpu / d_wall * 100.0;
		else
			out->cpu_percent = 0.0;
	}
	t->last_cpu_ticks = cpu_ticks;
	t->last_ns = now_ns;
	return 0;
}

/* ================================================================
 * CSV
 * ================================================================ */
FILE *metrics_csv_open(const char *path, const char *header)
{
	FILE *fp = fopen(path, "w");

	if (fp == NULL) {
		perror("fopen csv");
		fprintf(stderr, "[METRICS] cannot open %s\n", path);
		return NULL;
	}
	if (header != NULL)
		fprintf(fp, "%s\n", header);
	return fp;
}
