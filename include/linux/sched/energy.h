/* SPDX-License-Identifier: GPL-2.0 */
/* EAS energy model additions -- msm-4.4 backport
 *
 * NOTE: struct capacity_state, struct idle_state, struct sched_group_energy
 * and energy_diff() are already declared in include/linux/sched.h on msm-4.4.
 * This header only adds the per-CPU cpu_sge accessor and the
 * find_energy_efficient_cpu() helper that BBR/EAS placement uses.
 */
#ifndef _LINUX_SCHED_ENERGY_H
#define _LINUX_SCHED_ENERGY_H

#include <linux/percpu.h>
/* sched.h provides: struct capacity_state, idle_state, sched_group_energy */
#include <linux/sched.h>

/* Per-CPU pointer to the platform energy model table.
 * Populated by the SoC energy-model driver (or arch/arm64/kernel/topology.c).
 */
#ifndef cpu_sge
DECLARE_PER_CPU(const struct sched_group_energy *, cpu_sge);
static inline const struct sched_group_energy *cpu_energy(int cpu)
{
	return per_cpu(cpu_sge, cpu);
}
#endif /* cpu_sge */

#ifdef CONFIG_SCHED_EAS
/* find_energy_efficient_cpu - select lowest-energy CPU for task p.
 * Returns a valid CPU index, or -1 to fall back to normal selection.
 * Declared here; implemented in kernel/sched/fair.c (or energy.c).
 */
extern int find_energy_efficient_cpu(struct task_struct *p, int prev_cpu,
				     const struct cpumask *cpus);
#else
static inline int find_energy_efficient_cpu(struct task_struct *p, int prev,
					    const struct cpumask *c)
{
	return -1;
}
#endif /* CONFIG_SCHED_EAS */

#endif /* _LINUX_SCHED_ENERGY_H */
