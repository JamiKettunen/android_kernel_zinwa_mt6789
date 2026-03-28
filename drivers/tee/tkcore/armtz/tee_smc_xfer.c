/*
 * Copyright (c) 2015-2018 TrustKernel Incorporated
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)
#include <linux/init.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0)
#include <linux/notifier.h>
#endif


#define TKCORE_BL

#ifdef TKCORE_BL
#include <linux/kthread.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <asm/topology.h>
#endif

#include <linux/tee_clkmgr.h>
#include <linux/tee_tkcore.h>

#include <linux/tee_kernel_lowlevel_api.h>
#include <arm_common/teesmc.h>

#include "tee_tz_priv.h"
#include "smc_abi_helper.h"

#ifdef TKCORE_BL

static int nr_cpus __read_mostly;

/*
 * if a smc_task waits over 100ms
 * for one available core, we give up
 * and choose any core that is available
 * at this moment of time
 */
static const s64 tee_task_timeout_us = 100000LL;

struct tee_task {
	struct tee_tz *ptee;
	struct invoke_tee_response *param;
	struct invoke_tee_response *last_param;

	struct work_struct work;
};

#endif

struct tee_task_ctl {
	/* guarantee the mutual-exlusiveness of smc */
	struct mutex g_lock;

	/* cmds that wait for
	 * available TEE thread slots
	 */
	atomic_t nr_waiting_cmds;
	struct completion smc_comp;

#ifdef TKCORE_BL
	/*
	 * records information for the big cpus that are locked
	 * in the online state due to running smc commands
	 */
	int *tasks;
	spinlock_t task_lock;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0)
	struct notifier_block cpu_notifier;
#else
	enum cpuhp_state tkcore_cpuhp_state;
#endif
	struct workqueue_struct *wq;
#endif

	/* statistics information */
	s64 max_smc_time;
	s64 max_task_time;
};

static struct tee_task_ctl tee_task_ctl;

static inline void trace_tee_smc(struct tee_task_ctl *ctl, int rv,
				 s64 time_start, s64 time_end)
{
	s64 duration = time_end - time_start;

	if (duration > 1000000LL) {
		pr_warn("WARNING SMC[0x%x] %sDURATION %lld us\n", rv,
			rv == TEESMC_RPC_FUNC_IRQ ? "IRQ " : "", duration);
	}

	/* we needn't handle concurrency here. */
	if (duration > ctl->max_smc_time)
		ctl->max_smc_time = duration;
}

static inline void trace_tee_smc_done(struct tee_task_ctl *ctl,
				s64 time_start,
				s64 time_end)
{
	s64 duration = time_end - time_start;

	if (duration > ctl->max_task_time)
		ctl->max_task_time = duration;
}

static inline unsigned long rpc_ret_by_cmd_id(unsigned long cmd_id)
{
	return TEESMC_IS_FAST_CALL(cmd_id) ?
		TEE_SMC(FASTCALL_RETURN_FROM_RPC) : TEE_SMC(CALL_RETURN_FROM_RPC);
}

static void backup_call(struct invoke_tee_response *call,
						struct invoke_tee_response *last_call)
{
	last_call->a0 = call->a0;
	last_call->a1 = call->a1;
}

static void do_smc(struct tee_tz *ptee, struct tee_task_ctl *ctl,
				struct invoke_tee_response *call,
				struct invoke_tee_response *last_call)
{
	int r;
	unsigned long a0 = rpc_ret_by_cmd_id(call->a0);
	s64 task_start, task_end, call_start, call_end;

	mutex_lock(&ctl->g_lock);

	task_start = ktime_to_us(ktime_get());

	for (;;) {
		/* backup a0 and a1 in case TEESMC_RETURN_EAFFINITY */
		backup_call(call, last_call);

		call_start = ktime_to_us(ktime_get());
		r = ptee->invoke_tee(ptee, call->a0,
			call->a1, call->a2, call->a3, call->a4, call);
		call_end = ktime_to_us(ktime_get());

		if (r != 0) {
			pr_warn("Unexpected invoke tee failed with %d\n", r);
			call->a0 = r;
			goto out;
		}

		trace_tee_smc(ctl, TEESMC_RETURN_GET_RPC_FUNC(call->a0),
			call_start, call_end);

		if (!TEESMC_RETURN_IS_RPC(call->a0))
			goto out;

		if (TEESMC_RETURN_GET_RPC_FUNC(call->a0) != TEESMC_RPC_FUNC_IRQ)
			goto out;

		call->a0 = a0;
	}

