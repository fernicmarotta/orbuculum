#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "generics.h"
#include "symbols.h"
#include "rtos_support.h"
#include <rtos/rtx5/rtx5.h>
#include <rtos/freertos/freertos.h>
#include <rtos/zephyr/zephyr.h>
#include <output_handler.h>
#include "uthash.h"


/* -------------------------------------------------------------------------
 * RTOS Registry - extensible list of supported RTOS types
 * ------------------------------------------------------------------------- */

struct rtosRegistry {
    const char *name;
    const char *alias;
    enum rtosType type;
    const struct rtosOps *(*getOps)(void);
};

static const struct rtosRegistry rtos_registry[] = {
    { "rtx5",     "rtxv5",    RTOS_RTX5,     rtx5GetOps     },
    { "freertos", "FreeRTOS", RTOS_FREERTOS, freertosGetOps },
    { "zephyr",   "Zephyr",   RTOS_ZEPHYR,   zephyrGetOps   },
    { NULL, NULL, RTOS_NONE, NULL }
};


static const struct rtosRegistry *find_rtos_by_name(const char *name)
{
    for (const struct rtosRegistry *r = rtos_registry; r->name; r++)
    {
        if (strcasecmp(name, r->name) == 0 ||
            (r->alias && strcasecmp(name, r->alias) == 0))
            return r;
    }
    return NULL;
}


static struct rtosThread *find_or_create_thread(struct rtosState *rtos, struct SymbolSet *symbols,
                                                 uint32_t tcb_addr, int telnet_port);

/* Sort functions for threads */
static int cpu_usage_sort_desc(void *a, void *b) 
{
    struct rtosThread *ta = (struct rtosThread *)a;
    struct rtosThread *tb = (struct rtosThread *)b;
    if (tb->accumulated_time_us > ta->accumulated_time_us) return 1;
    if (tb->accumulated_time_us < ta->accumulated_time_us) return -1;
    return 0;
}

static int max_cpu_sort_desc(void *a, void *b) 
{
    struct rtosThread *ta = (struct rtosThread *)a;
    struct rtosThread *tb = (struct rtosThread *)b;
    if (tb->max_cpu_percent > ta->max_cpu_percent) return 1;
    if (tb->max_cpu_percent < ta->max_cpu_percent) return -1;
    return 0;
}

static int tcb_addr_sort_asc(void *a, void *b) 
{
    struct rtosThread *ta = (struct rtosThread *)a;
    struct rtosThread *tb = (struct rtosThread *)b;
    if (ta->tcb_addr > tb->tcb_addr) return 1;
    if (ta->tcb_addr < tb->tcb_addr) return -1;
    return 0;
}

static int name_sort_asc(void *a, void *b) 
{
    struct rtosThread *ta = (struct rtosThread *)a;
    struct rtosThread *tb = (struct rtosThread *)b;
    return strcmp(ta->name, tb->name);
}

static int func_sort_asc(void *a, void *b) 
{
    struct rtosThread *ta = (struct rtosThread *)a;
    struct rtosThread *tb = (struct rtosThread *)b;
    const char *fa = ta->entry_func_name ? ta->entry_func_name : "";
    const char *fb = tb->entry_func_name ? tb->entry_func_name : "";
    return strcmp(fa, fb);
}

static int priority_sort_desc(void *a, void *b) 
{
    struct rtosThread *ta = (struct rtosThread *)a;
    struct rtosThread *tb = (struct rtosThread *)b;
    if (tb->priority > ta->priority) return 1;
    if (tb->priority < ta->priority) return -1;
    return 0;
}

static int switches_sort_desc(void *a, void *b) 
{
    struct rtosThread *ta = (struct rtosThread *)a;
    struct rtosThread *tb = (struct rtosThread *)b;
    if (tb->context_switches > ta->context_switches) return 1;
    if (tb->context_switches < ta->context_switches) return -1;
    return 0;
}


