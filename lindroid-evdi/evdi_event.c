// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 * Copyright (c) 2025 Lindroid Authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include "evdi_drv.h"
#include <linux/sched.h>
#include <linux/prefetch.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>

struct evdi_event_pool global_event_pool = {0};
static void evdi_inflight_req_release(struct kref *kref);
void evdi_event_free_immediate(struct evdi_event *event);

DEFINE_STATIC_KEY_FALSE(evdi_perf_key);
bool evdi_perf_on;
struct evdi_perf_counters evdi_perf;

static void *evdi_mempool_kvzalloc(gfp_t gfp_mask, void *pool_data)
{
	return kvzalloc((size_t)pool_data, gfp_mask);
}

static void evdi_mempool_kvfree(void *element, void *pool_data)
{
	kvfree(element);
}

static void evdi_kmem_cache_destroy(struct kmem_cache **cache)
{
	if (*cache) {
		kmem_cache_destroy(*cache);
		*cache = NULL;
	}
}

static void evdi_event_destroy_caches(void)
{
	int i;
	evdi_kmem_cache_destroy(&global_event_pool.cache);
	for (i = 0; i < EVDI_EVENT_TYPE_MAX; i++)
		evdi_kmem_cache_destroy(&global_event_pool.type_cache[i]);
}

static mempool_t *evdi_mempool_create(size_t min, size_t elem_size)
{
    return mempool_create(min, evdi_mempool_kvzalloc,
                          evdi_mempool_kvfree, (void *)elem_size);
}

int evdi_event_system_init(void)
{
	char name[32];
	int i;

	global_event_pool.cache = kmem_cache_create("evdi_events",
						   sizeof(struct evdi_event),
						   0, SLAB_HWCACHE_ALIGN, NULL);
	if (!global_event_pool.cache)
		return -ENOMEM;

	for (i = 0; i < EVDI_EVENT_TYPE_MAX; i++) {
		snprintf(name, sizeof(name), "evdi_events_%d", i);
		global_event_pool.type_cache[i] =
			kmem_cache_create(name, sizeof(struct evdi_event),
					  0, SLAB_HWCACHE_ALIGN, NULL);
		if (!global_event_pool.type_cache[i]) {
			evdi_event_destroy_caches();
			return -ENOMEM;
		}
	}

	memset(&evdi_perf, 0, sizeof(evdi_perf));
	evdi_perf_on = false;
	evdi_smp_wmb();

	global_event_pool.inflight_pool = evdi_mempool_create(EVDI_INFLIGHT_POOL_MIN,
							      sizeof(struct evdi_inflight_req));
	global_event_pool.gralloc_data_pool = evdi_mempool_create(EVDI_GRALLOC_DATA_POOL_MIN,
								  sizeof(struct evdi_gralloc_data));

	if (!global_event_pool.inflight_pool || !global_event_pool.gralloc_data_pool)
		goto err;

	evdi_info("Event system initialized");

	/* Pre-warm cache */
	for (i = 0; i < 64; i++) {
		void *tmp = kmem_cache_alloc(global_event_pool.cache, GFP_NOWAIT);
		if (!tmp)
			break;
		kmem_cache_free(global_event_pool.cache, tmp);
	}

	return 0;

err:
	if (global_event_pool.gralloc_data_pool)
		mempool_destroy(global_event_pool.gralloc_data_pool);
	if (global_event_pool.inflight_pool)
		mempool_destroy(global_event_pool.inflight_pool);
	evdi_event_destroy_caches();
	return -ENOMEM;
}

void evdi_event_system_cleanup(void)
{
	mempool_destroy(global_event_pool.gralloc_data_pool);
	mempool_destroy(global_event_pool.inflight_pool);
	evdi_event_destroy_caches();
	evdi_debug("Event system cleaned up");
}

