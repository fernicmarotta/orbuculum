# FreeRTOS Object Tracking

## Overview

`orbtop-rtos` can track FreeRTOS kernel object blocking and release events
(mutex, semaphore, queue, event group, stream buffer) on ARM Cortex-M
targets. When a task blocks on an object, the event is captured via a DWT
watchpoint and displayed in ftrace output with `prev_state=D`
(`sched_switch`) and a named counter track (`tracing_mark_write`).

Optional release hooks provide precise contention timing: the counter track
stays high from the blocking event until the object is actually released,
allowing you to distinguish contention time from scheduling latency in
Perfetto.

## Architecture

```
 Firmware (Cortex-M)                    Host (orbuculum)
 ──────────────────                     ────────────────
 traceBLOCKING_ON_QUEUE_*()             ITM decoder
   │                                      │
   ▼                                      ▼
 rtos_obj_trace = Queue_t*  ──DWT──►  _handleDataAccessWP (comp1)
                            comp1        │
                            ITM pkt      ▼
                                      rtosHandleObjectEvent()
                                         │
                                         ▼
                                      find_or_create_object()
                                         │
                                         ▼
                                      freertos_read_object_info()
                                         │  reads ucQueueType
                                         │  + xQueueRegistry name
                                         │  via OpenOCD telnet
                                         ▼
                                      ftrace output:
                                        prev_state=D
                                        C|1|mutex:TestMutex|1
                                        C|1|mutex:TestMutex|0
```

## Type Encoding (bits [2:0])

The DWT comp1 value uses three low bits for metadata. Cortex-M SRAM
objects are 4+ byte aligned, so bits [2:0] are always zero in real
pointers and are free for encoding:

```
[31:3] = object address
[2]    = 0: acquire (blocking), 1: release
[1:0]  = type hint (see table below)
```

The host strips all three bits (`& ~7u`) to recover the real address.

| bits[1:0] | Tag      | Objects                                                |
|-----------|----------|--------------------------------------------------------|
| 00        | Queue_t  | mutex, recursive mutex, counting sem, binary sem, queue |
| 01        | EvtGrp   | `EventGroup_t`                                         |
| 10        | Stream   | `StreamBuffer_t` (stream buffers, message buffers)      |
| value=0   | —        | `vTaskDelay` / delay (`prev_state=S`)                  |

For Queue_t objects (tag=00), the host reads `ucQueueType` from the
control block via DWARF to distinguish the subtype:

| ucQueueType | Object Type        | Prefix     | Example output          |
|-------------|-------------------|------------|-------------------------|
| 0           | Queue / Queue Set  | `queue`    | `queue:CmdQueue`        |
| 1           | Mutex              | `mutex`    | `mutex:TestMutex`       |
| 2           | Counting Semaphore | `sem`      | `sem:TestSem`           |
| 3           | Binary Semaphore   | `sem`      | `sem:TestBSem`          |
| 4           | Recursive Mutex    | `rmutex`   | `rmutex:RecMutex`       |

For EvtGrp (tag=01) and Stream (tag=10), the type comes from the firmware
hint — the host skips Queue_t validation.

