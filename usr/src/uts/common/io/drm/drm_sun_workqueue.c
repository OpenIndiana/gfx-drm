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

#include <sys/sunddi.h>
#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/mutex.h>
#include <sys/condvar.h>

#include "drm_sun_workqueue.h"

/*
 * Module-global system workqueue for schedule_work().
 */
static struct workqueue_struct *drm_system_wq;

void
drm_sun_workqueue_init(void)
{
	drm_system_wq = create_workqueue(NULL, "drm_sys_wq");
}

void
drm_sun_workqueue_fini(void)
{
	if (drm_system_wq != NULL) {
		destroy_workqueue(drm_system_wq);
		drm_system_wq = NULL;
	}
}

/*
 * Wrapper that signals completion after the real work function runs.
 */
static void
work_wrapper(void *arg)
{
	struct work_struct *work = arg;

	work->func(work);

	if (work->ws_inited) {
		mutex_enter(&work->ws_lock);
		work->ws_pending = B_FALSE;
		cv_broadcast(&work->ws_cv);
		mutex_exit(&work->ws_lock);
	}
}

int
__queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	int	ret;

	ASSERT(wq->taskq != NULL);
	ASSERT(work->func != NULL);

	if (work->ws_inited) {
		mutex_enter(&work->ws_lock);
		work->ws_pending = B_TRUE;
		mutex_exit(&work->ws_lock);
	}

	/*
	 * ddi_taskq_dispatch can fail if there aren't enough memory
	 * resources.  In theory, since we are requesting a SLEEP
	 * allocation, it would be very rare to fail
	 */
	if ((ret = ddi_taskq_dispatch(wq->taskq, work_wrapper, work,
	    DDI_SLEEP)) == DDI_FAILURE) {
		cmn_err(CE_WARN, "queue_work: ddi_taskq_dispatch failure");
		if (work->ws_inited) {
			mutex_enter(&work->ws_lock);
			work->ws_pending = B_FALSE;
			cv_broadcast(&work->ws_cv);
			mutex_exit(&work->ws_lock);
		}
	}
	return (ret);
}

void
init_work(struct work_struct *work, void (*func)(void *))
{
	work->func = func;
	mutex_init(&work->ws_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&work->ws_cv, NULL, CV_DRIVER, NULL);
	work->ws_pending = B_FALSE;
	work->ws_inited = B_TRUE;
}

struct workqueue_struct *
create_workqueue(dev_info_t *dip, char *name)
{
	struct workqueue_struct *wq;

	wq = kmem_zalloc(sizeof (struct workqueue_struct), KM_SLEEP);
	wq->taskq = ddi_taskq_create(dip, name, 1, TASKQ_DEFAULTPRI, 0);
	if (wq->taskq == NULL)
		goto fail;
	wq->name = name;

	return wq;

fail :
	kmem_free(wq, sizeof (struct workqueue_struct));
	return (NULL);
}

void
destroy_workqueue(struct workqueue_struct *wq)
{
	if (wq) {
		ddi_taskq_destroy(wq->taskq);
		kmem_free(wq, sizeof (struct workqueue_struct));
	}
}

void
cancel_delayed_work(struct workqueue_struct *wq)
{
	ddi_taskq_wait(wq->taskq);
}

void
flush_workqueue(struct workqueue_struct *wq)
{
	ddi_taskq_wait(wq->taskq);
}

/*
 * schedule_work -- dispatch work to the global system workqueue.
 */
int
schedule_work(struct work_struct *work)
{
	if (drm_system_wq == NULL) {
		cmn_err(CE_WARN,
		    "schedule_work: system workqueue not initialized");
		return (DDI_FAILURE);
	}
	return (__queue_work(drm_system_wq, work));
}

/*
 * flush_work -- wait for a specific work_struct to complete.
 */
void
flush_work(struct work_struct *work)
{
	if (!work->ws_inited)
		return;

	mutex_enter(&work->ws_lock);
	while (work->ws_pending)
		cv_wait(&work->ws_cv, &work->ws_lock);
	mutex_exit(&work->ws_lock);
}

/*
 * cancel_work_sync -- cancel work and wait for completion.
 * On illumos we cannot actually cancel a dispatched taskq entry,
 * so we just wait for it to finish.  Returns B_TRUE if work was pending.
 */
boolean_t
cancel_work_sync(struct work_struct *work)
{
	boolean_t was_pending;

	if (!work->ws_inited)
		return (B_FALSE);

	mutex_enter(&work->ws_lock);
	was_pending = work->ws_pending;
	while (work->ws_pending)
		cv_wait(&work->ws_cv, &work->ws_lock);
	mutex_exit(&work->ws_lock);

	return (was_pending);
}
