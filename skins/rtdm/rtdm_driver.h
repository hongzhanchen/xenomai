/**
 * @file
 * Real-Time Driver Model for Xenomai, driver API header
 *
 * @note Copyright (C) 2005 Jan Kiszka <jan.kiszka@web.de>
 * @note Copyright (C) 2005 Joerg Langenberg <joerg.langenberg@gmx.net>
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
 * along with Xenomai; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * @ingroup driverapi
 */

#ifndef _RTDM_DRIVER_H
#define _RTDM_DRIVER_H

#ifndef __KERNEL__
#error This header is for kernel space usage only. \
       You are likely looking for rtdm/rtdm.h...
#endif /* !__KERNEL__ */

#include <asm/atomic.h>
#include <linux/list.h>

#include <nucleus/xenomai.h>
#include <nucleus/core.h>
#include <nucleus/heap.h>
#include <nucleus/pod.h>
#include <nucleus/synch.h>
#include <rtdm/rtdm.h>


struct rtdm_dev_context;


/*!
 * @ingroup devregister
 * @anchor dev_flags @name Device Flags
 * Static flags describing a RTDM device
 * @{
 */
/** If set, only a single instance of the device can be requested by an
 *  application. */
#define RTDM_EXCLUSIVE              0x0001

/** If set, the device is addressed via a clear-text name. */
#define RTDM_NAMED_DEVICE           0x0010

/** If set, the device is addressed via a combination of protocol ID and
 *  socket type. */
#define RTDM_PROTOCOL_DEVICE        0x0020

/** Mask selecting the device type. */
#define RTDM_DEVICE_TYPE_MASK       0x00F0
/** @} */


/*!
 * @anchor ctx_flags @name Context Flags
 * Dynamic flags describing the state of an open RTDM device (bit numbers)
 * @{
 */
/** Set by RTDM if the device instance was created in non-real-time
 *  context. */
#define RTDM_CREATED_IN_NRT         0

/** Set by RTDM when the device is being closed. */
#define RTDM_CLOSING                1

/** Set by RTDM if the device has to be closed regardless of possible pending
 *  locks held by other users. */
#define RTDM_FORCED_CLOSING         2

/** Lowest bit number the driver developer can use freely */
#define RTDM_USER_CONTEXT_FLAG      8   /* first user-definable flag */
/** @} */


/*!
 * @ingroup devregister
 * @anchor versioning @name Versioning
 * Current revisions of RTDM structures and interfaces, encoding of driver
 * versions.
 * @{
 */
/** Version of struct rtdm_device */
#define RTDM_DEVICE_STRUCT_VER      3

/** Version of struct rtdm_dev_context */
#define RTDM_CONTEXT_STRUCT_VER     3

/** Driver API version */
#define RTDM_API_VER                3

/** Minimum API revision compatible with the current release */
#define RTDM_API_MIN_COMPAT_VER     3

/** Flag indicating a secure variant of RTDM (not supported here) */
#define RTDM_SECURE_DEVICE          0x80000000

/** Version code constructor for driver revisions */
#define RTDM_DRIVER_VER(major, minor, patch) \
    (((major & 0xFF) << 16) | ((minor & 0xFF) << 8) | (patch & 0xFF))

/** Get major version number from driver revision code */
#define RTDM_DRIVER_MAJOR_VER(ver)  (((ver) >> 16) & 0xFF)

/** Get minor version number from driver revision code */
#define RTDM_DRIVER_MINOR_VER(ver)  (((ver) >> 8) & 0xFF)

/** Get patch version number from driver revision code */
#define RTDM_DRIVER_PATCH_VER(ver) ((ver) & 0xFF)
/** @} */


/*!
 * @ingroup devregister
 * @name Operation Handler Prototypes
 * @{
 */

