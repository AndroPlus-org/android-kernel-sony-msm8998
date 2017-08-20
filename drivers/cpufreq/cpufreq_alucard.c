/*
 * Alucard - Load Sensitive CPU Frequency Governor
 *
 * Copyright (c) 2011-2016, Alucard24 <dmbaoh2@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>

struct cpufreq_alucard_policyinfo {
	struct timer_list policy_timer;
	struct timer_list policy_slack_timer;
	struct hrtimer notif_timer;
	spinlock_t load_lock; /* protects load tracking stat */
	u64 last_evaluated_jiffy;
	struct cpufreq_policy *policy;
	struct cpufreq_policy p_nolim; /* policy copy with no limits */
	struct cpufreq_frequency_table *freq_table;
	spinlock_t target_freq_lock; /*protects target freq */
	unsigned int target_freq;
	unsigned int min_freq;
	unsigned int up_sampling_time;
	unsigned int down_sampling_time;
	struct rw_semaphore enable_sem;
	bool reject_notification;
	bool notif_pending;
	unsigned long notif_cpu;
	int governor_enabled;
	struct cpufreq_alucard_tunables *cached_tunables;
	struct sched_load *sl;
};

/* Protected by per-policy load_lock */
struct cpufreq_alucard_cpuinfo {
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;
	unsigned int load;
};

static DEFINE_PER_CPU(struct cpufreq_alucard_policyinfo *, polinfo);
static DEFINE_PER_CPU(struct cpufreq_alucard_cpuinfo, cpuinfo);

/* realtime thread handles frequency scaling */
static struct task_struct *speedchange_task;
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;
static struct mutex gov_lock;

static int set_window_count;
static int migration_register_count;
static struct mutex sched_lock;
static cpumask_t controlled_cpus;

#define LITTLE_NFREQS		22
#define BIG_NFREQS			31

/* Target loads.  */
static unsigned int little_up_target_loads[LITTLE_NFREQS] = {
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	95,
	95
};

static unsigned int little_down_target_loads[LITTLE_NFREQS] = {
	0,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	95,
	95
};

static unsigned int big_up_target_loads[BIG_NFREQS] = {
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	90,
	90,
	95,
	95,
	95,
	95,
	95,
	95,
	95
};

static unsigned int big_down_target_loads[BIG_NFREQS] = {
	0,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	83,
	90,
	90,
	95,
	95,
	95,
	95,
	95,
	95,
	95
};

static unsigned int little_up_target_sampling_time[LITTLE_NFREQS] = {
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1
};

static unsigned int little_down_target_sampling_time[LITTLE_NFREQS] = {
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1
};

static unsigned int big_up_target_sampling_time[BIG_NFREQS] = {
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1
};

static unsigned int big_down_target_sampling_time[BIG_NFREQS] = {
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1
};

#define DEFAULT_TIMER_RATE (20 * USEC_PER_MSEC)

#define FREQ_RESPONSIVENESS_LITTLE	1324800
#define FREQ_RESPONSIVENESS_BIG		1344000
#define LOAD_RESPONSIVENESS			90
#define PUMP_INC_STEP_AT_MIN_FREQ	3
#define PUMP_INC_STEP			2
#define PUMP_DEC_STEP_AT_MIN_FREQ	3
#define PUMP_DEC_STEP			2

struct cpufreq_alucard_tunables {
	int usage_count;
	/*
	 * CPUs frequency scaling
	 */
	int freq_responsiveness;
	bool freq_responsiveness_jump;
	int load_responsiveness;
	int pump_inc_step;
	int pump_inc_step_at_min_freq;
	int pump_dec_step;
	int pump_dec_step_at_min_freq;

	/* Target loads */
	spinlock_t target_loads_lock;
	unsigned int *up_target_loads;
	unsigned int *down_target_loads;
	int ntarget_loads;
	/*
	 * The sample rate of the timer used to increase frequency
	 */
	unsigned long timer_rate;
	/*
	 * Wait this long before raising speed above the current cpu frequency, by default a
	 * single timer interval.
	 */
	spinlock_t target_sampling_time_lock;
	unsigned int *up_target_sampling_time;
	unsigned int *down_target_sampling_time;
	int ntarget_sampling_time;
	/*
	 * Max additional time to wait in idle, beyond timer_rate, at speeds
	 * above minimum before wakeup to reduce speed, or -1 if unnecessary.
	 */
#define DEFAULT_TIMER_SLACK (4 * DEFAULT_TIMER_RATE)
	int timer_slack_val;
	bool io_is_busy;

	/* scheduler input related flags */
	bool use_sched_load;
	bool use_migration_notif;

	/*
	 * Whether to align timer windows across all CPUs. When
	 * use_sched_load is true, this flag is ignored and windows
	 * will always be aligned.
	 */
	bool align_windows;

	/* Whether to enable prediction or not */
	bool enable_prediction;
};

/* For cases where we have single governor instance for system */
static struct cpufreq_alucard_tunables *common_tunables;
static struct cpufreq_alucard_tunables *cached_common_tunables;

static struct attribute_group *get_sysfs_attr(void);

/* Round to starting jiffy of next evaluation window */
static u64 round_to_nw_start(u64 jif,
			     struct cpufreq_alucard_tunables *tunables)
{
	unsigned long step = usecs_to_jiffies(tunables->timer_rate);
	u64 ret;

	if (tunables->use_sched_load || tunables->align_windows) {
		do_div(jif, step);
		ret = (jif + 1) * step;
	} else {
		ret = jiffies + usecs_to_jiffies(tunables->timer_rate);
	}

	return ret;
}

static inline int set_window_helper(
			struct cpufreq_alucard_tunables *tunables)
{
	return sched_set_window(round_to_nw_start(get_jiffies_64(), tunables),
			 usecs_to_jiffies(tunables->timer_rate));
}

