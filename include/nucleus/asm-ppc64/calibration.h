/*
 * Copyright (C) 2001,2002,2003,2004,2005 Philippe Gerum <rpm@xenomai.org>.
 *
 * Xenomai is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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
 */

#ifndef _XENO_ASM_PPC_CALIBRATION_H
#define _XENO_ASM_PPC_CALIBRATION_H

#include <xeno_config.h>
#include <asm/delay.h>

#define __bogomips (loops_per_jiffy/(500000/HZ))

static inline unsigned long xnarch_get_sched_latency (void)

{
#if CONFIG_XENO_HW_SCHED_LATENCY != 0
#define __sched_latency CONFIG_XENO_HW_SCHED_LATENCY
#else

#define __sched_latency 1000

#endif /* CONFIG_XENO_HW_SCHED_LATENCY */

    return __sched_latency;
}

#undef __sched_latency
#undef __bogomips

#endif /* !_XENO_ASM_PPC_CALIBRATION_H */
