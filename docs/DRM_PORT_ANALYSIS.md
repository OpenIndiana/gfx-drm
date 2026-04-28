# DRM Re-Port Analysis: Linux to illumos

## Executive Summary

The gfx-drm repository (OpenIndiana) contains a port of the Linux DRM subsystem from
**Linux kernel 3.14** (March 2014) to illumos/Solaris. The modern Linux kernel (7.0-rc5)
has undergone massive DRM restructuring over the intervening ~12 years. This document
analyzes the original porting patterns and evaluates what it would take to repeat them
on modern Linux DRM.

---

## 1. Source Identification

| Component | Source |
|-----------|--------|
| gfx-drm origin | Linux 3.14 DRM (March 2014) |
| Evidence | Haswell (Gen7) max GPU support, no Broadwell (Gen8), copyright dates through 2013, matching API surface |
| Modern Linux | 7.0-rc5 (~2026) |
| illumos-gate | Current master |

---

## 2. Original Port Architecture

### 2.1 Directory Mapping

| Linux 3.14 | gfx-drm (illumos) |
|------------|-------------------|
| `drivers/gpu/drm/*.c` | `usr/src/uts/common/io/drm/*.c` |
| `include/drm/*.h` | `usr/src/uts/common/drm/*.h` |
| `drivers/gpu/drm/i915/*.c` | `usr/src/uts/intel/io/i915/*.c` |
| `include/uapi/drm/*.h` | `usr/src/uts/common/drm/*.h` (merged) |

### 2.2 Illumos-Specific Shim Layer

The port created a set of shim files that translate Linux kernel APIs to illumos DDI/DKI:

| Shim File | Purpose |
|-----------|---------|
| `drm_linux.h` | Type mappings, locking primitives, memory allocation, wait queues |
| `drm_os_solaris.h` | OS abstraction (BUG_ON, WARN_ON, assertions) |
| `drm_sun_pci.c/.h` | PCI config access, BAR regions, `struct pci_dev` wrapper |
| `drm_sun_timer.c/.h` | Timer abstraction (`timeout_id_t` wrapping) |
| `drm_sun_workqueue.c/.h` | Workqueue via `ddi_taskq_t` |
| `drm_sun_idr.c/.h` | IDR via AVL trees (replacing Linux radix trees) |
| `drm_sun_i2c.c/.h` | Bit-banging I2C implementation |
| `drm_sunmod.c/.h` | DDI module entry points (cb_ops, dev_ops) |

### 2.3 Key API Translations

#### Locking (ALL map to `kmutex_t` -- no spin/sleep distinction)

| Linux | illumos |
|-------|---------|
| `spinlock_t` / `spin_lock(l)` | `kmutex_t` / `mutex_enter(l)` |
| `struct mutex` / `mutex_lock(l)` | `kmutex_t` / `mutex_enter(l)` |
| `spin_lock_irqsave(l, flags)` | `mutex_enter(l)` (flags ignored) |
| `assert_spin_locked(l)` | `ASSERT(MUTEX_HELD(l))` |

#### Memory

| Linux | illumos | Notes |
|-------|---------|-------|
| `kmalloc(size, GFP_KERNEL)` | `kmem_alloc(size, KM_SLEEP)` | |
| `kzalloc(size, flags)` | `kmem_zalloc(size, KM_SLEEP)` | |
| `kfree(ptr)` | `kmem_free(ptr, size)` | illumos kfree requires size! |
| `GFP_ATOMIC` | `KM_SLEEP` | WARNING: not truly atomic |
| `shmem_file_setup()` | `gfxp_alloc_from_mempool()` / DDI DMA | Fundamental GEM change |

#### Wait Queues

| Linux | illumos |
|-------|---------|
| `wait_queue_head_t` | `kcondvar_t` + `kmutex_t` |
| `init_waitqueue_head(&q)` | `cv_init()` + `mutex_init()` |
| `wake_up(&q)` | `cv_broadcast()` |
| `wait_event(q, cond)` | `cv_reltimedwait_sig()` loop |

#### Time

