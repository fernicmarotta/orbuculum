
/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Zephyr RTOS Thread Tracking Support for Orbuculum
 * ==================================================
 * Single CPU (cpus[0].current) only.
 *
 * Offset resolution strategy:
 *   1. DWARF struct info from ELF (at init time)
 *   2. Offsets array from target memory (at verify time)
 *   3. Hardcoded defaults (fallback)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <symbols.h>
#include <rtos/zephyr/zephyr.h>
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


const char *zephyrGetPriorityName(int8_t priority)
{
    static char buf[16];
    if (priority < 0)
        snprintf(buf, sizeof(buf), "Coop(%d)", priority);
    else
        snprintf(buf, sizeof(buf), "Pre(%d)", priority);
    return buf;
}


/* -------------------------------------------------------------------------
 * Thread info reading
 * ------------------------------------------------------------------------- */

static bool read_thread_name(struct rtosThread *thread, uint32_t tcb_addr,
                             uint32_t name_offset, uint8_t max_name_len)
{
    if (!rtos_name_is_unknown(thread->name))
        return true;

    if (name_offset == ZEPHYR_OFFSET_NOT_AVAILABLE)
    {
        if (thread->name[0] == 0)
            strcpy(thread->name, RTOS_NAME_NO_NAME);
        return false;
    }

    /* Zephyr stores the name as an inline char[] (not a pointer) */
    uint32_t name_addr = tcb_addr + name_offset;
    char name_buf[RTOS_THREAD_NAME_MAX_LEN] = {0};
    size_t read_len = (max_name_len < sizeof(name_buf) - 1) ? max_name_len : sizeof(name_buf) - 1;

    genericsReport(V_DEBUG, "Zephyr name: TCB=0x%08X off=0x%X addr=0x%08X read_len=%zu" EOL,
                  tcb_addr, name_offset, name_addr, read_len);

    char *name_str = rtosReadMemoryString(name_addr, name_buf, read_len + 1);

    genericsReport(V_DEBUG, "Zephyr name: result=%s buf[0]=0x%02X '%s'" EOL,
                  name_str ? "OK" : "NULL", (unsigned char)name_buf[0], name_buf);

    if (!name_str || name_buf[0] < 0x20 || name_buf[0] >= 0x7F)
    {
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
                                 uint32_t prio_offset)
{
    if (prio_offset == ZEPHYR_OFFSET_NOT_AVAILABLE)
        return;

    /* Zephyr prio is int8_t — may sit at an unaligned offset.
     * mdw only does aligned 4-byte reads, so read the containing
     * word and extract the correct byte. */
    uint32_t byte_addr = tcb_addr + prio_offset;
    uint32_t aligned_addr = byte_addr & ~3u;
    uint32_t byte_pos = byte_addr & 3u;

    uint32_t word = rtosReadMemoryWord(aligned_addr);
    uint8_t raw = (uint8_t)((word >> (byte_pos * 8)) & 0xFF);
    thread->priority = (int8_t)raw;

    genericsReport(V_DEBUG, "Zephyr prio: TCB=0x%08X off=0x%X addr=0x%08X aligned=0x%08X pos=%u word=0x%08X raw=0x%02X prio=%d" EOL,
                  tcb_addr, prio_offset, byte_addr, aligned_addr, byte_pos, word, raw, thread->priority);
}


static void detect_entry_function(struct rtosThread *thread,
                                  struct SymbolSet *symbols,
                                  struct zephyr_private *priv,
                                  uint32_t tcb_addr)
{
    thread->entry_func = 0;
    thread->entry_func_name = NULL;

    if (!priv || priv->off_t_entry == ZEPHYR_OFFSET_NOT_AVAILABLE)
        return;

    /* In Zephyr, entry.pEntry is the first field of struct __thread_entry.
     * So the function pointer is at tcb_addr + off_t_entry directly. */
    uint32_t entry_addr = tcb_addr + priv->off_t_entry;
    uint32_t entry_ptr = rtosReadMemoryWord(entry_addr);

    genericsReport(V_DEBUG, "Zephyr entry: TCB=0x%08X off=0x%X addr=0x%08X ptr=0x%08X" EOL,
                  tcb_addr, priv->off_t_entry, entry_addr, entry_ptr);

    if (!entry_ptr || entry_ptr == 0xFFFFFFFF)
        return;

    /* Clear thumb bit for lookup */
    uint32_t func_addr = entry_ptr & ~1u;
    const char *func_name = rtosLookupPointerAsFunction(symbols, func_addr);

    genericsReport(V_DEBUG, "Zephyr entry: func_addr=0x%08X name=%s" EOL,
                  func_addr, func_name ? func_name : "NOT_FOUND");

    if (func_name)
    {
        thread->entry_func = func_addr;
        thread->entry_func_name = func_name;
    }
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

static int zephyr_read_thread_info(struct rtosState *rtos,
                                   struct SymbolSet *symbols,
                                   struct rtosThread *thread,
                                   uint32_t tcb_addr)
{
    if (!rtos || !thread || !tcb_addr || tcb_addr == 0xFFFFFFFF)
        return -1;

    struct zephyr_private *priv = (struct zephyr_private *)rtos->priv;

    uint32_t old_name_hash = thread->name_hash;
    uint32_t old_func_hash = thread->func_hash;

    read_thread_name(thread, tcb_addr,
                     priv ? priv->off_t_name : ZEPHYR_OFFSET_NOT_AVAILABLE,
                     priv ? priv->max_thread_name_len : ZEPHYR_DEFAULT_MAX_THREAD_NAME_LEN);

    read_thread_priority(thread, tcb_addr,
                         priv ? priv->off_t_prio : ZEPHYR_OFFSET_NOT_AVAILABLE);

    detect_entry_function(thread, symbols, priv, tcb_addr);

    if (rtos_name_is_unknown(thread->name) && thread->entry_func == 0)
    {
        genericsReport(V_DEBUG, "Zephyr: Rejecting TCB=0x%08X - all reads failed" EOL, tcb_addr);
        return -1;
    }

    bool reused = check_thread_reuse(thread, old_name_hash, old_func_hash, tcb_addr);

    return reused ? 1 : 0;
}


/* -------------------------------------------------------------------------
 * Symbol lookup via objdump
 * ------------------------------------------------------------------------- */

static uint32_t find_symbol_address(const char *elfFile, const char *symbol_name)
{
    if (!elfFile || !symbol_name)
        return 0;

    char cmd[512];
    FILE *fp;
    char line[256];
    uint32_t address = 0;

    snprintf(cmd, sizeof(cmd), "arm-none-eabi-objdump -t %s 2>/dev/null | grep '%s$'",
             elfFile, symbol_name);

    fp = popen(cmd, "r");
    if (fp && fgets(line, sizeof(line), fp))
        address = strtoul(line, NULL, 16);
    if (fp)
        pclose(fp);

    return address;
}


/* -------------------------------------------------------------------------
 * DWARF offset detection
 * ------------------------------------------------------------------------- */

static void detect_offsets_from_dwarf(struct zephyr_private *priv, struct SymbolSet *symbols)
{
    int32_t off;

    /* _kernel.cpus[0].current → combined offset for DWT watchpoint */
    int32_t cpus_off = SymbolGetStructOffset(symbols, "z_kernel", "cpus");
    int32_t curr_off = SymbolGetStructOffset(symbols, "_cpu", "current");

    if (cpus_off >= 0 && curr_off >= 0)
    {
        priv->off_k_curr_thread = (uint32_t)(cpus_off + curr_off);
        genericsReport(V_INFO, "Zephyr DWARF: cpus offset=%d, current offset=%d → k_curr_thread=%u" EOL,
                      cpus_off, curr_off, priv->off_k_curr_thread);
    }

    /* _kernel.threads (thread list head, requires CONFIG_THREAD_MONITOR) */
    off = SymbolGetStructOffset(symbols, "z_kernel", "threads");
    if (off >= 0)
    {
        priv->off_k_threads = (uint32_t)off;
        genericsReport(V_INFO, "Zephyr DWARF: threads list offset=%d" EOL, off);
    }

    /* k_thread field offsets */
    off = SymbolGetStructOffset(symbols, "k_thread", "name");
    if (off >= 0)
    {
        priv->off_t_name = (uint32_t)off;
        genericsReport(V_INFO, "Zephyr DWARF: name offset=%d" EOL, off);
    }

    int32_t name_size = SymbolGetStructFieldSize(symbols, "k_thread", "name");
    if (name_size > 0)
        priv->max_thread_name_len = (uint8_t)name_size;

    off = SymbolGetStructOffset(symbols, "k_thread", "entry");
    if (off >= 0)
    {
        priv->off_t_entry = (uint32_t)off;
        genericsReport(V_INFO, "Zephyr DWARF: entry offset=%d" EOL, off);
    }

    off = SymbolGetStructOffset(symbols, "k_thread", "next_thread");
    if (off >= 0)
    {
        priv->off_t_next_thread = (uint32_t)off;
        genericsReport(V_INFO, "Zephyr DWARF: next_thread offset=%d" EOL, off);
    }

    /* base.thread_state and base.prio - need base offset + field offset */
    int32_t base_off = SymbolGetStructOffset(symbols, "k_thread", "base");
    if (base_off >= 0)
    {
        int32_t state_off = SymbolGetStructOffset(symbols, "_thread_base", "thread_state");
        if (state_off >= 0)
        {
            priv->off_t_state = (uint32_t)(base_off + state_off);
            genericsReport(V_INFO, "Zephyr DWARF: state offset=%d (base=%d + state=%d)" EOL,
                          priv->off_t_state, base_off, state_off);
        }

        int32_t prio_off = SymbolGetStructOffset(symbols, "_thread_base", "prio");
        if (prio_off >= 0)
        {
            priv->off_t_prio = (uint32_t)(base_off + prio_off);
            genericsReport(V_INFO, "Zephyr DWARF: prio offset=%d (base=%d + prio=%d)" EOL,
                          priv->off_t_prio, base_off, prio_off);
        }
    }

    /* Stack pointer - architecture-dependent */
    off = SymbolGetStructOffset(symbols, "k_thread", "callee_saved");
    if (off >= 0)
    {
        priv->off_t_stack_ptr = (uint32_t)off;
        genericsReport(V_INFO, "Zephyr DWARF: callee_saved (stack_ptr) offset=%d" EOL, off);
    }
}


/* -------------------------------------------------------------------------
 * Read offsets array from target memory
 * ------------------------------------------------------------------------- */

static int read_offsets_from_target(struct zephyr_private *priv)
{
    if (!priv->offsets_array_addr)
        return -1;

    /* Read sizeof(size_t) from target if available */
    if (priv->size_t_size_addr)
    {
        uint32_t sz = rtosReadMemoryWord(priv->size_t_size_addr);
        if (sz == 4 || sz == 8)
            priv->size_t_size = (uint8_t)sz;
    }

    if (priv->size_t_size != 4)
    {
        genericsReport(V_WARN, "Zephyr: Unsupported size_t size %u (only 32-bit targets supported)" EOL,
                      priv->size_t_size);
        return -1;
    }

    /* Read each offset from the array */
    uint32_t offsets[ZEPHYR_NUM_OFFSETS];
    for (int i = 0; i < ZEPHYR_NUM_OFFSETS; i++)
        offsets[i] = rtosReadMemoryWord(priv->offsets_array_addr + (i * priv->size_t_size));

    /* Validate version */
    if (offsets[ZEPHYR_OFF_VERSION] != 1)
    {
        genericsReport(V_WARN, "Zephyr: Unexpected offsets version %u" EOL, offsets[ZEPHYR_OFF_VERSION]);
        return -1;
    }

    genericsReport(V_INFO, "Zephyr: Read offsets array from target (version %u)" EOL,
                  offsets[ZEPHYR_OFF_VERSION]);

    /* Apply offsets - only override if valid (not 0xFFFFFFFF) */
    if (offsets[ZEPHYR_OFF_K_CURR_THREAD] != ZEPHYR_OFFSET_NOT_AVAILABLE)
        priv->off_k_curr_thread = offsets[ZEPHYR_OFF_K_CURR_THREAD];

    if (offsets[ZEPHYR_OFF_K_THREADS] != ZEPHYR_OFFSET_NOT_AVAILABLE)
        priv->off_k_threads = offsets[ZEPHYR_OFF_K_THREADS];

    if (offsets[ZEPHYR_OFF_T_ENTRY] != ZEPHYR_OFFSET_NOT_AVAILABLE)
        priv->off_t_entry = offsets[ZEPHYR_OFF_T_ENTRY];

    if (offsets[ZEPHYR_OFF_T_NEXT_THREAD] != ZEPHYR_OFFSET_NOT_AVAILABLE)
        priv->off_t_next_thread = offsets[ZEPHYR_OFF_T_NEXT_THREAD];

    if (offsets[ZEPHYR_OFF_T_STATE] != ZEPHYR_OFFSET_NOT_AVAILABLE)
        priv->off_t_state = offsets[ZEPHYR_OFF_T_STATE];

    if (offsets[ZEPHYR_OFF_T_PRIO] != ZEPHYR_OFFSET_NOT_AVAILABLE)
        priv->off_t_prio = offsets[ZEPHYR_OFF_T_PRIO];

    if (offsets[ZEPHYR_OFF_T_STACK_PTR] != ZEPHYR_OFFSET_NOT_AVAILABLE)
        priv->off_t_stack_ptr = offsets[ZEPHYR_OFF_T_STACK_PTR];

    if (offsets[ZEPHYR_OFF_T_NAME] != ZEPHYR_OFFSET_NOT_AVAILABLE)
        priv->off_t_name = offsets[ZEPHYR_OFF_T_NAME];

    priv->offsets_from_target = true;

    /* Recompute DWT watchpoint address */
    priv->current_thread_addr = priv->kernel_addr + priv->off_k_curr_thread;

    genericsReport(V_INFO, "Zephyr offsets: curr_thread=%u threads=%u entry=0x%X next=0x%X "
                  "state=0x%X prio=0x%X name=0x%X stack=0x%X" EOL,
                  priv->off_k_curr_thread, priv->off_k_threads, priv->off_t_entry,
                  priv->off_t_next_thread, priv->off_t_state, priv->off_t_prio,
                  priv->off_t_name, priv->off_t_stack_ptr);

    return 0;
}


/* -------------------------------------------------------------------------
 * Initialization and detection
 * ------------------------------------------------------------------------- */

static bool zephyr_detect(struct SymbolSet *symbols, struct rtosDetection *result)
{
    if (!result)
        return false;

    result->type = RTOS_ZEPHYR;
    result->name = "Zephyr";
    result->confidence = 90;
    result->reason = "Zephyr selected by user";
    return true;
}


static void init_default_offsets(struct zephyr_private *priv)
{
    priv->size_t_size = 4;  /* 32-bit ARM */
    priv->off_k_curr_thread = ZEPHYR_DEFAULT_K_CURR_THREAD_OFFSET;
    priv->off_k_threads = ZEPHYR_OFFSET_NOT_AVAILABLE;
    priv->off_t_entry = ZEPHYR_OFFSET_NOT_AVAILABLE;
    priv->off_t_next_thread = ZEPHYR_OFFSET_NOT_AVAILABLE;
    priv->off_t_state = ZEPHYR_OFFSET_NOT_AVAILABLE;
    priv->off_t_prio = ZEPHYR_OFFSET_NOT_AVAILABLE;
    priv->off_t_name = ZEPHYR_OFFSET_NOT_AVAILABLE;
    priv->off_t_stack_ptr = ZEPHYR_OFFSET_NOT_AVAILABLE;
    priv->max_thread_name_len = ZEPHYR_DEFAULT_MAX_THREAD_NAME_LEN;
    priv->offsets_from_target = false;
}


static int find_kernel_symbols(struct zephyr_private *priv, const char *elfFile)
{
    priv->kernel_addr = find_symbol_address(elfFile, ZEPHYR_SYM_KERNEL);
    if (priv->kernel_addr == 0)
    {
        genericsReport(V_ERROR, "Zephyr: Symbol '%s' not found!" EOL, ZEPHYR_SYM_KERNEL);
        return -1;
    }

    genericsReport(V_INFO, "Zephyr: _kernel at 0x%08X" EOL, priv->kernel_addr);

    /* Try both naming conventions for offsets array */
    priv->offsets_array_addr = find_symbol_address(elfFile, ZEPHYR_SYM_OFFSETS);
    if (priv->offsets_array_addr == 0)
        priv->offsets_array_addr = find_symbol_address(elfFile, ZEPHYR_SYM_OFFSETS_ALT);

    if (priv->offsets_array_addr)
        genericsReport(V_INFO, "Zephyr: offsets array at 0x%08X" EOL, priv->offsets_array_addr);
    else
        genericsReport(V_WARN, "Zephyr: offsets array not found (CONFIG_DEBUG_THREAD_INFO=y needed)" EOL);

    /* Try both naming conventions for size_t_size */
    priv->size_t_size_addr = find_symbol_address(elfFile, ZEPHYR_SYM_SIZE_T_SIZE);
    if (priv->size_t_size_addr == 0)
        priv->size_t_size_addr = find_symbol_address(elfFile, ZEPHYR_SYM_SIZE_T_SIZE_ALT);

    return 0;
}


static int zephyr_init(struct rtosState *rtos, struct SymbolSet *symbols)
{
    if (!rtos)
        return -1;

    struct zephyr_private *priv = calloc(1, sizeof(struct zephyr_private));
    if (!priv)
        return -1;

    init_default_offsets(priv);
    rtos->priv = priv;

    if (symbols && symbols->elfFile)
    {
        if (find_kernel_symbols(priv, symbols->elfFile) < 0)
        {
            free(priv);
            rtos->priv = NULL;
            return -1;
        }

        detect_offsets_from_dwarf(priv, symbols);
    }

    /* Compute DWT watchpoint address */
    priv->current_thread_addr = priv->kernel_addr + priv->off_k_curr_thread;
    genericsReport(V_INFO, "Zephyr: Current thread pointer at 0x%08X" EOL, priv->current_thread_addr);

    return 0;
}


static void zephyr_cleanup(struct rtosState *rtos)
{
    if (rtos && rtos->priv)
    {
        free(rtos->priv);
        rtos->priv = NULL;
    }
}


/* -------------------------------------------------------------------------
 * State and priority helpers
 * ------------------------------------------------------------------------- */

static const char *zephyr_get_state_name(uint8_t state)
{
    if (state & ZEPHYR_THREAD_DEAD)
        return "Dead";
    if (state & ZEPHYR_THREAD_SUSPENDED)
        return "Suspended";
    if (state & ZEPHYR_THREAD_SLEEPING)
        return "Sleeping";
    if (state & ZEPHYR_THREAD_PENDING)
        return "Blocked";
    if (state & ZEPHYR_THREAD_QUEUED)
        return "Ready";
    if (state & ZEPHYR_THREAD_DUMMY)
        return "Dummy";
    return "Running";
}


static bool zephyr_is_idle_thread(struct rtosThread *thread)
{
    if (!thread)
        return false;

    /* Zephyr idle thread is typically named "idle" or "idle 00" */
    if (strncasecmp(thread->name, "idle", 4) == 0)
        return true;

    if (thread->entry_func_name &&
        (strcmp(thread->entry_func_name, "idle") == 0 ||
         strcmp(thread->entry_func_name, "z_thread_idle") == 0))
        return true;

    return false;
}


/* -------------------------------------------------------------------------
 * Target verification
 * ------------------------------------------------------------------------- */

static int ensure_telnet_connected(struct rtosState *rtos)
{
    if (rtos->telnet_port <= 0)
        return RTOS_VERIFY_SUCCESS;

    if (!telnet_is_connected() && telnet_connect(rtos->telnet_port) < 0)
        return RTOS_VERIFY_NO_CONNECTION;

    return RTOS_VERIFY_SUCCESS;
}


static int zephyr_verify_target_match(struct rtosState *rtos, struct SymbolSet *symbols)
{
    if (!rtos || !rtos->priv)
        return RTOS_VERIFY_ERROR;

    struct zephyr_private *priv = (struct zephyr_private *)rtos->priv;

    int conn = ensure_telnet_connected(rtos);
    if (conn != RTOS_VERIFY_SUCCESS)
        return conn;

    /* Try to read offsets from target memory to refine/validate */
    if (priv->offsets_array_addr)
    {
        if (read_offsets_from_target(priv) == 0)
            genericsReport(V_INFO, "Zephyr: Offsets validated from target memory" EOL);
    }

    /* Verify current thread pointer is readable */
    uint32_t current_tcb = rtosReadMemoryWord(priv->current_thread_addr);
    if (current_tcb == 0 || current_tcb == 0xFFFFFFFF)
    {
        genericsReport(V_INFO, "Zephyr: Kernel not started yet (current=0x%08X)" EOL, current_tcb);
        return RTOS_VERIFY_SUCCESS;  /* Not an error - kernel may not be running yet */
    }

    /* Try to read thread name to confirm it's a valid TCB */
    if (priv->off_t_name != ZEPHYR_OFFSET_NOT_AVAILABLE)
    {
        char name_buf[RTOS_THREAD_NAME_MAX_LEN] = {0};
        rtosReadMemoryString(current_tcb + priv->off_t_name, name_buf, sizeof(name_buf));
        if (name_buf[0] && name_buf[0] != (char)0xFF)
            genericsReport(V_INFO, "Zephyr: Verified - thread '%s' at 0x%08X" EOL, name_buf, current_tcb);
    }

    return RTOS_VERIFY_SUCCESS;
}


static uint32_t zephyr_get_watchpoint_addr(struct rtosState *rtos)
{
    if (!rtos || !rtos->priv)
        return 0;
    struct zephyr_private *priv = (struct zephyr_private *)rtos->priv;
    return priv->current_thread_addr;
}


/* -------------------------------------------------------------------------
 * Operations table
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Object tracking (minimal — Zephyr has no unified object header)
 * ------------------------------------------------------------------------- */

static int zephyr_read_object_info(struct rtosState *rtos, struct rtosObject *obj, uint32_t cb_addr)
{
    if (!rtos || !obj || !cb_addr)
        return -1;

    /* Zephyr objects have no common type ID field.  The firmware encodes
     * the object type in bits [1:0] of the DWT comp1 value, which
     * rtosHandleObjectEvent() stores in pending_type_hint before calling us.
     *   00 = mutex, 01 = sem, 10 = msgq, 11 = event */
    static const struct
    {
        enum rtosObjectType type;
        const char *prefix;
    } type_map[] =
    {
        { RTOS_OBJ_MUTEX,         "mutex"    },  /* hint = 0 */
        { RTOS_OBJ_SEMAPHORE,     "sem"      },  /* hint = 1 */
        { RTOS_OBJ_MESSAGE_QUEUE, "msgqueue" },  /* hint = 2 */
        { RTOS_OBJ_EVENT_FLAGS,   "evtflags" },  /* hint = 3 */
    };

    uint8_t hint = rtos->pending_type_hint;

    if (hint < 4)
    {
        obj->type = type_map[hint].type;
        obj->type_prefix = type_map[hint].prefix;
    }
    else
    {
        obj->type = RTOS_OBJ_UNKNOWN;
        obj->type_prefix = "obj";
    }

    /* Zephyr objects have no name registry — use hex address */
    snprintf(obj->name, sizeof(obj->name), "0x%08X", cb_addr);

    genericsReport(V_INFO, "Zephyr Object: CB=0x%08X, type=%s, Name=%s" EOL,
                  cb_addr, obj->type_prefix, obj->name);

    return 0;
}


static const struct rtosOps zephyr_ops =
{
    .read_thread_info = zephyr_read_thread_info,
    .get_priority_name = zephyrGetPriorityName,
    .detect = zephyr_detect,
    .init = zephyr_init,
    .cleanup = zephyr_cleanup,
    .get_state_name = zephyr_get_state_name,
    .is_idle_thread = zephyr_is_idle_thread,
    .verify_target_match = zephyr_verify_target_match,
    .get_watchpoint_addr = zephyr_get_watchpoint_addr,
    .read_object_info = zephyr_read_object_info
};


const struct rtosOps *zephyrGetOps(void)
{
    return &zephyr_ops;
}
