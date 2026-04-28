# illumos-Native API Estimates for amdgpu Port

For each API amdgpu needs, this document describes how to write it as proper
illumos-gate-quality code -- not a Linux shim but something that reads like
it was always part of illumos.

Each entry has: what it replaces, the illumos primitives it uses, the API
surface, the estimated lines of new code, and a difficulty rating.

---

## Already Complete

| # | API | Status | Lines |
|---|-----|--------|-------|
| A | `dma_fence` | DONE (`drm_sun_dma_fence.c/.h`) | 636 |
| B | `dma_fence_array` | DONE (`drm_sun_dma_fence_array.c/.h`) | 319 |
| C | `dma_resv` | DONE (`drm_sun_dma_resv.c/.h`) | 491 |
| D | `ww_mutex` | DONE (`drm_sun_ww_mutex.c/.h`) | 339 |
| E | `drm_ref` (kref) | DONE (`drm_sun_ref.h`) | 93 |
| F | PCI wrapper | EXISTS (`drm_sun_pci.c/.h`) | 303 |
| G | Workqueue | EXISTS (`drm_sun_workqueue.c/.h`) | 146 |
| H | Timer | EXISTS (`drm_sun_timer.c/.h`) | 146 |
| I | IDR | EXISTS (`drm_sun_idr.c/.h`) | 578 |
| J | `drm_mm` | EXISTS in gfx-drm | ~800 |

---

## New APIs Needed

### 1. Completion (`struct completion` replacement)

**Replaces:** Linux `<linux/completion.h>` -- `init_completion`, `complete`, `complete_all`, `wait_for_completion`, `wait_for_completion_timeout`, `wait_for_completion_interruptible`, `wait_for_completion_interruptible_timeout`, `try_wait_for_completion`, `completion_done`

**illumos primitives:** `kmutex_t`, `kcondvar_t`, `cv_broadcast`, `cv_reltimedwait_sig`

**API:**
```c
typedef struct drm_completion {
    kmutex_t    dc_lock;
    kcondvar_t  dc_cv;
    boolean_t   dc_done;
    uint32_t    dc_count;    /* for re-init / multi-complete */
} drm_completion_t;

void drm_completion_init(drm_completion_t *);
void drm_completion_destroy(drm_completion_t *);
void drm_complete(drm_completion_t *);           /* signal one waiter */
void drm_complete_all(drm_completion_t *);       /* signal all waiters */
int  drm_wait_for_completion(drm_completion_t *);
int  drm_wait_for_completion_timeout(drm_completion_t *, hrtime_t ns);
int  drm_wait_for_completion_interruptible(drm_completion_t *);
int  drm_wait_for_completion_interruptible_timeout(drm_completion_t *, hrtime_t ns);
boolean_t drm_completion_done(drm_completion_t *);
void drm_reinit_completion(drm_completion_t *);
```

**Files:** `drm_sun_completion.h` (header-only, all inline or with 1 small .c)

| | |
|-|-|
| Lines | ~120 |
| Difficulty | Easy |
| Used by | DRM scheduler (heavily), amdgpu ring init, PSP firmware load |
| Pattern | `mutex_enter; done=B_TRUE; cv_broadcast; mutex_exit` / `mutex_enter; while(!done) cv_wait; mutex_exit` |

---

### 2. Kernel Thread Wrapper

**Replaces:** Linux `<linux/kthread.h>` -- `kthread_run`, `kthread_create`, `kthread_stop`, `kthread_should_stop`, `kthread_park`, `kthread_unpark`

**illumos primitives:** `thread_create()` (in `<sys/thread.h>`), `thread_exit()`, `cv_wait_sig()` for stop notification

**API:**
```c
typedef struct drm_kthread {
    kthread_t       *dk_thread;
    kmutex_t        dk_lock;
    kcondvar_t      dk_cv;
    volatile boolean_t dk_should_stop;
    volatile boolean_t dk_exited;
    int             (*dk_func)(void *);
    void            *dk_data;
    char            dk_name[32];
} drm_kthread_t;

drm_kthread_t *drm_kthread_run(int (*func)(void *), void *data, const char *name);
int  drm_kthread_stop(drm_kthread_t *);   /* signals stop, waits for exit */
boolean_t drm_kthread_should_stop(drm_kthread_t *);
void drm_kthread_destroy(drm_kthread_t *);
```

