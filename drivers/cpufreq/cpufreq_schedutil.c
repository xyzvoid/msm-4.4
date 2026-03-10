// SPDX-License-Identifier: GPL-2.0
/*
 * CPUfreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * Backported for Android kernel 4.4.205 (msm-4.4) with EAS support.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <trace/events/power.h>
#include <linux/sched.h>
#include "cpufreq_schedutil.h"

#define DEFAULT_RATE_LIMIT_US (5 * USEC_PER_MSEC)

struct sugov_tunables {
	unsigned int        rate_limit_us;
	bool                iowait_boost_enable;
	struct gov_attr_set attr_set;
};

struct sugov_policy {
	struct cpufreq_policy  *policy;
	struct sugov_tunables  *tunables;
	struct list_head        tunables_hook;

	raw_spinlock_t update_lock;
	u64            last_freq_update_time;
	s64            freq_update_delay_ns;
	unsigned int   next_freq;
	unsigned int   cached_raw_freq;

	struct irq_work      irq_work;
	struct kthread_work  work;
	struct mutex         work_lock;
	struct kthread_worker worker;
	struct task_struct  *thread;
	bool work_in_progress;
	bool need_freq_update;
};

struct sugov_cpu {
	struct update_util_data update_util;
	struct sugov_policy    *sg_policy;
	unsigned int            cpu;

	bool         iowait_boost_pending;
	unsigned int iowait_boost;
	unsigned int iowait_boost_max;
	u64          last_update;

	unsigned long util;
	unsigned long max;
	unsigned int  flags;
};

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);
static struct cpufreq_governor schedutil_gov;
static DEFINE_MUTEX(global_tunables_lock);
static struct sugov_tunables *global_tunables;

static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;
	if (unlikely(sg_policy->need_freq_update)) {
		sg_policy->need_freq_update = false;
		sg_policy->next_freq = UINT_MAX;
		return true;
	}
	delta_ns = (s64)(time - sg_policy->last_freq_update_time);
	return delta_ns >= sg_policy->freq_update_delay_ns;
}

static bool sugov_update_next_freq(struct sugov_policy *sg_policy,
				   u64 time, unsigned int next_freq)
{
	if (sg_policy->next_freq == next_freq)
		return false;
	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;
	return true;
}

static unsigned int get_next_freq(struct sugov_policy *sg_policy,
				  unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = arch_scale_freq_invariant() ?
		policy->cpuinfo.max_freq : policy->cur;

	freq = (freq + (freq >> 2)) * util / max;
	if (freq == sg_policy->cached_raw_freq && sg_policy->next_freq != UINT_MAX)
		return sg_policy->next_freq;
	sg_policy->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static void sugov_iowait_boost(struct sugov_cpu *sg_cpu,
			       unsigned long *util, unsigned long *max)
{
	if (!sg_cpu->iowait_boost_pending)
		return;
	sg_cpu->iowait_boost_pending = false;
	if (sg_cpu->iowait_boost) {
		*util = max_t(unsigned long, *util, sg_cpu->iowait_boost);
		*max  = max_t(unsigned long, *max,  sg_cpu->iowait_boost_max);
	}
}

void sugov_deferred_update(struct sugov_policy *sg_policy,
			   u64 time, unsigned int next_freq)
{
	unsigned long irqflags;
	raw_spin_lock_irqsave(&sg_policy->update_lock, irqflags);
	if (!sg_policy->work_in_progress) {
		sg_policy->work_in_progress = true;
		irq_work_queue(&sg_policy->irq_work);
	}
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, irqflags);
}

static void sugov_update_single(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu =
		container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util, max;
	unsigned int next_f;
	bool busy;

	raw_spin_lock(&sg_policy->update_lock);
	busy = sugov_cpu_is_busy(sg_cpu);

	if (flags & SCHED_CPUFREQ_DL) {
		next_f = policy->cpuinfo.max_freq;
	} else {
		sugov_get_util(&util, &max, sg_cpu->cpu);
		sugov_iowait_boost(sg_cpu, &util, &max);
		sg_cpu->last_update = time;
		next_f = get_next_freq(sg_policy, util, max);
		if (busy && next_f < sg_policy->next_freq)
			next_f = sg_policy->next_freq;
	}
	if (sugov_should_update_freq(sg_policy, time) &&
	    sugov_update_next_freq(sg_policy, time, next_f)) {
		if (policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(policy, next_f);
		else
			sugov_deferred_update(sg_policy, time, next_f);
	}
	raw_spin_unlock(&sg_policy->update_lock);
}

static unsigned int sugov_next_freq_shared(struct sugov_cpu *sg_cpu,
					   unsigned long util,
					   unsigned long max,
					   unsigned int flags)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int max_f = policy->cpuinfo.max_freq;
	u64 last_freq_update_time = sg_policy->last_freq_update_time;
	unsigned int j;

	if (flags & SCHED_CPUFREQ_DL)
		return max_f;
	sugov_iowait_boost(sg_cpu, &util, &max);

	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg_cpu;
		unsigned long j_util, j_max;
		s64 delta_ns;

		if (j == sg_cpu->cpu)
			continue;
		j_sg_cpu = &per_cpu(sugov_cpu, j);
		if (j_sg_cpu->flags & SCHED_CPUFREQ_DL)
			return max_f;
		delta_ns = (s64)(last_freq_update_time - j_sg_cpu->last_update);
		if (delta_ns > TICK_NSEC)
			continue;
		j_util = j_sg_cpu->util;
		j_max  = j_sg_cpu->max;
		if (j_util * max > j_max * util) {
			util = j_util;
			max  = j_max;
		}
		sugov_iowait_boost(j_sg_cpu, &util, &max);
	}
	return get_next_freq(sg_policy, util, max);
}

static void sugov_update_shared(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu =
		container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max;
	unsigned int next_f;

	sugov_get_util(&util, &max, sg_cpu->cpu);
	raw_spin_lock(&sg_policy->update_lock);
	sg_cpu->util  = util;
	sg_cpu->max   = max;
	sg_cpu->flags = flags;
	sg_cpu->last_update = time;

	if (sugov_should_update_freq(sg_policy, time)) {
		next_f = sugov_next_freq_shared(sg_cpu, util, max, flags);
		if (sugov_update_next_freq(sg_policy, time, next_f)) {
			if (sg_policy->policy->fast_switch_enabled)
				cpufreq_driver_fast_switch(sg_policy->policy, next_f);
			else
				sugov_deferred_update(sg_policy, time, next_f);
		}
	}
	raw_spin_unlock(&sg_policy->update_lock);
}

static void sugov_work(struct kthread_work *work)
{
	struct sugov_policy *sg_policy =
		container_of(work, struct sugov_policy, work);
	unsigned int freq;
	unsigned long flags;

	mutex_lock(&sg_policy->work_lock);
	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	freq = sg_policy->next_freq;
	sg_policy->work_in_progress = false;
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	__cpufreq_driver_target(sg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);
}

static void sugov_irq_work(struct irq_work *irq_work)
{
	struct sugov_policy *sg_policy =
		container_of(irq_work, struct sugov_policy, irq_work);
	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

static ssize_t rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	return sprintf(buf, "%u\n", tunables->rate_limit_us);
}

static ssize_t rate_limit_us_store(struct gov_attr_set *attr_set,
				   const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;
	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;
	tunables->rate_limit_us = rate_limit_us;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook)
		sg_policy->freq_update_delay_ns = rate_limit_us * NSEC_PER_USEC;
	return count;
}

static struct governor_attr rate_limit_us_attr = __ATTR_RW(rate_limit_us);

static ssize_t iowait_boost_enable_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	return sprintf(buf, "%u\n", tunables->iowait_boost_enable);
}

static ssize_t iowait_boost_enable_store(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	unsigned int enable;
	if (kstrtouint(buf, 10, &enable))
		return -EINVAL;
	tunables->iowait_boost_enable = !!enable;
	return count;
}

static struct governor_attr iowait_boost_enable_attr =
		__ATTR_RW(iowait_boost_enable);

static struct attribute *sugov_attributes[] = {
	&rate_limit_us_attr.attr,
	&iowait_boost_enable_attr.attr,
	NULL
};

static struct kobj_type sugov_tunables_ktype = {
	.default_attrs = sugov_attributes,
	.sysfs_ops     = &governor_sysfs_ops,
};

static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;
	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static void sugov_policy_free(struct sugov_policy *sg_policy)
{
	kfree(sg_policy);
}

static int sugov_kthread_create(struct sugov_policy *sg_policy)
{
	struct task_struct *thread;
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	kthread_init_work(&sg_policy->work, sugov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov:%d", policy->cpu);
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}
	sg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&sg_policy->irq_work, sugov_irq_work);
	mutex_init(&sg_policy->work_lock);
	wake_up_process(thread);
	return 0;
}

static void sugov_kthread_stop(struct sugov_policy *sg_policy)
{
	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables;
	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			schedutil_gov.tunables = tunables;
	}
	return tunables;
}

static void sugov_tunables_free(struct sugov_tunables *tunables)
{
	if (!have_governor_per_policy())
		schedutil_gov.tunables = NULL;
	kfree(tunables);
}

static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;
	int ret = 0;

	if (policy->governor_data)
		return -EBUSY;
	cpufreq_enable_fast_switch(policy);

	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy) { ret = -ENOMEM; goto disable_fast_switch; }

	if (!policy->fast_switch_enabled) {
		ret = sugov_kthread_create(sg_policy);
		if (ret) goto free_sg_policy;
	}

	mutex_lock(&global_tunables_lock);
	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL; goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables   = global_tunables;
		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) { ret = -ENOMEM; goto stop_kthread; }

	tunables->rate_limit_us       = DEFAULT_RATE_LIMIT_US;
	tunables->iowait_boost_enable = true;
	policy->governor_data         = sg_policy;
	sg_policy->tunables           = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj,
				   &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy),
				   "%s", schedutil_gov.name);
	if (ret) goto fail;
out:
	mutex_unlock(&global_tunables_lock);
	return 0;
fail:
	policy->governor_data = NULL;
	sugov_tunables_free(tunables);
stop_kthread:
	if (!policy->fast_switch_enabled)
		sugov_kthread_stop(sg_policy);
	mutex_unlock(&global_tunables_lock);
free_sg_policy:
	sugov_policy_free(sg_policy);
disable_fast_switch:
	cpufreq_disable_fast_switch(policy);
	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);
	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		kobject_put(&tunables->attr_set.kobj);
		sugov_tunables_free(tunables);
	}
	mutex_unlock(&global_tunables_lock);
	if (!policy->fast_switch_enabled)
		sugov_kthread_stop(sg_policy);
	sugov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sg_policy->freq_update_delay_ns  = sg_policy->tunables->rate_limit_us * NSEC_PER_USEC;
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq             = UINT_MAX;
	sg_policy->work_in_progress      = false;
	sg_policy->need_freq_update      = false;
	sg_policy->cached_raw_freq       = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);
		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu              = cpu;
		sg_cpu->sg_policy        = sg_policy;
		sg_cpu->iowait_boost_max = policy->cpuinfo.max_freq;
	}
	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);
		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
			policy_is_shared(policy) ?
			sugov_update_shared : sugov_update_single);
	}
	return 0;
}

static void sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;
	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);
	synchronize_sched();
	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
}

static void sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	}
	sg_policy->need_freq_update = true;
}

static struct cpufreq_governor schedutil_gov = {
	.name              = "schedutil",
	.owner             = THIS_MODULE,
	.dynamic_switching = true,
	.init              = sugov_init,
	.exit              = sugov_exit,
	.start             = sugov_start,
	.stop              = sugov_stop,
	.limits            = sugov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &schedutil_gov;
}
#endif

static int __init sugov_register(void)   { return cpufreq_register_governor(&schedutil_gov); }
static void __exit sugov_unregister(void){ cpufreq_unregister_governor(&schedutil_gov); }

module_init(sugov_register);
module_exit(sugov_unregister);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rafael J. Wysocki <rafael.j.wysocki@intel.com>");
MODULE_DESCRIPTION("schedutil CPUfreq governor (msm-4.4 EAS backport)");