**Not tracked**: Task Notifications (no distinct object address — the
"object" is the task's own TCB, not a kernel object).

### Requirements

- **`configUSE_TRACE_FACILITY = 1`** in `FreeRTOSConfig.h` — enables
  `ucQueueType` field in `Queue_t`. Without this, all objects appear as `obj:`.
- **`configQUEUE_REGISTRY_SIZE > 0`** — enables name lookup via
  `xQueueRegistry[]`. Without this, objects show as `sem:0x24001234`.

Objects must be registered with `vQueueAddToRegistry()` to have names:

```c
vQueueAddToRegistry(myMutex, "TestMutex");
vQueueAddToRegistry((QueueHandle_t)mySem, "TestSem");
vQueueAddToRegistry(myQueue, "TestQueue");
```

## Firmware Setup

### 1. Add trace hooks to FreeRTOSConfig.h

Copy [`samples/orbuculum_freertos_trace.h`](samples/orbuculum_freertos_trace.h)
into your project and include it from `FreeRTOSConfig.h`:

```c
/* At the end of FreeRTOSConfig.h */
#include "orbuculum_freertos_trace.h"
```

The header defines blocking hooks (acquire) and release hooks for all
FreeRTOS object types: Queue_t (mutex, semaphore, queue), EventGroup,
and StreamBuffer.

**Blocking hooks** (acquire): bit [2]=0. The host emits `C|1` and sets
`prev_state=D` in ftrace.

**Release hooks** (optional): bit [2]=1. The host emits `C|0` at the exact
release time. Without release hooks, `C|0` is emitted at the next context
switch — still functional but less precise.

**Spurious releases**: `traceQUEUE_SEND` fires on every send, not just when
unblocking a waiter. The host silently ignores release events for objects
that are not currently blocking (`blocking_active==false`).

**Note**: Release hooks are optional and auto-detected per object. Firmware
without release hooks works exactly as before (backward compatible).

### 2. Define the trace variable

In any `.c` file (e.g. `rtos_obj_trace.c`):

```c
#include <stdint.h>

volatile uint32_t rtos_obj_trace __attribute__((used));
```

The `__attribute__((used))` prevents the linker from stripping the symbol
when LTO is enabled.

### 3. Register objects with names

```c
SemaphoreHandle_t myMutex = xSemaphoreCreateMutex();
SemaphoreHandle_t mySem   = xSemaphoreCreateCounting(10, 0);
QueueHandle_t     myQueue = xQueueCreate(4, sizeof(uint32_t));

vQueueAddToRegistry(myMutex, "TestMutex");
vQueueAddToRegistry((QueueHandle_t)mySem, "TestSem");
vQueueAddToRegistry(myQueue, "TestQueue");
```

### 4. OpenOCD configuration

Add the `rtos_dwt2_config` proc to your OpenOCD board config:

```tcl
set DWT_COMP1 0xE0001030
set DWT_MASK1 0xE0001034
set DWT_FUNC1 0xE0001038

proc rtos_dwt2_config {addr} {
    global DWT_COMP1 DWT_MASK1 DWT_FUNC1
    mww $DWT_COMP1 $addr
    mww $DWT_MASK2 0
    mww $DWT_FUNC2 0x0D
    echo "DWT Comparator 1 configured for data write+value at [format 0x%08X $addr]"
}
```

### 5. Run orbtop-rtos

```bash
./build/orbtop-rtos \
    -e firmware.elf \
    -T freertos \
    -w rtos_obj_trace \
    -s localhost:42995 \
    -W 43109 \
    -F 480000000 \
    -p ITM \
    -K trace.ftrace
```

| Option | Description                                     |
|--------|-------------------------------------------------|
| `-e`   | Firmware ELF file (for DWARF + symbols)         |
| `-T`   | RTOS type (`freertos`, `rtx5`, `zephyr`)        |
| `-w`   | Watchpoint variable for object tracking (comp1) |
| `-s`   | orbuculum server (host:port)                    |
| `-W`   | OpenOCD telnet port                             |
| `-F`   | CPU frequency in Hz                             |
| `-p`   | Trace protocol (`ITM` or `ETM`)                 |
| `-K`   | ftrace output file                              |

## How It Works Internally

### Bit stripping and type detection

When a DWT comp1 event arrives, the host:

1. Strips bits [2:0]: `is_release = (value & 4) != 0`, `type_hint = value & 3`,
   `real_addr = value & ~7u`
2. If `real_addr == 0`: delay event → `pending_prev_state = 'S'`, return
3. If `is_release`: look up existing object; if `blocking_active`, emit `C|0`
   and set `has_release_hooks = true`. Spurious releases are silently ignored.
4. If acquire: look up or create object, call `freertos_read_object_info()`,
   emit `C|1`, set `blocking_active = true`

### Type detection (DWARF-based)

At startup, `orbtop-rtos` resolves from the ELF:

1. `Queue_t.ucQueueType` offset via `SymbolGetStructOffset("Queue_t", "ucQueueType")`
2. `xQueueRegistry` address via `arm-none-eabi-objdump -t`
3. `QueueRegistryItem_t.pcQueueName` and `.xHandle` offsets via DWARF
4. Registry entry count = `sizeof(xQueueRegistry)` / `sizeof(QueueRegistryItem_t)`

For Queue_t objects (type_hint=0):

1. **pcHead validation**: Read offset 0 (`pcHead` pointer) — must be non-null
   and word-aligned (exception: `pcHead=NULL` is valid for mutexes).
2. **Type read**: Read `ucQueueType` byte at the DWARF-resolved offset.
3. **Name lookup**: Scan `xQueueRegistry[]` entries comparing `xHandle` to
   the object address. If found, read the name string via `pcQueueName` pointer.

For EventGroup (type_hint=1) and StreamBuffer (type_hint=2), the type comes
from the firmware hint — Queue_t validation is skipped entirely.

### Release auto-detection

The host auto-detects per object whether firmware has release hooks:

- **First blocking cycle**: `has_release_hooks` is false. The `C|0` is
  emitted at the context switch (backward compatible).
- **First release event**: sets `has_release_hooks = true` permanently
  for that object. From then on, `C|0` is emitted at release time, and
  the counter stays high through context switches.

This means the first cycle for each object uses backward-compatible timing.
All subsequent cycles show precise contention (counter high from blocking
to release).

### prev_state mapping

| Value written to `rtos_obj_trace`  | Meaning              | ftrace `prev_state` |
|------------------------------------|----------------------|---------------------|
| Queue_t address (bits[2:0]=000)     | Blocked on queue obj | `D`                 |
| EventGroup_t addr \| 1 (bits=001)  | Blocked on evtflags  | `D`                 |
| StreamBuffer_t addr \| 2 (bits=010) | Blocked on stream    | `D`                 |
| Any addr \| 4 (bit[2]=1)           | Release event        | —                   |
| `0`                                 | Delay / sleep        | `S`                 |

## Limitations

### Event Groups and Stream Buffers have no names

Event Groups (`EventGroup_t`) and Stream Buffers (`StreamBuffer_t`) do not
contain name fields and there is no equivalent of `xQueueRegistry` for them.
They always appear with hex addresses: `evtflags:0x24001234`,
`stream:0x24005678`.

### Memory Pools not available

FreeRTOS does not have native memory pools. The CMSIS-RTOS2 wrapper
implements `osMemoryPoolAlloc` using an internal counting semaphore — this
semaphore will appear as a `sem:0xNNNN` event (without a registry name),
not as a dedicated `mpool:` event.

### Unregistered objects

Queue_t objects not registered with `vQueueAddToRegistry()` appear with
their hex address instead of a name: `sem:0x24006958`.

### Task Notifications not tracked

Task Notifications have no distinct kernel object address — the "object"
is the task's own TCB. They cannot be tracked with the DWT comp1 mechanism.

### Single CPU only

The current implementation only supports single-core FreeRTOS targets.
FreeRTOS v11+ SMP renames `pxCurrentTCB` to `pxCurrentTCBs` (array per
core), which is not yet supported.

## Ftrace Output

All examples below are from a real FreeRTOS capture (`freertos_pcs_260_autotest_trace.ftrace`,
STM32H7, 480 MHz).

### Context switches by state

**Preempted (R)** — thread still runnable, higher-priority thread took CPU:
```
mutexA|task_mutex_a-604019640 [000] ....  1668.583580: sched_switch: prev_comm=mutexA|task_mutex_a prev_pid=604019640 prev_prio=24 prev_state=R ==> next_comm=autotestTask|autotest_task next_pid=604032448 next_prio=25
```

**Sleeping (S)** — thread called `vTaskDelay` / `osDelay`:
```
blinkTask|blink_task-604027912 [000] ....   614.533882: sched_switch: prev_comm=blinkTask|blink_task prev_pid=604027912 prev_prio=24 prev_state=S ==> next_comm=mutexA|task_mutex_a next_pid=604019640 next_prio=24
```

**Blocked on object (D)** — thread waiting on mutex/sem/queue:
```
mutexB|task_mutex_b-604021104 [000] ....   614.510154: sched_switch: prev_comm=mutexB|task_mutex_b prev_pid=604021104 prev_prio=24 prev_state=D ==> next_comm=semWait|task_sem_wait next_pid=604022568 next_prio=24
```

### Object tracking — acquire/release pairs

Each pair shows `C|1` (acquire = thread blocks) and `C|0` (release = object freed).

**mutex** (named via `vQueueAddToRegistry`):
```
        rtos_obj-1 [000] ....   621.705868: tracing_mark_write: C|1|mutex:TestMutex|1
        rtos_obj-1 [000] ....   628.464283: tracing_mark_write: C|1|mutex:TestMutex|0
```

**sem** (named counting semaphore):
```
        rtos_obj-1 [000] ....   614.512741: tracing_mark_write: C|1|sem:TestSem|1
        rtos_obj-1 [000] ....   614.516284: tracing_mark_write: C|1|sem:TestSem|0
```

**sem** (named binary semaphore):
```
        rtos_obj-1 [000] ....   614.518883: tracing_mark_write: C|1|sem:TestBSem|1
        rtos_obj-1 [000] ....   614.522390: tracing_mark_write: C|1|sem:TestBSem|0
```

**sem** (unnamed — no `vQueueAddToRegistry`, shows hex):
```
        rtos_obj-1 [000] ....  1682.033379: tracing_mark_write: C|1|sem:0x24006960|1
        rtos_obj-1 [000] ....  1682.037092: tracing_mark_write: C|1|sem:0x24006960|0
```

**queue** (unnamed):
```
        rtos_obj-1 [000] ....  1677.526053: tracing_mark_write: C|1|queue:0x2400DF08|1
        rtos_obj-1 [000] ....  1677.527760: tracing_mark_write: C|1|queue:0x2400DF08|0
```

**rmutex** (recursive mutex, unnamed):
```
        rtos_obj-1 [000] ....  2177.550138: tracing_mark_write: C|1|rmutex:0x2400CFE8|1
        rtos_obj-1 [000] ....  2177.550138: tracing_mark_write: C|1|rmutex:0x2400CFE8|0
```

### Delay events

When `rtos_obj_trace = 0` (delay), the context switch shows `prev_state=S`
with no object counter event:
```
producer|task_producer-604026960 [000] ....   614.528683: sched_switch: prev_comm=producer|task_producer prev_pid=604026960 prev_prio=24 prev_state=S ==> next_comm=blinkTask|blink_task next_pid=604027912 next_prio=24
```

### ITM stimulus channels

Firmware writes to ITM stimulus ports appear as `C|0|tag|value` counters:
```
           <...>-0 [000] ....  1094.505372: tracing_mark_write: C|0|signal_1|45
           <...>-0 [000] ....  1094.505667: tracing_mark_write: C|0|signal_2|207
           <...>-0 [000] ....  1094.505954: tracing_mark_write: C|0|signal_3|70
```

### Complete sequence — mutex contention

A full blocking cycle: thread blocks, context switch, release, resume:
```
        rtos_obj-1 [000] ....   614.506031: tracing_mark_write: C|1|mutex:TestMutex|1
mutexB|task_mutex_b-604021104 [000] ....   614.510154: sched_switch: prev_comm=mutexB|task_mutex_b prev_pid=604021104 prev_prio=24 prev_state=D ==> next_comm=semWait|task_sem_wait next_pid=604022568 next_prio=24
        rtos_obj-1 [000] ....   614.510154: tracing_mark_write: C|1|mutex:TestMutex|0
```
