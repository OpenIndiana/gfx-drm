/*
 * illumos dma_fence -- GPU synchronization primitives
 *
 * This is a design sketch for implementing Linux's dma_fence, dma_resv,
 * and ww_mutex on illumos, using native kernel primitives.
 *
 * Copyright 2026 -- Design Document / Not Yet Compilable
 *
 * References:
 *   Linux include/linux/dma-fence.h
 *   Linux include/linux/dma-resv.h
 *   Linux include/linux/ww_mutex.h
 */

#ifndef _ILLUMOS_DMA_FENCE_DESIGN_H
#define _ILLUMOS_DMA_FENCE_DESIGN_H

#include <sys/types.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/atomic.h>
#include <sys/list.h>
#include <sys/taskq.h>
#include <sys/kmem.h>
#include <sys/time.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * PART 1: REFCOUNT
 *
 * Linux uses kref (atomic refcount with release callback).
 * illumos has no kref, but atomic ops + a simple wrapper suffice.
 * ====================================================================== */

typedef struct drm_ref {
	volatile uint32_t	ref_count;
} drm_ref_t;

static inline void
drm_ref_init(drm_ref_t *ref)
{
	ref->ref_count = 1;
}

static inline void
drm_ref_get(drm_ref_t *ref)
{
	atomic_inc_32(&ref->ref_count);
}

/*
 * Returns B_TRUE if this was the last reference (count dropped to 0).
 */
static inline boolean_t
drm_ref_put(drm_ref_t *ref)
{
	uint32_t old = atomic_dec_32_nv(&ref->ref_count);
	ASSERT3S((int32_t)old, >=, 0);
	return (old == 0);
}

/*
 * Try to get a reference. Returns B_FALSE if refcount was already 0.
 * This is the equivalent of Linux's kref_get_unless_zero().
 * Used when traversing structures where the object might be dying.
 */
static inline boolean_t
drm_ref_get_unless_zero(drm_ref_t *ref)
{
	uint32_t old;

	do {
		old = ref->ref_count;
		if (old == 0)
			return (B_FALSE);
	} while (atomic_cas_32(&ref->ref_count, old, old + 1) != old);

	return (B_TRUE);
}

/* ======================================================================
 * PART 2: DMA_FENCE
 *
 * Core synchronization primitive: a one-shot signalable future.
 *
 * Design decisions for illumos:
 *
 * 1. LOCK MODEL: Linux uses a spinlock_t pointer (shared across fences
 *    in the same context). We use a kmutex_t pointer similarly.
 *    MUTEX_DRIVER with the device's iblock cookie, so it works from
 *    normal device interrupt handlers (PIL <= LOCK_LEVEL).
 *
 * 2. NO RCU: Linux uses RCU for lockless fence reads in dma_resv.
 *    illumos has no RCU. We use mutex + refcount instead. This means
 *    dma_resv iteration always holds the resv lock (no unlocked
 *    iteration). This is a simplification -- it trades some read-side
 *    parallelism for correctness without RCU. The locked iterator is
 *    what most callers use anyway.
 *
 * 3. NO UNION TRICK: Linux overlays cb_list/timestamp/rcu in a union
 *    to save 16 bytes per fence. We keep them separate for clarity and
 *    safety. Memory is cheap; debugging union state bugs is not.
 *
 * 4. CALLBACKS: Linux dispatches callbacks inline under spinlock with
 *    IRQs disabled. We dispatch under our mutex (which is adaptive,
 *    safe from device IRQ context). Callbacks MUST be fast and
 *    non-blocking -- same contract as Linux.
 *
 *    For cases where callback execution needs deferred context (e.g.,
 *    fence_array signaling its parent), we use taskq_dispatch_ent()
 *    with a preallocated taskq_ent_t to avoid allocation in IRQ context.
 *
 * 5. SIGNALED BIT: We use atomic_cas_32 on the flags field for the
 *    one-shot SIGNALED transition. Only one caller ever succeeds.
 * ====================================================================== */

struct dma_fence;
struct dma_fence_cb;

typedef void (*dma_fence_func_t)(struct dma_fence *, struct dma_fence_cb *);

