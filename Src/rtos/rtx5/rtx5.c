

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <symbols.h>
#include <rtos/rtx5/rtx5.h>
#include <telnet_client.h>
#include <generics.h>
#include <rtos_support.h>


static const char *rtx5_priority_names[] = {
    "osPriorityNone",
    "osPriorityIdle",
    "osPriorityReserved2",
    "osPriorityReserved3",
    "osPriorityReserved4",
    "osPriorityReserved5",
    "osPriorityReserved6",
    "osPriorityReserved7",
    "osPriorityLow",
    "osPriorityLow1",
    "osPriorityLow2",
    "osPriorityLow3",
    "osPriorityLow4",
    "osPriorityLow5",
    "osPriorityLow6",
    "osPriorityLow7",
    "osPriorityBelowNormal",
    "osPriorityBelowNormal1",
    "osPriorityBelowNormal2",
    "osPriorityBelowNormal3",
    "osPriorityBelowNormal4",
    "osPriorityBelowNormal5",
    "osPriorityBelowNormal6",
    "osPriorityBelowNormal7",
    "osPriorityNormal",
    "osPriorityNormal1",
    "osPriorityNormal2",
    "osPriorityNormal3",
    "osPriorityNormal4",
    "osPriorityNormal5",
    "osPriorityNormal6",
    "osPriorityNormal7",
    "osPriorityAboveNormal",
    "osPriorityAboveNormal1",
    "osPriorityAboveNormal2",
    "osPriorityAboveNormal3",
    "osPriorityAboveNormal4",
    "osPriorityAboveNormal5",
    "osPriorityAboveNormal6",
    "osPriorityAboveNormal7",
    "osPriorityHigh",
    "osPriorityHigh1",
    "osPriorityHigh2",
    "osPriorityHigh3",
    "osPriorityHigh4",
    "osPriorityHigh5",
    "osPriorityHigh6",
    "osPriorityHigh7",
    "osPriorityRealtime",
    "osPriorityRealtime1",
    "osPriorityRealtime2",
    "osPriorityRealtime3",
    "osPriorityRealtime4",
    "osPriorityRealtime5",
    "osPriorityRealtime6",
    "osPriorityRealtime7",
    "osPriorityISR"
};

static uint32_t simple_hash(const char *str)
{
    uint32_t hash = 5381;
    if (!str)
    {
        return 0;
    }
    while (*str)
    {
        hash = ((hash << 5) + hash) + *str++;
    }
    return hash;
}

const char *rtx5GetPriorityName(int8_t priority)
{
    if (priority >= 0 && priority <= 56)
    {
        return rtx5_priority_names[priority];
    }
    else if (priority == -1)
    {
        return "osPriorityError";
    }
    else
    {
        return "osPriorityUnknown";
    }
}


struct rtx5_private
{
    uint32_t osRtxInfo;
    uint32_t thread_run_curr;
    uint32_t pendSV_Handler;
    uint32_t osRtxThreadListPut;
    uint8_t id_offset;
    uint8_t name_offset;
    uint8_t priority_offset;
    uint8_t thread_addr_offset;
    uint8_t info_os_id_offset;
    uint8_t info_kernel_offset;
    uint8_t info_thread_run_curr_offset;
};


static const char *rtx5_object_type_prefix(uint8_t id)
{
    switch (id)
    {
        case RTX5_ID_MUTEX:        return "mutex";
        case RTX5_ID_SEMAPHORE:    return "sem";
        case RTX5_ID_EVENTFLAGS:   return "evflags";
        case RTX5_ID_MESSAGEQUEUE: return "msgq";
        case RTX5_ID_MEMPOOL:      return "mempool";
        default:                   return "obj";
    }
}

static enum rtosObjectType rtx5_id_to_object_type(uint8_t id)
{
    switch (id)
    {
        case RTX5_ID_MUTEX:        return RTOS_OBJ_MUTEX;
        case RTX5_ID_SEMAPHORE:    return RTOS_OBJ_SEMAPHORE;
        case RTX5_ID_EVENTFLAGS:   return RTOS_OBJ_EVENT_FLAGS;
        case RTX5_ID_MESSAGEQUEUE: return RTOS_OBJ_MESSAGE_QUEUE;
        case RTX5_ID_MEMPOOL:      return RTOS_OBJ_MEMORY_POOL;
        default:                   return RTOS_OBJ_UNKNOWN;
    }
}

