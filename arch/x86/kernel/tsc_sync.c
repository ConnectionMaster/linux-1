// SPDX-License-Identifier: GPL-2.0
/*
 * check TSC synchronization.
 *
 * Copyright (C) 2006, Red Hat, Inc., Ingo Molnar
 *
 * We check whether all boot CPUs have their TSC's synchronized,
 * print a warning if not and turn off the TSC clock-source.
 *
 * The warp-check is point-to-point between two CPUs, the CPU
 * initiating the bootup is the 'source CPU', the freshly booting
 * CPU is the 'target CPU'.
 *
 * Only two CPUs may participate - they can enter in any order.
 * ( The serial nature of the boot logic and the CPU hotplug lock
 *   protects against more than 2 CPUs entering this code. )
 */
#include <linux/workqueue.h>
#include <linux/topology.h>
#include <linux/spinlock.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/nmi.h>
#include <asm/msr.h>
#include <asm/tsc.h>

struct tsc_adjust {
	s64		bootval;
	s64		adjusted;
	unsigned long	nextcheck;
	bool		warned;
};

static DEFINE_PER_CPU(struct tsc_adjust, tsc_adjust);
static struct timer_list tsc_sync_check_timer;

/*
 * TSC's on different sockets may be reset asynchronously.
 * This may cause the TSC ADJUST value on socket 0 to be NOT 0.
 */
bool __read_mostly tsc_async_resets;

void mark_tsc_async_resets(char *reason)
{
	if (tsc_async_resets)
		return;
	tsc_async_resets = true;
	pr_info("tsc: Marking TSC async resets true due to %s\n", reason);
}

void tsc_verify_tsc_adjust(bool resume)
{
	struct tsc_adjust *adj = this_cpu_ptr(&tsc_adjust);
	s64 curval;

	if (!boot_cpu_has(X86_FEATURE_TSC_ADJUST))
		return;

	/* Skip unnecessary error messages if TSC already unstable */
	if (check_tsc_unstable())
		return;

	/* Rate limit the MSR check */
	if (!resume && time_before(jiffies, adj->nextcheck))
		return;

	adj->nextcheck = jiffies + HZ;

	rdmsrq(MSR_IA32_TSC_ADJUST, curval);
	if (adj->adjusted == curval)
		return;

	/* Restore the original value */
	wrmsrq(MSR_IA32_TSC_ADJUST, adj->adjusted);

	if (!adj->warned || resume) {
		pr_warn(FW_BUG "TSC ADJUST differs: CPU%u %lld --> %lld. Restoring\n",
			smp_processor_id(), adj->adjusted, curval);
		adj->warned = true;
	}
}

/*
 * Normally the tsc_sync will be checked every time system enters idle
 * state, but there is still caveat that a system won't enter idle,
 * either because it's too busy or configured purposely to not enter
 * idle.
 *
 * So setup a periodic timer (every 10 minutes) to make sure the check
 * is always on.
 */

#define SYNC_CHECK_INTERVAL		(HZ * 600)

static void tsc_sync_check_timer_fn(struct timer_list *unused)
{
	int next_cpu;

	tsc_verify_tsc_adjust(false);

	/* Run the check for all onlined CPUs in turn */
	next_cpu = cpumask_next(raw_smp_processor_id(), cpu_online_mask);
	if (next_cpu >= nr_cpu_ids)
		next_cpu = cpumask_first(cpu_online_mask);

	tsc_sync_check_timer.expires += SYNC_CHECK_INTERVAL;
	add_timer_on(&tsc_sync_check_timer, next_cpu);
}

static int __init start_sync_check_timer(void)
{
	if (!cpu_feature_enabled(X86_FEATURE_TSC_ADJUST) || tsc_clocksource_reliable)
		return 0;

	timer_setup(&tsc_sync_check_timer, tsc_sync_check_timer_fn, 0);
	tsc_sync_check_timer.expires = jiffies + SYNC_CHECK_INTERVAL;
	add_timer(&tsc_sync_check_timer);

	return 0;
}
late_initcall(start_sync_check_timer);