**Implementation:** `drm_kthread_run` calls `thread_create(NULL, 0, wrapper_fn, kt, 0, &p0, TS_RUN, minclsyspri)`. The wrapper calls `kt->dk_func(kt->dk_data)`, then sets `dk_exited=B_TRUE` and `cv_broadcast`. `drm_kthread_stop` sets `dk_should_stop=B_TRUE`, broadcasts, then waits on `dk_exited` via `cv_wait`. The thread checks `dk_should_stop` via `drm_kthread_should_stop()`.

**Files:** `drm_sun_kthread.c` + `drm_sun_kthread.h`

| | |
|-|-|
| Lines | ~180 |
| Difficulty | Easy-Medium |
| Used by | DRM scheduler main loop, amdgpu reset handler, GPU recovery threads |
| Note | `thread_create` args differ from Linux: stack is auto-allocated, priority model is different (use `minclsyspri` for normal, `maxclsyspri` for realtime) |

---

### 3. Firmware Loading

**Replaces:** Linux `<linux/firmware.h>` -- `request_firmware`, `release_firmware`, `struct firmware { size_t size; const u8 *data; }`

**illumos primitives:** `firmware_open()`, `firmware_read()`, `firmware_close()`, `firmware_get_size()` from `<sys/firmload.h>`. Already used by `iwn(4D)` and `cxgbe(4D)` drivers.

**API:**
```c
typedef struct drm_firmware {
    size_t     fw_size;
    uint8_t    *fw_data;     /* kmem_alloc'd copy */
} drm_firmware_t;

int  drm_request_firmware(drm_firmware_t **fwp, const char *name,
         dev_info_t *dip);
void drm_release_firmware(drm_firmware_t *fw);
```

**Implementation:** `drm_request_firmware` calls `firmware_open(ddi_driver_name(dip), name, &fh)`, reads `firmware_get_size(fh)`, allocates `kmem_alloc(size, KM_SLEEP)`, calls `firmware_read(fh, 0, buf, size)`, closes handle.

Firmware files go in `/kernel/firmware/amdgpu/` (or `/usr/lib/firmware/amdgpu/` with module path configured).

**Files:** `drm_sun_firmware.c` + `drm_sun_firmware.h`

| | |
|-|-|
| Lines | ~100 |
| Difficulty | Easy |
| Used by | amdgpu PSP, GFX, SDMA, VCN, JPEG, MES microcode loading (139 files reference firmware) |
| Note | `firmware_open` searches the kernel module path. Firmware blobs must be installed there. Async loading (`request_firmware_nowait`) not needed -- amdgpu loads synchronously at attach time |

---

### 4. Scatter-Gather List / DMA Mapping

**Replaces:** Linux `<linux/scatterlist.h>` -- `struct scatterlist`, `struct sg_table`, `sg_alloc_table`, `sg_free_table`, `for_each_sg`, `sg_dma_address`, `sg_dma_len`. Also `<linux/dma-mapping.h>` -- `dma_map_sg`, `dma_unmap_sg`, `dma_map_page`, `dma_alloc_coherent`, `dma_free_coherent`

**illumos primitives:** `ddi_dma_alloc_handle()`, `ddi_dma_mem_alloc()`, `ddi_dma_addr_bind_handle()`, `ddi_dma_cookie_iter()`, `ddi_dma_ncookies()`, `ddi_dma_sync()` from `<sys/sunddi.h>` and `<sys/ddidmareq.h>`

**API:**
```c
/*
 * A scatter-gather entry. Wraps one DMA cookie.
 */
typedef struct drm_sg_entry {
    uint64_t    dse_dma_addr;    /* bus address */
    size_t      dse_length;      /* segment length */
} drm_sg_entry_t;

/*
 * A scatter-gather table. Built from DDI DMA cookies.
 */
typedef struct drm_sg_table {
    uint32_t        dst_nents;       /* number of entries */
    uint32_t        dst_max_ents;
    drm_sg_entry_t  *dst_entries;    /* kmem_alloc'd array */
    ddi_dma_handle_t dst_dma_hdl;    /* backing DMA handle */
} drm_sg_table_t;

int  drm_sg_alloc_table(drm_sg_table_t *, uint32_t nents);
void drm_sg_free_table(drm_sg_table_t *);

/*
 * DMA mapping. Wraps DDI DMA operations.
 */
int  drm_dma_map_sg(dev_info_t *dip, ddi_dma_attr_t *attr,
         drm_sg_table_t *sgt, caddr_t kaddr, size_t len, uint_t dir);
void drm_dma_unmap_sg(drm_sg_table_t *sgt);

/* Coherent allocation (for ring buffers, descriptors) */
int  drm_dma_alloc_coherent(dev_info_t *dip, ddi_dma_attr_t *attr,
         size_t size, caddr_t *kvap, uint64_t *dma_addrp,
         ddi_dma_handle_t *hdlp, ddi_acc_handle_t *accp);
void drm_dma_free_coherent(caddr_t kva, size_t size,
         ddi_dma_handle_t hdl, ddi_acc_handle_t acc);

/* Sync */
int  drm_dma_sync(ddi_dma_handle_t hdl, off_t off, size_t len, uint_t dir);
```

