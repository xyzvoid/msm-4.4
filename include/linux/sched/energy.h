/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Energy model API for EAS - Android kernel 4.4.205 (msm-4.4) backport.
 */
#ifndef _LINUX_SCHED_ENERGY_H
#define _LINUX_SCHED_ENERGY_H

#include <linux/percpu.h>
#include <linux/sched.h>

struct capacity_state {
	unsigned long cap;   /* CPU capacity (1024 == max) */
	unsigned int  power; /* dynamic power at this OPP (mW) */
};

struct idle_state {
	unsigned int power; /* idle power (mW) */
};

struct sched_group_energy {
	unsigned int          nr_cap_states;
	struct capacity_state *cap_states;
	unsigned int          nr_idle_states;
	struct idle_state     *idle_states;
};

DECLARE_PER_CPU(const struct sched_group_energy *, cpu_sge);

static inline const struct sched_group_energy *cpu_energy(int cpu)
{
	return per_cpu(cpu_sge, cpu);
}

#ifdef CONFIG_SCHED_EAS
extern unsigned long energy_diff(struct task_struct *p,
				 int prev_cpu, int new_cpu,
				 unsigned long util_delta);
extern int find_energy_efficient_cpu(struct task_struct *p,
				     int prev_cpu,
				     const struct cpumask *cpus);
#else
static inline unsigned long energy_diff(struct task_struct *p,
					int prev_cpu, int new_cpu,
					unsigned long util_delta) { return 0; }
static inline int find_energy_efficient_cpu(struct task_struct *p,
					    int prev_cpu,
					    const struct cpumask *cpus)
{ return -ENOSYS; }
#endif

#endif /* _LINUX_SCHED_ENERGY_H */
