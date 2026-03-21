/*
 * Orbuculum Zephyr Object Tracking — Trace Hook Overrides
 *
 * This header is included by the tracing.h wrapper (see tracing.h in this
 * directory) AFTER Zephyr's own tracing.h has been fully processed.
 * It #undefs the no-op macros and redefines them with DWT comp1 writes.
 *
 * Also define the trace variable in any .c file:
 *
 *   volatile uint32_t rtos_obj_trace __attribute__((used));
 *
 * See Docs/rtos/zephyr/zephyr-object-tracking.md for full documentation.
 */

#ifndef ORBUCULUM_OBJ_TRACE_H
#define ORBUCULUM_OBJ_TRACE_H

#include <stdint.h>

extern volatile uint32_t rtos_obj_trace;

/* Type tags — bits [1:0] */
#define ZEPHYR_OBJ_MUTEX  0u  /* 00 */
#define ZEPHYR_OBJ_SEM    1u  /* 01 */
#define ZEPHYR_OBJ_MSGQ   2u  /* 10 */
#define ZEPHYR_OBJ_EVENT  3u  /* 11 */

/* Release flag — bit [2] */
#define ZEPHYR_RELEASE_BIT 4u

/* ================================================================
 * Blocking hooks (acquire)
 * ================================================================ */

#undef sys_port_trace_k_mutex_lock_blocking
#define sys_port_trace_k_mutex_lock_blocking(mutex, timeout) \
    do { rtos_obj_trace = (uint32_t)(mutex) | ZEPHYR_OBJ_MUTEX; } while (0)

#undef sys_port_trace_k_sem_take_blocking
#define sys_port_trace_k_sem_take_blocking(sem, timeout) \
    do { rtos_obj_trace = (uint32_t)(sem) | ZEPHYR_OBJ_SEM; } while (0)

#undef sys_port_trace_k_msgq_get_blocking
#define sys_port_trace_k_msgq_get_blocking(msgq, timeout) \
    do { rtos_obj_trace = (uint32_t)(msgq) | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_msgq_put_blocking
#define sys_port_trace_k_msgq_put_blocking(msgq, timeout) \
    do { rtos_obj_trace = (uint32_t)(msgq) | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_event_wait_blocking
#define sys_port_trace_k_event_wait_blocking(event, events, options, timeout) \
    do { rtos_obj_trace = (uint32_t)(event) | ZEPHYR_OBJ_EVENT; } while (0)

#undef sys_port_trace_k_pipe_read_blocking
#define sys_port_trace_k_pipe_read_blocking(pipe, timeout) \
    do { rtos_obj_trace = (uint32_t)(pipe) | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_pipe_write_blocking
#define sys_port_trace_k_pipe_write_blocking(pipe, timeout) \
    do { rtos_obj_trace = (uint32_t)(pipe) | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_mbox_get_blocking
#define sys_port_trace_k_mbox_get_blocking(mbox, timeout) \
    do { rtos_obj_trace = (uint32_t)(mbox) | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_mbox_message_put_blocking
#define sys_port_trace_k_mbox_message_put_blocking(mbox, timeout) \
    do { rtos_obj_trace = (uint32_t)(mbox) | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_mem_slab_alloc_blocking
#define sys_port_trace_k_mem_slab_alloc_blocking(slab, timeout) \
    do { rtos_obj_trace = (uint32_t)(slab) | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_condvar_wait_blocking
#define sys_port_trace_k_condvar_wait_blocking(condvar, timeout) \
    do { rtos_obj_trace = (uint32_t)(condvar) | ZEPHYR_OBJ_EVENT; } while (0)

#undef sys_port_trace_k_stack_pop_blocking
#define sys_port_trace_k_stack_pop_blocking(stack, timeout) \
    do { rtos_obj_trace = (uint32_t)(stack) | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_thread_sleep_enter
#define sys_port_trace_k_thread_sleep_enter(timeout) \
    do { rtos_obj_trace = 0; } while (0)

/* ================================================================
 * Release hooks (optional — enables precise contention timing)
 *
 * Without these, C|0 is emitted at the next context switch (still
 * functional).  With these, C|0 is emitted at the exact release
 * time, showing true contention vs scheduling latency in Perfetto.
 *
 * Release hooks are auto-detected per object on first release event.
 * ================================================================ */

#undef sys_port_trace_k_mutex_unlock_exit
#define sys_port_trace_k_mutex_unlock_exit(mutex, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(mutex) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_MUTEX; } while (0)

#undef sys_port_trace_k_sem_give_exit
#define sys_port_trace_k_sem_give_exit(sem) \
    do { rtos_obj_trace = (uint32_t)(sem) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_SEM; } while (0)

#undef sys_port_trace_k_msgq_put_exit
#define sys_port_trace_k_msgq_put_exit(msgq, timeout, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(msgq) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_msgq_get_exit
#define sys_port_trace_k_msgq_get_exit(msgq, timeout, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(msgq) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_event_post_exit
#define sys_port_trace_k_event_post_exit(event, events, events_mask) \
    do { rtos_obj_trace = (uint32_t)(event) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_EVENT; } while (0)

#undef sys_port_trace_k_pipe_write_exit
#define sys_port_trace_k_pipe_write_exit(pipe, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(pipe) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_pipe_read_exit
#define sys_port_trace_k_pipe_read_exit(pipe, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(pipe) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_mbox_message_put_exit
#define sys_port_trace_k_mbox_message_put_exit(mbox, timeout, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(mbox) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_mbox_get_exit
#define sys_port_trace_k_mbox_get_exit(mbox, timeout, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(mbox) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_mem_slab_free_exit
#define sys_port_trace_k_mem_slab_free_exit(slab) \
    do { rtos_obj_trace = (uint32_t)(slab) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_MSGQ; } while (0)

#undef sys_port_trace_k_condvar_signal_exit
#define sys_port_trace_k_condvar_signal_exit(condvar, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(condvar) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_EVENT; } while (0)

#undef sys_port_trace_k_condvar_broadcast_exit
#define sys_port_trace_k_condvar_broadcast_exit(condvar, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(condvar) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_EVENT; } while (0)

#undef sys_port_trace_k_stack_push_exit
#define sys_port_trace_k_stack_push_exit(stack, ret) \
    do { if ((ret) == 0) rtos_obj_trace = (uint32_t)(stack) | ZEPHYR_RELEASE_BIT | ZEPHYR_OBJ_MSGQ; } while (0)

#endif
