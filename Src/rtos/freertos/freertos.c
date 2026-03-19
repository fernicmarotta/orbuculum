
/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <symbols.h>
#include <rtos/freertos/freertos.h>
#include <telnet_client.h>
#include <generics.h>
#include <rtos_support.h>


/* -------------------------------------------------------------------------
 * Helper functions
 * ------------------------------------------------------------------------- */

static uint32_t simple_hash(const char *str)
{
    uint32_t hash = 5381;
    if (!str)
        return 0;
    while (*str)
        hash = ((hash << 5) + hash) + *str++;
    return hash;
}


const char *freertosGetPriorityName(int8_t priority)
{
    static char buf[16];
    if (priority < 0)
        return "Invalid";
    if (priority == 0)
        return "Idle(0)";
    snprintf(buf, sizeof(buf), "Pri(%d)", priority);
    return buf;
}


/* -------------------------------------------------------------------------
 * Thread info reading - split into focused functions
 * ------------------------------------------------------------------------- */

static bool read_thread_name(struct rtosThread *thread, uint32_t tcb_addr,
                             uint8_t name_offset, uint8_t max_name_len)
{
    if (!rtos_name_is_unknown(thread->name))
        return true;

    uint32_t name_addr = tcb_addr + name_offset;
    char name_buf[RTOS_THREAD_NAME_MAX_LEN] = {0};
    size_t read_len = (max_name_len < sizeof(name_buf) - 1) ? max_name_len : sizeof(name_buf) - 1;

    char *name_str = rtosReadMemoryString(name_addr, name_buf, read_len + 1);
    
    if (!name_str || name_buf[0] < 0x20 || name_buf[0] >= 0x7F)
    {
        /* Keep temporary name but don't cache permanently - will retry */
        if (thread->name[0] == 0)
            strcpy(thread->name, RTOS_NAME_NO_NAME);
        return false;
    }

    /* Truncate at first non-printable */
    for (size_t i = 0; name_buf[i] && i < read_len; i++)
    {
        if (name_buf[i] < 0x20 || name_buf[i] >= 0x7F)
        {
            name_buf[i] = '\0';
            break;
        }
    }

    strncpy(thread->name, name_buf, sizeof(thread->name) - 1);
    thread->name[sizeof(thread->name) - 1] = '\0';
    thread->name_ptr = name_addr;
    return true;
}


static void read_thread_priority(struct rtosThread *thread, uint32_t tcb_addr,
                                 uint8_t priority_offset)
{
    uint32_t priority_word = rtosReadMemoryWord(tcb_addr + priority_offset);
    thread->priority = (int8_t)(priority_word & 0xFF);

    if (thread->priority < 0 || thread->priority > FREERTOS_MAX_PRIORITIES)
    {
        genericsReport(V_WARN, "FreeRTOS: Suspicious priority %d at TCB=0x%08X" EOL,
                      thread->priority, tcb_addr);
    }
}


/*
 * Entry function detection via stack scan for prvTaskExitError
 * 
 * FreeRTOS doesn't store the entry function in TCB. However,
 * pxPortInitialiseStack() sets LR = prvTaskExitError. When the entry
 * function calls other functions, it saves this LR in the stack.
 *
 * See: https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/portable/GCC/ARM_CM4F/port.c#L205
 *
 * Method: Scan from pxEndOfStack for prvTaskExitError, PC is at addr+4.
 */
