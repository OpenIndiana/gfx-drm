# Virtio-GPU Port Implementation Plan

## Context

We're porting the Linux virtio-gpu DRM driver to illumos so we can test DRM
inside a QEMU VM with `-device virtio-gpu-pci`. This is the minimum viable
DRM port — the smallest driver that exercises the full stack from `modload`
to userspace ioctls (and eventually pixels on screen).

**What exists:**
- gfx-drm repo with Linux 3.14 DRM core + i915/radeon drivers ported to illumos
- dma_fence/dma_resv/ww_mutex shim layer (2,144 lines, complete)
- Existing shims: PCI, timer, workqueue, IDR, I2C, module entry points
- illumos virtio framework in illumos-gate (complete, modern)
- Linux 7.0-rc5 virtio-gpu source as reference (18 files, 6,385 lines)

**Key decision: DRM core approach — Option B (thin modern wrapper)**

The existing 3.14 DRM core has `drm_drv.c`, `drm_gem.c`, `drm_ioctl.c`,
`drm_fops.c`, `drm_crtc.c` etc. already ported. But virtio-gpu needs modern
APIs (`drm_gem_shmem_helper`, `drm_dev_alloc` style, `drm_gem_object_funcs`).

We build a parallel, minimal "modern DRM" layer that:
1. Defines `struct drm_gem_shmem_object` backed by `virtio_dma_t`
2. Provides the ~20 modern GEM helper functions virtio-gpu actually calls
3. Reuses existing `drm_device`/`drm_driver`/`drm_gem_object` with extensions
4. Does NOT disturb the working i915 driver

---

## Phase 0: Foundation Shims (~550 lines, ~1 week)

**Milestone:** All new shim headers compile; existing DRM module still builds clean.

### 0.1 — Completion primitive
**File:** `usr/src/uts/common/drm/drm_sun_completion.h` (~120 lines, header-only)

```c
typedef struct drm_completion {
    kmutex_t    dc_lock;
    kcondvar_t  dc_cv;
    boolean_t   dc_done;
    uint32_t    dc_count;
} drm_completion_t;
```

Functions: `drm_completion_init`, `drm_complete`, `drm_complete_all`,
`drm_wait_for_completion`, `drm_wait_for_completion_timeout`,
`drm_wait_for_completion_interruptible`, `drm_completion_done`,
`drm_reinit_completion`.

Pattern: `mutex_enter; done=B_TRUE; cv_broadcast; mutex_exit` for signal,
`mutex_enter; while(!done) cv_wait; mutex_exit` for wait.

Also extend `wait_event`/`wait_event_timeout`/`wake_up` macros — the existing
`drmP.h` (line ~297) has partial coverage. These use `kcondvar_t + kmutex_t`.

### 0.2 — Print macros
**File:** `usr/src/uts/common/drm/drm_sun_print.h` (~100 lines)

Map modern per-device debug macros to existing `cmn_err`:
- `drm_dbg(dev, fmt, ...)` → `DRM_DEBUG(fmt, ...)`
- `drm_err(dev, fmt, ...)` → `cmn_err(CE_WARN, fmt, ...)`
- `drm_info(dev, fmt, ...)` → `cmn_err(CE_NOTE, fmt, ...)`
- `DRM_ERROR_RATELIMITED` → `cmn_err(CE_WARN, ...)` with rate counter
- Verify `WARN_ON`, `WARN_ON_ONCE`, `BUG_ON` completeness in `drm_os_solaris.h`

### 0.3 — Managed allocation stubs
**File:** `usr/src/uts/common/drm/drm_sun_managed.h` (~80 lines)

For initial bring-up, implement as plain `kmem_zalloc`/`kmem_free`:
- `drmm_kzalloc(dev, size, flags)` → `kmem_zalloc(size, KM_SLEEP)`
- `drmm_kcalloc(dev, n, size, flags)` → `kmem_zalloc(n * size, KM_SLEEP)`
- `drmm_kfree(dev, ptr)` → no-op (freed at device destroy)

Tracking can come later. The key thing is compilation.

### 0.4 — Debugfs stubs
**File:** `usr/src/uts/common/drm/drm_sun_debugfs.h` (~40 lines)

No-op stubs: `debugfs_create_*`, `struct drm_info_list`, `drm_debugfs_create_files`.
Just enough so `virtgpu_debugfs.c` compiles to nothing.

