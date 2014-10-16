/*
 * Copyright (C) 2011 Philippe Gerum <rpm@xenomai.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef _COBALT_POSIX_MONITOR_H
#define _COBALT_POSIX_MONITOR_H

#include <cobalt/kernel/synch.h>
#include <cobalt/uapi/monitor.h>
#include <xenomai/posix/syscall.h>

struct cobalt_kqueues;

struct cobalt_monitor {
	unsigned int magic;
	struct xnsynch gate;
	struct xnsynch drain;
	struct cobalt_monitor_data *data;
	struct cobalt_kqueues *owningq;
	struct list_head link;
	struct list_head waiters;
	int flags;
	xntmode_t tmode;
	xnhandle_t handle;
};

int __cobalt_monitor_wait(struct cobalt_monitor_shadow __user *u_mon,
			  int event, const struct timespec *ts,
			  int __user *u_ret);

COBALT_SYSCALL_DECL(monitor_init,
		    int, (struct cobalt_monitor_shadow __user *u_monsh,
			  clockid_t clk_id,
			  int flags));

COBALT_SYSCALL_DECL(monitor_enter,
		    int, (struct cobalt_monitor_shadow __user *u_monsh));

COBALT_SYSCALL_DECL(monitor_sync,
		    int, (struct cobalt_monitor_shadow __user *u_monsh));

COBALT_SYSCALL_DECL(monitor_exit,
		    int, (struct cobalt_monitor_shadow __user *u_monsh));

COBALT_SYSCALL_DECL(monitor_wait,
		    int, (struct cobalt_monitor_shadow __user *u_monsh,
			  int event, const struct timespec __user *u_ts,
			  int __user *u_ret));

COBALT_SYSCALL_DECL(monitor_destroy,
		    int, (struct cobalt_monitor_shadow __user *u_monsh));

void cobalt_monitorq_cleanup(struct cobalt_kqueues *q);

void cobalt_monitor_pkg_init(void);

void cobalt_monitor_pkg_cleanup(void);

#endif /* !_COBALT_POSIX_MONITOR_H */