/**
 * Named device open handler
 *
 * @param[in] context Context structure associated with opened device instance
 * @param[in] user_info Opaque pointer to information about user mode caller,
 * NULL if kernel mode call
 * @param[in] oflag Open flags as passed by the user
 *
 * @return 0 on success, otherwise negative error code
 *
 * @see @c open() in IEEE Std 1003.1,
 * http://www.opengroup.org/onlinepubs/009695399 */
typedef
    int     (*rtdm_open_handler_t)   (struct rtdm_dev_context   *context,
                                      rtdm_user_info_t          *user_info,
                                      int                       oflag);

/**
 * Socket creation handler for protocol devices
 *
 * @param[in] context Context structure associated with opened device instance
 * @param[in] user_info Opaque pointer to information about user mode caller,
 * NULL if kernel mode call
 * @param[in] protocol Protocol number as passed by the user
 *
 * @return 0 on success, otherwise negative error code
 *
 * @see @c socket() in IEEE Std 1003.1,
 * http://www.opengroup.org/onlinepubs/009695399 */
typedef
    int     (*rtdm_socket_handler_t) (struct rtdm_dev_context   *context,
                                      rtdm_user_info_t          *user_info,
                                      int                       protocol);

/**
 * Close handler
 *
 * @param[in] context Context structure associated with opened device instance
 * @param[in] user_info Opaque pointer to information about user mode caller,
 * NULL if kernel mode call
 *
 * @return 0 on success, otherwise negative error code
 *
 * @see @c close() in IEEE Std 1003.1,
 * http://www.opengroup.org/onlinepubs/009695399 */
typedef
    int     (*rtdm_close_handler_t)  (struct rtdm_dev_context   *context,
                                      rtdm_user_info_t          *user_info);

/**
 * IOCTL handler
 *
 * @param[in] context Context structure associated with opened device instance
 * @param[in] user_info Opaque pointer to information about user mode caller,
 * NULL if kernel mode call
 * @param[in] request Request number as passed by the user
 * @param[in,out] arg Request argument as passed by the user
 *
 * @return Positiv value on success, otherwise negative error code
 *
 * @see @c ioctl() in IEEE Std 1003.1,
 * http://www.opengroup.org/onlinepubs/009695399 */
typedef
    int     (*rtdm_ioctl_handler_t)  (struct rtdm_dev_context   *context,
                                      rtdm_user_info_t          *user_info,
                                      int                       request,
                                      void                      *arg);

/**
 * Read handler
 *
 * @param[in] context Context structure associated with opened device instance
 * @param[in] user_info Opaque pointer to information about user mode caller,
 * NULL if kernel mode call
 * @param[out] buf Input buffer as passed by the user
 * @param[in] nbyte Number of bytes the user requests to read
 *
 * @return On success, the number of bytes read, otherwise negative error code
 *
 * @see @c read() in IEEE Std 1003.1,
 * http://www.opengroup.org/onlinepubs/009695399 */
typedef
    ssize_t (*rtdm_read_handler_t)   (struct rtdm_dev_context   *context,
                                      rtdm_user_info_t          *user_info,
                                      void                      *buf,
                                      size_t                    nbyte);

/**
 * Write handler
 *
 * @param[in] context Context structure associated with opened device instance
 * @param[in] user_info Opaque pointer to information about user mode caller,
 * NULL if kernel mode call
 * @param[in] buf Output buffer as passed by the user
 * @param[in] nbyte Number of bytes the user requests to write
 *
 * @return On success, the number of bytes written, otherwise negative error
 * code
 *
 * @see @c write() in IEEE Std 1003.1,
 * http://www.opengroup.org/onlinepubs/009695399 */
typedef
    ssize_t (*rtdm_write_handler_t)  (struct rtdm_dev_context   *context,
                                      rtdm_user_info_t          *user_info,
                                      const void                *buf,
                                      size_t                    nbyte);