typedef struct dma_fence_cb {
	list_node_t		fcb_node;	/* linkage in fence's cb list */
	dma_fence_func_t	fcb_func;
} dma_fence_cb_t;

typedef struct dma_fence_ops {
	/*
	 * Mandatory: return static strings identifying the driver and
	 * timeline. Used for debugging/tracing.
	 */
	const char *(*get_driver_name)(struct dma_fence *);
	const char *(*get_timeline_name)(struct dma_fence *);

	/*
	 * Optional: called under fence->lock when the first waiter or
	 * callback is registered. The driver should arrange for
	 * dma_fence_signal() to be called when the HW operation completes
	 * (e.g., enable an interrupt). Returns B_TRUE on success, B_FALSE
	 * if already signaled or error.
	 *
	 * Called with fence->lock held.
	 */
	boolean_t (*enable_signaling)(struct dma_fence *);

	/*
	 * Optional: fast poll -- returns B_TRUE if the fence has passed.
	 * Does NOT need to catch all transitions. May set fence->error.
	 * Called WITHOUT fence->lock.
	 */
	boolean_t (*signaled)(struct dma_fence *);

	/*
	 * Optional: custom release. Called when refcount drops to 0.
	 * May be called from ANY context, including interrupt.
	 * If NULL, kmem_free is used.
	 */
	void (*release)(struct dma_fence *);

	/*
	 * Optional: hint that the fence should complete by this deadline.
	 * Called WITHOUT fence->lock, possibly from multiple threads.
	 */
	void (*set_deadline)(struct dma_fence *, hrtime_t);
} dma_fence_ops_t;

/*
 * Fence flag bits, manipulated atomically.
 */
#define	DMA_FENCE_FLAG_SIGNALED		(1U << 0)
#define	DMA_FENCE_FLAG_TIMESTAMP	(1U << 1)
#define	DMA_FENCE_FLAG_ENABLE_SIGNAL	(1U << 2)
#define	DMA_FENCE_FLAG_USER_BITS	3	/* start of driver-private bits */

typedef struct dma_fence {
	kmutex_t		*fence_lock;	/* pointer to shared lock */
	const dma_fence_ops_t	*fence_ops;
	drm_ref_t		fence_ref;
	volatile uint32_t	fence_flags;
	int			fence_error;	/* 0 or negative errno */
	uint64_t		fence_context;
	uint64_t		fence_seqno;
	hrtime_t		fence_timestamp; /* set on signal */
	list_t			fence_cb_list;	/* list of dma_fence_cb_t */

	/*
	 * For blocking waiters: a condvar + the lock it's associated with.
	 * We use a dedicated lock for the cv rather than fence_lock because
	 * fence_lock is a POINTER to an external shared lock, and cv_wait
	 * needs a stable lock association.
	 */
	kmutex_t		fence_wait_lock;
	kcondvar_t		fence_wait_cv;
} dma_fence_t;

/*
 * Global context allocator. Returns a unique context ID (or range of
 * num contiguous IDs). Lock-free via atomic add.
 */
uint64_t dma_fence_context_alloc(unsigned int num);

/*
 * Initialize a fence. Caller provides:
 *   - ops: mandatory ops table
 *   - lock: pointer to a kmutex_t shared across fences in this context
 *   - context: from dma_fence_context_alloc()
 *   - seqno: monotonically increasing within context
 */
void dma_fence_init(dma_fence_t *fence, const dma_fence_ops_t *ops,
    kmutex_t *lock, uint64_t context, uint64_t seqno);

/*
 * Refcounting.
 */
static inline void
dma_fence_get(dma_fence_t *fence)
{
	if (fence != NULL)
		drm_ref_get(&fence->fence_ref);
}

static inline dma_fence_t *
dma_fence_get_ref(dma_fence_t *fence)
{
	if (fence != NULL)
		drm_ref_get(&fence->fence_ref);
	return (fence);
}

void dma_fence_release(dma_fence_t *fence);

static inline void
dma_fence_put(dma_fence_t *fence)
{
	if (fence != NULL && drm_ref_put(&fence->fence_ref))
		dma_fence_release(fence);
}

/*
 * Try-get for lockless traversal. Returns NULL if refcount was 0.
 */