	task_end = ktime_to_us(ktime_get());
	trace_tee_smc_done(ctl, task_start, task_end);

out:
	mutex_unlock(&ctl->g_lock);
}

#ifdef TKCORE_BL

/* callers shall hold cpu->task_lock */
static inline void lock_cpu(struct tee_task_ctl *ctl, int cpu)
{
	++ctl->tasks[cpu];
	smp_mb();
}

/* callers shall hold cpu->task_lock */
static inline void unlock_cpu(struct tee_task_ctl *ctl, int cpu)
{
	--ctl->tasks[cpu];
	WARN_ON(ctl->tasks[cpu] < 0);
	smp_mb();
}

/*
 * can only be called in cpu hp callback or w/ ctl->task_lock
 *
 * It's OK to call cpu_locked from cpuhp callback w/o
 * ctl->task_lock, because we already grab cpus_read_lock()
 * before lock_cpu()
 */
static inline bool cpu_locked(struct tee_task_ctl *ctl, int cpu)
{
	bool locked = !!ctl->tasks[cpu];
	smp_mb();
	return locked;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0)
static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)
__cpuinit
#endif
tee_cpu_callback(struct notifier_block *self,
		unsigned long action, void *hcpu)
{
	int cpu = (int) ((unsigned long) hcpu);

	struct tee_task_ctl *ctl = &tee_task_ctl;

	if (unlikely(cpu >= nr_cpus)) {
		pr_err("Bad cpu: %d\n", cpu);
		return NOTIFY_BAD;
	}

	switch (action) {
		case CPU_DOWN_PREPARE:
		/* fall-through */
		case CPU_DOWN_PREPARE_FROZEN:
			return cpu_locked(ctl, cpu) ? NOTIFY_BAD : NOTIFY_OK;
		default:
			break;
	}

	return NOTIFY_OK;
}
#else
static int tee_cpu_prepare_down(unsigned int cpu)
{
	struct tee_task_ctl *ctl = &tee_task_ctl;

	if (unlikely(cpu >= nr_cpus)) {
		pr_err("Bad cpu: %d\n", cpu);
		return -1;
	}

	return cpu_locked(ctl, cpu) ? -1 : 0;
}
#endif

extern void handle_rpc(struct tee_tz *ptee, struct invoke_tee_response *resp);

static void do_run_tee_task(struct work_struct *w)
{
	struct tee_task *task = container_of(w, struct tee_task, work);
	u32 rpc_ret_fid = rpc_ret_by_cmd_id(task->param->a0);

	for (;;) {
		do_smc(task->ptee, &tee_task_ctl,
			task->param, task->last_param);
		if (!TEESMC_RETURN_IS_RPC(task->param->a0) ||
				(task->param->a0 == TEESMC_RETURN_ETHREAD_LIMIT) ||
				(task->param->a0 == TEESMC_RETURN_TKCORE_RPC_BIND_CPU) ||
				(task->param->a0 == TEESMC_RETURN_EAFFINITY)) {
			break;
		}

		handle_rpc(task->ptee, task->param);
		task->param->a0 = rpc_ret_fid;
	}
}

static int find_matched_cpu(struct tee_task_ctl *ctl,
				const struct tee_task_cpu_affinity *aff)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (aff->ops->match_cpu(aff->priv, cpu))
			return cpu;
	}

	return -1;
}

static void init_tee_task(struct tee_task *task,
				struct tee_tz *ptee,
				struct invoke_tee_response *param,
				struct invoke_tee_response *last_param)
{
	// initialize call to tee
	task->ptee = ptee;
	task->param = param;
	task->last_param = last_param;

	INIT_WORK(&task->work, do_run_tee_task);
}

static int post_tee_task(struct tee_tz *ptee,
			struct tee_task_ctl *ctl,
			const struct tee_task_cpu_affinity *aff,
			struct invoke_tee_response *call,
			struct invoke_tee_response *last_call)
{
	int r = 0, picked_cpu = -1;

