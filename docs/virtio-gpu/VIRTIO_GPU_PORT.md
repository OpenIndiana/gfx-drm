# Porting virtio-gpu to illumos: Guest Driver

## Why This Driver

1. **Smallest DRM driver** -- 6,385 lines across 18 files (vs 500K+ for amdgpu)
2. **illumos-as-VM guest** -- OpenIndiana under QEMU/KVM, bhyve, or crosvm gets GPU acceleration
3. **DRM test vehicle** -- validates the DRM core port before tackling real hardware drivers
4. **Display for VMs** -- accelerated desktop for OpenIndiana as a GUI distro under Linux hosts
5. **No hardware secrets** -- paravirtual transport, all rendering done by host

## Driver Architecture

virtio-gpu is NOT a hardware GPU driver. It's a paravirtual transport layer:

```
┌─────────────────────────────────────────┐
│  Guest (illumos)                        │
│                                         │
│  Mesa (virgl/venus)                     │
│       ↓                                 │
│  virtio-gpu DRM driver                  │
│       ↓ (virtqueue commands)            │
│  virtio transport (vioblk-style rings)  │
├─────────────────────────────────────────┤
│  Host hypervisor (QEMU/crosvm)          │
│       ↓                                 │
│  virglrenderer / venus / gfxstream      │
│       ↓                                 │
│  Host GPU driver (amdgpu/i915/nvidia)   │
│       ↓                                 │
│  Physical GPU                           │
└─────────────────────────────────────────┘
```

The guest driver sends opaque command buffers to the host via virtqueues.
The host interprets them using the real GPU. The driver supports:

| Capset | Rendering API | Use Case |
|--------|--------------|----------|
| virgl | OpenGL (Gallium) | Desktop compositing, 3D apps |
| venus | Vulkan | Games, compute shaders |
| gfxstream | Vulkan (Android) | Android guests |
| cross-domain | Wayland proxy | Wayland guests on crosvm |

---

## File Inventory

| File | Lines | Purpose | Port Difficulty |
|------|-------|---------|-----------------|
| `virtgpu_drv.c` | 258 | Driver registration, PCI probe | Medium (virtio attach model) |
| `virtgpu_drv.h` | 515 | All structures, declarations | Direct port |
| `virtgpu_kms.c` | 353 | Device init, feature probe, queue setup | Medium (virtio API mapping) |
| `virtgpu_vq.c` | 1,493 | Virtqueue command transport | **Hardest** (virtio chain API differs) |
| `virtgpu_fence.c` | 158 | dma_fence implementation | Easy (uses drm_sun_dma_fence) |
| `virtgpu_gem.c` | 298 | GEM object management, arrays | Medium (needs GEM shmem) |
| `virtgpu_object.c` | 273 | Resource creation (shmem/vram) | Medium |
| `virtgpu_ioctl.c` | 735 | 12 userspace ioctls | Easy (mostly copyin/copyout) |
| `virtgpu_display.c` | 401 | CRTC, connector, encoder | Needs DRM atomic |
| `virtgpu_plane.c` | 608 | Display planes, scanout | Needs DRM atomic |
| `virtgpu_submit.c` | 542 | Execbuffer command submission | Medium (syncobj deps) |
| `virtgpu_prime.c` | 347 | DMA-buf import/export | Defer (advanced feature) |
| `virtgpu_vram.c` | 231 | Host-visible VRAM blobs | Defer (needs virtio SHM region) |
| `virtgpu_debugfs.c` | 112 | Debug info | Stub |
| `virtgpu_trace.h` | 56 | Tracepoints | Stub |
| **Total** | **6,385** | | |

---

## Dependencies Map

### Already Done (from dma_fence work)

| Dependency | Status | Used By |
|------------|--------|---------|
| `dma_fence` | DONE (`drm_sun_dma_fence`) | `virtgpu_fence.c`, all fenced commands |
| `dma_resv` | DONE (`drm_sun_dma_resv`) | GEM array locking, WAIT ioctl |
| `ww_mutex` | DONE (`drm_sun_ww_mutex`) | Multi-BO reservation locking |
| `kref` / refcount | DONE (`drm_sun_ref`) | GEM objects, fences |

