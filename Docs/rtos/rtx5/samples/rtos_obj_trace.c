/*
 * Orbuculum RTX5 Object Tracking — EVR Hook Overrides
 *
 * Add this file to your firmware project build.  These functions override
 * the __WEAK stubs in rtx_evr.c to write object addresses (with type and
 * release bits) to rtos_obj_trace, which triggers DWT comparator 1.
 *
 * Prerequisites in RTX_Config.h:
 *   OS_EVR_MUTEX = 1, OS_EVR_SEMAPHORE = 1, OS_EVR_EVFLAGS = 1,
 *   OS_EVR_MSGQUEUE = 1, OS_EVR_MEMPOOL = 1, OS_EVR_WAIT = 1
 *   EVR_RTX_DISABLE must NOT be defined.
 *
 * See Docs/rtos/rtx5/rtx5-object-tracking.md for full documentation.
 */

#include <stdint.h>

volatile uint32_t rtos_obj_trace __attribute__((used));

#define RTX5_RELEASE_BIT 0x80000000u   /* bit [31] = release event */

/* ================================================================
 * Blocking hooks (acquire) — override __WEAK stubs in rtx_evr.c
 * ================================================================ */

void EvrRtxMutexAcquirePending(void *mutex_id, uint32_t timeout)
{
    (void)timeout;
    rtos_obj_trace = (uint32_t)mutex_id;
}

void EvrRtxSemaphoreAcquirePending(void *semaphore_id, uint32_t timeout)
{
    (void)timeout;
    rtos_obj_trace = (uint32_t)semaphore_id;
}

void EvrRtxEventFlagsWaitPending(void *ef_id, uint32_t flags, uint32_t options, uint32_t timeout)
{
    (void)flags;
    (void)options;
    (void)timeout;
    rtos_obj_trace = (uint32_t)ef_id;
}

void EvrRtxMessageQueueGetPending(void *mq_id, void *msg_ptr, uint32_t timeout)
{
    (void)msg_ptr;
    (void)timeout;
    rtos_obj_trace = (uint32_t)mq_id;
}

void EvrRtxMessageQueuePutPending(void *mq_id, const void *msg_ptr, uint32_t timeout)
{
    (void)msg_ptr;
    (void)timeout;
    rtos_obj_trace = (uint32_t)mq_id;
}

void EvrRtxMessageQueueInsertPending(void *mq_id, const void *msg_ptr)
{
    (void)msg_ptr;
    rtos_obj_trace = (uint32_t)mq_id;
}

void EvrRtxMemoryPoolAllocPending(void *mp_id, uint32_t timeout)
{
    (void)timeout;
    rtos_obj_trace = (uint32_t)mp_id;
}

void EvrRtxDelay(uint32_t ticks)
{
    (void)ticks;
    rtos_obj_trace = 0;
}

void EvrRtxDelayUntil(uint32_t ticks)
{
    (void)ticks;
    rtos_obj_trace = 0;
}

/* ================================================================
 * Release hooks (optional — enables precise contention timing)
 *
 * Without these, C|0 is emitted at the next context switch (still
 * functional).  With these, C|0 is emitted at the exact release
 * time, showing true contention vs scheduling latency in Perfetto.
 *
 * Release hooks are auto-detected per object on first release event.
 * ================================================================ */

void EvrRtxMutexReleased(void *mutex_id, uint32_t lock)
{
    (void)lock;
    rtos_obj_trace = (uint32_t)mutex_id | RTX5_RELEASE_BIT;
}

void EvrRtxSemaphoreReleased(void *semaphore_id, uint32_t tokens)
{
    (void)tokens;
    rtos_obj_trace = (uint32_t)semaphore_id | RTX5_RELEASE_BIT;
}

void EvrRtxEventFlagsSet(void *ef_id, uint32_t flags)
{
    (void)flags;
    rtos_obj_trace = (uint32_t)ef_id | RTX5_RELEASE_BIT;
}

void EvrRtxMessageQueueInserted(void *mq_id, const void *msg_ptr)
{
    (void)msg_ptr;
    rtos_obj_trace = (uint32_t)mq_id | RTX5_RELEASE_BIT;
}

void EvrRtxMessageQueueRetrieved(void *mq_id, void *msg_ptr)
{
    (void)msg_ptr;
    rtos_obj_trace = (uint32_t)mq_id | RTX5_RELEASE_BIT;
}

void EvrRtxMemoryPoolDeallocated(void *mp_id, void *block)
{
    (void)block;
    rtos_obj_trace = (uint32_t)mp_id | RTX5_RELEASE_BIT;
}
