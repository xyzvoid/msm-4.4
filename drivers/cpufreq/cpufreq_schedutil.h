/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal header for cpufreq_schedutil.c
 * Android kernel 4.4.205 (msm-4.4) EAS backport.
 */
#ifndef _CPUFREQ_SCHEDUTIL_H
#define _CPUFREQ_SCHEDUTIL_H

#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/irq_work.h>

struct sugov_policy;
struct sugov_cpu;
struct sugov_tunables;

#define to_sugov_tunables(attr_set) \
	container_of(attr_set, struct sugov_tunables, attr_set)

void sugov_get_util(unsigned long *util, unsigned long *max, int cpu);
bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu);
void sugov_deferred_update(struct sugov_policy *sg_policy,
			   u64 time, unsigned int next_freq);

#endif /* _CPUFREQ_SCHEDUTIL_H */
