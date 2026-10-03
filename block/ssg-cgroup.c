// SPDX-License-Identifier: GPL-2.0
/*
 *  Control Group of SamSung Generic I/O scheduler
 *
 *  Copyright (C) 2021 Changheun Lee <nanich.lee@samsung.com>
 */

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/cgroup.h>
#include <linux/kthread.h>

#include "blk-cgroup.h"
#include "blk-mq.h"
#include "ssg.h"



static struct blkcg_policy ssg_blkcg_policy;



#define CPD_TO_SSG_BLKCG(_cpd) \
	ssg_container_of_safe((_cpd), struct ssg_blkcg, cpd)
#define BLKCG_TO_SSG_BLKCG(_blkcg) \
	CPD_TO_SSG_BLKCG(blkcg_to_cpd((_blkcg), &ssg_blkcg_policy))

#define PD_TO_SSG_BLKG(_pd) \
	ssg_container_of_safe((_pd), struct ssg_blkg, pd)
#define BLKG_TO_SSG_BLKG(_blkg) \
	PD_TO_SSG_BLKG(blkg_to_pd((_blkg), &ssg_blkcg_policy))

#define CSS_TO_SSG_BLKCG(css) BLKCG_TO_SSG_BLKCG(css_to_blkcg(css))



static struct blkcg_policy_data *ssg_blkcg_cpd_alloc(gfp_t gfp)
{
	struct ssg_blkcg *ssg_blkcg;

	ssg_blkcg = kzalloc(sizeof(struct ssg_blkcg), gfp);
	if (ZERO_OR_NULL_PTR(ssg_blkcg))
		return NULL;

	/* no cpd_init_fn in this kernel, so set the default here */
	ssg_blkcg->max_available_ratio = 25;

	return &ssg_blkcg->cpd;
}

static void ssg_blkcg_cpd_free(struct blkcg_policy_data *cpd)
{
	struct ssg_blkcg *ssg_blkcg = CPD_TO_SSG_BLKCG(cpd);

	if (IS_ERR_OR_NULL(ssg_blkcg))
		return;

	kfree(ssg_blkcg);
}

/*
 * Scheduler tags of the first hardware queue. All tag maps of a queue have the
 * same depth, so it is representative for the whole queue. May be NULL while
 * no scheduler tags are allocated yet.
 */
static bool ssg_blkcg_sched_depth(struct request_queue *q,
		unsigned int *depth, unsigned int *map_nr)
{
	struct blk_mq_hw_ctx *hctx;
	struct blk_mq_tags *tags;
	bool ret = false;

	/* sched_tags can be replaced with the queue frozen, see ssg_for_each_sched_tags() */
	if (!percpu_ref_tryget(&q->q_usage_counter))
		return false;

	hctx = xa_load(&q->hctx_table, 0);
	tags = hctx ? hctx->sched_tags : NULL;
	if (tags) {
		*depth = tags->bitmap_tags.sb.depth;
		*map_nr = tags->bitmap_tags.sb.map_nr;
		ret = true;
	}

	blk_queue_exit(q);
	return ret;
}

/* blkcg of the current context, same rules as the block layer uses for bios */
static struct blkcg *ssg_current_blkcg(void)
{
	struct cgroup_subsys_state *css = kthread_blkcg();

	if (css)
		return css_to_blkcg(css);

	return css_to_blkcg(task_css(current, io_cgrp_id));
}

/*
 * Recompute the limits of a blkg from the cached sched tag geometry. Without a
 * known geometry the limits stay at 0, which means "no limit".
 */
static void ssg_blkcg_set_shallow_depth(struct ssg_blkcg *ssg_blkcg,
		struct ssg_blkg *ssg_blkg)
{
	unsigned int depth = ssg_blkg->tags_depth;
	unsigned int map_nr = ssg_blkg->tags_map_nr;

	if (!depth || !map_nr) {
		ssg_blkg->max_available_rqs = 0;
		ssg_blkg->shallow_depth = 0;
		return;
	}

