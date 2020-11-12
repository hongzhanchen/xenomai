/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_XN_IRQ_H
#define _ASM_GENERIC_XN_IRQ_H

#include <xenomai/irq.h>
static inline void irq_enter_pipeline(void)
{
#ifdef CONFIG_XENOMAI
	xn_enter_irq();
#endif
}

static inline void irq_exit_pipeline(void)
{
#ifdef CONFIG_XENOMAI
	xn_exit_irq();
#endif
}

#endif /* !_ASM_GENERIC_XN_IRQ_H */