struct rtosState *rtosDetectAndInit(struct SymbolSet *symbols, const char *requested_type,
                                    int options_telnetPort, uint32_t cpu_freq)
{
    if (!requested_type)
        return NULL;

    const struct rtosRegistry *reg = find_rtos_by_name(requested_type);
    if (!reg)
    {
        genericsReport(V_ERROR, "Unknown RTOS type: %s" EOL, requested_type);
        genericsReport(V_ERROR, "Supported: rtx5, freertos, zephyr" EOL);
        return NULL;
    }

    struct rtosState *rtos = calloc(1, sizeof(struct rtosState));
    if (!rtos)
        return NULL;

    rtos->type = reg->type;
    rtos->name = reg->name;
    rtos->ops = reg->getOps();
    rtos->cpu_freq = cpu_freq;
    rtos->telnet_port = options_telnetPort;

    if (rtos->ops && rtos->ops->init)
    {
        if (rtos->ops->init(rtos, symbols) < 0)
        {
            genericsReport(V_ERROR, "Failed to initialize %s" EOL, rtos->name);
            if (rtos->ops->cleanup)
                rtos->ops->cleanup(rtos);
            free(rtos);
            return NULL;
        }
    }

    if (rtos->ops && rtos->ops->verify_target_match && options_telnetPort > 0)
    {
        int result = rtos->ops->verify_target_match(rtos, symbols);
        if (result == RTOS_VERIFY_MISMATCH)
        {
            if (rtos->ops->cleanup)
                rtos->ops->cleanup(rtos);
            free(rtos);
            return NULL;
        }
    }

    if (rtos->ops && rtos->ops->get_watchpoint_addr && options_telnetPort > 0)
    {
        uint32_t wp_addr = rtos->ops->get_watchpoint_addr(rtos);
        if (wp_addr)
        {
            /* Pre-seed the current thread before DWT is active.
             * The micro is already running, so read who is executing
             * right now to avoid "unknown" on the first context switch. */
            uint32_t current_tcb = rtosReadMemoryWord(wp_addr);
            if (current_tcb && current_tcb != 0xFFFFFFFF)
            {
                struct rtosThread *thread = find_or_create_thread(rtos, symbols, current_tcb, options_telnetPort);
                if (thread)
                {
                    rtos->current_thread = current_tcb;
                    genericsReport(V_INFO, "Current thread at connect: 0x%08X (%s)" EOL,
                                  current_tcb, thread->name);
                }
            }

            genericsReport(V_INFO, "Configuring DWT watchpoint at 0x%08X" EOL, wp_addr);
            rtosConfigureDWT(wp_addr);
        }
    }

    rtos->enabled = true;
    return rtos;
}


void rtosFree(struct rtosState *rtos)
{
    if (!rtos) return;
    
    struct rtosThread *thread, *tmp;
    HASH_ITER(hh, rtos->threads, thread, tmp) {
        HASH_DEL(rtos->threads, thread);
        free(thread);
    }
    
    if (rtos->ops && rtos->ops->cleanup) {
        rtos->ops->cleanup(rtos);
    }
    
    free(rtos);
}

/* Lookup pointer as string in symbols */
const char *rtosLookupPointerAsString(struct SymbolSet *symbols, uint32_t ptr_value)
{
    if (!ptr_value || !symbols) return NULL;
    
    struct nameEntry n;
    if (SymbolLookup(symbols, ptr_value, &n)) {
        const char *name = SymbolFunction(symbols, n.functionindex);
        if (name && name[0] != '\0' && name[0] != '.') {
            return name;
        }
    }
    
    return NULL;
}

/* Lookup pointer as function in symbols */
const char *rtosLookupPointerAsFunction(struct SymbolSet *symbols, uint32_t ptr_value)
{
    if (!symbols || !ptr_value || ptr_value == 0xFFFFFFFF) return NULL;
    
    const char *name = rtosLookupPointerAsString(symbols, ptr_value & ~1);
    if (name) return name;
    
    name = rtosLookupPointerAsString(symbols, ptr_value);
    if (name) return name;
    
    if (ptr_value > 0) {
        name = rtosLookupPointerAsString(symbols, ptr_value - 1);
        if (name) return name;
    }
    
    return NULL;
}