	ssg_blkg->max_available_rqs =
		depth * ssg_blkcg->max_available_ratio / 100U;
	ssg_blkg->shallow_depth =
		max_t(unsigned int, 1, ssg_blkg->max_available_rqs / map_nr);
}

static struct blkg_policy_data *ssg_blkcg_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	struct ssg_blkg *ssg_blkg;

	ssg_blkg = kzalloc_node(sizeof(struct ssg_blkg), gfp, disk->node_id);
	if (ZERO_OR_NULL_PTR(ssg_blkg))
		return NULL;

	return &ssg_blkg->pd;
}

static void ssg_blkcg_pd_init(struct blkg_policy_data *pd)
{
	struct ssg_blkg *ssg_blkg;
	struct ssg_blkcg *ssg_blkcg;

	ssg_blkg = PD_TO_SSG_BLKG(pd);
	if (IS_ERR_OR_NULL(ssg_blkg))
		return;

	ssg_blkcg = BLKCG_TO_SSG_BLKCG(pd->blkg->blkcg);
	if (IS_ERR_OR_NULL(ssg_blkcg))
		return;

	atomic_set(&ssg_blkg->current_rqs, 0);
	/*
	 * While the queue is frozen (elevator switch, nr_requests update) the
	 * geometry is not readable here. Then ssg_blkcg_depth_updated() fills it
	 * in before the queue is unfrozen.
	 */
	if (ssg_blkcg_sched_depth(pd->blkg->q, &ssg_blkg->tags_depth,
				  &ssg_blkg->tags_map_nr))
		ssg_blkcg_set_shallow_depth(ssg_blkcg, ssg_blkg);
}

static void ssg_blkcg_pd_free(struct blkg_policy_data *pd)
{
	struct ssg_blkg *ssg_blkg = PD_TO_SSG_BLKG(pd);

	if (IS_ERR_OR_NULL(ssg_blkg))
		return;

	kfree(ssg_blkg);
}

unsigned int ssg_blkcg_shallow_depth(struct request_queue *q)
{
	struct blkcg_gq *blkg;
	struct ssg_blkg *ssg_blkg;

	rcu_read_lock();
	blkg = blkg_lookup(ssg_current_blkcg(), q);
	ssg_blkg = BLKG_TO_SSG_BLKG(blkg);
	rcu_read_unlock();

	if (IS_ERR_OR_NULL(ssg_blkg))
		return 0;

	if (atomic_read(&ssg_blkg->current_rqs) < ssg_blkg->max_available_rqs)
		return 0;

	return ssg_blkg->shallow_depth;
}

/*
 * Look up the blkg of the current context and pin it, so that it stays valid
 * until ssg_blkcg_dec_rq() is called for the request. Returns NULL if there is
 * none.
 */
struct blkcg_gq *ssg_blkcg_get_blkg(struct request_queue *q)
{
	struct blkcg_gq *blkg;

	rcu_read_lock();
	blkg = blkg_lookup(ssg_current_blkcg(), q);
	if (!blkg_tryget(blkg))
		blkg = NULL;
	rcu_read_unlock();

	return blkg;
}

void ssg_blkcg_depth_updated(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct cgroup_subsys_state *pos_css;
	struct blkcg_gq *blkg;
	struct ssg_blkg *ssg_blkg;
	struct ssg_blkcg *ssg_blkcg;

	if (!q->root_blkg)
		return;

	rcu_read_lock();
	blkg_for_each_descendant_pre(blkg, pos_css, q->root_blkg) {
		ssg_blkg = BLKG_TO_SSG_BLKG(blkg);
		if (IS_ERR_OR_NULL(ssg_blkg))
			continue;

		ssg_blkcg = BLKCG_TO_SSG_BLKCG(blkg->blkcg);
		if (IS_ERR_OR_NULL(ssg_blkcg))
			continue;

		atomic_set(&ssg_blkg->current_rqs, 0);
		/* called with the queue frozen, sched_tags are stable */
		ssg_blkg->tags_depth = hctx->sched_tags->bitmap_tags.sb.depth;
		ssg_blkg->tags_map_nr = hctx->sched_tags->bitmap_tags.sb.map_nr;
		ssg_blkcg_set_shallow_depth(ssg_blkcg, ssg_blkg);
	}
	rcu_read_unlock();
}