static int rtx5_read_object_info(struct rtosState *rtos, struct rtosObject *obj, uint32_t cb_addr)
{
    if (!rtos || !obj || !cb_addr)
        return -1;

    struct rtx5_private *priv = (struct rtx5_private *)rtos->priv;

    uint32_t id_word = rtosReadMemoryWord(cb_addr + priv->id_offset);
    uint8_t object_id = (uint8_t)(id_word & 0xFF);

    obj->type = rtx5_id_to_object_type(object_id);
    obj->type_prefix = rtx5_object_type_prefix(object_id);

    if (obj->type == RTOS_OBJ_UNKNOWN)
    {
        genericsReport(V_WARN, "RTX5: Unknown object ID 0x%02X at CB=0x%08X\n", object_id, cb_addr);
        snprintf(obj->name, sizeof(obj->name), "0x%08X", cb_addr);
        return 0;
    }

    uint32_t name_ptr = rtosReadMemoryWord(cb_addr + priv->name_offset);
    if (name_ptr && name_ptr != 0xFFFFFFFF)
    {
        char name_buf[64] = {0};
        char *name_str = rtosReadMemoryString(name_ptr, name_buf, sizeof(name_buf));
        if (name_str && name_buf[0] != 0)
        {
            strncpy(obj->name, name_buf, sizeof(obj->name) - 1);
            obj->name[sizeof(obj->name) - 1] = '\0';
        }
        else
        {
            snprintf(obj->name, sizeof(obj->name), "0x%08X", cb_addr);
        }
    }
    else
    {
        snprintf(obj->name, sizeof(obj->name), "0x%08X", cb_addr);
    }

    genericsReport(V_INFO, "RTX5 Object: CB=0x%08X, Type=%s, Name=%s\n",
                  cb_addr, obj->type_prefix, obj->name);
    return 0;
}


static int rtx5_read_thread_info(struct rtosState *rtos,
                                 struct SymbolSet *symbols,
                                 struct rtosThread *thread,
                                 uint32_t tcb_addr)
{
    if (!rtos || !thread || !tcb_addr)
    {
        genericsReport(V_ERROR, "rtx5_read_thread_info: Invalid parameters\n");
        return -1;
    }

    struct rtx5_private *priv = (struct rtx5_private *)rtos->priv;

    if (tcb_addr == 0x00000000 || tcb_addr == 0xFFFFFFFF)
    {
        genericsReport(V_WARN, "rtx5_read_thread_info: Invalid TCB address 0x%08X\n", tcb_addr);
        strcpy(thread->name, "INVALID");
        return -1;
    }

    uint32_t id_word = rtosReadMemoryWord(tcb_addr + priv->id_offset);
    uint8_t thread_id = (uint8_t)(id_word & 0xFF);
    if (thread_id != RTX5_ID_THREAD)
    {
        genericsReport(V_DEBUG, "RTX5: Not a thread at TCB=0x%08X - ID=0x%02X (expected 0xF1)\n",
                      tcb_addr, thread_id);
        return -1;
    }

    uint32_t name_ptr = rtosReadMemoryWord(tcb_addr + priv->name_offset);
    thread->name_ptr = name_ptr;
    
    uint32_t old_name_hash = thread->name_hash;
    uint32_t old_func_hash = thread->func_hash;
    
    if (name_ptr && name_ptr != 0xFFFFFFFF)
    {
        char name_buf[64] = {0};
        char *name_str = rtosReadMemoryString(name_ptr, name_buf, sizeof(name_buf));
        if (name_str && name_buf[0] != 0)
        {
            strncpy(thread->name, name_buf, sizeof(thread->name) - 1);
            thread->name[sizeof(thread->name) - 1] = '\0';
            
            if (strstr(thread->name, "_inq") || strstr(thread->name, "_timer"))
            {
                genericsReport(V_DEBUG, "RTX5: TCB=0x%08X has name='%s' from ptr=0x%08X\n", 
                              tcb_addr, thread->name, name_ptr);
            }
        }
    else
        {
            strcpy(thread->name, RTOS_NAME_UNKNOWN);
        }
    }
    else
    {
        strcpy(thread->name, RTOS_NAME_UNKNOWN);
    }
    
    uint32_t thread_func = rtosReadMemoryWord(tcb_addr + priv->thread_addr_offset);
    thread->entry_func = thread_func & ~1;
    
    if (!thread_func || thread_func == 0xFFFFFFFF)
    {
        genericsReport(V_WARN, "RTX5: Invalid thread data at TCB=0x%08X - function is NULL/invalid (0x%08X)\n", 
                      tcb_addr, thread_func);
        strcpy(thread->name, "INVALID_READ");
        thread->priority = -1;
        return -1;
    }
    
    if (symbols)
    {
        thread->entry_func_name = rtosLookupPointerAsFunction(symbols, thread_func);
    }
    else
    {
        thread->entry_func_name = NULL;
    }
    
    uint32_t priority_word = rtosReadMemoryWord(tcb_addr + priv->priority_offset);
    thread->priority = (int8_t)(priority_word & 0xFF);
    
    if (thread->priority < -3 || thread->priority > 56)
    {
        genericsReport(V_WARN, "RTX5: Suspicious priority %d at TCB=0x%08X (raw=0x%08X), name='%s', func=0x%08X\n", 
                      thread->priority, tcb_addr, priority_word, thread->name, thread_func);
    }
    
    thread->name_hash = simple_hash(thread->name);
    thread->func_hash = simple_hash(thread->entry_func_name);
    
    if (old_name_hash != 0 && old_func_hash != 0)
    {
        if (old_name_hash != thread->name_hash && old_func_hash != thread->func_hash)
        {
            if (!rtos_name_is_unknown(thread->name) || thread->entry_func != 0)
            {
                genericsReport(V_INFO, "Thread REUSED detected: TCB=0x%08X, resetting statistics\n", tcb_addr);
                thread->accumulated_time_us = 0;
                thread->accumulated_cycles = 0;
                thread->context_switches = 0;
                thread->max_cpu_percent = 0;
                return 1;
            }
        }
    }
    
    genericsReport(V_INFO, "RTX5 Thread: TCB=0x%08X, Name='%s', Func=0x%08X/%s, Priority=%d\n", 
                  tcb_addr, thread->name, thread->entry_func,
                  thread->entry_func_name ? thread->entry_func_name : "-", 
                  thread->priority);
    
    return 0;
}

