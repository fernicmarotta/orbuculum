# RTX5 Object Tracking

## Overview

`orbtop-rtos` can track RTOS kernel object blocking and release events
(mutex, semaphore, event flags, message queue, memory pool) on ARM RTX5
targets. When a thread blocks on an object, the event is captured via a
DWT watchpoint and displayed in ftrace output with `prev_state=D`
(`sched_switch`) and a named counter track (`tracing_mark_write`).

Optional release hooks provide precise contention timing: the counter track
stays high from the blocking event until the object is actually released,
allowing you to distinguish contention time from scheduling latency in
Perfetto.

## Architecture

```
 Firmware (Cortex-M)                    Host (orbuculum)
 ──────────────────                     ────────────────
 EvrRtx*Pending()                       ITM decoder
   │                                      │
   ▼                                      ▼
 rtos_obj_trace = cb_addr  ──DWT──►  _handleDataAccessWP (comp1)
                           comp1        │
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

## Type Encoding (bits [2:0])

The DWT comp1 value uses bit [2] for acquire/release signaling. RTX5
control blocks have an `id` byte at offset 0 that the host reads via
telnet — no firmware type hint in bits [1:0] is needed (always 0).

```
[31:3] = object address
[2]    = 0: acquire (blocking), 1: release
[1:0]  = 00 (unused — type from control block id byte)
```

The host strips bits [2:0] (`& ~7u`) to recover the real address.

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
| `OS_EVR_MUTEX = 1` | `EvrRtxMutexAcquirePending`, `EvrRtxMutexReleased` |
| `OS_EVR_SEMAPHORE = 1` | `EvrRtxSemaphoreAcquirePending`, `EvrRtxSemaphoreReleased` |
| `OS_EVR_EVFLAGS = 1` | `EvrRtxEventFlagsWaitPending`, `EvrRtxEventFlagsSet` |
| `OS_EVR_MSGQUEUE = 1` | `EvrRtxMessageQueue*Pending`, `*Inserted`, `*Retrieved` |
| `OS_EVR_MEMPOOL = 1` | `EvrRtxMemoryPoolAllocPending`, `*Deallocated` |
| `OS_EVR_WAIT = 1` | `EvrRtxDelay`, `EvrRtxDelayUntil` |

Additionally, `EVR_RTX_DISABLE` must **not** be defined (it disables all
event callbacks globally).

These macros are typically enabled by default in `RTX_Config.h`. The actual
`RTE_Compiler_EventRecorder` component is **not** required — the weak
functions exist regardless, they just become empty stubs without it. Our
override replaces them with a single write to `rtos_obj_trace`.

### 1. Add the trace file to your project

Copy [`samples/rtos_obj_trace.c`](samples/rtos_obj_trace.c) into your firmware
project and add it to your build.  It defines the `rtos_obj_trace` variable
and overrides all `__WEAK` EVR stubs for blocking and release hooks.

These functions are `__WEAK`-linked RTX5 event recorder callbacks defined in
`rtx_evr.c`. The RTX kernel calls them automatically when a thread is about
to block (acquire) or when an object operation completes (release). Our
implementations override the weak stubs with a single write to
`rtos_obj_trace`, which triggers DWT comparator 1.

**Blocking hooks** (acquire): bit [2]=0. The host emits `C|1` and sets
`prev_state=D` in ftrace.

**Release hooks** (optional): bit [2]=1. The host emits `C|0` at the exact
release time. Without release hooks, `C|0` is emitted at the next context
switch — still functional but less precise.

Writing `0` (in `EvrRtxDelay`/`EvrRtxDelayUntil`) signals a voluntary delay,
which produces `prev_state=S` in ftrace output.

**Note**: Release hooks are optional and auto-detected per object. Firmware
without release hooks works exactly as before (backward compatible). All
`os*Id_t` types (e.g. `osMutexId_t`) are `void*` in CMSIS-RTOS2.


### 2. OpenOCD configuration

Add the `rtos_dwt2_config` proc to your OpenOCD board config:

```tcl
set DWT_COMP1 0xE0001030
set DWT_MASK1 0xE0001034
set DWT_FUNC1 0xE0001038

proc rtos_dwt2_config {addr} {
    global DWT_COMP1 DWT_MASK1 DWT_FUNC1
    mww $DWT_COMP1 $addr
    mww $DWT_MASK1 0
    mww $DWT_FUNC1 0x0D
    echo "DWT Comparator 1 configured for data write+value at [format 0x%08X $addr]"
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
in the ELF and configure DWT comparator 1 via `rtos_dwt2_config`.

## How It Works Internally

For the generic object tracking mechanism (bit encoding, release auto-detection),
see [Object Tracking Internals](../orbtop-rtos.md#object-tracking-internals).

## Ftrace Output

See [Ftrace Output Reference](../ftrace/ftrace-output.md) for the full format
specification. RTX5 object tracking uses the same ftrace events as FreeRTOS
and Zephyr (`sched_switch` with `prev_state=D` and `tracing_mark_write C|1|...`
counter tracks). Object type prefixes are determined by the control block `id`
byte read from target memory (see [Object Type Prefixes](#object-type-prefixes)
above).