static void cpufreq_alucard_timer_resched(unsigned long cpu,
					      bool slack_only)
{
	struct cpufreq_alucard_policyinfo *ppol = per_cpu(polinfo, cpu);
	struct cpufreq_alucard_cpuinfo *pcpu;
	struct cpufreq_alucard_tunables *tunables =
		ppol->policy->governor_data;
	u64 expires;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&ppol->load_lock, flags);
	expires = round_to_nw_start(ppol->last_evaluated_jiffy, tunables);
	if (!slack_only) {
		for_each_cpu(i, ppol->policy->cpus) {
			pcpu = &per_cpu(cpuinfo, i);
			pcpu->time_in_idle = get_cpu_idle_time(i,
						&pcpu->time_in_idle_timestamp,
						tunables->io_is_busy);
			pcpu->cputime_speedadj = 0;
			pcpu->cputime_speedadj_timestamp =
						pcpu->time_in_idle_timestamp;
		}
		del_timer(&ppol->policy_timer);
		ppol->policy_timer.expires = expires;
		add_timer(&ppol->policy_timer);
	}

	if (tunables->timer_slack_val >= 0 &&
	    ppol->target_freq > ppol->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_slack_val);
		del_timer(&ppol->policy_slack_timer);
		ppol->policy_slack_timer.expires = expires;
		add_timer(&ppol->policy_slack_timer);
	}

	spin_unlock_irqrestore(&ppol->load_lock, flags);
}

/* The caller shall take enable_sem write semaphore to avoid any timer race.
 * The policy_timer and policy_slack_timer must be deactivated when calling
 * this function.
 */
static void cpufreq_alucard_timer_start(
	struct cpufreq_alucard_tunables *tunables, int cpu)
{
	struct cpufreq_alucard_policyinfo *ppol = per_cpu(polinfo, cpu);
	struct cpufreq_alucard_cpuinfo *pcpu;
	u64 expires = round_to_nw_start(ppol->last_evaluated_jiffy, tunables);
	unsigned long flags;
	int i;

	spin_lock_irqsave(&ppol->load_lock, flags);
	ppol->policy_timer.expires = expires;
	add_timer(&ppol->policy_timer);
	if (tunables->timer_slack_val >= 0 &&
	    ppol->target_freq > ppol->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_slack_val);
		ppol->policy_slack_timer.expires = expires;
		add_timer(&ppol->policy_slack_timer);
	}

	for_each_cpu(i, ppol->policy->cpus) {
		pcpu = &per_cpu(cpuinfo, i);
		pcpu->time_in_idle =
			get_cpu_idle_time(i, &pcpu->time_in_idle_timestamp,
					  tunables->io_is_busy);
		pcpu->cputime_speedadj = 0;
		pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	}
	spin_unlock_irqrestore(&ppol->load_lock, flags);
}

static u64 update_load(int cpu)
{
	struct cpufreq_alucard_policyinfo *ppol = per_cpu(polinfo, cpu);
	struct cpufreq_alucard_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	struct cpufreq_alucard_tunables *tunables =
		ppol->policy->governor_data;
	u64 now;
	u64 now_idle;
	u64 delta_idle;
	u64 delta_time;
	u64 active_time;

	now_idle = get_cpu_idle_time(cpu, &now, tunables->io_is_busy);
	delta_idle = (now_idle - pcpu->time_in_idle);
	delta_time = (now - pcpu->time_in_idle_timestamp);

	if (delta_time <= delta_idle)
		active_time = 0;
	else
		active_time = delta_time - delta_idle;

	pcpu->cputime_speedadj += active_time * ppol->policy->cur;

	pcpu->time_in_idle = now_idle;
	pcpu->time_in_idle_timestamp = now;
	return now;
}

static unsigned int sl_busy_to_laf(struct cpufreq_alucard_policyinfo *ppol,
				   unsigned long busy)
{
	int prev_load;
	struct cpufreq_alucard_tunables *tunables =
		ppol->policy->governor_data;

	prev_load = mult_frac(ppol->policy->cpuinfo.max_freq * 100,
				busy, tunables->timer_rate);
	return prev_load;
}

