/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * FreeRTOS Thread Tracking Support for Orbuculum
 * ===============================================
 * Host-side header for decoding FreeRTOS data structures
 * on a 32-bit ARMv7-M target (Cortex-M).
 *
 * Supported: FreeRTOS v10.1.0 and later
 */

#ifndef _FREERTOS_H_
#define _FREERTOS_H_

#include <stdint.h>
#include <rtos_support.h>

/* --------------------------------------------------------------------------
 * Target layout assumptions
 * --------------------------------------------------------------------------
 * - ARMv7-M (Cortex-M) 32-bit
 * - Little-endian
 * - Natural alignment
 * - FreeRTOS v10.1+ standard configuration WITHOUT MPU
 * - configMAX_TASK_NAME_LEN = 16 (default) or user-specified
 *
 * Structure sizes (32-bit ARM):
 * - ListItem_t: 20 bytes (5 x uint32_t)
 * - StackType_t: 4 bytes
 * - UBaseType_t: 4 bytes
 */

/* --------------------------------------------------------------------------
 * FreeRTOS kernel symbols
 * --------------------------------------------------------------------------
 */

/* Primary symbol - pointer to current running task's TCB */
#define FREERTOS_SYM_PX_CURRENT_TCB             "pxCurrentTCB"

/* Detection symbol - if this exists, FreeRTOS is present */
#define FREERTOS_SYM_PX_READY_TASKS_LISTS       "pxReadyTasksLists"

/* Scheduler state - 1 if running */
#define FREERTOS_SYM_X_SCHEDULER_RUNNING        "xSchedulerRunning"

/* Number of tasks created */
#define FREERTOS_SYM_UX_CURRENT_NUMBER_OF_TASKS "uxCurrentNumberOfTasks"

/* Priority info - optional since v7.5.3 */
#define FREERTOS_SYM_UX_TOP_USED_PRIORITY       "uxTopUsedPriority"

/* --------------------------------------------------------------------------
 * FreeRTOS TCB (Task Control Block) layout - Standard Configuration
 * --------------------------------------------------------------------------
 * Based on FreeRTOS v10.x source (tasks.c)
 *
 * typedef struct tskTaskControlBlock {
 *     volatile StackType_t *pxTopOfStack;    // offset 0,  4 bytes - MUST BE FIRST
 *
 *     #if (portUSING_MPU_WRAPPERS == 1)
 *         xMPU_SETTINGS xMPUSettings;        // NOT SUPPORTED
 *     #endif
 *
 *     ListItem_t xStateListItem;             // offset 4,  20 bytes
 *     ListItem_t xEventListItem;             // offset 24, 20 bytes
 *     UBaseType_t uxPriority;                // offset 44, 4 bytes
 *     StackType_t *pxStack;                  // offset 48, 4 bytes
 *     char pcTaskName[configMAX_TASK_NAME_LEN]; // offset 52, varies
 *     ...
 * } TCB_t;
 *
 * ListItem_t structure (20 bytes on 32-bit):
 *     TickType_t xItemValue;        // 4 bytes
 *     struct xLIST_ITEM *pxNext;    // 4 bytes
 *     struct xLIST_ITEM *pxPrevious;// 4 bytes
 *     void *pvOwner;                // 4 bytes  (TCB pointer at offset +12)
 *     struct xLIST *pxContainer;    // 4 bytes
 */

/* TCB offsets - Standard FreeRTOS v10.x WITHOUT MPU (Cortex-M) */
#define FREERTOS_TCB_TOP_OF_STACK_OFFSET    0u   /* volatile StackType_t* - FIRST */
#define FREERTOS_TCB_STATE_LIST_OFFSET      4u   /* ListItem_t (20 bytes) */
#define FREERTOS_TCB_EVENT_LIST_OFFSET      24u  /* ListItem_t (20 bytes) */
#define FREERTOS_TCB_PRIORITY_OFFSET        44u  /* UBaseType_t */
#define FREERTOS_TCB_STACK_OFFSET           48u  /* StackType_t* */
#define FREERTOS_TCB_NAME_OFFSET            52u  /* char[configMAX_TASK_NAME_LEN] */

/* Offset for pxEndOfStack - only present if configRECORD_STACK_HIGH_ADDRESS=1 */
/* This is after pcTaskName, so offset = 52 + configMAX_TASK_NAME_LEN (typically 16) */
#define FREERTOS_TCB_END_OF_STACK_OFFSET    68u  /* StackType_t* (optional) */

/* ListItem_t offsets (for traversing task lists) */
#define FREERTOS_LIST_ITEM_VALUE_OFFSET     0u
#define FREERTOS_LIST_ITEM_NEXT_OFFSET      4u
#define FREERTOS_LIST_ITEM_PREV_OFFSET      8u
#define FREERTOS_LIST_ITEM_OWNER_OFFSET     12u  /* Points back to TCB */
#define FREERTOS_LIST_ITEM_CONTAINER_OFFSET 16u

