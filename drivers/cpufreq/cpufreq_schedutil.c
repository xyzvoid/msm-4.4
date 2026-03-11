// SPDX-License-Identifier: GPL-2.0
/* schedutil cpufreq governor - msm-4.4 EAS backport
 * Copyright (C) 2016 Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com> */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include "cpufreq_schedutil.h"
#define DEFAULT_RATE_LIMIT_US (5 * USEC_PER_MSEC)
struct sugov_tunables {
	unsigned int rate_limit_us;
	bool iowait_boost_enable;
	struct gov_attr_set attr_set;
};
struct sugov_policy {
	struct cpufreq_policy *policy;
	struct sugov_tunables *tunables;
	struct list_head tunables_hook;
	raw_spinlock_t update_lock;
	u64 last_freq_update_time;
	s64 freq_update_delay_ns;
	unsigned int next_freq, cached_raw_freq;
	struct irq_work irq_work;
	struct kthread_work work;
	struct mutex work_lock;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress, need_freq_update;
};
struct sugov_cpu {
	struct update_util_data update_util;
	struct sugov_policy *sg_policy;
	unsigned int cpu;
	bool iowait_boost_pending;
	unsigned int iowait_boost, iowait_boost_max;
	u64 last_update;
	unsigned long util, max;
	unsigned int flags;
};
static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);
static struct cpufreq_governor schedutil_gov;
static DEFINE_MUTEX(global_tunables_lock);
static struct sugov_tunables *global_tunables;
static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	if (unlikely(sg_policy->need_freq_update)) {
		sg_policy->need_freq_update = false;
		sg_policy->next_freq = UINT_MAX;
		return true;
	}
	return (s64)(time - sg_policy->last_freq_update_time)
		>= sg_policy->freq_update_delay_ns;
}
static bool sugov_update_next_freq(struct sugov_policy *sg_policy,
				   u64 time, unsigned int next_freq)
{
	if (sg_policy->next_freq == next_freq) return false;
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
	if (!sg_cpu->iowait_boost_pending) return;
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
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
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
					   unsigned long util, unsigned long max,
					   unsigned int flags)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int max_f = policy->cpuinfo.max_freq;
	u64 lft = sg_policy->last_freq_update_time;
	unsigned int j;
	if (flags & SCHED_CPUFREQ_DL) return max_f;
	sugov_iowait_boost(sg_cpu, &util, &max);
	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg;
		unsigned long ju, jm;
		if (j == sg_cpu->cpu) continue;
		j_sg = &per_cpu(sugov_cpu, j);
		if (j_sg->flags & SCHED_CPUFREQ_DL) return max_f;
		if ((s64)(lft - j_sg->last_update) > TICK_NSEC) continue;
		ju = j_sg->util; jm = j_sg->max;
		if (ju * max > jm * util) { util = ju; max = jm; }
		sugov_iowait_boost(j_sg, &util, &max);
	}
	return get_next_freq(sg_policy, util, max);
}
static void sugov_update_shared(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max;
	unsigned int next_f;
	sugov_get_util(&util, &max, sg_cpu->cpu);
	raw_spin_lock(&sg_policy->update_lock);
	sg_cpu->util = util; sg_cpu->max = max;
	sg_cpu->flags = flags; sg_cpu->last_update = time;
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
	struct sugov_policy *sg_policy = container_of(work, struct sugov_policy, work);
	unsigned int freq; unsigned long flags;
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
	struct sugov_policy *sg_policy = container_of(irq_work, struct sugov_policy, irq_work);
	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}
static ssize_t rate_limit_us_show(struct gov_attr_set *a, char *buf) {
	return sprintf(buf, "%u\n", to_sugov_tunables(a)->rate_limit_us); }
static ssize_t rate_limit_us_store(struct gov_attr_set *a, const char *buf, size_t c) {
	struct sugov_tunables *t = to_sugov_tunables(a);
	struct sugov_policy *p; unsigned int v;
	if (kstrtouint(buf, 10, &v)) return -EINVAL;
	t->rate_limit_us = v;
	list_for_each_entry(p, &a->policy_list, tunables_hook)
		p->freq_update_delay_ns = v * NSEC_PER_USEC;
	return c; }
static struct governor_attr rate_limit_us_attr = __ATTR_RW(rate_limit_us);
static ssize_t iowait_boost_enable_show(struct gov_attr_set *a, char *buf) {
	return sprintf(buf, "%u\n", to_sugov_tunables(a)->iowait_boost_enable); }
static ssize_t iowait_boost_enable_store(struct gov_attr_set *a, const char *buf, size_t c) {
	unsigned int v;
	if (kstrtouint(buf, 10, &v)) return -EINVAL;
	to_sugov_tunables(a)->iowait_boost_enable = !!v; return c; }
static struct governor_attr iowait_boost_enable_attr = __ATTR_RW(iowait_boost_enable);
static struct attribute *sugov_attrs[] = {
	&rate_limit_us_attr.attr, &iowait_boost_enable_attr.attr, NULL };
static struct kobj_type sugov_tunables_ktype = {
	.default_attrs = sugov_attrs, .sysfs_ops = &governor_sysfs_ops };
