# GPU Driver Comparison: i915 vs xe vs amdgpu vs nvidia-open vs nouveau

## Scale at a Glance

| Driver | .c files | .h files | Total | HW Scope | Memory Mgr | Scheduler |
|--------|----------|----------|-------|----------|------------|-----------|
| **i915** | 420 | 476 | 896 | Gen 2-12 (2002-2023) | GEM+shmem (int), TTM (discrete) | Custom (own) |
| **xe** | 190 | 323 | 513 | Gen 12+ (2020-future) | TTM + drm_buddy + drm_gpuvm | DRM sched |
| **amdgpu** | 941 | 1,677 | 2,618 | GCN 1-RDNA 4 (2012-current) | TTM + drm_buddy | DRM sched |
| **nvidia-open** | 1,462 | 2,246 | 3,708 | Turing+ (2018-current) | Own (RM) | Own |
| **nouveau** | 773 | 438 | 1,211 | NV04-Blackwell (all) | TTM | DRM sched |

---

## Shared Infrastructure Required

All five drivers need a common DRM/illumos foundation. Porting one driver
funds infrastructure for the others. Here's what's shared:

| Infrastructure | i915 | xe | amdgpu | nvidia-open | nouveau |
|----------------|------|----|--------|-------------|---------|
| dma_fence | Yes (52) | Yes (57) | Yes (74) | Yes (6) | Yes (11) |
| dma_resv | Yes (32) | Yes (18) | Yes (24) | Minimal | Yes |
| ww_mutex | Yes | Yes | Yes | No | Yes |
| TTM | Discrete only (38) | Yes (62) | Yes (43) | No | Yes (24) |
| DRM scheduler | **No (own)** | Yes (19) | Yes (48) | **No (own)** | Yes (3) |
| DRM atomic | Yes (48) | Minimal | Yes (13) | Yes | Yes |
| drm_buddy | Yes | Yes | Yes | No | No |
| drm_exec | No | Yes (23) | Yes | No | No |
| drm_gpuvm | No | Yes (14) | No | No | Yes (7) |
| Firmware | Yes (3) | Yes (48) | Yes (139) | Yes (6) | Yes (4) |
| Scatter-gather / DMA | Yes (54) | Yes (12) | Yes (11) | Yes (34) | Yes (18) |
| Workqueue | Yes (103) | Yes (64) | Yes (32) | Yes (14) | Yes (35) |
| debugfs | Yes (87) | Yes (36) | Yes (57) | Yes (6) | Yes (8) |
| sysfs | Yes (33) | Yes (39) | Yes (67) | Minimal | Minimal |
| mmu_notifier | Yes (3) | Yes (3) | Yes (5) | Yes (5) | Yes (3) |
| I2C | Yes (32) | Yes (9) | Yes | Yes | Yes |

**Key insight:** TTM + DRM scheduler are shared by **xe, amdgpu, and nouveau**.
Porting those once enables three drivers. i915 and nvidia-open have their own
memory and scheduler -- they benefit less from shared infrastructure but still
need dma_fence, DRM core, PCI, etc.

---

## Per-Driver Analysis

### Intel i915

**Pros for porting:**
- gfx-drm already has a (very old) i915 port -- patterns are understood
- Covers widest Intel HW range (Gen 2 through Meteor Lake)
- Integrated GPU = no VRAM management (simpler, no TTM for iGPU)

**Cons:**
- 896 files, ~15 years of accumulated code
- Custom scheduler (not DRM sched) -- complex, i915-specific
- GEM+shmem memory model depends on Linux `shmem_file_setup` / `struct page` -- the hardest Linux-ism to replicate
- Custom fence/request system (`i915_request`) layered on dma_fence
- 103 files use workqueue, 87 use debugfs -- huge surface area

**Unique APIs needed (beyond shared):**