/* Resolve thread info from pointers */
bool rtosResolveThreadInfo(struct rtosThread *thread, struct SymbolSet *symbols,
                          uint32_t name_ptr, uint32_t func_ptr)
{
    if (!thread || !symbols) return false;
    
    bool resolved = false;
    
    if (name_ptr && !thread->entry_func_name) {
        const char *name = rtosLookupPointerAsString(symbols, name_ptr);
        if (name) {
            strncpy(thread->name, name, sizeof(thread->name) - 1);
            thread->name[sizeof(thread->name) - 1] = '\0';
            resolved = true;
        }
    }
    
    if (func_ptr && !thread->entry_func_name) {
        const char *func = rtosLookupPointerAsFunction(symbols, func_ptr);
        if (func) {
            thread->entry_func = func_ptr & ~1;
            thread->entry_func_name = func;
            resolved = true;
        }
    }
    
    return resolved;
}


/* -------------------------------------------------------------------------
 * DWT Match Handlers - Context Switch Detection
 * ------------------------------------------------------------------------- */

static struct rtosThread *find_or_create_thread(struct rtosState *rtos, struct SymbolSet *symbols,
                                                 uint32_t tcb_addr, int telnet_port)
{
    struct rtosThread *thread;
    HASH_FIND_INT(rtos->threads, &tcb_addr, thread);

    if (thread)
    {
        /* Retry reading info if name is still unknown */
        if (telnet_port > 0 && rtos->ops && rtos->ops->read_thread_info &&
            rtos_name_is_unknown(thread->name))
        {
            rtos->ops->read_thread_info(rtos, symbols, thread, tcb_addr);
        }
        return thread;
    }

    thread = calloc(1, sizeof(struct rtosThread));
    if (!thread)
        return NULL;

    thread->tcb_addr = tcb_addr;
    HASH_ADD_INT(rtos->threads, tcb_addr, thread);
    rtos->thread_count++;

    if (telnet_port > 0 && rtos->ops && rtos->ops->read_thread_info)
    {
        int result = rtos->ops->read_thread_info(rtos, symbols, thread, tcb_addr);
        if (result < 0)
        {
            HASH_DEL(rtos->threads, thread);
            rtos->thread_count--;
            free(thread);
            return NULL;
        }
        if (result > 0)
            rtosClearMemoryCacheForTCB(tcb_addr);
    }
    else
    {
        strcpy(thread->name, RTOS_NAME_UNKNOWN);
    }

    return thread;
}


static void account_prev_thread_time_cycles(struct rtosState *rtos, uint32_t current_cyccnt)
{
    if (!rtos->current_thread || rtos->last_cyccnt == 0)
        return;

    struct rtosThread *prev;
    HASH_FIND_INT(rtos->threads, &rtos->current_thread, prev);
    if (!prev)
        return;

    uint32_t delta = (current_cyccnt >= rtos->last_cyccnt)
        ? current_cyccnt - rtos->last_cyccnt
        : (0xFFFFFFFF - rtos->last_cyccnt) + current_cyccnt + 1;

    if (delta > 0 && delta < 0x80000000 && rtos->cpu_freq > 0)
    {
        uint32_t delta_us = delta / (rtos->cpu_freq / 1000000);
        prev->accumulated_time_us += delta_us;
        prev->accumulated_cycles += delta;
    }
}


static void account_prev_thread_time_us(struct rtosState *rtos, uint64_t current_time_us)
{
    if (!rtos->current_thread || rtos->last_switch_time == 0)
        return;

    struct rtosThread *prev;
    HASH_FIND_INT(rtos->threads, &rtos->current_thread, prev);
    if (!prev)
        return;

    uint64_t delta = current_time_us - rtos->last_switch_time;
    prev->accumulated_time_us += delta;
}


static void handle_context_switch(struct rtosState *rtos, struct rtosThread *thread,
                                   uint64_t timestamp)
{
    if (rtos->current_thread == thread->tcb_addr)
        return;

    thread->context_switches++;
    thread->window_switches++;

    struct rtosThread *prev = NULL;
    if (rtos->current_thread)
        HASH_FIND_INT(rtos->threads, &rtos->current_thread, prev);

    if (rtos->output_config)
        output_thread_switch((OutputConfig *)rtos->output_config, prev, thread, timestamp);

    rtos->current_thread = thread->tcb_addr;
}