/**
 * Receive message handler
 *
 * @param[in] context Context structure associated with opened device instance
 * @param[in] user_info Opaque pointer to information about user mode caller,
 * NULL if kernel mode call
 * @param[in,out] msg Message descriptor as passed by the user, automatically
 * mirrored to safe kernel memory in case of user mode call
 * @param[in] flags Message flags as passed by the user
 *
 * @return On success, the number of bytes received, otherwise negative error
 * code
 *
 * @see @c recvmsg() in IEEE Std 1003.1,
 * http://www.opengroup.org/onlinepubs/009695399 */
typedef
    ssize_t (*rtdm_recvmsg_handler_t)(struct rtdm_dev_context   *context,
                                      rtdm_user_info_t          *user_info,
                                      struct msghdr             *msg,
                                      int                       flags);

/**
 * Transmit message handler
 *
 * @param[in] context Context structure associated with opened device instance
 * @param[in] user_info Opaque pointer to information about user mode caller,
 * NULL if kernel mode call
 * @param[in] msg Message descriptor as passed by the user, automatically
 * mirrored to safe kernel memory in case of user mode call
 * @param[in] flags Message flags as passed by the user
 *
 * @return On success, the number of bytes transmitted, otherwise negative
 * error code
 *
 * @see @c sendmsg() in IEEE Std 1003.1,
 * http://www.opengroup.org/onlinepubs/009695399 */
typedef
    ssize_t (*rtdm_sendmsg_handler_t)(struct rtdm_dev_context   *context,
                                      rtdm_user_info_t          *user_info,
                                      const struct msghdr       *msg,
                                      int                       flags);
/** @} */

typedef
    int     (*rtdm_rt_handler_t)     (struct rtdm_dev_context   *context,
                                      rtdm_user_info_t          *user_info,
                                      void                      *arg);


/**
 * @ingroup devregister
 * Device operations
 */
struct rtdm_operations {
    /*! @name Common Operations
     * @{ */
    /** Close handler for real-time contexts,
     *  optional if \a close_nrt is non-NULL */
    rtdm_close_handler_t            close_rt;
    /** Close handler for non-real-time contexts,
     *  optional if \a close_rt is non-NULL */
    rtdm_close_handler_t            close_nrt;
    /** IOCTL from real-time context (optional) */
    rtdm_ioctl_handler_t            ioctl_rt;
    /** IOCTL from non-real-time context (optional) */
    rtdm_ioctl_handler_t            ioctl_nrt;
    /** @} */

    /*! @name Stream-Oriented Device Operations
     * @{ */
    /** Read handler for real-time context (optional) */
    rtdm_read_handler_t             read_rt;
    /** Read handler for non-real-time context (optional) */
    rtdm_read_handler_t             read_nrt;
    /** Write handler for real-time context (optional) */
    rtdm_write_handler_t            write_rt;
    /** Write handler for non-real-time context (optional) */
    rtdm_write_handler_t            write_nrt;
    /** @} */

    /*! @name Message-Oriented Device Operations
     * @{ */
    /** Receive message handler for real-time context (optional) */
    rtdm_recvmsg_handler_t          recvmsg_rt;
    /** Receive message handler for non-real-time context (optional) */
    rtdm_recvmsg_handler_t          recvmsg_nrt;
    /** Transmit message handler for real-time context (optional) */
    rtdm_sendmsg_handler_t          sendmsg_rt;
    /** Transmit message handler for non-real-time context (optional) */
    rtdm_sendmsg_handler_t          sendmsg_nrt;
    /** @} */
};

/**
 * @brief Device context
 *
 * A device context structure is associated with every open device instance.
 * RTDM takes care of its creation and destruction and passes it to the
 * operation handlers when being invoked.
 *
 * Driver get attach arbitrary data immediately after the official structure.
 * The size of this data is provided via rtdm_device.context_size during
 * device registration.
 */
