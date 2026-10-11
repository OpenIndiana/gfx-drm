/*
 * Copyright (c) 2026 Toasty.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * illumos GEM shmem helper implementation.
 *
 * Provides DMA-backed anonymous memory for GEM objects, replacing Linux's
 * shmem_file_setup() + struct page infrastructure.  Each GEM object gets
 * a contiguous (or scatter-gather) DMA allocation via ddi_dma_mem_alloc().
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>

/*
 * We need the virtio_gpu driver header for struct drm_gem_shmem_object
 * and struct sg_table definitions.  This creates a dependency but is
 * acceptable since this file is only compiled for the virtio-gpu driver.
 */
#include "drmP.h"
#include "drm_sun_gem_shmem.h"

/*
 * Forward declaration -- the full struct is in virtgpu_drv.h.
 * We only need the DMA fields here.
 */
struct drm_gem_shmem_object;
struct sg_table;
struct scatterlist;

/*
 * DMA attributes for GEM shmem allocations.
 * These are suitable for virtio device scatter-gather:
 *   - 64-bit addressing (virtio uses 64-bit PAs)
 *   - page-aligned segments
 *   - multiple cookies allowed (scatter-gather)
 */
ddi_dma_attr_t drm_gem_shmem_dma_attr = {
	.dma_attr_version	= DMA_ATTR_V0,
	.dma_attr_addr_lo	= 0x0ULL,
	.dma_attr_addr_hi	= 0xFFFFFFFFFFFFFFFFULL,
	.dma_attr_count_max	= 0xFFFFFFFFULL,
	.dma_attr_align		= PAGESIZE,
	.dma_attr_burstsizes	= 0x7FF,
	.dma_attr_minxfer	= 1,
	.dma_attr_maxxfer	= 0xFFFFFFFFULL,
	.dma_attr_seg		= 0xFFFFFFFFFFFFFFFFULL,
	.dma_attr_sgllen	= -1,		/* unlimited scatter-gather */
	.dma_attr_granular	= PAGESIZE,
	.dma_attr_flags		= 0,
};

static ddi_device_acc_attr_t gem_shmem_acc_attr = {
	.devacc_attr_version	= DDI_DEVICE_ATTR_V1,
	.devacc_attr_endian_flags = DDI_NEVERSWAP_ACC,
	.devacc_attr_dataorder	= DDI_STRICTORDER_ACC,
};