### Already Exists in illumos

| Dependency | illumos Component | Used By |
|------------|------------------|---------|
| Virtio transport | `virtio_init/queue_alloc/chain_*` in `<sys/virtio/virtio.h>` | All virtqueue I/O |
| Virtio DMA | `virtio_dma_alloc/bind` | Command buffers, backing pages |
| Virtio config | `virtio_dev_get/put` | Feature probe, scanout count |
| Virtio interrupts | Queue handler callback in `virtio_queue_alloc` | Completion interrupts |
| kmem_cache | `kmem_cache_create/alloc/free` (same API name) | vbuffer pool |
| PCI | `drm_sun_pci` (existing shim) | Device discovery |
| Workqueue | `drm_sun_workqueue` (existing shim) | Deferred dequeue, config change |
| Timer | `drm_sun_timer` (existing shim) | Vblank timer |

### Needs to be Written (from ILLUMOS_NATIVE_API_ESTIMATES.md)

| Dependency | Est. Lines | Blocking For | Priority |
|------------|-----------|--------------|----------|
| Completion | 120 | Config change wait, capset query | P0 |
| DRM Print | 100 | All logging | P0 |
| DRM Managed (`drmm_*`) | 200 | Device init, resource cleanup | P0 |
| rwsem macros | 20 | Resource export lock | P0 |
| Debugfs stub | 80 | Compilation | P0 |
| DRM Core midlayer (subset) | ~5,000 | Everything | P0 |
| GEM shmem helper | ~800 | Buffer objects | P0 |
| Scatter-gather + DMA mapping | 400 | Page backing, transfers | P0 |
| Syncobj | ~1,200 | Execbuffer (submit.c) | P1 (defer for phase 1) |
| Firmware loading | 0 | NOT NEEDED (no firmware!) | -- |
| TTM | 0 | NOT NEEDED | -- |
| DRM scheduler | 0 | NOT NEEDED | -- |

### Needs Implementation in illumos Virtio Framework

| Feature | Description | Est. Lines |
|---------|-------------|-----------|
| `virtio_get_shm_region()` | Parse `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG` for host-visible VRAM | ~150 |

This is optional -- without it, host-visible VRAM mode (`virtgpu_vram.c`) is
disabled. shmem mode and virgl/venus 3D still work.

---

## DRM Core Subset Needed

virtio-gpu does NOT use TTM or DRM scheduler. The DRM core subset is
significantly smaller than what amdgpu needs:

| DRM Core File | Lines | Why virtio-gpu Needs It |
|---------------|-------|------------------------|
| `drm_drv.c` | ~1,000 | `drm_dev_alloc`, `drm_dev_register`, driver struct |
| `drm_file.c` | ~500 | File open/close, per-file context |
| `drm_ioctl.c` | ~800 | Ioctl dispatch table |
| `drm_gem.c` | ~1,200 | GEM handle create/lookup/close, object lifecycle |
| `drm_gem_shmem_helper.c` | ~800 | Shmem-backed GEM objects (the main memory backend) |
| `drm_mm.c` | ~600 | Range manager (for VRAM blobs -- EXISTS in gfx-drm) |
| `drm_managed.c` | ~250 | Lifecycle cleanup |
| `drm_print.c` | ~100 | Logging |
| `drm_syncobj.c` | ~1,200 | Explicit sync (can defer, needed for execbuffer) |
| `drm_mode_config.c` | ~500 | Mode config init (for display) |
| `drm_connector.c` | ~800 | Connector management (for display) |
| `drm_crtc.c` | ~500 | CRTC management (for display) |
| `drm_plane.c` | ~400 | Plane management (for display) |
| `drm_framebuffer.c` | ~400 | Framebuffer management (for display) |
| `drm_vblank.c` | ~600 | Vblank timing (for display) |
| `drm_atomic.c` | ~800 | Atomic state (for display) |
| `drm_atomic_helper.c` | ~3,000 | Atomic commit (for display) |
| `drm_atomic_state_helper.c` | ~600 | State alloc/free (for display) |
| `drm_edid.c` | ~2,000 | EDID parsing (for display) |
| `drm_probe_helper.c` | ~500 | Connector polling (for display) |
| **Headless total** | **~5,450** | Items 1-8 above |
| **Display total** | **~14,550** | All items |