int evdi_event_init(struct evdi_device *evdi)
{
	spin_lock_init(&evdi->events.lock);
	init_waitqueue_head(&evdi->events.wait_queue);
	atomic_set(&evdi->events.cleanup_in_progress, 0);

	evdi->events.head = NULL;
	evdi->events.tail = NULL;
	atomic_set(&evdi->events.queue_size, 0);
	atomic_set(&evdi->events.next_poll_id, 1);
	atomic_set(&evdi->events.stopping, 0);

	init_llist_head(&evdi->events.lockfree_head);

	atomic64_set(&evdi->events.events_queued, 0);
	atomic64_set(&evdi->events.events_dequeued, 0);
	atomic_set(&evdi->events.wake_pending, 0);

	evdi_smp_wmb();

	evdi_debug("Event system initialized for device %d", evdi->dev_index);
	return 0;
}

void evdi_event_cleanup(struct evdi_device *evdi)
{
	struct evdi_event *event, *next;

	atomic_set(&evdi->events.cleanup_in_progress, 1);
	atomic_set(&evdi->events.stopping, 1);

	evdi_smp_wmb();

	wake_up_all(&evdi->events.wait_queue);

	spin_lock(&evdi->events.lock);
	event = READ_ONCE(evdi->events.head);
	WRITE_ONCE(evdi->events.head, NULL);
	WRITE_ONCE(evdi->events.tail, NULL);
	atomic_set(&evdi->events.queue_size, 0);
	atomic_set(&evdi->events.wake_pending, 0);
	spin_unlock(&evdi->events.lock);

	while (event) {
		next = READ_ONCE(event->next);
		evdi_event_free(event);
		event = next;
	}

	atomic_set(&evdi->events.cleanup_in_progress, 0);

	evdi_debug("Event system cleaned up for device %d", evdi->dev_index);
}

struct evdi_event *evdi_event_alloc(struct evdi_device *evdi,
				    enum poll_event_type type,
				    int poll_id, void *data,
				    size_t data_size, struct drm_file *owner)
{
	struct evdi_event *event;
	struct kmem_cache *cache = (type >= 0 && type < EVDI_EVENT_TYPE_MAX)
				   ? global_event_pool.type_cache[type]
				   : global_event_pool.cache;

	event = kmem_cache_alloc(cache, GFP_ATOMIC);
	if (!event)
		return NULL;

	event->from_pool = true;
	event->cache_idx = (cache != global_event_pool.cache) ? type : 0xff;
	event->type = type;
	event->poll_id = poll_id;
	event->payload_size = min(data_size, (size_t)EVDI_EVENT_PAYLOAD_MAX);
	if (data && data_size > 0)
		memcpy(event->payload, data, event->payload_size);
	event->next = NULL;
	event->owner = owner;
	event->evdi = evdi;
	atomic_set(&event->freed, 0);

	EVDI_PERF_INC64(&evdi_perf.allocs);
	return event;
}

void evdi_inflight_req_get(struct evdi_inflight_req *req)
{
	if (unlikely(!req))
		return;

	kref_get(&req->refcount);
}

void evdi_inflight_req_put(struct evdi_inflight_req *req)
{
	if (unlikely(!req))
		return;

	kref_put(&req->refcount, evdi_inflight_req_release);
}

static void evdi_inflight_req_release(struct kref *kref)
{
	struct evdi_inflight_req *req =
		container_of(kref, struct evdi_inflight_req, refcount);
	struct evdi_gralloc_data *gralloc;
	int i, nfd;

	if (atomic_xchg(&req->freed, 1))
		return;

	gralloc = req->reply.get_buf.gralloc_buf.gralloc;
	if (gralloc) {
		nfd = gralloc->numFds;
		if (nfd < 0)
			nfd = 0;
		else if (nfd > EVDI_MAX_FDS)
			nfd = EVDI_MAX_FDS;

		for (i = 0; i < nfd; i++) {
			if (gralloc->data_files[i]) {
				fput(gralloc->data_files[i]);
				gralloc->data_files[i] = NULL;
			}
		}
		mempool_free(gralloc, global_event_pool.gralloc_data_pool);
		req->reply.get_buf.gralloc_buf.gralloc = NULL;
	}

	mempool_free(req, global_event_pool.inflight_pool);
}