| API | Purpose | illumos Approach | Est. Lines |
|-----|---------|-----------------|------------|
| shmem backing for GEM | Anonymous file-backed pages | `gfxp_alloc_from_mempool` or `ddi_umem_alloc` + pfnarray | ~500 |
| i915 custom scheduler | Request/execlists/GuC submission | Port as-is (it's driver-internal, not OS-specific) | ~0 (HW code) |
| `get_user_pages` | Pin userspace pages for GPU | `as_pagelock()` / `ddi_umem_lock()` | ~200 |
| `seqlock` | Lightweight read-mostly lock | Build from `atomic_cas` + sequence counter | ~80 |
| `srcu` | Sleepable RCU | Replace with `krwlock_t` (reader-heavy pattern) | ~50 |
| Intel GMBUS I2C | Display connector I2C | Bridge to illumos I2C framework | ~150 |

**Total unique effort:** ~980 lines beyond shared infrastructure

---

### Intel xe

**Pros for porting:**
- Clean-sheet design using modern DRM subsystems (TTM, drm_sched, drm_gpuvm, drm_exec)
- 513 files -- significantly smaller than i915/amdgpu
- If TTM + drm_sched are ported for amdgpu, xe gets them for free
- Clear subsystem boundaries, less spaghetti than i915
- Supports current and future Intel GPUs (Gen 12+)

**Cons:**
- 63 files use `pm_runtime` -- deeply embedded, harder to stub
- Mandatory GuC firmware (no fallback to direct HW programming)
- Uses drm_gpuvm and drm_gpusvm (2 more DRM subsystems to port)
- Newest Intel driver, less battle-tested than i915

**Unique APIs needed (beyond shared):**

| API | Purpose | illumos Approach | Est. Lines |
|-----|---------|-----------------|------------|
| drm_gpuvm | GPU virtual address space management | Port (self-contained DRM file, ~1000 lines) | ~1000 |
| drm_gpusvm | Shared virtual memory CPU↔GPU | Port or stub initially | ~800 or stub |
| `configfs` | Runtime config interface | Stub or use ioctl | ~50 (stub) |
| `hwmon` | Hardware monitoring (temp, power) | Map to kstat | ~200 |
| `shrinker` | Memory reclaim under pressure | Hook illumos ARC or use callback | ~150 |

**Total unique effort:** ~1200-2200 lines beyond shared infrastructure

---

### AMD amdgpu

**(Already analyzed in detail in AMDGPU_PORT_ANALYSIS.md)**

**Pros:**
- Largest AMD HW coverage (Tahiti through RDNA 4)
- Uses standard DRM subsystems (TTM, drm_sched) -- shared with xe/nouveau
- AMD DC display is largely OS-independent HW register code
- Massive community / corporate support upstream

**Cons:**
- 2,618 files -- largest driver by far
- AMD DC display alone is 1,070 files
- 311 PCI IDs, 11+ GPU generations, enormous variant matrix
- 67 sysfs files, 57 debugfs files -- huge monitoring surface

**Unique APIs needed (beyond shared):**

| API | Purpose | illumos Approach | Est. Lines |
|-----|---------|-----------------|------------|
| AMD-specific power mgmt (SMU) | GPU power/thermal control | Port PM subsystem (driver-internal) | ~0 (HW code) |
| `hwmon` | Temperature/fan/power reporting | Map to kstat | ~200 |
| `devcoredump` | GPU crash dumps | Stub initially | ~30 |
| `pci-p2pdma` | Peer-to-peer GPU DMA | Stub or defer | ~50 (stub) |

**Total unique effort:** ~280 lines beyond shared infrastructure (most complexity is in the shared TTM/scheduler stack)

---

### NVIDIA open-gpu-kernel-modules

**Pros for porting:**
- **OS abstraction layer already knows about Solaris** (`NV_SUNOS` defined in `cpuopsys.h`)
- Massive OS-agnostic core: 3,281 files in `src/` need ZERO changes
- Only `kernel-open/` (427 files) needs porting
- nvidia-drm module is only 19 .c files -- thin DRM wrapper
- Does NOT use TTM or DRM scheduler -- fewer DRM dependencies
- Core nvidia.ko interface (`os-interface.c`) is a well-defined porting target

**Cons:**
- nvidia-uvm (unified virtual memory) is 131K lines of deeply Linux-specific code (HMM, mmu_notifier, migrate.h) -- extremely hard to port
- Requires GSP firmware (GPU System Processor) -- binary firmware must run on-chip
- 186 unique Linux headers across the tree
- Proprietary GPU driver logic in `src/` -- no ability to debug/fix core GPU bugs
- Only supports Turing+ (2018+) -- no older GPUs

**Unique APIs needed (beyond shared):**

| API | Purpose | illumos Approach | Est. Lines |
|-----|---------|-----------------|------------|
| `nv-solaris.h` (os-interface impl) | Core OS abstraction (~40 functions) | Write from scratch using DDI | ~2000 |
| `nv_uvm_interface` | UVM kernel interface | Stub initially (lose CUDA unified memory) | ~100 (stub) |
| `os_alloc_mem` / `os_free_mem` | Memory allocation | `kmem_alloc` / `kmem_free` | included above |
| `os_map_kernel_space` | MMIO mapping | `gfxp_map_kernel_space` (exists!) | included above |
| `os_pci_init_handle` | PCI access | `pci_config_get/put` | included above |
| `os_io_*` | Port I/O | `ddi_io_get/put` or `inb/outb` | included above |
| `os_schedule` / `os_delay` | Timing | `drv_usecwait` / `cv_reltimedwait` | included above |
| `sync_file` support | dma_fence fd export | Port `sync_file.c` (~200 lines) | ~200 |

**Total unique effort:** ~2300 lines (dominated by `nv-solaris.h` os-interface implementation)

**Strategy note:** The nvidia-open approach is fundamentally different from the other
drivers. You don't port 3,708 files -- you write ONE file (`nv-solaris.h` /
`os-interface.c`) that implements ~40 OS abstraction functions, and the entire
3,281-file OS-agnostic core works unchanged. Then you port the thin DRM wrapper
(19 files) for display.

