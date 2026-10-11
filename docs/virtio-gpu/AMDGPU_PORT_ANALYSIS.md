# amdgpu Port Analysis: What's Needed for illumos

## Scale

| Component | .c files | .h files | Total | Size |
|-----------|----------|----------|-------|------|
| amdgpu (core driver) | 300 | 317 | 617 | 6.4 MB |
| display (AMD DC) | 487 | 583 | 1,070 | 12 MB |
| include (ASIC regs) | 0 | 543 | 543 | 81 MB |
| pm (power management) | 84 | 184 | 268 | 3.1 MB |
| amdkfd (HSA compute) | 42 | 20 | 62 | 841 KB |
| ras (error handling) | 13 | 41 | 54 | 396 KB |
| **Total** | **941** | **1,677** | **2,618** | **~102 MB** |

For comparison: the Linux 3.14 i915 driver that gfx-drm ported had **65 files**.
amdgpu is **40x larger**.

The driver supports **311 PCI device IDs** spanning Tahiti (2012) through RDNA 4 (current),
plus APU variants.

---

## Dependency Stack

The amdgpu driver sits on a deep stack of Linux subsystems. Each layer must
either be ported, shimmed, or stubbed. Here they are bottom-up:

```
┌──────────────────────────────────────────────────┐
│  amdgpu driver (941 .c files)                    │
├──────────────────────────────────────────────────┤
│  AMD Display Core (DC) -- 1,070 files            │
├──────────────────────────────────────────────────┤
│  DRM midlayer (drm_*, 53 headers used)           │
├──────────┬────────────┬──────────────────────────┤
│  TTM     │ DRM sched  │ DRM atomic / KMS         │
│  (memory)│ (jobs)     │ (display)                │
├──────────┴────────────┴──────────────────────────┤
│  dma_fence / dma_resv / ww_mutex   [DONE]        │
├──────────────────────────────────────────────────┤
│  Linux kernel primitives                         │
│  (PCI, IRQ, DMA mapping, firmware, workqueue,    │
│   timers, mm, shmem, sysfs, debugfs, ...)        │
├──────────────────────────────────────────────────┤
│  illumos DDI/DKI (kmutex, kcondvar, taskq,       │
│   ddi_dma, devmap, gfx_private, ...)             │
└──────────────────────────────────────────────────┘
```

---

## Subsystem-by-Subsystem Assessment

### Tier 1: MUST PORT (driver cannot function without these)

#### 1. dma_fence / dma_resv / ww_mutex -- STATUS: DONE

Already implemented as `drm_sun_*` shim files. 74 amdgpu files use `dma_fence`,
24 use `dma_resv`. This is the synchronization backbone.

#### 2. TTM (Translation Table Manager) -- 43 files depend on it

**What it does:** All buffer object (BO) lifecycle: allocate, free, move between
VRAM/GTT/system, evict under pressure, map to CPU, map to GPU page tables.

**Linux files:** 12 source files in `drivers/gpu/drm/ttm/`

| TTM file | Lines | illumos Challenge |
|----------|-------|-------------------|
| `ttm_device.c` | ~300 | Device init; depends on `drm_managed` |
| `ttm_bo.c` | ~1100 | BO core: validate/move/evict; depends on `dma_resv`, `ww_mutex` |
| `ttm_bo_util.c` | ~500 | BO helpers: kmap, memcpy move |
| `ttm_bo_vm.c` | ~400 | **mmap fault handler** -- deepest Linux-ism. Uses `vm_operations_struct`, `vm_fault`. Must map to illumos `devmap`/`segdev` |
| `ttm_tt.c` | ~400 | Page backing: uses `shmem_file_setup`, `shmem_read_mapping_page`. Must map to `gfxp_pmem` or similar |
| `ttm_pool.c` | ~700 | Page pool with DMA coherent alloc, huge pages, per-device pools. Heavily Linux mm-dependent |
| `ttm_resource.c` | ~400 | Resource tracking, LRU lists |
| `ttm_range_manager.c` | ~200 | Uses `drm_mm` (range allocator) |
| `ttm_sys_manager.c` | ~80 | Trivial system memory manager |
| `ttm_execbuf_util.c` | ~200 | ww_mutex locking helpers for multi-BO locking |
| `ttm_backup.c` | ~200 | Page swap for eviction |
| `ttm_agp_backend.c` | ~150 | Legacy AGP (probably skip) |

