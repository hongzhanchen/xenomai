/**@file pod.c
 * @brief Real-time pod services.
 * @author Philippe Gerum
 *
 * Copyright (C) 2001-2008 Philippe Gerum <rpm@xenomai.org>.
 * Copyright (C) 2004 The RTAI project <http://www.rtai.org>
 * Copyright (C) 2004 The HYADES project <http://www.hyades-itea.org>
 * Copyright (C) 2005 The Xenomai project <http://www.Xenomai.org>
 *
 * Xenomai is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * @ingroup pod
 */

/**
 * @ingroup nucleus
 * @defgroup pod Real-time pod services.
 *
 * Real-time pod services.
 *@{*/

#include <stdarg.h>
#include <linux/kallsyms.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <nucleus/version.h>
#include <nucleus/pod.h>
#include <nucleus/timer.h>
#include <nucleus/synch.h>
#include <nucleus/heap.h>
#include <nucleus/intr.h>
#include <nucleus/registry.h>
#include <nucleus/clock.h>
#include <nucleus/stat.h>
#include <nucleus/assert.h>
#include <nucleus/select.h>
#include <nucleus/shadow.h>
#include <nucleus/lock.h>
#include <asm/xenomai/thread.h>

xnpod_t nkpod_struct;
EXPORT_SYMBOL_GPL(nkpod_struct);

u_long nklatency;
EXPORT_SYMBOL_GPL(nklatency);

/* Already accounted for in nklatency, kept separately for user information. */
u_long nktimerlat;

cpumask_t nkaffinity = XNPOD_ALL_CPUS;

#ifdef CONFIG_XENO_OPT_DEBUG
struct xnvfile_directory debug_vfroot;
EXPORT_SYMBOL_GPL(debug_vfroot);
#endif

static DECLARE_WAIT_QUEUE_HEAD(nkjoinq);

#ifdef CONFIG_XENO_HW_FPU

static inline void __xnpod_giveup_fpu(struct xnsched *sched,
				      struct xnthread *thread)
{
	if (thread == sched->fpuholder)
		sched->fpuholder = NULL;
}

static inline void __xnpod_release_fpu(struct xnthread *thread)
{
	if (xnthread_test_state(thread, XNFPU)) {
		/*
		 * Force the FPU save, and nullify the
		 * sched->fpuholder pointer, to avoid leaving
		 * fpuholder pointing on the backup area of the
		 * migrated thread.
		 */
		xnarch_save_fpu(xnthread_archtcb(thread));
		thread->sched->fpuholder = NULL;
	}
}

static inline void __xnpod_switch_fpu(struct xnsched *sched)
{
	xnthread_t *curr = sched->curr;

	if (!xnthread_test_state(curr, XNFPU))
		return;

	if (sched->fpuholder != curr) {
		if (sched->fpuholder == NULL ||
		    xnarch_fpu_ptr(xnthread_archtcb(sched->fpuholder)) !=
		    xnarch_fpu_ptr(xnthread_archtcb(curr))) {
			if (sched->fpuholder)
				xnarch_save_fpu(xnthread_archtcb
						(sched->fpuholder));

			xnarch_restore_fpu(xnthread_archtcb(curr));
		} else
			xnarch_enable_fpu(xnthread_archtcb(curr));

		sched->fpuholder = curr;
	} else
		xnarch_enable_fpu(xnthread_archtcb(curr));
}

/* xnpod_switch_fpu() -- Switches to the current thread's FPU context,
   saving the previous one as needed. */

void xnpod_switch_fpu(xnsched_t *sched)
{
	__xnpod_switch_fpu(sched);
}

#else /* !CONFIG_XENO_HW_FPU */

static inline void __xnpod_giveup_fpu(struct xnsched *sched,
				      struct xnthread *thread)
{
}

static inline void __xnpod_release_fpu(struct xnthread *thread)
{
}

static inline void __xnpod_switch_fpu(struct xnsched *sched)
{
}

#endif /* !CONFIG_XENO_HW_FPU */

void xnpod_fatal(const char *format, ...)
{
	static char msg_buf[1024];
	const unsigned nr_cpus = num_online_cpus();
	struct xnthread *thread;
	xnholder_t *holder;
	xnsched_t *sched;
	char pbuf[16];
	xnticks_t now;
	unsigned cpu;
	va_list ap;
	int cprio;
	spl_t s;

	xntrace_panic_freeze();
	ipipe_prepare_panic();

	xnlock_get_irqsave(&nklock, s);

	va_start(ap, format);
	vsnprintf(msg_buf, sizeof(msg_buf), format, ap);
	printk(KERN_ERR "%s", msg_buf);
	va_end(ap);

	if (!xnpod_active_p() || xnpod_fatal_p())
		goto out;

	__setbits(nkpod->status, XNFATAL);
	now = xnclock_read_monotonic();

	printk(KERN_ERR "\n %-3s  %-6s %-8s %-8s %-8s  %s\n",
	       "CPU", "PID", "PRI", "TIMEOUT", "STAT", "NAME");

	for (cpu = 0; cpu < nr_cpus; ++cpu) {
		sched = xnpod_sched_slot(cpu);

		holder = getheadq(&nkpod->threadq);
		while (holder) {
			thread = link2thread(holder, glink);
			holder = nextq(&nkpod->threadq, holder);
			if (thread->sched != sched)
				continue;

			cprio = xnthread_current_priority(thread);
			snprintf(pbuf, sizeof(pbuf), "%3d", cprio);

			printk(KERN_ERR "%c%3u  %-6d %-8s %-8Lu %.8lx  %s\n",
			       thread == sched->curr ? '>' : ' ',
			       cpu,
			       xnthread_host_pid(thread),
			       pbuf,
			       xnthread_get_timeout(thread, now),
			       xnthread_state_flags(thread),
			       xnthread_name(thread));
		}
	}

	printk(KERN_ERR "Master time base: clock=%Lu\n", xnclock_read_raw());
#ifdef CONFIG_SMP
	printk(KERN_ERR "Current CPU: #%d\n", ipipe_processor_id());
#endif
out:
	xnlock_put_irqrestore(&nklock, s);

	show_stack(NULL,NULL);
	xntrace_panic_dump();
	for (;;)
		cpu_relax();
}
EXPORT_SYMBOL_GPL(xnpod_fatal);

void __xnpod_schedule_handler(void) /* hw interrupts off. */
{
	trace_mark(xn_nucleus, sched_remote, MARK_NOARGS);
	xnarch_memory_barrier();
	xnpod_schedule();
}

void xnpod_schedule_deferred(void)
{
	if (xnpod_active_p())
		xnpod_schedule();
}

static void xnpod_flush_heap(xnheap_t *heap,
			     void *extaddr, u_long extsize, void *cookie)
{
	free_pages_exact(extaddr, extsize);
}

/*!
 * \fn int xnpod_init(void)
 * \brief Initialize the core pod.
 *
 * Initializes the core interface pod which can subsequently be used
 * to start real-time activities. Once the core pod is active,
 * real-time skins can be stacked over. There can only be a single
 * core pod active in the host environment.
 *
 * @return 0 is returned on success. Otherwise:
 *
 * - -ENOMEM is returned if the memory manager fails to initialize.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization code
 */

