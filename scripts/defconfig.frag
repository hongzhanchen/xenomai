
#
# Interrupt pipeline
#
CONFIG_IPIPE=y
CONFIG_IPIPE_EXTENDED=y
# CONFIG_IPIPE_STATS is not set

#
# Real-time sub-system
#
CONFIG_XENOMAI=y
CONFIG_XENO_OPT_NUCLEUS=y

#
# Nucleus options
#
CONFIG_XENO_OPT_PERVASIVE=y
CONFIG_XENO_OPT_PIPE=y
CONFIG_XENO_OPT_PIPE_NRDEV=32
CONFIG_XENO_OPT_SYS_HEAPSZ=128
# CONFIG_XENO_OPT_ISHIELD is not set
CONFIG_XENO_OPT_STATS=y
# CONFIG_XENO_OPT_DEBUG is not set
# CONFIG_XENO_OPT_WATCHDOG is not set
# CONFIG_XENO_OPT_TIMING_PERIODIC is not set
CONFIG_XENO_OPT_TIMING_PERIOD=0
CONFIG_XENO_OPT_TIMING_TIMERLAT=0
CONFIG_XENO_OPT_TIMING_SCHEDLAT=0

#
# Scalability options
#
# CONFIG_XENO_OPT_SCALABLE_SCHED is not set

#
# LTT tracepoints filtering
#
# CONFIG_XENO_OPT_FILTER_EVIRQ is not set
# CONFIG_XENO_OPT_FILTER_EVTHR is not set
# CONFIG_XENO_OPT_FILTER_EVSYS is not set
# CONFIG_XENO_OPT_FILTER_EVALL is not set

#
# Interfaces
#
CONFIG_XENO_SKIN_NATIVE=y

#
# Native interface options
#
CONFIG_XENO_OPT_NATIVE_REGISTRY=y
CONFIG_XENO_OPT_NATIVE_REGISTRY_NRSLOTS=512
CONFIG_XENO_OPT_NATIVE_PIPE=y
CONFIG_XENO_OPT_NATIVE_PIPE_BUFSZ=4096
CONFIG_XENO_OPT_NATIVE_SEM=y
CONFIG_XENO_OPT_NATIVE_EVENT=y
CONFIG_XENO_OPT_NATIVE_MUTEX=y
CONFIG_XENO_OPT_NATIVE_COND=y
CONFIG_XENO_OPT_NATIVE_QUEUE=y
CONFIG_XENO_OPT_NATIVE_HEAP=y
CONFIG_XENO_OPT_NATIVE_ALARM=y
CONFIG_XENO_OPT_NATIVE_MPS=y
CONFIG_XENO_OPT_NATIVE_INTR=y
CONFIG_XENO_SKIN_POSIX=m
CONFIG_XENO_SKIN_RTDM=y
# CONFIG_XENO_SKIN_UVM is not set
# CONFIG_XENO_SKIN_PSOS is not set
# CONFIG_XENO_SKIN_VXWORKS is not set
# CONFIG_XENO_SKIN_VRTX is not set
# CONFIG_XENO_SKIN_UITRON is not set
# CONFIG_XENO_SKIN_RTAI is not set

#
# Real-time drivers
#
# CONFIG_XENO_DRIVERS_16550A is not set

#
# Machine
#
# CONFIG_XENO_HW_FPU is not set

#
# NMI watchdog
#
# CONFIG_XENO_HW_NMI_DEBUG_LATENCY is not set

#
# SMI workaround
#
CONFIG_XENO_HW_SMI_DETECT_DISABLE=y
