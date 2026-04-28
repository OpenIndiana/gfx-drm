# Deliverables: dma_fence Shim Layer + Upstream Sync Tooling

**Date:** 2026-03-31
**Repository:** `/home/toasty/ws/illumos/gfx-drm/`
**Status:** Code complete, not yet build-tested on illumos host

---

## Overview

This deliverable adds GPU synchronization primitives (`dma_fence`, `ww_mutex`,
`dma_resv`) to gfx-drm as illumos-native shim files, plus tooling to repeatedly
sync upstream Linux DRM sources into the repository.

These primitives are required by all modern Linux DRM code (post-3.17) and were
completely absent from illumos. They are implemented using native illumos kernel
primitives (`kmutex_t`, `kcondvar_t`, `atomic_*`, `list_t`, `taskq_t`) following
the established `drm_sun_*` shim pattern.

**Total new code:** 2,144 lines across 10 files (9 shim + 1 script)
**Modified files:** 4 (Makefile.mod, drmP.h, drm_linux.h, top-level Makefile)

---

## 1. New Files

### 1.1 `drm_sun_ref.h` -- Atomic Reference Counting

| | |
|-|-|
| **Path** | `usr/src/uts/common/drm/drm_sun_ref.h` |
| **Lines** | 93 |
| **Type** | Header-only (all inline) |
| **Replaces** | Linux `kref` |
| **Dependencies** | `<sys/atomic.h>`, `<sys/debug.h>` |

Provides `drm_ref_t` with four operations:

| Function | Description |
|----------|-------------|
| `drm_ref_init(ref)` | Set refcount to 1 |
| `drm_ref_get(ref)` | Atomic increment |
| `drm_ref_put(ref) → boolean_t` | Atomic decrement; returns `B_TRUE` if dropped to 0 |
| `drm_ref_get_unless_zero(ref) → boolean_t` | CAS-loop try-get for dying objects |

All operations are lock-free and safe from any context including interrupt handlers.

---

### 1.2 `drm_sun_ww_mutex` -- Wait-Die Deadlock Avoidance Mutex

| | |
|-|-|
| **Header** | `usr/src/uts/common/drm/drm_sun_ww_mutex.h` (105 lines) |
| **Source** | `usr/src/uts/common/io/drm/drm_sun_ww_mutex.c` (234 lines) |
| **Replaces** | Linux `ww_mutex` / `<linux/ww_mutex.h>` |
| **Dependencies** | `kmutex_t`, `kcondvar_t`, `atomic_inc_64_nv()` |

Implements the **Wait-Die** algorithm used by `dma_resv` for deadlock-free
multi-buffer locking:

- Each transaction gets a monotonically increasing **stamp** (lower = older = higher priority)
- When transaction A tries to lock a mutex held by B:
  - A older than B → A **waits**
  - A younger than B → A **dies** (returns `-EDEADLK`)
- After `-EDEADLK`, caller releases all locks and retries via the slow path

| Function | Returns |
|----------|---------|
| `ww_mutex_init(wm, cls)` | void |
| `ww_mutex_destroy(wm)` | void |
| `ww_mutex_lock(wm, ctx)` | `0`, `-EALREADY`, `-EDEADLK` |
| `ww_mutex_lock_interruptible(wm, ctx)` | also `-EINTR` |
| `ww_mutex_lock_slow(wm, ctx)` | `0` (always blocks, never `-EDEADLK`) |
| `ww_mutex_lock_slow_interruptible(wm, ctx)` | `0`, `-EINTR` |
| `ww_mutex_trylock(wm)` | `B_TRUE` / `B_FALSE` |
| `ww_mutex_unlock(wm)` | void |

**Internals:** A `kmutex_t` protects the logical state (`wm_locked`, `wm_ctx`).
A `kcondvar_t` serializes waiters. The internal mutex is held only during the
brief lock/unlock protocol, not for the duration of the logical lock hold.

Never called from interrupt context.

---

### 1.3 `drm_sun_dma_fence` -- GPU Synchronization Primitive

| | |
|-|-|
| **Header** | `usr/src/uts/common/drm/drm_sun_dma_fence.h` (220 lines) |
| **Source** | `usr/src/uts/common/io/drm/drm_sun_dma_fence.c` (416 lines) |
| **Replaces** | Linux `dma_fence` / `<linux/dma-fence.h>` |
| **Dependencies** | `drm_sun_ref.h`, `kmutex_t`, `kcondvar_t`, `list_t`, `gethrtime()` |