**Key illumos challenges:**
- `ttm_pool.c` -- Linux page allocation with `alloc_pages`, DMA32 zone, `dma_map_page`. Needs complete reimplementation using illumos `page_create`/`hat` or `ddi_dma_*`
- `ttm_bo_vm.c` -- Linux VFS mmap with fault handlers. Needs `devmap_callback_ctl` / `gfxp_devmap_umem_setup` (gfx-drm already has this pattern)
- `ttm_tt.c` -- shmem backing. Needs `gfxp_alloc_from_mempool` or direct page allocation

**Estimated effort:** HIGH -- this is the second-largest piece after display

#### 3. DRM GPU Scheduler -- 48 files depend on it

**What it does:** Job dispatch for every GPU engine (GFX, SDMA, VCN, JPEG).
amdgpu creates a `drm_gpu_scheduler` per ring. Every command submission,
buffer move, and page table update goes through the scheduler.

**Linux files:** 3 source files in `drivers/gpu/drm/scheduler/`

| Scheduler file | Lines | illumos Challenge |
|----------------|-------|-------------------|
| `sched_main.c` | ~1200 | Main scheduler loop: workqueue-based, uses `kthread`, `wait_queue`, `completion` |
| `sched_entity.c` | ~600 | Per-context entity management, priority |
| `sched_fence.c` | ~300 | Scheduler fences (extends `dma_fence`) |

**Dependencies:** `dma_fence` (DONE), `dma_resv` (DONE), workqueue (existing shim), `completion` (need to add: `kmutex_t` + `kcondvar_t`), `kthread` (need shim: use `thread_create()`)

**Estimated effort:** MEDIUM -- 3 files, well-contained, but threading model differs

#### 4. DRM Core Midlayer -- 53 DRM headers used

amdgpu uses these DRM core components:

| Component | Header | Files Using | Status |
|-----------|--------|-------------|--------|
| `drm_drv` | `drm_drv.h` | driver init | Need to port |
| `drm_device` | `drm_device.h` | everywhere | Need to port |
| `drm_file` | `drm_file.h` | ioctl/open/close | Need to port |
| `drm_ioctl` | `drm_ioctl.h` | command dispatch | Existing pattern in gfx-drm |
| `drm_gem` | `drm_gem.h` | GEM objects | Need update (per-object ops) |
| `drm_mm` | `drm_mm.h` | GTT allocator | Already ported in gfx-drm |
| `drm_buddy` | `drm_buddy.h` | VRAM allocator | Need to port (~500 lines, self-contained) |
| `drm_vblank` | `drm_vblank.h` | display timing | Need to port |
| `drm_exec` | `drm_exec.h` | multi-BO locking | Need to port (~300 lines) |
| `drm_syncobj` | `drm_syncobj.h` | userspace sync | Need to port |
| `drm_print` | `drm_print.h` | logging | Need to add (map to `cmn_err`) |
| `drm_managed` | `drm_managed.h` | lifecycle cleanup | Need to port |

#### 5. PCI Subsystem -- 42 files

**What amdgpu uses:** `pci_read/write_config`, `pci_resource_start/len`,
`pci_enable_device`, `pci_set_master`, `pci_find_capability`, `pci_enable_msi`,
P2P DMA, AER (Advanced Error Reporting), ATS.

**illumos status:** `drm_sun_pci.c` already wraps basic PCI access. Needs extension
for P2P DMA and possibly AER.

**Estimated effort:** LOW-MEDIUM -- existing shim covers 80%