| Linux | illumos |
|-------|---------|
| `jiffies` | `ddi_get_lbolt()` |
| `msecs_to_jiffies(x)` | `drv_usectohz(x * 1000)` |
| `udelay(d)` | `drv_usecwait(d)` |
| `msleep(x)` | `drv_usecwait(x * 1000)` |

#### User/Kernel Copy

| Linux | illumos |
|-------|---------|
| `copy_to_user(dst, src, sz)` | `ddi_copyout(src, dst, sz, 0)` |
| `copy_from_user(dst, src, sz)` | `ddi_copyin(src, dst, sz, 0)` |

#### MMIO

| Linux | illumos |
|-------|---------|
| `ioread32(addr)` | `*(volatile uint32_t *)addr` via `DRM_READ32()` |
| `iowrite32(val, addr)` | `*(volatile uint32_t *)addr = val` via `DRM_WRITE32()` |
| `ioremap(base, sz)` | `drm_sun_ioremap(base, sz, DRM_MEM_UNCACHED)` |
| `ioremap_wc(base, sz)` | `drm_sun_ioremap(base, sz, DRM_MEM_WC)` |

#### IRQ

| Linux | illumos |
|-------|---------|
| `request_irq()` | `ddi_add_intr()` / `ddi_intr_add_handler()` (MSI) |
| `free_irq()` | `ddi_remove_intr()` / `ddi_intr_remove_handler()` |
| `IRQ_HANDLED` | `DDI_INTR_CLAIMED` |
| `IRQ_NONE` | `DDI_INTR_UNCLAIMED` |

#### PCI

| Linux | illumos |
|-------|---------|
| `pci_read_config_dword()` | `pci_config_get32()` |
| `pci_resource_start(pdev, bar)` | `pdev->regions[bar].start` (from DDI `assigned-addresses`) |
| `pci_enable_msi()` | `ddi_intr_alloc()` with `DDI_INTR_TYPE_MSI` |

#### Driver Lifecycle

| Linux | illumos |
|-------|---------|
| `module_init()` / `module_exit()` | `_init()` / `_fini()` / `_info()` |
| `pci_register_driver()` | `mod_install(&modlinkage)` |
| PCI `probe()` | `i915_attach(dip, DDI_ATTACH)` |
| PCI `remove()` | `i915_detach(dip, DDI_DETACH)` |
| `suspend()` / `resume()` | `DDI_SUSPEND` / `DDI_RESUME` in attach/detach |

#### List Macros (crucial difference)

| Linux | illumos |
|-------|---------|
| `list_for_each_entry(pos, head, member)` | `list_for_each_entry(pos, TYPE, head, member)` -- extra TYPE param (no `typeof()`) |
| `list_add(&entry->link, &head)` | `list_add(&entry->link, &head, (caddr_t)entry)` -- extra contain_ptr |

#### IOCTL Functions

| Linux | illumos |
|-------|---------|
| `int func(struct drm_device *dev, void *data, struct drm_file *fp)` | `int func(dev_t dev_id, struct drm_device *dev, void *data, struct drm_file *fp, int ioctl_mode, cred_t *credp)` |
| 3-arg `DRM_IOCTL_DEF()` | 5-arg with `copyin32` / `copyout32` for ILP32 compat |

### 2.4 Subsystems Completely Removed

- debugfs / sysfs / procfs
- DMA-BUF / PRIME buffer sharing
- Render nodes
- VGA arbitration / switcheroo
- ACPI opregion
- Runtime power management
- ftrace / tracing
- Linux module EXPORT_SYMBOL
- seq_file debugging
- DSI display output
- GPU error state capture (debugfs-dependent)

### 2.5 Subsystems Added for illumos

- FMA (Fault Management Architecture) integration
- kstat monitoring
- MDB (Modular Debugger) tracking lists
- devmap framework for userspace memory mapping
- Clone device minor management
- ILP32/LP64 multi-datamodel ioctl compat
- VGA text mode minor device
- AGP master integration
- `i915_quiesce()` for fast reboot

---

## 3. Modern Linux DRM Changes (3.14 -> 7.0)