	s64 tee_task_start, tee_task_current;

	struct tee_task task;
	init_tee_task(&task, ptee, call, last_call);

	tee_task_start = ktime_to_us(ktime_get());

	while (picked_cpu < 0) {
		int candidate_cpu;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
		get_online_cpus();
#else
		cpus_read_lock();
#endif
		picked_cpu = find_matched_cpu(ctl, aff);

		spin_lock(&ctl->task_lock);
		if (picked_cpu >= 0)
			lock_cpu(ctl, picked_cpu);
		spin_unlock(&ctl->task_lock);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
		put_online_cpus();
#else
		cpus_read_unlock();
#endif

		if (picked_cpu >= 0) {
			if (WARN_ON(!queue_work_on(picked_cpu, ctl->wq, &task.work))) {
				r = -1;
				goto out;
			}
			r = 0;
		} else {
			tee_task_current = ktime_to_us(ktime_get());
			if (tee_task_current - tee_task_start >= tee_task_timeout_us) {
				pr_warn("submit tee task to target cpu timeout\n");
				break;
			}

			candidate_cpu = aff->ops->get_candidate_cpu(aff->priv);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
			r = cpu_up(candidate_cpu);
#else
			r = add_cpu(candidate_cpu);
#endif
			if (r != 0) {
				pr_warn("cpu_on(%d) failed with ret: %d\n",
					candidate_cpu, r);
			}
		}
	}

	if (picked_cpu < 0)
		return -1;

	flush_work(&task.work);

out:
	spin_lock(&ctl->task_lock);
	unlock_cpu(ctl, picked_cpu);
	spin_unlock(&ctl->task_lock);
	return r;
}

static bool tee_bind_cpu_request_need_bind_cpu(void *priv)
{
	(void) priv;
	return true;
}

static bool tee_bind_cpu_request_match_cpu(void *priv, int cpu)
{
	uint32_t cpumask = ((uint32_t) (unsigned long) priv);

	return !!(cpumask & (1U << cpu));
}

static int tee_bind_cpu_request_get_candidate_cpu(void *priv)
{
	uint32_t cpumask = ((uint32_t) (unsigned long) priv);

	if (cpumask == 0)
		return -1;

	return __builtin_ffs(cpumask) - 1;
}

const struct tee_task_cpu_affinity_operations tee_bind_cpu_request_operations = {
	.need_bind_cpu = tee_bind_cpu_request_need_bind_cpu,
	.match_cpu = tee_bind_cpu_request_match_cpu,
	.get_candidate_cpu = tee_bind_cpu_request_get_candidate_cpu,
};

static void update_tee_task_affinity(struct tee_task_cpu_affinity *aff, uint32_t cpu_mask)
{
	aff->ops = &tee_bind_cpu_request_operations;
	aff->priv = (void *) ((unsigned long) cpu_mask);
}

static void restore_call(struct invoke_tee_response *call,
						struct invoke_tee_response *last_call)
{
	call->a0 = last_call->a0;
	call->a1 = last_call->a1;
}

static void submit_tee_task(struct tee_tz *ptee, struct tee_task_ctl *ctl,
			struct tee_task_cpu_affinity *aff,
			struct invoke_tee_response *call)
{
	int r;
	struct invoke_tee_response last_call;
	unsigned long cmd_id = call->a0;

	for (;;) {
		r = aff->ops->need_bind_cpu(aff->priv) ?
			post_tee_task(ptee, ctl, aff, call, &last_call) : -1;
		if (r < 0)
			do_smc(ptee, ctl, call, &last_call);

		if (call->a0 == TEESMC_RETURN_TKCORE_RPC_BIND_CPU) {
			update_tee_task_affinity(aff, (uint32_t) call->a1);
			call->a0 = rpc_ret_by_cmd_id(cmd_id);
			call->a1 = 0;
		} else if (call->a0 == TEESMC_RETURN_EAFFINITY) {
			update_tee_task_affinity(aff, (uint32_t) call->a1);
			restore_call(call, &last_call);
		} else {
			break;
		}
	}
}