struct rtdm_dev_context {
    /** Context flags, see @ref ctx_flags "Context Flags" for details */
    unsigned long                   context_flags;
    /** Associated file descriptor */
    int                             fd;
    /** Lock counter of context, held while structure is referenced by an
     *  operation handler */
    atomic_t                        close_lock_count;
    /** Set of active device operation handlers */
    struct rtdm_operations          *ops;
    /** Reference to owning device */
    volatile struct rtdm_device     *device;
    /** Begin of driver defined context data structure */
    char                            dev_private[0];
};

struct rtdm_dev_reserved {
    struct list_head                entry;
    atomic_t                        refcount;
    struct rtdm_dev_context         *exclusive_context;
};

/**
 * @ingroup devregister
 * @brief RTDM device
 *
 * This structure specifies a RTDM device. As some fields, especially the
 * reserved area, will be modified by RTDM during runtime, the structure must
 * not reside in write-protected memory.
 */
struct rtdm_device {
    /** Revision number of this structure, see
     *  @ref versioning "Versioning" defines */
    int                             struct_version;

    /** Device flags, see @ref dev_flags "Device Flags" for details */
    int                             device_flags;
    /** Size of driver defined appendix to struct rtdm_dev_context */
    size_t                          context_size;

    /** Named device identification (orthogonal to Linux device name space) */
    char                            device_name[RTDM_MAX_DEVNAME_LEN+1];

    /** Protocol device identification: protocol family (PF_xxx) */
    int                             protocol_family;
    /** Protocol device identification: socket type (SOCK_xxx) */
    int                             socket_type;

    /** Named device instance creation for real-time contexts,
     *  optional if open_nrt is non-NULL, ignored for protocol devices */
    rtdm_open_handler_t             open_rt;
    /** Named device instance creation for non-real-time contexts,
     *  optional if open_rt is non-NULL, ignored for protocol devices */
    rtdm_open_handler_t             open_nrt;

    /** Protocol socket creation for real-time contexts,
     *  optional if socket_nrt is non-NULL, ignored for named devices */
    rtdm_socket_handler_t           socket_rt;
    /** Protocol socket creation for non-real-time contexts,
     *  optional if socket_rt is non-NULL, ignored for named devices */
    rtdm_socket_handler_t           socket_nrt;

    /** Default operations on newly opened device instance */
    struct rtdm_operations          ops;

    /** Device class ID, see @ref RTDM_CLASS_xxx */
    int                             device_class;
    /** Device sub-class, see RTDM_SUBCLASS_xxx definition in the
     *  @ref profiles "Device Profiles" */
    int                             device_sub_class;
    /** Informational driver name (reported via /proc) */
    const char                      *driver_name;
    /** Driver version, see @ref versioning "Versioning" defines */
    int                             driver_version;
    /** Informational peripheral name the device is attached to
     *  (reported via /proc) */
    const char                      *peripheral_name;
    /** Informational driver provider name (reported via /proc) */
    const char                      *provider_name;

    /** Name of /proc entry for the device, must not be NULL */
    const char                      *proc_name;
    /** Set to device's /proc root entry after registration, do not modify */
    struct proc_dir_entry           *proc_entry;

    /** Driver definable device ID */
    int                             device_id;

    /** Data stored by RTDM inside a registered device (internal use only) */
    struct rtdm_dev_reserved        reserved;
};


/* --- device registration --- */

int rtdm_dev_register(struct rtdm_device* device);
int rtdm_dev_unregister(struct rtdm_device* device, unsigned int poll_delay);


/* --- inter-driver API --- */