static void detect_entry_function(struct rtosThread *thread,
                                  struct SymbolSet *symbols,
                                  struct freertos_private *priv,
                                  uint32_t tcb_addr,
                                  uint32_t pxTopOfStack)
{
    thread->entry_func = 0;
    thread->entry_func_name = NULL;

    if (!priv || !priv->prvTaskExitError_addr || !priv->has_end_of_stack)
        return;

    /* pxTopOfStack == 0 means the TCB read failed — skip */
    if (!pxTopOfStack || pxTopOfStack == 0xFFFFFFFF)
        return;

    uint32_t pxEndOfStack = rtosReadMemoryWord(tcb_addr + priv->end_of_stack_offset);

    if (!pxEndOfStack || pxEndOfStack == 0xFFFFFFFF || pxEndOfStack <= pxTopOfStack)
        return;

    /* Sanity check: stack size must be reasonable (< 64KB) */
    uint32_t stack_size = pxEndOfStack - pxTopOfStack;
    if (stack_size > 0x10000)
    {
        genericsReport(V_DEBUG, "FreeRTOS: Stack range too large (0x%X), skipping scan for TCB=0x%08X" EOL,
                      stack_size, tcb_addr);
        return;
    }

    uint32_t scan_limit = (pxEndOfStack > FREERTOS_MAX_STACK_SCAN_WORDS * 4)
        ? pxEndOfStack - (FREERTOS_MAX_STACK_SCAN_WORDS * 4)
        : pxTopOfStack;
    if (scan_limit < pxTopOfStack)
        scan_limit = pxTopOfStack;

    /* Scan from below pxEndOfStack (it's usually fill pattern at the top) */
    uint32_t scan_start = pxEndOfStack - 4;
    int consecutive_zeros = 0;

    for (uint32_t addr = scan_start; addr >= scan_limit && addr >= 4; addr -= 4)
    {
        uint32_t val = rtosReadMemoryWord(addr);

        /* Abort if reads keep failing (non-existent memory returns 0) */
        if (val == 0)
        {
            if (++consecutive_zeros >= 3)
            {
                genericsReport(V_DEBUG, "FreeRTOS: Stack scan aborted at 0x%08X (read failures)" EOL, addr);
                return;
            }
            continue;
        }
        consecutive_zeros = 0;

        if ((val & ~1) == priv->prvTaskExitError_addr)
        {
            /* First try: original stack layout - entry PC is at addr+4 (above prvTaskExitError) */
            uint32_t entry_pc = rtosReadMemoryWord(addr + 4) & ~1;
            if (entry_pc && entry_pc != 0xFFFFFFFF && 
                entry_pc != 0xA5A5A5A4 && entry_pc != 0xA5A5A5A5 &&
                (entry_pc & 0xFFFFFF00) != 0xFFFFFF00)
            {
                const char *func_name = rtosLookupPointerAsFunction(symbols, entry_pc);
                if (func_name)
                {
                    thread->entry_func = entry_pc;
                    thread->entry_func_name = func_name;
                    return;
                }
            }
            
            /* Fallback: Scan DOWN from prvTaskExitError (toward SP) for entry function.
             * The entry function's return address is saved when it calls other functions.
             * Limit to 16 words (64 bytes) to avoid too many memory reads. */
            for (int i = 1; i <= 16; i++)
            {
                uint32_t candidate_addr = addr - (i * 4);
                if (candidate_addr < pxTopOfStack) break;
                
                uint32_t candidate = rtosReadMemoryWord(candidate_addr) & ~1;
                /* Filter out invalid values: NULL, fill patterns, EXC_RETURN (0xFFFFFFxx) */
                if (candidate && candidate != 0xFFFFFFFF && 
                    candidate != 0xA5A5A5A4 && candidate != 0xA5A5A5A5 &&
                    candidate != priv->prvTaskExitError_addr &&
                    (candidate & 0xFFFFFF00) != 0xFFFFFF00)
                {
                    const char *func_name = rtosLookupPointerAsFunction(symbols, candidate);
                    if (func_name)
                    {
                        thread->entry_func = candidate;
                        thread->entry_func_name = func_name;
                        return;
                    }
                }
            }
            return;
        }
    }
}


static void apply_known_task_names(struct rtosThread *thread)
{
    if (thread->entry_func_name)
        return;

    if (strcasecmp(thread->name, FREERTOS_IDLE_TASK_NAME) == 0)
        thread->entry_func_name = "prvIdleTask";
    else if (strcasecmp(thread->name, "Tmr Svc") == 0)
        thread->entry_func_name = "prvTimerTask";
}