static void cpufreq_alucard_timer(unsigned long data)
{
	s64 now;
	unsigned int delta_time;
	u64 cputime_speedadj;
	int cpu_load;
	int pol_load = 0;
	struct cpufreq_alucard_policyinfo *ppol = per_cpu(polinfo, data);
	struct cpufreq_alucard_tunables *tunables =
		ppol->policy->governor_data;
	struct sched_load *sl = ppol->sl;
	struct cpufreq_alucard_cpuinfo *pcpu;
	unsigned int new_freq = 0;
	unsigned int prev_laf = 0, t_prevlaf;
	unsigned int pred_laf = 0, t_predlaf = 0;
	unsigned int index;
	unsigned long flags;
	unsigned long flagsl;
	unsigned long max_cpu;
	unsigned int freq_responsiveness = tunables->freq_responsiveness;
	int pump_inc_step = tunables->pump_inc_step;
	int pump_dec_step = tunables->pump_dec_step;
	unsigned int up_target_sampling_time;
	unsigned int down_target_sampling_time;
	int cpu, i;
	int new_load_pct = 0;
	int prev_l, pred_l = 0;
#if defined(CONFIG_MSM_PERFORMANCE) || defined(CONFIG_SCHED_CORE_CTL)
	struct cpufreq_govinfo govinfo;
#endif

	if (!down_read_trylock(&ppol->enable_sem))
		return;
	if (!ppol->governor_enabled)
		goto exit;

	spin_lock_irqsave(&ppol->target_freq_lock, flags);
	spin_lock(&ppol->load_lock);

	ppol->notif_pending = false;
	ppol->last_evaluated_jiffy = get_jiffies_64();

	if (tunables->use_sched_load)
		sched_get_cpus_busy(sl, ppol->policy->cpus);
	max_cpu = cpumask_first(ppol->policy->cpus);
	i = 0;
	for_each_cpu(cpu, ppol->policy->cpus) {
		pcpu = &per_cpu(cpuinfo, cpu);
		if (tunables->use_sched_load) {
			t_prevlaf = sl_busy_to_laf(ppol, sl[i].prev_load);
			prev_l = t_prevlaf / ppol->target_freq;
			if (tunables->enable_prediction) {
				t_predlaf = sl_busy_to_laf(ppol,
						sl[i].predicted_load);
				pred_l = t_predlaf / ppol->target_freq;
			}
			if (sl[i].prev_load)
				new_load_pct = sl[i].new_task_load * 100 /
							sl[i].prev_load;
			else
				new_load_pct = 0;
		} else {
			now = update_load(cpu);
			delta_time = (unsigned int)
				(now - pcpu->cputime_speedadj_timestamp);
			if (WARN_ON_ONCE(!delta_time))
				continue;
			cputime_speedadj = pcpu->cputime_speedadj;
			do_div(cputime_speedadj, delta_time);
			t_prevlaf = (unsigned int)cputime_speedadj * 100;
			prev_l = t_prevlaf / ppol->target_freq;
		}

		/* find max of load inside policy */
		if (t_prevlaf > prev_laf) {
			prev_laf = t_prevlaf;
			max_cpu = cpu;
		}
		pred_laf = max(t_predlaf, pred_laf);

		cpu_load = max(prev_l, pred_l);
		pol_load = max(pol_load, cpu_load);

		/* save load for notification */
		pcpu->load = pol_load;

		i++;
	}
	spin_unlock(&ppol->load_lock);

	/* CPUs Online Scale Frequency*/
	if (ppol->policy->cur < freq_responsiveness) {
		pump_inc_step = tunables->pump_inc_step_at_min_freq;
		pump_dec_step = tunables->pump_dec_step_at_min_freq;
	}
#ifdef CONFIG_MSM_TRACK_FREQ_TARGET_INDEX
	index = ppol->policy->cur_index;
#else
	index = cpufreq_frequency_table_get_index(ppol->policy, ppol->policy->cur);
	if (index < 0) {
		spin_unlock_irqrestore(&ppol->target_freq_lock, flags);
		goto rearm;
	}
#endif
	spin_lock_irqsave(&tunables->target_sampling_time_lock, flagsl);
	up_target_sampling_time = tunables->up_target_sampling_time[index];
	down_target_sampling_time = tunables->down_target_sampling_time[index];
	spin_unlock_irqrestore(&tunables->target_sampling_time_lock, flagsl);

	if (ppol->up_sampling_time > up_target_sampling_time)
		ppol->up_sampling_time = 1;
	if (ppol->down_sampling_time > down_target_sampling_time)
		ppol->down_sampling_time = 1;
	
	spin_lock_irqsave(&tunables->target_loads_lock, flagsl);
	if (pol_load >= tunables->up_target_loads[index]) {
		if (ppol->up_sampling_time == up_target_sampling_time) {
			for (i = index + 1; i < tunables->ntarget_loads; i++) {
				if (ppol->freq_table[i].frequency == CPUFREQ_ENTRY_INVALID)
					continue;

				new_freq = ppol->freq_table[i].frequency;
				pump_inc_step--;
				if (!pump_inc_step)
					break;
			}
		}
		ppol->up_sampling_time++;
	} else if (pol_load < tunables->down_target_loads[index]) {
		if (ppol->down_sampling_time == down_target_sampling_time) {
			for (i = index - 1; i >= 0; i--) {
				if (ppol->freq_table[i].frequency == CPUFREQ_ENTRY_INVALID)
					continue;

				new_freq = ppol->freq_table[i].frequency;
				pump_dec_step--;
				if (!pump_dec_step)
					break;
			}
		}
		ppol->down_sampling_time++;
	}
	spin_unlock_irqrestore(&tunables->target_loads_lock, flagsl);
	if (!new_freq) {
		spin_unlock_irqrestore(&ppol->target_freq_lock, flags);
		goto rearm;
	}
	if (tunables->freq_responsiveness_jump &&
		pol_load >= tunables->load_responsiveness &&
		new_freq < freq_responsiveness)
		new_freq = freq_responsiveness;

	ppol->target_freq = new_freq;
	spin_unlock_irqrestore(&ppol->target_freq_lock, flags);
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	cpumask_set_cpu(max_cpu, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
	wake_up_process_no_notif(speedchange_task);

rearm:
	if (!timer_pending(&ppol->policy_timer))
		cpufreq_alucard_timer_resched(data, false);

	/*
	 * Send govinfo notification.
	 * Govinfo notification could potentially wake up another thread
	 * managed by its clients. Thread wakeups might trigger a load
	 * change callback that executes this function again. Therefore
	 * no spinlock could be held when sending the notification.
	 */
#if defined(CONFIG_MSM_PERFORMANCE) || defined(CONFIG_SCHED_CORE_CTL)
	for_each_cpu(i, ppol->policy->cpus) {
		pcpu = &per_cpu(cpuinfo, i);
		govinfo.cpu = i;
		govinfo.load = pcpu->load;
		govinfo.sampling_rate_us = tunables->timer_rate;
		atomic_notifier_call_chain(&cpufreq_govinfo_notifier_list,
					   CPUFREQ_LOAD_CHANGE, &govinfo);
	}
#endif

exit:
	up_read(&ppol->enable_sem);
	return;
}

static int cpufreq_alucard_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_alucard_policyinfo *ppol;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&speedchange_cpumask_lock, flags);

		if (cpumask_empty(&speedchange_cpumask)) {
			spin_unlock_irqrestore(&speedchange_cpumask_lock,
					       flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&speedchange_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = speedchange_cpumask;
		cpumask_clear(&speedchange_cpumask);
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			ppol = per_cpu(polinfo, cpu);
			if (!down_read_trylock(&ppol->enable_sem))
				continue;
			if (!ppol->governor_enabled) {
				up_read(&ppol->enable_sem);
				continue;
			}

			if (ppol->target_freq != ppol->policy->cur) {
				__cpufreq_driver_target(ppol->policy,
							ppol->target_freq,
							CPUFREQ_RELATION_H);
				ppol->up_sampling_time = 1;
				ppol->down_sampling_time = 1;
			}
			up_read(&ppol->enable_sem);
		}
	}

	return 0;
}

static int load_change_callback(struct notifier_block *nb, unsigned long val,
				void *data)
{
	unsigned long cpu = (unsigned long) data;
	struct cpufreq_alucard_policyinfo *ppol = per_cpu(polinfo, cpu);
	struct cpufreq_alucard_tunables *tunables;
	unsigned long flags;

	if (!ppol || ppol->reject_notification)
		return 0;

	if (!down_read_trylock(&ppol->enable_sem))
		return 0;
	if (!ppol->governor_enabled)
		goto exit;

	tunables = ppol->policy->governor_data;
	if (!tunables->use_sched_load || !tunables->use_migration_notif)
		goto exit;

	spin_lock_irqsave(&ppol->target_freq_lock, flags);
	ppol->notif_pending = true;
	ppol->notif_cpu = cpu;
	spin_unlock_irqrestore(&ppol->target_freq_lock, flags);

	if (!hrtimer_is_queued(&ppol->notif_timer))
		hrtimer_start(&ppol->notif_timer, ms_to_ktime(1),
			      HRTIMER_MODE_REL);
exit:
	up_read(&ppol->enable_sem);
	return 0;
}