A **one-shot signalable future**. A producer creates a fence, hands it to
consumers, and later signals it (typically from a GPU interrupt handler).
Once signaled, a fence stays signaled forever.

#### Structure: `dma_fence_t`

```
fence_lock       → kmutex_t *    (pointer to shared context lock)
fence_ops        → ops vtable    (driver callbacks)
fence_ref        → drm_ref_t     (atomic refcount)
fence_flags      → uint32_t      (SIGNALED, TIMESTAMP, ENABLE_SIGNAL)
fence_error      → int           (0 or negative errno)
fence_context    → uint64_t      (globally unique context ID)
fence_seqno      → uint64_t      (monotonic within context)
fence_timestamp  → hrtime_t      (set on signal)
fence_cb_list    → list_t        (callback chain)
fence_wait_lock  → kmutex_t      (per-fence, for condvar)
fence_wait_cv    → kcondvar_t    (blocked waiters sleep here)
```

#### Two-Lock Design

| Lock | Scope | Purpose |
|------|-------|---------|
| `fence_lock` | Shared across fences in same context (pointer) | Protects callback list, used during signal |
| `fence_wait_lock` | Per-fence | Associated with `fence_wait_cv` for blocking waits |

The two-lock design is necessary because `cv_wait()` requires a stable lock
association, but `fence_lock` is a pointer to a shared lock that may be used
by many fences.

#### Exported Functions

| Function | Context | Description |
|----------|---------|-------------|
| `dma_fence_context_alloc(num)` | Any | Allocate globally unique context IDs |
| `dma_fence_init(fence, ops, lock, ctx, seqno)` | Process | Initialize a fence (refcount=1, unsignaled) |
| `dma_fence_signal(fence)` | **IRQ-safe** | Signal the fence; dispatches callbacks, wakes waiters |
| `dma_fence_signal_locked(fence)` | IRQ-safe | Signal with `fence_lock` already held |
| `dma_fence_add_callback(fence, cb, func)` | Any | Register async callback; returns `-ENOENT` if already signaled |
| `dma_fence_remove_callback(fence, cb)` | Any | Unregister callback |
| `dma_fence_wait_timeout(fence, intr, ns)` | Process | Block until signaled/timeout/interrupted |
| `dma_fence_enable_sw_signaling(fence)` | Any | Ensure driver has armed its interrupt |
| `dma_fence_release(fence)` | Any | Called on last ref drop; invokes `ops->release` |
| `dma_fence_free(fence)` | Any | Default release for `sizeof(dma_fence_t)` allocations |

#### Inline Functions (in header)

| Function | Description |
|----------|-------------|
| `dma_fence_get(fence)` | Increment refcount |
| `dma_fence_put(fence)` | Decrement; calls `release` if last |
| `dma_fence_get_unless_zero(fence)` | Try-get for dying fences |
| `dma_fence_is_signaled(fence)` | Poll; may call `ops->signaled()` |
| `dma_fence_wait(fence, intr)` | Convenience: wait forever |
| `dma_fence_is_later(f1, f2)` | Seqno ordering within same context |
| `dma_fence_set_error(fence, err)` | Set error before signaling |

#### Ops Table: `dma_fence_ops_t`

| Callback | Required | Called Under | Contract |
|----------|----------|-------------|----------|
| `get_driver_name` | Yes | Any | Return static string |
| `get_timeline_name` | Yes | Any | Return static string |
| `enable_signaling` | No | `fence_lock` | Arm HW interrupt; return `B_TRUE` on success |
| `signaled` | No | No lock | Fast poll; may set `fence_error` |
| `release` | No | Any (incl. IRQ) | Custom free; if NULL → `dma_fence_free()` |
| `set_deadline` | No | No lock | Hint for power management |

#### Signal Path (Hot Path)

Called from GPU interrupt handler at PIL ≤ 10:

1. `atomic_cas_32(&fence_flags, old, old | SIGNALED)` -- exactly one caller wins
2. `mutex_enter(fence->fence_lock)` -- adaptive mutex, fast when uncontended
3. Record `gethrtime()` timestamp
4. Move callback list to local variable (`list_move_tail`)
5. Dispatch all callbacks inline (must be O(1), non-blocking)
6. `mutex_exit(fence->fence_lock)`
7. `mutex_enter(&fence->fence_wait_lock)` → `cv_broadcast()` → `mutex_exit`

Total: 2 mutex enter/exit pairs per signal. Both are adaptive (no spinning
unless contended with a running thread on another CPU).

#### Callback Contract