static bool check_thread_reuse(struct rtosThread *thread,
                               uint32_t old_name_hash, uint32_t old_func_hash,
                               uint32_t tcb_addr)
{
    thread->name_hash = simple_hash(thread->name);
    thread->func_hash = simple_hash(thread->entry_func_name);

    if (old_name_hash == 0 || old_func_hash == 0)
        return false;

    if (old_name_hash != thread->name_hash && old_func_hash != thread->func_hash)
    {
        if (!rtos_name_is_unknown(thread->name))
        {
            genericsReport(V_INFO, "Thread REUSED: TCB=0x%08X, resetting stats" EOL, tcb_addr);
            thread->accumulated_time_us = 0;
            thread->accumulated_cycles = 0;
            thread->context_switches = 0;
            thread->max_cpu_percent = 0;
            return true;
        }
    }
    return false;
}


/* -------------------------------------------------------------------------
 * Main thread info reader
 * ------------------------------------------------------------------------- */

static int freertos_read_thread_info(struct rtosState *rtos,
                                     struct SymbolSet *symbols,
                                     struct rtosThread *thread,
                                     uint32_t tcb_addr)
{
    if (!rtos || !thread || !tcb_addr || tcb_addr == 0xFFFFFFFF)
        return -1;

    struct freertos_private *priv = (struct freertos_private *)rtos->priv;
    uint8_t name_offset = priv ? priv->name_offset : FREERTOS_TCB_NAME_OFFSET;
    uint8_t priority_offset = priv ? priv->priority_offset : FREERTOS_TCB_PRIORITY_OFFSET;
    uint8_t max_name_len = priv ? priv->max_task_name_len : FREERTOS_DEFAULT_MAX_TASK_NAME_LEN;

    uint32_t old_name_hash = thread->name_hash;
    uint32_t old_func_hash = thread->func_hash;

    /* pxTopOfStack is the first field in the TCB (offset 0).  It is a
     * saved stack pointer — always non-null and word-aligned for any
     * valid task.  A failed memory read returns 0; a non-TCB address
     * in flash will typically yield an odd value.  Either way we can
     * reject early with a single read. */
    uint32_t pxTopOfStack = rtosReadMemoryWord(tcb_addr + FREERTOS_TCB_TOP_OF_STACK_OFFSET);
    if (!pxTopOfStack || pxTopOfStack == 0xFFFFFFFF || (pxTopOfStack & 3))
        return -1;

    bool name_valid = read_thread_name(thread, tcb_addr, name_offset, max_name_len);
    read_thread_priority(thread, tcb_addr, priority_offset);
    detect_entry_function(thread, symbols, priv, tcb_addr, pxTopOfStack);
    apply_known_task_names(thread);

    /* Reject if neither name nor entry function could be resolved —
     * matches the approach used by RTX5 and Zephyr plugins. */
    if (!name_valid && !thread->entry_func)
        return -1;

    bool reused = check_thread_reuse(thread, old_name_hash, old_func_hash, tcb_addr);

    return reused ? 1 : 0;
}


/* -------------------------------------------------------------------------
 * RTOS detection and initialization
 * ------------------------------------------------------------------------- */

static bool freertos_detect(struct SymbolSet *symbols, struct rtosDetection *result)
{
    if (!result)
    {
        return false;
    }

    result->type = RTOS_FREERTOS;
    result->name = "FreeRTOS";
    result->confidence = 90;
    result->reason = "FreeRTOS selected by user";
    return true;
}


/* Helper to find symbol address using objdump */
static uint32_t find_symbol_address(const char *elfFile, const char *symbol_name)
{
    if (!elfFile || !symbol_name)
    {
        return 0;
    }

    char cmd[512];
    FILE *fp;
    char line[256];
    uint32_t address = 0;

    snprintf(cmd, sizeof(cmd), "arm-none-eabi-objdump -t %s 2>/dev/null | grep '%s$'",
             elfFile, symbol_name);

    fp = popen(cmd, "r");
    if (fp && fgets(line, sizeof(line), fp))
    {
        address = strtoul(line, NULL, 16);
    }
    if (fp)
    {
        pclose(fp);
    }

    return address;
}