static void tsc_sanitize_first_cpu(struct tsc_adjust *cur, s64 bootval,
				   unsigned int cpu, bool bootcpu)
{
	/*
	 * First online CPU in a package stores the boot value in the
	 * adjustment value. This value might change later via the sync
	 * mechanism. If that fails we still can yell about boot values not
	 * being consistent.
	 *
	 * On the boot cpu we just force set the ADJUST value to 0 if it's
	 * non zero. We don't do that on non boot cpus because physical
	 * hotplug should have set the ADJUST register to a value > 0 so
	 * the TSC is in sync with the already running cpus.
	 *
	 * Also don't force the ADJUST value to zero if that is a valid value
	 * for socket 0 as determined by the system arch.  This is required
	 * when multiple sockets are reset asynchronously with each other
	 * and socket 0 may not have an TSC ADJUST value of 0.
	 */
	if (bootcpu && bootval != 0) {
		if (likely(!tsc_async_resets)) {
			pr_warn(FW_BUG "TSC ADJUST: CPU%u: %lld force to 0\n",
				cpu, bootval);
			wrmsrq(MSR_IA32_TSC_ADJUST, 0);
			bootval = 0;
		} else {
			pr_info("TSC ADJUST: CPU%u: %lld NOT forced to 0\n",
				cpu, bootval);
		}
	}
	cur->adjusted = bootval;
}

#ifndef CONFIG_SMP
bool __init tsc_store_and_check_tsc_adjust(bool bootcpu)
{
	struct tsc_adjust *cur = this_cpu_ptr(&tsc_adjust);
	s64 bootval;

	if (!boot_cpu_has(X86_FEATURE_TSC_ADJUST))
		return false;

	/* Skip unnecessary error messages if TSC already unstable */
	if (check_tsc_unstable())
		return false;

	rdmsrq(MSR_IA32_TSC_ADJUST, bootval);
	cur->bootval = bootval;
	cur->nextcheck = jiffies + HZ;
	tsc_sanitize_first_cpu(cur, bootval, smp_processor_id(), bootcpu);
	return false;
}

#else /* !CONFIG_SMP */

/*
 * Store and check the TSC ADJUST MSR if available
 */
bool tsc_store_and_check_tsc_adjust(bool bootcpu)
{
	struct tsc_adjust *ref, *cur = this_cpu_ptr(&tsc_adjust);
	unsigned int refcpu, cpu = smp_processor_id();
	struct cpumask *mask;
	s64 bootval;

	if (!boot_cpu_has(X86_FEATURE_TSC_ADJUST))
		return false;

	rdmsrq(MSR_IA32_TSC_ADJUST, bootval);
	cur->bootval = bootval;
	cur->nextcheck = jiffies + HZ;
	cur->warned = false;

	/*
	 * The default adjust value cannot be assumed to be zero on any socket.
	 */
	cur->adjusted = bootval;

	/*
	 * Check whether this CPU is the first in a package to come up. In
	 * this case do not check the boot value against another package
	 * because the new package might have been physically hotplugged,
	 * where TSC_ADJUST is expected to be different. When called on the
	 * boot CPU topology_core_cpumask() might not be available yet.
	 */
	mask = topology_core_cpumask(cpu);
	refcpu = mask ? cpumask_any_but(mask, cpu) : nr_cpu_ids;

	if (refcpu >= nr_cpu_ids) {
		tsc_sanitize_first_cpu(cur, bootval, smp_processor_id(),
				       bootcpu);
		return false;
	}

	ref = per_cpu_ptr(&tsc_adjust, refcpu);
	/*
	 * Compare the boot value and complain if it differs in the
	 * package.
	 */
	if (bootval != ref->bootval)
		printk_once(FW_BUG "TSC ADJUST differs within socket(s), fixing all errors\n");

	/*
	 * The TSC_ADJUST values in a package must be the same. If the boot
	 * value on this newly upcoming CPU differs from the adjustment
	 * value of the already online CPU in this package, set it to that
	 * adjusted value.
	 */
	if (bootval != ref->adjusted) {
		cur->adjusted = ref->adjusted;
		wrmsrq(MSR_IA32_TSC_ADJUST, ref->adjusted);
	}
	/*
	 * We have the TSCs forced to be in sync on this package. Skip sync
	 * test:
	 */
	return true;
}

/*
 * Entry/exit counters that make sure that both CPUs
 * run the measurement code at once:
 */
static atomic_t start_count;
static atomic_t stop_count;
static atomic_t test_runs;

/*
 * We use a raw spinlock in this exceptional case, because
 * we want to have the fastest, inlined, non-debug version
 * of a critical section, to be able to prove TSC time-warps:
 */