### 3.1 Structural Revolution

The DRM subsystem went from ~40 core files to 90+ headers and dozens of new source files.
The i915 driver went from **~65 flat files** to **~600+ files across 6 subdirectories**
(`display/`, `gt/`, `gem/`, `gvt/`, `pxp/`, `selftests/`).

### 3.2 Major New Subsystems

| Subsystem | Description | Impact on Port |
|-----------|-------------|----------------|
| **Atomic Modesetting** | Complete replacement for legacy KMS | CRITICAL -- must implement or wrap |
| **DRM Scheduler** | GPU job scheduling framework | Important for modern drivers |
| **drm_managed** | devres-style cleanup (`drmm_*()`) | Needs illumos equivalent |
| **DRM Client** | In-kernel display client (fbdev emulation) | Moderate effort |
| **GPU SVM/VM** | GPU virtual memory management | Complex, GPU-dependent |
| **Buddy Allocator** | `drm_buddy` for VRAM management | Straightforward to port |
| **DRM Exec** | Multi-object locking helper | Needs mutex adaptation |
| **syncobj** | Userspace sync objects | Moderate, needs dma_fence port |
| **DRM Leasing** | Display resource partitioning | Low priority |

### 3.3 Removed Infrastructure (was in 3.14, gone in modern)

These are things the *original* gfx-drm port dealt with that NO LONGER EXIST:

- `drm_agpsupport.c` -- AGP support removed from DRM core
- `drm_bufs.c`, `drm_dma.c`, `drm_context.c`, `drm_lock.c`, `drm_scatter.c` -- legacy UMS/DMA
- `drm_irq.c` -- drivers now manage IRQs directly
- `drm_stub.c` -- merged into `drm_drv.c`
- `drm_vm.c` -- replaced by GEM mmap helpers
- `drm_platform.c` -- platform devices handle their own init
- Monolithic `drmP.h` -- split into ~20 specific headers

### 3.4 Driver Registration API Change

**3.14 (what gfx-drm targets):**
```c
drm_pci_init(&driver, &pci_driver);  // in module_init
driver.load = i915_driver_load;       // called from drm_get_dev
driver.unload = i915_driver_unload;
```

**Modern:**
```c
dev = devm_drm_dev_alloc(&pdev->dev, &driver, struct my_dev, drm);
drm_dev_register(dev, 0);
// load()/unload() are deprecated -- probe/remove do everything
```

### 3.5 GEM Object Model Change

**3.14:** Callbacks on `struct drm_driver` (global for all objects)
**Modern:** Per-object vtable `struct drm_gem_object_funcs` with `.free`, `.open`, `.close`, `.pin`, `.unpin`, `.vmap`, `.vunmap`, `.mmap`, `.export`, `.get_sg_table`, `.evict`, `.status`

### 3.6 Atomic Modesetting (biggest change)

3.14 used legacy modesetting: individual `.dpms()`, `.mode_set()`, `.mode_set_base()` callbacks.

Modern uses atomic: `struct drm_atomic_state` captures entire display state, committed atomically via `.atomic_check()` + `.atomic_commit()`. Every CRTC, plane, and connector has state objects.

### 3.7 i915 Decomposition

| 3.14 (flat) | Modern (structured) |
|-------------|-------------------|
| `intel_display.c` (11604 lines) | `display/` (343 files) |
| `intel_ringbuffer.c` (2219 lines) | `gt/` (164 files) |
| `i915_gem.c` + `i915_gem_*.c` | `gem/` (48 files) |
| `i915_drv.c` | `i915_driver.c` + extensive changes |
| No virtualization | `gvt/` (GPU virtualization) |
| No content protection | `pxp/` (23 files) |

---

## 4. Re-Port Strategy

### 4.1 Approach: Layer-by-Layer

The porting should be done in layers, matching the original port's strategy:

#### Layer 0: Updated Shim Layer (`drm_sun_*` files)

The existing shim files need updates for new Linux APIs:

| New Linux API | Needed illumos Shim |
|---------------|-------------------|
| `dma_fence` / `dma_resv` | New: fence abstraction over illumos sync primitives |
| `drm_managed` (`drmm_*`) | New: lifecycle-managed allocation tied to drm_device |
| `iosys_map` | New: unified iomem/sysmem mapping abstraction |
| `drm_print` (`drm_dbg/drm_err`) | New: logging wrappers (can map to `cmn_err()`) |
| `xarray` | New: replaces some IDR usage |
| `maple_tree` | New: advanced data structure (may need port) |
| `drm_exec` | New: multi-object locking (adapt to kmutex_t) |

The existing shims (PCI, timer, workqueue, IDR, I2C) remain valid but need expansion.

#### Layer 1: DRM Core

Port in this order:
1. `drm_drv.c` -- driver registration (adapt `devm_drm_dev_alloc` to DDI)
2. `drm_file.c` -- file ops (map to clone-open model)
3. `drm_ioctl.c` -- ioctl dispatch (add ILP32 compat)
4. `drm_gem.c` -- GEM core (keep gfxp_pmem/pfnarray approach)
5. `drm_mm.c` -- memory manager (mostly portable)
6. `drm_buddy.c` -- buddy allocator (new, mostly portable)
7. `drm_mode_config.c` + `drm_connector.c` + `drm_crtc.c` + `drm_encoder.c` + `drm_plane.c`
8. `drm_atomic.c` + `drm_atomic_helper.c` -- atomic modesetting
9. `drm_dp_helper.c` -- DisplayPort (mostly hardware protocol)
10. `drm_edid.c` -- EDID parsing (mostly portable)

Files to skip (no illumos equivalent / not needed):
- `drm_debugfs.c`, `drm_sysfs.c` -- no equivalent
- `drm_fbdev.c` / DRM client fbdev -- replace with illumos console
- `drm_panic.c` -- optional, nice-to-have later

#### Layer 2: i915 Driver (or xe)

**Decision point: port i915 or xe?**

- **i915**: Mature, supports Gen2-Gen12+, complex but well-understood
- **xe**: Newer, cleaner architecture, uses modern DRM patterns natively, but only supports newer GPUs (Gen12+/Xe)

For maximum hardware coverage, start with i915. The modern i915's subdirectory structure (`display/`, `gt/`, `gem/`) actually helps -- each can be ported somewhat independently.

Port order for i915:
1. `i915_driver.c` -- attach/detach (adapt from existing `i915_drv.c`)
2. `gem/` -- GEM subsystem (adapt existing gfxp_pmem approach)
3. `gt/` -- GPU engine/execution (mostly hardware programming)
4. `display/` -- modesetting (largest effort, needs atomic)

### 4.2 Estimated Complexity

| Component | Files (modern) | Port Difficulty | Existing gfx-drm Reuse |
|-----------|---------------|-----------------|----------------------|
| Shim layer updates | ~10 | Medium | 70% reusable |
| DRM core | ~40 | High | 40% patterns reusable |
| Atomic modesetting | ~15 | Very High | New (no precedent) |
| GEM core | ~8 | High | 60% approach reusable |
| i915 gem/ | ~48 | High | 30% reusable |
| i915 gt/ | ~164 | Medium-High | 20% reusable (HW code portable) |
| i915 display/ | ~343 | Very High | 10% reusable |
| i915 other | ~50 | Medium | Varies |

### 4.3 What Makes This Hard

1. **Atomic modesetting**: Completely new paradigm with no precedent in gfx-drm. Every display operation needs state tracking, check/commit phases, and fence synchronization.

2. **`dma_fence` / `dma_resv`**: Modern DRM uses fence-based synchronization pervasively. Needs a complete implementation for illumos (could build on `kcondvar_t` + atomic ops).

3. **GEM VMA management**: Modern i915 uses per-process virtual address spaces (full PPGTT) extensively. The old port used a single global GTT.

4. **Scale**: Modern i915 is ~10x the code of the 3.14 version (600+ files vs 65).

5. **GuC/HuC firmware**: Modern i915 requires firmware loading for the GPU's internal microcontrollers. Needs illumos firmware loading infrastructure.