void rtosHandleDWTMatchWithTimestamp(struct rtosState *rtos, struct SymbolSet *symbols,
                                     uint32_t comp_num, uint32_t address, uint32_t value,
                                     uint64_t itm_timestamp, int options_telnetPort)
{
    if (!rtos || !rtos->enabled)
        return;

    uint32_t current_cyccnt = (uint32_t)(itm_timestamp & 0xFFFFFFFF);

    struct rtosThread *thread = find_or_create_thread(rtos, symbols, value, options_telnetPort);
    if (!thread)
        return;

    account_prev_thread_time_cycles(rtos, current_cyccnt);
    handle_context_switch(rtos, thread, itm_timestamp);

    thread->last_scheduled_us = current_cyccnt;
    rtos->last_cyccnt = current_cyccnt;
}


void rtosHandleDWTMatch(struct rtosState *rtos, struct SymbolSet *symbols,
                        uint32_t comp_num, uint32_t address, uint32_t value,
                        int options_telnetPort)
{
    if (!rtos || !rtos->enabled)
        return;

    uint64_t current_time_us = genericsTimestampuS();

    struct rtosThread *thread = find_or_create_thread(rtos, symbols, value, options_telnetPort);
    if (!thread)
        return;

    account_prev_thread_time_us(rtos, current_time_us);
    handle_context_switch(rtos, thread, current_time_us);

    thread->last_scheduled_us = current_time_us;
    rtos->last_switch_time = current_time_us;
}


/* -------------------------------------------------------------------------
 * Table Output Helpers
 * ------------------------------------------------------------------------- */

struct ColumnWidths {
    int name;
    int address;
    int function;
    int priority;
    int time;
    int cpu;
    int max;
    int switches;
};


static void printTableSeparator(FILE *f, struct ColumnWidths *widths)
{
    fprintf(f, "|");
    for (int i = 0; i < widths->name + 2; i++) fprintf(f, "-");
    fprintf(f, "|");
    for (int i = 0; i < widths->address + 2; i++) fprintf(f, "-");
    fprintf(f, "|");
    for (int i = 0; i < widths->function + 2; i++) fprintf(f, "-");
    fprintf(f, "|");
    for (int i = 0; i < widths->priority + 2; i++) fprintf(f, "-");
    fprintf(f, "|");
    for (int i = 0; i < widths->time + 2; i++) fprintf(f, "-");
    fprintf(f, "|");
    for (int i = 0; i < widths->cpu + 2; i++) fprintf(f, "-");
    fprintf(f, "|");
    for (int i = 0; i < widths->max + 2; i++) fprintf(f, "-");
    fprintf(f, "|");
    for (int i = 0; i < widths->switches + 2; i++) fprintf(f, "-");
    fprintf(f, "|\n");
}


static void getThreadFunctionString(struct rtosThread *thread, char *buf, size_t bufsize)
{
    if (bufsize == 0) return;
    
    if (thread->entry_func_name && thread->entry_func) {
        snprintf(buf, bufsize, "%s", thread->entry_func_name);
    } else if (thread->entry_func && thread->entry_func != 0xFFFFFFFF) {
        snprintf(buf, bufsize, "0x%08X", thread->entry_func);
    } else {
        snprintf(buf, bufsize, "-");
    }
}

