/*
 * kernel/trace/latency_hist.c
 *
 * Add support for histograms of preemption-off latency and
 * interrupt-off latency and wakeup latency, it depends on
 * Real-Time Preemption Support.
 *
 *  Copyright (C) 2005 MontaVista Software, Inc.
 *  Yi Yang <yyang@ch.mvista.com>
 *
 *  Converted to work with the new latency tracer.
 *  Copyright (C) 2008 Red Hat, Inc.
 *    Steven Rostedt <srostedt@redhat.com>
 *
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/percpu.h>
#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <asm/atomic.h>
#include <asm/div64.h>

#include "trace.h"
#include <trace/events/sched.h>

#define CREATE_TRACE_POINTS
#include <trace/events/hist.h>

enum {
	IRQSOFF_LATENCY = 0,
	PREEMPTOFF_LATENCY,
	PREEMPTIRQSOFF_LATENCY,
	WAKEUP_LATENCY,
	WAKEUP_LATENCY_SHAREDPRIO,
	MISSED_TIMER_OFFSETS,
	MAX_LATENCY_TYPE,
};

#define MAX_ENTRY_NUM 10240

struct hist_data {
	atomic_t hist_mode; /* 0 log, 1 don't log */
	long offset; /* set it to MAX_ENTRY_NUM/2 for a bipolar scale */
	unsigned long min_lat;
	unsigned long max_lat;
	unsigned long long below_hist_bound_samples;
	unsigned long long above_hist_bound_samples;
	unsigned long long accumulate_lat;
	unsigned long long total_samples;
	unsigned long long hist_array[MAX_ENTRY_NUM];
};

struct enable_data {
	int latency_type;
	int enabled;
};

static char *latency_hist_dir_root = "latency_hist";

#ifdef CONFIG_INTERRUPT_OFF_HIST
static DEFINE_PER_CPU(struct hist_data, irqsoff_hist);
static char *irqsoff_hist_dir = "irqsoff";
static DEFINE_PER_CPU(cycles_t, hist_irqsoff_start);
static DEFINE_PER_CPU(int, hist_irqsoff_counting);
#endif

#ifdef CONFIG_PREEMPT_OFF_HIST
static DEFINE_PER_CPU(struct hist_data, preemptoff_hist);
static char *preemptoff_hist_dir = "preemptoff";
static DEFINE_PER_CPU(cycles_t, hist_preemptoff_start);
static DEFINE_PER_CPU(int, hist_preemptoff_counting);
#endif

#if defined(CONFIG_PREEMPT_OFF_HIST) && defined(CONFIG_INTERRUPT_OFF_HIST)
static DEFINE_PER_CPU(struct hist_data, preemptirqsoff_hist);
static char *preemptirqsoff_hist_dir = "preemptirqsoff";
static DEFINE_PER_CPU(cycles_t, hist_preemptirqsoff_start);
static DEFINE_PER_CPU(int, hist_preemptirqsoff_counting);
#endif

#if defined(CONFIG_PREEMPT_OFF_HIST) || defined(CONFIG_INTERRUPT_OFF_HIST)
static notrace void probe_preemptirqsoff_hist(int reason, int start);
static struct enable_data preemptirqsoff_enabled_data = {
	.latency_type = PREEMPTIRQSOFF_LATENCY,
	.enabled = 0,
};
#endif

#if defined(CONFIG_WAKEUP_LATENCY_HIST) || \
    defined(CONFIG_MISSED_TIMER_OFFSETS_HIST)
struct maxlatproc_data {
	char comm[FIELD_SIZEOF(struct task_struct, comm)];
	unsigned int pid;
	unsigned int prio;
	unsigned long latency;
};
#endif

#ifdef CONFIG_WAKEUP_LATENCY_HIST
static DEFINE_PER_CPU(struct hist_data, wakeup_latency_hist);
static DEFINE_PER_CPU(struct hist_data, wakeup_latency_hist_sharedprio);
static char *wakeup_latency_hist_dir = "wakeup";
static char *wakeup_latency_hist_dir_sharedprio = "sharedprio";
static notrace void probe_wakeup_latency_hist_start(struct rq *rq,
    struct task_struct *p, int success);
static notrace void probe_wakeup_latency_hist_stop(struct rq *rq,
    struct task_struct *prev, struct task_struct *next);
static struct enable_data wakeup_latency_enabled_data = {
	.latency_type = WAKEUP_LATENCY,
	.enabled = 0,
};
static DEFINE_PER_CPU(struct maxlatproc_data, wakeup_maxlatproc);
static DEFINE_PER_CPU(struct maxlatproc_data, wakeup_maxlatproc_sharedprio);
static struct task_struct *wakeup_task;
static int wakeup_sharedprio;
static int wakeup_pid;
#endif

