/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Using the avg_vruntime, do the right thing and preserve lag across
 * sleep+wake cycles. EEVDF placement strategy #1, #2 if disabled.
 */
SCHED_FEAT(PLACE_LAG, true)
SCHED_FEAT(PLACE_DEADLINE_INITIAL, true)
/*
 * Preserve relative virtual deadline on 'migration'.
 */
SCHED_FEAT(PLACE_REL_DEADLINE, true)
SCHED_FEAT(RUN_TO_PARITY, false)

/*
 * Prefer to schedule the task we woke last (assuming it failed
 * wakeup-preemption), since its likely going to consume data we
 * touched, increases cache locality.
 */
SCHED_FEAT(NEXT_BUDDY, true)

/*
 * Consider buddies to be cache hot, decreases the likeliness of a
 * cache buddy being migrated away, increases cache locality.
 */
SCHED_FEAT(CACHE_HOT_BUDDY, true)

/*
 * Allow wakeup-time preemption of the current task:
 */
SCHED_FEAT(WAKEUP_PREEMPTION, true)

SCHED_FEAT(HRTICK, false)
SCHED_FEAT(HRTICK_DL, false)
SCHED_FEAT(DOUBLE_TICK, false)

/*
 * Decrement CPU capacity based on time not spent running tasks
 */
SCHED_FEAT(NONTASK_CAPACITY, true)

#ifdef CONFIG_PREEMPT_RT
SCHED_FEAT(TTWU_QUEUE, false)
#else

/*
 * Queue remote wakeups on the target CPU and process them
 * using the scheduler IPI. Reduces rq->lock contention/bounces.
 */
SCHED_FEAT(TTWU_QUEUE, true)
#endif

/*
 * When doing wakeups, attempt to limit superfluous scans of the LLC domain.
 */
SCHED_FEAT(SIS_PROP, false)
SCHED_FEAT(SIS_UTIL, true)

/*
 * Issue a WARN when we do multiple update_rq_clock() calls
 * in a single rq->lock section. Default disabled because the
 * annotations are not complete.
 */
SCHED_FEAT(WARN_DOUBLE_CLOCK, false)

#ifdef HAVE_RT_PUSH_IPI
/*
 * In order to avoid a thundering herd attack of CPUs that are
 * lowering their priorities at the same time, and there being
 * a single CPU that has an RT task that can migrate and is waiting
 * to run, where the other CPUs will try to take that CPUs
 * rq lock and possibly create a large contention, sending an
 * IPI to that CPU and let that CPU push the RT task to where
 * it should go may be a better scenario.
 */
SCHED_FEAT(RT_PUSH_IPI, true)
#endif

SCHED_FEAT(RT_RUNTIME_SHARE, false)
SCHED_FEAT(LB_MIN, false)
SCHED_FEAT(ATTACH_AGE_LOAD, true)

/*
 * Advance detach_tasks()' scan window by rotating the examined block to the
 * head of cfs_tasks, instead of moving each rejected task there one by one.
 *
 * Both schemes give the same coverage -- a later balance pass resumes where
 * the previous one stopped -- but the per-reject move costs one list
 * operation per rejected task under rq_lock, where a block rotation costs
 * one per scan.  It also keeps the list order independent of migration
 * outcome, which the per-reject move does not.
 *
 * Turn off to get the upstream per-reject list_move() behaviour.
 */
SCHED_FEAT(LB_ROTATE_BLOCK, true)

/*
 * Compare a migration candidate's cost against the remaining imbalance budget
 * directly, instead of relaxing the comparison by sd->nr_balance_failed.
 *
 * The upstream relaxation halves the candidate's apparent cost once per
 * recorded balance failure, so a task larger than the budget is eventually
 * let through.  nr_balance_failed is reset on *any* successful migration at
 * that domain, so a steady supply of cheap-to-move tasks keeps resetting it
 * and the relaxation never reaches the expensive ones: they are considered
 * only once nothing cheaper is left.
 *
 * That relaxation is also the only way an over-budget task ever moves under
 * migrate_load/migrate_util, since imbalanced_active_balance() escalates for
 * migrate_task only.  So dropping it alone would leave a task larger than the
 * imbalance permanently unmovable; detach_tasks() replaces the guarantee with
 * an explicit one -- a scan that admits nothing concedes to the smallest
 * overshoot it saw.  That asks the candidates actually present instead of a
 * domain counter, so no other task's success can defer it, and it acts on the
 * first scan that admits nothing rather than once enough failures have been
 * recorded.
 *
 * Turn off to get the upstream shr_bound() relaxation.
 */
SCHED_FEAT(LB_STRICT_BUDGET, true)

SCHED_FEAT(WA_IDLE, true)
SCHED_FEAT(WA_WEIGHT, true)
SCHED_FEAT(WA_BIAS, true)

/*
 * UtilEstimation. Use estimated CPU utilization.
 */
SCHED_FEAT(UTIL_EST, true)

SCHED_FEAT(LATENCY_WARN, false)

SCHED_FEAT(HZ_BW, true)