### 0.5 — GEM object extensions
**File:** `usr/src/uts/common/drm/drm_sun_gem_modern.h` (~80 lines)

Compatibility layer so virtio-gpu can use modern GEM APIs without modifying
the 3.14 `struct drm_gem_object` in `drmP.h`:
- `drm_gem_object_get()` → `kref_get(&obj->refcount)` (exists as `drm_gem_object_reference`)
- `drm_gem_object_put()` → `drm_gem_object_unreference_unlocked()`
- Macros for `obj->funcs` dispatch (the virtio_gpu_object embeds its own funcs)
- `drm_gem_object_init()` compatibility

### 0.6 — Linux compat additions
**File:** Modify `usr/src/uts/common/drm/drm_linux.h` (~50 lines added)

- `rwsem` → `krwlock_t` (`init_rwsem`/`down_read`/`up_read`/`down_write`/`up_write`)
- `atomic64_t` / `atomic64_set` / `atomic64_read` / `atomic64_inc_return`
- `kvmalloc` / `kvfree` → `kmem_alloc` / `kmem_free`
- `ida_alloc` / `ida_free` → IDR wrapper or atomic counter
- `cpu_to_le32` / `le32_to_cpu` / `cpu_to_le64` / `le64_to_cpu` (verify coverage)
- `kmem_cache_create` calling convention wrapper (illumos has native `kmem_cache_create(9F)`)

### 0.7 — Workqueue extensions
**File:** Modify `usr/src/uts/common/drm/drm_sun_workqueue.h` + `.c` (~60 lines)

Add `schedule_work()` (dispatch to global system workqueue) and `flush_work()`
(wait for specific `work_struct` to finish). Requires adding a `done` flag +
condvar to `struct work_struct`.

### Verification
```bash
# Build the DRM misc module with new headers included
cd /home/toasty/ws/illumos/gfx-drm && make drm
# Must compile cleanly with no new warnings
```

---

## Phase 1: Virtio Transport Layer (~2,600 lines, ~2 weeks)

**Milestone:** `virtgpu_vq.c` compiles and can submit/receive commands via
illumos virtio framework.

### 1.1 — UAPI + protocol headers
**Files:**
- `usr/src/uts/common/drm/virtgpu_drm.h` — copy from Linux `include/uapi/drm/virtgpu_drm.h`
- `usr/src/uts/common/drm/virtio_gpu.h` — copy from Linux `include/uapi/linux/virtio_gpu.h`

Pure ABI/protocol definitions, no porting needed. Fix includes only.

### 1.2 — Port `virtgpu_drv.h` (~515 lines)
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_drv.h`

Key type substitutions:
| Linux | illumos |
|-------|---------|
| `struct virtio_device *vdev` | `virtio_t *vio` + `dev_info_t *dip` |
| `struct virtqueue *vq` | `virtio_queue_t *vq` |
| `struct kmem_cache *vbufs` | `kmem_cache_t *vbufs` |
| `spinlock_t` | `kmutex_t` |
| `wait_queue_head_t` | `kcondvar_t` + `kmutex_t` |
| `struct work_struct` | workqueue shim type |
| `struct idr` | existing `drm_sun_idr` |
| `struct drm_gem_shmem_object` | forward declare, defined in Phase 2 |

### 1.3 — Port `virtgpu_vq.c` (~1,493 lines) — **HARDEST FILE**
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_vq.c`

**Core transformation — command submission:**

Linux builds scatter-gather lists and submits atomically:
```c
virtqueue_add_sgs(vq, sgs, outcnt, incnt, vbuf, GFP_ATOMIC);
```

illumos builds chains incrementally:
```c
virtio_chain_t *vic = virtio_chain_alloc(vq, KM_NOSLEEP);
virtio_chain_append(vic, cmd_pa, cmd_len, VIRTIO_DIR_DEVICE_READS);
if (data_size)
    virtio_chain_append(vic, data_pa, data_len, VIRTIO_DIR_DEVICE_READS);
virtio_chain_append(vic, resp_pa, resp_len, VIRTIO_DIR_DEVICE_WRITES);
virtio_chain_data_set(vic, vbuf);
virtio_chain_submit(vic, B_TRUE);
```