static arch_spinlock_t sync_lock = __ARCH_SPIN_LOCK_UNLOCKED;

static cycles_t last_tsc;
static cycles_t max_warp;
static int nr_warps;
static int random_warps;

/*
 * TSC-warp measurement loop running on both CPUs.  This is not called
 * if there is no TSC.
 */
static cycles_t check_tsc_warp(unsigned int timeout)
{
	cycles_t start, now, prev, end, cur_max_warp = 0;
	int i, cur_warps = 0;

	start = rdtsc_ordered();
	/*
	 * The measurement runs for 'timeout' msecs:
	 */
	end = start + (cycles_t) tsc_khz * timeout;

	for (i = 0; ; i++) {
		/*
		 * We take the global lock, measure TSC, save the
		 * previous TSC that was measured (possibly on
		 * another CPU) and update the previous TSC timestamp.
		 */
		arch_spin_lock(&sync_lock);
		prev = last_tsc;
		now = rdtsc_ordered();
		last_tsc = now;
		arch_spin_unlock(&sync_lock);

		/*
		 * Be nice every now and then (and also check whether
		 * measurement is done [we also insert a 10 million
		 * loops safety exit, so we dont lock up in case the
		 * TSC readout is totally broken]):
		 */
		if (unlikely(!(i & 7))) {
			if (now > end || i > 10000000)
				break;
			cpu_relax();
			touch_nmi_watchdog();
		}
		/*
		 * Outside the critical section we can now see whether
		 * we saw a time-warp of the TSC going backwards:
		 */
		if (unlikely(prev > now)) {
			arch_spin_lock(&sync_lock);
			max_warp = max(max_warp, prev - now);
			cur_max_warp = max_warp;
			/*
			 * Check whether this bounces back and forth. Only
			 * one CPU should observe time going backwards.
			 */
			if (cur_warps != nr_warps)
				random_warps++;
			nr_warps++;
			cur_warps = nr_warps;
			arch_spin_unlock(&sync_lock);
		}
	}
	WARN(!(now-start),
		"Warning: zero tsc calibration delta: %Ld [max: %Ld]\n",
			now-start, end-start);
	return cur_max_warp;
}

/*
 * If the target CPU coming online doesn't have any of its core-siblings
 * online, a timeout of 20msec will be used for the TSC-warp measurement
 * loop. Otherwise a smaller timeout of 2msec will be used, as we have some
 * information about this socket already (and this information grows as we
 * have more and more logical-siblings in that socket).
 *
 * Ideally we should be able to skip the TSC sync check on the other
 * core-siblings, if the first logical CPU in a socket passed the sync test.
 * But as the TSC is per-logical CPU and can potentially be modified wrongly
 * by the bios, TSC sync test for smaller duration should be able
 * to catch such errors. Also this will catch the condition where all the
 * cores in the socket don't get reset at the same time.
 */
static inline unsigned int loop_timeout(int cpu)
{
	return (cpumask_weight(topology_core_cpumask(cpu)) > 1) ? 2 : 20;
}

static void tsc_sync_mark_tsc_unstable(struct work_struct *work)
{
	mark_tsc_unstable("check_tsc_sync_source failed");
}

static DECLARE_WORK(tsc_sync_work, tsc_sync_mark_tsc_unstable);

/*
 * The freshly booted CPU initiates this via an async SMP function call.
 */