#ifdef CONFIG_MISSED_TIMER_OFFSETS_HIST
static DEFINE_PER_CPU(struct hist_data, missed_timer_offsets);
static char *missed_timer_offsets_dir = "missed_timer_offsets";
static notrace void probe_hrtimer_interrupt(int cpu,
    long long offset, struct task_struct *task);
static struct enable_data missed_timer_offsets_enabled_data = {
	.latency_type = MISSED_TIMER_OFFSETS,
	.enabled = 0,
};
static DEFINE_PER_CPU(struct maxlatproc_data,
    missed_timer_offsets_maxlatproc);
#endif

void notrace latency_hist(int latency_type, int cpu, unsigned long latency,
			  struct task_struct *p)
{
	struct hist_data *my_hist;
#if defined(CONFIG_WAKEUP_LATENCY_HIST) || \
    defined(CONFIG_MISSED_TIMER_OFFSETS_HIST)
	struct maxlatproc_data *mp = NULL;
#endif

	if (cpu < 0 || cpu >= NR_CPUS || latency_type < 0 ||
	    latency_type >= MAX_LATENCY_TYPE)
		return;

	switch (latency_type) {
#ifdef CONFIG_INTERRUPT_OFF_HIST
	case IRQSOFF_LATENCY:
		my_hist = &per_cpu(irqsoff_hist, cpu);
		break;
#endif
#ifdef CONFIG_PREEMPT_OFF_HIST
	case PREEMPTOFF_LATENCY:
		my_hist = &per_cpu(preemptoff_hist, cpu);
		break;
#endif
#if defined(CONFIG_PREEMPT_OFF_HIST) && defined(CONFIG_INTERRUPT_OFF_HIST)
	case PREEMPTIRQSOFF_LATENCY:
		my_hist = &per_cpu(preemptirqsoff_hist, cpu);
		break;
#endif
#ifdef CONFIG_WAKEUP_LATENCY_HIST
	case WAKEUP_LATENCY:
		my_hist = &per_cpu(wakeup_latency_hist, cpu);
		mp = &per_cpu(wakeup_maxlatproc, cpu);
		break;
	case WAKEUP_LATENCY_SHAREDPRIO:
		my_hist = &per_cpu(wakeup_latency_hist_sharedprio, cpu);
		mp = &per_cpu(wakeup_maxlatproc_sharedprio, cpu);
		break;
#endif
#ifdef CONFIG_MISSED_TIMER_OFFSETS_HIST
	case MISSED_TIMER_OFFSETS:
		my_hist = &per_cpu(missed_timer_offsets, cpu);
		mp = &per_cpu(missed_timer_offsets_maxlatproc, cpu);
		break;
#endif
	default:
		return;
	}

	if (atomic_read(&my_hist->hist_mode) == 0)
		return;

	latency += my_hist->offset;

	if (latency < 0 || latency >= MAX_ENTRY_NUM) {
		if (latency < 0)
			my_hist->below_hist_bound_samples++;
		else
			my_hist->above_hist_bound_samples++;
	} else
		my_hist->hist_array[latency]++;

	if (latency < my_hist->min_lat)
		my_hist->min_lat = latency;
	else if (latency > my_hist->max_lat) {
#if defined (CONFIG_WAKEUP_LATENCY_HIST) || \
    defined(CONFIG_MISSED_TIMER_OFFSETS_HIST)
		if (latency_type == WAKEUP_LATENCY ||
		    latency_type == WAKEUP_LATENCY_SHAREDPRIO) {
			strncpy(mp->comm, p->comm, sizeof(mp->comm));
			mp->pid = task_pid_nr(p);
			mp->prio = p->prio;
			mp->latency = latency;
		}
#endif
		my_hist->max_lat = latency;
	}
#ifdef CONFIG_MISSED_TIMER_OFFSETS_HIST
	if (latency_type == MISSED_TIMER_OFFSETS && p != NULL &&
	    latency > mp->latency) {
		strncpy(mp->comm, p->comm, sizeof(mp->comm));
		mp->pid = task_pid_nr(p);
		mp->prio = p->prio;
		mp->latency = latency;
	}
#endif
	my_hist->total_samples++;
	my_hist->accumulate_lat += latency;
	return;
}