**vbuf DMA memory:** Linux allocates vbufs from kmem_cache with inline
cmd/response buffers. On illumos these must be DMA-addressable.

**Recommended approach:** Use `virtio_dma_alloc()` for each vbuf, getting
physically-contiguous DMA memory. `VBUFFER_SIZE` is only ~144 bytes, so a
pool of pre-allocated DMA buffers (matching queue size) is practical. Store
`virtio_dma_t *` in the vbuf so we know the PA at submit time.

**Completion/dequeue:**
```c
// Interrupt handler (registered via virtio_queue_alloc):
static uint_t
virtgpu_ctrl_intr(caddr_t arg1, caddr_t arg2)
{
    schedule_work(&vgdev->ctrlq.dequeue_work);
    return (DDI_INTR_CLAIMED);
}

// Dequeue work function:
virtio_chain_t *vic;
while ((vic = virtio_queue_poll(vq)) != NULL) {
    vbuf = virtio_chain_data(vic);
    // process response...
    virtio_chain_free(vic);
}
```

**Config space reads:**
```c
// Linux: virtio_cread_le(vdev, struct virtio_gpu_config, num_scanouts, &val)
// illumos:
val = virtio_dev_get32(vio, offsetof(struct virtio_gpu_config, num_scanouts));
```

### 1.4 — Port `virtgpu_kms.c` (~353 lines)
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_kms.c`

`virtio_gpu_init()` becomes the core of attach. Sequence:
1. `virtio_init(dip)` → init framework
2. `virtio_init_features(vio, features)` → negotiate features
3. Read config (num_scanouts, num_capsets)
4. `virtio_queue_alloc(vio, 0, "control", ctrl_handler, ...)` → control queue
5. `virtio_queue_alloc(vio, 1, "cursor", cursor_handler, ...)` → cursor queue
6. `virtio_init_complete(vio, VIRTIO_ANY_INTR_TYPE)` → finalize
7. Allocate vbuf pool
8. Initialize DRM device

### 1.5 — Port `virtgpu_fence.c` (~158 lines)
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_fence.c`

Straightforward — the `dma_fence` shim already provides everything:
`dma_fence_init`, `dma_fence_signal_locked`, `dma_fence_get`, `dma_fence_put`,
`dma_fence_is_later`. Replace `spin_lock_irqsave` → `mutex_enter`.

### Verification
```bash
# Compile Phase 1 files (even if not linkable yet)
# Check for any missing type/macro/function references
```

---

## Phase 2: GEM Memory Management (~1,000 lines, ~1.5 weeks)

**Milestone:** Can allocate and free GPU buffer objects.

### 2.1 — GEM shmem helper for illumos
**Files:**
- `usr/src/uts/common/drm/drm_sun_gem_shmem.h` (~150 lines)
- `usr/src/uts/common/io/drm/drm_sun_gem_shmem.c` (~250 lines)

Replace Linux `shmem_file_setup()` + `struct page` with `ddi_dma_mem_alloc`:

```c
struct drm_gem_shmem_object {
    struct drm_gem_object   base;
    struct dma_resv         _resv;       /* inline reservation object */
    ddi_dma_handle_t        dma_hdl;     /* DMA handle */
    ddi_acc_handle_t        acc_hdl;     /* access handle */
    caddr_t                 vaddr;       /* kernel VA */
    size_t                  real_size;   /* actual allocation size */
    unsigned int            pages_use_count;
    unsigned int            pages_pin_count;
    /* Scatter-gather built from DMA cookies */
    struct sg_table         *sgt;
};
```

Minimal `struct sg_table` / `struct scatterlist` for cookie iteration:
```c
struct scatterlist {
    uint64_t    dma_address;
    uint32_t    length;
};
struct sg_table {
    struct scatterlist  *sgl;
    unsigned int        nents;
};
```

Functions (subset virtio-gpu actually calls):
- `drm_gem_shmem_create(dev, size)` — `ddi_dma_alloc_handle` + `ddi_dma_mem_alloc`
- `drm_gem_shmem_free(shmem)` — free everything
- `drm_gem_shmem_get_pages_sgt(shmem)` — build sg_table from `ddi_dma_cookie_iter`
- `drm_gem_shmem_pin/unpin` — no-ops (pages always pinned on illumos)
- `drm_gem_shmem_vmap/vunmap` — return/release kernel VA