static uint32_t find_symbol_size(const char *elfFile, const char *symbol_name)
{
    if (!elfFile || !symbol_name)
    {
        return 0;
    }

    char cmd[512];
    FILE *fp;
    char line[256];
    uint32_t size = 0;

    /* objdump -t output: "ADDR FLAGS TYPE SECTION\tSIZE NAME"
     * e.g.: "24019628 g     O .bss\t00000040 xQueueRegistry"
     * The size field is between the tab after section and the symbol name. */
    snprintf(cmd, sizeof(cmd),
             "arm-none-eabi-objdump -t %s 2>/dev/null | grep '%s$' | awk '{print $(NF-1)}'",
             elfFile, symbol_name);

    fp = popen(cmd, "r");
    if (fp && fgets(line, sizeof(line), fp))
    {
        size = strtoul(line, NULL, 16);
    }
    if (fp)
    {
        pclose(fp);
    }

    return size;
}



/* Initialize FreeRTOS tracking */
static void init_default_offsets(struct freertos_private *priv)
{
    priv->name_offset = FREERTOS_TCB_NAME_OFFSET;
    priv->priority_offset = FREERTOS_TCB_PRIORITY_OFFSET;
    priv->end_of_stack_offset = FREERTOS_TCB_END_OF_STACK_OFFSET;
    priv->max_task_name_len = FREERTOS_DEFAULT_MAX_TASK_NAME_LEN;
    priv->has_end_of_stack = true;
    priv->queue_type_offset = -1;
    priv->has_queue_type = false;
    priv->queue_registry_addr = 0;
    priv->has_queue_registry = false;
}


static void detect_tcb_offsets_from_dwarf(struct freertos_private *priv, struct SymbolSet *symbols)
{
    int32_t off;

    off = SymbolGetStructOffset(symbols, "TCB_t", "pcTaskName");
    if (off >= 0)
        priv->name_offset = (uint8_t)off;

    int32_t name_size = SymbolGetStructFieldSize(symbols, "TCB_t", "pcTaskName");
    if (name_size > 0)
        priv->max_task_name_len = (uint8_t)name_size;

    off = SymbolGetStructOffset(symbols, "TCB_t", "uxPriority");
    if (off >= 0)
        priv->priority_offset = (uint8_t)off;

    off = SymbolGetStructOffset(symbols, "TCB_t", "pxEndOfStack");
    if (off >= 0)
        priv->end_of_stack_offset = (uint8_t)off;

    /* Detect Queue_t.ucQueueType offset (present when configUSE_TRACE_FACILITY=1) */
    priv->queue_type_offset = SymbolGetStructOffset(symbols, "Queue_t", "ucQueueType");
    priv->has_queue_type = (priv->queue_type_offset >= 0);

    if (priv->has_queue_type)
        genericsReport(V_INFO, "FreeRTOS: Queue_t.ucQueueType offset=%d" EOL, priv->queue_type_offset);

    /* Detect xQueueRegistry[] for object name lookup.
     * The name is NOT in Queue_t — it's in a separate registry array.
     * All offsets and sizes come from DWARF / ELF — nothing hardcoded. */
    if (symbols->elfFile)
    {
        priv->queue_registry_addr = find_symbol_address(symbols->elfFile, "xQueueRegistry");
        if (priv->queue_registry_addr)
        {
            /* Get QueueRegistryItem_t field offsets from DWARF */
            int32_t name_off = SymbolGetStructOffset(symbols, "QueueRegistryItem_t", "pcQueueName");
            int32_t handle_off = SymbolGetStructOffset(symbols, "QueueRegistryItem_t", "xHandle");

            if (name_off < 0 || handle_off < 0)
            {
                genericsReport(V_WARN, "FreeRTOS: Cannot resolve QueueRegistryItem_t fields from DWARF" EOL);
            }
            else
            {
                priv->registry_name_offset = (uint8_t)name_off;
                priv->registry_handle_offset = (uint8_t)handle_off;

                /* Get sizeof(QueueRegistryItem_t) via GDB and total array
                 * size from objdump, then derive entry count. */
                uint32_t sym_size = find_symbol_size(symbols->elfFile, "xQueueRegistry");
                int32_t item_size = -1;
                {
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd),
                             "gdb-multiarch -batch -ex \"print sizeof(QueueRegistryItem_t)\" %s 2>/dev/null",
                             symbols->elfFile);
                    FILE *fp = popen(cmd, "r");
                    if (fp)
                    {
                        char line[256];
                        if (fgets(line, sizeof(line), fp))
                        {
                            char *eq = strchr(line, '=');
                            if (eq)
                                item_size = (int32_t)strtol(eq + 1, NULL, 10);
                        }
                        pclose(fp);
                    }
                }

                if (item_size > 0 && sym_size > 0)
                {
                    priv->registry_item_size = (uint8_t)item_size;
                    priv->queue_registry_size = (uint8_t)(sym_size / (uint32_t)item_size);
                    priv->has_queue_registry = true;

                    genericsReport(V_INFO, "FreeRTOS: xQueueRegistry at 0x%08X, %d entries of %d bytes "
                                  "(name@%d, handle@%d)" EOL,
                                  priv->queue_registry_addr, priv->queue_registry_size,
                                  priv->registry_item_size,
                                  priv->registry_name_offset, priv->registry_handle_offset);
                }
                else
                {
                    genericsReport(V_WARN, "FreeRTOS: Cannot determine xQueueRegistry layout "
                                  "(sym_size=%u, item_size=%d)" EOL, sym_size, item_size);
                }
            }
        }
        else
        {
            genericsReport(V_INFO, "FreeRTOS: xQueueRegistry not found (configQUEUE_REGISTRY_SIZE=0?)" EOL);
        }
    }
}