**Strategy:** Port headless first (render node only, no display). This is
items 1-8 (~5,450 lines). Display adds ~9,100 more lines.

---

## Phased Port Plan

### Phase 0: Shim Prerequisites (~520 lines new)

Before touching virtio-gpu code, write these small shims:

| File | Lines | What |
|------|-------|------|
| `drm_sun_completion.h` | 120 | `kmutex_t` + `kcondvar_t` + `boolean_t done` |
| `drm_sun_print.h` | 100 | `drm_dbg`/`drm_err` → `cmn_err` macros |
| `drm_sun_managed.c/.h` | 200 | Destructor list on device struct |
| `drm_sun_debugfs.h` | 80 | No-op stubs |
| `drm_linux.h` additions | 20 | `rwsem` → `krwlock_t` macros |

### Phase 1: Headless Render Node (~5,450 + 4,200 lines to port)

**Goal:** `modload` the driver, attach to virtio-gpu device, allocate GEM
objects, submit commands via render node. No display.

**DRM core files to port** (5,450 lines):
- `drm_drv.c`, `drm_file.c`, `drm_ioctl.c` -- device lifecycle
- `drm_gem.c` + `drm_gem_shmem_helper.c` -- memory management
- `drm_managed.c`, `drm_print.c` -- infrastructure
- `drm_mm.c` -- already in gfx-drm, update if needed

**virtio-gpu files to port** (4,200 lines, 10 files):

| File | Lines | Key Porting Work |
|------|-------|-----------------|
| `virtgpu_drv.c` | 258 | Replace `module_init` with `_init/_fini`. Replace `register_virtio_driver` with `virtio_init()` in `attach(9E)`. Skip VGA arbitration. |
| `virtgpu_drv.h` | 515 | Replace Linux types. `spinlock_t` → `kmutex_t`. `wait_queue_head_t` → `kcondvar_t` + `kmutex_t`. `struct work_struct` → `taskq_ent_t`. |
| `virtgpu_kms.c` | 353 | Map virtio feature query to `virtio_features_present()`. Map `virtio_find_vqs` to per-queue `virtio_queue_alloc()`. Map config reads to `virtio_dev_get32()`. |
| `virtgpu_vq.c` | 1,493 | **Main work.** Convert `virtqueue_add_sgs()` scatter-gather model to illumos `virtio_chain_alloc()` + `virtio_chain_append(pa, size, dir)` model. Convert `virtqueue_get_buf()` to `virtio_queue_poll()`. Convert `virtqueue_kick` to `virtio_chain_submit()`. |
| `virtgpu_fence.c` | 158 | Trivial -- uses `dma_fence_init/signal_locked/get/put` from our `drm_sun_dma_fence`. Replace `spin_lock_irqsave` with `mutex_enter`. |
| `virtgpu_gem.c` | 298 | Replace `drm_gem_shmem_*` calls with illumos-ported equivalents. Replace `dma_resv_*` calls with our `drm_sun_dma_resv`. |
| `virtgpu_object.c` | 273 | Map `drm_gem_shmem_get_pages_sgt()` → illumos DMA cookie iteration. Map `sg_table` → `drm_sg_table_t` from our shim. |
| `virtgpu_ioctl.c` | 735 | `copy_to_user/copy_from_user` → `ddi_copyout/ddi_copyin`. `memdup_user` → copyin + alloc. Rest is struct manipulation. |
| `virtgpu_submit.c` | 542 | Defer syncobj parts initially. Core execbuffer uses fence + virtqueue submit. |
| (Makefile.mod) | -- | Add object list |