---

### nouveau (open-source NVIDIA)

**Pros:**
- Pure open-source, community maintained
- Supports ALL NVIDIA GPUs (NV04 through Blackwell)
- Uses standard DRM subsystems (TTM, drm_sched, drm_gpuvm) -- shared with xe/amdgpu
- If DRM+TTM+scheduler are ported, nouveau is "just another consumer"

**Cons:**
- 1,211 files, 100% Linux-native -- no OS abstraction layer
- Reverse-engineered for pre-Turing hardware (may be incomplete/buggy)
- For Turing+ needs GSP firmware (same as nvidia-open)
- Performance typically worse than nvidia-open for supported GPUs
- Heavy TTM dependency (24 files)

**Unique APIs needed (beyond shared):**

| API | Purpose | illumos Approach | Est. Lines |
|-----|---------|-----------------|------------|
| `nvkm` OS abstractions | Timer, memory, PCI, event | Write thin wrappers (~10 functions) | ~300 |
| `nvif` client interface | Kernel↔userspace channel | Port as-is (mostly protocol code) | ~0 |
| `nvbios` VBIOS parsing | Read GPU VBIOS | `gfxp_pci_read` for ROM, or `ddi_regs_map_setup` for ROM BAR | ~100 |

**Total unique effort:** ~400 lines beyond shared infrastructure

---

## Shared Infrastructure Cost (One-Time)

This is the foundation that benefits ALL drivers:

| Component | Est. Lines | Status | Needed By |
|-----------|-----------|--------|-----------|
| dma_fence + array + resv + ww_mutex + ref | 1,878 | **DONE** | All 5 |
| Completion | 120 | TODO | All 5 |
| Kernel Thread | 180 | TODO | All 5 |
| Firmware Loading | 100 | TODO | All 5 |
| Scatter-Gather + DMA Mapping | 400 | TODO | All 5 |
| GPU Page Pool | 600 | TODO | TTM users (xe, amdgpu, nouveau) |
| GPU Mmap / Fault | 350 | TODO | All 5 |
| DRM Managed | 200 | TODO | All 5 |
| DRM Print | 100 | TODO | All 5 |
| Sysfs → kstat | 300 | TODO | All 5 |
| Debugfs Stub | 80 | TODO | All 5 |
| xarray | 200 | TODO | xe, amdgpu |
| Interval Tree | 250 | TODO | xe, amdgpu, nouveau |
| PCI Extensions | 200 | TODO | All 5 |
| mmu_notifier Stub | 50 | TODO | Compute features |
| rwsem | 20 | TODO | All 5 |
| DRM Core Midlayer (20 files) | ~15,000 | TODO | All 5 |
| TTM (12 files) | ~4,500 | TODO | xe, amdgpu, nouveau |
| DRM Scheduler (3 files) | ~2,100 | TODO | xe, amdgpu, nouveau |
| drm_buddy (1 file) | ~500 | TODO | xe, amdgpu |
| drm_exec (1 file) | ~300 | TODO | xe, amdgpu |
| drm_syncobj (1 file) | ~1,200 | TODO | xe, amdgpu |
| **Shared total** | **~28,628** | | |