void ssg_blkcg_inc_rq(struct blkcg_gq *blkg)
{
	struct ssg_blkg *ssg_blkg = BLKG_TO_SSG_BLKG(blkg);

	if (IS_ERR_OR_NULL(ssg_blkg))
		return;

	atomic_inc(&ssg_blkg->current_rqs);
}

void ssg_blkcg_dec_rq(struct blkcg_gq *blkg)
{
	struct ssg_blkg *ssg_blkg;

	if (!blkg)
		return;

	ssg_blkg = BLKG_TO_SSG_BLKG(blkg);
	if (!IS_ERR_OR_NULL(ssg_blkg))
		atomic_dec(&ssg_blkg->current_rqs);

	/* drop the reference taken in ssg_blkcg_get_blkg() */
	blkg_put(blkg);
}

static int ssg_blkcg_show_max_available_ratio(struct seq_file *sf, void *v)
{
	struct ssg_blkcg *ssg_blkcg = CSS_TO_SSG_BLKCG(seq_css(sf));

	if (IS_ERR_OR_NULL(ssg_blkcg))
		return -EINVAL;

	seq_printf(sf, "%d\n", ssg_blkcg->max_available_ratio);

	return 0;
}

static int ssg_blkcg_set_max_available_ratio(struct cgroup_subsys_state *css,
		struct cftype *cftype, u64 ratio)
{
	struct blkcg *blkcg = css_to_blkcg(css);
	struct ssg_blkcg *ssg_blkcg = CSS_TO_SSG_BLKCG(css);
	struct blkcg_gq *blkg;
	struct ssg_blkg *ssg_blkg;

	if (IS_ERR_OR_NULL(ssg_blkcg))
		return -EINVAL;

	if (ratio > 100)
		return -EINVAL;

	spin_lock_irq(&blkcg->lock);
	ssg_blkcg->max_available_ratio = ratio;
	hlist_for_each_entry(blkg, &blkcg->blkg_list, blkcg_node) {

		ssg_blkg = BLKG_TO_SSG_BLKG(blkg);
		if (IS_ERR_OR_NULL(ssg_blkg))
			continue;

		ssg_blkcg_set_shallow_depth(ssg_blkcg, ssg_blkg);
	}
	spin_unlock_irq(&blkcg->lock);

	return 0;
}

struct cftype ssg_blkg_files[] = {
	{
		.name = "ssg.max_available_ratio",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = ssg_blkcg_show_max_available_ratio,
		.write_u64 = ssg_blkcg_set_max_available_ratio,
	},

	{} /* terminate */
};

static struct blkcg_policy ssg_blkcg_policy = {
	.legacy_cftypes = ssg_blkg_files,

	.cpd_alloc_fn = ssg_blkcg_cpd_alloc,
	.cpd_free_fn = ssg_blkcg_cpd_free,

	.pd_alloc_fn = ssg_blkcg_pd_alloc,
	.pd_init_fn = ssg_blkcg_pd_init,
	.pd_free_fn = ssg_blkcg_pd_free,
};

int ssg_blkcg_activate(struct request_queue *q)
{
	if (!q->disk)
		return 0;

	return blkcg_activate_policy(q->disk, &ssg_blkcg_policy);
}

void ssg_blkcg_deactivate(struct request_queue *q)
{
	if (q->disk)
		blkcg_deactivate_policy(q->disk, &ssg_blkcg_policy);
}

int ssg_blkcg_init(void)
{
	return blkcg_policy_register(&ssg_blkcg_policy);
}

void ssg_blkcg_exit(void)
{
	blkcg_policy_unregister(&ssg_blkcg_policy);
}