struct evdi_inflight_req *evdi_inflight_req_alloc(struct evdi_device *evdi)
{
    struct evdi_inflight_req *req;

    req = mempool_alloc(global_event_pool.inflight_pool, GFP_ATOMIC);
    if (unlikely(!req))
        return NULL;

    memset(req, 0, sizeof(*req));
    kref_init(&req->refcount);
    init_completion(&req->done);
    atomic_set(&req->freed, 0);

    return req;
}

void evdi_event_free_immediate(struct evdi_event *event)
{
	if (!event)
		return;

	if (likely(event->from_pool)) {
		struct kmem_cache *cache = (event->cache_idx < EVDI_EVENT_TYPE_MAX)
					   ? global_event_pool.type_cache[event->cache_idx]
					   : global_event_pool.cache;
		kmem_cache_free(cache, event);
	} else {
		kfree(event);
	}
}

static void evdi_event_free_rcu_cb(struct rcu_head *head)
{
    struct evdi_event *event = container_of(head, struct evdi_event, rcu);
    evdi_event_free_immediate(event);
}

void evdi_event_free_rcu(struct rcu_head *head)
{
	struct evdi_event *event = container_of(head, struct evdi_event, rcu);

	evdi_event_free_immediate(event);
}

void evdi_event_free(struct evdi_event *event)
{
	if (!event || atomic_xchg(&event->freed, 1))
		return;
	call_rcu(&event->rcu, evdi_event_free_rcu_cb);
}

static inline bool evdi_event_account_queue(struct evdi_device *evdi)
{
	bool first = evdi_events_inc_and_test_first(evdi);
	atomic64_inc(&evdi->events.events_queued);
	EVDI_PERF_INC64(&evdi_perf.event_queue_ops);
	return first;
}

static inline bool evdi_event_queue_lockfree(struct evdi_device *evdi, struct evdi_event *event)
{
	if (unlikely(atomic_read_acquire(&evdi->events.cleanup_in_progress)))
		return false;

	if (unlikely(atomic_read_acquire(&evdi->events.stopping)))
		return false;

	prefetchw(&event->llist);
	llist_add(&event->llist, &evdi->events.lockfree_head);
	
	evdi_smp_wmb();

	if (likely(evdi_event_account_queue(evdi)))
		evdi_wakeup_pollers(evdi);

	return true;
}

static inline void evdi_event_enqueue_ll(struct evdi_device *evdi, struct evdi_event *event)
{
	if (unlikely(atomic_read(&evdi->events.cleanup_in_progress) ||
		     atomic_read(&evdi->events.stopping))) {
		evdi_event_free(event);
		return;
	}

	llist_add(&event->llist, &evdi->events.lockfree_head);

	evdi_smp_wmb();

	if (likely(evdi_events_inc_and_test_first(evdi))) {
		atomic64_inc(&evdi->events.events_queued);
		EVDI_PERF_INC64(&evdi_perf.event_queue_ops);
		evdi_wakeup_pollers(evdi);
	}
}

void evdi_event_queue(struct evdi_device *evdi, struct evdi_event *event)
{
	if (!evdi || !event)
		return;

	evdi_event_enqueue_ll(evdi, event);
}

static inline struct evdi_event *evdi_event_pop(struct evdi_device *evdi)
{
	struct evdi_event *head;
	struct llist_node *lst = llist_del_all(&evdi->events.lockfree_head);
	struct llist_node *n;

	if (lst) {
		lst = llist_reverse_order(lst);
		spin_lock(&evdi->events.lock);
		for (n = lst; n; n = n->next) {
			struct evdi_event *e = llist_entry(n, struct evdi_event, llist);
			e->next = NULL;
			if (!evdi->events.head) {
				evdi->events.head = evdi->events.tail = e;
			} else {
				evdi->events.tail->next = e;
				evdi->events.tail = e;
			}
		}
		spin_unlock(&evdi->events.lock);
	}

	spin_lock(&evdi->events.lock);
	head = evdi->events.head;
	if (head) {
		evdi->events.head = head->next;
		if (!head->next)
			evdi->events.tail = NULL;
	}
	spin_unlock(&evdi->events.lock);

	if (head) {
		prefetch(&head->payload[0]);
		atomic64_inc(&evdi->events.events_dequeued);
		EVDI_PERF_INC64(&evdi_perf.event_dequeue_ops);
		if (evdi_events_dec_and_test_empty(evdi))
			atomic_set(&evdi->events.wake_pending, 0);
		evdi_smp_wmb();
	}

	return head;
}

