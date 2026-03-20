# Zephyr Object Tracking

## Overview

`orbtop-rtos` can track Zephyr kernel object blocking events (mutex,
semaphore, message queue, event) on ARM Cortex-M targets. When a thread
blocks on a kernel object, the event is captured via a DWT watchpoint and
displayed in ftrace/Perfetto output with `prev_state=D` and a named
counter track.

## Architecture

```
 Firmware (Cortex-M / Zephyr)           Host (orbuculum)
 ───────────────────────────            ────────────────
 sys_port_trace_k_mutex_lock_blocking   ITM decoder
   │                                      │
   ▼                                      ▼
 rtos_obj_trace = k_mutex* | tag ─DWT─► _handleDataAccessWP (comp2)
                             comp2         │
                             ITM pkt       ▼
                                        rtosHandleObjectEvent()
                                           │
                                           ▼
                                        find_or_create_object()
                                           │
                                           ▼
                                        zephyr_read_object_info()
                                           │  uses pending_type_hint
                                           │  (bits [1:0] from firmware)
                                           ▼
                                        ftrace output:
                                          prev_state=D
                                          C|1|mutex:0x20001234|1
                                          C|1|mutex:0x20001234|0
```

## Type Encoding

Unlike FreeRTOS (where all objects share the `Queue_t` structure and have
a `ucQueueType` field), Zephyr kernel objects (`k_mutex`, `k_sem`, `k_msgq`,
`k_event`) have no common type identifier.  The firmware encodes the object
type in bits [1:0] of the address written to `rtos_obj_trace`:

| bits [1:0] | Type tag | Object          | Prefix     | Example output        |
|------------|----------|-----------------|------------|-----------------------|
| 00         | MUTEX    | `k_mutex`       | `mutex`    | `mutex:0x20001234`    |
| 01         | SEM      | `k_sem`         | `sem`      | `sem:0x20005678`      |
| 10         | MSGQ     | `k_msgq`        | `msgqueue` | `msgqueue:0x2000ABCD` |
| 11         | EVENT    | `k_event`       | `evtflags` | `evtflags:0x2000EF00` |
| value = 0  | —        | delay / sleep   | —          | `prev_state=S`        |

Since all Cortex-M heap/BSS addresses are word-aligned, bits [1:0] are always
0 in real pointers.  The host strips these bits to recover the actual address.

### Objects grouped under MSGQ (tag 10)

| Zephyr Object   | Hook                                           |
|------------------|------------------------------------------------|
| `k_msgq`         | `sys_port_trace_k_msgq_get_blocking`           |
| `k_msgq`         | `sys_port_trace_k_msgq_put_blocking`           |
| `k_pipe`         | `sys_port_trace_k_pipe_get_blocking`           |
| `k_pipe`         | `sys_port_trace_k_pipe_put_blocking`           |
| `k_mbox`         | `sys_port_trace_k_mbox_message_put_blocking`   |
| `k_mbox`         | `sys_port_trace_k_mbox_message_get_blocking`   |
| `k_mem_slab`     | `sys_port_trace_k_mem_slab_alloc_blocking`     |

### Objects grouped under EVENT (tag 11)

| Zephyr Object   | Hook                                           |
|------------------|------------------------------------------------|
| `k_event`        | `sys_port_trace_k_event_wait_blocking`         |
| `k_condvar`      | `sys_port_trace_k_condvar_wait_blocking`       |

## prev_state Mapping

| Value written to `rtos_obj_trace` | Meaning              | ftrace `prev_state` |
|----------------------------------|----------------------|---------------------|
| Object address \| tag (non-zero)  | Blocked on object    | `D`                 |
| `0`                               | Delay / sleep        | `S`                 |
| `0xFFFFFFFF`                      | Invalid (ignored)    | —                   |

## Firmware Setup

### 1. Kconfig (`prj.conf`)

```
CONFIG_THREAD_NAME=y
CONFIG_DEBUG_THREAD_INFO=y
CONFIG_THREAD_MONITOR=y
CONFIG_THREAD_STACK_INFO=y
CONFIG_DEBUG_OPTIMIZATIONS=y
CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y
```

No `CONFIG_TRACING` is needed.  The trace hooks are overridden at the
preprocessor level using a `tracing.h` wrapper (see below).

### 2. Trace hook override via `#include_next` wrapper

Zephyr defines the `sys_port_trace_*_blocking` macros as empty no-ops
in `<zephyr/tracing/tracing.h>`.  Since these are `#define` macros (not
weak functions), the only way to override them is at the preprocessor
level with `#undef` + `#define`.

The approach uses a wrapper header that shadows Zephyr's `tracing.h`:

**Directory structure:**

```
samples/obj_tracking/
├── CMakeLists.txt
├── prj.conf
└── src/
    ├── main.c
    ├── orbuculum_obj_trace.h          ← #undef + #define overrides
    └── zephyr/tracing/
        └── tracing.h                  ← wrapper (shadows Zephyr's)
```

**`CMakeLists.txt`** — place `src/` BEFORE Zephyr's include paths:

```cmake
zephyr_include_directories(BEFORE src)
```

**`src/zephyr/tracing/tracing.h`** — wrapper that includes the real
header first, then applies overrides:

```c
/* Include the real Zephyr tracing.h first */
#include_next <zephyr/tracing/tracing.h>

/* Override the blocking hooks with our DWT comp2 writes */
#include "orbuculum_obj_trace.h"
```