#### 6. DMA Mapping -- 11 files

**What amdgpu uses:** `dma_map_page`, `dma_map_sg`, `dma_alloc_coherent`,
`sg_table`, `scatterlist`.

**illumos equivalent:** `ddi_dma_alloc_handle`, `ddi_dma_addr_bind_handle`,
`ddi_dma_mem_alloc`. The gfx-drm port already does this for GEM objects
via `pfnarray`. Need a general `sg_table` shim.

**Estimated effort:** MEDIUM -- need scatter-gather list abstraction

#### 7. Firmware Loading -- 4 files (but 139 reference firmware)

**What amdgpu uses:** `request_firmware()` to load GPU microcode (PSP, GFX,
SDMA, VCN, JPEG, MES). Firmware blobs live in `/lib/firmware/amdgpu/`.

**illumos equivalent:** No `request_firmware()`. Options:
- Implement `request_firmware()` shim using `firmware_open()`/`firmware_read()`
  from illumos `ddi_modopen()` or `kobj_open()/kobj_read()` on a file path
- Or link firmware statically into the module (not practical for amdgpu -- hundreds of blobs)

**Estimated effort:** MEDIUM -- need new shim, but it's a focused API

#### 8. IRQ -- centralized in `amdgpu_irq.c`

**What amdgpu uses:** `pci_irq_vector()`, MSI-X support, source-based IRQ
routing to internal handlers (one handler per IP block).

**illumos status:** `drm_sun_pci.c` has MSI support (`ddi_intr_alloc`).
The IRQ routing inside amdgpu is driver-internal and mostly portable.

**Estimated effort:** LOW -- existing shim + amdgpu's internal routing is HW-specific

#### 9. Workqueue -- 32 files

**illumos status:** `drm_sun_workqueue.c` already wraps `ddi_taskq`. May need
`delayed_work` and `flush_delayed_work` support (partially implemented).

**Estimated effort:** LOW -- extend existing shim

---

### Tier 2: NEEDED FOR DISPLAY (can defer if headless-first)

#### 10. Atomic Modesetting -- 13 files

**What it is:** The modern display commit model. Every CRTC, plane, and connector
has state objects committed atomically.

**Scale:** AMD DC (Display Core) is 1,070 files. It implements its own display
pipeline internally but needs the DRM atomic framework for the KMS interface.

**Key files to port:**
- `drm_atomic.c` (~800 lines)
- `drm_atomic_helper.c` (~3000 lines)
- `drm_atomic_state_helper.c` (~600 lines)
- `drm_atomic_uapi.c` (~1500 lines)
- Plus connector/plane/crtc infrastructure

**Estimated effort:** VERY HIGH -- this is the single largest DRM core piece

#### 11. Display Pipeline (AMD DC) -- 1,070 files

The good news: AMD DC is largely self-contained with its own hardware abstraction.
The bad news: it's 1,070 files and deeply integrated with DRM atomic.

DC internally manages:
- Timing controllers (OPTC) per generation
- Pixel pipes (DPP, HUBP, OPP, MPC)
- Link encoders (DP, HDMI, DSC)
- Clock managers per ASIC
- Display math library (DML) for bandwidth calculation
- DMUB firmware (display microcontroller)

Much of this is **hardware register programming** that is OS-independent.
The OS-dependent parts are the glue in `amdgpu_dm/` (~50 files).

#### 12. I2C -- via display connectors

**illumos status:** Robert's new I2C framework in illumos-gate replaces the
need for the old `drm_sun_i2c.c` bitbanger. AMD DC has its own I2C layer
(`dc/link/protocols/`) that would need to bridge to illumos I2C.

---

### Tier 3: OPTIONAL (stub or skip initially)