static enum hrtimer_restart cpufreq_alucard_hrtimer(struct hrtimer *timer)
{
	struct cpufreq_alucard_policyinfo *ppol = container_of(timer,
			struct cpufreq_alucard_policyinfo, notif_timer);
	int cpu;

	if (!down_read_trylock(&ppol->enable_sem))
		return 0;
	if (!ppol->governor_enabled) {
		up_read(&ppol->enable_sem);
		return 0;
	}
	cpu = ppol->notif_cpu;
	del_timer(&ppol->policy_timer);
	del_timer(&ppol->policy_slack_timer);
	cpufreq_alucard_timer(cpu);

	up_read(&ppol->enable_sem);
	return HRTIMER_NORESTART;
}

static struct notifier_block load_notifier_block = {
	.notifier_call = load_change_callback,
};

static int cpufreq_alucard_notifier(
	struct notifier_block *nb, unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpufreq_alucard_policyinfo *ppol;
	int cpu;
	unsigned long flags;

	if (val == CPUFREQ_PRECHANGE) {
		ppol = per_cpu(polinfo, freq->cpu);
		if (!ppol)
			return 0;
		if (!down_read_trylock(&ppol->enable_sem))
			return 0;
		if (!ppol->governor_enabled) {
			up_read(&ppol->enable_sem);
			return 0;
		}

		if (cpumask_first(ppol->policy->cpus) != freq->cpu) {
			up_read(&ppol->enable_sem);
			return 0;
		}
		spin_lock_irqsave(&ppol->load_lock, flags);
		for_each_cpu(cpu, ppol->policy->cpus)
			update_load(cpu);
		spin_unlock_irqrestore(&ppol->load_lock, flags);

		up_read(&ppol->enable_sem);
	}
	return 0;
}

static struct notifier_block cpufreq_notifier_block = {
	.notifier_call = cpufreq_alucard_notifier,
};

/* pump_inc_step_at_min_freq */
static ssize_t show_pump_inc_step_at_min_freq(
	struct cpufreq_alucard_tunables *tunables,
	char *buf)
{
	return sprintf(buf, "%d\n", tunables->pump_inc_step_at_min_freq);
}

static ssize_t store_pump_inc_step_at_min_freq(
	struct cpufreq_alucard_tunables *tunables,
	const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = min(max(1, input), 10);

	if (input == tunables->pump_inc_step_at_min_freq)
		return count;

	tunables->pump_inc_step_at_min_freq = input;

	return count;
}

/* pump_inc_step */
static ssize_t show_pump_inc_step(
	struct cpufreq_alucard_tunables *tunables,
	char *buf)
{
	return sprintf(buf, "%d\n", tunables->pump_inc_step);
}

static ssize_t store_pump_inc_step(
	struct cpufreq_alucard_tunables *tunables,
	const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = min(max(1, input), 10);

	if (input == tunables->pump_inc_step)
		return count;

	tunables->pump_inc_step = input;

	return count;
}

/* pump_dec_step_at_min_freq */
static ssize_t show_pump_dec_step_at_min_freq(
	struct cpufreq_alucard_tunables *tunables,
	char *buf)
{
	return sprintf(buf, "%d\n", tunables->pump_dec_step_at_min_freq);
}

static ssize_t store_pump_dec_step_at_min_freq(
	struct cpufreq_alucard_tunables *tunables,
	const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = min(max(1, input), 10);

	if (input == tunables->pump_dec_step_at_min_freq)
		return count;

	tunables->pump_dec_step_at_min_freq = input;

	return count;
}

/* pump_dec_step */
static ssize_t show_pump_dec_step(
	struct cpufreq_alucard_tunables *tunables,
	char *buf)
{
	return sprintf(buf, "%d\n", tunables->pump_dec_step);
}

static ssize_t store_pump_dec_step(
	struct cpufreq_alucard_tunables *tunables,
	const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = min(max(1, input), 10);

	if (input == tunables->pump_dec_step)
		return count;

	tunables->pump_dec_step = input;

	return count;
}

/* up_target_loads */
static ssize_t show_up_target_loads(
	struct cpufreq_alucard_tunables *tunables,
	char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	if (!tunables->up_target_loads)
		return -EINVAL;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	for (i = 0; i < tunables->ntarget_loads; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->up_target_loads[i],
			       ":");
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	sprintf(buf + ret - 1, "\n");

	return ret;
}

static ssize_t store_up_target_loads(
	struct cpufreq_alucard_tunables *tunables,
	const char *buf, size_t count)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned long flags;

	if (!tunables->up_target_loads)
		return -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, ":")))
		ntokens++;

	if (ntokens != tunables->ntarget_loads)
		return -EINVAL;

	cp = buf;
	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	for (i = 0; i < ntokens; i++) {
		if (sscanf(cp, "%u", &tunables->up_target_loads[i]) != 1) {
			spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
			return -EINVAL;
		} else {
			pr_debug("index[%d], val[%u]\n", i, tunables->up_target_loads[i]);
		}

		cp = strpbrk(cp, ":");
		if (!cp)
			break;
		cp++;
	}
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	return count;
}

/* down_target_loads */
static ssize_t show_down_target_loads(
	struct cpufreq_alucard_tunables *tunables,
	char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	if (!tunables->down_target_loads)
		return -EINVAL;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	for (i = 0; i < tunables->ntarget_loads; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->down_target_loads[i],
			       ":");
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	sprintf(buf + ret - 1, "\n");

	return ret;
}

static ssize_t store_down_target_loads(
	struct cpufreq_alucard_tunables *tunables,
	const char *buf, size_t count)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned long flags;

	if (!tunables->down_target_loads)
		return -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, ":")))
		ntokens++;

	if (ntokens != tunables->ntarget_loads)
		return -EINVAL;

	cp = buf;
	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	for (i = 0; i < ntokens; i++) {
		if (sscanf(cp, "%u", &tunables->down_target_loads[i]) != 1) {
			spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
			return -EINVAL;
		} else {
			pr_debug("index[%d], val[%u]\n", i, tunables->down_target_loads[i]);
		}

		cp = strpbrk(cp, ":");
		if (!cp)
			break;
		cp++;
	}
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	return count;
}

