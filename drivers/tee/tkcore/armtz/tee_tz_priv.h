/*
 * Copyright (c) 2015-2018 TrustKernel Incorporated
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __TEE_TZ_PRIV__
#define __TEE_TZ_PRIV__

#include <linux/tee_kernel_lowlevel_api.h>
#include <linux/tee_tkcore.h>

#include "tee_wait_queue.h"
#include "handle.h"

struct invoke_tee_response {
	unsigned long a0;
	unsigned long a1;
	unsigned long a2;
	unsigned long a3;
	unsigned long a4;
};

struct tee_tz;

typedef int (* invoke_tee_fn) (struct tee_tz *ptee, uint32_t fid,
		unsigned long a0, unsigned long a1, unsigned long a2,
		unsigned long a3, struct invoke_tee_response *resp);

typedef int (* build_mem_share_handle_fn) (struct tee_tz *ptee,
		unsigned long base_pa, size_t size, bool cacheable,
		unsigned long *ptr_handle, uint32_t *ptr_flags);

typedef void (* free_mem_share_handle_fn) (struct tee_tz *ptee,
		unsigned long handle);

struct tee;
struct shm_pool;

struct tee_tz {
	bool started;
	struct tee *tee;

	unsigned long shm_paddr;
	void *shm_vaddr;
	bool shm_cached;
	struct shm_pool *shm_pool;

	void *log_buffer;
	size_t log_buffer_size;

	invoke_tee_fn invoke_tee;
	build_mem_share_handle_fn build_mem_share_handle;
	free_mem_share_handle_fn free_mem_share_handle;

	struct tee_wait_queue_private wait_queue;

	struct handle_db shm_handle_db;
};

struct tee_task_cpu_affinity;
void run_tee_task(struct tee_tz *ptee,
		struct invoke_tee_response *req_resp,
		struct tee_task_cpu_affinity *aff);

int tee_init_task(void);
void tee_exit_task(void);

#endif /* __TEE_TZ_PRIV__ */
