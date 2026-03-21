/*
 * Orbuculum FreeRTOS Object Tracking — Trace Hook Definitions
 *
 * Include this header from FreeRTOSConfig.h (or paste the contents directly).
 * Also define the trace variable in a .c file:
 *
 *   volatile uint32_t rtos_obj_trace __attribute__((used));
 *
 * See Docs/rtos/freertos/freertos-object-tracking.md for full documentation.
 */

#ifndef ORBUCULUM_FREERTOS_TRACE_H
#define ORBUCULUM_FREERTOS_TRACE_H

#include <stdint.h>

extern volatile uint32_t rtos_obj_trace;

#define FREERTOS_RELEASE_BIT 4u   /* bit [2] = release event */

/* ================================================================
 * Blocking hooks (acquire) — DWT comp1 writes
 * ================================================================ */

/* Queue-based objects: mutex, semaphore, queue (tag=00) */
#define traceBLOCKING_ON_QUEUE_RECEIVE(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue); } while(0)
#define traceBLOCKING_ON_QUEUE_SEND(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue); } while(0)

/* Event Groups (tag=01) */
#define traceEVENT_GROUP_WAIT_BITS_BLOCK(xEventGroup, uxBitsToWaitFor) \
    do { rtos_obj_trace = (uint32_t)(xEventGroup) | 1u; } while(0)
#define traceEVENT_GROUP_SYNC_BLOCK(xEventGroup, uxBitsToSet, uxBitsToWaitFor) \
    do { rtos_obj_trace = (uint32_t)(xEventGroup) | 1u; } while(0)

/* Stream Buffers / Message Buffers (tag=10) */
#define traceBLOCKING_ON_STREAM_BUFFER_SEND(xStreamBuffer) \
    do { rtos_obj_trace = (uint32_t)(xStreamBuffer) | 2u; } while(0)
#define traceBLOCKING_ON_STREAM_BUFFER_RECEIVE(xStreamBuffer) \
    do { rtos_obj_trace = (uint32_t)(xStreamBuffer) | 2u; } while(0)

/* Delays: write 0 for prev_state=S in ftrace */
#define traceTASK_DELAY()            do { rtos_obj_trace = 0; } while(0)
#define traceTASK_DELAY_UNTIL(x)     do { rtos_obj_trace = 0; } while(0)

/* ================================================================
 * Release hooks (optional — enables precise contention timing)
 *
 * Without these, C|0 is emitted at the next context switch (still
 * functional).  With these, C|0 is emitted at the exact release
 * time, showing true contention vs scheduling latency in Perfetto.
 *
 * Release hooks are auto-detected per object on first release event.
 * ================================================================ */

/* Queue_t release: send/receive on queue, mutex, semaphore (tag=00) */
#define traceQUEUE_SEND(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue) | FREERTOS_RELEASE_BIT; } while(0)
#define traceQUEUE_SEND_FROM_ISR(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue) | FREERTOS_RELEASE_BIT; } while(0)
#define traceQUEUE_RECEIVE(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue) | FREERTOS_RELEASE_BIT; } while(0)
#define traceQUEUE_RECEIVE_FROM_ISR(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue) | FREERTOS_RELEASE_BIT; } while(0)
#define traceGIVE_MUTEX_RECURSIVE(pxMutex) \
    do { rtos_obj_trace = (uint32_t)(pxMutex) | FREERTOS_RELEASE_BIT; } while(0)

/* Event Group release (tag=01) */
#define traceEVENT_GROUP_SET_BITS(xEventGroup, uxBitsToSet) \
    do { rtos_obj_trace = (uint32_t)(xEventGroup) | FREERTOS_RELEASE_BIT | 1u; } while(0)

/* Stream Buffer release (tag=10) */
#define traceSTREAM_BUFFER_SEND(xStreamBuffer, xBytesSent) \
    do { rtos_obj_trace = (uint32_t)(xStreamBuffer) | FREERTOS_RELEASE_BIT | 2u; } while(0)
#define traceSTREAM_BUFFER_RECEIVE(xStreamBuffer, xReceivedLength) \
    do { rtos_obj_trace = (uint32_t)(xStreamBuffer) | FREERTOS_RELEASE_BIT | 2u; } while(0)

#endif /* ORBUCULUM_FREERTOS_TRACE_H */