static void check_tsc_sync_source(void *__cpu)
{
	unsigned int cpu = (unsigned long)__cpu;
	int cpus = 2;

	/*
	 * Set the maximum number of test runs to
	 *  1 if the CPU does not provide the TSC_ADJUST MSR
	 *  3 if the MSR is available, so the target can try to adjust
	 */
	if (!boot_cpu_has(X86_FEATURE_TSC_ADJUST))
		atomic_set(&test_runs, 1);
	else
		atomic_set(&test_runs, 3);
retry:
	/* Wait for the target to start. */
	while (atomic_read(&start_count) != cpus - 1)
		cpu_relax();

	/*
	 * Trigger the target to continue into the measurement too:
	 */
	atomic_inc(&start_count);

	check_tsc_warp(loop_timeout(cpu));

	while (atomic_read(&stop_count) != cpus-1)
		cpu_relax();

	/*
	 * If the test was successful set the number of runs to zero and
	 * stop. If not, decrement the number of runs an check if we can
	 * retry. In case of random warps no retry is attempted.
	 */
	if (!nr_warps) {
		atomic_set(&test_runs, 0);

		pr_debug("TSC synchronization [CPU#%d -> CPU#%u]: passed\n",
			smp_processor_id(), cpu);

	} else if (atomic_dec_and_test(&test_runs) || random_warps) {
		/* Force it to 0 if random warps brought us here */
		atomic_set(&test_runs, 0);

		pr_warn("TSC synchronization [CPU#%d -> CPU#%u]:\n",
			smp_processor_id(), cpu);
		pr_warn("Measured %Ld cycles TSC warp between CPUs, "
			"turning off TSC clock.\n", max_warp);
		if (random_warps)
			pr_warn("TSC warped randomly between CPUs\n");
		schedule_work(&tsc_sync_work);
	}

	/*
	 * Reset it - just in case we boot another CPU later:
	 */
	atomic_set(&start_count, 0);
	random_warps = 0;
	nr_warps = 0;
	max_warp = 0;
	last_tsc = 0;

	/*
	 * Let the target continue with the bootup:
	 */
	atomic_inc(&stop_count);

	/*
	 * Retry, if there is a chance to do so.
	 */
	if (atomic_read(&test_runs) > 0)
		goto retry;
}

/*
 * Freshly booted CPUs call into this:
 */
void check_tsc_sync_target(void)
{
	struct tsc_adjust *cur = this_cpu_ptr(&tsc_adjust);
	unsigned int cpu = smp_processor_id();
	cycles_t cur_max_warp, gbl_max_warp;
	int cpus = 2;

	/* Also aborts if there is no TSC. */
	if (unsynchronized_tsc())
		return;

	/*
	 * Store, verify and sanitize the TSC adjust register. If
	 * successful skip the test.
	 *
	 * The test is also skipped when the TSC is marked reliable. This
	 * is true for SoCs which have no fallback clocksource. On these
	 * SoCs the TSC is frequency synchronized, but still the TSC ADJUST
	 * register might have been wreckaged by the BIOS..
	 */
	if (tsc_store_and_check_tsc_adjust(false) || tsc_clocksource_reliable)
		return;

	/* Kick the control CPU into the TSC synchronization function */
	smp_call_function_single(cpumask_first(cpu_online_mask), check_tsc_sync_source,
				 (unsigned long *)(unsigned long)cpu, 0);
retry:
	/*
	 * Register this CPU's participation and wait for the
	 * source CPU to start the measurement:
	 */
	atomic_inc(&start_count);
	while (atomic_read(&start_count) != cpus)
		cpu_relax();

	cur_max_warp = check_tsc_warp(loop_timeout(cpu));

	/*
	 * Store the maximum observed warp value for a potential retry:
	 */
	gbl_max_warp = max_warp;

	/*
	 * Ok, we are done:
	 */
	atomic_inc(&stop_count);

	/*
	 * Wait for the source CPU to print stuff:
	 */
	while (atomic_read(&stop_count) != cpus)
		cpu_relax();

	/*
	 * Reset it for the next sync test:
	 */
	atomic_set(&stop_count, 0);

	/*
	 * Check the number of remaining test runs. If not zero, the test
	 * failed and a retry with adjusted TSC is possible. If zero the
	 * test was either successful or failed terminally.
	 */
	if (!atomic_read(&test_runs))
		return;

	/*
	 * If the warp value of this CPU is 0, then the other CPU
	 * observed time going backwards so this TSC was ahead and
	 * needs to move backwards.
	 */
	if (!cur_max_warp)
		cur_max_warp = -gbl_max_warp;

	/*
	 * Add the result to the previous adjustment value.
	 *
	 * The adjustment value is slightly off by the overhead of the
	 * sync mechanism (observed values are ~200 TSC cycles), but this
	 * really depends on CPU, node distance and frequency. So
	 * compensating for this is hard to get right. Experiments show
	 * that the warp is not longer detectable when the observed warp
	 * value is used. In the worst case the adjustment needs to go
	 * through a 3rd run for fine tuning.
	 */
	cur->adjusted += cur_max_warp;

	pr_warn("TSC ADJUST compensate: CPU%u observed %lld warp. Adjust: %lld\n",
		cpu, cur_max_warp, cur->adjusted);

	wrmsrq(MSR_IA32_TSC_ADJUST, cur->adjusted);
	goto retry;

}

#endif /* CONFIG_SMP */