6. **Memory model**: Linux's `struct page` + scatterlist + DMA mapping API is deeply embedded. The gfxp_pmem/pfnarray approach needs significant extension.

### 4.4 What Makes This Feasible

1. **The shim layer pattern is proven**: The `drm_sun_*` approach successfully abstracted Linux kernel APIs once and can do so again.

2. **Hardware programming is unchanged**: Register writes, PTE encoding, ring buffer management, display timings -- these are identical regardless of OS. This is the bulk of i915 code.

3. **Atomic can be incremental**: Start with legacy modesetting helpers (still present in modern DRM) and add atomic later.

4. **illumos-gate has evolved**: Modern illumos may have better infrastructure for some of the needed primitives.

5. **DRM core is more modular now**: The split into many small files actually makes selective porting easier.

---

## 5. Concrete Next Steps

### Phase 1: Foundation (Shim Layer)
- Update `drm_linux.h` for new types (`dma_fence`, `iosys_map`, etc.)
- Implement `dma_fence` / `dma_resv` for illumos (NEW -- no existing illumos equivalent)
- Implement `drm_managed` lifecycle helpers (NEW -- no existing illumos equivalent)
- Update `drm_sun_pci.c` for modern PCI patterns
- Port `drm_print.c` logging framework
- Replace old `drm_sun_i2c.c` bit-banging with new illumos I2C framework (see below)

> **NOTE on illumos-gate infrastructure (2025-2026):**
> Robert Mustacchi (Oxide) has contributed significant new infrastructure to
> illumos-gate that is directly relevant:
>
> - **New I2C/SMBus Framework** (bug 17659-17673): A complete, modern I2C nexus
>   with controller/client/mux abstractions (`<sys/i2c/client.h>`,
>   `<sys/i2c/controller.h>`, `<sys/i2c/mux.h>`), Intel SMBus controller drivers,
>   proper bus locking, register-based I/O, and DDC/EDID-capable transfers.
>   This replaces the need for gfx-drm's custom `drm_sun_i2c.c` bit-banging.
>
> - **GPIO Framework** (bug 17670): Kernel GPIO with PCA953x drivers.
>   Relevant for display hot-plug detection and panel control.
>
> - **PCIe enhancements** (bug 17804-17806): BAR management fixes, config
>   space offset support, DOE decoding -- relevant for GPU PCI access.
>
> However, the following are **confirmed NOT present** in illumos-gate:
> - `dma_fence` / `dma_resv` / `dma_buf` -- must be implemented from scratch
> - `drm_managed` (`drmm_*`) -- must be implemented from scratch
> - Any DRM core code -- must be ported entirely
> - The `gfx_private.h` interface (devmap, gfxp_pmem, kernel space mapping)
>   is still the 2007 Sun-era code and may need updates

### Phase 1.5: dma_fence Design
See `illumos_dma_fence_design.h` for the complete design with implementation
notes. Key design decisions documented there.

### Phase 2: DRM Core
- Port modern `drm_drv.c` with illumos DDI registration
- Port `drm_file.c` with clone-open model
- Port `drm_gem.c` using gfxp_pmem approach
- Port `drm_mm.c` and `drm_buddy.c`
- Port atomic modesetting core (start with legacy compat helpers)

### Phase 3: i915 Driver
- Port `i915_driver.c` (attach/detach)
- Port `gem/` subsystem
- Port `gt/` subsystem (engine init, ring buffers, submission)
- Port `display/` subsystem (start with HDMI/DP, add features incrementally)

### Phase 4: Validation
- Boot with VGA console -> DRM handoff
- Basic modesetting (resolution change)
- GEM object allocation/mapping
- GPU command submission
- Full X11/Wayland compositor

---

## 6. Repository Layout

```
/home/toasty/ws/illumos/
  illumos-gate-current/     # Fresh illumos-gate master
  gfx-drm/                  # OpenIndiana gfx-drm (port from Linux 3.14)
  linux-3.14/               # Original Linux source matching gfx-drm
  linux-modern/             # Modern Linux 7.0-rc5
  drm/                      # This working directory (new port)
```