### 2.2 — Port `virtgpu_object.c` (~273 lines)
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_object.c`

Creates GEM objects, manages resource IDs, builds `virtio_gpu_mem_entry`
arrays from sg_table. The `virtio_gpu_object_shmem_init()` iterates sg_table
to build the `ents` array — maps directly to DMA cookie iteration.

### 2.3 — Port `virtgpu_gem.c` (~298 lines)
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_gem.c`

`dumb_create`, array helpers, reservation locking. Uses `dma_resv_lock_interruptible`
and `dma_resv_reserve_fences` from the existing shim.

Need `drm_gem_lock_reservations` / `drm_gem_unlock_reservations` (~50 lines)
using the existing `ww_mutex` implementation.

### Verification
```bash
# Unit test: allocate GEM object, verify DMA cookies, free
```

---

## Phase 3: Driver Shell and Ioctls (~1,700 lines, ~1.5 weeks)

**Milestone:** Driver loads in QEMU, creates `/dev/dri/renderD128`, handles
basic ioctls.

### 3.1 — DDI entry point (`virtgpu_sunmod.c`, ~400 lines)
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_sunmod.c`

Follow the pattern of `drm_sunmod.c` + existing virtio drivers (e.g., vioblk):

```c
static struct cb_ops virtgpu_cb_ops = {
    drm_sun_open, drm_sun_close, nodev, nodev,    /* open, close, strategy, print */
    drm_sun_ioctl, nodev, nodev,                   /* ioctl, devmap, mmap */
    ...
};

static struct dev_ops virtgpu_dev_ops = {
    DEVO_REV, 0, drm_sun_getinfo,
    nulldev, nulldev,
    virtgpu_attach, virtgpu_detach,
    nodev, &virtgpu_cb_ops, NULL, virtgpu_quiesce
};

static struct modldrv virtgpu_modldrv = {
    &mod_driverops, "virtio-gpu DRM", &virtgpu_dev_ops
};
static struct modlinkage virtgpu_modlinkage = {
    MODREV_1, { &virtgpu_modldrv, NULL }
};

int _init(void)  { return mod_install(&virtgpu_modlinkage); }
int _fini(void)  { return mod_remove(&virtgpu_modlinkage); }
int _info(...)   { return mod_info(&virtgpu_modlinkage, modinfop); }
```

`virtgpu_attach(dip, DDI_ATTACH)`:
1. `virtio_init(dip)`, negotiate features
2. Set up DRM device via `drm_sun_attach()` equivalent
3. Call `virtio_gpu_init()` (from virtgpu_kms.c)
4. `ddi_create_minor_node(dip, "drm", S_IFCHR, ...)`

### 3.2 — Port `virtgpu_ioctl.c` (~735 lines)
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_ioctl.c`

Adapt ioctl table to illumos `drm_ioctl_desc` format. Set `copyin32`/`copyout32`
to NULL initially (64-bit only).

Essential ioctls for render-only:
- `VIRTGPU_GETPARAM` — query device capabilities
- `VIRTGPU_RESOURCE_CREATE` — create GPU resource
- `VIRTGPU_RESOURCE_INFO` — query resource
- `VIRTGPU_MAP` — map GEM for CPU access
- `VIRTGPU_WAIT` — wait for fence
- `VIRTGPU_GET_CAPS` — capability sets

Replace `copy_to_user` → `ddi_copyout`, `copy_from_user` → `ddi_copyin`,
`memdup_user` → copyin + alloc.

### 3.3 — Stub `virtgpu_submit.c` (~100 lines stub)
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_submit.c`

Stub returning `-ENOSYS`. Only needed for virgl 3D rendering, not basic 2D.

### 3.4 — Stub `virtgpu_debugfs.c` (~20 lines)
**File:** `usr/src/uts/common/io/virtio_gpu/virtgpu_debugfs.c`

Empty init function, no-op.

### 3.5 — Driver conf file
**File:** `usr/src/uts/common/io/virtio_gpu/virtio_gpu.conf`

PCI binding for virtio-gpu device (vendor 0x1AF4, device 0x1050):
```
name="virtio_gpu" parent="pci" unit-address="0";
```

Or alternatively, use the PCI ID alias mechanism via `pci_id_list` in
the driver's `drm_driver` struct.

### Verification
```bash
qemu-system-x86_64 \
  -enable-kvm -m 2G \
  -drive file=illumos.img,if=virtio \
  -device virtio-gpu-pci \
  -display none -serial stdio -nographic

