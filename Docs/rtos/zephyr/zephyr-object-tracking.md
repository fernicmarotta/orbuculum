# Zephyr Object Tracking

## Overview

`orbtop-rtos` can track Zephyr kernel object blocking and release events
(mutex, semaphore, message queue, event, pipe, mailbox, mem_slab, condvar,
stack) on ARM Cortex-M targets. When a thread blocks on a kernel object,
the event is captured via a DWT watchpoint and displayed in ftrace output
with `prev_state=D` (`sched_switch`) and a named counter track. Release
events show when the object is unlocked, allowing separation of contention
time from scheduling latency.

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

Zephyr kernel objects have no common type identifier.  The firmware encodes
the object type and acquire/release direction in bits [2:0] of the address
written to `rtos_obj_trace`:

```
[31:3] = object address
[2]    = 0: acquire (blocking), 1: release (unlock/give/post)
[1:0]  = type tag (00:mutex, 01:sem, 10:msgq, 11:event)
```

| bits [1:0] | Type tag | Objects                                       | Prefix     |
|------------|----------|-----------------------------------------------|------------|
| 00         | MUTEX    | `k_mutex`                                      | `mutex`    |
| 01         | SEM      | `k_sem`                                        | `sem`      |
| 10         | MSGQ     | `k_msgq`, `k_pipe`, `k_mbox`, `k_mem_slab`, `k_stack` | `msgqueue` |
| 11         | EVENT    | `k_event`, `k_condvar`                          | `evtflags` |
| value = 0  | —        | delay / sleep                                   | —          |

Since all Cortex-M SRAM addresses are at least 4-byte aligned, bits [2:0]
are always 0 in real pointers.  The host strips these bits to recover the
actual address.

### Objects grouped under MSGQ (tag 10)

| Zephyr Object | Blocking hook                                  | Release hook                                       |
|---------------|------------------------------------------------|----------------------------------------------------|
| `k_msgq`      | `sys_port_trace_k_msgq_get_blocking`           | `sys_port_trace_k_msgq_get_exit`                   |
| `k_msgq`      | `sys_port_trace_k_msgq_put_blocking`           | `sys_port_trace_k_msgq_put_exit`                   |
| `k_pipe`      | `sys_port_trace_k_pipe_read_blocking`          | `sys_port_trace_k_pipe_read_exit`                  |
| `k_pipe`      | `sys_port_trace_k_pipe_write_blocking`         | `sys_port_trace_k_pipe_write_exit`                 |
| `k_mbox`      | `sys_port_trace_k_mbox_message_put_blocking`   | `sys_port_trace_k_mbox_message_put_exit`           |
| `k_mbox`      | `sys_port_trace_k_mbox_get_blocking`           | `sys_port_trace_k_mbox_get_exit`                   |
| `k_mem_slab`  | `sys_port_trace_k_mem_slab_alloc_blocking`     | `sys_port_trace_k_mem_slab_free_exit`              |
| `k_stack`     | `sys_port_trace_k_stack_pop_blocking`          | `sys_port_trace_k_stack_push_exit`                 |

### Objects grouped under EVENT (tag 11)

| Zephyr Object | Blocking hook                                  | Release hook                                       |
|---------------|------------------------------------------------|----------------------------------------------------|
| `k_event`     | `sys_port_trace_k_event_wait_blocking`         | `sys_port_trace_k_event_post_exit`                 |
| `k_condvar`   | `sys_port_trace_k_condvar_wait_blocking`       | `sys_port_trace_k_condvar_signal_exit`             |
| `k_condvar`   |                                                | `sys_port_trace_k_condvar_broadcast_exit`          |

### Not trackable

`k_fifo`, `k_lifo`, `k_poll`, `k_heap`, `k_timer`, `k_thread_join` — these
have no `_blocking` hook in the Zephyr tracing API.

## prev_state Mapping

| Value written to `rtos_obj_trace` | Meaning              | ftrace `prev_state` |
|----------------------------------|----------------------|---------------------|
| Object address \| tag (non-zero)  | Blocked on object    | `D`                 |
| `0`                               | Delay / sleep        | `S`                 |
| `0xFFFFFFFF`                      | Invalid (ignored)    | —                   |

## Firmware Setup

### 1. Kconfig (`prj.conf`)

```
CONFIG_TRACING=y
CONFIG_THREAD_NAME=y
CONFIG_DEBUG_THREAD_INFO=y
CONFIG_THREAD_MONITOR=y
CONFIG_THREAD_STACK_INFO=y
CONFIG_DEBUG_OPTIMIZATIONS=y
CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y
```