---

## Porting Strategy: Which Driver First?

### Option A: amdgpu First

**Rationale:** Largest HW coverage, uses standard DRM subsystems (TTM, drm_sched),
all shared infrastructure benefits xe and nouveau too.

| Phase | What | Enables |
|-------|------|---------|
| 1 | Shared infra (shims + DRM core + TTM + scheduler) | Foundation for 3 drivers |
| 2 | amdgpu headless | GPU memory + command submission |
| 3 | amdgpu display (AMD DC) | Full GPU driver |
| 4 | xe (nearly free -- shared infra) | Modern Intel GPUs |
| 5 | nouveau (nearly free -- shared infra) | All NVIDIA GPUs |

**Total new illumos code:** ~5,000 (shims) + ~28,000 (shared DRM/TTM) = ~33,000 lines
to get the first driver. Second and third drivers are incremental.

### Option B: nvidia-open First

**Rationale:** Smallest porting surface due to OS abstraction layer. Only ~2,300
lines of new code for the OS interface, plus DRM wrapper.

| Phase | What | Enables |
|-------|------|---------|
| 1 | Write `nv-solaris.h` (~2,000 lines) | nvidia.ko core |
| 2 | Minimal DRM core (drm_drv, drm_file, drm_ioctl, dma_fence) | nvidia-drm.ko |
| 3 | Port nvidia-drm (19 files) | Display output |
| 4 | Full DRM/TTM/scheduler for other drivers | amdgpu, xe, nouveau |

**Total new illumos code:** ~5,000 lines for basic nvidia. But this doesn't
fund infrastructure for other drivers. You'd still need ~28,000 lines for
DRM/TTM/scheduler when you want amdgpu or xe.

### Option C: xe First

**Rationale:** Cleanest architecture, smallest Intel driver (513 files),
uses all modern DRM subsystems. Clean-sheet code easier to port than
accumulated legacy.

Same phase structure as Option A but xe instead of amdgpu in phase 2.
xe is simpler but supports fewer GPUs (Gen 12+ only).

### Recommendation: Option A (amdgpu first)

1. Broadest hardware coverage (AMD has dominant discrete GPU market share)
2. All shared infrastructure benefits xe and nouveau
3. AMD DC display is ~90% OS-independent HW register code
4. After amdgpu, xe is nearly free
5. After xe, nouveau is nearly free
6. nvidia-open can be added independently (different arch, small unique effort)

---

## Per-Driver Unique Effort (After Shared Infrastructure)

| Driver | Unique Lines | Unique Difficulty | Total Files to Touch |
|--------|-------------|-------------------|---------------------|
| **amdgpu** | ~280 | Low (HW code) | ~10 OS-interface files |
| **xe** | ~1,200-2,200 | Medium (gpuvm/gpusvm) | ~15 OS-interface files |
| **i915** | ~980 | Medium-High (shmem, custom sched) | ~20 OS-interface files |
| **nvidia-open** | ~2,300 | Medium (os-interface from scratch) | 1 main file + 19 DRM |
| **nouveau** | ~400 | Low (uses shared infra) | ~5 OS-interface files |

The elephant in the room is the **~33,000 lines of shared infrastructure**
(shims + DRM core + TTM + scheduler). Once that's done, each individual
driver is a comparatively small incremental effort.