Callbacks execute under `fence_lock` (step 5 above). They **must not**:
- Sleep or block
- Allocate with `KM_SLEEP`
- Acquire sleeping locks

If real work is needed, dispatch to a taskq from the callback.

---

### 1.4 `drm_sun_dma_fence_array` -- Wait-All Combinator

| | |
|-|-|
| **Header** | `usr/src/uts/common/drm/drm_sun_dma_fence_array.h` (75 lines) |
| **Source** | `usr/src/uts/common/io/drm/drm_sun_dma_fence_array.c` (244 lines) |
| **Replaces** | Linux `dma_fence_array` / `<linux/dma-fence-array.h>` |
| **Dependencies** | `drm_sun_dma_fence.h`, `system_taskq`, `taskq_dispatch_ent()` |

Aggregates N fences into one composite fence. Signals when:
- **All** children signal (default), or
- **Any** child signals (`signal_on_any = B_TRUE`)

#### API

| Function | Description |
|----------|-------------|
| `dma_fence_array_create(num, fences, ctx, seqno, signal_on_any)` | Create array; takes ownership of fence array |
| `to_dma_fence_array(fence)` | Type-safe downcast; returns NULL if not an array |

#### Internal Mechanism

1. `enable_signaling` registers a callback on each child fence
2. Each callback decrements an atomic `dfa_num_pending` counter
3. When counter reaches 0, **defers** the parent signal via `taskq_dispatch_ent(system_taskq, ..., TQ_NOSLEEP, &preallocated_entry)`
4. The taskq callback calls `dma_fence_signal(&array->dfa_base)` and drops the reference

**Why deferred?** The child callback runs under the child's `fence_lock`. Calling
`dma_fence_signal` on the parent would try to acquire the parent's `fence_lock`.
If child and parent share the same lock, or if the parent's callbacks need to
acquire locks held by the child, this would deadlock. The `system_taskq` dispatch
is IRQ-safe (preallocated `taskq_ent_t`, `TQ_NOSLEEP` flag -- no allocation).

#### Error Propagation

The first child error is propagated to the parent via a non-atomic write
(races are benign -- any error is better than no error).

---

### 1.5 `drm_sun_dma_resv` -- Reservation Object

| | |
|-|-|
| **Header** | `usr/src/uts/common/drm/drm_sun_dma_resv.h` (151 lines) |
| **Source** | `usr/src/uts/common/io/drm/drm_sun_dma_resv.c` (340 lines) |
| **Replaces** | Linux `dma_resv` / `<linux/dma-resv.h>` |
| **Dependencies** | `drm_sun_dma_fence.h`, `drm_sun_ww_mutex.h` |

Associates a dynamically-sized set of fences with a buffer object,
protected by a `ww_mutex_t` for deadlock-safe multi-buffer locking.

#### Usage Levels

| Level | Value | Meaning |
|-------|-------|---------|
| `DMA_RESV_USAGE_KERNEL` | 0 | In-kernel memory management (highest priority) |
| `DMA_RESV_USAGE_WRITE` | 1 | Implicit write synchronization |
| `DMA_RESV_USAGE_READ` | 2 | Implicit read synchronization |
| `DMA_RESV_USAGE_BOOKKEEP` | 3 | No implicit sync (lowest priority) |

Querying at level N returns all fences at levels ≤ N.

#### API

| Function | Description |
|----------|-------------|
| `dma_resv_init(resv)` | Initialize (creates ww_mutex) |
| `dma_resv_fini(resv)` | Destroy (drops all fence refs, frees list) |
| `dma_resv_lock(resv, ctx)` | Lock (ww_mutex wrapper) |
| `dma_resv_lock_interruptible(resv, ctx)` | Lock (interruptible) |
| `dma_resv_lock_slow(resv, ctx)` | Lock slow path (after EDEADLK) |
| `dma_resv_trylock(resv)` | Try-lock |
| `dma_resv_unlock(resv)` | Unlock |
| `dma_resv_reserve_fences(resv, num)` | Pre-allocate slots (KM_SLEEP) |
| `dma_resv_add_fence(resv, fence, usage)` | Add fence (cannot fail, takes ref) |
| `dma_resv_wait_timeout(resv, usage, intr, ns)` | Wait for matching fences |
| `dma_resv_test_signaled(resv, usage)` | Poll all matching fences |
| `dma_resv_for_each_fence(iter, resv, usage, fence)` | Iterate (locked) |

#### Two-Phase Add Protocol