static int find_kernel_symbols(struct freertos_private *priv, const char *elfFile)
{
    priv->pxCurrentTCB_addr = find_symbol_address(elfFile, FREERTOS_SYM_PX_CURRENT_TCB);
    if (priv->pxCurrentTCB_addr == 0)
    {
        genericsReport(V_ERROR, "FreeRTOS: Symbol '%s' not found!" EOL, FREERTOS_SYM_PX_CURRENT_TCB);
        return -1;
    }

    genericsReport(V_INFO, "FreeRTOS: pxCurrentTCB at 0x%08X" EOL, priv->pxCurrentTCB_addr);

    priv->pxReadyTasksLists_addr = find_symbol_address(elfFile, FREERTOS_SYM_PX_READY_TASKS_LISTS);
    priv->xSchedulerRunning_addr = find_symbol_address(elfFile, FREERTOS_SYM_X_SCHEDULER_RUNNING);
    priv->uxCurrentNumberOfTasks_addr = find_symbol_address(elfFile, FREERTOS_SYM_UX_CURRENT_NUMBER_OF_TASKS);
    priv->prvTaskExitError_addr = find_symbol_address(elfFile, "prvTaskExitError");

    if (priv->prvTaskExitError_addr)
        genericsReport(V_INFO, "FreeRTOS: prvTaskExitError at 0x%08X" EOL, priv->prvTaskExitError_addr);

    return 0;
}


static int freertos_init(struct rtosState *rtos, struct SymbolSet *symbols)
{
    if (!rtos)
        return -1;

    struct freertos_private *priv = calloc(1, sizeof(struct freertos_private));
    if (!priv)
        return -1;

    init_default_offsets(priv);
    rtos->priv = priv;

    if (symbols && symbols->elfFile)
    {
        detect_tcb_offsets_from_dwarf(priv, symbols);

        if (find_kernel_symbols(priv, symbols->elfFile) < 0)
        {
            free(priv);
            rtos->priv = NULL;
            return -1;
        }
    }

    return 0;
}


/* Cleanup FreeRTOS tracking */
static void freertos_cleanup(struct rtosState *rtos)
{
    if (rtos && rtos->priv)
    {
        free(rtos->priv);
        rtos->priv = NULL;
    }
}


