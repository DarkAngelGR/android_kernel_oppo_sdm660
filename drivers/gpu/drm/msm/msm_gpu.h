/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * Copyright (c) 2018 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __MSM_GPU_H__
#define __MSM_GPU_H__

#include <linux/clk.h>
#include <linux/pm_qos.h>
#include <linux/regulator/consumer.h>
#include <linux/notifier.h>

#include "msm_drv.h"
#include "msm_ringbuffer.h"
#include "msm_snapshot.h"

struct msm_gem_submit;
struct msm_gpu_perfcntr;

#define MSM_GPU_DEFAULT_IONAME  "kgsl_3d0_reg_memory"
#define MSM_GPU_DEFAULT_IRQNAME "kgsl_3d0_irq"

struct msm_gpu_config {
	const char *ioname;
	const char *irqname;
	int nr_rings;
	uint64_t va_start;
	uint64_t va_end;
	uint64_t secure_va_start;
	uint64_t secure_va_end;
};

/* So far, with hardware that I've seen to date, we can have:
 *  + zero, one, or two z180 2d cores
 *  + a3xx or a2xx 3d core, which share a common CP (the firmware
 *    for the CP seems to implement some different PM4 packet types
 *    but the basics of cmdstream submission are the same)
 *
 * Which means that the eventual complete "class" hierarchy, once
 * support for all past and present hw is in place, becomes:
 *  + msm_gpu
 *    + adreno_gpu
 *      + a3xx_gpu
 *      + a2xx_gpu
 *    + z180_gpu
 */
struct msm_gpu_funcs {
	int (*get_param)(struct msm_gpu *gpu, uint32_t param, uint64_t *value);
	int (*hw_init)(struct msm_gpu *gpu);
	int (*pm_suspend)(struct msm_gpu *gpu);
	int (*pm_resume)(struct msm_gpu *gpu);
	void (*submit)(struct msm_gpu *gpu, struct msm_gem_submit *submit);
	void (*flush)(struct msm_gpu *gpu, struct msm_ringbuffer *ring);
	irqreturn_t (*irq)(struct msm_gpu *irq);
	uint32_t (*submitted_fence)(struct msm_gpu *gpu,
			struct msm_ringbuffer *ring);
	struct msm_ringbuffer *(*active_ring)(struct msm_gpu *gpu);
	void (*recover)(struct msm_gpu *gpu);
	void (*destroy)(struct msm_gpu *gpu);
#ifdef CONFIG_DEBUG_FS
	/* show GPU status in debugfs: */
	void (*show)(struct msm_gpu *gpu, struct seq_file *m);
#endif
	int (*snapshot)(struct msm_gpu *gpu, struct msm_snapshot *snapshot);
	int (*get_counter)(struct msm_gpu *gpu, u32 groupid, u32 countable,
		u32 *lo, u32 *hi);
	void (*put_counter)(struct msm_gpu *gpu, u32 groupid, int counterid);
	u64 (*read_counter)(struct msm_gpu *gpu, u32 groupid, int counterid);
	u64 (*gpu_busy)(struct msm_gpu *gpu);
};

struct msm_gpu {
	const char *name;
	struct drm_device *dev;
	struct platform_device *pdev;
	const struct msm_gpu_funcs *funcs;

	/* performance counters (hw & sw): */
	spinlock_t perf_lock;
	bool perfcntr_active;
	struct {
		bool active;
		ktime_t time;
	} last_sample;
	uint32_t totaltime, activetime;    /* sw counters */
	uint32_t last_cntrs[5];            /* hw counters */
	const struct msm_gpu_perfcntr *perfcntrs;
	uint32_t num_perfcntrs;

	struct msm_ringbuffer *rb[MSM_GPU_MAX_RINGS];
	int nr_rings;

	/* list of GEM active objects: */
	struct list_head active_list;

	/* does gpu need hw_init? */
	bool needs_hw_init;

	/* worker for handling active-list retiring: */
	struct work_struct retire_work;

	void __iomem *mmio;
	int irq;

	struct msm_gem_address_space *aspace;
	struct msm_gem_address_space *secure_aspace;

	/* Power Control: */
	struct regulator *gpu_reg, *gpu_cx;
	struct clk **grp_clks;
	struct clk *ebi1_clk, *core_clk, *rbbmtimer_clk;
	int nr_clocks;

	uint32_t gpufreq[10];
	uint32_t busfreq[10];
	uint32_t nr_pwrlevels;

	struct pm_qos_request pm_qos_req_dma;

	struct drm_gem_object *memptrs_bo;

#ifdef DOWNSTREAM_CONFIG_MSM_BUS_SCALING
	struct msm_bus_scale_pdata *bus_scale_table;
	uint32_t bsc;
#endif

	/* Hang and Inactivity Detection:
	 */
#define DRM_MSM_INACTIVE_PERIOD   66 /* in ms (roughly four frames) */

#define DRM_MSM_HANGCHECK_PERIOD 500 /* in ms */
#define DRM_MSM_HANGCHECK_JIFFIES msecs_to_jiffies(DRM_MSM_HANGCHECK_PERIOD)
	struct timer_list hangcheck_timer;
	struct work_struct recover_work;
	struct msm_snapshot *snapshot;

	struct {
		struct devfreq *devfreq;
		u64 busy_cycles;
		ktime_t time;
		struct thermal_cooling_device *cooling_dev;
	} devfreq;

	struct notifier_block nb;
};