static inline dma_fence_t *
dma_fence_get_unless_zero(dma_fence_t *fence)
{
	if (fence != NULL && drm_ref_get_unless_zero(&fence->fence_ref))
		return (fence);
	return (NULL);
}

/*
 * Free a fence (the default release). Uses kmem_free with the size
 * from kmem_alloc. Drivers allocating larger structs embedding
 * dma_fence_t must provide their own release op.
 */
void dma_fence_free(dma_fence_t *fence);

/*
 * Signal a fence. Only the first call succeeds (one-shot).
 *
 * The signal path:
 *   1. Atomically set SIGNALED flag via atomic_cas_32
 *   2. Acquire fence_lock
 *   3. Snapshot timestamp
 *   4. Move callback list to local variable
 *   5. Dispatch all callbacks under lock (they must be fast)
 *   6. Release fence_lock
 *   7. Wake all blocked waiters via cv_broadcast on fence_wait_cv
 *
 * Returns 0 on success, -EINVAL if already signaled.
 *
 * SAFE TO CALL FROM INTERRUPT CONTEXT: uses mutex_enter on an
 * adaptive mutex (OK at PIL <= LOCK_LEVEL) and cv_broadcast (always OK).
 */
int dma_fence_signal(dma_fence_t *fence);

/*
 * Signal with fence_lock already held.
 */
int dma_fence_signal_locked(dma_fence_t *fence);

/*
 * Test if signaled (non-blocking). May call ops->signaled() first.
 */