/* Get state name (FreeRTOS state is implicit from list membership) */
static const char* freertos_get_state_name(uint8_t state)
{
    switch (state)
    {
        case FREERTOS_TASK_RUNNING:   return "Running";
        case FREERTOS_TASK_READY:     return "Ready";
        case FREERTOS_TASK_BLOCKED:   return "Blocked";
        case FREERTOS_TASK_SUSPENDED: return "Suspended";
        case FREERTOS_TASK_DELETED:   return "Deleted";
        default:                      return "Unknown";
    }
}


static bool freertos_is_idle_thread(struct rtosThread *thread)
{
    if (!thread)
        return false;

    if (thread->entry_func_name && strcmp(thread->entry_func_name, "prvIdleTask") == 0)
        return true;

    if (strcasecmp(thread->name, FREERTOS_IDLE_TASK_NAME) == 0)
        return true;

    return false;
}


static int ensure_telnet_connected(struct rtosState *rtos)
{
    if (rtos->telnet_port <= 0)
        return RTOS_VERIFY_SUCCESS;

    if (!telnet_is_connected() && telnet_connect(rtos->telnet_port) < 0)
        return RTOS_VERIFY_NO_CONNECTION;

    return RTOS_VERIFY_SUCCESS;
}


static int freertos_verify_target_match(struct rtosState *rtos, struct SymbolSet *symbols)
{
    if (!rtos || !rtos->priv)
        return RTOS_VERIFY_ERROR;

    struct freertos_private *priv = (struct freertos_private *)rtos->priv;

    int conn = ensure_telnet_connected(rtos);
    if (conn != RTOS_VERIFY_SUCCESS)
        return conn;

    uint32_t current_tcb = rtosReadMemoryWord(priv->pxCurrentTCB_addr);
    if (current_tcb == 0 || current_tcb == 0xFFFFFFFF)
        return RTOS_VERIFY_SUCCESS;  /* Scheduler not started yet */

    char name_buf[RTOS_THREAD_NAME_MAX_LEN] = {0};
    rtosReadMemoryString(current_tcb + priv->name_offset, name_buf, sizeof(name_buf));

    if (name_buf[0] && name_buf[0] != 0xFF)
        genericsReport(V_INFO, "FreeRTOS: Verified - task '%s' at 0x%08X" EOL, name_buf, current_tcb);

    return RTOS_VERIFY_SUCCESS;
}


static uint32_t freertos_get_watchpoint_addr(struct rtosState *rtos)
{
    if (!rtos || !rtos->priv)
        return 0;
    struct freertos_private *priv = (struct freertos_private *)rtos->priv;
    return priv->pxCurrentTCB_addr;
}


/* -------------------------------------------------------------------------
 * Object tracking (Queue_t → mutex/semaphore/queue)
 * ------------------------------------------------------------------------- */

static enum rtosObjectType freertos_queue_type_to_object_type(uint8_t queue_type)
{
    switch (queue_type)
    {
        case FREERTOS_QUEUE_TYPE_MUTEX:   return RTOS_OBJ_MUTEX;
        case FREERTOS_QUEUE_TYPE_RMUTEX:  return RTOS_OBJ_MUTEX;
        case FREERTOS_QUEUE_TYPE_CSEM:    return RTOS_OBJ_SEMAPHORE;
        case FREERTOS_QUEUE_TYPE_BSEM:    return RTOS_OBJ_SEMAPHORE;
        case FREERTOS_QUEUE_TYPE_BASE:    return RTOS_OBJ_MESSAGE_QUEUE;
        default:                          return RTOS_OBJ_UNKNOWN;
    }
}

static const char *freertos_queue_type_prefix(uint8_t queue_type)
{
    switch (queue_type)
    {
        case FREERTOS_QUEUE_TYPE_MUTEX:   return "mutex";
        case FREERTOS_QUEUE_TYPE_RMUTEX:  return "rmutex";
        case FREERTOS_QUEUE_TYPE_CSEM:    return "sem";
        case FREERTOS_QUEUE_TYPE_BSEM:    return "sem";
        case FREERTOS_QUEUE_TYPE_BASE:    return "queue";
        default:                          return "obj";
    }
}

