/*
 * Copyright (c) 2006, 2015, Oracle and/or its affiliates. All rights reserved.
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
 * Copyright (c) 2012, 2012 Intel Corporation.  All rights reserved.
 */

#ifndef __DRM_LINUX_H__
#define __DRM_LINUX_H__

#include <sys/types.h>
#include <sys/byteorder.h>
#include "drm_atomic.h"

#define DRM_MEM_CACHED	0
#define DRM_MEM_UNCACHED 	1
#define DRM_MEM_WC 		2

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define clamp_int64_t(val)			\
	val = min((int64_t)INT_MAX, val);	\
	val = max((int64_t)INT_MIN, val);

#define ioremap_wc(base,size) drm_sun_ioremap((base), (size), DRM_MEM_WC)
#define ioremap(base, size)   drm_sun_ioremap((base), (size), DRM_MEM_UNCACHED)
#define iounmap(addr)         drm_sun_iounmap((addr))

#define spinlock_t                       kmutex_t
#define	spin_lock_init(l)                mutex_init((l), NULL, MUTEX_DRIVER, NULL);
#define	spin_lock(l)	                 mutex_enter(l)
#define	spin_unlock(u)                   mutex_exit(u)
#define	spin_lock_irq(l)		mutex_enter(l)
#define	spin_unlock_irq(u)		mutex_exit(u)
#ifdef __lint
/*
 * The following is to keep lint happy when it encouters the use of 'flag'.
 * On Linux, this allows a local variable to be used to retain context,
 * but is unused on Solaris.  Rather than trying to place LINTED
 * directives in the source, we actually consue the flag for lint here.
 */
#define	spin_lock_irqsave(l, flag)       flag = 0; mutex_enter(l)
#define	spin_unlock_irqrestore(u, flag)  flag &= flag; mutex_exit(u)
#else
#define	spin_lock_irqsave(l, flag)       mutex_enter(l)
#define	spin_unlock_irqrestore(u, flag)  mutex_exit(u)
#endif

#define mutex_lock(l)			mutex_enter(l)
#define mutex_unlock(u)			mutex_exit(u)
#define mutex_is_locked(l)		mutex_owned(l)

#define assert_spin_locked(l)		ASSERT(MUTEX_HELD(l))

#define kmalloc           kmem_alloc
#define kzalloc           kmem_zalloc
#define kcalloc(x, y, z)  kzalloc((x)*(y), z)
#define kfree             kmem_free

#define do_gettimeofday   (void) uniqtime
#define msleep_interruptible(s)  DRM_UDELAY(s)
#define	timeval_to_ns(tvp)	TICK_TO_NSEC(TIMEVAL_TO_TICK(tvp))
#define	ns_to_timeval(nsec, tvp)	TICK_TO_TIMEVAL(NSEC_TO_TICK(nsec), tvp)

#define GFP_KERNEL KM_SLEEP
#define GFP_ATOMIC KM_SLEEP

#define	KHZ2PICOS(a)		(1000000000UL/(a))

#define udelay			drv_usecwait
#define mdelay(x)		udelay((x) * 1000)
#define msleep(x)		mdelay((x))
#define msecs_to_jiffies(x)	drv_usectohz((x) * 1000)
#define jiffies_to_msecs(x)	drv_hztousec(x) / 1000
#define time_after(a,b)	((long)(b) - (long)(a) < 0)
#define	time_after_eq(a,b)	((long)(a) - (long)(b) >= 0)
#define time_before_eq(a,b)	time_after_eq(b,a)
#define time_in_range(a,b,c) \
	(time_after_eq(a,b) && \
	 time_before_eq(a,c))

#define	jiffies	ddi_get_lbolt()

#ifdef _BIG_ENDIAN
#define cpu_to_le16(x) LE_16(x) 
#define le16_to_cpu(x) LE_16(x)
#else
#define cpu_to_le16(x) (x) 
#define le16_to_cpu(x) (x)
#endif

#define swap(a, b) \
	do { int tmp = (a); (a) = (b); (b) = tmp; } while (__lintzero)

#define abs(x) ((x < 0) ? -x : x)

#define div_u64(x, y) ((unsigned long long)(x))/((unsigned long long)(y))  /* XXX FIXME */
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))

#define put_user(val,ptr) DRM_COPY_TO_USER(ptr,(&val),sizeof(val))
#define get_user(x,ptr) DRM_COPY_FROM_USER((&x),ptr,sizeof(x))
#define copy_to_user DRM_COPY_TO_USER
#define copy_from_user DRM_COPY_FROM_USER
#define unlikely(a)  (a)