**`src/orbuculum_obj_trace.h`** — the actual hook overrides:

```c
#ifndef ORBUCULUM_OBJ_TRACE_H
#define ORBUCULUM_OBJ_TRACE_H

#include <stdint.h>

extern volatile uint32_t rtos_obj_trace;

#define ZEPHYR_OBJ_MUTEX  0u  /* 00 */
#define ZEPHYR_OBJ_SEM    1u  /* 01 */
#define ZEPHYR_OBJ_MSGQ   2u  /* 10 */
#define ZEPHYR_OBJ_EVENT  3u  /* 11 */

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

#undef sys_port_trace_k_thread_sleep_enter
#define sys_port_trace_k_thread_sleep_enter(timeout) \
    do { rtos_obj_trace = 0; } while (0)

#endif
```

Add more hooks as needed (pipes, mailbox, mem_slab, condvar) following
the same pattern with the appropriate type tag.

**How it works:** when Zephyr's kernel compiles `mutex.c` and does
`#include <zephyr/tracing/tracing.h>`, it finds our wrapper first
(thanks to `BEFORE`).  The wrapper uses `#include_next` to pull in the
real Zephyr header (which defines empty macros), then includes
`orbuculum_obj_trace.h` which `#undef`s and redefines the blocking
hooks with our DWT comp2 writes.  This works on all Zephyr versions
(3.x and 4.x).

### 3. Define the trace variable

In any `.c` file (e.g. `main.c`):

```c
volatile uint32_t rtos_obj_trace __attribute__((used));
```

### 4. OpenOCD configuration

Same as FreeRTOS — configure DWT comparator 2 on the `rtos_obj_trace`
address:

```tcl
set DWT_COMP2 0xE0001040
set DWT_MASK2 0xE0001044
set DWT_FUNC2 0xE0001048

proc rtos_dwt2_config {addr} {
    global DWT_COMP2 DWT_MASK2 DWT_FUNC2
    mww $DWT_COMP2 $addr
    mww $DWT_MASK2 0
    mww $DWT_FUNC2 0x0D
    echo "DWT Comparator 2 configured for data write+value at [format 0x%08X $addr]"
}
```

### 5. Run orbtop-rtos

```bash
./build/orbtop-rtos \
    -e firmware.elf \
    -T zephyr \
    -w rtos_obj_trace \
    -s localhost:42995 \
    -W 43109 \
    -F 480000000 \
    -p ITM \
    -K trace.ftrace
```

| Option | Description                                     |
|--------|-------------------------------------------------|
| `-e`   | ELF file (for DWARF + symbols)                  |
| `-T`   | RTOS type (`zephyr`)                            |
| `-w`   | Watchpoint variable for object tracking (comp2) |
| `-s`   | orbuculum server (host:port)                    |
| `-W`   | OpenOCD telnet port                             |
| `-F`   | CPU frequency in Hz                             |
| `-p`   | Trace protocol (`ITM` or `ETM`)                 |
| `-K`   | ftrace output file                              |

## How It Works Internally

### Type detection (firmware-side, via type tag)

Unlike FreeRTOS where the host reads `ucQueueType` from the target, in
Zephyr the type is encoded by the firmware in bits [1:0] of the value
written to `rtos_obj_trace`.

When `rtosHandleObjectEvent()` receives a DWT comp2 value:

1. **Strip bits [1:0]** → `type_hint` (0–3) + `real_addr`
2. **Store** `type_hint` in `rtos->pending_type_hint`
3. **Call** `zephyr_read_object_info()` which maps `type_hint` to
   `rtosObjectType` and prefix string

No telnet memory reads are needed for type detection — the firmware
already encoded the type.

### Name resolution

Zephyr kernel objects do not have name registries (unlike FreeRTOS
`xQueueRegistry`).  Object names are always the hex address:
`mutex:0x20001234`, `sem:0x20005678`.

## Ftrace Output

Thread switches with blocking show `prev_state=D`:
```
mutex_a|task_mutex_a-603979776 [000] .... 12.345: sched_switch: ... prev_state=D ==> next_comm=mutex_b ...
```

Object events appear as counter tracks:
```
rtos_obj-1 [000] .... 12.340: tracing_mark_write: C|1|mutex:0x20001234|1
rtos_obj-1 [000] .... 12.345: tracing_mark_write: C|1|mutex:0x20001234|0
```

## Limitations

### Objects have no names

Zephyr kernel objects do not have name fields or registries.  Objects
always appear with their hex address: `mutex:0x20001234`.

### Event Groups have no names

Same as FreeRTOS — `k_event` objects show as `evtflags:0x20001234`.

### Trace hook stability

The `sys_port_trace_*_blocking` macro signatures are not part of
Zephyr's stable API.  If Zephyr changes the parameter list of these
macros in a future release, `orbuculum_obj_trace.h` will need updating.
The orbuculum host side is not affected — it only reads the DWT comp2
value.

### Single CPU only

The current implementation only supports single-CPU Zephyr targets
(Cortex-M).  SMP configurations are not supported.

## Backward Compatibility

The extension from 1-bit (FreeRTOS) to 2-bit (Zephyr) type encoding is
backward compatible:

- **FreeRTOS**: Queue_t addresses use tag 00, EventGroup_t uses tag 01.
  Bit 1 is always 0, so the old `& ~1u` behavior is preserved by `& ~3u`.
- **RTX5**: Same as FreeRTOS (bit 1 always 0).
- **Zephyr**: Uses all four tag values (00–11).