static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg = kzalloc(sizeof(*sg), GFP_KERNEL);
	if (!sg) return NULL;
	sg->policy = policy; raw_spin_lock_init(&sg->update_lock); return sg;
}
static void sugov_policy_free(struct sugov_policy *sg) { kfree(sg); }
static int sugov_kthread_create(struct sugov_policy *sg)
{
	struct task_struct *t; int ret;
	kthread_init_work(&sg->work, sugov_work);
	kthread_init_worker(&sg->worker);
	t = kthread_create(kthread_worker_fn, &sg->worker, "sugov:%d", sg->policy->cpu);
	if (IS_ERR(t)) return PTR_ERR(t);
	sg->thread = t;
	kthread_bind_mask(t, sg->policy->related_cpus);
	init_irq_work(&sg->irq_work, sugov_irq_work);
	mutex_init(&sg->work_lock);
	wake_up_process(t); return 0;
}
static void sugov_kthread_stop(struct sugov_policy *sg)
{
	kthread_flush_worker(&sg->worker);
	kthread_stop(sg->thread);
	mutex_destroy(&sg->work_lock);
}
static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg)
{
	struct sugov_tunables *t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (t) {
		gov_attr_set_init(&t->attr_set, &sg->tunables_hook);
		if (!have_governor_per_policy()) schedutil_gov.tunables = t;
	}
	return t;
}
static void sugov_tunables_free(struct sugov_tunables *t)
{
	if (!have_governor_per_policy()) schedutil_gov.tunables = NULL;
	kfree(t);
}
static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg; struct sugov_tunables *t; int ret = 0;
	if (policy->governor_data) return -EBUSY;
	cpufreq_enable_fast_switch(policy);
	sg = sugov_policy_alloc(policy);
	if (!sg) { ret = -ENOMEM; goto disable_fast_switch; }
	if (!policy->fast_switch_enabled) {
		ret = sugov_kthread_create(sg);
		if (ret) goto free_sg;
	}
	mutex_lock(&global_tunables_lock);
	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) { ret = -EINVAL; goto stop_kt; }
		policy->governor_data = sg; sg->tunables = global_tunables;
		gov_attr_set_get(&global_tunables->attr_set, &sg->tunables_hook);
		goto out;
	}
	t = sugov_tunables_alloc(sg);
	if (!t) { ret = -ENOMEM; goto stop_kt; }
	t->rate_limit_us = DEFAULT_RATE_LIMIT_US; t->iowait_boost_enable = true;
	policy->governor_data = sg; sg->tunables = t;
	ret = kobject_init_and_add(&t->attr_set.kobj, &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s", schedutil_gov.name);
	if (ret) goto fail;
out:
	mutex_unlock(&global_tunables_lock); return 0;
fail:
	policy->governor_data = NULL; sugov_tunables_free(t);
stop_kt:
	if (!policy->fast_switch_enabled) sugov_kthread_stop(sg);
	mutex_unlock(&global_tunables_lock);
free_sg:
	sugov_policy_free(sg);
disable_fast_switch:
	cpufreq_disable_fast_switch(policy);
	pr_err("init failed (%d)\n", ret); return ret;
}
static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg = policy->governor_data;
	struct sugov_tunables *t = sg->tunables; unsigned int c;
	mutex_lock(&global_tunables_lock);
	c = gov_attr_set_put(&t->attr_set, &sg->tunables_hook);
	policy->governor_data = NULL;
	if (!c) { kobject_put(&t->attr_set.kobj); sugov_tunables_free(t); }
	mutex_unlock(&global_tunables_lock);
	if (!policy->fast_switch_enabled) sugov_kthread_stop(sg);
	sugov_policy_free(sg); cpufreq_disable_fast_switch(policy);
}
static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg = policy->governor_data; unsigned int cpu;
	sg->freq_update_delay_ns  = sg->tunables->rate_limit_us * NSEC_PER_USEC;
	sg->last_freq_update_time = 0; sg->next_freq = UINT_MAX;
	sg->work_in_progress = false; sg->need_freq_update = false;
	sg->cached_raw_freq = 0;
	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sc = &per_cpu(sugov_cpu, cpu);
		memset(sc, 0, sizeof(*sc));
		sc->cpu = cpu; sc->sg_policy = sg;
		sc->iowait_boost_max = policy->cpuinfo.max_freq;
	}
	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sc = &per_cpu(sugov_cpu, cpu);
		cpufreq_add_update_util_hook(cpu, &sc->update_util,
			policy_is_shared(policy) ? sugov_update_shared : sugov_update_single);
	}
	return 0;
}
static void sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg = policy->governor_data; unsigned int cpu;
	for_each_cpu(cpu, policy->cpus) cpufreq_remove_update_util_hook(cpu);
	synchronize_sched();
	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg->irq_work);
		kthread_cancel_work_sync(&sg->work);
	}
}
static void sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg = policy->governor_data;
	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg->work_lock);
	}
	sg->need_freq_update = true;
}
static struct cpufreq_governor schedutil_gov = {
	.name = "schedutil", .owner = THIS_MODULE, .dynamic_switching = true,
	.init = sugov_init, .exit = sugov_exit, .start = sugov_start,
	.stop = sugov_stop, .limits = sugov_limits,
};
#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
struct cpufreq_governor *cpufreq_default_governor(void) { return &schedutil_gov; }
#endif
static int __init sugov_register(void)   { return cpufreq_register_governor(&schedutil_gov); }
static void __exit sugov_unregister(void){ cpufreq_unregister_governor(&schedutil_gov); }
module_init(sugov_register);
module_exit(sugov_unregister);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("schedutil cpufreq governor (msm-4.4 EAS backport)");