#if 0 /* See sys/agpgart.h */
#define AGP_USER_TYPES (1 << 16)
#define AGP_USER_MEMORY (AGP_USER_TYPES)
#define AGP_USER_CACHED_MEMORY (AGP_USER_TYPES + 1)
#endif

#define ALIGN(x, a)	(((x) + ((a) - 1)) & ~((a) - 1))

#define page_to_phys(x)	*(uint32_t *)(uintptr_t)(x)
#define in_dbg_master()	0

#define BITS_PER_BYTE		8
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))
#define BITS_TO_LONGS(nr)	DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))
#define	POS_DIV_ROUND_CLOSEST(x, d)	((x + (d / 2)) / d)
#define POS_DIV_ROUND_UP_ULL(x, d)	DIV_ROUND_UP(x,d)

typedef unsigned long dma_addr_t;
typedef uint64_t	u64;
typedef int64_t		s64;
typedef uint32_t	u32;
typedef int32_t		s32;
typedef uint16_t	u16;
typedef uint8_t		u8;
typedef uint_t		irqreturn_t;

typedef int		bool;

#define true		(1)
#define false		(0)

#define __init
#define __exit
#define __iomem

#ifdef _ILP32
typedef u32 resource_size_t;
#else /* _LP64 */
typedef u64 resource_size_t;
#endif

typedef struct kref {
	atomic_t refcount;
} kref_t;

extern void kref_init(struct kref *kref);
extern void kref_get(struct kref *kref);
extern void kref_put(struct kref *kref, void (*release)(struct kref *kref));

extern unsigned int hweight16(unsigned int w);

extern long IS_ERR(const void *ptr);
#define	IS_ERR_OR_NULL(ptr)	(!ptr || IS_ERR(ptr))

#ifdef __lint
/*
 * The actual code for _wait_for() causes Solaris lint2 to fail, though
 * by all appearances, the code actually works (may try and peek at
 * the compiled code to understand why).  So to get around the problem,
 * we create a special lint version for _wait_for().
 */
#define _wait_for(COND, MS, W) (! (COND))
#else /* !__lint */
#define _wait_for(COND, MS, W) ({ \
	unsigned long timeout__ = jiffies + msecs_to_jiffies(MS);	\
	int ret__ = 0;							\
	while (! (COND)) {						\
		if (time_after(jiffies, timeout__)) {			\
			ret__ = -ETIMEDOUT;				\
			break;						\
		}							\
		if (W) udelay(W);					\
	}								\
	ret__;								\
})
#endif /* __lint */

#define wait_for(COND, MS) _wait_for(COND, MS, 1)
#define wait_for_atomic(COND, MS) _wait_for(COND, MS, 0)

/*
 * Memory barriers. Linux smp_*mb() map to illumos membar_*().
 *   smp_mb()  -- full barrier (acquire + release)
 *   smp_rmb() -- read barrier (loads won't be reordered before this)
 *   smp_wmb() -- write barrier (stores won't be reordered after this)
 */
#define	smp_mb()	membar_producer(); membar_consumer()
#define	smp_rmb()	membar_consumer()
#define	smp_wmb()	membar_producer()

/*
 * Byte order macros for 32-bit and 64-bit values.
 * The existing 16-bit macros use LE_16; extend to 32/64.
 */
#ifdef _BIG_ENDIAN
#define	cpu_to_le32(x)	LE_32(x)
#define	le32_to_cpu(x)	LE_32(x)
#define	cpu_to_le64(x)	LE_64(x)
#define	le64_to_cpu(x)	LE_64(x)
#define	cpu_to_be32(x)	(x)
#define	be32_to_cpu(x)	(x)
#define	cpu_to_be64(x)	(x)
#define	be64_to_cpu(x)	(x)
#else
#define	cpu_to_le32(x)	(x)
#define	le32_to_cpu(x)	(x)
#define	cpu_to_le64(x)	(x)
#define	le64_to_cpu(x)	(x)
#define	cpu_to_be32(x)	BE_32(x)
#define	be32_to_cpu(x)	BE_32(x)
#define	cpu_to_be64(x)	BE_64(x)
#define	be64_to_cpu(x)	BE_64(x)
#endif

/*
 * 64-bit atomics.  illumos has atomic_add_64_nv etc. in <sys/atomic.h>.
 */
typedef struct {
	volatile int64_t	counter;
} atomic64_t;