**Implementation:**
- `drm_dma_map_sg`: calls `ddi_dma_alloc_handle`, `ddi_dma_addr_bind_handle`, then iterates cookies with `ddi_dma_cookie_iter` to populate `drm_sg_entry_t` array
- `drm_dma_alloc_coherent`: calls `ddi_dma_alloc_handle` + `ddi_dma_mem_alloc` + `ddi_dma_addr_bind_handle` (standard DDI DMA pattern, used by every NIC driver)
- `drm_dma_sync`: thin wrapper around `ddi_dma_sync`

**Files:** `drm_sun_scatterlist.c` + `drm_sun_scatterlist.h` (combined SG + DMA mapping)

| | |
|-|-|
| Lines | ~400 |
| Difficulty | Medium |
| Used by | TTM page pool, GART binding, amdgpu VRAM↔GTT moves, ring buffers |
| Note | The main complexity is mapping Linux's `dma_direction_t` (TO_DEVICE, FROM_DEVICE, BIDIRECTIONAL) to DDI DMA flags (`DDI_DMA_READ`, `DDI_DMA_WRITE`, `DDI_DMA_RDWR`). The `ddi_dma_attr_t` must be set up correctly for GPU constraints (64-bit addressing, large max transfer, alignment requirements) |

---

### 5. GPU Page Pool (TTM Pool Replacement)

**Replaces:** Linux `ttm_pool.c` -- DMA-coherent page allocation with caching modes, huge page support, per-device pools, page recycling

**illumos primitives:** `page_create_va()`, `hat_memload()`, `ddi_dma_mem_alloc()`, `gfxp_alloc_from_mempool()` from `<sys/gfx_private.h>`, `ddi_umem_alloc()`

**API:**
```c
typedef struct drm_gpu_pool {
    dev_info_t      *dgp_dip;
    kmutex_t        dgp_lock;
    list_t          dgp_free_wc;     /* free write-combining pages */
    list_t          dgp_free_uc;     /* free uncached pages */
    list_t          dgp_free_cached; /* free cached pages */
    uint64_t        dgp_alloc_count;
    uint64_t        dgp_free_count;
    ddi_dma_attr_t  dgp_dma_attr;    /* GPU DMA constraints */
} drm_gpu_pool_t;

typedef struct drm_gpu_page {
    list_node_t     dgpg_node;
    caddr_t         dgpg_kva;        /* kernel virtual address */
    uint64_t        dgpg_dma_addr;   /* bus address */
    pfn_t           dgpg_pfn;        /* physical frame number */
    size_t          dgpg_size;       /* PAGE_SIZE or large page */
    uint_t          dgpg_cache_mode; /* WC, UC, or cached */
    ddi_dma_handle_t dgpg_dma_hdl;
    ddi_acc_handle_t dgpg_acc_hdl;
} drm_gpu_page_t;

void drm_gpu_pool_init(drm_gpu_pool_t *, dev_info_t *, ddi_dma_attr_t *);
void drm_gpu_pool_fini(drm_gpu_pool_t *);
int  drm_gpu_pool_alloc_pages(drm_gpu_pool_t *, uint32_t npages,
         uint_t cache_mode, drm_gpu_page_t **pages_out);
void drm_gpu_pool_free_pages(drm_gpu_pool_t *, drm_gpu_page_t **pages,
         uint32_t npages);
void drm_gpu_pool_shrink(drm_gpu_pool_t *, uint64_t target_pages);
```

**Implementation:**
- Allocate pages via `ddi_dma_mem_alloc()` with appropriate cache attributes (`IOMEM_DATA_UC_WR_COMBINE` for WC, `IOMEM_DATA_UNCACHED` for UC, `IOMEM_DATA_CACHED` for cached)
- Keep free lists per cache mode for recycling (avoid repeated DMA handle setup)
- `hat_getpfnum(kas.a_hat, kva)` to get PFN for GART programming
- For large pages: `gfxp_alloc_from_mempool()` can provide contiguous physical memory
- Shrink callback for memory pressure (hook into illumos `segkp` or arc shrinker mechanism -- or just shrink on explicit request)