/* up_target_sampling_time */
static ssize_t show_up_target_sampling_time(
	struct cpufreq_alucard_tunables *tunables,
	char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	if (!tunables->up_target_sampling_time)
		return -EINVAL;

	spin_lock_irqsave(&tunables->target_sampling_time_lock, flags);
	for (i = 0; i < tunables->ntarget_sampling_time; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->up_target_sampling_time[i],
			       ":");
	spin_unlock_irqrestore(&tunables->target_sampling_time_lock, flags);

	sprintf(buf + ret - 1, "\n");

	return ret;
}

static ssize_t store_up_target_sampling_time(
	struct cpufreq_alucard_tunables *tunables,
	const char *buf, size_t count)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned long flags;

	if (!tunables->up_target_sampling_time)
		return -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, ":")))
		ntokens++;

	if (ntokens != tunables->ntarget_sampling_time)
		return -EINVAL;

	cp = buf;
	spin_lock_irqsave(&tunables->target_sampling_time_lock, flags);
	for (i = 0; i < ntokens; i++) {
		if (sscanf(cp, "%u", &tunables->up_target_sampling_time[i]) != 1) {
			spin_unlock_irqrestore(&tunables->target_sampling_time_lock, flags);
			return -EINVAL;
		} else {
			pr_debug("index[%d], val[%u]\n", i, tunables->up_target_sampling_time[i]);
		}

		cp = strpbrk(cp, ":");
		if (!cp)
			break;
		cp++;
	}
	spin_unlock_irqrestore(&tunables->target_sampling_time_lock, flags);

	return count;
}

/* down_target_sampling_time */
static ssize_t show_down_target_sampling_time(
	struct cpufreq_alucard_tunables *tunables,
	char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	if (!tunables->down_target_sampling_time)
		return -EINVAL;

	spin_lock_irqsave(&tunables->target_sampling_time_lock, flags);
	for (i = 0; i < tunables->ntarget_sampling_time; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->down_target_sampling_time[i],
			       ":");
	spin_unlock_irqrestore(&tunables->target_sampling_time_lock, flags);

	sprintf(buf + ret - 1, "\n");

	return ret;
}

static ssize_t store_down_target_sampling_time(
	struct cpufreq_alucard_tunables *tunables,
	const char *buf, size_t count)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned long flags;

	if (!tunables->down_target_sampling_time)
		return -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, ":")))
		ntokens++;

	if (ntokens != tunables->ntarget_sampling_time)
		return -EINVAL;

	cp = buf;
	spin_lock_irqsave(&tunables->target_sampling_time_lock, flags);
	for (i = 0; i < ntokens; i++) {
		if (sscanf(cp, "%u", &tunables->down_target_sampling_time[i]) != 1) {
			spin_unlock_irqrestore(&tunables->target_sampling_time_lock, flags);
			return -EINVAL;
		} else {
			pr_debug("index[%d], val[%u]\n", i, tunables->down_target_sampling_time[i]);
		}

		cp = strpbrk(cp, ":");
		if (!cp)
			break;
		cp++;
	}
	spin_unlock_irqrestore(&tunables->target_sampling_time_lock, flags);

	return count;
}

#define show_store_one(file_name)					\
static ssize_t show_##file_name(					\
	struct cpufreq_alucard_tunables *tunables, char *buf)	\
{									\
	return snprintf(buf, PAGE_SIZE, "%u\n", tunables->file_name);	\
}									\
static ssize_t store_##file_name(					\
		struct cpufreq_alucard_tunables *tunables,		\
		const char *buf, size_t count)				\
{									\
	int ret;							\
	unsigned long int val;						\
									\
	ret = kstrtoul(buf, 0, &val);				\
	if (ret < 0)							\
		return ret;						\
	tunables->file_name = val;					\
	return count;							\
}
show_store_one(align_windows);
show_store_one(enable_prediction);
show_store_one(freq_responsiveness);
show_store_one(freq_responsiveness_jump);
show_store_one(load_responsiveness);

static ssize_t show_timer_rate(struct cpufreq_alucard_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%lu\n", tunables->timer_rate);
}

static ssize_t store_timer_rate(struct cpufreq_alucard_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val, val_round;
	struct cpufreq_alucard_tunables *t;
	int cpu;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	val_round = jiffies_to_usecs(usecs_to_jiffies(val));
	if (val != val_round)
		pr_warn("timer_rate not aligned to jiffy. Rounded up to %lu\n",
			val_round);
	tunables->timer_rate = val_round;

	if (!tunables->use_sched_load)
		return count;

	for_each_possible_cpu(cpu) {
		if (!per_cpu(polinfo, cpu))
			continue;
		t = per_cpu(polinfo, cpu)->cached_tunables;
		if (t && t->use_sched_load)
			t->timer_rate = val_round;
	}
	set_window_helper(tunables);

	return count;
}

static ssize_t show_timer_slack(struct cpufreq_alucard_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%d\n", tunables->timer_slack_val);
}

static ssize_t store_timer_slack(struct cpufreq_alucard_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;

	tunables->timer_slack_val = val;
	return count;
}

static ssize_t show_io_is_busy(struct cpufreq_alucard_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%u\n", tunables->io_is_busy);
}

static ssize_t store_io_is_busy(struct cpufreq_alucard_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;
	struct cpufreq_alucard_tunables *t;
	int cpu;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->io_is_busy = val;

	if (!tunables->use_sched_load)
		return count;

	for_each_possible_cpu(cpu) {
		if (!per_cpu(polinfo, cpu))
			continue;
		t = per_cpu(polinfo, cpu)->cached_tunables;
		if (t && t->use_sched_load)
			t->io_is_busy = val;
	}
	sched_set_io_is_busy(val);

	return count;
}

static int cpufreq_alucard_enable_sched_input(
			struct cpufreq_alucard_tunables *tunables)
{
	int rc = 0, j;
	struct cpufreq_alucard_tunables *t;

	mutex_lock(&sched_lock);

	set_window_count++;
	if (set_window_count > 1) {
		for_each_possible_cpu(j) {
			if (!per_cpu(polinfo, j))
				continue;
			t = per_cpu(polinfo, j)->cached_tunables;
			if (t && t->use_sched_load) {
				tunables->timer_rate = t->timer_rate;
				tunables->io_is_busy = t->io_is_busy;
				break;
			}
		}
	} else {
		rc = set_window_helper(tunables);
		if (rc) {
			pr_err("%s: Failed to set sched window\n", __func__);
			set_window_count--;
			goto out;
		}
		sched_set_io_is_busy(tunables->io_is_busy);
	}

