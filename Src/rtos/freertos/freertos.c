
/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <symbols.h>
#include <freertos.h>
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
    if (thread->name[0] != 0 && strcmp(thread->name, "No Name") != 0)
        return true;  /* Already cached */

    uint32_t name_addr = tcb_addr + name_offset;
    char name_buf[RTOS_THREAD_NAME_MAX_LEN] = {0};
    size_t read_len = (max_name_len < sizeof(name_buf) - 1) ? max_name_len : sizeof(name_buf) - 1;

    char *name_str = rtosReadMemoryString(name_addr, name_buf, read_len + 1);
    if (!name_str || name_buf[0] < 0x20 || name_buf[0] >= 0x7F)
    {
        strcpy(thread->name, "No Name");
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

    uint32_t pxEndOfStack = rtosReadMemoryWord(tcb_addr + priv->end_of_stack_offset);
    if (!pxEndOfStack || pxEndOfStack == 0xFFFFFFFF || pxEndOfStack <= pxTopOfStack)
        return;

    uint32_t scan_limit = pxEndOfStack - (FREERTOS_MAX_STACK_SCAN_WORDS * 4);
    if (scan_limit < pxTopOfStack)
        scan_limit = pxTopOfStack;

    for (uint32_t addr = pxEndOfStack; addr >= scan_limit; addr -= 4)
    {
        uint32_t val = rtosReadMemoryWord(addr);
        if ((val & ~1) == priv->prvTaskExitError_addr)
        {
            uint32_t entry_pc = rtosReadMemoryWord(addr + 4) & ~1;
            if (entry_pc && entry_pc != 0xFFFFFFFF && symbols)
            {
                const char *func_name = rtosLookupPointerAsFunction(symbols, entry_pc);
                if (func_name)
                {
                    thread->entry_func = entry_pc;
                    thread->entry_func_name = func_name;
                    return;
                }
            }
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
        if (strcmp(thread->name, "No Name") != 0)
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

    read_thread_name(thread, tcb_addr, name_offset, max_name_len);
    read_thread_priority(thread, tcb_addr, priority_offset);

    uint32_t pxTopOfStack = rtosReadMemoryWord(tcb_addr + FREERTOS_TCB_TOP_OF_STACK_OFFSET);
    detect_entry_function(thread, symbols, priv, tcb_addr, pxTopOfStack);
    apply_known_task_names(thread);

    bool reused = check_thread_reuse(thread, old_name_hash, old_func_hash, tcb_addr);

    genericsReport(V_DEBUG, "FreeRTOS: TCB=0x%08X '%s' func=%s pri=%d" EOL,
                  tcb_addr, thread->name,
                  thread->entry_func_name ? thread->entry_func_name : "-",
                  thread->priority);

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



/* Initialize FreeRTOS tracking */
static void init_default_offsets(struct freertos_private *priv)
{
    priv->name_offset = FREERTOS_TCB_NAME_OFFSET;
    priv->priority_offset = FREERTOS_TCB_PRIORITY_OFFSET;
    priv->end_of_stack_offset = FREERTOS_TCB_END_OF_STACK_OFFSET;
    priv->max_task_name_len = FREERTOS_DEFAULT_MAX_TASK_NAME_LEN;
    priv->has_end_of_stack = true;
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
    .get_watchpoint_addr = freertos_get_watchpoint_addr
};


void rtosRegisterFreeRTOS(void)
{
    /* Registration placeholder - not currently used */
}


const struct rtosOps *freertosGetOps(void)
{
    return &freertos_ops;
}
