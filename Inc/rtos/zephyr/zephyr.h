/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Zephyr RTOS Thread Tracking Support for Orbuculum
 * ==================================================
 * Host-side header for decoding Zephyr data structures
 * on a 32-bit ARMv7-M target (Cortex-M), single CPU only.
 *
 * Requires target firmware built with:
 *   CONFIG_DEBUG_THREAD_INFO=y  (enables offsets array + thread list + names)
 *
 * Supported: Zephyr 2.6+ (thread_info offsets array)
 */

#ifndef _ZEPHYR_H_
#define _ZEPHYR_H_

#include <stdint.h>
#include <rtos_support.h>

/* --------------------------------------------------------------------------
 * Zephyr kernel symbols
 * --------------------------------------------------------------------------
 */

/* Global kernel instance - presence confirms Zephyr */
#define ZEPHYR_SYM_KERNEL                   "_kernel"

/* Offsets array exported by subsys/debug/thread_info.c */
#define ZEPHYR_SYM_OFFSETS                  "_kernel_openocd_offsets"
#define ZEPHYR_SYM_OFFSETS_ALT              "_kernel_thread_info_offsets"

/* sizeof(size_t) on target - needed to interpret offsets array */
#define ZEPHYR_SYM_SIZE_T_SIZE              "_kernel_openocd_size_t_size"
#define ZEPHYR_SYM_SIZE_T_SIZE_ALT          "_kernel_thread_info_size_t_size"

/* Number of offsets in the array (optional) */
#define ZEPHYR_SYM_NUM_OFFSETS              "_kernel_openocd_num_offsets"
#define ZEPHYR_SYM_NUM_OFFSETS_ALT          "_kernel_thread_info_num_offsets"

/* --------------------------------------------------------------------------
 * Offsets array indices (from Zephyr subsys/debug/thread_info.c)
 * --------------------------------------------------------------------------
 * The target firmware exports an array of size_t values containing
 * struct offsets computed at compile time. 0xFFFFFFFF means "not available".
 */
enum zephyrOffsetsIndex {
    ZEPHYR_OFF_VERSION         = 0,   /* Always 1 */
    ZEPHYR_OFF_K_CURR_THREAD   = 1,   /* offsetof(struct _cpu, current) */
    ZEPHYR_OFF_K_THREADS       = 2,   /* offsetof(struct z_kernel, threads) */
    ZEPHYR_OFF_T_ENTRY         = 3,   /* offsetof(struct k_thread, entry) */
    ZEPHYR_OFF_T_NEXT_THREAD   = 4,   /* offsetof(struct k_thread, next_thread) */
    ZEPHYR_OFF_T_STATE         = 5,   /* offsetof(struct k_thread, base.thread_state) */
    ZEPHYR_OFF_T_USER_OPTIONS  = 6,   /* offsetof(struct k_thread, base.user_options) */
    ZEPHYR_OFF_T_PRIO          = 7,   /* offsetof(struct k_thread, base.prio) */
    ZEPHYR_OFF_T_STACK_PTR     = 8,   /* arch-specific stack pointer offset */
    ZEPHYR_OFF_T_NAME          = 9,   /* offsetof(struct k_thread, name) */
    ZEPHYR_OFF_T_ARCH          = 10,  /* offsetof(struct k_thread, arch) */
    ZEPHYR_OFF_T_PREEMPT_FLOAT = 11,  /* FPU preempt offset or 0xFFFFFFFF */
    ZEPHYR_OFF_T_COOP_FLOAT    = 12,  /* FPU coop offset or 0xFFFFFFFF */
    ZEPHYR_OFF_T_ARM_EXC_RET   = 13,  /* ARM exc_return offset or 0xFFFFFFFF */
    ZEPHYR_NUM_OFFSETS         = 14
};

/* Sentinel value meaning "offset not available" */
#define ZEPHYR_OFFSET_NOT_AVAILABLE 0xFFFFFFFF

/* --------------------------------------------------------------------------
 * Zephyr thread states (bitmask in base.thread_state)
 * --------------------------------------------------------------------------
 */
#define ZEPHYR_THREAD_DUMMY       0x01  /* Not a real thread */
#define ZEPHYR_THREAD_PENDING     0x02  /* Waiting on an object */
#define ZEPHYR_THREAD_SLEEPING    0x04  /* k_sleep() called */
#define ZEPHYR_THREAD_DEAD        0x08  /* Terminated */
#define ZEPHYR_THREAD_SUSPENDED   0x10  /* Suspended */
#define ZEPHYR_THREAD_ABORTING    0x20  /* Being aborted */
#define ZEPHYR_THREAD_SUSPENDING  0x40  /* Being suspended */
#define ZEPHYR_THREAD_QUEUED      0x80  /* In ready queue */

/* --------------------------------------------------------------------------
 * Default offsets for ARM Cortex-M single-CPU (common Zephyr config)
 * --------------------------------------------------------------------------
 * Used as fallback when DWARF and offsets array are unavailable.
 * These match a typical Zephyr 3.x build with:
 *   CONFIG_DEBUG_THREAD_INFO=y, CONFIG_THREAD_MONITOR=y,
 *   CONFIG_THREAD_NAME=y, single CPU, no SMP, no userspace
 */
#define ZEPHYR_DEFAULT_K_CURR_THREAD_OFFSET  0u   /* _cpu.current is first member */
#define ZEPHYR_DEFAULT_K_THREADS_OFFSET      8u   /* _kernel.threads after cpus[1] */

/* Default thread name max length */
#define ZEPHYR_DEFAULT_MAX_THREAD_NAME_LEN   32u

/* --------------------------------------------------------------------------
 * Private data for Zephyr tracking
 * --------------------------------------------------------------------------
 */
struct zephyr_private {
    /* Symbol addresses (from ELF via objdump) */
    uint32_t kernel_addr;                    /* Address of _kernel */
    uint32_t offsets_array_addr;             /* Address of _kernel_openocd_offsets */
    uint32_t size_t_size_addr;               /* Address of _kernel_openocd_size_t_size */

    /* Target's sizeof(size_t) - 4 for 32-bit ARM */
    uint8_t size_t_size;

    /* Offsets from the offsets array (or DWARF/defaults) */
    uint32_t off_k_curr_thread;              /* Offset of current thread ptr within _kernel */
    uint32_t off_k_threads;                  /* Offset of thread list head within _kernel */
    uint32_t off_t_entry;                    /* Offset of entry function within k_thread */
    uint32_t off_t_next_thread;              /* Offset of next_thread within k_thread */
    uint32_t off_t_state;                    /* Offset of thread_state within k_thread */
    uint32_t off_t_prio;                     /* Offset of prio within k_thread */
    uint32_t off_t_name;                     /* Offset of name[] within k_thread */
    uint32_t off_t_stack_ptr;                /* Offset of stack pointer within k_thread */

    /* Computed addresses */
    uint32_t current_thread_addr;            /* _kernel + off_k_curr_thread (DWT target) */

    /* Whether offsets were loaded from target memory */
    bool offsets_from_target;

    /* Max thread name length (from DWARF or default) */
    uint8_t max_thread_name_len;
};

/* API functions */
const char *zephyrGetPriorityName(int8_t priority);
const struct rtosOps *zephyrGetOps(void);

#endif /* _ZEPHYR_H_ */