#define rtdm_open                   rt_dev_open
#define rtdm_socket                 rt_dev_socket
#define rtdm_close                  rt_dev_close
#define rtdm_ioctl                  rt_dev_ioctl
#define rtdm_read                   rt_dev_read
#define rtdm_write                  rt_dev_write
#define rtdm_rescmsg                rt_dev_recvmsg
#define rtdm_recv                   rt_dev_recv
#define rtdm_recvfrom               rt_dev_recvfrom
#define rtdm_sendmsg                rt_dev_sendmsg
#define rtdm_send                   rt_dev_send
#define rtdm_sendto                 rt_dev_sendto
#define rtdm_bind                   rt_dev_bind
#define rtdm_listen                 rt_dev_listen
#define rtdm_accept                 rt_dev_accept
#define rtdm_getsockopt             rt_dev_getsockopt
#define rtdm_setsockopt             rt_dev_setsockopt
#define rtdm_getsockname            rt_dev_getsockname
#define rtdm_getpeername            rt_dev_getpeername
#define rtdm_shutdown               rt_dev_shutdown

struct rtdm_dev_context *rtdm_context_get(int fd);

static inline void rtdm_context_lock(struct rtdm_dev_context *context)
{
    atomic_inc(&context->close_lock_count);
}

static inline void rtdm_context_unlock(struct rtdm_dev_context *context)
{
    atomic_dec(&context->close_lock_count);
}


/* --- clock services --- */
static inline uint64_t rtdm_clock_read(void)
{
    return xnpod_ticks2ns(xnpod_get_time());
}


/* --- spin lock services --- */
/*!
 * @addtogroup rtdmsync
 * @{
 */

/*!
 * @name Global Lock across Scheduler Invocation
 * @{
 */

/**
 * @brief Execute code block atomically
 *
 * Generally, it is illegal to suspend the current task by calling
 * rtdm_task_sleep(), rtdm_event_wait(), etc. while holding a spinlock. In
 * contrast, this macro allows to combine several operations including
 * a potentially rescheduling call to an atomic code block with respect to
 * other RTDM_EXECUTE_ATOMICALLY() blocks. The macro is a light-weight
 * alternative for protecting code blocks via mutexes, and it can even be used
 * to synchronise real-time and non-real-time contexts.
 *
 * @param code_block Commands to be executed atomically
 *
 * @note It is not allowed to leave the code block explicitely by using
 * @c break, @c return, @c goto, etc. This would leave the global lock held
 * during the code block execution in an inconsistent state. Moreover, do not
 * embed complex operations into the code bock. Consider that they will be
 * executed under preemption lock with interrupts switched-off. Also note that
 * invocation of rescheduling calls may break the atomicity until the task
 * gains the CPU again.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task (RT, non-RT)
 *
 * Rescheduling: possible, depends on functions called within @a code_block.
 */
#define RTDM_EXECUTE_ATOMICALLY(code_block)                                 \
{                                                                           \
    spl_t   s;                                                              \
                                                                            \
    xnlock_get_irqsave(&nklock, s);                                         \
    code_block;                                                             \
    xnlock_put_irqrestore(&nklock, s);                                      \
}
/** @} */

/*!
 * @name Spinlock with Preemption Deactivation
 * @{
 */

/**
 * Static lock initialisation
 */
#define RTDM_LOCK_UNLOCKED          SPIN_LOCK_UNLOCKED

/** Lock variable */
typedef spinlock_t                  rtdm_lock_t;

/** Variable to save the context while holding a lock */
typedef unsigned long               rtdm_lockctx_t;

/**
 * Dynamic lock initialisation
 *
 * @param lock Address of lock variable
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task (RT, non-RT)
 *
 * Rescheduling: never.
 */
#define rtdm_lock_init(lock)        spin_lock_init(lock)

/**
 * Acquire lock from non-preemptible contexts
 *
 * @param lock Address of lock variable
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task (RT, non-RT)
 *
 * Rescheduling: never.
 */
#define rtdm_lock_get(lock)         rthal_spin_lock(lock)

/**
 * Release lock without preemption restoration
 *
 * @param lock Address of lock variable
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task (RT, non-RT)
 *
 * Rescheduling: never.
 */
#define rtdm_lock_put(lock)         rthal_spin_unlock(lock)

