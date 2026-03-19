# FreeRTOS Object Tracking

## Overview

`orbtop-rtos` can track FreeRTOS kernel object blocking events (mutex,
semaphore, queue) on ARM Cortex-M targets. When a task blocks on a
queue-based object, the event is captured via a DWT watchpoint and displayed
in ftrace/Perfetto output with `prev_state=D` and a named counter track.

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

## Supported Object Types

FreeRTOS uses `Queue_t` internally for all synchronization primitives.
The `ucQueueType` field identifies the object type:

| ucQueueType | Object Type        | Prefix   | Example output          |
|-------------|-------------------|----------|-------------------------|
| 0           | Queue / Queue Set  | `queue`  | `queue:CmdQueue`        |
| 1           | Mutex              | `mutex`  | `mutex:TestMutex`       |
| 2           | Counting Semaphore | `sem`    | `sem:TestSem`           |
| 3           | Binary Semaphore   | `sem`    | `sem:TestBSem`          |
| 4           | Recursive Mutex    | `rmutex` | `rmutex:RecMutex`       |

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

/* Queue-based objects: mutex, semaphore, queue */
#define traceBLOCKING_ON_QUEUE_RECEIVE(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue); } while(0)
#define traceBLOCKING_ON_QUEUE_SEND(pxQueue) \
    do { rtos_obj_trace = (uint32_t)(pxQueue); } while(0)
```

These macros are called by FreeRTOS internally when a task is about to block
on `xQueueReceive`, `xSemaphoreTake`, `xQueueSend`, etc. Writing the
`Queue_t` address to `rtos_obj_trace` triggers DWT comparator 2.

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
orbtop-rtos -e firmware.elf -T <telnet_port> -w rtos_obj_trace -f trace.ftrace
```

## How It Works Internally

### Type detection (DWARF-based, no hardcoded offsets)

At startup, `orbtop-rtos` resolves from the ELF:

1. `Queue_t.ucQueueType` offset via `SymbolGetStructOffset("Queue_t", "ucQueueType")`
2. `xQueueRegistry` address via `arm-none-eabi-objdump -t`
3. `QueueRegistryItem_t.pcQueueName` and `.xHandle` offsets via DWARF
4. Registry entry count = `sizeof(xQueueRegistry)` / `sizeof(QueueRegistryItem_t)`

When a DWT comp2 event arrives with a `Queue_t` address:

1. **pcHead validation**: Read offset 0 (`pcHead` pointer) — must be non-null
   and word-aligned. This rejects invalid addresses with a single read.
2. **Type read**: Read `ucQueueType` byte at the DWARF-resolved offset.
3. **Name lookup**: Scan `xQueueRegistry[]` entries comparing `xHandle` to
   the object address. If found, read the name string via `pcQueueName` pointer.

### prev_state mapping

| Value written to `rtos_obj_trace` | Meaning           | ftrace `prev_state` |
|----------------------------------|-------------------|---------------------|
| Queue_t address (word-aligned)    | Blocked on object | `D`                 |
| `0`                               | Delay / sleep     | `S`                 |
| `0xFFFFFFFF`                      | Invalid (ignored) | —                   |

## Limitations

### Event Groups not supported (yet)

FreeRTOS Event Groups (`xEventGroupWaitBits`) use `EventGroup_t`, which is a
different structure from `Queue_t`. The `traceBLOCKING_ON_QUEUE_*` hooks do
not fire for event group operations.

Available hooks: `traceEVENT_GROUP_WAIT_BITS_BLOCK`, `traceEVENT_GROUP_SYNC_BLOCK`.
These require a different mechanism — see the source code for the bit-flag
encoding approach planned for future support.

### Task delays not tracked by default

`vTaskDelay()` and `vTaskDelayUntil()` do not write to `rtos_obj_trace` by
default. To get `prev_state=S` for delays, add:

```c
#define traceTASK_DELAY()            do { rtos_obj_trace = 0; } while(0)
#define traceTASK_DELAY_UNTIL(x)     do { rtos_obj_trace = 0; } while(0)
```

### Unregistered objects

Objects not registered with `vQueueAddToRegistry()` appear with their hex
address instead of a name: `sem:0x24006958`.

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

In Perfetto, these render as named counter tracks showing when each object
causes blocking, correlated with the thread timeline.