**Files:** `drm_sun_gpu_pool.c` + `drm_sun_gpu_pool.h`

| | |
|-|-|
| Lines | ~600 |
| Difficulty | High |
| Used by | TTM (replaces `ttm_pool.c`), backing store for all GPU buffer objects |
| Note | This is the deepest illumos-specific code. Linux uses `alloc_pages` with GFP flags and `set_pages_uc/wc` to change cache attributes. illumos allocates pages with cache attributes set at allocation time via `ddi_dma_mem_alloc`. The free-list recycling is critical for performance -- TTM's pool exists specifically to avoid the cost of DMA handle setup/teardown on every allocation |

---

### 6. GPU Mmap / Fault Handler (TTM VM Replacement)

**Replaces:** Linux `ttm_bo_vm.c` -- `vm_operations_struct` with `.fault` handler, `vm_insert_pfn`, `vmf_insert_pfn`

**illumos primitives:** `devmap_callback_ctl` with `devmap_access` callback, `devmap_load()`, `devmap_unload()`, `gfxp_devmap_umem_setup()` from `<sys/ddidevmap.h>` and `<sys/gfx_private.h>`

**API:**
```c
/*
 * devmap(9E) entry point for the DRM driver's cb_ops.
 * Called by the kernel when userspace mmaps a DRM device offset.
 */
int drm_gem_devmap(dev_t dev, devmap_cookie_t dhp, offset_t off,
    size_t len, size_t *maplen, uint_t model);

/*
 * devmap_access callback -- called on page fault.
 * Resolves the GPU object, pins it, loads the translation.
 */
int drm_gem_devmap_access(devmap_cookie_t dhp, void *pvtp,
    offset_t off, size_t len, uint_t type, uint_t rw);

/*
 * devmap_unmap callback -- called when mapping is destroyed.
 * Unpins the GPU object.
 */
void drm_gem_devmap_unmap(devmap_cookie_t dhp, void *pvtp,
    offset_t off, size_t len, devmap_cookie_t new_dhp1,
    void **new_pvtp1, devmap_cookie_t new_dhp2, void **new_pvtp2);
```