| Subsystem | Files | Why Skip |
|-----------|-------|----------|
| debugfs | 57+9 | No illumos equivalent. Stub to no-ops. Lose debug visibility. |
| sysfs/device_attribute | 67 | Map selectively to kstat. Most is power/thermal monitoring. |
| pm_runtime | 15 | Stub for always-on. No power savings but functional. |
| mmu_notifier / HMM | 15 | Only for KFD/compute (ROCm). Skip for display-only. |
| backlight | 76 | Laptop panels only. Skip for desktop/server. |
| fbdev/drm_client | 7 | Console framebuffer. Nice but not required. |
| ACPI / vga_switcheroo | 3 | Laptop hybrid GPU. Skip. |
| devcoredump | 1 | GPU crash dumps. Stub. |
| perf_events / PMU | 1 | Performance counters. Stub. |

---

## What We Have vs What We Need

### Already Done (from this session)

| Component | Status | Files |
|-----------|--------|-------|
| `dma_fence` | **DONE** | `drm_sun_dma_fence.c/.h` |
| `dma_fence_array` | **DONE** | `drm_sun_dma_fence_array.c/.h` |
| `dma_resv` | **DONE** | `drm_sun_dma_resv.c/.h` |
| `ww_mutex` | **DONE** | `drm_sun_ww_mutex.c/.h` |
| `drm_ref` (kref) | **DONE** | `drm_sun_ref.h` |
| PCI wrapper | **EXISTS** | `drm_sun_pci.c/.h` (needs extension) |
| Workqueue | **EXISTS** | `drm_sun_workqueue.c/.h` (needs extension) |
| Timer | **EXISTS** | `drm_sun_timer.c/.h` |
| IDR | **EXISTS** | `drm_sun_idr.c/.h` |
| `drm_mm` | **EXISTS** | Already in gfx-drm |
| Upstream sync | **DONE** | `tools/upstream-sync.sh` |

### New Shim Files Needed

| # | Component | Approx Lines | Priority | illumos Primitive |
|---|-----------|-------------|----------|-------------------|
| 1 | `drm_sun_completion.h` | ~50 | P0 | `kmutex_t` + `kcondvar_t` (one-shot completion) |
| 2 | `drm_sun_kthread.c/.h` | ~150 | P0 | `thread_create()` / `thread_exit()` |
| 3 | `drm_sun_firmware.c/.h` | ~200 | P0 | `kobj_open()` / `kobj_read()` for firmware blobs |
| 4 | `drm_sun_scatterlist.c/.h` | ~300 | P0 | `ddi_dma_*` + pfnarray for sg_table |
| 5 | `drm_sun_dma_mapping.c/.h` | ~200 | P0 | `ddi_dma_alloc_handle` / `ddi_dma_mem_alloc` |
| 6 | `drm_sun_sysfs.c/.h` | ~300 | P1 | `kstat_create()` / `kstat_install()` |
| 7 | `drm_sun_debugfs.c/.h` | ~100 | P1 | Stub to no-ops (or map to kstat) |
| 8 | `drm_sun_xarray.c/.h` | ~200 | P1 | AVL tree or hash table wrapper |
| 9 | `drm_sun_interval_tree.h` | ~100 | P1 | AVL tree with range queries |
| 10 | `drm_sun_mmu_notifier.h` | ~50 | P2 | Stub (needed for KFD compute) |

### DRM Core Files to Port