static inline boolean_t
dma_fence_is_signaled(dma_fence_t *fence)
{
	if (fence->fence_flags & DMA_FENCE_FLAG_SIGNALED)
		return (B_TRUE);
	if (fence->fence_ops->signaled != NULL &&
	    fence->fence_ops->signaled(fence)) {
		/* Hardware says it's done -- trigger full signal path */
		(void) dma_fence_signal(fence);
		return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Register a callback. cb must be caller-allocated (typically embedded
 * in a larger struct). Returns:
 *   0        -- callback registered, will be called on signal
 *   -ENOENT  -- fence already signaled, callback NOT called
 *
 * Callbacks execute under fence_lock. They MUST NOT sleep, MUST NOT
 * allocate with KM_SLEEP, and MUST be fast. If you need to do real
 * work, dispatch to a taskq from the callback.
 */
int dma_fence_add_callback(dma_fence_t *fence, dma_fence_cb_t *cb,
    dma_fence_func_t func);

/*
 * Remove a previously registered callback. Returns B_TRUE if removed,
 * B_FALSE if the callback was already dispatched (fence signaled).
 */
boolean_t dma_fence_remove_callback(dma_fence_t *fence, dma_fence_cb_t *cb);

/*
 * Block until fence is signaled or timeout expires.
 *
 * Arguments:
 *   fence    -- the fence to wait on
 *   intr     -- B_TRUE for interruptible (returns -EINTR on signal)
 *   timeout  -- relative timeout in nanoseconds, 0 = infinite
 *
 * Returns:
 *   Remaining time (> 0) on success
 *   0 on timeout
 *   -EINTR if interrupted by signal (only if intr == B_TRUE)
 *
 * Implementation uses fence_wait_cv + fence_wait_lock:
 *   mutex_enter(&fence->fence_wait_lock);
 *   while (!dma_fence_is_signaled(fence)) {
 *       if (intr)
 *           ret = cv_reltimedwait_sig(&fence->fence_wait_cv,
 *               &fence->fence_wait_lock, ticks, TR_NANOSEC);
 *       else
 *           ret = cv_reltimedwait(&fence->fence_wait_cv,
 *               &fence->fence_wait_lock, ticks, TR_NANOSEC);
 *       if (ret <= 0) break;  // timeout or signal
 *   }
 *   mutex_exit(&fence->fence_wait_lock);
 *
 * Before waiting, calls enable_signaling (under fence_lock) to ensure
 * the driver will generate an interrupt when the HW completes.
 */
clock_t dma_fence_wait_timeout(dma_fence_t *fence, boolean_t intr,
    hrtime_t timeout_ns);

/*
 * Convenience: wait forever, interruptible.
 */
static inline int
dma_fence_wait(dma_fence_t *fence, boolean_t intr)
{
	clock_t ret = dma_fence_wait_timeout(fence, intr, 0);

	return (ret < 0 ? (int)ret : 0);
}

/*
 * Sequence ordering: is f1 later than f2?
 * Both must be from the same context.
 */
static inline boolean_t
dma_fence_is_later(dma_fence_t *f1, dma_fence_t *f2)
{
	ASSERT3U(f1->fence_context, ==, f2->fence_context);
	return ((int64_t)(f1->fence_seqno - f2->fence_seqno) > 0);
}

/*
 * Set an error on the fence. Must be called BEFORE signaling.
 */
static inline void
dma_fence_set_error(dma_fence_t *fence, int error)
{
	ASSERT(!(fence->fence_flags & DMA_FENCE_FLAG_SIGNALED));
	ASSERT(error <= 0);
	fence->fence_error = error;
}

/* ======================================================================
 * PART 3: WW_MUTEX (Wait-Die Deadlock Avoidance)
 *
 * Linux's dma_resv uses a ww_mutex with Wait-Die semantics to allow
 * multiple buffer objects to be locked in arbitrary order without
 * deadlock. illumos has no ww_mutex; we build one from kmutex_t + cv.
 *
 * Wait-Die algorithm:
 *   Each transaction ("acquire context") gets a monotonically increasing
 *   stamp. Lower stamp = older = higher priority.
 *
 *   When transaction A tries to lock a mutex held by transaction B:
 *     - If A is older (A.stamp < B.stamp): A WAITS for B.
 *     - If A is younger (A.stamp > B.stamp): A DIES (returns -EDEADLK).
 *       A must release all locks, re-acquire the contended one via the
 *       slow path (which always blocks), then retry the others.
 *
 *   The "slow path" lock never returns EDEADLK -- it blocks regardless,
 *   because the caller has already released all other locks (so no
 *   deadlock is possible).
 * ====================================================================== */

typedef struct ww_class {
	volatile uint64_t	wc_stamp;	/* global counter */
	const char		*wc_name;
} ww_class_t;

typedef struct ww_acquire_ctx {
	uint64_t		wac_stamp;	/* acquired from wc_stamp */
	uint32_t		wac_acquired;	/* number of locks held */
	const ww_class_t	*wac_class;
} ww_acquire_ctx_t;

typedef struct ww_mutex {
	kmutex_t		wm_lock;	/* the actual mutex */
	kcondvar_t		wm_cv;		/* waiters sleep here */
	const ww_class_t	*wm_class;
	ww_acquire_ctx_t	*wm_ctx;	/* current owner's ctx, or NULL */
	boolean_t		wm_locked;	/* is it held? */
} ww_mutex_t;

/*
 * Define a ww_class. Typically file-scope static.
 */
#define	DEFINE_WW_CLASS(_name)						\
	ww_class_t _name = { .wc_stamp = 0, .wc_name = #_name }

void ww_mutex_init(ww_mutex_t *wm, ww_class_t *cls);
void ww_mutex_destroy(ww_mutex_t *wm);

/*
 * Start a transaction. Gets a unique stamp for ordering.
 */
static inline void
ww_acquire_init(ww_acquire_ctx_t *ctx, ww_class_t *cls)
{
	ctx->wac_stamp = atomic_inc_64_nv(&cls->wc_stamp);
	ctx->wac_acquired = 0;
	ctx->wac_class = cls;
}

static inline void
ww_acquire_fini(ww_acquire_ctx_t *ctx)
{
	ASSERT3U(ctx->wac_acquired, ==, 0);
}

/*
 * Lock a ww_mutex within a transaction context.
 *
 * Returns:
 *   0         -- locked successfully
 *   -EALREADY -- already held by this ctx
 *   -EDEADLK  -- must back off (younger than holder)
 *
 * ctx may be NULL for a context-less lock (no deadlock avoidance).
 */
int ww_mutex_lock(ww_mutex_t *wm, ww_acquire_ctx_t *ctx);

/*
 * Interruptible variant. Also returns -EINTR.
 */
int ww_mutex_lock_interruptible(ww_mutex_t *wm, ww_acquire_ctx_t *ctx);

/*
 * Slow path: called after receiving -EDEADLK and releasing all locks.
 * Blocks until the mutex is available (never returns -EDEADLK).
 */
int ww_mutex_lock_slow(ww_mutex_t *wm, ww_acquire_ctx_t *ctx);
int ww_mutex_lock_slow_interruptible(ww_mutex_t *wm, ww_acquire_ctx_t *ctx);

/*
 * Trylock. No context (cannot participate in Wait-Die ordering).
 */
boolean_t ww_mutex_trylock(ww_mutex_t *wm);

/*
 * Unlock. Wakes waiters.
 */
void ww_mutex_unlock(ww_mutex_t *wm);

static inline boolean_t
ww_mutex_is_locked(ww_mutex_t *wm)
{
	return (wm->wm_locked);
}

/*
 * Implementation sketch for ww_mutex_lock:
 *
 * int ww_mutex_lock(ww_mutex_t *wm, ww_acquire_ctx_t *ctx) {
 *     mutex_enter(&wm->wm_lock);
 *
 *     // Fast path: unlocked
 *     if (!wm->wm_locked) {
 *         wm->wm_locked = B_TRUE;
 *         wm->wm_ctx = ctx;
 *         if (ctx) ctx->wac_acquired++;
 *         mutex_exit(&wm->wm_lock);
 *         return (0);
 *     }
 *
 *     // Already held by us?
 *     if (ctx != NULL && wm->wm_ctx == ctx) {
 *         mutex_exit(&wm->wm_lock);
 *         return (-EALREADY);
 *     }
 *
 *     // Wait-Die: if we're younger than holder, die
 *     if (ctx != NULL && wm->wm_ctx != NULL &&
 *         ctx->wac_stamp > wm->wm_ctx->wac_stamp) {
 *         mutex_exit(&wm->wm_lock);
 *         return (-EDEADLK);
 *     }
 *
 *     // We're older (or no ctx) -- wait
 *     while (wm->wm_locked)
 *         cv_wait(&wm->wm_cv, &wm->wm_lock);
 *
 *     wm->wm_locked = B_TRUE;
 *     wm->wm_ctx = ctx;
 *     if (ctx) ctx->wac_acquired++;
 *     mutex_exit(&wm->wm_lock);
 *     return (0);
 * }
 */

/* ======================================================================
 * PART 4: DMA_RESV (Reservation Object)
 *
 * A container that associates a set of fences with a buffer object,
 * protected by a ww_mutex for deadlock-safe multi-buffer locking.
 *
 * Simplification vs Linux: no RCU, no unlocked iteration.
 * All fence list access requires holding the ww_mutex. This is safe
 * because the hot path (command submission) already holds the lock,
 * and the read path (wait/poll) can take the lock briefly.
 * ====================================================================== */

/*
 * Usage levels (lower numeric = higher priority).
 * When querying fences at level N, all fences at levels < N are
 * also returned.
 */
typedef enum dma_resv_usage {
	DMA_RESV_USAGE_KERNEL	= 0,	/* in-kernel memory management */
	DMA_RESV_USAGE_WRITE	= 1,	/* implicit write sync */
	DMA_RESV_USAGE_READ	= 2,	/* implicit read sync */
	DMA_RESV_USAGE_BOOKKEEP = 3	/* no implicit sync */
} dma_resv_usage_t;

/*
 * Fence list: dynamically sized array of (fence_ptr, usage) pairs.
 * Linux encodes usage in the low 2 bits of the fence pointer. We
 * use an explicit struct for clarity.
 */
typedef struct dma_resv_fence_entry {
	dma_fence_t		*drfe_fence;
	dma_resv_usage_t	drfe_usage;
} dma_resv_fence_entry_t;

typedef struct dma_resv_list {
	uint32_t		drl_num_fences;
	uint32_t		drl_max_fences;
	dma_resv_fence_entry_t	drl_entries[];	/* flex array */
} dma_resv_list_t;

extern ww_class_t reservation_ww_class;

typedef struct dma_resv {
	ww_mutex_t		dr_lock;
	dma_resv_list_t		*dr_fences;	/* NULL until first add */
} dma_resv_t;

void dma_resv_init(dma_resv_t *resv);
void dma_resv_fini(dma_resv_t *resv);

/*
 * Locking -- thin wrappers around ww_mutex.
 */
static inline int
dma_resv_lock(dma_resv_t *resv, ww_acquire_ctx_t *ctx)
{
	return (ww_mutex_lock(&resv->dr_lock, ctx));
}

static inline int
dma_resv_lock_interruptible(dma_resv_t *resv, ww_acquire_ctx_t *ctx)
{
	return (ww_mutex_lock_interruptible(&resv->dr_lock, ctx));
}

static inline void
dma_resv_lock_slow(dma_resv_t *resv, ww_acquire_ctx_t *ctx)
{
	(void) ww_mutex_lock_slow(&resv->dr_lock, ctx);
}

static inline boolean_t
dma_resv_trylock(dma_resv_t *resv)
{
	return (ww_mutex_trylock(&resv->dr_lock));
}

static inline void
dma_resv_unlock(dma_resv_t *resv)
{
	ww_mutex_unlock(&resv->dr_lock);
}

static inline boolean_t
dma_resv_is_locked(dma_resv_t *resv)
{
	return (ww_mutex_is_locked(&resv->dr_lock));
}

/*
 * Pre-allocate space for num_fences additional fences.
 * Must be called with lock held. May allocate memory (KM_SLEEP).
 * This is the ONLY function in the add path that can allocate.
 *
 * Returns 0 on success, -ENOMEM on failure.
 */
int dma_resv_reserve_fences(dma_resv_t *resv, uint32_t num_fences);

/*
 * Add a fence to the reservation. Must be called after reserve_fences.
 * Takes a reference on the fence. Replaces fences from the same context
 * with same-or-higher usage and later-or-equal seqno.
 *
 * CANNOT FAIL -- reserve_fences guarantees space.
 * Must be called with lock held.
 */
void dma_resv_add_fence(dma_resv_t *resv, dma_fence_t *fence,
    dma_resv_usage_t usage);

/*
 * Wait for all fences at the given usage level (and higher priority).
 * Acquires and releases the lock internally.
 *
 * Returns:
 *   B_TRUE   -- all fences signaled
 *   B_FALSE  -- timeout or interrupted
 */
boolean_t dma_resv_wait_timeout(dma_resv_t *resv, dma_resv_usage_t usage,
    boolean_t intr, hrtime_t timeout_ns);

/*
 * Test if all fences at the given usage level are signaled.
 * Acquires and releases the lock internally.
 */
boolean_t dma_resv_test_signaled(dma_resv_t *resv, dma_resv_usage_t usage);

/*
 * Iteration (locked). Caller must hold dma_resv_lock.
 */
typedef struct dma_resv_iter {
	dma_resv_t		*dri_resv;
	dma_resv_usage_t	dri_usage;
	uint32_t		dri_index;
	dma_fence_t		*dri_fence;	/* current fence (no extra ref) */
	dma_resv_usage_t	dri_fence_usage;
} dma_resv_iter_t;

void dma_resv_iter_begin(dma_resv_iter_t *iter, dma_resv_t *resv,
    dma_resv_usage_t usage);
dma_fence_t *dma_resv_iter_next(dma_resv_iter_t *iter);
void dma_resv_iter_end(dma_resv_iter_t *iter);

#define	dma_resv_for_each_fence(_iter, _resv, _usage, _fence)	\
	for (dma_resv_iter_begin((_iter), (_resv), (_usage));	\
	    ((_fence) = dma_resv_iter_next((_iter))) != NULL; )

/* ======================================================================
 * PART 5: DMA_FENCE_ARRAY (Wait-All Combinator)
 *
 * Aggregates N fences into one. Signals when all (or any) child fences
 * have signaled. Uses a pending counter + callbacks on each child.
 *
 * For deferred signaling (to avoid lock ordering issues when a child's
 * callback fires under its own lock), we use taskq_dispatch_ent with
 * a preallocated entry. This is the illumos equivalent of Linux's
 * irq_work.
 * ====================================================================== */

typedef struct dma_fence_array_cb {
	dma_fence_cb_t		fac_cb;
	struct dma_fence_array	*fac_array;
} dma_fence_array_cb_t;

typedef struct dma_fence_array {
	dma_fence_t		dfa_base;
	kmutex_t		dfa_lock;
	uint32_t		dfa_num_fences;
	volatile uint32_t	dfa_num_pending;
	dma_fence_t		**dfa_fences;
	boolean_t		dfa_signal_on_any;
	taskq_ent_t		dfa_tqent;	/* preallocated for deferred signal */
	dma_fence_array_cb_t	dfa_callbacks[];	/* flex array [num_fences] */
} dma_fence_array_t;

dma_fence_array_t *dma_fence_array_create(uint32_t num_fences,
    dma_fence_t **fences, uint64_t context, uint64_t seqno,
    boolean_t signal_on_any);

static inline dma_fence_array_t *
to_dma_fence_array(dma_fence_t *fence)
{
	/* Check ops pointer to verify type */
	extern const dma_fence_ops_t dma_fence_array_ops;
	if (fence->fence_ops != &dma_fence_array_ops)
		return (NULL);
	/* container_of */
	return ((dma_fence_array_t *)((char *)fence -
	    offsetof(dma_fence_array_t, dfa_base)));
}

/* ======================================================================
 * PART 6: RCU DISCUSSION
 *
 * Linux's dma_resv uses RCU for lockless read-side iteration. illumos
 * has no RCU. Our options:
 *
 * Option A (chosen): Always hold the lock for iteration.
 *   Pro: Simple, correct, no new infrastructure.
 *   Con: Readers block writers (and vice versa). Minor perf impact.
 *   Why it's OK: The lock hold times are very short (iterating a small
 *   array of fences). The main consumer of unlocked iteration in Linux
 *   is dma_resv_wait_timeout, which we implement by snapshotting the
 *   fence list under the lock, then waiting outside the lock.
 *
 * Option B (future): Build a minimal epoch-based reclamation.
 *   Similar to FreeBSD's epoch(9). Each reader enters an epoch (bumps
 *   a per-CPU counter), reads data, exits epoch. Writers defer frees
 *   until all readers from the previous epoch have exited.
 *   This would be a general-purpose kernel facility, not DRM-specific.
 *
 * Option C (future): Use krwlock_t for dma_resv.
 *   Replace ww_mutex with a ww_rwlock that allows concurrent readers.
 *   The fence list could use membar_producer/membar_consumer for
 *   lockless reads of the array (with a generation counter to detect
 *   concurrent modification).
 *
 * For the initial port, Option A is the right call. Optimize later
 * with profiling data.
 * ====================================================================== */

/* ======================================================================
 * PART 7: SIGNAL PATH -- DETAILED WALKTHROUGH
 *
 * The most performance-critical path is fence signaling from a GPU
 * interrupt handler. Here's the exact sequence:
 *
 * 1. GPU completes a command buffer, fires MSI interrupt.
 *
 * 2. i915 IRQ handler runs at PIL ~5 (below LOCK_LEVEL=10).
 *    It reads HW seqno registers and determines which fences completed.
 *
 * 3. For each completed fence, calls dma_fence_signal(fence):
 *    a. atomic_cas_32(&fence->fence_flags, old, old | SIGNALED)
 *       - If SIGNALED was already set, return immediately (idempotent).
 *       - CAS ensures exactly one caller succeeds.
 *    b. mutex_enter(fence->fence_lock)  [adaptive mutex, safe at PIL 5]
 *    c. fence->fence_timestamp = gethrtime()
 *    d. Move callback list to local variable (list_move_tail)
 *    e. For each callback: cb->fcb_func(fence, cb)
 *       - Callbacks run under fence_lock. They MUST be O(1).
 *       - Typical callback: decrement a pending counter, if zero
 *         dispatch a taskq_ent to signal the parent fence_array.
 *    f. mutex_exit(fence->fence_lock)
 *    g. mutex_enter(&fence->fence_wait_lock)
 *       cv_broadcast(&fence->fence_wait_cv)
 *       mutex_exit(&fence->fence_wait_lock)
 *
 * 4. Blocked waiters (in dma_fence_wait_timeout) wake up from cv_wait,
 *    see SIGNALED bit set, and return.
 *
 * Total locks touched per fence signal: 2 (fence_lock + fence_wait_lock).
 * Both are adaptive mutexes, very fast when uncontended.
 * ====================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* _ILLUMOS_DMA_FENCE_DESIGN_H */