static bool rtx5_detect(struct SymbolSet *symbols, struct rtosDetection *result)
{
    if (!result)
    {
        return false;
    }
    
    result->type = RTOS_RTX5;
    result->name = "RTX5";
    result->confidence = 90;
    result->reason = "RTX5 selected by user";
    return true;
}

static int rtx5_init(struct rtosState *rtos, struct SymbolSet *symbols)
{
    if (!rtos)
    {
        return -1;
    }

    struct rtx5_private *priv = calloc(1, sizeof(struct rtx5_private));
    if (!priv)
    {
        return -1;
    }

    priv->id_offset = RTX5_THREAD_ID_OFFSET;
    priv->name_offset = RTX5_THREAD_NAME_OFFSET;
    priv->priority_offset = RTX5_THREAD_PRIORITY_OFFSET;
    priv->thread_addr_offset = RTX5_THREAD_THREAD_ADDR_OFFSET;
    priv->info_os_id_offset = RTX5_INFO_OS_ID_OFFSET;
    priv->info_kernel_offset = RTX5_INFO_KERNEL_OFFSET;
    priv->info_thread_run_curr_offset = RTX5_INFO_THREAD_RUN_CURR_OFFSET;

    rtos->priv = priv;

    if (symbols && symbols->elfFile)
    {
        char cmd[512];
        FILE *fp;
        char line[256];

        snprintf(cmd, sizeof(cmd), "arm-none-eabi-objdump -t %s 2>/dev/null | grep 'osRtxInfo$'", symbols->elfFile);
        fp = popen(cmd, "r");
        if (fp && fgets(line, sizeof(line), fp))
        {
            priv->osRtxInfo = strtoul(line, NULL, 16);
        }
        if (fp)
        {
            pclose(fp);
        }

        if (priv->osRtxInfo == 0)
        {
            genericsReport(V_ERROR, "osRtxInfo symbol not found in ELF!" EOL);
            free(priv);
            rtos->priv = NULL;
            return -1;
        }

        int32_t off;

        off = SymbolGetStructOffset(symbols, "osRtxInfo_t", "os_id");
        if (off >= 0)
            priv->info_os_id_offset = (uint8_t)off;

        off = SymbolGetStructOffset(symbols, "osRtxInfo_t", "kernel");
        if (off >= 0)
            priv->info_kernel_offset = (uint8_t)off;

        off = SymbolGetStructOffset(symbols, "osRtxInfo_t", "thread.run.curr");
        if (off >= 0)
            priv->info_thread_run_curr_offset = (uint8_t)off;

        priv->thread_run_curr = priv->osRtxInfo + priv->info_thread_run_curr_offset;

        off = SymbolGetStructOffset(symbols, "osRtxThread_t", "id");
        if (off >= 0)
            priv->id_offset = (uint8_t)off;

        off = SymbolGetStructOffset(symbols, "osRtxThread_t", "name");
        if (off >= 0)
            priv->name_offset = (uint8_t)off;

        off = SymbolGetStructOffset(symbols, "osRtxThread_t", "priority");
        if (off >= 0)
            priv->priority_offset = (uint8_t)off;

        off = SymbolGetStructOffset(symbols, "osRtxThread_t", "thread_addr");
        if (off >= 0)
            priv->thread_addr_offset = (uint8_t)off;
    }

    return 0;
}