**Virtqueue porting detail** (`virtgpu_vq.c`, the hardest file):

Linux sends commands by building scatter-gather lists:
```c
// Linux: one call with pre-built sg arrays
virtqueue_add_sgs(vq, sgs, outcnt, incnt, vbuf, GFP_ATOMIC);
```

illumos builds chains incrementally:
```c
// illumos: build chain piece by piece
virtio_chain_t *chain = virtio_chain_alloc(vq, KM_NOSLEEP);
// command header (device reads)
virtio_chain_append(chain, cmd_pa, cmd_len, VIRTIO_DIR_DEVICE_READS);
// optional data payload (device reads)
virtio_chain_append(chain, data_pa, data_len, VIRTIO_DIR_DEVICE_READS);
// response buffer (device writes)
virtio_chain_append(chain, resp_pa, resp_len, VIRTIO_DIR_DEVICE_WRITES);
// submit
virtio_chain_data_set(chain, vbuf);  // attach vbuf as private data
virtio_chain_submit(chain, B_TRUE);  // submit + notify
```

And completion handling:
```c
// Linux:
while ((vbuf = virtqueue_get_buf(vq, &len)) != NULL) { ... }

// illumos:
virtio_chain_t *chain;
while ((chain = virtio_queue_poll(vq)) != NULL) {
    vbuf = virtio_chain_data(chain);
    len = virtio_chain_received_length(chain);
    // process response...
    virtio_chain_free(chain);
}
```

**Test:** Load module in illumos VM under QEMU with `-device virtio-gpu-pci`.
Verify: PCI attach, virtqueue init, feature negotiation, `GETPARAM` ioctl works
from userspace via libdrm.

### Phase 2: Display (~9,100 lines DRM KMS + 1,009 lines virtio-gpu)

**Goal:** Desktop output. CRTC, connector, mode setting.

**DRM KMS files to port** (~9,100 lines):
- `drm_mode_config.c`, `drm_connector.c`, `drm_crtc.c`, `drm_plane.c`
- `drm_framebuffer.c`, `drm_vblank.c`, `drm_edid.c`, `drm_probe_helper.c`
- `drm_atomic.c`, `drm_atomic_helper.c`, `drm_atomic_state_helper.c`

**virtio-gpu display files** (1,009 lines):
- `virtgpu_display.c` (401 lines) -- CRTC/connector/encoder setup, uses atomic helpers
- `virtgpu_plane.c` (608 lines) -- Plane update, damage tracking, scanout commands

**Test:** Boot OpenIndiana in QEMU with virtio-gpu, get Xorg or Wayland
compositor running with hardware-accelerated display.

### Phase 3: 3D Acceleration (Mesa userspace, no kernel changes)

**Goal:** virgl OpenGL and/or venus Vulkan.

No additional kernel work needed -- the driver already forwards opaque 3D
commands via `EXECBUFFER` ioctl. What's needed is:

1. Port Mesa with virgl/venus support (userspace, not kernel)
2. libdrm with virtio-gpu support (userspace library)

With virgl: OpenGL 4.3+ in the VM (depends on host GPU capabilities).
With venus: Vulkan 1.3 in the VM.

### Phase 4: Advanced Features (optional)

| Feature | File | What | When |
|---------|------|------|------|
| DMA-buf sharing | `virtgpu_prime.c` | Cross-device buffer sharing | When needed |
| Host-visible VRAM | `virtgpu_vram.c` | Zero-copy host memory mapping | After `virtio_get_shm_region` impl |
| Syncobj timeline | `virtgpu_submit.c` | Explicit sync with timelines | After `drm_syncobj` port |
| Context init | `virtgpu_ioctl.c` | Per-context capset selection | After basic 3D works |

---

## Virtio Transport API Mapping