static int freertos_read_object_info(struct rtosState *rtos, struct rtosObject *obj, uint32_t cb_addr)
{
    if (!rtos || !obj || !cb_addr)
        return -1;

    /* Queue_t.pcHead (offset 0): for queues/semaphores it points to the
     * storage area (non-null, word-aligned).  For mutexes, FreeRTOS sets
     * pcHead = NULL intentionally (prvInitialiseMutex).  So NULL is valid. */
    uint32_t pcHead = rtosReadMemoryWord(cb_addr);
    if (pcHead == 0xFFFFFFFF || (pcHead && (pcHead & 3)))
        return -1;

    struct freertos_private *priv = (struct freertos_private *)rtos->priv;

    /* Read ucQueueType from Queue_t (if available) */
    if (priv && priv->has_queue_type)
    {
        uint32_t type_addr = cb_addr + (uint32_t)priv->queue_type_offset;
        uint32_t aligned_addr = type_addr & ~3u;
        uint32_t byte_pos = type_addr & 3u;
        uint32_t type_word = rtosReadMemoryWord(aligned_addr);
        uint8_t queue_type = (uint8_t)((type_word >> (byte_pos * 8)) & 0xFF);

        obj->type = freertos_queue_type_to_object_type(queue_type);
        obj->type_prefix = freertos_queue_type_prefix(queue_type);

        genericsReport(V_DEBUG, "FreeRTOS ObjType: CB=0x%08X type_off=%d type_addr=0x%08X word=0x%08X byte_pos=%u raw=0x%02X -> %s" EOL,
                      cb_addr, priv->queue_type_offset, type_addr, type_word, byte_pos, queue_type, obj->type_prefix);
    }
    else
    {
        obj->type = RTOS_OBJ_UNKNOWN;
        obj->type_prefix = "obj";
    }

    /* Look up name in xQueueRegistry[] — a separate array of {pcQueueName, xHandle} */
    bool name_found = false;
    if (priv && priv->has_queue_registry)
    {
        for (uint8_t i = 0; i < priv->queue_registry_size; i++)
        {
            uint32_t entry_addr = priv->queue_registry_addr + (i * priv->registry_item_size);
            uint32_t handle = rtosReadMemoryWord(entry_addr + priv->registry_handle_offset);

            if (handle == cb_addr)
            {
                uint32_t name_ptr = rtosReadMemoryWord(entry_addr + priv->registry_name_offset);
                if (name_ptr && name_ptr != 0xFFFFFFFF)
                {
                    char name_buf[64] = {0};
                    char *name_str = rtosReadMemoryString(name_ptr, name_buf, sizeof(name_buf));
                    if (name_str && name_buf[0] >= 0x20 && name_buf[0] < 0x7F)
                    {
                        strncpy(obj->name, name_buf, sizeof(obj->name) - 1);
                        obj->name[sizeof(obj->name) - 1] = '\0';
                        name_found = true;
                    }
                }
                break;
            }
        }
    }

    if (!name_found)
        snprintf(obj->name, sizeof(obj->name), "0x%08X", cb_addr);

    genericsReport(V_DEBUG, "FreeRTOS Object: CB=0x%08X, Type=%s, Name=%s" EOL,
                  cb_addr, obj->type_prefix, obj->name);

    return 0;
}


static const struct rtosOps freertos_ops =
{
    .read_thread_info = freertos_read_thread_info,
    .get_priority_name = freertosGetPriorityName,
    .detect = freertos_detect,
    .init = freertos_init,
    .cleanup = freertos_cleanup,
    .get_state_name = freertos_get_state_name,
    .is_idle_thread = freertos_is_idle_thread,
    .verify_target_match = freertos_verify_target_match,
    .get_watchpoint_addr = freertos_get_watchpoint_addr,
    .read_object_info = freertos_read_object_info
};


void rtosRegisterFreeRTOS(void)
{
    /* Registration placeholder - not currently used */
}


const struct rtosOps *freertosGetOps(void)
{
    return &freertos_ops;
}
