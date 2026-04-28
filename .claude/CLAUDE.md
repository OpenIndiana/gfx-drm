# illumos DRM / virtio-gpu Port

## Project Overview

Re-porting the Linux DRM subsystem to illumos. The gfx-drm repo contains the
original Linux 3.14 DRM port with i915/radeon drivers. We're adding modern
GPU support starting with virtio-gpu as the minimum viable DRM driver for
VM testing.

## What's Been Done

### Phase 0: Foundation Shims (commit 07faaa2)
- `drm_sun_completion.h`: struct completion, wait_event/wake_up macros
- `drm_sun_print.h`: drm_dbg/drm_err per-device logging
- `drm_sun_managed.h`: drmm_kzalloc/kcalloc/kfree stubs
- `drm_sun_debugfs.h`: no-op stubs
- `drm_sun_gem_modern.h`: drm_gem_object_funcs, get/put, DRIVER_RENDER
- Extended `drm_linux.h`: cpu_to_le32/64, atomic64_t, rwsem, kvmalloc
- Extended `drm_sun_workqueue`: schedule_work/flush_work

### Phase 1: Virtio Transport (commit 0aa7a30)
- `virtgpu_drm.h`, `virtio_gpu_hw.h`: UAPI + protocol headers
- `virtgpu_drv.h`: All structures adapted for illumos virtio framework
- `virtgpu_vq.c`: Core transport using virtio_chain_alloc/append/submit
- `virtgpu_kms.c`: Device init via virtio_init/queue_alloc/init_complete
- `virtgpu_fence.c`: Fence alloc/emit/signal via drm_sun_dma_fence

### Phase 2-4: GEM, Ioctls, DDI, Build (commit e582576)
- `virtgpu_object.c`: GEM objects backed by ddi_dma_mem_alloc
- `virtgpu_gem.c`: Object arrays, reservation locking, dumb_create
- `virtgpu_ioctl.c`: GETPARAM, RESOURCE_CREATE, MAP, WAIT, GET_CAPS
- `virtgpu_sunmod.c`: Full DDI entry point (_init/_fini, attach/detach)
- `Makefile.mod` + `intel/virtio_gpu/Makefile`: Build system

### Phase 5: Display (commit 7792ff6)
- `drm_sun_atomic.h`: Minimal atomic shim using 3.14 non-atomic path
- `virtgpu_display.c`: CRTC, connector, encoder using drm_crtc_helper

### API Review Fix (commit 5932d41)
Fixed 15 API mismatches found during manual review against gfx-drm 3.14:
- virtio_init() takes 1 arg, not 3
- virtio_features_present (plural)
- virtio_dma_va() takes 2 args (dma, offset)
- drm_connector_register/unregister → no-op stubs
- mode_set_nofb → mode_set (3.14 helper API)
- Ioctl table indexing: [DRM_IOCTL_NR(ioctl) - DRM_COMMAND_BASE]

### Packaging (commit 9cdc25d)
- Enabled virtgpu_drm.h in libdrm header package
- Added virtio_gpu driver + PCI alias pci1af4,1050 to driver manifest

## Build Instructions

```bash
# On illumos with gcc-10 (/usr/gcc/10) and onbld installed:
cd usr/src/uts/intel/virtio_gpu
make
```

## Test Instructions

```bash
# QEMU with virtio-gpu:
qemu-system-x86_64 -m 8G -smp 4 -enable-kvm -cpu host \
  -drive file=vm.qcow2,format=qcow2,if=none,id=disk0 \
  -device ahci,id=ahci0 -device ide-hd,drive=disk0,bus=ahci0.0 \
  -vga none -device virtio-gpu-pci \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -display none -serial stdio

# In guest:
add_drv -i '"pci1af4,1050"' virtio_gpu
ls /dev/dri/renderD128
```

## Key Architecture Decisions

- **Option B: Thin modern wrapper** — extends 3.14 DRM core without breaking i915
- **GEM shmem via ddi_dma_mem_alloc** — replaces Linux shmem_file_setup
- **virtio_chain API** — replaces Linux virtqueue_add_sgs scatter-gather
- **Non-atomic modesetting** — uses 3.14 drm_crtc_helper_set_config
- **gfx-drm list_head** — uses 3-arg list_add_tail(ptr, head, entry)

## File Inventory (31 files, 5,612 lines)

### New shim headers (usr/src/uts/common/drm/):
drm_sun_completion.h, drm_sun_print.h, drm_sun_managed.h,
drm_sun_debugfs.h, drm_sun_gem_modern.h, drm_sun_gem_shmem.h,
drm_sun_atomic.h, virtgpu_drm.h, virtio_gpu_hw.h

### Driver source (usr/src/uts/common/io/virtio_gpu/):
virtgpu_drv.h, virtgpu_sunmod.c, virtgpu_kms.c, virtgpu_vq.c,
virtgpu_fence.c, virtgpu_gem.c, virtgpu_object.c, virtgpu_ioctl.c,
virtgpu_submit.c (stub), virtgpu_debugfs.c (stub),
virtgpu_display.c, Makefile.mod, virtio_gpu.conf

### Build (usr/src/uts/intel/virtio_gpu/):
Makefile

## What's Next

1. Build the driver on an illumos host (VM with gcc-10)
2. Load and test basic ioctls (GETPARAM, RESOURCE_CREATE)
3. Enable virgl in Mesa (oi-userland change committed separately)
4. Test glxinfo/glxgears with virgl renderer
5. Eventually: real GPU drivers (amdgpu/xe)

## Related Repos

- oi-userland: Mesa virgl enablement (commit d69854c on oi/hipster)
- refraction-forger: VM image builder (virtgpu-dev.kdl spec in vm/)