/**
 * Acquire lock and disable preemption
 *
 * @param lock Address of lock variable
 * @param context name of local variable to store the context in
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task (RT, non-RT)
 *
 * Rescheduling: never.
 */
#define rtdm_lock_get_irqsave(lock, context)    \
    rthal_spin_lock_irqsave(lock, context)

/**
 * Release lock and restore preemption state
 *
 * @param lock Address of lock variable
 * @param context name of local variable which stored the context
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task (RT, non-RT)
 *
 * Rescheduling: possible.
 */
#define rtdm_lock_put_irqrestore(lock, context) \
    rthal_spin_unlock_irqrestore(lock, context)

/**
 * Disable preemption locally
 *
 * @param context name of local variable to store the context in
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task (RT, non-RT)
 *
 * Rescheduling: never.
 */
#define rtdm_lock_irqsave(context)              \
    rthal_local_irq_save(context)

/**
 * Restore preemption state
 *
 * @param context name of local variable which stored the context
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task (RT, non-RT)
 *
 * Rescheduling: possible.
 */
#define rtdm_lock_irqrestore(context)           \
    rthal_local_irq_restore(context)
/** @} */

/** @} */


/* --- Interrupt management services --- */
/*!
 * @addtogroup rtdmirq
 * @{
 */

typedef xnintr_t                    rtdm_irq_t;

/**
 * Interrupt handler
 *
 * @param[in] irq_handle IRQ handle as returned by rtdm_irq_request()
 *
 * @return 0 or a combination of @ref RTDM_IRQ_xxx flags
 */
typedef int (*rtdm_irq_handler_t)(rtdm_irq_t *irq_handle);


/*!
 * @anchor RTDM_IRQ_xxx   @name RTDM_IRQ_xxx
 * Return flags of interrupt handlers
 * @{
 */
/** Propagate unhandled interrupt to possible other handlers */
#define RTDM_IRQ_PROPAGATE          XN_ISR_CHAINED
/** Re-enable interrupt line on return */
#define RTDM_IRQ_ENABLE             XN_ISR_ENABLE
/** @} */

/**
 * Retrieve IRQ handler argument
 *
 * @param irq_handle IRQ handle
 * @param type Type of the pointer to return
 *
 * @return The argument pointer registered on rtdm_irq_request() is returned,
 * type-casted to the specified @a type.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Interrupt service routine
 *
 * Rescheduling: never.
 */
#define rtdm_irq_get_arg(irq_handle, type)  ((type *)irq_handle->cookie)
/** @} */

static inline int rtdm_irq_request(rtdm_irq_t *irq_handle,
                                   unsigned int irq_no,
                                   rtdm_irq_handler_t handler,
                                   unsigned long flags,
                                   const char *device_name,
                                   void *arg)
{
    xnintr_init(irq_handle, irq_no, handler, NULL, flags);
    return xnintr_attach(irq_handle, arg);
}

static inline int rtdm_irq_free(rtdm_irq_t *irq_handle)
{
    return xnintr_detach(irq_handle);
}

static inline int rtdm_irq_enable(rtdm_irq_t *irq_handle)
{
    return xnintr_enable(irq_handle);
}

static inline int rtdm_irq_disable(rtdm_irq_t *irq_handle)
{
    return xnintr_disable(irq_handle);
}


/* --- non-real-time signalling services --- */

/*!
 * @addtogroup nrtsignal
 * @{
 */

typedef unsigned                    rtdm_nrtsig_t;

/**
 * Non-real-time signal handler
 *
 * @param[in] nrt_sig signal handle as returned by rtdm_nrtsig_init()
 *
 * @note The signal handler will run in soft-IRQ context of the non-real-time
 * subsystem. Note the implications of this context, e.g. no invocation of
 * blocking operations.
 */
typedef void (*rtdm_nrtsig_handler_t)(rtdm_nrtsig_t nrt_sig);
/** @} */