	if (!tunables->use_migration_notif)
		goto out;

	migration_register_count++;
	if (migration_register_count > 1)
		goto out;
	else
		atomic_notifier_chain_register(&load_alert_notifier_head,
						&load_notifier_block);
out:
	mutex_unlock(&sched_lock);
	return rc;
}

static int cpufreq_alucard_disable_sched_input(
			struct cpufreq_alucard_tunables *tunables)
{
	mutex_lock(&sched_lock);

	if (tunables->use_migration_notif) {
		migration_register_count--;
		if (migration_register_count < 1)
			atomic_notifier_chain_unregister(
					&load_alert_notifier_head,
					&load_notifier_block);
	}
	set_window_count--;

	mutex_unlock(&sched_lock);
	return 0;
}

static ssize_t show_use_sched_load(
		struct cpufreq_alucard_tunables *tunables, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", tunables->use_sched_load);
}

static ssize_t store_use_sched_load(
			struct cpufreq_alucard_tunables *tunables,
			const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (tunables->use_sched_load == (bool) val)
		return count;

	tunables->use_sched_load = val;

	if (val)
		ret = cpufreq_alucard_enable_sched_input(tunables);
	else
		ret = cpufreq_alucard_disable_sched_input(tunables);

	if (ret) {
		tunables->use_sched_load = !val;
		return ret;
	}

	return count;
}

static ssize_t show_use_migration_notif(
		struct cpufreq_alucard_tunables *tunables, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
			tunables->use_migration_notif);
}

static ssize_t store_use_migration_notif(
			struct cpufreq_alucard_tunables *tunables,
			const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (tunables->use_migration_notif == (bool) val)
		return count;
	tunables->use_migration_notif = val;

	if (!tunables->use_sched_load)
		return count;

	mutex_lock(&sched_lock);
	if (val) {
		migration_register_count++;
		if (migration_register_count == 1)
			atomic_notifier_chain_register(
					&load_alert_notifier_head,
					&load_notifier_block);
	} else {
		migration_register_count--;
		if (!migration_register_count)
			atomic_notifier_chain_unregister(
					&load_alert_notifier_head,
					&load_notifier_block);
	}
	mutex_unlock(&sched_lock);

	return count;
}

/*
 * Create show/store routines
 * - sys: One governor instance for complete SYSTEM
 * - pol: One governor instance per struct cpufreq_policy
 */
#define show_gov_pol_sys(file_name)					\
static ssize_t show_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return show_##file_name(common_tunables, buf);			\
}									\
									\
static ssize_t show_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, char *buf)				\
{									\
	return show_##file_name(policy->governor_data, buf);		\
}

#define store_gov_pol_sys(file_name)					\
static ssize_t store_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, const char *buf,		\
	size_t count)							\
{									\
	return store_##file_name(common_tunables, buf, count);		\
}									\
									\
static ssize_t store_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, const char *buf, size_t count)		\
{									\
	return store_##file_name(policy->governor_data, buf, count);	\
}

#define show_store_gov_pol_sys(file_name)				\
show_gov_pol_sys(file_name);						\
store_gov_pol_sys(file_name)

show_store_gov_pol_sys(freq_responsiveness);
show_store_gov_pol_sys(freq_responsiveness_jump);
show_store_gov_pol_sys(load_responsiveness);
show_store_gov_pol_sys(pump_inc_step_at_min_freq);
show_store_gov_pol_sys(pump_inc_step);
show_store_gov_pol_sys(pump_dec_step_at_min_freq);
show_store_gov_pol_sys(pump_dec_step);
show_store_gov_pol_sys(up_target_loads);
show_store_gov_pol_sys(down_target_loads);
show_store_gov_pol_sys(up_target_sampling_time);
show_store_gov_pol_sys(down_target_sampling_time);
show_store_gov_pol_sys(timer_rate);
show_store_gov_pol_sys(timer_slack);
show_store_gov_pol_sys(io_is_busy);
show_store_gov_pol_sys(use_sched_load);
show_store_gov_pol_sys(use_migration_notif);
show_store_gov_pol_sys(align_windows);
show_store_gov_pol_sys(enable_prediction);