```
dma_resv_lock(resv, &ctx);
dma_resv_reserve_fences(resv, 1);   /* CAN allocate, CAN fail */
dma_resv_add_fence(resv, fence, WRITE);  /* CANNOT fail */
dma_resv_unlock(resv);
```

`reserve_fences` is the only function that allocates memory. It garbage-collects
signaled fences when growing the list. `add_fence` replaces existing fences from
the same context with same-or-higher usage and later-or-equal seqno.

#### Wait Strategy (No RCU)

Linux uses RCU for lockless fence iteration. illumos has no RCU. Our approach:

1. Take `dma_resv_lock`
2. Snapshot matching fences into a local array, taking references
3. Drop `dma_resv_lock`
4. Wait on each fence outside the lock
5. Drop all references

This avoids holding the ww_mutex while sleeping. The lock hold time is
proportional to the number of fences (typically < 10), which is fast.

---

### 1.6 `tools/upstream-sync.sh` -- Linux DRM Source Sync Script

| | |
|-|-|
| **Path** | `tools/upstream-sync.sh` |
| **Lines** | 266 |
| **Usage** | `make upstream-sync LINUX_SRC=/path/to/linux` |

Copies DRM source and header files from a Linux kernel tree into gfx-drm
while **never overwriting** illumos-specific shim files.

#### What It Does

1. **Validates** the Linux tree (`drivers/gpu/drm/Makefile` must exist)
2. **Records** the sync point to `LINUX_UPSTREAM_SYNC`:
   ```
   LINUX_COMMIT=<full sha>
   LINUX_VERSION=<tag>
   SYNC_DATE=<ISO 8601>
   ```
3. **Copies DRM core .c files** from `drivers/gpu/drm/*.c` to `usr/src/uts/common/io/drm/`
4. **Copies DRM headers** from `include/drm/*.h` to `usr/src/uts/common/drm/`
5. **Copies Linux reference files** (dma-buf sources, kernel-internal headers) to `usr/src/uts/common/io/drm/linux-ref/`
6. **Prints summary** with counts and next-step instructions

#### Protected Files (Never Overwritten)

**Source files:**
`drm_sun_*.c`, `drm_sunmod.c`, `drm_io32.c`, `drm_kstat.c`, `drm_linux.c`, `drm_msg.c`, `drm_dp_i2c_helper.c`

**Header files:**
`drm_sun_*.h`, `drm_os_solaris.h`, `drm_linux.h`, `drm_linux_list.h`, `drmP.h`, `drm_io32.h`, `drm_sunmod.h`

#### Excluded Files (Linux-Only, No illumos Equivalent)

`drm_debugfs*.c`, `drm_sysfs.c`, `drm_panic*.c`, `drm_of.c`, `drm_fbdev_*.c`, `drm_mipi_*.c`, `drm_privacy_screen*.c`, `drm_client_sysrq.c`

---

## 2. Modified Files

### 2.1 `usr/src/uts/common/io/drm/Makefile.mod`

Added 4 object files to `DRM_OBJS` (alphabetical within the `drm_sun_*` group):

```
drm_sun_dma_fence.o
drm_sun_dma_fence_array.o
drm_sun_dma_resv.o
drm_sun_ww_mutex.o
```

### 2.2 `usr/src/uts/common/drm/drmP.h`

Added 5 includes after the existing shim includes (line 70):

```c
#include "drm_sun_ref.h"
#include "drm_sun_ww_mutex.h"
#include "drm_sun_dma_fence.h"
#include "drm_sun_dma_fence_array.h"
#include "drm_sun_dma_resv.h"
```

### 2.3 `usr/src/uts/common/drm/drm_linux.h`

Added memory barrier macros (end of file, before `#endif`):

```c
#define smp_mb()   membar_producer(); membar_consumer()
#define smp_rmb()  membar_consumer()
#define smp_wmb()  membar_producer()
```

### 2.4 `Makefile` (top-level)

Added `upstream-sync` target:

```make
upstream-sync: FRC
	@/usr/bin/ksh93 tools/upstream-sync.sh "$(LINUX_SRC)"
```

---

## 3. Design Decisions

### 3.1 No RCU

Linux's `dma_resv` uses RCU for lockless read-side iteration. illumos has no
RCU. All fence list access requires holding the `ww_mutex` lock. For waiting,
we snapshot under the lock and wait outside it.

**Trade-off:** Readers block writers during the brief snapshot. This is acceptable
because fence lists are small (typically < 10 entries) and the lock hold is O(n)
where n is tiny. Can be revisited with an epoch-based reclamation scheme if
profiling shows contention.