**Implementation:**
- `drm_gem_devmap`: Looks up the buffer object from the offset, sets up `devmap_umem_setup` or `gfxp_devmap_umem_setup` with `DEVMAP_MAPPING_INVALID` flag (all accesses fault)
- `drm_gem_devmap_access`: Pins the BO (ensuring it's in VRAM or GTT), gets the PFN or bus address, calls `devmap_load()` to establish the page translation. For VRAM: uses the PCI BAR aperture. For GTT: uses the GART-mapped DMA address.
- On eviction/migration: driver calls `devmap_unload()` to invalidate, next access re-faults

**Files:** `drm_sun_bo_vm.c` + `drm_sun_bo_vm.h`

| | |
|-|-|
| Lines | ~350 |
| Difficulty | High |
| Used by | Every userspace GPU memory access (Mesa, Vulkan, X11, Wayland) |
| Note | The gfx-drm i915 port already has this exact pattern in `drm_gem.c` (`drm_gem_map_ops` with `devmap_map`, `devmap_access`, `devmap_dup`, `devmap_unmap` callbacks and `devmap_load`/`devmap_unload`). The new implementation adapts it for TTM's multi-domain model (VRAM vs GTT vs system memory, with migration on fault) |

---

### 7. DRM Managed Resources

**Replaces:** Linux `drm_managed.c` / `<drm/drm_managed.h>` -- `drmm_add_action`, `drmm_add_final_kfree`, `drmm_kmalloc`, `drmm_kzalloc`, `drmm_kfree`, `drmm_mutex_init`

**illumos primitives:** `list_t`, `kmem_alloc/free`, `kmutex_t`

**API:**
```c
typedef void (*drm_managed_func_t)(void *arg);

typedef struct drm_managed_entry {
    list_node_t     dme_node;
    drm_managed_func_t dme_func;
    void            *dme_arg;
    size_t          dme_size;    /* for drmm_kzalloc allocations */
} drm_managed_entry_t;

/* Attach to a drm_device (or a structure with a list_t for managed entries) */
int   drmm_add_action(void *dev, drm_managed_func_t func, void *arg);
void  drmm_add_final_kfree(void *dev, void *ptr, size_t size);
void *drmm_kzalloc(void *dev, size_t size);
void  drmm_kfree(void *dev, void *ptr);
void  drmm_cleanup_all(void *dev);   /* called on device teardown */
```

**Implementation:** Maintains a `list_t` of cleanup actions on the device struct. `drmm_cleanup_all()` walks the list in reverse order (LIFO), calling each cleanup function. `drmm_kzalloc` allocates memory and registers a `kmem_free` cleanup. This is conceptually identical to Linux's `devm_*` / devres pattern.

**Files:** `drm_sun_managed.c` + `drm_sun_managed.h`

| | |
|-|-|
| Lines | ~200 |
| Difficulty | Easy |
| Used by | DRM device init, connector/encoder/crtc creation, every modern DRM driver |
| Note | The key insight is that this is just a destructor list. On device detach, walk the list and call all cleanup functions. Replaces the need for error-path `goto` chains in init code |

---

### 8. DRM Print / Logging

**Replaces:** Linux `drm_print.c` / `<drm/drm_print.h>` -- `drm_dbg`, `drm_info`, `drm_warn`, `drm_err`, `drm_dbg_kms`, `drm_dbg_driver`, `DRM_DEV_DEBUG`, category-based debug filtering

**illumos primitives:** `cmn_err()` (`CE_NOTE`, `CE_WARN`, `CE_CONT`), `dev_err()` from `<sys/cmn_err.h>` and `<sys/sunddi.h>`

**API:**
```c
/* Debug categories (bitfield) */
#define DRM_DBG_CORE    (1 << 0)
#define DRM_DBG_DRIVER  (1 << 1)
#define DRM_DBG_KMS     (1 << 2)
#define DRM_DBG_ATOMIC  (1 << 3)
#define DRM_DBG_VBL     (1 << 4)
#define DRM_DBG_DP      (1 << 5)

extern uint32_t drm_debug_mask;  /* tunable, set via /etc/system or ioctl */

#define drm_dbg(dev, cat, fmt, ...) do {                        \
    if (drm_debug_mask & (cat))                                 \
        dev_err((dev)->devinfo, CE_CONT, "?[drm] " fmt,        \
            ##__VA_ARGS__);                                     \
} while (0)

#define drm_info(dev, fmt, ...)                                 \
    dev_err((dev)->devinfo, CE_NOTE, "[drm] " fmt, ##__VA_ARGS__)

#define drm_warn(dev, fmt, ...)                                 \
    dev_err((dev)->devinfo, CE_WARN, "[drm] " fmt, ##__VA_ARGS__)

#define drm_err(dev, fmt, ...)                                  \
    dev_err((dev)->devinfo, CE_WARN, "![drm:ERROR] " fmt,      \
        ##__VA_ARGS__)
```

**Files:** `drm_sun_print.h` (mostly macros, maybe small .c for `drm_debug_mask` global)

| | |
|-|-|
| Lines | ~100 |
| Difficulty | Easy |
| Used by | Every file in every DRM driver |
| Note | The `?` prefix in `CE_CONT` format strings makes the message only go to `/var/adm/messages` (not console). The `!` prefix forces console output. `drm_debug_mask` can be set via `/etc/system` with `set drm:drm_debug_mask=0xff` for debugging |

---

### 9. Sysfs → kstat

**Replaces:** Linux `sysfs` / `<linux/device.h>` -- `device_attribute`, `DEVICE_ATTR_RO`, `DEVICE_ATTR_RW`, `sysfs_create_group`

**illumos primitives:** `kstat_create()`, `kstat_named_t`, `kstat_install()`, `kstat_delete()` from `<sys/kstat.h>`

**API:**
```c
/*
 * Create a named kstat group for a GPU device.
 * Each group maps to one Linux sysfs directory of attributes.
 */
typedef struct drm_kstat_group {
    kstat_t         *dkg_kstat;
    kstat_named_t   *dkg_entries;
    uint32_t        dkg_nentries;
    char            dkg_name[KSTAT_STRLEN];
    /* Optional update callback (called before kstat is read) */
    int             (*dkg_update)(struct drm_kstat_group *, int rw);
    void            *dkg_private;
} drm_kstat_group_t;

int  drm_kstat_create_group(dev_info_t *dip, const char *group_name,
         uint32_t nentries, drm_kstat_group_t **grpp);
void drm_kstat_add_u64(drm_kstat_group_t *, const char *name, uint64_t *valp);
void drm_kstat_add_u32(drm_kstat_group_t *, const char *name, uint32_t *valp);
void drm_kstat_add_string(drm_kstat_group_t *, const char *name, char *str);
void drm_kstat_install(drm_kstat_group_t *);
void drm_kstat_destroy(drm_kstat_group_t *);
```

**Example usage** (replacing amdgpu sysfs):
```c
/* Linux: /sys/class/drm/card0/device/gpu_busy_percent */
/* illumos: kstat -m amdgpu -i 0 -n gpu_info -s gpu_busy_percent */

drm_kstat_create_group(dip, "gpu_info", 5, &grp);
drm_kstat_add_u64(grp, "vram_total", &adev->gmc.real_vram_size);
drm_kstat_add_u64(grp, "vram_used", &adev->vram_usage);
drm_kstat_add_u32(grp, "gpu_busy_percent", &adev->gpu_busy);
drm_kstat_add_u32(grp, "gpu_clock_mhz", &adev->gpu_sclk);
drm_kstat_add_u32(grp, "mem_clock_mhz", &adev->gpu_mclk);
drm_kstat_install(grp);
```

User reads with: `kstat -m amdgpu -i 0 -n gpu_info`

**Files:** `drm_sun_sysfs.c` + `drm_sun_sysfs.h`

| | |
|-|-|
| Lines | ~300 |
| Difficulty | Medium |
| Used by | 67 amdgpu files for GPU info, power, thermal, firmware version, RAS stats |
| Note | kstat is read-only by design. For writable controls (power profile, fan speed), use a custom ioctl on the DRM device or a per-device devctl node. Most of the 67 sysfs files are read-only monitoring, so kstat covers the majority |

---

### 10. Debugfs → Stub (or kstat)

**Replaces:** Linux `<linux/debugfs.h>` -- `debugfs_create_file`, `debugfs_create_dir`, `seq_file` based debug output

**illumos primitives:** No debugfs equivalent. Stub to no-ops, or selectively expose via kstat.

**API:**
```c
/* Stub -- these compile to nothing */
#define debugfs_create_file(name, mode, parent, data, fops)  (NULL)
#define debugfs_create_dir(name, parent)                      (NULL)
#define debugfs_remove(dentry)                                 do {} while (0)
#define debugfs_remove_recursive(dentry)                       do {} while (0)

/* For seq_file users, provide a minimal stub */
struct seq_file { /* empty */ };
#define seq_printf(m, fmt, ...)   do {} while (0)
#define seq_puts(m, s)            do {} while (0)
```

**Files:** `drm_sun_debugfs.h` (header-only stubs)

| | |
|-|-|
| Lines | ~80 |
| Difficulty | Easy |
| Used by | 57 amdgpu files, 9 display files |
| Note | Stubbing debugfs means losing the `amdgpu_debugfs` ring dump, register dump, firmware dump, etc. For actual debugging, MDB (modular debugger) is the illumos equivalent -- GPU state can be inspected via `::walk` and `::print` dcmds. A future `amdgpu.so` MDB module could replace debugfs |

---

### 11. xarray / Radix Tree

**Replaces:** Linux `<linux/xarray.h>` -- `xa_init`, `xa_store`, `xa_load`, `xa_erase`, `xa_for_each`. Also `<linux/idr.h>` -- `idr_alloc`, `idr_find`, `idr_remove` (modern Linux IDR is built on xarray)

**illumos primitives:** `mod_hash_create_idhash()` from `<sys/modhash.h>` for integer-keyed lookups, `avl_tree_t` from `<sys/avl.h>` for ordered iteration

**API:**
```c
/*
 * Integer-indexed associative array. Wraps mod_hash for O(1) lookup
 * and AVL for ordered iteration.
 */
typedef struct drm_xarray {
    mod_hash_t      *dxa_hash;    /* integer key → pointer */
    kmutex_t        dxa_lock;
    uint64_t        dxa_next_id;  /* for auto-ID allocation */
} drm_xarray_t;

void  drm_xa_init(drm_xarray_t *);
void  drm_xa_destroy(drm_xarray_t *);
int   drm_xa_store(drm_xarray_t *, unsigned long index, void *entry);
void *drm_xa_load(drm_xarray_t *, unsigned long index);
void *drm_xa_erase(drm_xarray_t *, unsigned long index);
int   drm_xa_alloc(drm_xarray_t *, uint32_t *idp, void *entry); /* auto-assign ID */
```

**Files:** `drm_sun_xarray.c` + `drm_sun_xarray.h`

| | |
|-|-|
| Lines | ~200 |
| Difficulty | Medium |
| Used by | GEM handle tables, amdgpu BO tracking, VM page table management |
| Note | mod_hash is a chained hash table -- O(1) average for lookup/insert/delete. For range queries, use AVL tree alongside. The existing `drm_sun_idr.c` already provides IDR-like functionality using AVL trees; xarray extends this with more flexible indexing |

---

### 12. Interval Tree

**Replaces:** Linux `<linux/interval_tree.h>` -- `interval_tree_insert`, `interval_tree_remove`, `interval_tree_iter_first`, `interval_tree_iter_next`

**illumos primitives:** `avl_tree_t` with a range comparator

**API:**
```c
typedef struct drm_interval_node {
    avl_node_t  din_avl;
    uint64_t    din_start;
    uint64_t    din_last;     /* inclusive end */
    uint64_t    din_subtree_last; /* max end in subtree (augmented) */
} drm_interval_node_t;

typedef struct drm_interval_tree {
    avl_tree_t  dit_tree;
} drm_interval_tree_t;

void drm_interval_tree_init(drm_interval_tree_t *);
void drm_interval_tree_insert(drm_interval_tree_t *, drm_interval_node_t *);
void drm_interval_tree_remove(drm_interval_tree_t *, drm_interval_node_t *);
drm_interval_node_t *drm_interval_tree_iter_first(drm_interval_tree_t *,
    uint64_t start, uint64_t last);
drm_interval_node_t *drm_interval_tree_iter_next(drm_interval_node_t *,
    uint64_t start, uint64_t last);
```

**Implementation:** Augmented AVL tree. Each node stores `subtree_last` = max of (own last, left subtree_last, right subtree_last). This enables O(log n + k) range queries where k is the number of overlapping intervals.

**Files:** `drm_sun_interval_tree.c` + `drm_sun_interval_tree.h`

| | |
|-|-|
| Lines | ~250 |
| Difficulty | Medium-High |
| Used by | amdgpu VM address space management, mmu_notifier ranges, TTM range tracking |
| Note | The augmented field (`subtree_last`) must be updated on every insert/remove/rebalance. illumos AVL trees do not support augmentation natively, so the rebalance hook must be handled manually (check and update after every `avl_insert`/`avl_remove` by walking up the tree). Alternatively, use a simpler linear scan for small trees and optimize later. |

---

### 13. PCI Extensions (P2P, AER, ATS)

**Replaces:** Linux `<linux/pci.h>` extensions -- `pci_p2pdma_distance_many`, `pci_enable_atomic_ops_to_root`, `pci_aer_clear_nonfatal_status`, `pci_enable_ats`

**illumos primitives:** `pci_config_get/put` from existing `drm_sun_pci.c`, `pcieadm` infrastructure, `pcie_*` functions

**API:** Extend existing `drm_sun_pci.c`:
```c
/* Additional PCI capabilities */
int  drm_pci_find_ext_capability(struct pci_dev *, uint16_t cap);
int  drm_pci_enable_ats(struct pci_dev *, int ps);  /* Address Translation Services */
void drm_pci_disable_ats(struct pci_dev *);

/* AER (Advanced Error Reporting) */
int  drm_pci_aer_clear_nonfatal(struct pci_dev *);

/* MSI-X (multi-vector) */
int  drm_pci_alloc_irq_vectors(struct pci_dev *, int min, int max, uint_t flags);
void drm_pci_free_irq_vectors(struct pci_dev *);
int  drm_pci_irq_vector(struct pci_dev *, int nr);
```

**Files:** Extend `drm_sun_pci.c` + `drm_sun_pci.h`

| | |
|-|-|
| Lines | ~200 (additions to existing ~300) |
| Difficulty | Medium |
| Used by | amdgpu PCI init, IOMMU/ATS setup, interrupt allocation |
| Note | MSI-X support requires `ddi_intr_alloc()` with `DDI_INTR_TYPE_MSIX`. PCIe extended capabilities are in the extended config space (offset 0x100+), accessed via same `pci_config_get` functions. ATS may need IOMMU coordination via illumos `iommulib` |

---

### 14. mmu_notifier (Stub for Phase 1)

**Replaces:** Linux `<linux/mmu_notifier.h>` -- `mmu_interval_notifier_insert`, `mmu_interval_notifier_remove`, range invalidation callbacks

**illumos primitives:** None -- illumos has no mmu_notifier equivalent

**API:** Stub for initial port:
```c
struct mmu_interval_notifier { /* empty */ };
#define mmu_interval_notifier_insert(n, mm, s, l, ops) (0)
#define mmu_interval_notifier_remove(n)                do {} while (0)
```

**Files:** `drm_sun_mmu_notifier.h` (header-only stub)

| | |
|-|-|
| Lines | ~50 |
| Difficulty | Easy (stub) / Very Hard (real implementation) |
| Used by | amdgpu userptr BOs, KFD/HSA compute |
| Note | A real implementation would need `hat_callback` or `as_callback` to get notifications when userspace page tables change. This is needed for SVM (Shared Virtual Memory) in ROCm/compute. For display-only or headless-compute with kernel-managed buffers, the stub is sufficient |

---

### 15. rwsem (Reader-Writer Semaphore)

**Replaces:** Linux `<linux/rwsem.h>` -- `init_rwsem`, `down_read`, `up_read`, `down_write`, `up_write`, `down_read_trylock`, `down_write_trylock`

**illumos primitives:** `krwlock_t` from `<sys/rwlock.h>`

**API:**
```c
/* Direct mapping, can be done as macros */
#define init_rwsem(sem)          rw_init((sem), NULL, RW_DEFAULT, NULL)
#define down_read(sem)           rw_enter((sem), RW_READER)
#define down_write(sem)          rw_enter((sem), RW_WRITER)
#define up_read(sem)             rw_exit(sem)
#define up_write(sem)            rw_exit(sem)
#define down_read_trylock(sem)   rw_tryenter((sem), RW_READER)
#define down_write_trylock(sem)  rw_tryenter((sem), RW_WRITER)
#define downgrade_write(sem)     rw_downgrade(sem)
```

**Files:** Add to existing `drm_linux.h` (just macros)

| | |
|-|-|
| Lines | ~20 |
| Difficulty | Trivial |
| Used by | 5+ amdgpu files (VM, reset, XGMI) |
| Note | Near 1:1 mapping. The only subtlety: Linux `down_read_trylock` returns 1 on success, 0 on failure. illumos `rw_tryenter` returns non-zero on success, 0 on failure. Same convention. |

---

## Summary Table

| # | API | Lines | Difficulty | illumos Foundation | Blocking for |
|---|-----|-------|------------|-------------------|--------------|
| 1 | Completion | 120 | Easy | `kmutex_t` + `kcondvar_t` | DRM scheduler, firmware |
| 2 | Kernel Thread | 180 | Easy-Med | `thread_create()` | DRM scheduler, recovery |
| 3 | Firmware Loading | 100 | Easy | `firmware_open()` (exists!) | GPU init (all engines) |
| 4 | Scatter-Gather + DMA | 400 | Medium | `ddi_dma_*` + cookies | TTM, GART, ring buffers |
| 5 | GPU Page Pool | 600 | High | `ddi_dma_mem_alloc`, `gfxp` | TTM pool replacement |
| 6 | GPU Mmap / Fault | 350 | High | `devmap_callback_ctl` | Userspace GPU access |
| 7 | DRM Managed | 200 | Easy | `list_t` + `kmem` | DRM device lifecycle |
| 8 | DRM Print | 100 | Easy | `cmn_err()` / `dev_err()` | All logging |
| 9 | Sysfs → kstat | 300 | Medium | `kstat_create()` | GPU monitoring |
| 10 | Debugfs Stub | 80 | Easy | No-op macros | Compiles without debugfs |
| 11 | xarray | 200 | Medium | `mod_hash` + `avl_tree_t` | GEM handles, BO tracking |
| 12 | Interval Tree | 250 | Med-High | Augmented `avl_tree_t` | VM address management |
| 13 | PCI Extensions | 200 | Medium | `ddi_intr_alloc`, `pci_config` | MSI-X, ATS |
| 14 | mmu_notifier Stub | 50 | Easy | No-op macros | Compute (stub OK for display) |
| 15 | rwsem | 20 | Trivial | `krwlock_t` (direct map) | VM, reset |
| | **TOTAL** | **~3,150** | | | |

### Grand Total: New illumos-Native Code

| Category | Lines |
|----------|-------|
| Already done (dma_fence stack) | 1,878 |
| New APIs (this document) | ~3,150 |
| **Shim layer total** | **~5,028** |

This gives the DRM and TTM subsystems a complete illumos-native foundation.
The remaining work is porting the DRM core midlayer (~15,000 lines) and TTM
(~4,500 lines) on top of this foundation, plus bringing in the amdgpu driver
itself (which is mostly hardware register code that needs no OS adaptation).