#define gov_sys_attr_rw(_name)						\
static struct global_attr _name##_gov_sys =				\
__ATTR(_name, 0644, show_##_name##_gov_sys, store_##_name##_gov_sys)

#define gov_pol_attr_rw(_name)						\
static struct freq_attr _name##_gov_pol =				\
__ATTR(_name, 0644, show_##_name##_gov_pol, store_##_name##_gov_pol)

#define gov_sys_pol_attr_rw(_name)					\
	gov_sys_attr_rw(_name);						\
	gov_pol_attr_rw(_name)

gov_sys_pol_attr_rw(freq_responsiveness);
gov_sys_pol_attr_rw(freq_responsiveness_jump);
gov_sys_pol_attr_rw(load_responsiveness);
gov_sys_pol_attr_rw(pump_inc_step_at_min_freq);
gov_sys_pol_attr_rw(pump_inc_step);
gov_sys_pol_attr_rw(pump_dec_step_at_min_freq);
gov_sys_pol_attr_rw(pump_dec_step);
gov_sys_pol_attr_rw(up_target_loads);
gov_sys_pol_attr_rw(down_target_loads);
gov_sys_pol_attr_rw(up_target_sampling_time);
gov_sys_pol_attr_rw(down_target_sampling_time);
gov_sys_pol_attr_rw(timer_rate);
gov_sys_pol_attr_rw(timer_slack);
gov_sys_pol_attr_rw(io_is_busy);
gov_sys_pol_attr_rw(use_sched_load);
gov_sys_pol_attr_rw(use_migration_notif);
gov_sys_pol_attr_rw(align_windows);
gov_sys_pol_attr_rw(enable_prediction);

/* One Governor instance for entire system */
static struct attribute *alucard_attributes_gov_sys[] = {
	&freq_responsiveness_gov_sys.attr,
	&freq_responsiveness_jump_gov_sys.attr,
	&load_responsiveness_gov_sys.attr,
	&pump_inc_step_at_min_freq_gov_sys.attr,
	&pump_inc_step_gov_sys.attr,
	&pump_dec_step_at_min_freq_gov_sys.attr,
	&pump_dec_step_gov_sys.attr,
	&up_target_loads_gov_sys.attr,
	&down_target_loads_gov_sys.attr,
	&up_target_sampling_time_gov_sys.attr,
	&down_target_sampling_time_gov_sys.attr,
	&timer_rate_gov_sys.attr,
	&timer_slack_gov_sys.attr,
	&io_is_busy_gov_sys.attr,
	&use_sched_load_gov_sys.attr,
	&use_migration_notif_gov_sys.attr,
	&align_windows_gov_sys.attr,
	&enable_prediction_gov_sys.attr,
	NULL,
};

static struct attribute_group alucard_attr_group_gov_sys = {
	.attrs = alucard_attributes_gov_sys,
	.name = "alucard",
};

/* Per policy governor instance */
static struct attribute *alucard_attributes_gov_pol[] = {
	&freq_responsiveness_gov_pol.attr,
	&freq_responsiveness_jump_gov_pol.attr,
	&load_responsiveness_gov_pol.attr,
	&pump_inc_step_at_min_freq_gov_pol.attr,
	&pump_inc_step_gov_pol.attr,
	&pump_dec_step_at_min_freq_gov_pol.attr,
	&pump_dec_step_gov_pol.attr,
	&up_target_loads_gov_pol.attr,
	&down_target_loads_gov_pol.attr,
	&up_target_sampling_time_gov_pol.attr,
	&down_target_sampling_time_gov_pol.attr,
	&timer_rate_gov_pol.attr,
	&timer_slack_gov_pol.attr,
	&io_is_busy_gov_pol.attr,
	&use_sched_load_gov_pol.attr,
	&use_migration_notif_gov_pol.attr,
	&align_windows_gov_pol.attr,
	&enable_prediction_gov_pol.attr,
	NULL,
};

static struct attribute_group alucard_attr_group_gov_pol = {
	.attrs = alucard_attributes_gov_pol,
	.name = "alucard",
};

static struct attribute_group *get_sysfs_attr(void)
{
	if (have_governor_per_policy())
		return &alucard_attr_group_gov_pol;
	else
		return &alucard_attr_group_gov_sys;
}

static void cpufreq_alucard_nop_timer(unsigned long data)
{
}

static struct cpufreq_alucard_tunables *alloc_tunable(
					struct cpufreq_policy *policy)
{
	struct cpufreq_alucard_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables)
		return ERR_PTR(-ENOMEM);

	if (policy->cpu < 4) {
		tunables->freq_responsiveness = FREQ_RESPONSIVENESS_LITTLE;
		tunables->up_target_loads = little_up_target_loads;
		tunables->down_target_loads = little_down_target_loads;
		tunables->ntarget_loads = LITTLE_NFREQS;
		tunables->up_target_sampling_time = little_up_target_sampling_time;
		tunables->down_target_sampling_time = little_down_target_sampling_time;
		tunables->ntarget_sampling_time = LITTLE_NFREQS;
	} else {
		tunables->freq_responsiveness = FREQ_RESPONSIVENESS_BIG;
		tunables->up_target_loads = big_up_target_loads;
		tunables->down_target_loads = big_down_target_loads;
		tunables->ntarget_loads = BIG_NFREQS;
		tunables->up_target_sampling_time = big_up_target_sampling_time;
		tunables->down_target_sampling_time = big_down_target_sampling_time;
		tunables->ntarget_sampling_time = BIG_NFREQS;
	}
	tunables->freq_responsiveness_jump = 1;
	tunables->load_responsiveness = LOAD_RESPONSIVENESS;
	tunables->pump_inc_step_at_min_freq = PUMP_INC_STEP_AT_MIN_FREQ;
	tunables->pump_inc_step = PUMP_INC_STEP;
	tunables->pump_dec_step = PUMP_DEC_STEP;
	tunables->pump_dec_step_at_min_freq = PUMP_DEC_STEP_AT_MIN_FREQ;
	tunables->timer_rate = DEFAULT_TIMER_RATE;
	tunables->timer_slack_val = DEFAULT_TIMER_SLACK;
	tunables->use_sched_load = true;
	tunables->use_migration_notif = true;

	spin_lock_init(&tunables->target_loads_lock);
	spin_lock_init(&tunables->target_sampling_time_lock);

	return tunables;
}

static struct cpufreq_alucard_policyinfo *get_policyinfo(
					struct cpufreq_policy *policy)
{
	struct cpufreq_alucard_policyinfo *ppol =
				per_cpu(polinfo, policy->cpu);
	int i;
	struct sched_load *sl;

	/* polinfo already allocated for policy, return */
	if (ppol)
		return ppol;

	ppol = kzalloc(sizeof(*ppol), GFP_KERNEL);
	if (!ppol)
		return ERR_PTR(-ENOMEM);

	sl = kcalloc(cpumask_weight(policy->related_cpus), sizeof(*sl),
		     GFP_KERNEL);
	if (!sl) {
		kfree(ppol);
		return ERR_PTR(-ENOMEM);
	}
	ppol->sl = sl;

	init_timer_deferrable(&ppol->policy_timer);
	ppol->policy_timer.function = cpufreq_alucard_timer;
	init_timer(&ppol->policy_slack_timer);
	ppol->policy_slack_timer.function = cpufreq_alucard_nop_timer;
	hrtimer_init(&ppol->notif_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ppol->notif_timer.function = cpufreq_alucard_hrtimer;
	spin_lock_init(&ppol->load_lock);
	spin_lock_init(&ppol->target_freq_lock);
	init_rwsem(&ppol->enable_sem);

	for_each_cpu(i, policy->related_cpus)
		per_cpu(polinfo, i) = ppol;
	return ppol;
}

/* This function is not multithread-safe. */
static void free_policyinfo(int cpu)
{
	struct cpufreq_alucard_policyinfo *ppol = per_cpu(polinfo, cpu);
	int j;

	if (!ppol)
		return;

	for_each_possible_cpu(j)
		if (per_cpu(polinfo, j) == ppol)
			per_cpu(polinfo, cpu) = NULL;
	kfree(ppol->cached_tunables);
	kfree(ppol->sl);
	kfree(ppol);
}

static struct cpufreq_alucard_tunables *get_tunables(
				struct cpufreq_alucard_policyinfo *ppol)
{
	if (have_governor_per_policy())
		return ppol->cached_tunables;
	else
		return cached_common_tunables;
}

static int cpufreq_governor_alucard(struct cpufreq_policy *policy,
		unsigned int event)
{
	int rc;
	struct cpufreq_alucard_policyinfo *ppol;
	struct cpufreq_frequency_table *freq_table;
	struct cpufreq_alucard_tunables *tunables;

	if (have_governor_per_policy())
		tunables = policy->governor_data;
	else
		tunables = common_tunables;

	BUG_ON(!tunables && (event != CPUFREQ_GOV_POLICY_INIT));

	switch (event) {
	case CPUFREQ_GOV_POLICY_INIT:
		ppol = get_policyinfo(policy);
		if (IS_ERR(ppol))
			return PTR_ERR(ppol);

		if (have_governor_per_policy()) {
			WARN_ON(tunables);
		} else if (tunables) {
			tunables->usage_count++;
			cpumask_or(&controlled_cpus, &controlled_cpus,
				   policy->related_cpus);
			sched_update_freq_max_load(policy->related_cpus);
			policy->governor_data = tunables;
			return 0;
		}

		tunables = get_tunables(ppol);
		if (!tunables) {
			tunables = alloc_tunable(policy);
			if (IS_ERR(tunables))
				return PTR_ERR(tunables);
		}

		tunables->usage_count = 1;
		policy->governor_data = tunables;
		if (!have_governor_per_policy())
			common_tunables = tunables;

		rc = sysfs_create_group(get_governor_parent_kobj(policy),
				get_sysfs_attr());
		if (rc) {
			kfree(tunables);
			policy->governor_data = NULL;
			if (!have_governor_per_policy())
				common_tunables = NULL;
			return rc;
		}

		if (!policy->governor->initialized)
			cpufreq_register_notifier(&cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);

		if (tunables->use_sched_load)
			cpufreq_alucard_enable_sched_input(tunables);

		cpumask_or(&controlled_cpus, &controlled_cpus,
			   policy->related_cpus);
		sched_update_freq_max_load(policy->related_cpus);

		if (have_governor_per_policy())
			ppol->cached_tunables = tunables;
		else
			cached_common_tunables = tunables;

		break;

	case CPUFREQ_GOV_POLICY_EXIT:
		cpumask_andnot(&controlled_cpus, &controlled_cpus,
			       policy->related_cpus);
		sched_update_freq_max_load(cpu_possible_mask);
		if (!--tunables->usage_count) {
			if (policy->governor->initialized == 1)
				cpufreq_unregister_notifier(&cpufreq_notifier_block,
						CPUFREQ_TRANSITION_NOTIFIER);

			sysfs_remove_group(get_governor_parent_kobj(policy),
					get_sysfs_attr());

			common_tunables = NULL;
		}

		policy->governor_data = NULL;

		if (tunables->use_sched_load)
			cpufreq_alucard_disable_sched_input(tunables);

		break;

	case CPUFREQ_GOV_START:
		mutex_lock(&gov_lock);

		freq_table = cpufreq_frequency_get_table(policy->cpu);

		ppol = per_cpu(polinfo, policy->cpu);
		ppol->policy = policy;
		ppol->target_freq = policy->cur;
		ppol->freq_table = freq_table;
		ppol->p_nolim = *policy;
		ppol->p_nolim.min = policy->cpuinfo.min_freq;
		ppol->p_nolim.max = policy->cpuinfo.max_freq;
		ppol->min_freq = policy->min;
		ppol->up_sampling_time = 1;
		ppol->down_sampling_time = 1;
		ppol->reject_notification = true;
		ppol->notif_pending = false;
		down_write(&ppol->enable_sem);
		del_timer_sync(&ppol->policy_timer);
		del_timer_sync(&ppol->policy_slack_timer);
		ppol->policy_timer.data = policy->cpu;
		ppol->last_evaluated_jiffy = get_jiffies_64();
		cpufreq_alucard_timer_start(tunables, policy->cpu);
		ppol->governor_enabled = 1;
		up_write(&ppol->enable_sem);
		ppol->reject_notification = false;

		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_STOP:
		mutex_lock(&gov_lock);

		ppol = per_cpu(polinfo, policy->cpu);
		ppol->reject_notification = true;
		down_write(&ppol->enable_sem);
		ppol->governor_enabled = 0;
		ppol->target_freq = 0;
		del_timer_sync(&ppol->policy_timer);
		del_timer_sync(&ppol->policy_slack_timer);
		up_write(&ppol->enable_sem);
		ppol->reject_notification = false;

		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_LIMITS:
		ppol = per_cpu(polinfo, policy->cpu);

		__cpufreq_driver_target(policy,
				ppol->target_freq, CPUFREQ_RELATION_L);

		down_read(&ppol->enable_sem);
		if (ppol->governor_enabled) {
			if (policy->min < ppol->min_freq)
				cpufreq_alucard_timer_resched(policy->cpu,
								  true);
			ppol->min_freq = policy->min;
		}
		up_read(&ppol->enable_sem);

		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_ALUCARD
static
#endif
struct cpufreq_governor cpufreq_gov_alucard = {
	.name = "alucard",
	.governor = cpufreq_governor_alucard,
	.max_transition_latency = 10000000,
	.owner = THIS_MODULE,
};

static int __init cpufreq_alucard_init(void)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	spin_lock_init(&speedchange_cpumask_lock);
	mutex_init(&gov_lock);
	mutex_init(&sched_lock);
	speedchange_task =
		kthread_create(cpufreq_alucard_speedchange_task, NULL,
			       "cfalucard");
	if (IS_ERR(speedchange_task))
		return PTR_ERR(speedchange_task);

	sched_setscheduler_nocheck(speedchange_task, SCHED_FIFO, &param);
	get_task_struct(speedchange_task);

	/* NB: wake up so the thread does not look hung to the freezer */
	wake_up_process_no_notif(speedchange_task);

	return cpufreq_register_governor(&cpufreq_gov_alucard);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ALUCARD
fs_initcall(cpufreq_alucard_init);
#else
module_init(cpufreq_alucard_init);
#endif

static void __exit cpufreq_alucard_exit(void)
{
	int cpu;

	cpufreq_unregister_governor(&cpufreq_gov_alucard);
	kthread_stop(speedchange_task);
	put_task_struct(speedchange_task);

	for_each_possible_cpu(cpu)
		free_policyinfo(cpu);
}

module_exit(cpufreq_alucard_exit);

MODULE_AUTHOR("Alucard24 <dmbaoh2@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_alucard' - A dynamic cpufreq governor v6.0");
MODULE_LICENSE("GPLv2");