| # | File | Lines | What It Provides |
|---|------|-------|-----------------|
| 1 | `drm_drv.c` | ~1000 | Device registration (`devm_drm_dev_alloc`, `drm_dev_register`) |
| 2 | `drm_file.c` | ~500 | File open/close/ioctl dispatch |
| 3 | `drm_ioctl.c` | ~800 | Ioctl table, permission checking |
| 4 | `drm_gem.c` | ~1200 | GEM object management (needs per-object ops) |
| 5 | `drm_buddy.c` | ~500 | Buddy allocator (self-contained, easy port) |
| 6 | `drm_exec.c` | ~300 | Multi-BO locking helper |
| 7 | `drm_syncobj.c` | ~1200 | Userspace sync objects |
| 8 | `drm_managed.c` | ~250 | Lifecycle cleanup (devres-style) |
| 9 | `drm_print.c` | ~300 | Logging (`drm_dbg`/`drm_err` → `cmn_err`) |
| 10 | `drm_vblank.c` | ~600 | Vblank timing |
| 11 | `drm_mode_config.c` | ~500 | Mode configuration |
| 12 | `drm_connector.c` | ~800 | Connector management |
| 13 | `drm_crtc.c` | ~500 | CRTC management |
| 14 | `drm_plane.c` | ~400 | Plane management |
| 15 | `drm_framebuffer.c` | ~400 | Framebuffer management |
| 16 | `drm_property.c` | ~400 | Property system |
| 17 | `drm_atomic.c` | ~800 | Atomic state core |
| 18 | `drm_atomic_helper.c` | ~3000 | Atomic commit helpers |
| 19 | `drm_atomic_state_helper.c` | ~600 | State alloc/free helpers |
| 20 | `drm_atomic_uapi.c` | ~1500 | Atomic userspace API |

### TTM Files to Port

All 12 TTM source files (see Tier 1 item 2 above). Most complex: `ttm_pool.c`
(page allocation) and `ttm_bo_vm.c` (mmap fault handling).

### DRM Scheduler to Port

3 source files (see Tier 1 item 3 above).

---

## Recommended Port Strategy

### Phase 1: Headless Compute (no display)

Target: GPU memory allocation, command submission, firmware loading.
Skip: display, atomic modesetting, backlight, fbdev.

1. Extend shim layer (items 1-5 from "New Shim Files Needed")
2. Port DRM core midlayer (items 1-9 from "DRM Core Files to Port")
3. Port TTM (all 12 files)
4. Port DRM scheduler (3 files)
5. Port `drm_buddy.c` (1 file, easy)
6. Bring in amdgpu core (~300 .c files) via upstream-sync
7. Fix up amdgpu for illumos (attach/detach, IRQ, firmware paths)
8. Test: `modload`, PCI attach, firmware load, VRAM detect, ring init

### Phase 2: Display

1. Port DRM atomic core (items 17-20 from "DRM Core Files")
2. Port KMS objects (items 11-16)
3. Bring in AMD DC (1,070 files -- mostly HW register code, OS-independent)
4. Port `amdgpu_dm/` glue layer (~50 files)
5. Test: modeset, HDMI/DP output, resolution change

### Phase 3: Full Feature

1. sysfs → kstat for power/thermal monitoring
2. debugfs for debug visibility
3. KFD/compute (mmu_notifier, HMM)
4. Runtime PM
5. Multi-GPU (XGMI)

---

## Effort Estimate

| Component | New Code | Port/Adapt | Difficulty |
|-----------|----------|-----------|------------|
| Shim extensions (10 files) | ~1,650 lines | -- | Medium |
| DRM core midlayer (20 files) | -- | ~15,000 lines | High |
| TTM (12 files) | -- | ~4,500 lines | Very High |
| DRM scheduler (3 files) | -- | ~2,100 lines | Medium |
| drm_buddy (1 file) | -- | ~500 lines | Low |
| amdgpu core (300 files) | -- | ~150,000 lines | High (but mostly HW code) |
| AMD DC display (1,070 files) | -- | ~200,000 lines | Very High (but mostly HW code) |
| amdgpu_dm glue (50 files) | -- | ~20,000 lines | High |

The vast majority of amdgpu and AMD DC is **hardware register programming**
that is identical regardless of OS. The OS-dependent surface is concentrated in:
- TTM callbacks (`bo_driver`)
- `amdgpu_dm` display manager glue
- `amdgpu_drv.c` driver registration
- `amdgpu_irq.c` interrupt handling
- `amdgpu_ttm.c` memory management glue
- `amdgpu_gem.c` GEM ioctl handlers

These ~10 files are where 90% of the porting effort concentrates.
