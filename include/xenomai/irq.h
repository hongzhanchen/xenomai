/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2017 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _XN_IRQ_H
#define _XN_IRQ_H
#include <cobalt/kernel/sched.h>
/* hard irqs off. */
static inline void xn_enter_irq(void)
{
	struct xnsched *sched = xnsched_current();

	sched->lflags |= XNINIRQ;
}

/* hard irqs off. */
static inline void xn_exit_irq(void)
{
	struct xnsched *sched = xnsched_current();

	sched->lflags &= ~XNINIRQ;

	/*
	 * CAUTION: Switching stages as a result of rescheduling may
	 * re-enable irqs, shut them off before returning if so.
	 */
	if ((sched->status|sched->lflags) & XNRESCHED) {
		xnsched_run();
		if (!hard_irqs_disabled())
			hard_local_irq_disable();
	}
}
#endif /* !_XN_IRQ_H */

