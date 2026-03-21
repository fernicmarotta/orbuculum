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
 rtos_obj_trace = Queue_t*  ──DWT──►  _handleDataAccessWP (comp2)
                            comp2        │
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

The DWT comp2 value uses three low bits for metadata. Cortex-M SRAM
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

```c
/* ---- Orbuculum object tracking (DWT comp2) ---- */
extern volatile uint32_t rtos_obj_trace;

#define FREERTOS_RELEASE_BIT 4u   /* bit [2] = release event */

/* === Blocking hooks (acquire) === */

/* Queue-based objects: mutex, semaphore, queue (tag=00) */
#define traceBLOCKING_ON_QUEUE_RECEIVE(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue); } while(0)
#define traceBLOCKING_ON_QUEUE_SEND(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue); } while(0)

/* Event Groups (tag=01) */
#define traceEVENT_GROUP_WAIT_BITS_BLOCK(xEventGroup, uxBitsToWaitFor) \
    do { rtos_obj_trace = (uint32_t)(xEventGroup) | 1u; } while(0)
#define traceEVENT_GROUP_SYNC_BLOCK(xEventGroup, uxBitsToSet, uxBitsToWaitFor) \
    do { rtos_obj_trace = (uint32_t)(xEventGroup) | 1u; } while(0)

/* Stream Buffers / Message Buffers (tag=10) */
#define traceBLOCKING_ON_STREAM_BUFFER_SEND(xStreamBuffer) \
    do { rtos_obj_trace = (uint32_t)(xStreamBuffer) | 2u; } while(0)
#define traceBLOCKING_ON_STREAM_BUFFER_RECEIVE(xStreamBuffer) \
    do { rtos_obj_trace = (uint32_t)(xStreamBuffer) | 2u; } while(0)

/* Delays: write 0 for prev_state=S in ftrace */
#define traceTASK_DELAY()            do { rtos_obj_trace = 0; } while(0)
#define traceTASK_DELAY_UNTIL(x)     do { rtos_obj_trace = 0; } while(0)

/* === Release hooks (optional — enables precise contention timing) === */

/* Queue_t release: send/receive on queue, mutex, semaphore (tag=00) */
#define traceQUEUE_SEND(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue) | FREERTOS_RELEASE_BIT; } while(0)
#define traceQUEUE_SEND_FROM_ISR(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue) | FREERTOS_RELEASE_BIT; } while(0)
#define traceQUEUE_RECEIVE(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue) | FREERTOS_RELEASE_BIT; } while(0)
#define traceQUEUE_RECEIVE_FROM_ISR(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue) | FREERTOS_RELEASE_BIT; } while(0)
#define traceGIVE_MUTEX_RECURSIVE(pxMutex) \
    do { rtos_obj_trace = (uint32_t)(pxMutex) | FREERTOS_RELEASE_BIT; } while(0)

/* Event Group release (tag=01) */
#define traceEVENT_GROUP_SET_BITS(xEventGroup, uxBitsToSet) \
    do { rtos_obj_trace = (uint32_t)(xEventGroup) | FREERTOS_RELEASE_BIT | 1u; } while(0)

/* Stream Buffer release (tag=10) */
#define traceSTREAM_BUFFER_SEND(xStreamBuffer, xBytesSent) \
    do { rtos_obj_trace = (uint32_t)(xStreamBuffer) | FREERTOS_RELEASE_BIT | 2u; } while(0)
#define traceSTREAM_BUFFER_RECEIVE(xStreamBuffer, xReceivedLength) \
    do { rtos_obj_trace = (uint32_t)(xStreamBuffer) | FREERTOS_RELEASE_BIT | 2u; } while(0)
```

These macros are called by FreeRTOS internally. The blocking hooks fire when
a task is about to block; the release hooks fire when an operation completes
(send, receive, give, set bits). Writing the object address (with type and
release bits) to `rtos_obj_trace` triggers DWT comparator 2.

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
| `-w`   | Watchpoint variable for object tracking (comp2) |
| `-s`   | orbuculum server (host:port)                    |
| `-W`   | OpenOCD telnet port                             |
| `-F`   | CPU frequency in Hz                             |
| `-p`   | Trace protocol (`ITM` or `ETM`)                 |
| `-K`   | ftrace output file                              |

## How It Works Internally

### Bit stripping and type detection

When a DWT comp2 event arrives, the host:

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
is the task's own TCB. They cannot be tracked with the DWT comp2 mechanism.

### Single CPU only

The current implementation only supports single-core FreeRTOS targets.
FreeRTOS v11+ SMP renames `pxCurrentTCB` to `pxCurrentTCBs` (array per
core), which is not yet supported.

## Ftrace Output

Thread switches with blocking show `prev_state=D`:
```
semWait|task_sem_wait-604022432 [000] .... 614.394: sched_switch: ... prev_state=D ==> next_comm=producer ...
```

Object events appear as counter tracks:
```
rtos_obj-1 [000] .... 614.391: tracing_mark_write: C|1|sem:TestSem|1
rtos_obj-1 [000] .... 614.394: tracing_mark_write: C|1|sem:TestSem|0
```

With release hooks enabled, the `C|0` appears at the actual release time
(not at the context switch), showing precise contention duration:
```
rtos_obj-1 [000] .... 614.391: tracing_mark_write: C|1|mutex:TestMutex|1
rtos_obj-1 [000] .... 614.450: tracing_mark_write: C|1|mutex:TestMutex|0
```

In Perfetto, these render as named counter tracks showing when each object
causes blocking, correlated with the thread timeline.