/* Calculate column widths based on thread data */
static void calculateColumnWidths(struct rtosState *rtos, struct ColumnWidths *widths)
{
    struct rtosThread *thread, *tmp;
    
    /* Initialize with header widths */
    widths->name = strlen("Thread Name");
    widths->address = 10;  /* Fixed for 0xXXXXXXXX format */
    widths->function = strlen("Function");
    widths->priority = strlen("Priority");
    widths->time = strlen("Time(ms)");
    widths->cpu = 7;  /* Fixed width for XXX.XXX format */
    widths->max = 7;  /* Fixed width for XXX.XXX format */
    widths->switches = strlen("Switches");
    
    /* Calculate actual maximum widths from thread data */
    HASH_ITER(hh, rtos->threads, thread, tmp) {
        /* Thread name */
        int len = strlen(thread->name);
        if (len > widths->name) widths->name = len;
        
        /* Function name */
        char func_str[64];
        getThreadFunctionString(thread, func_str, sizeof(func_str));
        len = strlen(func_str);
        if (len > widths->function) widths->function = len;
        
        /* Priority name - use RTOS-specific function */
        const char *pri_name = (rtos->ops && rtos->ops->get_priority_name)
            ? rtos->ops->get_priority_name(thread->priority)
            : "Unknown";
        len = strlen(pri_name);
        if (len > widths->priority) widths->priority = len;
        
        /* Time */
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%llu", (unsigned long long)(thread->accumulated_time_us / 1000));
        len = strlen(time_str);
        if (len > widths->time) widths->time = len;
        
        /* Switches */
        char switches_str[32];
        snprintf(switches_str, sizeof(switches_str), "%llu", (unsigned long long)thread->window_switches);
        len = strlen(switches_str);
        if (len > widths->switches) widths->switches = len;
    }
    
    /* Add padding for readability */
    widths->name += 2;
    widths->function += 2;
    widths->priority += 2;
    widths->time += 2;
    widths->switches += 2;
}

/* Print table header */
static void printTableHeader(FILE *f, struct ColumnWidths *widths)
{
    /* Print top separator first */
    printTableSeparator(f, widths);
    
    /* Then print column headers */
    fprintf(f, "| %-*s | %-*s | %-*s | %-*s | %*s | %*s | %*s | %*s |\n",
            widths->name, "Thread Name",
            widths->address, "Address",
            widths->function, "Function",
            widths->priority, "Priority",
            widths->time, "Time(ms)",
            widths->cpu, "CPU%",
            widths->max, "Max%",
            widths->switches, "Switches");
}

/* Print a single thread row */
static void printThreadRow(FILE *f, struct ColumnWidths *widths, struct rtosThread *thread, 
                           struct rtosState *rtos, uint64_t window_time_us)
{
    /* Calculate CPU percentage - use cycles if available, otherwise time */
    uint32_t pct = 0;
    if (window_time_us > 0) {
        if (thread->accumulated_cycles > 0 && rtos->total_cycles > 0) {
            /* Calculate from cycles */
            pct = (thread->accumulated_cycles * 10000) / rtos->total_cycles;
        } else if (thread->accumulated_time_us > 0) {
            /* Fallback to time-based calculation */
            pct = (thread->accumulated_time_us * 10000) / window_time_us;
        }
        if (pct > 10000) pct = 10000;  /* Cap at 100% */
    }
    
    /* Update maximum if needed */
    if (pct > thread->max_cpu_percent) {
        thread->max_cpu_percent = pct;
    }
    
    /* Get thread info strings */
    char func_str[64];
    getThreadFunctionString(thread, func_str, sizeof(func_str));
    const char *pri_name = (rtos->ops && rtos->ops->get_priority_name)
        ? rtos->ops->get_priority_name(thread->priority)
        : "Unknown";
    
    if (rtos->cpu_freq == 0) {
        fprintf(f, "| %-*s | 0x%08X | %-*s | %-*s | %*s | %*.3f | %*.3f | %*" PRIu64 " |\n",
            widths->name, thread->name,
            thread->tcb_addr,
            widths->function, func_str,
            widths->priority, pri_name,
            widths->time, "NA",
            widths->cpu, pct / 100.0,
            widths->max, thread->max_cpu_percent / 100.0,
            widths->switches, thread->window_switches);
    } else {
        uint64_t time_ms = (thread->accumulated_cycles > 0 && rtos->total_cycles > 0)
            ? (thread->accumulated_cycles * (window_time_us / 1000)) / rtos->total_cycles
            : thread->accumulated_time_us / 1000;
        fprintf(f, "| %-*s | 0x%08X | %-*s | %-*s | %*" PRIu64 " | %*.3f | %*.3f | %*" PRIu64 " |\n",
            widths->name, thread->name,
            thread->tcb_addr,
            widths->function, func_str,
            widths->priority, pri_name,
            widths->time, time_ms,
            widths->cpu, pct / 100.0,
            widths->max, thread->max_cpu_percent / 100.0,
            widths->switches, thread->window_switches);
    }
}


