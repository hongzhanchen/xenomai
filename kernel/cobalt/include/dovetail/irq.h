/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _XN_DOVETAIL_IRQ_H
#define _XN_DOVETAIL_IRQ_H

#ifdef CONFIG_XENOMAI
#include <asm-generic/xenomai/irq.h>
#else
#include_next <dovetail/irq.h>
#endif

#endif /* !_XN_DOVETAIL_IRQ_H */

