#ifndef CAPMIX_CONFIG_H
#define CAPMIX_CONFIG_H

/* CapMix — capability hybrid kernel
 * Skeleton (irreversible): Zircon caps + L4-fast IPC + XNU in-kernel FS/net
 * + user-mode drivers. Layers independently pick Linux / NT / XNU.
 */

#define CAPMIX_NAME        "CapMix"
#define CAPMIX_VERSION     "0.8.0-phase8"
#define CAPMIX_BANNER      "CapMix " CAPMIX_VERSION " — capability hybrid kernel"

#define CM_BUDDY_MAX_ORDER     11          /* 4 KiB .. 8 MiB */
#define CM_SLAB_MAX_CLASSES    16
#define CM_MAX_CPUS            8
#define CM_MAX_THREADS         128
#define CM_MAX_TASKS           64
#define CM_MAX_CAPS            4096
#define CM_HANDLES_PER_TASK    256
#define CM_MAX_CHANNELS        256
#define CM_IPC_SMALL_BYTES     64          /* register-sized payload */
#define CM_TRACE_RING_SIZE     2048
#define CM_COMMIT_RESERVE_PCT  6           /* Windows-like commit limit headroom */

/* Memory policy: Windows no-overcommit + forced swap accounting */
#define CM_NO_OVERCOMMIT       1

/* THP: madvise only, never always (Linux lesson) */
#define CM_THP_ALWAYS          0

/* Observability is a capability, never a global SIP-style kill switch */
#define CM_OBS_AS_CAPABILITY   1

/* fork is POSIX compat only; posix_spawn is the first-class path */
#define CM_FORK_IS_COMPAT      1

#endif