#define	ATOMIC64_INIT(i)	{ (i) }
#define	atomic64_read(v)	((v)->counter)
#define	atomic64_set(v, i)	((v)->counter = (i))
#define	atomic64_add(i, v)	atomic_add_64((volatile uint64_t *)&(v)->counter, (i))
#define	atomic64_sub(i, v)	atomic_add_64((volatile uint64_t *)&(v)->counter, -(i))
#define	atomic64_inc(v)		atomic_add_64((volatile uint64_t *)&(v)->counter, 1)
#define	atomic64_dec(v)		atomic_add_64((volatile uint64_t *)&(v)->counter, -1)
#define	atomic64_inc_return(v)	\
	((int64_t)atomic_add_64_nv((volatile uint64_t *)&(v)->counter, 1))
#define	atomic64_dec_return(v)	\
	((int64_t)atomic_add_64_nv((volatile uint64_t *)&(v)->counter, -1))
#define	atomic64_add_return(i, v) \
	((int64_t)atomic_add_64_nv((volatile uint64_t *)&(v)->counter, (i)))

/*
 * Read-write semaphore.  Map to illumos krwlock_t.
 */
#define	rw_semaphore		krwlock_t
#define	init_rwsem(rwl)		rw_init((rwl), NULL, RW_DRIVER, NULL)
#define	destroy_rwsem(rwl)	rw_destroy(rwl)
#define	down_read(rwl)		rw_enter((rwl), RW_READER)
#define	up_read(rwl)		rw_exit(rwl)
#define	down_write(rwl)		rw_enter((rwl), RW_WRITER)
#define	up_write(rwl)		rw_exit(rwl)
#define	down_read_trylock(rwl)	rw_tryenter((rwl), RW_READER)
#define	down_write_trylock(rwl)	rw_tryenter((rwl), RW_WRITER)

/*
 * kvmalloc / kvfree -- on illumos just use kmem.
 * Linux uses these for allocations that can fall back from kmalloc to vmalloc.
 */
#define	kvmalloc(size, flags)		kmem_alloc((size), KM_SLEEP)
#define	kvmalloc_array(n, sz, flags)	kmem_zalloc((n) * (sz), KM_SLEEP)
#define	kvzalloc(size, flags)		kmem_zalloc((size), KM_SLEEP)
#define	kvfree(ptr, size)		kmem_free((ptr), (size))

/*
 * Simple IDA (ID allocator) -- map to atomic counter for basic use.
 * Full IDA is available via drm_sun_idr; this covers the simple
 * ida_alloc/ida_free pattern used by modern DRM for minor numbering etc.
 */
typedef struct {
	atomic_t	counter;
} ida_simple_t;

#define	DEFINE_IDA(name)	ida_simple_t name = { { 0 } }

/*
 * ERR_PTR / PTR_ERR / IS_ERR pattern -- already partially defined.
 * Ensure ERR_PTR and PTR_ERR are available.
 */
#ifndef ERR_PTR
#define	ERR_PTR(err)	((void *)(uintptr_t)(long)(err))
#define	PTR_ERR(ptr)	((long)(uintptr_t)(ptr))
#endif

/*
 * container_of -- widely used in modern DRM.
 */
#ifndef container_of
#define	container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/*
 * Kernel logging helpers used by modern DRM (map to cmn_err).
 */
#ifndef pr_err
#define	pr_err(fmt, ...)	cmn_err(CE_WARN, fmt, ##__VA_ARGS__)
#define	pr_warn(fmt, ...)	cmn_err(CE_WARN, fmt, ##__VA_ARGS__)
#define	pr_info(fmt, ...)	cmn_err(CE_NOTE, fmt, ##__VA_ARGS__)
#define	pr_debug(fmt, ...)	/* nothing */
#endif

/*
 * typeof -- GCC and clang support this as __typeof__.
 */
#ifndef typeof
#define	typeof	__typeof__
#endif

/*
 * upper_32_bits / lower_32_bits -- used everywhere in DRM for 64-bit splits.
 */
#ifndef upper_32_bits
#define	upper_32_bits(n)	((uint32_t)(((uint64_t)(n)) >> 32))
#define	lower_32_bits(n)	((uint32_t)((n) & 0xFFFFFFFFUL))
#endif

/*
 * Multiplication overflow checking.
 */
#ifndef check_mul_overflow
#define	check_mul_overflow(a, b, res) ({	\
	typeof(a) __a = (a);			\
	typeof(b) __b = (b);			\
	typeof(res) __res = (res);		\
	*__res = __a * __b;			\
	(__b != 0 && (*__res / __b) != __a);	\
})
#endif

/*
 * READ_ONCE / WRITE_ONCE -- compiler barrier + volatile access.
 */
#ifndef READ_ONCE
#define	READ_ONCE(x)	(*(volatile typeof(x) *)&(x))
#endif
#ifndef WRITE_ONCE
#define	WRITE_ONCE(x, val)	(*(volatile typeof(x) *)&(x) = (val))
#endif

#endif /* __DRM_LINUX_H__ */
