/*
 * Copyright (c) 2006, 2013, Oracle and/or its affiliates. All rights reserved.
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
 * Copyright (c) 2012 Intel Corporation.  All rights reserved.
 */

#ifndef __DRM_SUN_WORKQUEUE_H__
#define __DRM_SUN_WORKQUEUE_H__

typedef void (* taskq_func_t)(void *);

#define INIT_WORK(work, func) \
	init_work((work), ((taskq_func_t)(func)))

struct work_struct {
	void		(*func) (void *);
	kmutex_t	ws_lock;
	kcondvar_t	ws_cv;
	boolean_t	ws_pending;	/* dispatch pending or running */
	boolean_t	ws_inited;	/* lock/cv initialized */
};

struct workqueue_struct {
	ddi_taskq_t *taskq;
	char *name;
};

extern int __queue_work(struct workqueue_struct *wq, struct work_struct *work);
#define queue_work	(void)__queue_work
extern void init_work(struct work_struct *work, void (*func)(void *));
extern struct workqueue_struct *create_workqueue(dev_info_t *dip, char *name);
extern void destroy_workqueue(struct workqueue_struct *wq);
extern void cancel_delayed_work(struct workqueue_struct *wq);
extern void flush_workqueue(struct workqueue_struct *wq);

/*
 * Global system workqueue and schedule_work / flush_work.
 *
 * schedule_work(ws) dispatches to a module-global system workqueue.
 * flush_work(ws) waits for a specific work_struct to complete.
 * The system workqueue is created in drm_sun_workqueue_init() and
 * destroyed in drm_sun_workqueue_fini(), called from _init/_fini.
 */
extern void drm_sun_workqueue_init(void);
extern void drm_sun_workqueue_fini(void);
extern int  schedule_work(struct work_struct *work);
extern void flush_work(struct work_struct *work);
extern boolean_t cancel_work_sync(struct work_struct *work);

#endif /* __DRM_SUN_WORKQUEUE_H__ */