static void *l_start(struct seq_file *m, loff_t *pos)
{
	loff_t *index_ptr = kmalloc(sizeof(loff_t), GFP_KERNEL);
	loff_t index = *pos;
	struct hist_data *my_hist = m->private;

	if (!index_ptr)
		return NULL;

	if (index == 0) {
		char minstr[32], avgstr[32], maxstr[32];

		atomic_dec(&my_hist->hist_mode);

		if (likely(my_hist->total_samples)) {
			unsigned long avg = (unsigned long)
			    div64_u64(my_hist->accumulate_lat,
			    my_hist->total_samples);
			snprintf(minstr, sizeof(minstr), "%ld",
			    (long) my_hist->min_lat - my_hist->offset);
			snprintf(avgstr, sizeof(avgstr), "%ld",
			    (long) avg - my_hist->offset);
			snprintf(maxstr, sizeof(minstr), "%ld",
			    (long) my_hist->max_lat - my_hist->offset);
		} else {
			strcpy(minstr, "<undef>");
			strcpy(avgstr, minstr);
			strcpy(maxstr, minstr);
		}

		seq_printf(m, "#Minimum latency: %s microseconds\n"
			   "#Average latency: %s microseconds\n"
			   "#Maximum latency: %s microseconds\n"
			   "#Total samples: %llu\n"
			   "#There are %llu samples lower than %ld"
			   " microseconds.\n"
			   "#There are %llu samples greater or equal"
			   " than %ld microseconds.\n"
			   "#usecs\t%16s\n",
			   minstr, avgstr, maxstr,
			   my_hist->total_samples,
			   my_hist->below_hist_bound_samples,
			   -my_hist->offset,
			   my_hist->above_hist_bound_samples,
			   MAX_ENTRY_NUM - my_hist->offset,
			   "samples");
	}
	if (index >= MAX_ENTRY_NUM)
		return NULL;

	*index_ptr = index;
	return index_ptr;
}

static void *l_next(struct seq_file *m, void *p, loff_t *pos)
{
	loff_t *index_ptr = p;
	struct hist_data *my_hist = m->private;

	if (++*pos >= MAX_ENTRY_NUM) {
		atomic_inc(&my_hist->hist_mode);
		return NULL;
	}
	*index_ptr = *pos;
	return index_ptr;
}

static void l_stop(struct seq_file *m, void *p)
{
	kfree(p);
}

static int l_show(struct seq_file *m, void *p)
{
	int index = *(loff_t *) p;
	struct hist_data *my_hist = m->private;
	char *fmt;

	if (my_hist->offset)
		fmt = "%6d\t%16llu\n";
	else
		fmt = "%5d\t%16llu\n";

	seq_printf(m, fmt, index - my_hist->offset,
	    my_hist->hist_array[index]);
	return 0;
}

static struct seq_operations latency_hist_seq_op = {
	.start = l_start,
	.next  = l_next,
	.stop  = l_stop,
	.show  = l_show
};

static int latency_hist_open(struct inode *inode, struct file *file)
{
	int ret;

	ret = seq_open(file, &latency_hist_seq_op);
	if (!ret) {
		struct seq_file *seq = file->private_data;
		seq->private = inode->i_private;
	}
	return ret;
}