| Linux virtio API | illumos virtio API | Notes |
|------------------|--------------------|-------|
| `register_virtio_driver()` | `virtio_init(dip)` in `attach(9E)` | illumos uses DDI attach, not Linux driver model |
| `virtio_has_feature(vdev, F)` | `virtio_features_present(vio, 1ULL << F)` | Bitmap check after negotiation |
| `virtio_cread_le(vdev, T, off, &v)` | `v = virtio_dev_get32(vio, off)` | Endian handling built into illumos API |
| `virtio_find_vqs(vdev, 2, vqs, info, NULL)` | Two calls to `virtio_queue_alloc(vio, idx, name, handler, arg, B_TRUE, max_segs)` | illumos allocates queues individually |
| `virtio_device_ready(vdev)` | `virtio_init_complete(vio, DDI_INTR_TYPE_MSIX)` | Also enables interrupts |
| `virtio_reset_device(vdev)` | `virtio_device_reset(vio)` | Resets to initial state |
| `virtqueue_add_sgs(vq, sgs, out, in, data, gfp)` | `chain = virtio_chain_alloc(); virtio_chain_append(chain, pa, len, dir); virtio_chain_submit(chain, notify)` | Chain-building model vs sg-list model |
| `virtqueue_get_buf(vq, &len)` | `chain = virtio_queue_poll(vq); data = virtio_chain_data(chain); len = virtio_chain_received_length(chain)` | Returns chain, not raw data pointer |
| `virtqueue_kick_prepare() + virtqueue_notify()` | Implicit in `virtio_chain_submit(chain, B_TRUE)` | Or explicit `virtio_queue_flush(vq)` |
| `virtqueue_disable_cb(vq)` | `virtio_queue_no_interrupt(vq, B_TRUE)` | Suppress interrupts during polling |
| `virtqueue_enable_cb(vq)` | `virtio_queue_no_interrupt(vq, B_FALSE)` | Re-enable interrupts |
| `vq->num_free` | `virtio_queue_size(vq) - virtio_queue_nactive(vq)` | Computed, not direct field |
| `virtio_get_shm_region(vdev, &region, id)` | **NOT IMPLEMENTED** -- needs ~150 lines | Parse `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG` |

---

## GEM shmem on illumos

The biggest conceptual gap. Linux `drm_gem_shmem_helper` uses:
- `shmem_file_setup()` -- create anonymous tmpfs-backed file
- `shmem_read_mapping_page()` -- get pages from the file
- Pages are `struct page *` with reference counting

illumos has no `shmem_file_setup`. The equivalent approach:

```c
/*
 * Allocate DMA-capable anonymous pages for a GEM object.
 * Uses ddi_dma_mem_alloc which gives us:
 *   - kernel virtual address (for CPU access)
 *   - DMA handle (for building virtio sg lists)
 *   - cache-coherent allocation (DDI_DMA_CONSISTENT)
 */
int
drm_gem_shmem_create_illumos(dev_info_t *dip, size_t size,
    ddi_dma_attr_t *attr, caddr_t *kvap, ddi_dma_handle_t *hdlp,
    ddi_acc_handle_t *accp)
{
    ddi_device_acc_attr_t acc = {
        .devacc_attr_version = DDI_DEVICE_ATTR_V1,
        .devacc_attr_endian_flags = DDI_NEVERSWAP_ACC,
        .devacc_attr_dataorder = DDI_STRICTORDER_ACC,
    };
    size_t real_size;

    if (ddi_dma_alloc_handle(dip, attr, DDI_DMA_SLEEP, NULL, hdlp) != 0)
        return (-ENOMEM);

    if (ddi_dma_mem_alloc(*hdlp, size, &acc, DDI_DMA_CONSISTENT,
        DDI_DMA_SLEEP, NULL, kvap, &real_size, accp) != 0) {
        ddi_dma_free_handle(hdlp);
        return (-ENOMEM);
    }

    return (0);
}
```

For the virtio-gpu scatter-gather list (sent to host via `RESOURCE_ATTACH_BACKING`):