### 3.2 No Union Trick

Linux overlays `cb_list` / `timestamp` / `rcu_head` in a union to save 16 bytes
per fence. We keep separate fields. Memory is cheap; debugging union state
corruption across three lifecycle phases is not.

### 3.3 Two-Lock Fence Design

`fence_lock` is a pointer to an external shared lock (same lock for all fences
in a GPU context). `fence_wait_lock` is per-fence for the condvar.

This is necessary because `cv_wait()` requires a stable lock association. If we
used the shared `fence_lock` for the condvar, all fences in a context would share
a single condvar, causing thundering herd on every signal. The per-fence condvar
ensures only waiters on the signaled fence are woken.

### 3.4 Deferred Signal in fence_array

When the last child fence signals, the callback runs under the child's
`fence_lock`. Calling `dma_fence_signal` on the parent would acquire the parent's
lock -- potential deadlock if locks are shared or ordered. We defer to
`system_taskq` via a preallocated `taskq_ent_t` (no allocation, IRQ-safe).

### 3.5 Error Codes

Linux DRM uses negative error returns (`-EINVAL`, `-EDEADLK`). illumos errno
values are positive. The shim code consistently uses **negative** values because
callers are ported Linux DRM code that checks `ret < 0`.

---

## 4. illumos Primitive Mapping

| Linux | illumos | Used In |
|-------|---------|---------|
| `spinlock_t` / `spin_lock()` | `kmutex_t` / `mutex_enter()` | `fence_lock` |
| `kref` | `drm_ref_t` / `atomic_cas_32()` | `fence_ref` |
| `wait_queue_head_t` / `wake_up_all()` | `kcondvar_t` / `cv_broadcast()` | `fence_wait_cv` |
| `wait_event_interruptible_timeout()` | `cv_reltimedwait_sig()` | `dma_fence_wait_timeout` |
| `struct mutex` / `mutex_lock()` | `kmutex_t` / `mutex_enter()` | `ww_mutex.wm_lock` |
| `atomic_long_t` / `atomic_long_inc_return()` | `atomic_inc_64_nv()` | `ww_class.wc_stamp` |
| `ktime_get()` | `gethrtime()` | `fence_timestamp` |
| `test_and_set_bit()` | `atomic_cas_32()` | fence flags |
| `smp_wmb()` / `smp_rmb()` | `membar_producer()` / `membar_consumer()` | `dma_resv_add_fence` |
| `irq_work_queue()` | `taskq_dispatch_ent(system_taskq, TQ_NOSLEEP)` | fence_array deferred signal |
| `kmalloc()` / `kfree()` | `kmem_zalloc()` / `kmem_free(ptr, size)` | resv list allocation |
| `struct list_head` | `list_t` | `fence_cb_list` |

---

## 5. Verification Plan

| Step | Command | Expected Result |
|------|---------|-----------------|
| Build | `make debug` (on illumos host) | Clean compilation of 4 new .o files |
| Symbols | `nm drm \| grep dma_fence` | All exported functions visible |
| Upstream sync | `make upstream-sync LINUX_SRC=../linux-modern` | Files copied, shims preserved |
| Unit test | Write kernel module test (future) | Fence signal/callback/wait/array/resv all pass |

---

## 6. File Inventory

```
gfx-drm/
├── Makefile                                          [modified: +upstream-sync target]
├── tools/
│   └── upstream-sync.sh                              [NEW: 266 lines]
└── usr/src/uts/
    └── common/
        ├── drm/
        │   ├── drmP.h                                [modified: +5 includes]
        │   ├── drm_linux.h                           [modified: +smp_*mb macros]
        │   ├── drm_sun_ref.h                         [NEW:  93 lines]
        │   ├── drm_sun_ww_mutex.h                    [NEW: 105 lines]
        │   ├── drm_sun_dma_fence.h                   [NEW: 220 lines]
        │   ├── drm_sun_dma_fence_array.h             [NEW:  75 lines]
        │   └── drm_sun_dma_resv.h                    [NEW: 151 lines]
        └── io/drm/
            ├── Makefile.mod                           [modified: +4 objects]
            ├── drm_sun_ww_mutex.c                    [NEW: 234 lines]
            ├── drm_sun_dma_fence.c                   [NEW: 416 lines]
            ├── drm_sun_dma_fence_array.c             [NEW: 244 lines]
            └── drm_sun_dma_resv.c                    [NEW: 340 lines]
```

**Total new lines:** 2,144
**Total new files:** 10
**Total modified files:** 4