int xnpod_init(void)
{
	extern int xeno_nucleus_status;
	int cpu, nr_cpus = num_online_cpus();
	struct xnsched *sched;
	struct xnpod *pod;
	void *heapaddr;
	int ret;
	spl_t s;

	if (xeno_nucleus_status < 0)
		/* xeno_nucleus module failed to load properly, bail out. */
		return xeno_nucleus_status;

	xnlock_get_irqsave(&nklock, s);

	pod = &nkpod_struct;
	if (pod->refcnt > 0) {
		/*
		 * Another skin has initialized the global pod
		 * already; just increment the reference count.
		 */
		++nkpod->refcnt;
		xnlock_put_irqrestore(&nklock, s);
		return 0;
	}

	pod->status = 0;
	pod->refcnt = 1;
	initq(&pod->threadq);
	initq(&pod->tstartq);
	initq(&pod->tswitchq);
	initq(&pod->tdeleteq);
	xnarch_atomic_set(&pod->timerlck, 0);

	xnlock_put_irqrestore(&nklock, s);

	heapaddr = alloc_pages_exact(CONFIG_XENO_OPT_SYS_HEAPSZ * 1024, GFP_KERNEL);
	if (heapaddr == NULL ||
	    xnheap_init(&kheap, heapaddr, CONFIG_XENO_OPT_SYS_HEAPSZ * 1024,
			XNHEAP_PAGE_SIZE) != 0) {
		return -ENOMEM;
	}
	xnheap_set_label(&kheap, "main heap");

	for (cpu = 0; cpu < nr_cpus; ++cpu) {
		sched = &pod->sched[cpu];
		xnsched_init(sched, cpu);
		if (xnarch_cpu_supported(cpu))
			appendq(&pod->threadq, &sched->rootcb.glink);
	}

#ifdef CONFIG_SMP
	ipipe_request_irq(&xnarch_machdata.domain, IPIPE_RESCHEDULE_IPI,
			  (ipipe_irq_handler_t)__xnpod_schedule_handler,
			  NULL, NULL);
#endif

	xnregistry_init();

	__setbits(pod->status, XNPEXEC);
	xnarch_memory_barrier();
	xnshadow_grab_events();

	ret = xnpod_enable_timesource();
	if (ret) {
		xnpod_shutdown(XNPOD_FATAL_EXIT);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(xnpod_init);

/*!
 * \fn void xnpod_shutdown(int xtype)
 * \brief Shutdown the current pod.
 *
 * Forcibly shutdowns the active pod. All existing nucleus threads
 * (but the root one) are terminated, and the system heap is freed.
 *
 * @param xtype An exit code passed to the host environment who
 * started the nucleus. Zero is always interpreted as a successful
 * return.
 *
 * The nucleus never calls this routine directly. Skins should provide
 * their own shutdown handlers which end up calling xnpod_shutdown()
 * after their own housekeeping chores have been carried out.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 *
 * Rescheduling: never.
 */

void xnpod_shutdown(int xtype)
{
	struct xnholder *h, *nh;
	struct xnthread *thread;
	struct xnsched *sched;
	int cpu;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	if (!xnpod_active_p() || --nkpod->refcnt != 0) {
		xnlock_put_irqrestore(&nklock, s);
		return;	/* No-op */
	}

	xnlock_put_irqrestore(&nklock, s);

	xnpod_disable_timesource();
	xnshadow_release_events();
#ifdef CONFIG_SMP
	ipipe_free_irq(&xnarch_machdata.domain, IPIPE_RESCHEDULE_IPI);
#endif

	xnlock_get_irqsave(&nklock, s);

	nh = getheadq(&nkpod->threadq);
	while ((h = nh) != NULL) {
		nh = nextq(&nkpod->threadq, h);

		thread = link2thread(h, glink);

		if (!xnthread_test_state(thread, XNROOT))
			xnpod_cancel_thread(thread);
	}

	xnpod_schedule();

	__clrbits(nkpod->status, XNPEXEC);

	for (cpu = 0; cpu < num_online_cpus(); cpu++) {
		sched = xnpod_sched_slot(cpu);
		xnsched_destroy(sched);
	}

	xnlock_put_irqrestore(&nklock, s);

	xnregistry_cleanup();
	xnheap_destroy(&kheap, xnpod_flush_heap, NULL);
}
EXPORT_SYMBOL_GPL(xnpod_shutdown);

void xnpod_fire_callouts(xnqueue_t *hookq, xnthread_t *thread)
{
	/* Must be called with nklock locked, interrupts off. */
	xnsched_t *sched = xnpod_current_sched();
	xnholder_t *holder, *nholder;

	__setbits(sched->status, XNKCOUT);

	/* The callee is allowed to alter the hook queue when running */

	nholder = getheadq(hookq);

	while ((holder = nholder) != NULL) {
		xnhook_t *hook = link2hook(holder);
		nholder = nextq(hookq, holder);
		hook->routine(thread);
	}

	__clrbits(sched->status, XNKCOUT);
}

/*!
 * \fn void xnpod_init_thread(struct xnthread *thread,const struct xnthread_init_attr *attr,struct xnsched_class *sched_class,const union xnsched_policy_param *sched_param)
 * \brief Initialize a new thread.
 *
 * Initializes a new thread. The thread is left dormant until it is
 * actually started by xnpod_start_thread().
 *
 * @param thread The address of a thread descriptor the nucleus will
 * use to store the thread-specific data.  This descriptor must always
 * be valid while the thread is active therefore it must be allocated
 * in permanent memory. @warning Some architectures may require the
 * descriptor to be properly aligned in memory; this is an additional
 * reason for descriptors not to be laid in the program stack where
 * alignement constraints might not always be satisfied.
 *
 * @param attr A pointer to an attribute block describing the initial
 * properties of the new thread. Members of this structure are defined
 * as follows:
 *
 * - name: An ASCII string standing for the symbolic name of the
 * thread. This name is copied to a safe place into the thread
 * descriptor. This name might be used in various situations by the
 * nucleus for issuing human-readable diagnostic messages, so it is
 * usually a good idea to provide a sensible value here.  NULL is fine
 * though and means "anonymous".
 *
 * - flags: A set of creation flags affecting the operation. The
 * following flags can be part of this bitmask, each of them affecting
 * the nucleus behaviour regarding the created thread:
 *
 *   - XNSUSP creates the thread in a suspended state. In such a case,
 * the thread shall be explicitly resumed using the
 * xnpod_resume_thread() service for its execution to actually begin,
 * additionally to issuing xnpod_start_thread() for it. This flag can
 * also be specified when invoking xnpod_start_thread() as a starting
 * mode.
 *
 * - XNUSER shall be set if @a thread will be mapped over an existing
 * user-space task. Otherwise, a new kernel host task is created, then
 * paired with the new Xenomai thread.
 *
 * - XNFPU (enable FPU) tells the nucleus that the new thread may use
 * the floating-point unit. XNFPU is implicitly assumed for user-space
 * threads even if not set in @a flags.
 *
 * - ops: A pointer to a structure defining the class-level operations
 * available for this thread. Fields from this structure must have
 * been set appropriately by the caller.
 *
 * @param sched_class The initial scheduling class the new thread
 * should be assigned to.
 *
 * @param sched_param The initial scheduling parameters to set for the
 * new thread; @a sched_param must be valid within the context of @a
 * sched_class.
 *
 * @return 0 is returned on success. Otherwise, the following error
 * code indicates the cause of the failure:
 *
 * - -EINVAL is returned if @a attr->flags has invalid bits set.
 *
 * Side-effect: This routine does not call the rescheduling procedure.
 *
 * Calling context: This service can be called from secondary mode only.
 *
 * Rescheduling: never.
 */
int xnpod_init_thread(struct xnthread *thread,
		      const struct xnthread_init_attr *attr,
		      struct xnsched_class *sched_class,
		      const union xnsched_policy_param *sched_param)
{
	struct xnsched *sched = xnpod_current_sched();
	spl_t s;
	int ret;

	if (attr->flags & ~(XNFPU | XNUSER | XNSUSP))
		return -EINVAL;

	ret = xnthread_init(thread, attr, sched, sched_class, sched_param);
	if (ret)
		return ret;

	trace_mark(xn_nucleus, thread_init,
		   "thread %p thread_name %s flags %lu class %s prio %d",
		   thread, xnthread_name(thread), attr->flags,
		   sched_class->name, thread->cprio);

	xnlock_get_irqsave(&nklock, s);
	appendq(&nkpod->threadq, &thread->glink);
	xnvfile_touch_tag(&nkpod->threadlist_tag);
	xnlock_put_irqrestore(&nklock, s);

	return 0;
}
EXPORT_SYMBOL_GPL(xnpod_init_thread);

/*!
 * \fn int xnpod_start_thread(struct xnthread *thread,const struct xnthread_start_attr *attr)
 * \brief Initial start of a newly created thread.
 *
 * Starts a (newly) created thread, scheduling it for the first
 * time. This call releases the target thread from the XNDORMANT
 * state. This service also sets the initial mode and interrupt mask
 * for the new thread.
 *
 * @param thread The descriptor address of the affected thread which
 * must have been previously initialized by the xnpod_init_thread()
 * service.
 *
 * @param attr A pointer to an attribute block describing the
 * execution properties of the new thread. Members of this structure
 * are defined as follows:
 *
 * - mode: The initial thread mode. The following flags can
 * be part of this bitmask, each of them affecting the nucleus
 * behaviour regarding the started thread:
 *
 *   - XNLOCK causes the thread to lock the scheduler when it starts.
 * The target thread will have to call the xnpod_unlock_sched()
 * service to unlock the scheduler. A non-preemptible thread may still
 * block, in which case, the lock is reasserted when the thread is
 * scheduled back in.
 *
 *   - XNASDI disables the asynchronous signal handling for this
 * thread.  See xnpod_schedule() for more on this.
 *
 *   - XNSUSP makes the thread start in a suspended state. In such a
 * case, the thread will have to be explicitly resumed using the
 * xnpod_resume_thread() service for its execution to actually begin.
 *
 * - affinity: The processor affinity of this thread. Passing
 * XNPOD_ALL_CPUS or an empty affinity set means "any cpu".
 *
 * - entry: The address of the thread's body routine. In other words,
 * it is the thread entry point.
 *
 * - cookie: A user-defined opaque cookie the nucleus will pass to the
 * emerging thread as the sole argument of its entry point.
 *
 * The START hooks are called on behalf of the calling context (if
 * any).
 *
 * @retval 0 if @a thread could be started ;
 *
 * @retval -EBUSY if @a thread was not dormant or stopped ;
 *
 * @retval -EINVAL if the value of @a attr->affinity is invalid.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: possible.
 */

int xnpod_start_thread(struct xnthread *thread,
		       const struct xnthread_start_attr *attr)
{
	struct xnsched *sched __maybe_unused;
	cpumask_t affinity;
	int ret = 0;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	if (!xnthread_test_state(thread, XNDORMANT)) {
		ret = -EBUSY;
		goto unlock_and_exit;
	}

	if (xnthread_test_state(thread, XNSTARTED)) {
		/* Resuming from a stopped state. */
		xnpod_resume_thread(thread, XNDORMANT);
		goto schedule;
	}

	/* This is our initial start. */

	affinity = attr->affinity;
	cpus_and(affinity, affinity, nkaffinity);
	thread->affinity = *cpu_online_mask;
	cpus_and(thread->affinity, affinity, thread->affinity);

	if (cpus_empty(thread->affinity)) {
		ret = -EINVAL;
		goto unlock_and_exit;
	}
#ifdef CONFIG_SMP
	if (!cpu_isset(xnsched_cpu(thread->sched), thread->affinity)) {
		sched = xnpod_sched_slot(first_cpu(thread->affinity));
		xnsched_migrate_passive(thread, sched);
	}
#endif /* CONFIG_SMP */

	xnthread_set_state(thread, (attr->mode & (XNTHREAD_MODE_BITS | XNSUSP)) | XNSTARTED);
	thread->imode = (attr->mode & XNTHREAD_MODE_BITS);
	thread->entry = attr->entry;
	thread->cookie = attr->cookie;

	trace_mark(xn_nucleus, thread_start, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	xnpod_resume_thread(thread, XNDORMANT);

	xnpod_run_hooks(&nkpod->tstartq, thread, "START");
schedule:
	xnpod_schedule();
unlock_and_exit:
	xnlock_put_irqrestore(&nklock, s);

	return ret;
}
EXPORT_SYMBOL_GPL(xnpod_start_thread);

/*!
 * @internal
 * \fn void __xnpod_reset_thread(struct xnthread *thread)
 * \brief Reset the thread.
 *
 * This internal routine resets the state of a thread so that it can
 * be subsequently stopped or restarted.
 */
static void __xnpod_reset_thread(struct xnthread *thread)
{
	/* Break the thread out of any wait it is currently in. */
	xnpod_unblock_thread(thread);

	/* Release all ownerships held by the thread on synch. objects */
	xnsynch_release_all_ownerships(thread);

	/* If the task has been explicitly suspended, resume it. */
	if (xnthread_test_state(thread, XNSUSP))
		xnpod_resume_thread(thread, XNSUSP);

	/* Reset modebits. */
	xnthread_clear_state(thread, XNTHREAD_MODE_BITS);
	xnthread_set_state(thread, thread->imode);

	/* Reset scheduling class and priority to the initial ones. */
	xnsched_set_policy(thread, thread->init_class,
			   &thread->init_schedparam);

	/* Clear pending signals. */
	thread->signals = 0;

	if (xnthread_test_state(thread, XNLOCK)) {
		xnthread_clear_state(thread, XNLOCK);
		xnthread_lock_count(thread) = 0;
	}
}

/*!
 * \fn void xnpod_stop_thread(xnthread_t *thread)
 *
 * \brief Stop a thread.
 *
 * Stop a previously started thread.  The thread is put back into the
 * dormant state; however, it is not deleted from the system.
 *
 * @param thread The descriptor address of the affected thread which
 * must have been previously started by the xnpod_start_thread()
 * service.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: possible.
 */

void xnpod_stop_thread(struct xnthread *thread)
{
	spl_t s;

	XENO_ASSERT(NUCLEUS, !xnthread_test_state(thread, XNROOT),
		    xnpod_fatal("attempt to stop the root thread");
		);

	trace_mark(xn_nucleus, thread_stop, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	xnlock_get_irqsave(&nklock, s);
	if (!xnthread_test_state(thread, XNDORMANT)) {
		__xnpod_reset_thread(thread);
		xnpod_suspend_thread(thread, XNDORMANT,
				     XN_INFINITE, XN_RELATIVE, NULL);
	} /* Otherwise, it is a nop. */
	xnlock_put_irqrestore(&nklock, s);

	xnpod_schedule();
}
EXPORT_SYMBOL_GPL(xnpod_stop_thread);

/*!
 * \fn void xnpod_set_thread_mode(xnthread_t *thread,xnflags_t clrmask,xnflags_t setmask)
 * \brief Change a thread's control mode.
 *
 * Change the control mode of a given thread. The control mode affects
 * the behaviour of the nucleus regarding the specified thread.
 *
 * @param thread The descriptor address of the affected thread.
 *
 * @param clrmask Clears the corresponding bits from the control field
 * before setmask is applied. The scheduler lock held by the current
 * thread can be forcibly released by passing the XNLOCK bit in this
 * mask. In this case, the lock nesting count is also reset to zero.
 *
 * @param setmask The new thread mode. The following flags can be part
 * of this bitmask, each of them affecting the nucleus behaviour
 * regarding the thread:
 *
 * - XNLOCK causes the thread to lock the scheduler.  The target
 * thread will have to call the xnpod_unlock_sched() service to unlock
 * the scheduler or clear the XNLOCK bit forcibly using this
 * service. A non-preemptible thread may still block, in which case,
 * the lock is reasserted when the thread is scheduled back in.
 *
 * - XNASDI disables the asynchronous signal handling for this thread.
 * See xnpod_schedule() for more on this.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel-based task
 * - User-space task in primary mode.
 *
 * Rescheduling: never, therefore, the caller should reschedule if
 * XNLOCK has been passed into @a clrmask.
 */

xnflags_t xnpod_set_thread_mode(xnthread_t *thread,
				xnflags_t clrmask, xnflags_t setmask)
{
	xnthread_t *curr = xnpod_current_thread();
	xnflags_t oldmode;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus, thread_setmode,
		   "thread %p thread_name %s clrmask %lu setmask %lu",
		   thread, xnthread_name(thread), clrmask, setmask);

	oldmode = xnthread_state_flags(thread) & XNTHREAD_MODE_BITS;
	xnthread_clear_state(thread, clrmask & XNTHREAD_MODE_BITS);
	xnthread_set_state(thread, setmask & XNTHREAD_MODE_BITS);

	if (curr == thread) {
		if (!(oldmode & XNLOCK)) {
			if (xnthread_test_state(thread, XNLOCK))
				/* Actually grab the scheduler lock. */
				xnpod_lock_sched();
		} else if (!xnthread_test_state(thread, XNLOCK))
			xnthread_lock_count(thread) = 0;
	}

	xnlock_put_irqrestore(&nklock, s);

	return oldmode;
}
EXPORT_SYMBOL_GPL(xnpod_set_thread_mode);

static inline int moving_target(struct xnsched *sched, struct xnthread *thread)
{
	int ret = 0;
#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	/*
	 * When deleting a thread in the course of a context switch or
	 * in flight to another CPU with nklock unlocked on a distant
	 * CPU, do nothing, this case will be caught in
	 * xnsched_finish_unlocked_switch.
	 */
	ret = testbits(sched->status, XNINSW) ||
		xnthread_test_state(thread, XNMIGRATE);
#endif
	return ret;
}

static void cleanup_thread(struct xnthread *thread) /* nklock held, irqs off */
{
	struct xnsched *sched = thread->sched;

	trace_mark(xn_nucleus, thread_cleanup, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	removeq(&nkpod->threadq, &thread->glink);
	xnvfile_touch_tag(&nkpod->threadlist_tag);

	if (xnthread_test_state(thread, XNREADY)) {
		XENO_BUGON(NUCLEUS, xnthread_test_state(thread, XNTHREAD_BLOCK_BITS));
		xnsched_dequeue(thread);
		xnthread_clear_state(thread, XNREADY);
	}

	xntimer_destroy(&thread->rtimer);
	xntimer_destroy(&thread->ptimer);
	xntimer_destroy(&thread->rrbtimer);
	thread->idtag = 0;

	if (thread->selector) {
		xnselector_destroy(thread->selector);
		thread->selector = NULL;
	}

	if (xnthread_test_state(thread, XNPEND))
		xnsynch_forget_sleeper(thread);

	xnthread_set_state(thread, XNZOMBIE);

	xnsynch_release_all_ownerships(thread);

	__xnpod_giveup_fpu(sched, thread);

	if (!moving_target(sched, thread)) {
		xnpod_run_hooks(&nkpod->tdeleteq, thread, "DELETE");

		xnsched_forget(thread);
		/*
		 * Note: the thread control block must remain
		 * available until the user hooks have been called.
		 */
		xnthread_cleanup(thread);

		if (xnthread_test_state(sched->curr, XNROOT))
			xnfreesync();
	}
}

void __xnpod_cleanup_thread(struct xnthread *thread)
{
	spl_t s;

	XENO_BUGON(NUCLEUS, !ipipe_root_p);
	xnlock_get_irqsave(&nklock, s);
	cleanup_thread(thread);
	xnlock_put_irqrestore(&nklock, s);
	wake_up(&nkjoinq);
}

/**
 * @fn void xnpod_testcancel_thread(void)
 *
 * @brief Introduce a thread cancellation point.
 *
 * Terminates the current thread if a cancellation request is pending
 * for it, i.e. if xnpod_cancel_thread() was called.
 *
 * Calling context: This service may be called from all runtime modes.
 *
 * Rescheduling: always in case of cancellation from primary mode.
 */
void xnpod_testcancel_thread(void)
{
	struct xnthread *curr = xnshadow_current();

	if (curr == NULL || !xnthread_test_info(curr, XNCANCELD))
		return;

	if (!xnthread_test_state(curr, XNRELAX))
		xnshadow_relax(0, 0);

	do_exit(0);
}
EXPORT_SYMBOL_GPL(xnpod_testcancel_thread);

/**
 * @fn void xnpod_cancel_thread(struct xnthread *thread)
 *
 * @brief Cancel a thread.
 *
 * Request cancellation of a thread. This service forces @a thread to
 * exit from any blocking call. @a thread will terminate as soon as it
 * reaches a cancellation point. Cancellation points are defined for
 * the following situations:
 *
 * - @a thread self-cancels by a call to xnpod_cancel_thread().
 * - @a thread calls any blocking Xenomai service that would otherwise
 * lead to a suspension in xnpod_suspend_thread().
 * - @a thread resumes from xnpod_suspend_thread().
 * - @a thread invokes a Linux syscall (user-space shadow only).
 * - @a thread receives a Linux signal (user-space shadow only).
 *
 * @param thread The descriptor address of the thread to terminate.
 *
 * Calling context: This service may be called from all runtime modes.
 *
 * Rescheduling: always in case of self-cancellation from primary mode.
 */
void xnpod_cancel_thread(struct xnthread *thread)
{
	spl_t s;

	XENO_ASSERT(NUCLEUS, !xnthread_test_state(thread, XNROOT),
		    xnpod_fatal("attempt to cancel the root thread");
		);

	xnlock_get_irqsave(&nklock, s);

	if (xnthread_test_info(thread, XNCANCELD))
		goto check_self_cancel;

	trace_mark(xn_nucleus, thread_cancel, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	xnthread_set_info(thread, XNCANCELD);

	if (xnthread_test_state(thread, XNDORMANT)) {
		cleanup_thread(thread);
		goto unlock_and_exit;
	}

check_self_cancel:
	if (xnshadow_current_p(thread)) {
		xnlock_put_irqrestore(&nklock, s);
		xnpod_testcancel_thread();
		/* ... won't return ... */
		XENO_BUGON(NUCLEUS, 1);
	}

	__xnshadow_kick(thread);

unlock_and_exit:
	xnlock_put_irqrestore(&nklock, s);
}
EXPORT_SYMBOL_GPL(xnpod_cancel_thread);

/**
 * @fn void xnpod_join_thread(struct xnthread *thread)
 *
 * @brief Join with a terminated thread.
 *
 * This service waits for @a thread to terminate after a call to
 * xnpod_cancel_thread().  If that thread has already terminated or is
 * dormant at the time of the call, then xnpod_join_thread() returns
 * immediately.
 *
 * @param thread The descriptor address of the thread to join with.
 *
 * Calling context: This service may be called from secondary mode
 * only.
 *
 * Rescheduling: always if @a thread did not terminate yet at the time
 * of the call.
 */
void xnpod_join_thread(struct xnthread *thread)
{
	unsigned int tag;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	tag = thread->idtag;
	if (xnthread_test_info(thread, XNDORMANT) || tag == 0) {
		xnlock_put_irqrestore(&nklock, s);
		return;
	}

	xnlock_put_irqrestore(&nklock, s);

	trace_mark(xn_nucleus, thread_join, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	/*
	 * Only a very few threads are likely to terminate within a
	 * short time frame at any point in time, so experiencing a
	 * thundering herd effect due to synchronizing on a single
	 * wait queue is quite unlikely. In any case, we run in
	 * secondary mode.
	 */
	wait_event(nkjoinq, thread->idtag != tag);
}
EXPORT_SYMBOL_GPL(xnpod_join_thread);

/*!
 * \fn void xnpod_suspend_thread(xnthread_t *thread, xnflags_t mask,
 *                               xnticks_t timeout, xntmode_t timeout_mode,
 *                               xnsynch_t *wchan)
 *
 * \brief Suspend a thread.
 *
 * Suspends the execution of a thread according to a given suspensive
 * condition. This thread will not be eligible for scheduling until it
 * all the pending suspensive conditions set by this service are
 * removed by one or more calls to xnpod_resume_thread().
 *
 * @param thread The descriptor address of the suspended thread.
 *
 * @param mask The suspension mask specifying the suspensive condition
 * to add to the thread's wait mask. Possible values usable by the
 * caller are:
 *
 * - XNSUSP. This flag forcibly suspends a thread, regardless of any
 * resource to wait for. A reverse call to xnpod_resume_thread()
 * specifying the XNSUSP bit must be issued to remove this condition,
 * which is cumulative with other suspension bits.@a wchan should be
 * NULL when using this suspending mode.
 *
 * - XNDELAY. This flags denotes a counted delay wait (in ticks) which
 * duration is defined by the value of the timeout parameter.
 *
 * - XNPEND. This flag denotes a wait for a synchronization object to
 * be signaled. The wchan argument must points to this object. A
 * timeout value can be passed to bound the wait. This suspending mode
 * should not be used directly by the client interface, but rather
 * through the xnsynch_sleep_on() call.
 *
 * @param timeout The timeout which may be used to limit the time the
 * thread pends on a resource. This value is a wait time given in
 * nanoseconds. It can either be relative, absolute monotonic, or
 * absolute adjustable depending on @a timeout_mode. Passing XN_INFINITE
 * @b and setting @a timeout_mode to XN_RELATIVE specifies an unbounded
 * wait. All other values are used to initialize a watchdog timer. If the
 * current operation mode of the system timer is oneshot and @a timeout
 * elapses before xnpod_suspend_thread() has completed, then the target
 * thread will not be suspended, and this routine leads to a null effect.
 *
 * @param timeout_mode The mode of the @a timeout parameter. It can
 * either be set to XN_RELATIVE, XN_ABSOLUTE, or XN_REALTIME (see also
 * xntimer_start()).
 *
 * @param wchan The address of a pended resource. This parameter is
 * used internally by the synchronization object implementation code
 * to specify on which object the suspended thread pends. NULL is a
 * legitimate value when this parameter does not apply to the current
 * suspending mode (e.g. XNSUSP).
 *
 * @note If the target thread is a shadow which has received a
 * Linux-originated signal, then this service immediately exits
 * without suspending the thread, but raises the XNBREAK condition in
 * its information mask.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: possible if the current thread suspends itself.
 */

void xnpod_suspend_thread(xnthread_t *thread, xnflags_t mask,
			  xnticks_t timeout, xntmode_t timeout_mode,
			  xnsynch_t *wchan)
{
	struct xnsched *sched;
	xnflags_t oldstate;
	spl_t s;

	XENO_ASSERT(NUCLEUS, !xnthread_test_state(thread, XNROOT),
		    xnpod_fatal("attempt to suspend root thread %s",
				thread->name);
		);

	XENO_ASSERT(NUCLEUS, wchan == NULL || thread->wchan == NULL,
		    xnpod_fatal("thread %s attempts a conjunctive wait",
				thread->name);
		);

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus, thread_suspend,
		   "thread %p thread_name %s mask %lu timeout %Lu "
		   "timeout_mode %d wchan %p",
		   thread, xnthread_name(thread), mask, timeout,
		   timeout_mode, wchan);

	sched = thread->sched;
	oldstate = thread->state;

	if (thread == sched->curr)
		xnsched_set_resched(sched);

	/* Is the thread ready to run? */
	if (!xnthread_test_state(thread, XNTHREAD_BLOCK_BITS)) {
		/*
		 * If attempting to suspend a runnable (shadow) thread
		 * which is pending a forced switch to secondary mode,
		 * just raise the break condition and return
		 * immediately.  We may end up suspending a kicked
		 * thread that has been preempted on its relaxing
		 * path, which is a perfectly valid situation: we just
		 * ignore the signal notification in primary mode, and
		 * rely on the wakeup call pending for that task in
		 * the root context, to collect and act upon the
		 * pending Linux signal.
		 */
		if ((mask & XNRELAX) == 0 &&
		    xnthread_test_info(thread, XNKICKED)) {
			if (wchan) {
				thread->wchan = wchan;
				xnsynch_forget_sleeper(thread);
			}
			xnthread_clear_info(thread, XNRMID | XNTIMEO);
			xnthread_set_info(thread, XNBREAK);
			xnlock_put_irqrestore(&nklock, s);
			xnpod_testcancel_thread();
			return;
		}
		xnthread_clear_info(thread, XNRMID | XNTIMEO | XNBREAK | XNWAKEN | XNROBBED);
	}

	/*
	 * Don't start the timer for a thread delayed indefinitely.
	 */
	if (timeout != XN_INFINITE || timeout_mode != XN_RELATIVE) {
		xntimer_set_sched(&thread->rtimer, thread->sched);
		if (xntimer_start(&thread->rtimer, timeout, XN_INFINITE,
				  timeout_mode)) {
			/* (absolute) timeout value in the past, bail out. */
			if (wchan) {
				thread->wchan = wchan;
				xnsynch_forget_sleeper(thread);
			}
			xnthread_set_info(thread, XNTIMEO);
			goto unlock_and_exit;
		}
		xnthread_set_state(thread, XNDELAY);
	}

	if (xnthread_test_state(thread, XNREADY)) {
		xnsched_dequeue(thread);
		xnthread_clear_state(thread, XNREADY);
	}

	xnthread_set_state(thread, mask);

	/*
	 * We must make sure that we don't clear the wait channel if a
	 * thread is first blocked (wchan != NULL) then forcibly
	 * suspended (wchan == NULL), since these are conjunctive
	 * conditions.
	 */
	if (wchan)
		thread->wchan = wchan;

	if (thread == sched->curr) {
		__clrbits(sched->lflags, XNINLOCK);
		/*
		 * If the current thread is being relaxed, we must
		 * have been called from xnshadow_relax(), in which
		 * case we introduce an opportunity for interrupt
		 * delivery right before switching context, which
		 * shortens the uninterruptible code path.
		 *
		 * We have to shut irqs off before xnpod_schedule()
		 * though: if an interrupt could preempt us in
		 * __xnpod_schedule right after the call to
		 * xnarch_escalate but before we lock the nklock, we
		 * would enter the critical section in xnpod_schedule
		 * while running in secondary mode, which would defeat
		 * the purpose of xnarch_escalate().
		 */
		if (mask & XNRELAX) {
			xnlock_clear_irqon(&nklock);
			splmax();
			xnpod_schedule();
			return;
		}
		/*
		 * If the thread is runnning on another CPU,
		 * xnpod_schedule will trigger the IPI as needed.
		 */
		xnpod_schedule();

		if (xnthread_test_info(thread, XNCANCELD)) {
			xnlock_put_irqrestore(&nklock, s);
			xnpod_testcancel_thread();
			/* ... won't return ... */
			XENO_BUGON(NUCLEUS, 1);
		}
		goto unlock_and_exit;
	}

	/*
	 * Ok, this one is an interesting corner case, which requires
	 * a bit of background first. Here, we handle the case of
	 * suspending a _relaxed_ user shadow which is _not_ the
	 * current thread.  The net effect is that we are attempting
	 * to stop the shadow thread at the nucleus level, whilst this
	 * thread is actually running some code under the control of
	 * the Linux scheduler (i.e. it's relaxed).  To make this
	 * possible, we force the target Linux task to migrate back to
	 * the Xenomai domain by sending it a SIGSHADOW signal the
	 * skin interface libraries trap for this specific internal
	 * purpose, whose handler is expected to call back the
	 * nucleus's migration service. By forcing this migration, we
	 * make sure that the real-time nucleus controls, hence
	 * properly stops, the target thread according to the
	 * requested suspension condition. Otherwise, the shadow
	 * thread in secondary mode would just keep running into the
	 * Linux domain, thus breaking the most common assumptions
	 * regarding suspended threads. We only care for threads that
	 * are not current, and for XNSUSP, XNDELAY, XNDORMANT and
	 * XNHELD conditions, because:
	 *
	 * - skins are supposed to ask for primary mode switch when
	 * processing any syscall which may block the caller; IOW,
	 * __xn_exec_primary must be set in the mode flags for
	 * those. So there is no need to deal specifically with the
	 * relax+suspend issue when the current thread is about to be
	 * suspended thread, since it can't be relaxed anyway.
	 *
	 * - among all blocking bits (XNTHREAD_BLOCK_BITS), only
	 * XNSUSP, XNDELAY, XNDORMANT and XNHELD may be applied by the
	 * current thread to a non-current thread. XNPEND is always
	 * added by the caller to its own state, XNMIGRATE and XNRELAX
	 * have special semantics escaping this issue.
	 *
	 * We don't signal threads which are in a dormant state, since
	 * they are already suspended by definition.
	 *
	 * XXX: forcing immediate suspension on a relaxed kernel
	 * shadow is not supported.
	 */
	if (((oldstate & (XNTHREAD_BLOCK_BITS|XNUSER)) == (XNRELAX|XNUSER)) &&
	    (mask & (XNDELAY | XNSUSP | XNDORMANT | XNHELD)) != 0)
		xnshadow_send_sig(thread, SIGSHADOW, SIGSHADOW_ACTION_HARDEN);

unlock_and_exit:
	xnlock_put_irqrestore(&nklock, s);
}
EXPORT_SYMBOL_GPL(xnpod_suspend_thread);

/*!
 * \fn void xnpod_resume_thread(struct xnthread *thread,xnflags_t mask)
 * \brief Resume a thread.
 *
 * Resumes the execution of a thread previously suspended by one or
 * more calls to xnpod_suspend_thread(). This call removes a
 * suspensive condition affecting the target thread. When all
 * suspensive conditions are gone, the thread is left in a READY state
 * at which point it becomes eligible anew for scheduling.
 *
 * @param thread The descriptor address of the resumed thread.
 *
 * @param mask The suspension mask specifying the suspensive condition
 * to remove from the thread's wait mask. Possible values usable by
 * the caller are:
 *
 * - XNSUSP. This flag removes the explicit suspension condition. This
 * condition might be additive to the XNPEND condition.
 *
 * - XNDELAY. This flag removes the counted delay wait condition.
 *
 * - XNPEND. This flag removes the resource wait condition. If a
 * watchdog is armed, it is automatically disarmed by this
 * call. Unlike the two previous conditions, only the current thread
 * can set this condition for itself, i.e. no thread can force another
 * one to pend on a resource.
 *
 * When the thread is eventually resumed by one or more calls to
 * xnpod_resume_thread(), the caller of xnpod_suspend_thread() in the
 * awakened thread that suspended itself should check for the
 * following bits in its own information mask to determine what caused
 * its wake up:
 *
 * - XNRMID means that the caller must assume that the pended
 * synchronization object has been destroyed (see xnsynch_flush()).
 *
 * - XNTIMEO means that the delay elapsed, or the watchdog went off
 * before the corresponding synchronization object was signaled.
 *
 * - XNBREAK means that the wait has been forcibly broken by a call to
 * xnpod_unblock_thread().
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

void xnpod_resume_thread(struct xnthread *thread, xnflags_t mask)
{
	struct xnsched *sched;
	xnflags_t oldstate;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus, thread_resume,
		   "thread %p thread_name %s mask %lu",
		   thread, xnthread_name(thread), mask);

	xntrace_pid(xnthread_host_pid(thread), xnthread_current_priority(thread));

	sched = thread->sched;
	oldstate = thread->state;

	if ((oldstate & XNTHREAD_BLOCK_BITS) == 0) {
		if (oldstate & XNREADY)
			xnsched_dequeue(thread);
		goto enqueue;
	}

	/* Clear the specified block bit(s) */
	xnthread_clear_state(thread, mask);

	/*
	 * If XNDELAY was set in the clear mask, xnpod_unblock_thread()
	 * was called for the thread, or a timeout has elapsed. In the
	 * latter case, stopping the timer is a no-op.
	 */
	if (mask & XNDELAY)
		xntimer_stop(&thread->rtimer);

	if (!xnthread_test_state(thread, XNTHREAD_BLOCK_BITS))
		goto clear_wchan;

	if (mask & XNDELAY) {
		mask = xnthread_test_state(thread, XNPEND);
		if (mask == 0)
			goto unlock_and_exit;
		if (thread->wchan)
			xnsynch_forget_sleeper(thread);
		goto recheck_state;
	}

	if (xnthread_test_state(thread, XNDELAY)) {
		if (mask & XNPEND) {
			/*
			 * A resource became available to the thread.
			 * Cancel the watchdog timer.
			 */
			xntimer_stop(&thread->rtimer);
			xnthread_clear_state(thread, XNDELAY);
		}
		goto recheck_state;
	}

	/*
	 * The thread is still suspended, but is no more pending on a
	 * resource.
	 */
	if ((mask & XNPEND) != 0 && thread->wchan)
		xnsynch_forget_sleeper(thread);

	goto unlock_and_exit;

recheck_state:
	if (xnthread_test_state(thread, XNTHREAD_BLOCK_BITS))
		goto unlock_and_exit;

clear_wchan:
	if ((mask & ~XNDELAY) != 0 && thread->wchan != NULL)
		/*
		 * If the thread was actually suspended, clear the
		 * wait channel.  -- this allows requests like
		 * xnpod_suspend_thread(thread,XNDELAY,...)  not to
		 * run the following code when the suspended thread is
		 * woken up while undergoing a simple delay.
		 */
		xnsynch_forget_sleeper(thread);

	if (unlikely((oldstate & mask) & XNHELD)) {
		xnsched_requeue(thread);
		goto ready;
	}
enqueue:
	xnsched_enqueue(thread);
ready:
	xnthread_set_state(thread, XNREADY);
	xnsched_set_resched(sched);
unlock_and_exit:
	xnlock_put_irqrestore(&nklock, s);
}
EXPORT_SYMBOL_GPL(xnpod_resume_thread);

/*!
 * \fn int xnpod_unblock_thread(xnthread_t *thread)
 * \brief Unblock a thread.
 *
 * Breaks the thread out of any wait it is currently in.  This call
 * removes the XNDELAY and XNPEND suspensive conditions previously put
 * by xnpod_suspend_thread() on the target thread. If all suspensive
 * conditions are gone, the thread is left in a READY state at which
 * point it becomes eligible anew for scheduling.
 *
 * @param thread The descriptor address of the unblocked thread.
 *
 * This call neither releases the thread from the XNSUSP, XNRELAX,
 * XNDORMANT or XNHELD suspensive conditions.
 *
 * When the thread resumes execution, the XNBREAK bit is set in the
 * unblocked thread's information mask. Unblocking a non-blocked
 * thread is perfectly harmless.
 *
 * @return non-zero is returned if the thread was actually unblocked
 * from a pending wait state, 0 otherwise.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

int xnpod_unblock_thread(xnthread_t *thread)
{
	int ret = 1;
	spl_t s;

	/*
	 * Attempt to abort an undergoing wait for the given thread.
	 * If this state is due to an alarm that has been armed to
	 * limit the sleeping thread's waiting time while it pends for
	 * a resource, the corresponding XNPEND state will be cleared
	 * by xnpod_resume_thread() in the same move. Otherwise, this
	 * call may abort an undergoing infinite wait for a resource
	 * (if any).
	 */
	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus, thread_unblock,
		   "thread %p thread_name %s state %lu",
		   thread, xnthread_name(thread),
		   xnthread_state_flags(thread));

	if (xnthread_test_state(thread, XNDELAY))
		xnpod_resume_thread(thread, XNDELAY);
	else if (xnthread_test_state(thread, XNPEND))
		xnpod_resume_thread(thread, XNPEND);
	else
		ret = 0;

	/*
	 * We should not clear a previous break state if this service
	 * is called more than once before the target thread actually
	 * resumes, so we only set the bit here and never clear
	 * it. However, we must not raise the XNBREAK bit if the
	 * target thread was already awake at the time of this call,
	 * so that downstream code does not get confused by some
	 * "successful but interrupted syscall" condition. IOW, a
	 * break state raised here must always trigger an error code
	 * downstream, and an already successful syscall cannot be
	 * marked as interrupted.
	 */
	if (ret)
		xnthread_set_info(thread, XNBREAK);

	xnlock_put_irqrestore(&nklock, s);

	return ret;
}
EXPORT_SYMBOL_GPL(xnpod_unblock_thread);

/*!
 * \fn int xnpod_set_thread_schedparam(struct xnthread *thread,struct xnsched_class *sched_class,const union xnsched_policy_param *sched_param)
 * \brief Change the base scheduling parameters of a thread.
 *
 * Changes the base scheduling policy and paramaters of a thread. If
 * the thread is currently blocked, waiting in priority-pending mode
 * (XNSYNCH_PRIO) for a synchronization object to be signaled, the
 * nucleus will attempt to reorder the object's wait queue so that it
 * reflects the new sleeper's priority, unless the XNSYNCH_DREORD flag
 * has been set for the pended object.
 *
 * @param thread The descriptor address of the affected thread.
 *
 * @param sched_class The new scheduling class the thread should be
 * assigned to.
 *
 * @param sched_param The scheduling parameters to set for the thread;
 * @a sched_param must be valid within the context of @a sched_class.
 *
 * It is absolutely required to use this service to change a thread
 * priority, in order to have all the needed housekeeping chores
 * correctly performed. i.e. Do *not* call xnsched_set_policy()
 * directly or worse, change the thread.cprio field by hand in any
 * case.
 *
 * @return 0 is returned on success. Otherwise, a negative error code
 * indicates the cause of a failure that happened in the scheduling
 * class implementation for @a sched_class. Invalid parameters passed
 * into @a sched_param are common causes of error.
 *
 * Side-effects:
 *
 * - This service does not call the rescheduling procedure but may
 * affect the state of the runnable queue for the previous and new
 * scheduling classes.
 *
 * - Assigning the same scheduling class and parameters to a running
 * or ready thread moves it to the end of the runnable queue, thus
 * causing a manual round-robin.
 *
 * - If the thread is a user-space shadow, this call propagates the
 * request to the mated Linux task.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

int xnpod_set_thread_schedparam(struct xnthread *thread,
				struct xnsched_class *sched_class,
				const union xnsched_policy_param *sched_param)
{
	return __xnpod_set_thread_schedparam(thread, sched_class, sched_param, 1);
}
EXPORT_SYMBOL_GPL(xnpod_set_thread_schedparam);

int __xnpod_set_thread_schedparam(struct xnthread *thread,
				  struct xnsched_class *sched_class,
				  const union xnsched_policy_param *sched_param,
				  int propagate)
{
	int old_wprio, new_wprio, ret;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	old_wprio = xnsched_weighted_cprio(thread);

	ret = xnsched_set_policy(thread, sched_class, sched_param);
	if (ret)
		goto unlock_and_exit;

	new_wprio = xnsched_weighted_cprio(thread);

	trace_mark(xn_nucleus, set_thread_schedparam,
		   "thread %p thread_name %s class %s prio %d",
		   thread, xnthread_name(thread),
		   thread->sched_class->name, thread->cprio);
	/*
	 * NOTE: The behaviour changed compared to v2.4.x: we do not
	 * prevent the caller from altering the scheduling parameters
	 * of a thread that currently undergoes a PIP boost
	 * anymore. Rationale: Calling xnpod_set_thread_schedparam()
	 * carelessly with no consideration for resource management is
	 * a bug in essence, and xnpod_set_thread_schedparam() does
	 * not have to paper over it, especially at the cost of more
	 * complexity when dealing with multiple scheduling classes.
	 * In short, callers have to make sure that lowering a thread
	 * priority is safe with respect to what their application
	 * currently does.
	 */
	if (old_wprio != new_wprio && thread->wchan != NULL &&
	    !testbits(thread->wchan->status, XNSYNCH_DREORD))
		/*
		 * Update the pending order of the thread inside its
		 * wait queue, unless this behaviour has been
		 * explicitly disabled for the pended synchronization
		 * object, or the requested (weighted) priority has
		 * not changed, thus preventing spurious round-robin
		 * effects.
		 */
		xnsynch_requeue_sleeper(thread);
	/*
	 * We don't need/want to move the thread at the end of its
	 * priority group whenever:
	 * - it is blocked and thus not runnable;
	 * - it bears the ready bit in which case xnsched_set_policy()
	 * already reordered the runnable queue;
	 * - we currently hold the scheduler lock, so we don't want
	 * any round-robin effect to take place.
	 */
	if (!xnthread_test_state(thread, XNTHREAD_BLOCK_BITS|XNREADY|XNLOCK))
		xnsched_putback(thread);

	if (propagate) {
		if (xnthread_test_state(thread, XNRELAX))
			xnshadow_renice(thread);
		else if (!xnthread_test_state(thread, XNROOT))
			xnthread_set_info(thread, XNPRIOSET);
	}

unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return ret;
}

/**
 * \fn int xnpod_migrate_thread(int cpu)
 *
 * \brief Migrate the current thread.
 *
 * This call makes the current thread migrate to another CPU if its
 * affinity allows it.
 *
 * @param cpu The destination CPU.
 *
 * @retval 0 if the thread could migrate ;
 * @retval -EPERM if the calling context is asynchronous, or the
 * current thread affinity forbids this migration ;
 * @retval -EBUSY if the scheduler is locked.
 */

int xnpod_migrate_thread(int cpu)
{
	struct xnthread *thread;
	struct xnsched *sched;
	int ret = 0;
	spl_t s;

	if (xnpod_asynch_p())
		return -EPERM;

	if (xnpod_locked_p())
		return -EBUSY;

	xnlock_get_irqsave(&nklock, s);

	thread = xnpod_current_thread();

	if (!cpu_isset(cpu, thread->affinity)) {
		ret = -EPERM;
		goto unlock_and_exit;
	}

	if (cpu == ipipe_processor_id())
		goto unlock_and_exit;

	sched = xnpod_sched_slot(cpu);

	trace_mark(xn_nucleus, thread_migrate,
		   "thread %p thread_name %s cpu %d",
		   thread, xnthread_name(thread), cpu);

	__xnpod_release_fpu(thread);

	/* Move to remote scheduler. */
	xnsched_migrate(thread, sched);

	/* Migrate the thread's periodic timer. */
	xntimer_set_sched(&thread->ptimer, sched);

	xnpod_schedule();

	/*
	 * Reset execution time measurement period so that we don't
	 * mess up per-CPU statistics.
	 */
	xnstat_exectime_reset_stats(&thread->stat.lastperiod);

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return ret;
}
EXPORT_SYMBOL_GPL(xnpod_migrate_thread);

/*!
 * @internal
 * \fn void xnpod_dispatch_signals(void)
 * \brief Deliver pending asynchronous signals to the running thread.
 *
 * This internal routine checks for the presence of asynchronous
 * signals directed to the running thread, and attempts to start the
 * asynchronous service routine (ASR) if any. Called with nklock
 * locked, interrupts off.
 */

void xnpod_dispatch_signals(void)
{
	xnthread_t *thread = xnpod_current_thread();
	xnflags_t oldmode;
	xnsigmask_t sigs;
	xnasr_t asr;

	/* Process user-defined signals if the ASR is enabled for this
	   thread. */

	if (thread->signals == 0 || xnthread_test_state(thread, XNASDI)
	    || thread->asr == XNTHREAD_INVALID_ASR)
		return;

	trace_mark(xn_nucleus, sched_sigdispatch, "signals %lu",
		   thread->signals);

	/* Start the asynchronous service routine */
	oldmode = xnthread_test_state(thread, XNTHREAD_MODE_BITS);
	sigs = thread->signals;
	asr = thread->asr;

	/* Clear pending signals mask since an ASR can be reentrant */
	thread->signals = 0;

	xnthread_clear_state(thread, XNTHREAD_MODE_BITS);
	xnthread_set_state(thread, thread->asrmode);
	thread->asrlevel++;
	asr(sigs);
	thread->asrlevel--;
	xnthread_clear_state(thread, XNTHREAD_MODE_BITS);
	xnthread_set_state(thread, oldmode);
}

static inline void xnpod_switch_to(xnsched_t *sched,
				   xnthread_t *prev, xnthread_t *next)
{
#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	sched->last = prev;
	__setbits(sched->status, XNINSW);
	xnlock_clear_irqon(&nklock);
#endif /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */

	xnarch_switch_to(xnthread_archtcb(prev),
			 xnthread_archtcb(next));
}

/*!
 * \fn void xnpod_schedule(void)
 * \brief Rescheduling procedure entry point.
 *
 * This is the central rescheduling routine which should be called to
 * validate and apply changes which have previously been made to the
 * nucleus scheduling state, such as suspending, resuming or changing
 * the priority of threads.  This call first determines if a thread
 * switch should take place, and performs it as
 * needed. xnpod_schedule() schedules out the current thread if:
 *
 * - the current thread is now blocked or deleted.
 * - a runnable thread from a higher priority scheduling class is
 * waiting for the CPU.
 * - the current thread does not lead the runnable threads from its
 * own scheduling class (e.g. round-robin in the RT class).
 *
 * The nucleus implements a lazy rescheduling scheme so that most
 * of the services affecting the threads state MUST be followed by a
 * call to the rescheduling procedure for the new scheduling state to
 * be applied. In other words, multiple changes on the scheduler state
 * can be done in a row, waking threads up, blocking others, without
 * being immediately translated into the corresponding context
 * switches, like it would be necessary would it appear that a higher
 * priority thread than the current one became runnable for
 * instance. When all changes have been applied, the rescheduling
 * procedure is then called to consider those changes, and possibly
 * replace the current thread by another one.
 *
 * As a notable exception to the previous principle however, every
 * action which ends up suspending or deleting the current thread
 * begets an immediate call to the rescheduling procedure on behalf of
 * the service causing the state transition. For instance,
 * self-suspension, self-destruction, or sleeping on a synchronization
 * object automatically leads to a call to the rescheduling procedure,
 * therefore the caller does not need to explicitly issue
 * xnpod_schedule() after such operations.
 *
 * The rescheduling procedure always leads to a null-effect if it is
 * called on behalf of an ISR or callout. Any outstanding scheduler
 * lock held by the outgoing thread will be restored when the thread
 * is scheduled back in.
 *
 * Calling this procedure with no applicable context switch pending is
 * harmless and simply leads to a null-effect.
 *
 * Side-effects:

 * - If an asynchronous service routine exists, the pending
 * asynchronous signals are delivered to a resuming thread or on
 * behalf of the caller before it returns from the procedure if no
 * context switch has taken place. This behaviour can be disabled by
 * setting the XNASDI flag in the thread's status mask by calling
 * xnpod_set_thread_mode().
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine, although this leads to a no-op.
 * - Kernel-based task
 * - User-space task
 *
 * @note The switch hooks are called on behalf of the resuming thread.
 */

static inline int test_resched(struct xnsched *sched)
{
	int resched = testbits(sched->status, XNRESCHED);
#ifdef CONFIG_SMP
	/* Send resched IPI to remote CPU(s). */
	if (unlikely(!cpus_empty(sched->resched))) {
		xnarch_memory_barrier();
		ipipe_send_ipi(IPIPE_RESCHEDULE_IPI, sched->resched);
		cpus_clear(sched->resched);
	}
#else
	resched = xnsched_resched_p(sched);
#endif
	clrbits(sched->status, XNRESCHED);
	return resched;
}

static inline void enter_root(struct xnthread *root)
{
	struct xnarchtcb *rootcb __maybe_unused = xnthread_archtcb(root);

#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	if (rootcb->core.mm == NULL)
		set_ti_thread_flag(rootcb->core.tip, TIF_MMSWITCH_INT);
#endif
	ipipe_unmute_pic();
}

static inline void leave_root(struct xnthread *root)
{
	struct xnarchtcb *rootcb = xnthread_archtcb(root);
	struct task_struct *p = current;

	ipipe_notify_root_preemption();
	ipipe_mute_pic();
	/* Remember the preempted Linux task pointer. */
	rootcb->core.host_task = p;
	rootcb->core.tsp = &p->thread;
	rootcb->core.mm = rootcb->core.active_mm = ipipe_get_active_mm();
#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	rootcb->core.tip = task_thread_info(p);
#endif
	xnarch_leave_root(rootcb);
}

void __xnpod_schedule(struct xnsched *sched)
{
	int zombie, switched, need_resched, shadow;
	struct xnthread *prev, *next, *curr;
	spl_t s;

	if (xnarch_escalate())
		return;

	trace_mark(xn_nucleus, sched, MARK_NOARGS);

	xnlock_get_irqsave(&nklock, s);

	curr = sched->curr;
	xntrace_pid(xnthread_host_pid(curr), xnthread_current_priority(curr));
reschedule:
	switched = 0;
	need_resched = test_resched(sched);
#if !XENO_DEBUG(NUCLEUS)
	if (!need_resched)
		goto signal_unlock_and_exit;
#endif /* !XENO_DEBUG(NUCLEUS) */
	zombie = xnthread_test_state(curr, XNZOMBIE);

	next = xnsched_pick_next(sched);
	if (next == curr) {
		if (unlikely(xnthread_test_state(next, XNROOT))) {
			if (testbits(sched->lflags, XNHTICK))
				xnintr_host_tick(sched);
			if (testbits(sched->lflags, XNHDEFER))
				xntimer_next_local_shot(sched);
		}
		goto signal_unlock_and_exit;
	}

	XENO_BUGON(NUCLEUS, need_resched == 0);

	prev = curr;

	trace_mark(xn_nucleus, sched_switch,
		   "prev %p prev_name %s "
		   "next %p next_name %s",
		   prev, xnthread_name(prev),
		   next, xnthread_name(next));

	if (xnthread_test_state(next, XNROOT)) {
		xnsched_reset_watchdog(sched);
		xnfreesync();
	}

	if (zombie)
		xnsched_zombie_hooks(prev);

	sched->curr = next;
	shadow = 1;

	if (xnthread_test_state(prev, XNROOT)) {
		leave_root(prev);
		shadow = 0;
	} else if (xnthread_test_state(next, XNROOT)) {
		if (testbits(sched->lflags, XNHTICK))
			xnintr_host_tick(sched);
		if (testbits(sched->lflags, XNHDEFER))
			xntimer_next_local_shot(sched);
		enter_root(next);
	}

	xnstat_exectime_switch(sched, &next->stat.account);
	xnstat_counter_inc(&next->stat.csw);

	xnpod_switch_to(sched, prev, next);

	/*
	 * Test whether we transitioned from primary mode to secondary
	 * over a shadow thread. This may happen in two cases:
	 *
	 * 1) the shadow thread just relaxed.
	 * 2) the shadow TCB has just been deleted, in which case
	 * we have to reap the mated Linux side as well.
	 *
	 * In both cases, we are running over the epilogue of Linux's
	 * schedule, and should skip our epilogue code.
	 */
	if (shadow && ipipe_root_p)
		goto shadow_epilogue;

	switched = 1;
	sched = xnsched_finish_unlocked_switch(sched);
	/*
	 * Re-read the currently running thread, this is needed
	 * because of relaxed/hardened transitions.
	 */
	curr = sched->curr;
	xntrace_pid(xnthread_host_pid(curr), xnthread_current_priority(curr));

	if (zombie)
		xnpod_fatal("zombie thread %s (%p) would not die...",
			    prev->name, prev);

	xnsched_finalize_zombie(sched);

	__xnpod_switch_fpu(sched);

	xnpod_run_hooks(&nkpod->tswitchq, curr, "SWITCH");

 signal_unlock_and_exit:
	if (xnthread_signaled_p(curr))
		xnpod_dispatch_signals();

	if (switched &&
	    xnsched_maybe_resched_after_unlocked_switch(sched))
		goto reschedule;

	if (xnthread_lock_count(curr))
		__setbits(sched->lflags, XNINLOCK);

	xnlock_put_irqrestore(&nklock, s);

	return;

shadow_epilogue:

	__ipipe_complete_domain_migration();
	/*
	 * Shadow on entry and root without shadow extension on exit?
	 * Mmmm... This must be the user-space mate of a deleted
	 * real-time shadow we've just rescheduled in the Linux domain
	 * to have it exit properly.  Reap it now.
	 */
	if (xnshadow_current() == NULL) {
		splnone();
		__ipipe_reenter_root();
		do_exit(0);
	}

	/*
	 * Interrupts must be disabled here (has to be done on entry
	 * of the Linux [__]switch_to function), but it is what
	 * callers expect, specifically the reschedule of an IRQ
	 * handler that hit before we call xnpod_schedule in
	 * xnpod_suspend_thread when relaxing a thread.
	 */
	XENO_BUGON(NUCLEUS, !hard_irqs_disabled());
}
EXPORT_SYMBOL_GPL(__xnpod_schedule);

void ___xnpod_lock_sched(xnsched_t *sched)
{
	struct xnthread *curr = sched->curr;

	if (xnthread_lock_count(curr)++ == 0) {
		__setbits(sched->lflags, XNINLOCK);
		xnthread_set_state(curr, XNLOCK);
	}
}
EXPORT_SYMBOL_GPL(___xnpod_lock_sched);

void ___xnpod_unlock_sched(xnsched_t *sched)
{
	struct xnthread *curr = sched->curr;
	XENO_ASSERT(NUCLEUS, xnthread_lock_count(curr) > 0,
		    xnpod_fatal("Unbalanced lock/unlock");
		    );

	if (--xnthread_lock_count(curr) == 0) {
		xnthread_clear_state(curr, XNLOCK);
		__clrbits(sched->lflags, XNINLOCK);
		xnpod_schedule();
	}
}
EXPORT_SYMBOL_GPL(___xnpod_unlock_sched);

/*!
 * \fn int xnpod_add_hook(int type,void (*routine)(xnthread_t *))
 * \brief Install a nucleus hook.
 *
 * The nucleus allows to register user-defined routines which get
 * called whenever a specific scheduling event occurs. Multiple hooks
 * can be chained for a single event type, and get called on a FIFO
 * basis.
 *
 * The scheduling is locked while a hook is executing.
 *
 * @param type Defines the kind of hook to install:
 *
 *        - XNHOOK_THREAD_START: The user-defined routine will be
 *        called on behalf of the starter thread whenever a new thread
 *        starts. The descriptor address of the started thread is
 *        passed to the routine.
 *
 *        - XNHOOK_THREAD_DELETE: The user-defined routine will be
 *        called on behalf of the deletor thread whenever a thread is
 *        deleted. The descriptor address of the deleted thread is
 *        passed to the routine.
 *
 *        - XNHOOK_THREAD_SWITCH: The user-defined routine will be
 *        called on behalf of the resuming thread whenever a context
 *        switch takes place. The descriptor address of the thread
 *        which has been switched out is passed to the routine.
 *
 * @param routine The address of the user-supplied routine to call.
 *
 * @return 0 is returned on success. Otherwise, one of the following
 * error codes indicates the cause of the failure:
 *
 *         - -EINVAL is returned if type is incorrect.
 *
 *         - -ENOMEM is returned if not enough memory is available
 *         from the system heap to add the new hook.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

int xnpod_add_hook(int type, void (*routine) (xnthread_t *))
{
	xnqueue_t *hookq;
	xnhook_t *hook;
	int err = 0;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus, sched_addhook, "type %d routine %p",
		   type, routine);

	switch (type) {
	case XNHOOK_THREAD_START:
		hookq = &nkpod->tstartq;
		break;
	case XNHOOK_THREAD_SWITCH:
		hookq = &nkpod->tswitchq;
		break;
	case XNHOOK_THREAD_DELETE:
		hookq = &nkpod->tdeleteq;
		break;
	default:
		err = -EINVAL;
		goto unlock_and_exit;
	}

	hook = xnmalloc(sizeof(*hook));

	if (hook) {
		inith(&hook->link);
		hook->routine = routine;
		prependq(hookq, &hook->link);
	} else
		err = -ENOMEM;

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return err;
}
EXPORT_SYMBOL_GPL(xnpod_add_hook);

/*!
 * \fn int xnpod_remove_hook(int type,void (*routine)(xnthread_t *))
 * \brief Remove a nucleus hook.
 *
 * This service removes a nucleus hook previously registered using
 * xnpod_add_hook().
 *
 * @param type Defines the kind of hook to remove among
 * XNHOOK_THREAD_START, XNHOOK_THREAD_DELETE and XNHOOK_THREAD_SWITCH.
 *
 * @param routine The address of the user-supplied routine to remove.
 *
 * @return 0 is returned on success. Otherwise, -EINVAL is returned if
 * type is incorrect or if the routine has never been registered
 * before.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

int xnpod_remove_hook(int type, void (*routine) (xnthread_t *))
{
	xnhook_t *hook = NULL;
	xnholder_t *holder;
	xnqueue_t *hookq;
	int err = 0;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus, sched_removehook, "type %d routine %p",
		   type, routine);

	switch (type) {
	case XNHOOK_THREAD_START:
		hookq = &nkpod->tstartq;
		break;
	case XNHOOK_THREAD_SWITCH:
		hookq = &nkpod->tswitchq;
		break;
	case XNHOOK_THREAD_DELETE:
		hookq = &nkpod->tdeleteq;
		break;
	default:
		goto bad_hook;
	}

	for (holder = getheadq(hookq);
	     holder != NULL; holder = nextq(hookq, holder)) {
		hook = link2hook(holder);

		if (hook->routine == routine) {
			removeq(hookq, holder);
			xnfree(hook);
			goto unlock_and_exit;
		}
	}

      bad_hook:

	err = -EINVAL;

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return err;
}
EXPORT_SYMBOL_GPL(xnpod_remove_hook);

/*!
 * \fn void xnpod_handle_exception(struct ipipe_trap_data *d);
 * \brief Exception handler.
 *
 * This is the handler which is called whenever an exception/fault is
 * caught over the primary domain.
 *
 * @param d A pointer to the trap information block received from the
 * pipeline core.
 */
int xnpod_handle_exception(struct ipipe_trap_data *d)
{
	struct xnthread *thread;
	struct xnarchtcb *tcb;

	if (!xnpod_active_p() || (!xnpod_interrupt_p() && xnpod_idle_p()))
		return 0;

	thread = xnpod_current_thread();

	trace_mark(xn_nucleus, thread_fault,
		   "thread %p thread_name %s ip %p type 0x%x",
		   thread, xnthread_name(thread),
		   (void *)xnarch_fault_pc(d),
		   xnarch_fault_trap(d));

	if (xnarch_fault_fpu_p(d)) {
		if (!xnthread_test_state(thread, XNROOT)) {
			/* FPU exception received in primary mode. */
			tcb = xnthread_archtcb(thread);
			if (xnarch_handle_fpu_fault(tcb))
				return 1;
		}
		print_symbol("invalid use of FPU in Xenomai context at %s\n",
			     xnarch_fault_pc(d));
	}

	if (xnthread_test_state(thread, XNROOT))
		return 0;
	/*
	 * If we experienced a trap on behalf of a shadow thread
	 * running in primary mode, move it to the Linux domain,
	 * leaving the kernel process the exception.
	 */
	thread->regs = xnarch_fault_regs(d);

#if XENO_DEBUG(NUCLEUS)
	if (!user_mode(d->regs)) {
		xntrace_panic_freeze();
		printk(XENO_WARN
		       "switching %s to secondary mode after exception #%u in "
		       "kernel-space at 0x%lx (pid %d)\n", thread->name,
		       xnarch_fault_trap(d),
		       xnarch_fault_pc(d),
		       xnthread_host_pid(thread));
		xntrace_panic_dump();
	} else if (xnarch_fault_notify(d)) /* Don't report debug traps */
		printk(XENO_WARN
		       "switching %s to secondary mode after exception #%u from "
		       "user-space at 0x%lx (pid %d)\n", thread->name,
		       xnarch_fault_trap(d),
		       xnarch_fault_pc(d),
		       xnthread_host_pid(thread));
#endif /* XENO_DEBUG(NUCLEUS) */

	if (xnarch_fault_pf_p(d))
		/*
		 * The page fault counter is not SMP-safe, but it's a
		 * simple indicator that something went wrong wrt
		 * memory locking anyway.
		 */
		xnstat_counter_inc(&thread->stat.pf);

	xnshadow_relax(xnarch_fault_notify(d), SIGDEBUG_MIGRATE_FAULT);

	return 0;
}
EXPORT_SYMBOL_GPL(xnpod_handle_exception);

/*!
 * \fn int xnpod_enable_timesource(void)
 * \brief Activate the core time source.
 *
 * On every architecture, Xenomai directly manages a hardware timer
 * clocked in one-shot mode, to support any number of software timers
 * internally. Timings are always specified as a count of nanoseconds.
 *
 * The xnpod_enable_timesource() service configures the hardware timer
 * chip. Because Xenomai most often interposes on the system timer
 * used by the Linux kernel, a software timer may be started to relay
 * periodic ticks to the host kernel if needed.
 *
 * @return 0 is returned on success. Otherwise:
 *
 * - -ENODEV is returned if a failure occurred while configuring the
 * hardware timer.
 *
 * - -ENOSYS is returned if no active pod exists.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Regular Linux kernel context.
 *
 * Rescheduling: never.
 */
int xnpod_enable_timesource(void)
{
	int err, htickval, cpu;
	xnsched_t *sched;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	if (!xnpod_active_p()) {
		err = -ENOSYS;
		xnlock_put_irqrestore(&nklock, s);
		return err;
	}

	trace_mark(xn_nucleus, enable_timesource, MARK_NOARGS);

#ifdef CONFIG_XENO_OPT_STATS
	/*
	 * Only for statistical purpose, the timer interrupt is
	 * attached by xntimer_grab_hardware().
	 */
	xnintr_init(&nktimer, "[timer]",
		    per_cpu(ipipe_percpu.hrtimer_irq, 0), NULL, NULL, 0);
#endif /* CONFIG_XENO_OPT_STATS */

	xnlock_put_irqrestore(&nklock, s);

	nkclock.wallclock_offset =
		xnclock_get_host_time() - xnclock_read_monotonic();

	for (cpu = 0; cpu < num_online_cpus(); cpu++) {

		if (!xnarch_cpu_supported(cpu))
			continue;

		htickval = xntimer_grab_hardware(cpu);
		if (htickval < 0) {
			while (--cpu >= 0)
				xntimer_release_hardware(cpu);

			return htickval;
		}

		xnlock_get_irqsave(&nklock, s);

		/* If the current tick device for the target CPU is
		 * periodic, we won't be called back for host tick
		 * emulation. Therefore, we need to start a periodic
		 * nucleus timer which will emulate the ticking for
		 * that CPU, since we are going to hijack the hw clock
		 * chip for managing our own system timer.
		 *
		 * CAUTION:
		 *
		 * - nucleus timers may be started only _after_ the hw
		 * timer has been set up for the target CPU through a
		 * call to xntimer_grab_hardware().
		 *
		 * - we don't compensate for the elapsed portion of
		 * the current host tick, since we cannot get this
		 * information easily for all CPUs except the current
		 * one, and also because of the declining relevance of
		 * the jiffies clocksource anyway.
		 *
		 * - we must not hold the nklock across calls to
		 * xntimer_grab_hardware().
		 */

		sched = xnpod_sched_slot(cpu);
		if (htickval > 1)
			xntimer_start(&sched->htimer, htickval, htickval, XN_RELATIVE);
		else
			xntimer_start(&sched->htimer, 0, 0, XN_RELATIVE);

#if defined(CONFIG_XENO_OPT_WATCHDOG)
		xntimer_start(&sched->wdtimer, 1000000000UL, 1000000000UL, XN_RELATIVE);
		xnsched_reset_watchdog(sched);
#endif /* CONFIG_XENO_OPT_WATCHDOG */
		xnlock_put_irqrestore(&nklock, s);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(xnpod_enable_timesource);

/*!
 * \fn void xnpod_disable_timesource(void)
 * \brief Stop the core time source.
 *
 * Releases the hardware timer, and deactivates the system clock.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - User-space task in secondary mode
 *
 * Rescheduling: never.
 */

void xnpod_disable_timesource(void)
{
	int cpu;

	trace_mark(xn_nucleus, disable_timesource, MARK_NOARGS);

	if (!xnpod_active_p())
		return;
	/*
	 * We must not hold the nklock while stopping the hardware
	 * timer, since this could cause deadlock situations to arise
	 * on SMP systems.
	 */
	for (cpu = 0; cpu < num_online_cpus(); cpu++)
		if (xnarch_cpu_supported(cpu))
			xntimer_release_hardware(cpu);

	xntimer_freeze();

	/*
	 * NOTE: The nktimer interrupt object is not destroyed on
	 * purpose since this would be mostly redundant after
	 * xntimer_release_hardware() has been called. In any case, no
	 * resource is associated with this object.
	 */
}
EXPORT_SYMBOL_GPL(xnpod_disable_timesource);

/*!
 * \fn int xnpod_set_thread_periodic(xnthread_t *thread,xnticks_t idate, xntmode_t timeout_mode, xnticks_t period)
 * \brief Make a thread periodic.
 *
 * Make a thread periodic by programming its first release point and
 * its period in the processor time line.  Subsequent calls to
 * xnpod_wait_thread_period() will delay the thread until the next
 * periodic release point in the processor timeline is reached.
 *
 * @param thread The descriptor address of the affected thread. This
 * thread is immediately delayed until the first periodic release
 * point is reached.
 *
 * @param idate The initial (absolute) date of the first release
 * point, expressed in nanoseconds. The affected thread will be
 * delayed by the first call to xnpod_wait_thread_period() until this
 * point is reached. If @a idate is equal to XN_INFINITE, the current
 * system date is used, and no initial delay takes place. In the
 * latter case, @a timeout_mode is not considered and can have any
 * valid value.
 *
 * @param timeout_mode The mode of the @a idate parameter. It can
 * either be set to XN_ABSOLUTE or XN_REALTIME with @a idate different
 * from XN_INFINITE (see also xntimer_start()).
 *
 * @param period The period of the thread, expressed in nanoseconds.
 * As a side-effect, passing XN_INFINITE attempts to stop the thread's
 * periodic timer; in the latter case, the routine always exits
 * succesfully, regardless of the previous state of this timer.
 *
 * @return 0 is returned upon success. Otherwise:
 *
 * - -ETIMEDOUT is returned @a idate is different from XN_INFINITE and
 * represents a date in the past.
 *
 * - -EINVAL is returned if @a period is different from XN_INFINITE
 * but shorter than the scheduling latency value for the target
 * system, as available from /proc/xenomai/latency. -EINVAL is also
 * returned if @a timeout_mode is not compatible with @a idate, such
 * as XN_RELATIVE with @a idate different from XN_INFINITE.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: none.
 */

int xnpod_set_thread_periodic(xnthread_t *thread, xnticks_t idate,
			      xntmode_t timeout_mode, xnticks_t period)
{
	int err = 0;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus, thread_setperiodic,
		   "thread %p thread_name %s idate %Lu mode %d period %Lu timer %p",
		   thread, xnthread_name(thread), idate, timeout_mode, period,
		   &thread->ptimer);

	if (period == XN_INFINITE) {
		if (xntimer_running_p(&thread->ptimer))
			xntimer_stop(&thread->ptimer);

		goto unlock_and_exit;
	}

	if (period < nklatency) {
		/*
		 * LART: detect periods which are shorter than the
		 * intrinsic latency figure; this must be a joke...
		 */
		err = -EINVAL;
		goto unlock_and_exit;
	}

	xntimer_set_sched(&thread->ptimer, thread->sched);

	if (idate == XN_INFINITE) {
		xntimer_start(&thread->ptimer, period, period, XN_RELATIVE);
	} else {
		if (timeout_mode == XN_REALTIME)
			idate -= xnclock_get_offset();
		else if (timeout_mode != XN_ABSOLUTE) {
			err = -EINVAL;
			goto unlock_and_exit;
		}
		err = xntimer_start(&thread->ptimer, idate + period, period,
				    XN_ABSOLUTE);
	}

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return err;
}
EXPORT_SYMBOL_GPL(xnpod_set_thread_periodic);

/**
 * @fn int xnpod_wait_thread_period(unsigned long *overruns_r)
 * @brief Wait for the next periodic release point.
 *
 * Make the current thread wait for the next periodic release point in
 * the processor time line.
 *
 * @param overruns_r If non-NULL, @a overruns_r must be a pointer to a
 * memory location which will be written with the count of pending
 * overruns. This value is copied only when xnpod_wait_thread_period()
 * returns -ETIMEDOUT or success; the memory location remains
 * unmodified otherwise. If NULL, this count will never be copied
 * back.
 *
 * @return 0 is returned upon success; if @a overruns_r is valid, zero
 * is copied to the pointed memory location. Otherwise:
 *
 * - -EWOULDBLOCK is returned if xnpod_set_thread_periodic() has not
 * previously been called for the calling thread.
 *
 * - -EINTR is returned if xnpod_unblock_thread() has been called for
 * the waiting thread before the next periodic release point has been
 * reached. In this case, the overrun counter is reset too.
 *
 * - -ETIMEDOUT is returned if the timer has overrun, which indicates
 * that one or more previous release points have been missed by the
 * calling thread. If @a overruns_r is valid, the count of pending
 * overruns is copied to the pointed memory location.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: always, unless the current release point has already
 * been reached.  In the latter case, the current thread immediately
 * returns from this service without being delayed.
 */

int xnpod_wait_thread_period(unsigned long *overruns_r)
{
	unsigned long overruns = 0;
	xnthread_t *thread;
	xnticks_t now;
	int err = 0;
	spl_t s;

	thread = xnpod_current_thread();

	xnlock_get_irqsave(&nklock, s);

	if (unlikely(!xntimer_running_p(&thread->ptimer))) {
		err = -EWOULDBLOCK;
		goto unlock_and_exit;
	}

	trace_mark(xn_nucleus, thread_waitperiod, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	now = xnclock_read_raw();
	if (likely((xnsticks_t)(now - xntimer_pexpect(&thread->ptimer)) < 0)) {
		xnpod_suspend_thread(thread, XNDELAY, XN_INFINITE, XN_RELATIVE, NULL);

		if (unlikely(xnthread_test_info(thread, XNBREAK))) {
			err = -EINTR;
			goto unlock_and_exit;
		}

		now = xnclock_read_raw();
	}

	overruns = xntimer_get_overruns(&thread->ptimer, now);
	if (overruns) {
		err = -ETIMEDOUT;

		trace_mark(xn_nucleus, thread_missedperiod,
			   "thread %p thread_name %s overruns %lu",
			   thread, xnthread_name(thread), overruns);
	}

	if (likely(overruns_r != NULL))
		*overruns_r = overruns;

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return err;
}
EXPORT_SYMBOL_GPL(xnpod_wait_thread_period);

/**
 * @fn int xnpod_set_thread_tslice(struct xnthread *thread, xnticks_t quantum)
 * @brief Set thread time-slicing information.
 *
 * Update the time-slicing information for a given thread. This
 * service enables or disables round-robin scheduling for the thread,
 * depending on the value of @a quantum. By default, times-slicing is
 * disabled for a new thread initialized by a call to
 * xnpod_init_thread().
 *
 * @param thread The descriptor address of the affected thread.
 *
 * @param quantum The time quantum assigned to the thread expressed in
 * nanoseconds. If @a quantum is different from XN_INFINITE, the
 * time-slice for the thread is set to that value and its current time
 * credit is refilled (i.e. the thread is given a full time-slice to
 * run next). Otherwise, if @a quantum equals XN_INFINITE,
 * time-slicing is stopped for that thread.
 *
 * @return 0 is returned upon success. Otherwise:
 *
 * - -EINVAL is returned if @a quantum is not XN_INFINITE, and the
 * base scheduling class of the target thread does not support
 * time-slicing.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Any kernel context.
 *
 * Rescheduling: never.
 */
int xnpod_set_thread_tslice(struct xnthread *thread, xnticks_t quantum)
{
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	thread->rrperiod = quantum;
	xntimer_stop(&thread->rrbtimer);

	if (quantum != XN_INFINITE) {
		if (thread->base_class->sched_tick == NULL) {
			xnlock_put_irqrestore(&nklock, s);
			return -EINVAL;
		}
		xnthread_set_state(thread, XNRRB);
		xntimer_start(&thread->rrbtimer,
			      quantum, quantum, XN_RELATIVE);
	} else
		xnthread_clear_state(thread, XNRRB);

	xnlock_put_irqrestore(&nklock, s);

	return 0;
}
EXPORT_SYMBOL_GPL(xnpod_set_thread_tslice);

#ifdef CONFIG_XENO_OPT_VFILE

#if XENO_DEBUG(XNLOCK)

struct xnlockinfo xnlock_stats[NR_CPUS];
EXPORT_SYMBOL_GPL(xnlock_stats);

static int lock_vfile_show(struct xnvfile_regular_iterator *it, void *data)
{
	struct xnlockinfo lockinfo;
	spl_t s;
	int cpu;

	for_each_online_cpu(cpu) {

		xnlock_get_irqsave(&nklock, s);
		lockinfo = xnlock_stats[cpu];
		xnlock_put_irqrestore(&nklock, s);

		if (cpu > 0)
			xnvfile_printf(it, "\n");

		xnvfile_printf(it, "CPU%d:\n", cpu);

		xnvfile_printf(it,
			     "  longest locked section: %llu ns\n"
			     "  spinning time: %llu ns\n"
			     "  section entry: %s:%d (%s)\n",
			     xnarch_tsc_to_ns(lockinfo.lock_time),
			     xnarch_tsc_to_ns(lockinfo.spin_time),
			     lockinfo.file, lockinfo.line, lockinfo.function);
	}

	return 0;
}

static ssize_t lock_vfile_store(struct xnvfile_input *input)
{
	ssize_t ret;
	spl_t s;
	int cpu;

	long val;

	ret = xnvfile_get_integer(input, &val);
	if (ret < 0)
		return ret;

	if (val != 0)
		return -EINVAL;

	for_each_online_cpu(cpu) {
		xnlock_get_irqsave(&nklock, s);
		memset(&xnlock_stats[cpu], '\0', sizeof(xnlock_stats[cpu]));
		xnlock_put_irqrestore(&nklock, s);
	}

	return ret;
}

static struct xnvfile_regular_ops lock_vfile_ops = {
	.show = lock_vfile_show,
	.store = lock_vfile_store,
};

static struct xnvfile_regular lock_vfile = {
	.ops = &lock_vfile_ops,
};

#endif /* XENO_DEBUG(XNLOCK) */

static int latency_vfile_show(struct xnvfile_regular_iterator *it, void *data)
{
	xnvfile_printf(it, "%Lu\n", xnarch_tsc_to_ns(nklatency - nktimerlat));

	return 0;
}

static ssize_t latency_vfile_store(struct xnvfile_input *input)
{
	ssize_t ret;
	long val;

	ret = xnvfile_get_integer(input, &val);
	if (ret < 0)
		return ret;

	nklatency = xnarch_ns_to_tsc(val) + nktimerlat;

	return ret;
}

static struct xnvfile_regular_ops latency_vfile_ops = {
	.show = latency_vfile_show,
	.store = latency_vfile_store,
};

static struct xnvfile_regular latency_vfile = {
	.ops = &latency_vfile_ops,
};

static int version_vfile_show(struct xnvfile_regular_iterator *it, void *data)
{
	xnvfile_printf(it, "%s\n", XENO_VERSION_STRING);

	return 0;
}

static struct xnvfile_regular_ops version_vfile_ops = {
	.show = version_vfile_show,
};

static struct xnvfile_regular version_vfile = {
	.ops = &version_vfile_ops,
};

static int faults_vfile_show(struct xnvfile_regular_iterator *it, void *data)
{
	int cpu, trap;

	xnvfile_puts(it, "TRAP ");

	for_each_online_cpu(cpu)
		xnvfile_printf(it, "        CPU%d", cpu);

	for (trap = 0; xnarch_machdesc.fault_labels[trap]; trap++) {
		if (*xnarch_machdesc.fault_labels[trap] == '\0')
			continue;

		xnvfile_printf(it, "\n%3d: ", trap);

		for_each_online_cpu(cpu)
			xnvfile_printf(it, "%12u",
				       xnarch_machdata.faults[cpu][trap]);

		xnvfile_printf(it, "    (%s)",
			       xnarch_machdesc.fault_labels[trap]);
	}

	xnvfile_putc(it, '\n');

	return 0;
}

static struct xnvfile_regular_ops faults_vfile_ops = {
	.show = faults_vfile_show,
};

static struct xnvfile_regular faults_vfile = {
	.ops = &faults_vfile_ops,
};

static int apc_vfile_show(struct xnvfile_regular_iterator *it, void *data)
{
	int cpu, apc;

	/* We assume the entire output fits in a single page. */

	xnvfile_puts(it, "APC ");

	for_each_online_cpu(cpu)
		xnvfile_printf(it, "         CPU%d", cpu);

	for (apc = 0; apc < BITS_PER_LONG; apc++) {
		if (!test_bit(apc, &xnarch_machdata.apc_map))
			continue; /* Not hooked. */

		xnvfile_printf(it, "\n%3d: ", apc);

		for_each_online_cpu(cpu)
			xnvfile_printf(it, "%12lu",
				       xnarch_machdata.apc_table[apc].hits[cpu]);

		if (xnarch_machdata.apc_table[apc].name)
			xnvfile_printf(it, "    (%s)",
				       xnarch_machdata.apc_table[apc].name);
	}

	xnvfile_putc(it, '\n');

	return 0;
}

static struct xnvfile_regular_ops apc_vfile_ops = {
	.show = apc_vfile_show,
};

static struct xnvfile_regular apc_vfile = {
	.ops = &apc_vfile_ops,
};

int __init xnpod_init_proc(void)
{
	int ret;

	ret = xnvfile_init_root();
	if (ret)
		return ret;

	ret = xnsched_init_proc();
	if (ret)
		return ret;

	xnclock_init_proc();
	xntimer_init_proc();
	xnheap_init_proc();
	xnintr_init_proc();

	xnvfile_init_regular("latency", &latency_vfile, &nkvfroot);
	xnvfile_init_regular("version", &version_vfile, &nkvfroot);
	xnvfile_init_regular("faults", &faults_vfile, &nkvfroot);
	xnvfile_init_regular("apc", &apc_vfile, &nkvfroot);
#ifdef CONFIG_XENO_OPT_DEBUG
	xnvfile_init_dir("debug", &debug_vfroot, &nkvfroot);
#if XENO_DEBUG(XNLOCK)
	xnvfile_init_regular("lock", &lock_vfile, &debug_vfroot);
#endif
#endif /* XENO_DEBUG(NUCLEUS) */

	return 0;
}

void xnpod_cleanup_proc(void)
{
#if XENO_DEBUG(NUCLEUS)
#if XENO_DEBUG(XNLOCK)
	xnvfile_destroy_regular(&lock_vfile);
#endif
	xnvfile_destroy_dir(&debug_vfroot);
#endif /* XENO_DEBUG(NUCLEUS) */
	xnvfile_destroy_regular(&apc_vfile);
	xnvfile_destroy_regular(&faults_vfile);
	xnvfile_destroy_regular(&version_vfile);
	xnvfile_destroy_regular(&latency_vfile);

	xnintr_cleanup_proc();
	xnheap_cleanup_proc();
	xntimer_cleanup_proc();
	xnclock_cleanup_proc();
	xnsched_cleanup_proc();

	xnvfile_destroy_root();
}

#endif /* CONFIG_XENO_OPT_VFILE */

/*@}*/