static inline int rtdm_nrtsig_init(rtdm_nrtsig_t *nrt_sig,
                                   rtdm_nrtsig_handler_t handler)
{
    *nrt_sig = rthal_alloc_virq();

    if (*nrt_sig == 0)
        return -EAGAIN;

    rthal_virtualize_irq(rthal_root_domain, *nrt_sig, handler, NULL,
                         IPIPE_HANDLE_MASK);
    return 0;
}

static inline void rtdm_nrtsig_destroy(rtdm_nrtsig_t *nrt_sig)
{
    rthal_free_virq(*nrt_sig);
}

static inline void rtdm_nrtsig_pend(rtdm_nrtsig_t *nrt_sig)
{
    rthal_trigger_irq(*nrt_sig);
}


/* --- task and timing services --- */
/*!
 * @addtogroup rtdmtask
 * @{
 */

typedef xnthread_t                  rtdm_task_t;

/**
 * Real-time task procedure
 *
 * @param[in,out] arg argument as passed to rtdm_task_init()
 */
typedef void (*rtdm_task_proc_t)(void *arg);


/*!
 * @anchor taskprio @name Task Priority Range
 * Maximum and minimum task priorities
 * @{ */
#define RTDM_TASK_LOWEST_PRIORITY   XNCORE_LOW_PRIO
#define RTDM_TASK_HIGHEST_PRIORITY  XNCORE_HIGH_PRIO
/** @} */

/*!
 * @anchor changetaskprio @name Task Priority Modification
 * Raise or lower task priorities by one level
 * @{ */
#define RTDM_TASK_RAISE_PRIORITY    (+1)
#define RTDM_TASK_LOWER_PRIORITY    (-1)
/** @} */

/** @} */

int rtdm_task_init(rtdm_task_t *task, const char *name,
                   rtdm_task_proc_t task_proc, void *arg,
                   int priority, uint64_t period);

static inline void rtdm_task_destroy(rtdm_task_t *task)
{
    xnpod_delete_thread(task);
}

void rtdm_task_join_nrt(rtdm_task_t *task, unsigned int poll_delay);

static inline void rtdm_task_set_priority(rtdm_task_t *task, int priority)
{
    xnpod_renice_thread(task, priority);
    xnpod_schedule();
}

static inline int rtdm_task_set_period(rtdm_task_t *task, uint64_t period)
{
    return xnpod_set_thread_periodic(task, XN_INFINITE,
                                     xnpod_ns2ticks(period));
}

static inline int rtdm_task_unblock(rtdm_task_t *task)
{
    int res = xnpod_unblock_thread(task);

    xnpod_schedule();
    return res;
}

static inline rtdm_task_t *rtdm_task_current(void)
{
    return xnpod_current_thread();
}

static inline int rtdm_task_wait_period(void)
{
    return xnpod_wait_thread_period();
}

int rtdm_task_sleep(uint64_t delay);
int rtdm_task_sleep_until(uint64_t wakeup_time);
void rtdm_task_busy_sleep(uint64_t delay);


/* --- timeout sequences */

typedef uint64_t                    rtdm_toseq_t;

static inline void rtdm_toseq_init(rtdm_toseq_t *timeout_seq, int64_t timeout)
{
    *timeout_seq = xnpod_get_time() + xnpod_ns2ticks(timeout);
}


/* --- event services --- */

typedef struct {
    unsigned long                   pending;
    xnsynch_t                       synch_base;
} rtdm_event_t;

static inline void rtdm_event_init(rtdm_event_t *event, unsigned long pending)
{
    event->pending = pending;
    xnsynch_init(&event->synch_base, XNSYNCH_PRIO);
}

void _rtdm_synch_flush(xnsynch_t *synch, unsigned long reason);

static inline void rtdm_event_destroy(rtdm_event_t *event)
{
    _rtdm_synch_flush(&event->synch_base, XNRMID);
}