`CONFIG_TRACING=y` is required so that the `SYS_PORT_TRACING_OBJ_FUNC_BLOCKING`
macros in the kernel expand to `sys_port_trace_*_blocking` calls.  Without it
these macros are compiled out entirely and our overrides have no effect.

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

**`CMakeLists.txt`** — place `src/` BEFORE Zephyr's include paths.
`zephyr_include_directories()` does not support the `BEFORE` keyword (it
treats it as a directory name).  Use CMake native
`target_include_directories()` on the `kernel` target instead:

```cmake
target_include_directories(kernel BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(zephyr BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
```

**`src/zephyr/tracing/tracing.h`** — wrapper that includes the real
header first, then applies overrides.  The header guard is critical:
Zephyr's own `tracing.h` may re-include `<zephyr/tracing/tracing.h>`
internally during its processing, and without the guard our overrides
would run before the real header finishes, letting Zephyr overwrite them.

```c
#ifndef ORBUCULUM_TRACING_WRAPPER_H
#define ORBUCULUM_TRACING_WRAPPER_H

/* Include the real Zephyr tracing.h first (fully) */
#include_next <zephyr/tracing/tracing.h>

/* Now override the blocking hooks with our DWT comp2 writes */
#include "orbuculum_obj_trace.h"

#endif /* ORBUCULUM_TRACING_WRAPPER_H */
```

**`src/orbuculum_obj_trace.h`** — the actual hook overrides:

```c
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

/* === Blocking hooks (acquire) === */

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

/* === Release hooks === */

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
```