static void accountCurrentThreadTime(struct rtosState *rtos)
{
    if (!rtos->current_thread || rtos->last_switch_time == 0 || rtos->last_cyccnt != 0)
        return;

    struct rtosThread *current;
    HASH_FIND_INT(rtos->threads, &rtos->current_thread, current);
    if (!current)
        return;

    uint64_t now = genericsTimestampuS();
    current->accumulated_time_us += now - rtos->last_switch_time;
    rtos->last_switch_time = now;
}


static void sortThreads(struct rtosState *rtos, const char *sort_order)
{
    typedef int (*SortFunc)(void *, void *);
    
    struct { const char *name; SortFunc func; } sorters[] = {
        { "cpu",      cpu_usage_sort_desc },
        { "maxcpu",   max_cpu_sort_desc },
        { "tcb",      tcb_addr_sort_asc },
        { "name",     name_sort_asc },
        { "func",     func_sort_asc },
        { "priority", priority_sort_desc },
        { "switches", switches_sort_desc },
        { NULL, NULL }
    };

    SortFunc func = cpu_usage_sort_desc;
    if (sort_order) {
        for (int i = 0; sorters[i].name; i++) {
            if (strcmp(sort_order, sorters[i].name) == 0) {
                func = sorters[i].func;
                break;
            }
        }
    }
    HASH_SORT(rtos->threads, func);
}


static void calculateCpuTotals(struct rtosState *rtos, bool has_idle,
                                uint64_t *total_us, uint64_t *active_us)
{
    struct rtosThread *thread, *tmp;
    *total_us = 0;
    *active_us = 0;
    uint64_t total_cycles = 0;

    HASH_ITER(hh, rtos->threads, thread, tmp) {
        *total_us += thread->accumulated_time_us;
        total_cycles += thread->accumulated_cycles;
        if (!has_idle || !rtos->ops || !rtos->ops->is_idle_thread(thread))
            *active_us += thread->accumulated_time_us;
    }
    rtos->total_cycles = total_cycles;
}


static void printSummaryLine(FILE *f, struct rtosState *rtos, uint64_t window_us,
                              uint64_t total_us, uint64_t active_us, bool has_idle, bool overflow)
{
    uint32_t pct = has_idle
        ? (uint32_t)((active_us * 10000) / window_us)
        : (uint32_t)((total_us * 10000) / window_us);
    if (pct > 10000) pct = 10000;

    if (has_idle) {
        if (pct > rtos->max_cpu_usage) rtos->max_cpu_usage = pct;
        fprintf(f, "Interval: %" PRIu64 " ms, CPU Usage: %.3f%%, Max: %.3f%%, CPU Freq: ",
                window_us / 1000, pct / 100.0, rtos->max_cpu_usage / 100.0);
        fprintf(f, rtos->cpu_freq > 0 ? "%uHz" : "NA", rtos->cpu_freq);
    } else {
        fprintf(f, "Window: %" PRIu64 " ms, Total CPU: %.3f%%", window_us / 1000, pct / 100.0);
    }

    uint32_t total_pct = (uint32_t)((total_us * 10000) / window_us);
    if (overflow)
        fprintf(f, " [ITM OVERFLOW DETECTED!]");
    else if (total_pct < 9500)
        fprintf(f, " [WARNING: Low total - possible lost DWT events]");
    else if (total_pct > 10500)
        fprintf(f, " [WARNING: High total - timing issue?]");
    fprintf(f, "\n");
}