static struct file_operations latency_hist_fops = {
	.open = latency_hist_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static void hist_reset(struct hist_data *hist)
{
	atomic_dec(&hist->hist_mode);

	memset(hist->hist_array, 0, sizeof(hist->hist_array));
	hist->below_hist_bound_samples = 0ULL;
	hist->above_hist_bound_samples = 0ULL;
	hist->min_lat = 0xFFFFFFFFUL;
	hist->max_lat = 0UL;
	hist->total_samples = 0ULL;
	hist->accumulate_lat = 0ULL;

	atomic_inc(&hist->hist_mode);
}

static ssize_t
latency_hist_reset(struct file *file, const char __user *a,
			   size_t size, loff_t *off)
{
	int cpu;
	struct hist_data *hist;
#if defined(CONFIG_WAKEUP_LATENCY_HIST) || \
    defined(CONFIG_MISSED_TIMER_OFFSETS_HIST)
	struct maxlatproc_data *mp = NULL;
#endif
	int latency_type = (int) file->private_data;

	switch (latency_type) {
#ifdef CONFIG_PREEMPT_OFF_HIST
	case PREEMPTOFF_LATENCY:
		for_each_online_cpu(cpu) {
			hist = &per_cpu(preemptoff_hist, cpu);
			hist_reset(hist);
		}
		break;
#endif
#ifdef CONFIG_INTERRUPT_OFF_HIST
	case IRQSOFF_LATENCY:
		for_each_online_cpu(cpu) {
			hist = &per_cpu(irqsoff_hist, cpu);
			hist_reset(hist);
		}
		break;
#endif
#if defined(CONFIG_INTERRUPT_OFF_HIST) && defined(CONFIG_PREEMPT_OFF_HIST)
	case PREEMPTIRQSOFF_LATENCY:
		for_each_online_cpu(cpu) {
			hist = &per_cpu(preemptirqsoff_hist, cpu);
			hist_reset(hist);
		}
		break;
#endif
#ifdef CONFIG_WAKEUP_LATENCY_HIST
	case WAKEUP_LATENCY:
		for_each_online_cpu(cpu) {
			hist = &per_cpu(wakeup_latency_hist, cpu);
			hist_reset(hist);
			mp = &per_cpu(wakeup_maxlatproc, cpu);
			mp->comm[0] = '\0';
			mp->prio = mp->pid = mp->latency = 0;
		}
		break;
	case WAKEUP_LATENCY_SHAREDPRIO:
		for_each_online_cpu(cpu) {
			hist = &per_cpu(wakeup_latency_hist_sharedprio, cpu);
			hist_reset(hist);
			mp = &per_cpu(wakeup_maxlatproc_sharedprio, cpu);
			mp->comm[0] = '\0';
			mp->prio = mp->pid = mp->latency = 0;
		}
		break;
#endif
#ifdef CONFIG_MISSED_TIMER_OFFSETS_HIST
	case MISSED_TIMER_OFFSETS:
		for_each_online_cpu(cpu) {
			hist = &per_cpu(missed_timer_offsets, cpu);
			hist_reset(hist);
			mp = &per_cpu(missed_timer_offsets_maxlatproc, cpu);
			mp->comm[0] = '\0';
			mp->prio = mp->pid = mp->latency = 0;
		}
		break;
#endif
	}

	return size;
}

#ifdef CONFIG_WAKEUP_LATENCY_HIST
static ssize_t
latency_hist_show_pid(struct file *filp, char __user *ubuf,
		      size_t cnt, loff_t *ppos)
{
	char buf[64];
	int r;

	r = snprintf(buf, sizeof(buf), "%u\n", wakeup_pid);
	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

static ssize_t
latency_hist_pid(struct file *filp, const char __user *ubuf,
		 size_t cnt, loff_t *ppos)
{
	char buf[64];
	unsigned long pid;

	if (cnt >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = '\0';

	if (strict_strtoul(buf, 10, &pid))
		return(-EINVAL);

	wakeup_pid = pid;
	return cnt;
}
#endif

#if defined(CONFIG_WAKEUP_LATENCY_HIST) || \
    defined(CONFIG_MISSED_TIMER_OFFSETS_HIST)
static ssize_t
latency_hist_show_maxlatproc(struct file *filp, char __user *ubuf,
		      size_t cnt, loff_t *ppos)
{
	char buf[1024];
	int r;
	struct maxlatproc_data *mp = (struct maxlatproc_data *)
	    filp->private_data;

	r = snprintf(buf, sizeof(buf), "%5d %3d %ld %s\n",
	    mp->pid, mp->prio, mp->latency, mp->comm);
	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

#endif

static ssize_t
latency_hist_show_enable(struct file *filp, char __user *ubuf,
			 size_t cnt, loff_t *ppos)
{
	char buf[64];
	struct enable_data *ed = (struct enable_data *) filp->private_data;
	int r;

	r = snprintf(buf, sizeof(buf), "%d\n", ed->enabled);
	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

static ssize_t
latency_hist_enable(struct file *filp, const char __user *ubuf,
			   size_t cnt, loff_t *ppos)
{
	char buf[64];
	long enable;
	struct enable_data *ed = (struct enable_data *) filp->private_data;

	if (cnt >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;

	if (strict_strtol(buf, 10, &enable))
		return(-EINVAL);

	if ((enable && ed->enabled) || (!enable && !ed->enabled))
		return cnt;

	if (enable) {
		int ret;

		switch (ed->latency_type) {
#if defined(CONFIG_INTERRUPT_OFF_HIST) || defined(CONFIG_PREEMPT_OFF_HIST)
		case PREEMPTIRQSOFF_LATENCY:
			ret = register_trace_preemptirqsoff_hist(
			    probe_preemptirqsoff_hist);
			if (ret) {
				pr_info("wakeup trace: Couldn't assign "
				    "probe_preemptirqsoff_hist "
				    "to trace_preemptirqsoff_hist\n");
				return ret;
			}
			break;
#endif
#ifdef CONFIG_WAKEUP_LATENCY_HIST
		case WAKEUP_LATENCY:
			ret = register_trace_sched_wakeup(
			    probe_wakeup_latency_hist_start);
			if (ret) {
				pr_info("wakeup trace: Couldn't assign "
				    "probe_wakeup_latency_hist_start "
				    "to trace_sched_wakeup\n");
				return ret;
			}
			ret = register_trace_sched_wakeup_new(
			    probe_wakeup_latency_hist_start);
			if (ret) {
				pr_info("wakeup trace: Couldn't assign "
				    "probe_wakeup_latency_hist_start "
				    "to trace_sched_wakeup_new\n");
				unregister_trace_sched_wakeup(
				    probe_wakeup_latency_hist_start);
				return ret;
			}
			ret = register_trace_sched_switch(
			    probe_wakeup_latency_hist_stop);
			if (ret) {
				pr_info("wakeup trace: Couldn't assign "
				    "probe_wakeup_latency_hist_stop "
				    "to trace_sched_switch\n");
				unregister_trace_sched_wakeup(
				    probe_wakeup_latency_hist_start);
				unregister_trace_sched_switch(
				    probe_wakeup_latency_hist_stop);
				return ret;
			}
			break;
#endif
#ifdef CONFIG_MISSED_TIMER_OFFSETS_HIST
		case MISSED_TIMER_OFFSETS:
			ret = register_trace_hrtimer_interrupt(
			    probe_hrtimer_interrupt);
			if (ret) {
				pr_info("wakeup trace: Couldn't assign "
				    "probe_hrtimer_interrupt "
				    "to trace_hrtimer_interrupt\n");
				return ret;
			}
			break;
#endif
		default:
			break;
		}
	} else {
		switch (ed->latency_type) {
#if defined(CONFIG_INTERRUPT_OFF_HIST) || defined(CONFIG_PREEMPT_OFF_HIST)
		case PREEMPTIRQSOFF_LATENCY:
			{
				int cpu;

				unregister_trace_preemptirqsoff_hist(
				    probe_preemptirqsoff_hist);
				for_each_online_cpu(cpu) {
#ifdef CONFIG_INTERRUPT_OFF_HIST
					per_cpu(hist_irqsoff_counting,
					    cpu) = 0;
#endif
#ifdef CONFIG_PREEMPT_OFF_HIST
					per_cpu(hist_preemptoff_counting,
					    cpu) = 0;
#endif
#if defined(CONFIG_INTERRUPT_OFF_HIST) && defined(CONFIG_PREEMPT_OFF_HIST)
					per_cpu(hist_preemptirqsoff_counting,
					    cpu) = 0;
#endif
				}
			}
			break;
#endif
#ifdef CONFIG_WAKEUP_LATEHCY_HIST
		case WAKEUP_LATENCY:
			unregister_trace_sched_wakeup(
			    probe_wakeup_latency_hist_start);
			unregister_trace_sched_wakeup_new(
			    probe_wakeup_latency_hist_start);
			unregister_trace_sched_switch(
			    probe_wakeup_latency_hist_stop);
			wakeup_task = NULL;
			wakeup_sharedprio = 0;
			break;
#endif
#ifdef CONFIG_MISSED_TIMER_OFFSETS_HIST
		case MISSED_TIMER_OFFSETS:
			unregister_trace_hrtimer_interrupt(
			    probe_hrtimer_interrupt);
			break;
#endif
		default:
			break;
		}
	}
	ed->enabled = enable;
	return cnt;
}

static const struct file_operations latency_hist_reset_fops = {
	.open = tracing_open_generic,
	.write = latency_hist_reset,
};

static const struct file_operations latency_hist_enable_fops = {
	.open = tracing_open_generic,
	.read = latency_hist_show_enable,
	.write = latency_hist_enable,
};

#ifdef CONFIG_WAKEUP_LATENCY_HIST
static const struct file_operations latency_hist_pid_fops = {
	.open = tracing_open_generic,
	.read = latency_hist_show_pid,
	.write = latency_hist_pid,
};
#endif

#if defined(CONFIG_WAKEUP_LATENCY_HIST) || \
    defined(CONFIG_MISSED_TIMER_OFFSETS_HIST)
static const struct file_operations latency_hist_maxlatproc_fops = {
	.open = tracing_open_generic,
	.read = latency_hist_show_maxlatproc,
};
#endif

#if defined(CONFIG_INTERRUPT_OFF_HIST) || defined(CONFIG_PREEMPT_OFF_HIST)
notrace void probe_preemptirqsoff_hist(int reason, int starthist)
{
	int cpu = raw_smp_processor_id();
	int time_set = 0;

	if (starthist) {
		cycle_t uninitialized_var(start);

		if (!preempt_count() && !irqs_disabled())
			return;

#ifdef CONFIG_INTERRUPT_OFF_HIST
		if ((reason == IRQS_OFF || reason == TRACE_START) &&
		    !per_cpu(hist_irqsoff_counting, cpu)) {
			per_cpu(hist_irqsoff_counting, cpu) = 1;
			start = ftrace_now(cpu);
			time_set++;
			per_cpu(hist_irqsoff_start, cpu) = start;
		}
#endif

#ifdef CONFIG_PREEMPT_OFF_HIST
		if ((reason == PREEMPT_OFF || reason == TRACE_START) &&
		    !per_cpu(hist_preemptoff_counting, cpu)) {
			per_cpu(hist_preemptoff_counting, cpu) = 1;
			if (!(time_set++))
				start = ftrace_now(cpu);
			per_cpu(hist_preemptoff_start, cpu) = start;
		}
#endif

#if defined(CONFIG_INTERRUPT_OFF_HIST) && defined(CONFIG_PREEMPT_OFF_HIST)
		if (per_cpu(hist_irqsoff_counting, cpu) &&
		    per_cpu(hist_preemptoff_counting, cpu) &&
		    !per_cpu(hist_preemptirqsoff_counting, cpu)) {
			per_cpu(hist_preemptirqsoff_counting, cpu) = 1;
			if (!time_set)
				start = ftrace_now(cpu);
			per_cpu(hist_preemptirqsoff_start, cpu) = start;
		}
#endif
	} else {
		cycle_t uninitialized_var(stop);

#ifdef CONFIG_INTERRUPT_OFF_HIST
		if ((reason == IRQS_ON || reason == TRACE_STOP) &&
		    per_cpu(hist_irqsoff_counting, cpu)) {
			cycle_t start = per_cpu(hist_irqsoff_start, cpu);

			stop = ftrace_now(cpu);
			time_set++;
			if (start && stop >= start) {
				unsigned long latency =
				    nsecs_to_usecs(stop - start);

				latency_hist(IRQSOFF_LATENCY, cpu, latency,
				    NULL);
			}
			per_cpu(hist_irqsoff_counting, cpu) = 0;
		}
#endif

#ifdef CONFIG_PREEMPT_OFF_HIST
		if ((reason == PREEMPT_ON || reason == TRACE_STOP) &&
		    per_cpu(hist_preemptoff_counting, cpu)) {
			cycle_t start = per_cpu(hist_preemptoff_start, cpu);

			if (!(time_set++))
				stop = ftrace_now(cpu);
			if (start && stop >= start) {
				unsigned long latency =
				    nsecs_to_usecs(stop - start);

				latency_hist(PREEMPTOFF_LATENCY, cpu, latency,
				    NULL);
			}
			per_cpu(hist_preemptoff_counting, cpu) = 0;
		}
#endif

#if defined(CONFIG_INTERRUPT_OFF_HIST) && defined(CONFIG_PREEMPT_OFF_HIST)
		if ((!per_cpu(hist_irqsoff_counting, cpu) ||
		     !per_cpu(hist_preemptoff_counting, cpu)) &&
		   per_cpu(hist_preemptirqsoff_counting, cpu)) {
			cycle_t start = per_cpu(hist_preemptirqsoff_start, cpu);

			if (!time_set)
				stop = ftrace_now(cpu);
			if (start && stop >= start) {
				unsigned long latency =
				    nsecs_to_usecs(stop - start);
				latency_hist(PREEMPTIRQSOFF_LATENCY, cpu,
				    latency, NULL);
			}
			per_cpu(hist_preemptirqsoff_counting, cpu) = 0;
		}
#endif
	}
}
#endif

#ifdef CONFIG_WAKEUP_LATENCY_HIST
static DEFINE_ATOMIC_SPINLOCK(wakeup_lock);

notrace void probe_wakeup_latency_hist_start(struct rq *rq,
    struct task_struct *p, int success)
{
	unsigned long flags;
	struct task_struct *curr = rq_curr(rq);

	if (wakeup_pid) {
		if ((wakeup_task && p->prio == wakeup_task->prio) ||
		    p->prio == curr->prio)
			wakeup_sharedprio = 1;
		if (likely(wakeup_pid != task_pid_nr(p)))
			return;
	} else {
		if (likely(!rt_task(p)) ||
		    (wakeup_task && p->prio > wakeup_task->prio) ||
		    p->prio > curr->prio)
			return;
		if ((wakeup_task && p->prio == wakeup_task->prio) ||
		    p->prio == curr->prio)
			wakeup_sharedprio = 1;
	}

	atomic_spin_lock_irqsave(&wakeup_lock, flags);
	if (wakeup_task)
		put_task_struct(wakeup_task);
	get_task_struct(p);
	wakeup_task = p;
	wakeup_task->preempt_timestamp_hist =
	    ftrace_now(raw_smp_processor_id());
	atomic_spin_unlock_irqrestore(&wakeup_lock, flags);
}

notrace void probe_wakeup_latency_hist_stop(struct rq *rq,
    struct task_struct *prev, struct task_struct *next)
{
	unsigned long flags;
	int cpu;
	unsigned long latency;
	cycle_t stop;

	if (next != wakeup_task)
		return;

	cpu = raw_smp_processor_id();
	stop = ftrace_now(cpu);

	atomic_spin_lock_irqsave(&wakeup_lock, flags);

	latency = nsecs_to_usecs(stop - next->preempt_timestamp_hist);
	if (next != wakeup_task) {
		if (wakeup_task && next->prio == wakeup_task->prio)
			latency_hist(WAKEUP_LATENCY_SHAREDPRIO, cpu, latency,
			    next);
		goto out;
	}

	if (wakeup_sharedprio) {
		latency_hist(WAKEUP_LATENCY_SHAREDPRIO, cpu, latency, next);
		wakeup_sharedprio = 0;
	} else
		latency_hist(WAKEUP_LATENCY, cpu, latency, next);

	put_task_struct(wakeup_task);
	wakeup_task = NULL;
out:
	atomic_spin_unlock_irqrestore(&wakeup_lock, flags);
}
#endif

#ifdef CONFIG_MISSED_TIMER_OFFSETS_HIST
notrace void probe_hrtimer_interrupt(int cpu, long long latency_ns,
    struct task_struct *task)
{
	if (latency_ns <= 0) {
		unsigned long latency;

		latency = (unsigned long) div_s64(-latency_ns, 1000);

		latency_hist(MISSED_TIMER_OFFSETS, cpu, latency, task);
	}
}
#endif

static __init int latency_hist_init(void)
{
	struct dentry *latency_hist_root = NULL;
	struct dentry *dentry;
#ifdef CONFIG_WAKEUP_LATENCY_HIST
	struct dentry *dentry_sharedprio;
#endif
	struct dentry *entry;
	struct dentry *latency_hist_enable_root;
	int i = 0;
	struct hist_data *my_hist;
	char name[64];
	char *cpufmt = "CPU%d";
#if defined(CONFIG_WAKEUP_LATENCY_HIST) || \
    defined(CONFIG_MISSED_TIMER_OFFSETS_HIST)
	char *cpufmt_maxlatproc = "max_latency-CPU%d";
#endif

	dentry = tracing_init_dentry();

	latency_hist_root =
		debugfs_create_dir(latency_hist_dir_root, dentry);

	latency_hist_enable_root =
		debugfs_create_dir("enable", latency_hist_root);

#ifdef CONFIG_INTERRUPT_OFF_HIST
	dentry = debugfs_create_dir(irqsoff_hist_dir, latency_hist_root);
	for_each_possible_cpu(i) {
		sprintf(name, cpufmt, i);
		entry = debugfs_create_file(name, 0444, dentry,
		    &per_cpu(irqsoff_hist, i), &latency_hist_fops);
		my_hist = &per_cpu(irqsoff_hist, i);
		atomic_set(&my_hist->hist_mode, 1);
		my_hist->min_lat = 0xFFFFFFFFUL;
	}
	entry = debugfs_create_file("reset", 0644, dentry,
	    (void *)IRQSOFF_LATENCY, &latency_hist_reset_fops);
#endif

#ifdef CONFIG_PREEMPT_OFF_HIST
	dentry = debugfs_create_dir(preemptoff_hist_dir,
	    latency_hist_root);
	for_each_possible_cpu(i) {
		sprintf(name, cpufmt, i);
		entry = debugfs_create_file(name, 0444, dentry,
		    &per_cpu(preemptoff_hist, i), &latency_hist_fops);
		my_hist = &per_cpu(preemptoff_hist, i);
		atomic_set(&my_hist->hist_mode, 1);
		my_hist->min_lat = 0xFFFFFFFFUL;
	}
	entry = debugfs_create_file("reset", 0644, dentry,
	    (void *)PREEMPTOFF_LATENCY, &latency_hist_reset_fops);
#endif

#if defined(CONFIG_INTERRUPT_OFF_HIST) && defined(CONFIG_PREEMPT_OFF_HIST)
	dentry = debugfs_create_dir(preemptirqsoff_hist_dir,
	    latency_hist_root);
	for_each_possible_cpu(i) {
		sprintf(name, cpufmt, i);
		entry = debugfs_create_file(name, 0444, dentry,
		    &per_cpu(preemptirqsoff_hist, i), &latency_hist_fops);
		my_hist = &per_cpu(preemptirqsoff_hist, i);
		atomic_set(&my_hist->hist_mode, 1);
		my_hist->min_lat = 0xFFFFFFFFUL;
	}
	entry = debugfs_create_file("reset", 0644, dentry,
	    (void *)PREEMPTIRQSOFF_LATENCY, &latency_hist_reset_fops);
#endif

#if defined(CONFIG_INTERRUPT_OFF_HIST) || defined(CONFIG_PREEMPT_OFF_HIST)
	entry = debugfs_create_file("preemptirqsoff", 0644,
	    latency_hist_enable_root, (void *)&preemptirqsoff_enabled_data,
	    &latency_hist_enable_fops);
#endif

#ifdef CONFIG_WAKEUP_LATENCY_HIST
	dentry = debugfs_create_dir(wakeup_latency_hist_dir,
	    latency_hist_root);
	dentry_sharedprio = debugfs_create_dir(
	    wakeup_latency_hist_dir_sharedprio, dentry);
	for_each_possible_cpu(i) {
		sprintf(name, cpufmt, i);
		entry = debugfs_create_file(name, 0444, dentry,
		    &per_cpu(wakeup_latency_hist, i),
		    &latency_hist_fops);
		my_hist = &per_cpu(wakeup_latency_hist, i);
		atomic_set(&my_hist->hist_mode, 1);
		my_hist->min_lat = 0xFFFFFFFFUL;

		sprintf(name, cpufmt, i);
		entry = debugfs_create_file(name, 0444, dentry_sharedprio,
		    &per_cpu(wakeup_latency_hist_sharedprio, i),
		    &latency_hist_fops);
		my_hist = &per_cpu(wakeup_latency_hist_sharedprio, i);
		atomic_set(&my_hist->hist_mode, 1);
		my_hist->min_lat = 0xFFFFFFFFUL;

		sprintf(name, cpufmt_maxlatproc, i);
		entry = debugfs_create_file(name, 0444, dentry,
		    &per_cpu(wakeup_maxlatproc, i),
		    &latency_hist_maxlatproc_fops);

		sprintf(name, cpufmt_maxlatproc, i);
		entry = debugfs_create_file(name, 0444, dentry_sharedprio,
		    &per_cpu(wakeup_maxlatproc_sharedprio, i),
		    &latency_hist_maxlatproc_fops);
	}
	entry = debugfs_create_file("pid", 0644, dentry,
	    (void *)&wakeup_pid, &latency_hist_pid_fops);
	entry = debugfs_create_file("reset", 0644, dentry,
	    (void *)WAKEUP_LATENCY, &latency_hist_reset_fops);
	entry = debugfs_create_file("reset", 0644, dentry_sharedprio,
	    (void *)WAKEUP_LATENCY_SHAREDPRIO, &latency_hist_reset_fops);
	entry = debugfs_create_file("wakeup", 0644,
	    latency_hist_enable_root, (void *)&wakeup_latency_enabled_data,
	    &latency_hist_enable_fops);
#endif

#ifdef CONFIG_MISSED_TIMER_OFFSETS_HIST
	dentry = debugfs_create_dir(missed_timer_offsets_dir,
	    latency_hist_root);
	for_each_possible_cpu(i) {
		sprintf(name, cpufmt, i);
		entry = debugfs_create_file(name, 0444, dentry,
		    &per_cpu(missed_timer_offsets, i), &latency_hist_fops);
		my_hist = &per_cpu(missed_timer_offsets, i);
		atomic_set(&my_hist->hist_mode, 1);
		my_hist->min_lat = 0xFFFFFFFFUL;

		sprintf(name, cpufmt_maxlatproc, i);
		entry = debugfs_create_file(name, 0444, dentry,
		    &per_cpu(missed_timer_offsets_maxlatproc, i),
		    &latency_hist_maxlatproc_fops);
	}
	entry = debugfs_create_file("reset", 0644, dentry,
	    (void *)MISSED_TIMER_OFFSETS, &latency_hist_reset_fops);
	entry = debugfs_create_file("missed_timer_offsets", 0644,
	    latency_hist_enable_root,
	    (void *)&missed_timer_offsets_enabled_data,
	    &latency_hist_enable_fops);
#endif
	return 0;
}

__initcall(latency_hist_init);