**How it works:** when Zephyr's kernel compiles `mutex.c` and does
`#include <zephyr/tracing/tracing.h>`, it finds our wrapper first
(thanks to `BEFORE` on the `kernel` target).  The header guard prevents
recursive entry.  `#include_next` pulls in the real Zephyr header, which
with `CONFIG_TRACING=y` defines the `sys_port_trace_*_blocking` macros
as no-ops.  Then `orbuculum_obj_trace.h` `#undef`s and redefines them
with our DWT comp2 writes.  This works on all Zephyr versions (3.x and
4.x) without requiring
[`CONFIG_TRACING_CUSTOM`](https://github.com/zephyrproject-rtos/zephyr/blob/main/subsys/tracing/Kconfig),
which was added to `main` in
[PR #102290](https://github.com/zephyrproject-rtos/zephyr/pull/102290)
(not yet in any release as of v4.3.0) and is marked as `[EXPERIMENTAL]`.

### 3. Define the trace variable

In any `.c` file (e.g. `main.c`):

```c
volatile uint32_t rtos_obj_trace __attribute__((used));
```

### 4. OpenOCD configuration

Configure DWT comparator 2 on the `rtos_obj_trace` address:

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

The object type and acquire/release direction are encoded by the firmware
in bits [2:0] of the value written to `rtos_obj_trace`.

When `rtosHandleObjectEvent()` receives a DWT comp2 value:

1. **Strip bits [2:0]** → `is_release` (bit 2), `type_hint` (bits 1:0), `real_addr`
2. If **release**: look up existing object, emit C|0 at release timestamp
3. If **acquire**: store `type_hint` in `rtos->pending_type_hint`,
   call `zephyr_read_object_info()` which maps `type_hint` to
   `rtosObjectType` and prefix string, emit C|1

No telnet memory reads are needed for type detection — the firmware
already encoded the type.

### Release event auto-detection

The host auto-detects whether firmware has release hooks on a per-object
basis.  The first blocking+context-switch cycle for each object uses
backward-compatible behavior (C|0 at context switch).  Once a release
event arrives for an object, subsequent cycles keep the counter high
until the release, showing true contention time in Perfetto.

### Name resolution

Zephyr kernel objects do not have name registries.  Object names are
always the hex address:
`mutex:0x20001234`, `sem:0x20005678`.

## Ftrace Output

All examples below are from a real Zephyr capture (`zephyr_example_trace.ftrace`,
STM32H743, 480 MHz). Zephyr objects have no name registries — all objects show
as hex addresses.

### Context switches by state

**Preempted (R)** — thread still runnable, yielded to equal/higher priority:
```
mutex_a_tid|task_mutex_a-603981200 [000] ....     0.116787: sched_switch: prev_comm=mutex_a_tid|task_mutex_a prev_pid=603981200 prev_prio=5 prev_state=R ==> next_comm=producer_tid|task_producer next_pid=603980048 next_prio=5
```

**Sleeping (S)** — thread called `k_sleep` / `k_msleep`:
```
producer_tid|task_producer-603980048 [000] ....     2.366437: sched_switch: prev_comm=producer_tid|task_producer prev_pid=603980048 prev_prio=5 prev_state=S ==> next_comm=mutex_b_tid|task_mutex_b next_pid=603981008 next_prio=5
```

**Blocked on object (D)** — thread waiting on mutex/sem/msgq/event:
```
mutex_b_tid|task_mutex_b-603981008 [000] ....     2.235023: sched_switch: prev_comm=mutex_b_tid|task_mutex_b prev_pid=603981008 prev_prio=5 prev_state=D ==> next_comm=mutex_a_tid|task_mutex_a next_pid=603981200 next_prio=5
```

### Object tracking — acquire/release pairs

Each pair shows `C|1` (acquire = thread blocks) and `C|0` (release = object freed).

**mutex** (k_mutex):
```
        rtos_obj-1 [000] ....     2.235014: tracing_mark_write: C|1|mutex:0x24000090|1
        rtos_obj-1 [000] ....     2.235023: tracing_mark_write: C|1|mutex:0x24000090|0
```

**sem** (k_sem):
```
        rtos_obj-1 [000] ....     2.600209: tracing_mark_write: C|1|sem:0x240000E8|1
        rtos_obj-1 [000] ....     2.775444: tracing_mark_write: C|1|sem:0x240000E8|0
```

**evtflags** (k_event):
```
        rtos_obj-1 [000] ....     2.395712: tracing_mark_write: C|1|evtflags:0x24000DC8|1
        rtos_obj-1 [000] ....     2.395721: tracing_mark_write: C|1|evtflags:0x24000DC8|0
```

**msgqueue** (k_msgq):
```
        rtos_obj-1 [000] ....     2.600233: tracing_mark_write: C|1|msgqueue:0x240000A8|1
        rtos_obj-1 [000] ....     2.775439: tracing_mark_write: C|1|msgqueue:0x240000A8|0
```

### Delay events

When `rtos_obj_trace = 0` (k_sleep), the context switch shows `prev_state=S`
with no object counter event:
```
event_tid|task_event_consumer-603980240 [000] ....     2.381083: sched_switch: prev_comm=event_tid|task_event_consumer prev_pid=603980240 prev_prio=5 prev_state=S ==> next_comm=mutex_a_tid|task_mutex_a next_pid=603981200 next_prio=5
```

### Batch release pattern

When a producer signals multiple consumers, multiple `C|0` events appear at
nearly the same timestamp:
```
        rtos_obj-1 [000] ....     2.775439: tracing_mark_write: C|1|msgqueue:0x240000A8|0
        rtos_obj-1 [000] ....     2.775444: tracing_mark_write: C|1|sem:0x240000E8|0
        rtos_obj-1 [000] ....     2.775454: tracing_mark_write: C|1|evtflags:0x24000DC8|0
```

### Complete sequence — mutex contention

A full blocking cycle: thread blocks, context switch, release, resume:
```
        rtos_obj-1 [000] ....     2.235014: tracing_mark_write: C|1|mutex:0x24000090|1
mutex_b_tid|task_mutex_b-603981008 [000] ....     2.235023: sched_switch: prev_comm=mutex_b_tid|task_mutex_b prev_pid=603981008 prev_prio=5 prev_state=D ==> next_comm=mutex_a_tid|task_mutex_a next_pid=603981200 next_prio=5
        rtos_obj-1 [000] ....     2.264211: tracing_mark_write: C|1|mutex:0x24000090|0
```

## Limitations

### Objects have no names

Zephyr kernel objects do not have name fields or registries.  Objects
always appear with their hex address: `mutex:0x20001234`.

### Event objects have no names

`k_event` objects show as `evtflags:0x20001234`.

### Trace hook stability

The `sys_port_trace_*_blocking` macro signatures are not part of
Zephyr's stable API.  If Zephyr changes the parameter list of these
macros in a future release, `orbuculum_obj_trace.h` will need updating.
The orbuculum host side is not affected — it only reads the DWT comp2
value.

### Single CPU only

The current implementation only supports single-CPU Zephyr targets
(Cortex-M).  SMP configurations are not supported.