/* List_t structure offsets */
#define FREERTOS_LIST_LENGTH_OFFSET         0u   /* Number of items in list */
#define FREERTOS_LIST_INDEX_OFFSET          4u   /* Pointer to current item */
#define FREERTOS_LIST_END_OFFSET            8u   /* MiniListItem_t at end */
#define FREERTOS_LIST_WIDTH                 20u  /* Total size of List_t */

/* Default task name length - used when DWARF not available */
#define FREERTOS_DEFAULT_MAX_TASK_NAME_LEN  16u

/* --------------------------------------------------------------------------
 * FreeRTOS task states
 * --------------------------------------------------------------------------
 * Note: FreeRTOS doesn't store state in TCB directly, state is implicit
 * based on which list the task is in (ready, blocked, suspended, etc.)
 */
#define FREERTOS_TASK_RUNNING               0
#define FREERTOS_TASK_READY                 1
#define FREERTOS_TASK_BLOCKED               2
#define FREERTOS_TASK_SUSPENDED             3
#define FREERTOS_TASK_DELETED               4

/* --------------------------------------------------------------------------
 * FreeRTOS priority values
 * --------------------------------------------------------------------------
 * - Priority 0 = tskIDLE_PRIORITY (lowest, idle task)
 * - Maximum = configMAX_PRIORITIES - 1
 * - Typical configMAX_PRIORITIES = 5 to 56
 */
#define FREERTOS_PRIORITY_IDLE              0
#define FREERTOS_MAX_PRIORITIES             63  /* Sanity check limit */

/* Idle task default name */
#define FREERTOS_IDLE_TASK_NAME             "IDLE"

/* Stack scanning limits for entry function detection */
#define FREERTOS_STACK_WORDS_PER_FRAME      24  /* Typical ARM registers per frame */
#define FREERTOS_MAX_FRAMES_TO_SCAN         3   /* Scan up to 3 stack frames */
#define FREERTOS_MAX_STACK_SCAN_WORDS       (FREERTOS_STACK_WORDS_PER_FRAME * FREERTOS_MAX_FRAMES_TO_SCAN)

/* --------------------------------------------------------------------------
 * FreeRTOS Queue types (ucQueueType in Queue_t)
 * --------------------------------------------------------------------------
 * All blocking objects are internally Queue_t. ucQueueType identifies kind.
 * Only present when configQUEUE_REGISTRY_SIZE > 0.
 */
/* Values from FreeRTOS queue.h: queueQUEUE_TYPE_* */
#define FREERTOS_QUEUE_TYPE_BASE    0   /* Plain queue / queue set */
#define FREERTOS_QUEUE_TYPE_MUTEX   1   /* Mutex */
#define FREERTOS_QUEUE_TYPE_CSEM    2   /* Counting semaphore */
#define FREERTOS_QUEUE_TYPE_BSEM    3   /* Binary semaphore */
#define FREERTOS_QUEUE_TYPE_RMUTEX  4   /* Recursive mutex */

/* --------------------------------------------------------------------------
 * Private data for FreeRTOS tracking
 * --------------------------------------------------------------------------
 */
struct freertos_private {
    /* Symbol addresses */
    uint32_t pxCurrentTCB_addr;          /* Address of pxCurrentTCB variable */
    uint32_t pxReadyTasksLists_addr;     /* Address of ready lists array */
    uint32_t xSchedulerRunning_addr;     /* Address of scheduler state */
    uint32_t uxCurrentNumberOfTasks_addr;/* Address of task count */
    uint32_t prvTaskExitError_addr;      /* Address of prvTaskExitError function */

    /* Configuration detected from ELF/target */
    uint8_t max_task_name_len;           /* configMAX_TASK_NAME_LEN */
    uint8_t max_priorities;              /* configMAX_PRIORITIES */

    /* TCB field offsets - detected from DWARF or defaults */
    uint8_t name_offset;                 /* Offset of pcTaskName in TCB */
    uint8_t priority_offset;             /* Offset of uxPriority in TCB */
    uint8_t end_of_stack_offset;         /* Offset of pxEndOfStack in TCB */
    bool has_end_of_stack;               /* Whether pxEndOfStack exists */

    /* Queue_t field offsets for object tracking (from DWARF) */
    int32_t queue_type_offset;           /* Queue_t.ucQueueType (-1 if absent) */
    bool has_queue_type;                 /* true if ucQueueType offset found */

    /* Queue registry for object name lookup */
    uint32_t queue_registry_addr;        /* Address of xQueueRegistry[] symbol */
    uint8_t queue_registry_size;         /* configQUEUE_REGISTRY_SIZE */
    uint8_t registry_item_size;          /* sizeof(QueueRegistryItem_t) */
    uint8_t registry_name_offset;        /* offset of pcQueueName in registry item */
    uint8_t registry_handle_offset;      /* offset of xHandle in registry item */
    bool has_queue_registry;             /* true if xQueueRegistry found */
};

/* API functions */
const char *freertosGetPriorityName(int8_t priority);
const struct rtosOps *freertosGetOps(void);

#endif /* _FREERTOS_H_ */
