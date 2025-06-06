/* SPDX-License-Identifier: GPL-2.0 */
/*
 *    Copyright IBM Corp. 1999, 2012
 *    Author(s): Denis Joseph Barrow,
 *		 Martin Schwidefsky <schwidefsky@de.ibm.com>,
 */
#ifndef __ASM_SMP_H
#define __ASM_SMP_H

#include <asm/processor.h>
#include <asm/lowcore.h>
#include <asm/machine.h>
#include <asm/sigp.h>

static __always_inline unsigned int raw_smp_processor_id(void)
{
	unsigned long lc_cpu_nr;
	unsigned int cpu;

	BUILD_BUG_ON(sizeof_field(struct lowcore, cpu_nr) != sizeof(cpu));
	lc_cpu_nr = offsetof(struct lowcore, cpu_nr);
	asm_inline(
		ALTERNATIVE("   ly      %[cpu],%[offzero](%%r0)\n",
			    "   ly      %[cpu],%[offalt](%%r0)\n",
			    ALT_FEATURE(MFEATURE_LOWCORE))
		: [cpu] "=d" (cpu)
		: [offzero] "i" (lc_cpu_nr),
		  [offalt] "i" (lc_cpu_nr + LOWCORE_ALT_ADDRESS),
		  "m" (((struct lowcore *)0)->cpu_nr));
	return cpu;
}

#define arch_scale_cpu_capacity smp_cpu_get_capacity

extern struct mutex smp_cpu_state_mutex;
extern unsigned int smp_cpu_mt_shift;
extern unsigned int smp_cpu_mtid;
extern __vector128 __initdata boot_cpu_vector_save_area[__NUM_VXRS];
extern cpumask_t cpu_setup_mask;

extern int __cpu_up(unsigned int cpu, struct task_struct *tidle);

extern void arch_send_call_function_single_ipi(int cpu);
extern void arch_send_call_function_ipi_mask(const struct cpumask *mask);

extern void smp_call_ipl_cpu(void (*func)(void *), void *);
extern void smp_emergency_stop(void);

extern int smp_find_processor_id(u16 address);
extern int smp_store_status(int cpu);
extern void smp_save_dump_ipl_cpu(void);
extern void smp_save_dump_secondary_cpus(void);
extern void smp_yield_cpu(int cpu);
extern void smp_cpu_set_polarization(int cpu, int val);
extern int smp_cpu_get_polarization(int cpu);
extern void smp_cpu_set_capacity(int cpu, unsigned long val);
extern void smp_set_core_capacity(int cpu, unsigned long val);
extern unsigned long smp_cpu_get_capacity(int cpu);
extern int smp_cpu_get_cpu_address(int cpu);
extern void smp_fill_possible_mask(void);
extern void smp_detect_cpus(void);

static inline void smp_stop_cpu(void)
{
	u16 pcpu = stap();

	for (;;) {
		__pcpu_sigp(pcpu, SIGP_STOP, 0, NULL);
		cpu_relax();
	}
}

/* Return thread 0 CPU number as base CPU */
static inline int smp_get_base_cpu(int cpu)
{
	return cpu - (cpu % (smp_cpu_mtid + 1));
}

static inline void smp_cpus_done(unsigned int max_cpus)
{
}

extern int smp_rescan_cpus(bool early);
extern void __noreturn cpu_die(void);
extern void __cpu_die(unsigned int cpu);
extern int __cpu_disable(void);
extern void schedule_mcck_handler(void);
void notrace smp_yield_cpu(int cpu);

#endif /* __ASM_SMP_H */