static int platform_bl_init(struct tee_task_ctl *ctl)
{
	int r;

	nr_cpus = num_possible_cpus();
	if (nr_cpus > NR_CPUS) {
		pr_err("nr_cpus %d exceeds NR_CPUS %d\n", nr_cpus, NR_CPUS);
		return -1;
	}

	ctl->tasks = (int *) kzalloc(nr_cpus * sizeof(int), GFP_KERNEL);
	if (ctl->tasks == NULL) {
		return -ENOMEM;
	}

	spin_lock_init(&ctl->task_lock);

	ctl->wq = alloc_workqueue("tee_work", WQ_CPU_INTENSIVE, 0);
	if (ctl->wq == NULL) {
		pr_err("bad alloc tee_work wq\n");
		r = -ENOMEM;
		goto err;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0)
	memset(&ctl->cpu_notifier, 0, sizeof(ctl->cpu_notifier));

	ctl->cpu_notifier.notifier_call = tee_cpu_callback;
	register_cpu_notifier(&ctl->cpu_notifier);
#else
	r = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
		"tee/tkcore_tzdrv:cpu_listener",
		NULL, tee_cpu_prepare_down);
	if (r < 0)
		goto err;

	ctl->tkcore_cpuhp_state = r;
#endif
	return 0;

err:
	if (ctl->tasks) {
		kfree(ctl->tasks);
		ctl->tasks = NULL;
	}

	if (ctl->wq) {
		destroy_workqueue(ctl->wq);
		ctl->wq = NULL;
	}

	return r;
}

static void platform_bl_deinit(struct tee_task_ctl *ctl)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0)
	unregister_cpu_notifier(&ctl->cpu_notifier);
#else
	cpuhp_remove_state(ctl->tkcore_cpuhp_state);
#endif

	destroy_workqueue(ctl->wq);
	ctl->wq = NULL;

	kfree(ctl->tasks);
	ctl->tasks = NULL;
}

#else

static inline void submit_tee_task(struct tee_tz *ptee,
			struct tee_task_ctl *ctl,
			const struct tee_task_cpu_affinity *aff,
			struct invoke_tee_response *p)
{
	struct invoke_tee_response last_call;

	(void) aff;
	do_smc(ptee, ctl, p, &last_call);
}

static int platform_bl_init(struct tee_task_ctl *ctl) { return 0; }

static void platform_bl_deinit(struct tee_task_ctl *ctl) { }

#endif

static void waiters_enqueue(struct tee_task_ctl *ctl)
{
	/*TODO handle too long time of waiting */
	atomic_inc(&ctl->nr_waiting_cmds);
	wait_for_completion(&ctl->smc_comp);
}

static void waiters_dequeue(struct tee_task_ctl *ctl)
{
	if (atomic_dec_if_positive(&ctl->nr_waiting_cmds) >= 0)
		complete(&ctl->smc_comp);
}

void run_tee_task(struct tee_tz *ptee,
				struct invoke_tee_response *tee_req_resp,
				struct tee_task_cpu_affinity *aff)
{
	/* NOTE!!! we remove the e_lock_teez(ptee) here !!!! */
	unsigned long orig_a0 = (unsigned long) tee_req_resp->a0;

	for (;;) {
		submit_tee_task(ptee, &tee_task_ctl, aff, tee_req_resp);
		if (tee_req_resp->a0 == TEESMC_RETURN_ETHREAD_LIMIT) {
			waiters_enqueue(&tee_task_ctl);
			tee_req_resp->a0 = orig_a0;
		} else {
			if (!TEESMC_RETURN_IS_RPC(tee_req_resp->a0))
				waiters_dequeue(&tee_task_ctl);
			break;
		}
	}
}

int tee_init_task(void)
{
	struct tee_task_ctl *ctl = &tee_task_ctl;

	mutex_init(&ctl->g_lock);

	atomic_set(&ctl->nr_waiting_cmds, 0);
	init_completion(&ctl->smc_comp);

	ctl->max_smc_time = 0LL;
	ctl->max_task_time = 0LL;

	return platform_bl_init(ctl);
}

void tee_exit_task(void)
{
	struct tee_task_ctl *ctl = &tee_task_ctl;

	platform_bl_deinit(ctl);
}