struct msm_gpu_submitqueue {
	int id;
	u32 flags;
	u32 prio;
	int faults;
	struct list_head node;
	struct kref ref;
};

/* It turns out that all targets use the same ringbuffer size. */
#define MSM_GPU_RINGBUFFER_SZ SZ_32K
#define MSM_GPU_RINGBUFFER_BLKSIZE 32

#define MSM_GPU_RB_CNTL_DEFAULT \
		(AXXX_CP_RB_CNTL_BUFSZ(ilog2(MSM_GPU_RINGBUFFER_SZ / 8)) | \
		AXXX_CP_RB_CNTL_BLKSZ(ilog2(MSM_GPU_RINGBUFFER_BLKSIZE / 8)))

static inline struct msm_ringbuffer *__get_ring(struct msm_gpu *gpu, int index)
{
	return (index < ARRAY_SIZE(gpu->rb) ? gpu->rb[index] : NULL);
}

#define FOR_EACH_RING(gpu, ring, index) \
	for (index = 0, ring = (gpu)->rb[0]; \
		index < (gpu)->nr_rings && index < ARRAY_SIZE((gpu)->rb); \
		index++, ring = __get_ring(gpu, index))

static inline bool msm_gpu_active(struct msm_gpu *gpu)
{
	struct msm_ringbuffer *ring;
	int i;

	FOR_EACH_RING(gpu, ring, i) {
		if (gpu->funcs->submitted_fence(gpu, ring) >
			ring->memptrs->fence)
			return true;
	}

	return false;
}

/* Perf-Counters:
 * The select_reg and select_val are just there for the benefit of the child
 * class that actually enables the perf counter..  but msm_gpu base class
 * will handle sampling/displaying the counters.
 */

struct msm_gpu_perfcntr {
	uint32_t select_reg;
	uint32_t sample_reg;
	uint32_t select_val;
	const char *name;
};

static inline void gpu_write(struct msm_gpu *gpu, u32 reg, u32 data)
{
	msm_writel(data, gpu->mmio + (reg << 2));
}

static inline u32 gpu_read(struct msm_gpu *gpu, u32 reg)
{
	return msm_readl(gpu->mmio + (reg << 2));
}

static inline void gpu_rmw(struct msm_gpu *gpu, u32 reg, u32 mask, u32 or)
{
	uint32_t val = gpu_read(gpu, reg);

	val &= ~mask;
	gpu_write(gpu, reg, val | or);
}

static inline u64 gpu_read64(struct msm_gpu *gpu, u32 lo, u32 hi)
{
	u64 val;

	/*
	 * Why not a readq here? Two reasons: 1) many of the LO registers are
	 * not quad word aligned and 2) the GPU hardware designers have a bit
	 * of a history of putting registers where they fit, especially in
	 * spins. The longer a GPU family goes the higher the chance that
	 * we'll get burned.  We could do a series of validity checks if we
	 * wanted to, but really is a readq() that much better? Nah.
	 */

	/*
	 * For some lo/hi registers (like perfcounters), the hi value is latched
	 * when the lo is read, so make sure to read the lo first to trigger
	 * that
	 */
	val = (u64) msm_readl(gpu->mmio + (lo << 2));
	val |= ((u64) msm_readl(gpu->mmio + (hi << 2)) << 32);

	return val;
}

static inline void gpu_write64(struct msm_gpu *gpu, u32 lo, u32 hi, u64 val)
{
	/* Why not a writeq here? Read the screed above */
	msm_writel(lower_32_bits(val), gpu->mmio + (lo << 2));
	msm_writel(upper_32_bits(val), gpu->mmio + (hi << 2));
}

int msm_gpu_pm_suspend(struct msm_gpu *gpu);
int msm_gpu_pm_resume(struct msm_gpu *gpu);

int msm_gpu_hw_init(struct msm_gpu *gpu);

void msm_gpu_perfcntr_start(struct msm_gpu *gpu);
void msm_gpu_perfcntr_stop(struct msm_gpu *gpu);
int msm_gpu_perfcntr_sample(struct msm_gpu *gpu, uint32_t *activetime,
		uint32_t *totaltime, uint32_t ncntrs, uint32_t *cntrs);

void msm_gpu_retire(struct msm_gpu *gpu);
int msm_gpu_submit(struct msm_gpu *gpu, struct msm_gem_submit *submit);

int msm_gpu_init(struct drm_device *drm, struct platform_device *pdev,
		struct msm_gpu *gpu, const struct msm_gpu_funcs *funcs,
		const char *name, struct msm_gpu_config *config);

void msm_gpu_cleanup(struct msm_gpu *gpu);

struct msm_gpu *adreno_load_gpu(struct drm_device *dev);
void __init adreno_register(void);
void __exit adreno_unregister(void);

int msm_gpu_counter_get(struct msm_gpu *gpu, struct drm_msm_counter *data,
	struct msm_file_private *ctx);

int msm_gpu_counter_put(struct msm_gpu *gpu, struct drm_msm_counter *data,
	struct msm_file_private *ctx);

void msm_gpu_cleanup_counters(struct msm_gpu *gpu,
	struct msm_file_private *ctx);

u64 msm_gpu_counter_read(struct msm_gpu *gpu,
		struct drm_msm_counter_read *data);

static inline void msm_submitqueue_put(struct msm_gpu_submitqueue *queue)
{
	if (queue)
		kref_put(&queue->ref, msm_submitqueue_destroy);
}

#endif /* __MSM_GPU_H__ */