```c
/*
 * Build virtio_gpu_mem_entry array from DMA cookies.
 * Each cookie is one contiguous physical segment.
 */
int nents, i;
ddi_dma_cookie_t cookie;
const ddi_dma_cookie_t *cp = NULL;

ddi_dma_addr_bind_handle(hdl, NULL, kva, size,
    DDI_DMA_RDWR | DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL,
    &cookie, &nents);

/* First cookie is in 'cookie', rest via iterator */
entries[0].addr = cookie.dmac_laddress;
entries[0].length = cookie.dmac_size;

i = 1;
while ((cp = ddi_dma_cookie_iter(hdl, cp)) != NULL) {
    entries[i].addr = cp->dmac_laddress;
    entries[i].length = cp->dmac_size;
    i++;
}
```

This maps directly to what `virtio_gpu_object_shmem_init()` does -- it builds
the same `virtio_gpu_mem_entry` array from Linux sg_table.

---

## Module Structure

```
usr/src/uts/
├── common/
│   ├── io/drm/
│   │   ├── virtio_gpu/              ← NEW: driver source
│   │   │   ├── virtgpu_drv.c
│   │   │   ├── virtgpu_kms.c
│   │   │   ├── virtgpu_vq.c
│   │   │   ├── virtgpu_fence.c
│   │   │   ├── virtgpu_gem.c
│   │   │   ├── virtgpu_object.c
│   │   │   ├── virtgpu_ioctl.c
│   │   │   ├── virtgpu_display.c
│   │   │   ├── virtgpu_plane.c
│   │   │   ├── virtgpu_submit.c
│   │   │   ├── virtgpu_prime.c
│   │   │   ├── virtgpu_vram.c
│   │   │   └── Makefile.mod         ← VIRTGPU_OBJS list
│   │   ├── drm_sun_*.c             ← existing + new shims
│   │   └── Makefile.mod            ← existing DRM_OBJS
│   └── drm/
│       ├── virtgpu_drv.h            ← ported header
│       └── drm_sun_*.h             ← existing + new shim headers
└── intel/
    └── virtio_gpu/
        └── Makefile                 ← module build rules
```

Module config (`virtio_gpu.conf`):
```
name="virtio_gpu" parent="virtio" unit-address="0";
```

Module dependencies:
```make
LDFLAGS += -dy -Nmisc/drm -Nmisc/virtio
```

---

## Effort Summary

| Component | Lines | Status |
|-----------|-------|--------|
| **Phase 0 shims** | 520 | New (completion, print, managed, debugfs stub, rwsem) |
| **DRM core headless** | 5,450 | Port from Linux (drm_drv, gem, gem_shmem, ioctl, file, managed, print, mm) |
| **virtio-gpu headless** | 4,200 | Port from Linux (10 files) |
| **Phase 1 total** | **10,170** | Headless render node |
| **DRM KMS** | 9,100 | Port from Linux (atomic, crtc, connector, plane, fb, vblank, edid) |
| **virtio-gpu display** | 1,009 | Port from Linux (display.c + plane.c) |
| **Phase 2 total** | **10,109** | Full display |
| **Grand total** | **~20,280** | Complete virtio-gpu driver with display |

For comparison: porting amdgpu requires ~33,000 lines of shared infrastructure
alone, before touching the 2,618-file driver. virtio-gpu is **the minimum
viable DRM port** -- the smallest possible driver that exercises the full stack
from `modload` to pixels on screen.

---

## Testing Matrix

| Test | QEMU Flag | What It Validates |
|------|-----------|-------------------|
| PCI attach | `-device virtio-gpu-pci` | Driver loads, virtqueue init, feature negotiation |
| GETPARAM ioctl | Same | DRM ioctl dispatch, copyin/copyout |
| GEM create | Same | Memory allocation, DMA binding |
| 2D scanout | Same | Display pipeline, mode setting |
| virgl 3D | `-device virtio-gpu-gl-pci` | 3D command submission, fencing |
| venus Vulkan | `-device virtio-gpu-pci,venus=on` (crosvm) | Vulkan compute + rendering |
| Multi-display | `-device virtio-gpu-pci,max_outputs=2` | Multi-head display |

First milestone: `modload drm && modload virtio_gpu && ls /dev/dri/renderD128`