static inline void evdi_event_drain_lockfree(struct evdi_device *evdi)
{
	struct llist_node *lst, *node;
	struct evdi_event *first = NULL, *last = NULL;

	lst = llist_del_all(&evdi->events.lockfree_head);
	if (!lst)
		return;

	lst = llist_reverse_order(lst);
	for (node = lst; node; node = node->next) {
		struct evdi_event *e = llist_entry(node, struct evdi_event, llist);
		e->next = NULL;
		if (!first)
			first = e;
		else
			last->next = e;

		last = e;
	}

	if (!first)
		return;

	spin_lock(&evdi->events.lock);
	if (!evdi->events.head) {
		WRITE_ONCE(evdi->events.head, first);
		WRITE_ONCE(evdi->events.tail, last);
	} else {
		WRITE_ONCE(evdi->events.tail->next, first);
		WRITE_ONCE(evdi->events.tail, last);
	}
	spin_unlock(&evdi->events.lock);
}

struct evdi_event *evdi_event_dequeue(struct evdi_device *evdi)
{
    return evdi_event_pop(evdi);
}

void evdi_event_cleanup_file(struct evdi_device *evdi, struct drm_file *file)
{
    struct llist_node *lst, *n;

    if (!evdi || !file)
        return;

    atomic_set(&evdi->events.cleanup_in_progress, 1);

    lst = llist_del_all(&evdi->events.lockfree_head);
    for (n = lst; n; n = n->next) {
        struct evdi_event *e = llist_entry(n, struct evdi_event, llist);
        if (e->owner == file) {
            call_rcu(&e->rcu, evdi_event_free_rcu_cb);
        } else {
            llist_add(&e->llist, &evdi->events.lockfree_head);
        }
    }

    spin_lock(&evdi->events.lock);
    {
        struct evdi_event *curr = evdi->events.head;
        struct evdi_event *next;

        /* Reset main queue pointers */
        evdi->events.head = NULL;
        evdi->events.tail = NULL;

        while (curr) {
            next = curr->next;

            if (curr->owner == file) {
                call_rcu(&curr->rcu, evdi_event_free_rcu_cb);
            } else {
                /* Re-append to queue */
                if (!evdi->events.head) {
                    evdi->events.head = evdi->events.tail = curr;
                    curr->next = NULL;
                } else {
                    evdi->events.tail->next = curr;
                    evdi->events.tail = curr;
                    curr->next = NULL;
                }
            }

            curr = next;
        }
    }
    spin_unlock(&evdi->events.lock);

    atomic_set(&evdi->events.cleanup_in_progress, 0);
    evdi_smp_wmb();
    wake_up_interruptible(&evdi->events.wait_queue);
}

int evdi_event_wait(struct evdi_device *evdi, struct drm_file *file)
{
	DEFINE_WAIT(wait);
	int ret = 0;

	EVDI_PERF_INC64(&evdi_perf.poll_cycles);

	for (;;) {
		prepare_to_wait(&evdi->events.wait_queue, &wait, TASK_INTERRUPTIBLE);

		if (atomic_read(&evdi->events.queue_size) > 0)
			break;

		if (atomic_read(&evdi->events.stopping)) {
			ret = -ENODEV;
			break;
		}

		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}
		schedule();
	}

	finish_wait(&evdi->events.wait_queue, &wait);
	return ret;
}