static void rtx5_cleanup(struct rtosState *rtos)
{
    if (rtos && rtos->priv)
    {
        free(rtos->priv);
        rtos->priv = NULL;
    }
}

static const char* rtx5_get_state_name(uint8_t state)
{
    switch (state)
    {
        case RTX5_THREAD_INACTIVE:   return "Inactive";
        case RTX5_THREAD_READY:      return "Ready";
        case RTX5_THREAD_RUNNING:    return "Running";
        case RTX5_THREAD_BLOCKED:    return "Blocked";
        case RTX5_THREAD_TERMINATED: return "Terminated";
        default:                     return "Unknown";
    }
}

static bool rtx5_is_idle_thread(struct rtosThread *thread)
{
    if (!thread)
    {
        return false;
    }
    
    if (thread->entry_func_name && 
        strcmp(thread->entry_func_name, "osRtxIdleThread") == 0)
    {
        return true;
    }
    
    if (thread->priority == -3)
    {
        return true;
    }
    
    return false;
}

static int rtx5_verify_target_match(struct rtosState *rtos, struct SymbolSet *symbols)
{
    if (!rtos || !rtos->priv)
    {
        return RTOS_VERIFY_ERROR;
    }
    
    struct rtx5_private *priv = (struct rtx5_private *)rtos->priv;
    
    if (rtos->telnet_port <= 0)
    {
        genericsReport(V_DEBUG, "RTX5: Cannot verify target match - telnet not configured" EOL);
        return RTOS_VERIFY_SUCCESS;
    }
    
    if (!telnet_is_connected())
    {
        if (telnet_connect(4444) < 0)
        {
            genericsReport(V_INFO, "RTX5: Telnet not available - will verify when connection is established" EOL);
            return RTOS_VERIFY_NO_CONNECTION;
        }
    }
    
    uint32_t os_id_ptr = rtosReadMemoryWord(priv->osRtxInfo + priv->info_os_id_offset);
    if (!os_id_ptr || os_id_ptr == 0xFFFFFFFF)
    {
        genericsReport(V_ERROR, "RTX5: Cannot read os_id pointer from osRtxInfo at 0x%08X" EOL, priv->osRtxInfo);
        genericsReport(V_ERROR, "Target Connected Mismatch with ELF" EOL);
        return RTOS_VERIFY_MISMATCH;
    }
    
    char version_buf[64] = {0};
    char *version_str = rtosReadMemoryString(os_id_ptr, version_buf, sizeof(version_buf));
    
    if (!version_str || version_buf[0] == 0)
    {
        genericsReport(V_WARN, "RTX5: Cannot read version string from target at 0x%08X" EOL, os_id_ptr);
        return RTOS_VERIFY_NO_CONNECTION;
    }
    
    if (!strstr(version_buf, "RTX"))
    {
        genericsReport(V_ERROR, "Target Connected Mismatch with ELF" EOL);
        genericsReport(V_ERROR, "Expected RTX version but got: '%s'" EOL, version_buf);
        return RTOS_VERIFY_MISMATCH;
    }
    
    genericsReport(V_INFO, "RTX5: Target verified - Version: %s" EOL, version_buf);
    
    uint32_t kernel_state = rtosReadMemoryWord(priv->osRtxInfo + priv->info_kernel_offset);
    if (kernel_state == 0 || kernel_state == 0xFFFFFFFF)
    {
        genericsReport(V_WARN, "RTX5: Kernel state invalid (0x%08X) - possible target mismatch" EOL, kernel_state);
    }
    
    return RTOS_VERIFY_SUCCESS;
}


static uint32_t rtx5_get_watchpoint_addr(struct rtosState *rtos)
{
    if (!rtos || !rtos->priv)
        return 0;
    struct rtx5_private *priv = (struct rtx5_private *)rtos->priv;
    return priv->thread_run_curr;
}


static const struct rtosOps rtx5_ops =
{
    .read_thread_info = rtx5_read_thread_info,
    .get_priority_name = rtx5GetPriorityName,
    .detect = rtx5_detect,
    .init = rtx5_init,
    .cleanup = rtx5_cleanup,
    .get_state_name = rtx5_get_state_name,
    .is_idle_thread = rtx5_is_idle_thread,
    .verify_target_match = rtx5_verify_target_match,
    .get_watchpoint_addr = rtx5_get_watchpoint_addr,
    .read_object_info = rtx5_read_object_info
};

void rtosRegisterRTX5(void)
{
}

const struct rtosOps *rtx5GetOps(void)
{
    return &rtx5_ops;
}