void rtosDumpThreadInfo(struct rtosState *rtos, FILE *f, uint64_t window_time_us,
                        bool itm_overflow, const char *sort_order)
{
    if (!rtos || !rtos->threads || !f)
        return;

    bool has_idle = (rtos->ops && rtos->ops->is_idle_thread);
    struct ColumnWidths widths;
    uint64_t total_us, active_us;

    accountCurrentThreadTime(rtos);
    calculateCpuTotals(rtos, has_idle, &total_us, &active_us);
    calculateColumnWidths(rtos, &widths);
    sortThreads(rtos, sort_order);

    fprintf(f, "\n=== RTOS Thread Statistics (%s) ===\n", rtos->name);
    printTableHeader(f, &widths);
    printTableSeparator(f, &widths);

    struct rtosThread *thread, *tmp;
    struct rtosThread *idle_thread = NULL;

    HASH_ITER(hh, rtos->threads, thread, tmp) {
        if (thread->tcb_addr == 0 || thread->tcb_addr == 0xFFFFFFFF)
            continue;
        if (has_idle && rtos->ops->is_idle_thread(thread)) {
            idle_thread = thread;
            continue;
        }
        printThreadRow(f, &widths, thread, rtos, window_time_us);
    }

    if (idle_thread) {
        printTableSeparator(f, &widths);
        printThreadRow(f, &widths, idle_thread, rtos, window_time_us);
    }

    if (window_time_us > 0) {
        printTableSeparator(f, &widths);
        printSummaryLine(f, rtos, window_time_us, total_us, active_us, has_idle, itm_overflow);
    }
}

void rtosUpdateThreadCpuMetrics(struct rtosState *rtos, uint64_t window_time_us)
{
    if (!rtos || !rtos->enabled || !rtos->threads || window_time_us == 0)
        return;
    
    struct rtosThread *thread, *tmp;
    uint64_t active_accum_us = 0;
    uint64_t total_accum_us = 0;
    uint64_t total_cycles = 0;
    uint64_t active_cycles = 0;
    bool has_idle_concept = false;

    /* First pass: calculate total cycles for normalization */
    HASH_ITER(hh, rtos->threads, thread, tmp)
    {
        total_cycles += thread->accumulated_cycles;
    }

    /* Calculate CPU percentages and update max values for all threads */
    HASH_ITER(hh, rtos->threads, thread, tmp)
    {
        /* Use cycle-based normalization when available (matches console output) */
        uint32_t cpu_pct = 0;
        if (thread->accumulated_cycles > 0 && total_cycles > 0)
        {
            cpu_pct = (uint32_t)((thread->accumulated_cycles * 10000ULL) / total_cycles);
        }
        else if (window_time_us > 0 && thread->accumulated_time_us > 0)
        {
            uint64_t temp = (uint64_t)thread->accumulated_time_us * 10000ULL;
            cpu_pct = (uint32_t)(temp / window_time_us);
        }
        if (cpu_pct > 10000) cpu_pct = 10000;

        /* Update max CPU percentage */
        if (cpu_pct > thread->max_cpu_percent)
        {
            thread->max_cpu_percent = cpu_pct;
        }

        /* Track totals for overall CPU usage calculation */
        total_accum_us += thread->accumulated_time_us;

        /* Check if this is an idle thread using RTOS-specific method */
        bool is_idle = false;
        if (rtos->ops && rtos->ops->is_idle_thread)
        {
            is_idle = rtos->ops->is_idle_thread(thread);
        }

        if (!is_idle)
        {
            active_accum_us += thread->accumulated_time_us;
            active_cycles += thread->accumulated_cycles;
        }
        else
        {
            has_idle_concept = true;
        }
    }

    /* Update overall CPU usage max if we have idle thread concept */
    if (has_idle_concept)
    {
        uint32_t cpu_usage_pct;
        if (total_cycles > 0)
        {
            cpu_usage_pct = (uint32_t)((active_cycles * 10000ULL) / total_cycles);
        }
        else
        {
            uint64_t temp = (uint64_t)active_accum_us * 10000ULL;
            cpu_usage_pct = (uint32_t)(temp / window_time_us);
        }
        if (cpu_usage_pct > 10000) cpu_usage_pct = 10000;

        if (cpu_usage_pct > rtos->max_cpu_usage)
        {
            rtos->max_cpu_usage = cpu_usage_pct;
        }
    }
}

void rtosResetThreadCounters(struct rtosState *rtos)
{
    if (!rtos || !rtos->enabled || !rtos->threads)
        return;
    
    struct rtosThread *thread, *tmp;
    HASH_ITER(hh, rtos->threads, thread, tmp) 
    {
        thread->accumulated_time_us = 0;
        thread->accumulated_cycles = 0;
        thread->window_switches = 0;
    }
    
    /* Reset timing base to avoid carryover from previous window */
    rtos->last_switch_time = genericsTimestampuS();
    rtos->last_cyccnt = 0;
}