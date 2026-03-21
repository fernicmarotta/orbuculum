# RTX5 Object Tracking

## Overview

`orbtop-rtos` can track RTOS kernel object blocking events (mutex, semaphore,
event flags, message queue, memory pool) on ARM RTX5 targets. When a thread
blocks on an object, the event is captured via a DWT watchpoint and displayed
in ftrace output with `prev_state=D` (`sched_switch`) and a named counter track (`tracing_mark_write`).

## Architecture

```
 Firmware (Cortex-M)                    Host (orbuculum)
 ──────────────────                     ────────────────
 EvrRtx*Pending()                       ITM decoder
   │                                      │
   ▼                                      ▼
 rtos_obj_trace = cb_addr  ──DWT──►  _handleDataAccessWP (comp2)
                           comp2        │
                           ITM pkt      ▼
                                     rtosHandleObjectEvent()
                                        │
                                        ▼
                                     find_or_create_object()
                                        │
                                        ▼
                                     rtx5_read_object_info()
                                        │  reads ID byte + name
                                        │  via OpenOCD telnet
                                        ▼
                                     ftrace output:
                                       prev_state=D
                                       C|1|mutex:MyMutex|1
                                       C|1|mutex:MyMutex|0
```

## Supported Object Types

RTX5 uses a common control block header with an ID byte at offset 0:

| ID byte | Object Type    | Prefix     | Example output       |
|---------|---------------|------------|----------------------|
| 0xF5    | Mutex          | `mutex`    | `mutex:MyMutex`      |
| 0xF6    | Semaphore      | `sem`      | `sem:MySemaphore`    |
| 0xF3    | Event Flags    | `evtflags` | `evtflags:MyFlags`   |
| 0xFA    | Message Queue  | `msgqueue` | `msgqueue:CmdQueue`  |
| 0xF7    | Memory Pool    | `mempool`  | `mempool:BlockPool`  |
| —       | Delay          | —          | `prev_state=S`       |

Object names are read from the RTX5 control block name pointer (offset 4).

## Firmware Setup

### Prerequisites: RTX5 Event Recorder macros

The `EvrRtx*Pending` functions are defined as `__WEAK` in `rtx_evr.c` (part of
the RTX5 kernel source). They are compiled **only** when the corresponding
`OS_EVR_*` macros are enabled in `RTX_Config.h`. The following must be set:

| Macro in `RTX_Config.h` | Enables callbacks for |
|---|---|
| `OS_EVR_MUTEX = 1` | `EvrRtxMutexAcquirePending` |
| `OS_EVR_SEMAPHORE = 1` | `EvrRtxSemaphoreAcquirePending` |
| `OS_EVR_EVFLAGS = 1` | `EvrRtxEventFlagsWaitPending` |
| `OS_EVR_MSGQUEUE = 1` | `EvrRtxMessageQueueGetPending`, `*PutPending`, `*InsertPending` |
| `OS_EVR_MEMPOOL = 1` | `EvrRtxMemoryPoolAllocPending` |
| `OS_EVR_WAIT = 1` | `EvrRtxDelay`, `EvrRtxDelayUntil` |

Additionally, `EVR_RTX_DISABLE` must **not** be defined (it disables all
event callbacks globally).

These macros are typically enabled by default in `RTX_Config.h`. The actual
`RTE_Compiler_EventRecorder` component is **not** required — the weak
functions exist regardless, they just become empty stubs without it. Our
override replaces them with a single write to `rtos_obj_trace`.

### 1. Create the trace variable

Create a source file (e.g. `rtos_obj_trace.c`) and add it to your build:

```c
#include <stdint.h>

volatile uint32_t rtos_obj_trace;

void EvrRtxMutexAcquirePending(void *mutex_id, uint32_t timeout)
{
    (void)timeout;
    rtos_obj_trace = (uint32_t)mutex_id;
}

void EvrRtxSemaphoreAcquirePending(void *semaphore_id, uint32_t timeout)
{
    (void)timeout;
    rtos_obj_trace = (uint32_t)semaphore_id;
}

void EvrRtxEventFlagsWaitPending(void *ef_id, uint32_t flags, uint32_t options, uint32_t timeout)
{
    (void)flags;
    (void)options;
    (void)timeout;
    rtos_obj_trace = (uint32_t)ef_id;
}

void EvrRtxMessageQueueGetPending(void *mq_id, void *msg_ptr, uint32_t timeout)
{
    (void)msg_ptr;
    (void)timeout;
    rtos_obj_trace = (uint32_t)mq_id;
}

void EvrRtxMessageQueuePutPending(void *mq_id, const void *msg_ptr, uint32_t timeout)
{
    (void)msg_ptr;
    (void)timeout;
    rtos_obj_trace = (uint32_t)mq_id;
}

void EvrRtxMessageQueueInsertPending(void *mq_id, const void *msg_ptr)
{
    (void)msg_ptr;
    rtos_obj_trace = (uint32_t)mq_id;
}

void EvrRtxMemoryPoolAllocPending(void *mp_id, uint32_t timeout)
{
    (void)timeout;
    rtos_obj_trace = (uint32_t)mp_id;
}

void EvrRtxDelay(uint32_t ticks)
{
    (void)ticks;
    rtos_obj_trace = 0;
}

void EvrRtxDelayUntil(uint32_t ticks)
{
    (void)ticks;
    rtos_obj_trace = 0;
}
```

These functions are `__WEAK`-linked RTX5 event recorder callbacks defined in
`rtx_evr.c`. The RTX kernel calls them automatically when a thread is about
to block. Our implementations override the weak stubs with a single write
to `rtos_obj_trace`, which triggers DWT comparator 2.

Writing `0` (in `EvrRtxDelay`/`EvrRtxDelayUntil`) signals a voluntary delay,
which produces `prev_state=S` in ftrace output.


### 2. OpenOCD configuration

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

### 3. Run orbtop-rtos

```bash
orbtop-rtos \
    -e firmware.elf \
    -F 480000000 \
    -T rtx5 \
    -s localhost:46000 \
    -p ITM \
    -W 4444 \
    -w rtos_obj_trace \
    -K trace.ftrace
```

The `-w rtos_obj_trace` option tells orbtop-rtos to look up the symbol address
in the ELF and configure DWT comparator 2 via `rtos_dwt2_config`.

## Ftrace Output

Thread switches with blocking show `prev_state=D`:
```
threadA-12345 [000] .... 1.000: sched_switch: ... prev_state=D ==> next_comm=threadB ...
```

Object events appear as counter tracks:
```
rtos_obj-1 [000] .... 1.000: tracing_mark_write: C|1|mutex:MyMutex|1
rtos_obj-1 [000] .... 1.050: tracing_mark_write: C|1|mutex:MyMutex|0
```

In Perfetto, these render as named counter tracks showing when each object
causes blocking, correlated with the thread timeline.