int rtdm_event_wait(rtdm_event_t *event);
int rtdm_event_timedwait(rtdm_event_t *event, int64_t timeout,
                         rtdm_toseq_t *timeout_seq);
void rtdm_event_signal(rtdm_event_t *event);

static inline void rtdm_event_pulse(rtdm_event_t *event)
{
    _rtdm_synch_flush(&event->synch_base, 0);
}

static inline void rtdm_event_clear(rtdm_event_t *event)
{
    event->pending = 0;
}


/* --- semaphore services --- */

typedef struct {
    unsigned long                   value;
    xnsynch_t                       synch_base;
} rtdm_sem_t;

static inline void rtdm_sem_init(rtdm_sem_t *sem, unsigned long value)
{
    sem->value = value;
    xnsynch_init(&sem->synch_base, XNSYNCH_PRIO);
}

static inline void rtdm_sem_destroy(rtdm_sem_t *sem)
{
    _rtdm_synch_flush(&sem->synch_base, XNRMID);
}

int rtdm_sem_down(rtdm_sem_t *sem);
int rtdm_sem_timeddown(rtdm_sem_t *sem, int64_t timeout,
                       rtdm_toseq_t *timeout_seq);
void rtdm_sem_up(rtdm_sem_t *sem);


/* --- mutex services --- */

typedef struct {
    unsigned long                   locked;
    xnsynch_t                       synch_base;
} rtdm_mutex_t;

static inline void rtdm_mutex_init(rtdm_mutex_t *mutex)
{
    mutex->locked = 0;
    xnsynch_init(&mutex->synch_base, XNSYNCH_PRIO|XNSYNCH_PIP);
}

static inline void rtdm_mutex_destroy(rtdm_mutex_t *mutex)
{
    _rtdm_synch_flush(&mutex->synch_base, XNRMID);
}

int rtdm_mutex_lock(rtdm_mutex_t *mutex);
int rtdm_mutex_timedlock(rtdm_mutex_t *mutex, int64_t timeout,
                         rtdm_toseq_t *timeout_seq);
void rtdm_mutex_unlock(rtdm_mutex_t *mutex);


/* --- utility functions --- */

#define rtdm_printk(format, ...)    printk(format, ##__VA_ARGS__)

static inline void *rtdm_malloc(size_t size)
{
    return xnmalloc(size);
}

static inline void rtdm_free(void *ptr)
{
    xnfree(ptr);
}

static inline int rtdm_read_user_ok(rtdm_user_info_t *user_info,
                                    const void __user *ptr, size_t size)
{
    return __xn_access_ok(user_info, VERIFY_READ, ptr, size);
}

static inline int rtdm_rw_user_ok(rtdm_user_info_t *user_info,
                                  const void __user *ptr, size_t size)
{
    return __xn_access_ok(user_info, VERIFY_WRITE, ptr, size);
}

static inline int rtdm_copy_from_user(rtdm_user_info_t *user_info,
                                      void *dst, const void __user *src,
                                      size_t size)
{
    return __xn_copy_from_user(user_info, dst, src, size);
}

static inline int rtdm_copy_to_user(rtdm_user_info_t *user_info,
                                    void __user *dst, const void *src,
                                    size_t size)
{
    return __xn_copy_to_user(user_info, dst, src, size);
}

static inline int rtdm_strncpy_from_user(rtdm_user_info_t *user_info,
                                         char *dst,
                                         const char __user *src,
                                         size_t count)
{
    if (unlikely(__xn_access_ok(user_info, VERIFY_READ, src, 1)))
        return -EFAULT;
    return __xn_strncpy_from_user(user_info, dst, src, count);
}

static inline int rtdm_in_rt_context(void)
{
    return (rthal_current_domain != rthal_root_domain);
}

int rtdm_exec_in_rt(struct rtdm_dev_context *context,
                    rtdm_user_info_t *user_info, void *arg,
                    rtdm_rt_handler_t handler);

#endif /* _RTDM_DRIVER_H */