# In illumos guest:
modload /kernel/drv/amd64/virtio_gpu
devfsadm -i virtio_gpu
ls -la /dev/dri/renderD*     # Should show renderD128
dtrace -n 'fbt::virtio_gpu_*:entry { printf("%s", probefunc); }'
```

---

## Phase 4: Build System (~100 lines, ~2 days)

**Milestone:** `make` produces `virtio_gpu` kernel module.

### 4.1 — Source directory
`usr/src/uts/common/io/virtio_gpu/Makefile.mod`:
```makefile
VIRTIO_GPU_OBJS = \
    virtgpu_sunmod.o \
    virtgpu_kms.o \
    virtgpu_vq.o \
    virtgpu_fence.o \
    virtgpu_gem.o \
    virtgpu_object.o \
    virtgpu_ioctl.o \
    virtgpu_submit.o \
    virtgpu_debugfs.o
```

### 4.2 — Build directory
`usr/src/uts/intel/virtio_gpu/Makefile`:
```makefile
MODULE = virtio_gpu
OBJECTS = $(VIRTIO_GPU_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE = $(ROOT_DRV_DIR)/$(MODULE)

CMN_DRM = $(UTSBASE)/common/drm
INC_PATH += -I$(CMN_DRM) -I$(UTSBASE)/common/io/virtio

LDFLAGS += -dy -Nmisc/drm -Nmisc/virtio
```

### 4.3 — Parent Makefile updates
- `usr/src/uts/intel/Makefile.intel` — add `virtio_gpu` to `DRV_KMODS`
- `usr/src/uts/common/io/drm/Makefile.mod` — add `drm_sun_gem_shmem.o` to `DRM_OBJS`

---

## Phase 5: Display Support (DEFERRED — ~3 weeks when ready)

**Milestone:** QEMU shows graphical output.

### Requires (not yet ported):
- `drm_atomic.c`, `drm_atomic_helper.c`, `drm_atomic_state_helper.c` (~4,400 lines)
- `drm_connector.c`, `drm_crtc.c`, `drm_plane.c`, `drm_framebuffer.c` (~2,100 lines)
- `drm_vblank.c`, `drm_edid.c` (exists), `drm_probe_helper.c` (~3,100 lines)

### Deferred virtio-gpu files:
- `virtgpu_display.c` (401 lines) — CRTC/connector/encoder
- `virtgpu_plane.c` (608 lines) — display planes, scanout
- `virtgpu_prime.c` (347 lines) — DMA-buf import/export
- `virtgpu_vram.c` (231 lines) — host-visible VRAM

### Test when ready:
```bash
qemu-system-x86_64 \
  -enable-kvm -m 2G \
  -drive file=illumos.img,if=virtio \
  -device virtio-gpu-pci \
  -display gtk
```

---

## Essential vs Deferred

### Must work (Phases 0-4):
| Component | Why |
|-----------|-----|
| Virtqueue transport (`virtgpu_vq.c`) | Entire driver depends on this |
| Fence signaling (`virtgpu_fence.c`) | Needed for any synchronous op |
| GEM allocation (`virtgpu_object.c`, `gem.c`) | Buffer management |
| Basic ioctls (`virtgpu_ioctl.c`) | Userspace interface |
| DMA memory (`drm_sun_gem_shmem`) | Backing all GEM objects |
| Workqueue + wait_event | Used throughout |
| DDI shell (`virtgpu_sunmod.c`) | Module load/attach |

### Can stub/defer:
| Component | Why safe to defer |
|-----------|-------------------|
| Display (`display.c`, `plane.c`) | Render node works without display |
| 3D submit (`submit.c`) | Only for virgl/venus |
| DMA-buf (`prime.c`) | Only for multi-device |
| Host VRAM (`vram.c`) | Advanced blob feature |
| Debugfs, tracing | Pure diagnostics |
| 32-bit ioctl compat | 64-bit only initially |
| `drm_send_event` | Fence events to userspace (can error) |
| `drm_dev_enter/exit` | Hotplug guard (stub as always-enter) |
| Context init ioctl | Only for virgl |

---

## File Inventory

### New files (Phases 0-4):

```
usr/src/uts/common/
├── drm/
│   ├── drm_sun_completion.h          [NEW: ~120 lines]
│   ├── drm_sun_print.h              [NEW: ~100 lines]
│   ├── drm_sun_managed.h            [NEW:  ~80 lines]
│   ├── drm_sun_debugfs.h            [NEW:  ~40 lines]
│   ├── drm_sun_gem_modern.h         [NEW:  ~80 lines]
│   ├── drm_sun_gem_shmem.h          [NEW: ~150 lines]
│   ├── virtgpu_drm.h                [NEW: copy from Linux UAPI]
│   └── virtio_gpu.h                 [NEW: copy from Linux UAPI]
├── io/
│   ├── drm/
│   │   └── drm_sun_gem_shmem.c      [NEW: ~250 lines]
│   └── virtio_gpu/
│       ├── virtgpu_sunmod.c          [NEW: ~400 lines]
│       ├── virtgpu_drv.h             [NEW: ~515 lines, ported]
│       ├── virtgpu_kms.c             [NEW: ~353 lines, ported]
│       ├── virtgpu_vq.c              [NEW: ~1,493 lines, ported]
│       ├── virtgpu_fence.c           [NEW: ~158 lines, ported]
│       ├── virtgpu_gem.c             [NEW: ~298 lines, ported]
│       ├── virtgpu_object.c          [NEW: ~273 lines, ported]
│       ├── virtgpu_ioctl.c           [NEW: ~735 lines, ported]
│       ├── virtgpu_submit.c          [NEW: ~100 lines, stub]
│       ├── virtgpu_debugfs.c         [NEW:  ~20 lines, stub]
│       ├── virtio_gpu.conf           [NEW: conf file]
│       └── Makefile.mod              [NEW: obj list]
└── ...

usr/src/uts/intel/
└── virtio_gpu/
    └── Makefile                      [NEW: build rules]
```

### Modified files:
- `usr/src/uts/common/drm/drm_linux.h` — rwsem, atomic64, kvmalloc, endian
- `usr/src/uts/common/drm/drm_sun_workqueue.h/.c` — schedule_work, flush_work
- `usr/src/uts/common/io/drm/Makefile.mod` — add drm_sun_gem_shmem.o
- `usr/src/uts/intel/Makefile.intel` — add virtio_gpu to DRV_KMODS

### Estimated totals:
| Phase | New Lines | Modified Lines |
|-------|-----------|----------------|
| Phase 0: Shims | ~550 | ~110 |
| Phase 1: Transport | ~2,600 | — |
| Phase 2: GEM | ~1,000 | — |
| Phase 3: Driver shell | ~1,300 | — |
| Phase 4: Build | ~100 | ~20 |
| **Total** | **~5,550** | **~130** |

---

## Critical Reference Files

| File | Purpose |
|------|---------|
| `gfx-drm/usr/src/uts/common/drm/drmP.h` | Existing drm_device/driver/gem_object structs |
| `gfx-drm/usr/src/uts/common/io/drm/drm_sunmod.c` | Template for DDI integration |
| `linux-modern/drivers/gpu/drm/virtio/virtgpu_vq.c` | Source for hardest port file |
| `linux-modern/drivers/gpu/drm/virtio/virtgpu_drv.h` | All driver structures |
| `illumos-gate-current/usr/src/uts/common/io/virtio/virtio.h` | Target virtio API |
| `illumos-gate-current/usr/src/uts/common/io/vioblk/vioblk.c` | Reference virtio driver |

---

## Implementation Order

Work phases 0-4 in order. Within each phase, the recommended file order is:

1. **Phase 0:** drm_linux.h additions → completion → print → managed → debugfs → gem_modern → workqueue ext
2. **Phase 1:** UAPI headers → virtgpu_drv.h → virtgpu_fence.c → virtgpu_kms.c → virtgpu_vq.c
3. **Phase 2:** drm_sun_gem_shmem → virtgpu_object.c → virtgpu_gem.c
4. **Phase 3:** virtgpu_sunmod.c → virtgpu_ioctl.c → stubs → conf
5. **Phase 4:** Makefiles → build test → QEMU test

First real test: `modload virtio_gpu` in QEMU with `-device virtio-gpu-pci`.
Success = PCI attach, virtqueue init, feature negotiation, `ls /dev/dri/renderD128`.
