/*
 * Copyright (c) 2015-2024 TrustKernel Incorporated
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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#if IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT)
#include <linux/arm_ffa.h>
#endif

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/init.h>

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#endif

#include <linux/tee_tkcore.h>
#include <linux/tee_ioc.h>
#include <linux/arm-smccc.h>

#include <linux/tee_clkmgr.h>

#include <tee_shm.h>
#include <tee_supp_com.h>
#include <tee_wait_queue.h>

#include <arm_common/teesmc.h>

#include "asm/io.h"
#include "tee_mem.h"
#include "tee_tz_op.h"
#include "tee_tz_priv.h"
#include "handle.h"
#include "smc_abi_helper.h"

#include "tee_procfs.h"

#define _TEE_TZ_NAME "tkcoredrv"
#define DEV (ptee->tee->dev)

#define ALLOC_ALIGN		PAGE_SIZE

#define CAPABLE(tee) !(tee->conf & TEE_CONF_FW_NOT_CAPABLE)

/*******************************************************************
 * Calling TEE
 *******************************************************************/

static void handle_rpc_func_cmd_wait_queue(struct tee_tz *ptee,
		struct teesmc_arg *arg)
{
	struct teesmc_param *params;

	if (arg->num_params != 2)
		goto bad;

	params = TEESMC_GET_PARAMS(arg);

	if ((params[0].attr & TEESMC_ATTR_TYPE_MASK) !=
		TEESMC_ATTR_TYPE_VALUE_INPUT)
		goto bad;
	if ((params[1].attr & TEESMC_ATTR_TYPE_MASK) !=
		TEESMC_ATTR_TYPE_NONE)
		goto bad;

	switch (arg->cmd) {
	case TEE_RPC_WAIT_QUEUE_SLEEP:
		tee_wait_queue_sleep(DEV, &ptee->wait_queue,
					params[0].u.value.a);
		break;
	case TEE_RPC_WAIT_QUEUE_WAKEUP:
		tee_wait_queue_wakeup(DEV, &ptee->wait_queue,
					params[0].u.value.a);
		break;
	default:
		goto bad;
	}

	arg->ret = TEEC_SUCCESS;
	return;
bad:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

static void handle_rpc_func_cmd_wait(struct teesmc_arg *arg)
{
	struct teesmc_param *params;
	unsigned long usec_wait;

	if (arg->num_params != 1)
		goto bad;

	params = TEESMC_GET_PARAMS(arg);
	usec_wait = ((unsigned long) params[0].u.value.a) * 1000;

	usleep_range(usec_wait, usec_wait + USEC_PER_MSEC);

	arg->ret = TEEC_SUCCESS;
	return;
bad:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

static void handle_rpc_func_cmd_to_supplicant(struct tee_tz *ptee,
		struct teesmc_arg *arg)
{
	struct teesmc_param *params;
	struct tee_rpc_invoke inv;
	size_t n;
	uint32_t ret;

	if (arg->num_params > TEE_RPC_BUFFER_NUMBER) {
		arg->ret = TEEC_ERROR_GENERIC;
		return;
	}

	params = TEESMC_GET_PARAMS(arg);

	memset(&inv, 0, sizeof(inv));
	inv.cmd = arg->cmd;
	/*
	 * Set a suitable error code in case teed
	 * ignores the request.
	 */
	inv.res = TEEC_ERROR_NOT_IMPLEMENTED;
	inv.nbr_bf = arg->num_params;
	for (n = 0; n < arg->num_params; n++) {
		switch (params[n].attr & TEESMC_ATTR_TYPE_MASK) {
		case TEESMC_ATTR_TYPE_VALUE_INPUT:
		/* fall-through */
		case TEESMC_ATTR_TYPE_VALUE_INOUT:
			inv.cmds[n].fd = (int) params[n].u.value.a;
			inv.cmds[n].type = TEE_RPC_VALUE;
			break;
		case TEESMC_ATTR_TYPE_VALUE_OUTPUT:
			inv.cmds[n].type = TEE_RPC_VALUE;
			break;
		case TEESMC_ATTR_TYPE_MEMREF_INPUT:
		/* fall-through */
		case TEESMC_ATTR_TYPE_MEMREF_OUTPUT:
		/* fall-through */
		case TEESMC_ATTR_TYPE_MEMREF_INOUT:
			inv.cmds[n].buffer =
				(void *) (uintptr_t) params[n].u.memref.buf_ptr;
			inv.cmds[n].size = params[n].u.memref.size;
			inv.cmds[n].type = TEE_RPC_BUFFER;

			if (params[n].attr & TEESMC_ATTR_ENABLE_NAMED_SHM) {
				// flag named_shm for this piece of shared memory
				inv.cmds[n].type |= (TEE_RPC_ENABLE_NAMED_SHM |
					TEE_RPC_SHM_NAME(TEESMC_ATTR_SHM_NAME_GET(params[n].attr)));
			}
			break;
		default:
			arg->ret = TEEC_ERROR_GENERIC;
			return;
		}
	}

	ret = tee_supp_cmd(ptee->tee, TEE_RPC_ICMD_INVOKE,
			   &inv, sizeof(inv));
	if (ret == TEEC_RPC_OK)
		arg->ret = inv.res;

	for (n = 0; n < arg->num_params; n++) {
		switch (params[n].attr & TEESMC_ATTR_TYPE_MASK) {
		case TEESMC_ATTR_TYPE_MEMREF_OUTPUT:
		case TEESMC_ATTR_TYPE_MEMREF_INOUT:
			/*
			 * Allow teed to assign a new pointer
			 * to an out-buffer. Needed when the
			 * teed allocates a new buffer, for
			 * instance when loading a TA.
			 */
			params[n].u.memref.buf_ptr =
				(uintptr_t) inv.cmds[n].buffer;
			params[n].u.memref.size = inv.cmds[n].size;
			break;
		case TEESMC_ATTR_TYPE_VALUE_OUTPUT:
		case TEESMC_ATTR_TYPE_VALUE_INOUT:
			params[n].u.value.a = inv.cmds[n].fd;
			break;
		default:
			break;
		}
	}
}

#if IS_ENABLED(CONFIG_TRUSTKERNEL_TEE_RPMB_SUPPORT)

#include "linux/tee_rpmb.h"

static int check_rpmb_request(struct teesmc_arg *arg)
{
	struct teesmc_param *params;

	if (arg->num_params != TEE_RPMB_BUFFER_NUMBER) {
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return -1;
	}

	params = TEESMC_GET_PARAMS(arg);

	if (((params[0].attr & TEESMC_ATTR_TYPE_MASK)
				!= TEESMC_ATTR_TYPE_MEMREF_INPUT) ||
			((params[1].attr & TEESMC_ATTR_TYPE_MASK)
				!= TEESMC_ATTR_TYPE_MEMREF_OUTPUT)) {
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return -1;
	}

	return 0;
}

static void handle_rpmb_cmd(struct tee_tz *ptee,
				struct teesmc_arg *arg)
{
	uint32_t ret;
	struct tee_rpc_invoke inv;

	struct teesmc_param *params;

	if (check_rpmb_request(arg) < 0)
		return;

	params = TEESMC_GET_PARAMS(arg);

	memset(&inv, 0, sizeof(inv));
	inv.cmd = arg->cmd;

	inv.res = TEEC_ERROR_NOT_IMPLEMENTED;

	/*
	 * in current TEE's implementation,
	 * all rpmb parameters are consequently
	 * placed in ONE piece of shared memory,
	 * in the following way,
	 *
	 * [ rpmb request buffer ] [ rpmb response buffer ]
	 */
	inv.nbr_bf = 1;
	inv.cmds[0].buffer =
		(void *) (uintptr_t) params[0].u.memref.buf_ptr;
	inv.cmds[0].type = TEE_RPC_BUFFER;
	inv.cmds[0].size = params[0].u.memref.size
		+ params[1].u.memref.size;

	ret = tee_supp_cmd(ptee->tee, TEE_RPC_ICMD_INVOKE,
			&inv, sizeof(inv));

	if (ret != TEEC_RPC_OK)
		arg->ret = ret;
	else
		arg->ret = inv.res;
}

#else

static void handle_rpmb_cmd(struct tee_tz *ptee __always_unused,
				struct teesmc_arg *arg)
{
	arg->ret = TEEC_ERROR_NOT_IMPLEMENTED;
}

#endif

#if IS_ENABLED(CONFIG_TRUSTKERNEL_TEE_FP_SUPPORT)

static void handle_clkmgr_cmd(struct tee_tz *ptee __always_unused,
				struct teesmc_arg *arg)
{
	struct teesmc_param *params = TEESMC_GET_PARAMS(arg);

	if ((arg->num_params < 1) ||
			(params[0].attr & TEESMC_ATTR_TYPE_MASK) != TEESMC_ATTR_TYPE_VALUE_INPUT) {
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return;
	}

	arg->ret = tee_clkmgr_handle(params[0].u.value.a, params[0].u.value.b);
}

#else

static void handle_clkmgr_cmd(struct tee_tz *ptee __always_unused,
				struct teesmc_arg *arg)
{
	arg->ret = TEEC_ERROR_NOT_IMPLEMENTED;
}

#endif

static void handle_rpc_func_cmd(struct tee_tz *ptee, unsigned long parg)
{
	struct teesmc_arg *arg;

	// at the most 2 params come with RPC
	arg = tee_shm_pool_p2v_range(DEV, ptee->shm_pool,
		parg, TEESMC_GET_ARG_SIZE(2));
	if (!arg)
		return;

	switch (arg->cmd) {
	case TEE_RPC_WAIT_QUEUE_SLEEP:
	case TEE_RPC_WAIT_QUEUE_WAKEUP:
		handle_rpc_func_cmd_wait_queue(ptee, arg);
		break;
	case TEE_RPC_WAIT:
		handle_rpc_func_cmd_wait(arg);
		break;
	case TEE_RPC_RPMB_CMD:
		handle_rpmb_cmd(ptee, arg);
		break;
	case TEE_RPC_CLKMGR_CMD:
		handle_clkmgr_cmd(ptee, arg);
		break;
	default:
		handle_rpc_func_cmd_to_supplicant(ptee, arg);
	}
}

static struct tee_shm *handle_rpc_alloc(struct tee_tz *ptee,
		size_t size, unsigned long named_shm)
{
	struct tee_rpc_alloc rpc_alloc = {
		.size = size,
		.named_shm = (uint16_t) named_shm
	};

	if (named_shm >= 1024)
		return NULL;

	tee_supp_cmd(ptee->tee, TEE_RPC_ICMD_ALLOCATE,
		&rpc_alloc, sizeof(rpc_alloc));
	return rpc_alloc.shm;
}

static void handle_rpc_free(struct tee_tz *ptee, struct tee_shm *shm)
{
	struct tee_rpc_free rpc_free;

	if (!shm)
		return;
	rpc_free.shm = shm;
	tee_supp_cmd(ptee->tee, TEE_RPC_ICMD_FREE, &rpc_free, sizeof(rpc_free));
}

static int handle_rpc_notify_tee_log(bool flush)
{
	tkcore_notify_logger(flush);
	return 0;
}

void handle_rpc(struct tee_tz *ptee, struct invoke_tee_response *resp)
{
	struct tee_shm *shm;
	int cookie;

	switch (TEESMC_RETURN_GET_RPC_FUNC(resp->a0)) {
	case TEESMC_RPC_FUNC_ALLOC_ARG:
		resp->a1 =
			tkcore_shm_pool_alloc(DEV,
			ptee->shm_pool, resp->a1, 8);

		break;
	case TEESMC_RPC_FUNC_ALLOC_PAYLOAD:
		/* Can't support payload shared memory with this interface */
		resp->a2 = 0;
		break;
	case TEESMC_RPC_FUNC_FREE_ARG:
		tkcore_shm_pool_free(DEV, ptee->shm_pool, resp->a1, 0);
		break;
	case TEESMC_RPC_FUNC_FREE_PAYLOAD:
		/* Can't support payload shared memory with this interface */
		break;
	case TEESMC_TKCORE_RPC_FUNC_ALLOC_PAYLOAD:
		shm = handle_rpc_alloc(ptee, resp->a1, resp->a2);
		if (IS_ERR_OR_NULL(shm)) {
			resp->a1 = 0;
			break;
		}
		cookie = handle_get(&ptee->shm_handle_db, shm);
		if (cookie < 0) {
			handle_rpc_free(ptee, shm);
			resp->a1 = 0;
			break;
		}
		resp->a1 = shm->static_shm.paddr;
		resp->a2 = cookie;
		break;
	case TEESMC_TKCORE_RPC_FUNC_FREE_PAYLOAD:
		shm = handle_put(&ptee->shm_handle_db, resp->a1);
		handle_rpc_free(ptee, shm);
		break;
	case TEESMC_RPC_FUNC_CMD:
		handle_rpc_func_cmd(ptee, resp->a1);
		break;
	case TEESMC_TKCORE_RPC_FLUSH_TEE_LOG:
		resp->a1 = handle_rpc_notify_tee_log(resp->a1);
		break;
	default:
		pr_warn("Unknown RPC func 0x%lx\n", resp->a0);
		break;
	}
}

static bool default_affinity_need_bind_cpu(void *priv)
{
	(void) priv;
	return false;
}

static bool default_affinity_match_cpu(void *priv, int cpu)
{
	(void) priv;
	(void) cpu;
	return true;
}

static int default_affinity_get_candidate_cpu(void *priv)
{
	(void) priv;
	return 0;
}

static const struct tee_task_cpu_affinity_operations default_affinity_operations = {
	.need_bind_cpu = default_affinity_need_bind_cpu,
	.match_cpu = default_affinity_match_cpu,
	.get_candidate_cpu = default_affinity_get_candidate_cpu,
};

static void call_tee(struct tee_tz *ptee,
			const struct tee_task_cpu_affinity *ctx_task_cpu_affinity,
			uintptr_t parg, struct teesmc_arg *arg, u32 fid)
{
	u32 ret, rpc_ret_fid = TEESMC_IS_FAST_CALL(fid) ?
		TEE_SMC(FASTCALL_RETURN_FROM_RPC) : TEE_SMC(CALL_RETURN_FROM_RPC);
	struct tee_task_cpu_affinity aff;

	struct invoke_tee_response tee_req_resp = {
		.a1 = parg,
	};

	if (ctx_task_cpu_affinity) {
		aff = *ctx_task_cpu_affinity;
	} else {
		aff.ops = &default_affinity_operations;
		aff.priv = NULL;
	}

	for (;;) {
		tee_req_resp.a0 = fid;

		run_tee_task(ptee,
			&tee_req_resp, &aff);
		if (!TEESMC_RETURN_IS_RPC(tee_req_resp.a0))
			break;

		handle_rpc(ptee, &tee_req_resp);
		fid = rpc_ret_fid;
	}

	ret = tee_req_resp.a0;

	if (unlikely(ret != TEESMC_RETURN_OK &&
			ret != TEESMC_RETURN_UNKNOWN_FUNCTION)) {
		arg->ret = TEEC_ERROR_COMMUNICATION;
		arg->ret_origin = TEEC_ORIGIN_COMMS;
	}
}

static void stdcall_tee(struct tee_tz *ptee,
			const struct tee_task_cpu_affinity *aff,
			uintptr_t parg, struct teesmc_arg *arg)
{
	call_tee(ptee, aff, parg, arg, TEE_SMC(CALL_WITH_ARG));
}

static void fastcall_tee(struct tee_tz *ptee, uintptr_t parg,
			struct teesmc_arg *arg)
{
	call_tee(ptee, NULL, parg, arg, TEE_SMC(FASTCALL_WITH_ARG));
}

/*******************************************************************
 * TEE service invoke formating
 *******************************************************************/

/* allocate tee service argument buffer and return virtual address */
static void *alloc_tee_arg(struct tee_tz *ptee, unsigned long *p, size_t l)
{
	void *vaddr;

	WARN_ON(!CAPABLE(ptee->tee));

	if ((p == NULL) || (l == 0))
		return NULL;

	/* assume a 4 bytes aligned is sufficient */
	*p = tkcore_shm_pool_alloc(DEV, ptee->shm_pool, l, ALLOC_ALIGN);
	if (*p == 0)
		return NULL;

	vaddr = tee_shm_pool_p2v(DEV, ptee->shm_pool, *p);


	return vaddr;
}

/* free tee service argument buffer (from its physical address) */
static void free_tee_arg(struct tee_tz *ptee, unsigned long p)
{
	WARN_ON(!CAPABLE(ptee->tee));

	if (p)
		tkcore_shm_pool_free(DEV, ptee->shm_pool, p, 0);

}

static uint32_t get_cache_attrs(struct tee_tz *ptee)
{
	if (tee_shm_pool_is_cached(ptee->shm_pool))
		return TEESMC_ATTR_CACHE_DEFAULT << TEESMC_ATTR_CACHE_SHIFT;
	else
		return TEESMC_ATTR_CACHE_NONCACHE << TEESMC_ATTR_CACHE_SHIFT;
}

static uint32_t param_type_teec2teesmc(uint8_t type)
{
	switch (type) {
	case TEEC_NONE:
		return TEESMC_ATTR_TYPE_NONE;
	case TEEC_VALUE_INPUT:
		return TEESMC_ATTR_TYPE_VALUE_INPUT;
	case TEEC_VALUE_OUTPUT:
		return TEESMC_ATTR_TYPE_VALUE_OUTPUT;
	case TEEC_VALUE_INOUT:
		return TEESMC_ATTR_TYPE_VALUE_INOUT;
	case TEEC_MEMREF_TEMP_INPUT:
	case TEEC_MEMREF_PARTIAL_INPUT:
		return TEESMC_ATTR_TYPE_MEMREF_INPUT;
	case TEEC_MEMREF_TEMP_OUTPUT:
	case TEEC_MEMREF_PARTIAL_OUTPUT:
		return TEESMC_ATTR_TYPE_MEMREF_OUTPUT;
	case TEEC_MEMREF_WHOLE:
	case TEEC_MEMREF_TEMP_INOUT:
	case TEEC_MEMREF_PARTIAL_INOUT:
		return TEESMC_ATTR_TYPE_MEMREF_INOUT;
	default:
		WARN_ON(true);
		return 0;
	}
}

static void set_params(struct tee_tz *ptee,
	struct teesmc_param params[TEEC_CONFIG_PAYLOAD_REF_COUNT],
	uint32_t param_types,
	struct tee_data *data)
{
	size_t n;
	struct tee_shm *shm;
	struct TEEC_Value *value;

	for (n = 0; n < TEEC_CONFIG_PAYLOAD_REF_COUNT; n++) {
		uint32_t type = TEEC_PARAM_TYPE_GET(param_types, n);

		params[n].attr = param_type_teec2teesmc(type);
		if (params[n].attr == TEESMC_ATTR_TYPE_NONE)
			continue;
		if (params[n].attr < TEESMC_ATTR_TYPE_MEMREF_INPUT) {
			value = (struct TEEC_Value *)&data->params[n];
			params[n].u.value.a = value->a;
			params[n].u.value.b = value->b;
			continue;
		}
		shm = data->params[n].shm;
		params[n].attr |= get_cache_attrs(ptee);
		params[n].u.memref.buf_ptr = shm->static_shm.paddr;
		params[n].u.memref.size = shm->size_req;
	}
}

static void get_params(struct tee_data *data,
	struct teesmc_param params[TEEC_CONFIG_PAYLOAD_REF_COUNT])
{
	size_t n;
	struct tee_shm *shm;
	struct TEEC_Value *value;

	for (n = 0; n < TEEC_CONFIG_PAYLOAD_REF_COUNT; n++) {
		if (params[n].attr == TEESMC_ATTR_TYPE_NONE)
			continue;
		if (params[n].attr < TEESMC_ATTR_TYPE_MEMREF_INPUT) {
			value = &data->params[n].value;
			value->a = params[n].u.value.a;
			value->b = params[n].u.value.b;
			continue;
		}
		shm = data->params[n].shm;
		shm->size_req = params[n].u.memref.size;
	}
}

/*
 * tee_open_session - invoke TEE to open a GP TEE session
 */
static int tz_open(struct tee_session *sess, struct tee_cmd *cmd)
{
	struct tee *tee;
	struct tee_tz *ptee;
	int ret = 0;

	struct teesmc_arg *arg;
	struct teesmc_param *params;
	struct teesmc_meta_open_session *meta;
	uintptr_t parg;
	uintptr_t pmeta;
	size_t num_meta = 1;
	uint8_t *ta;
	struct TEEC_UUID *uuid;

	if (WARN_ON(!sess->ctx->tee) || WARN_ON(!sess->ctx->tee->priv))
		return -1;

	tee = sess->ctx->tee;
	ptee = tee->priv;

	if (cmd->uuid)
		uuid = cmd->uuid->static_shm.kaddr;
	else
		uuid = NULL;

	if (!CAPABLE(ptee->tee)) {
		pr_err("tee not capable\n");
		return -EBUSY;
	}

	/* case ta binary is inside the open request */
	ta = NULL;
	if (cmd->ta)
		ta = cmd->ta->static_shm.kaddr;
	if (ta)
		num_meta++;

	arg = alloc_tee_arg(ptee, &parg,
		TEESMC_GET_ARG_SIZE(
		TEEC_CONFIG_PAYLOAD_REF_COUNT + num_meta));
	meta = alloc_tee_arg(ptee, &pmeta, sizeof(*meta));

	if ((arg == NULL) || (meta == NULL)) {
		free_tee_arg(ptee, parg);
		free_tee_arg(ptee, pmeta);
		return -ENOMEM;
	}

	memset(arg, 0, sizeof(*arg));
	memset(meta, 0, sizeof(*meta));
	arg->num_params = TEEC_CONFIG_PAYLOAD_REF_COUNT + num_meta;
	params = TEESMC_GET_PARAMS(arg);

	arg->cmd = TEESMC_CMD_OPEN_SESSION;

	params[0].u.memref.buf_ptr = pmeta;
	params[0].u.memref.size = sizeof(*meta);
	params[0].attr = TEESMC_ATTR_TYPE_MEMREF_INPUT |
			   TEESMC_ATTR_META | get_cache_attrs(ptee);

	if (ta) {
		params[1].u.memref.buf_ptr =
			tee_shm_pool_v2p(DEV, ptee->shm_pool,
				cmd->ta->static_shm.kaddr);
		params[1].u.memref.size = cmd->ta->size_req;
		params[1].attr = TEESMC_ATTR_TYPE_MEMREF_INPUT |
				   TEESMC_ATTR_META | get_cache_attrs(ptee);
	}

	if (uuid != NULL)
		memcpy(meta->uuid, uuid, TEESMC_UUID_LEN);

	meta->clnt_login = cmd->login_method;
	memcpy(&meta->clnt_uuid, cmd->login_identity, sizeof(cmd->login_identity));

	params += num_meta;
	set_params(ptee, params, cmd->param.type, &cmd->param);

	stdcall_tee(ptee, sess->ctx->config ?
			sess->ctx->config->aff: NULL, parg, arg);

	get_params(&cmd->param, params);

	if (arg->ret != TEEC_ERROR_COMMUNICATION) {
		sess->sessid = arg->session;
		cmd->err = arg->ret;
		cmd->origin = arg->ret_origin;
	} else
		ret = -EBUSY;

	free_tee_arg(ptee, parg);
	free_tee_arg(ptee, pmeta);

	return ret;
}

/*
 * tee_invoke_command - invoke TEE to invoke a GP TEE command
 */
static int tz_invoke(struct tee_session *sess, struct tee_cmd *cmd)
{
	struct tee *tee;
	struct tee_tz *ptee;
	int ret = 0;

	struct teesmc_arg *arg;
	uintptr_t parg;
	struct teesmc_param *params;

	if (WARN_ON(!sess->ctx) ||
			WARN_ON(!sess->ctx->tee) ||
			WARN_ON(!sess->ctx->tee->priv))
		return -1;

	tee = sess->ctx->tee;
	ptee = tee->priv;

	if (!CAPABLE(tee)) {
		pr_err("tee not capable\n");
		return -EBUSY;
	}

	arg = (typeof(arg)) alloc_tee_arg(ptee, &parg,
		TEESMC_GET_ARG_SIZE(TEEC_CONFIG_PAYLOAD_REF_COUNT));
	if (!arg) {
		free_tee_arg(ptee, parg);
		return TEEC_ERROR_OUT_OF_MEMORY;
	}

	memset(arg, 0, sizeof(*arg));
	arg->num_params = TEEC_CONFIG_PAYLOAD_REF_COUNT;
	params = TEESMC_GET_PARAMS(arg);

	arg->cmd = TEESMC_CMD_INVOKE_COMMAND;
	arg->session = sess->sessid;
	arg->ta_func = cmd->cmd;

	set_params(ptee, params, cmd->param.type, &cmd->param);

	stdcall_tee(ptee, sess->ctx->config ?
			sess->ctx->config->aff: NULL, parg, arg);

	get_params(&cmd->param, params);

	if (arg->ret != TEEC_ERROR_COMMUNICATION) {
		cmd->err = arg->ret;
		cmd->origin = arg->ret_origin;
	} else
		ret = -EBUSY;

	free_tee_arg(ptee, parg);

	return ret;
}

/*
 * tee_cancel_command - invoke TEE to cancel a GP TEE command
 */
static int tz_cancel(struct tee_session *sess, struct tee_cmd *cmd)
{
	struct tee *tee;
	struct tee_tz *ptee;
	int ret = 0;

	struct teesmc_arg *arg;
	uintptr_t parg;

	WARN_ON(!sess->ctx->tee);
	WARN_ON(!sess->ctx->tee->priv);
	tee = sess->ctx->tee;
	ptee = tee->priv;


	arg = alloc_tee_arg(ptee, &parg, TEESMC_GET_ARG_SIZE(0));
	if (arg == NULL) {
		free_tee_arg(ptee, parg);
		return TEEC_ERROR_OUT_OF_MEMORY;
	}

	memset(arg, 0, sizeof(*arg));
	arg->cmd = TEESMC_CMD_CANCEL;
	arg->session = sess->sessid;

	fastcall_tee(ptee, parg, arg);

	if (arg->ret == TEEC_ERROR_COMMUNICATION)
		ret = -EBUSY;

	free_tee_arg(ptee, parg);

	return ret;
}

/*
 * tee_close_session - invoke TEE to close a GP TEE session
 */
static int tz_close(struct tee_session *sess)
{
	struct tee *tee;
	struct tee_tz *ptee;
	int ret = 0;

	struct teesmc_arg *arg;
	uintptr_t parg;

	if (WARN_ON(!sess->ctx) ||
			WARN_ON(!sess->ctx->tee) ||
			WARN_ON(!sess->ctx->tee->priv))
		return 0;

	tee = sess->ctx->tee;
	ptee = tee->priv;

	if (!CAPABLE(tee)) {
		pr_err("tee not capable\n");
		return -EBUSY;
	}

	arg = alloc_tee_arg(ptee, &parg, TEESMC_GET_ARG_SIZE(0));
	if (arg == NULL) {
		pr_err("failed to allocate tee arg\n");
		free_tee_arg(ptee, parg);
		return TEEC_ERROR_OUT_OF_MEMORY;
	}

	memset(arg, 0, sizeof(*arg));
	arg->cmd = TEESMC_CMD_CLOSE_SESSION;
	arg->session = sess->sessid;

	stdcall_tee(ptee, sess->ctx->config ?
		sess->ctx->config->aff : NULL, parg, arg);

	if (arg->ret == TEEC_ERROR_COMMUNICATION)
		ret = -EBUSY;

	free_tee_arg(ptee, parg);
	return ret;
}

static int alloc_sgt(struct tee_shm *shm)
{
	unsigned long pfn;
	struct page *page;

	pfn = shm->static_shm.paddr >> PAGE_SHIFT;

	page = pfn_to_page(pfn);
	if (IS_ERR_OR_NULL(page))
		return -EFAULT;

	/* Only one piece of contiguous physical memory */
	return sg_alloc_table_from_pages(&shm->static_shm.sgt, &page,
					1, 0, shm->size_alloc, GFP_KERNEL);
}

static struct tee_shm *tz_alloc(struct tee *tee, size_t size, uint32_t flags)
{
	int ret;
	struct tee_shm *shm = NULL;
	struct tee_tz *ptee;
	size_t size_aligned;

	if (WARN_ON(!tee->priv))
		return NULL;

	ptee = tee->priv;

	size_aligned = ((size / ALLOC_ALIGN) + 1) * ALLOC_ALIGN;
	if (unlikely(size_aligned == 0)) {
		pr_err("requested size too big\n");
		return NULL;
	}

	shm = devm_kzalloc(tee->dev, sizeof(struct tee_shm), GFP_KERNEL);
	if (!shm)
		return ERR_PTR(-ENOMEM);

	shm->size_alloc = ((size / ALLOC_ALIGN) + 1) * ALLOC_ALIGN;
	shm->size_req = size;

	shm->static_shm.paddr = tkcore_shm_pool_alloc(tee->dev, ptee->shm_pool,
						shm->size_alloc, ALLOC_ALIGN);
	if (WARN_ON(!shm->static_shm.paddr)) {
		devm_kfree(tee->dev, shm);
		return ERR_PTR(-ENOMEM);
	}

	shm->static_shm.kaddr =
		tee_shm_pool_p2v(tee->dev, ptee->shm_pool, shm->static_shm.paddr);
	if (WARN_ON(!shm->static_shm.kaddr)) {
		tkcore_shm_pool_free(tee->dev,
			ptee->shm_pool, shm->static_shm.paddr, NULL);
		devm_kfree(tee->dev, shm);
		return ERR_PTR(-EFAULT);
	}

	ret = alloc_sgt(shm);
	if (WARN_ON(ret)) {
		tkcore_shm_pool_free(tee->dev,
			ptee->shm_pool, shm->static_shm.paddr, NULL);
		devm_kfree(tee->dev, shm);
		return ERR_PTR(ret);
	}

	shm->flags = flags;
	if (ptee->shm_cached)
		shm->flags |= TEE_SHM_CACHED;

	return shm;
}

static void tz_free(struct tee_shm *shm)
{
	int ret;
	size_t size;
	struct tee *tee;
	struct tee_tz *ptee;

	if (WARN_ON(!shm->tee || !shm->tee->priv))
		return;

	tee = shm->tee;
	ptee = tee->priv;

	ret = tkcore_shm_pool_free(tee->dev, ptee->shm_pool,
		shm->static_shm.paddr, &size);
	if (!ret) {
		sg_free_table(&shm->static_shm.sgt);
		devm_kfree(tee->dev, shm);
		shm = NULL;
	}
}

static int tz_shm_inc_ref(struct tee_shm *shm)
{
	struct tee *tee;
	struct tee_tz *ptee;

	if (WARN_ON(!shm->tee || !shm->tee->priv))
		return -EINVAL;

	tee = shm->tee;
	ptee = tee->priv;

	return tee_shm_pool_incref(tee->dev,
		ptee->shm_pool, shm->static_shm.paddr);
}

#define DEFAULT_SHM_LENGTH_SHIFT	22

#ifdef CONFIG_OF

#define TZDRV_RESERVED_MEM_COMPAT "trustkernel,shared_mem"

static struct reserved_mem *tee_shared_mem = NULL;

#if !defined(MODULE)

static int __init tkcore_shared_mem_setup(struct reserved_mem *rmem)
{
	tee_shared_mem = rmem;
	return 0;
}

RESERVEDMEM_OF_DECLARE(tkcore_shared_mem, TZDRV_RESERVED_MEM_COMPAT,
		tkcore_shared_mem_setup);

static void init_tkcore_shared_mem(void)
{
	// do nothing when built-in linux
}

#else

static void init_tkcore_shared_mem(void)
{
	struct device_node *rmem_node;
	struct reserved_mem *rmem;

	/* Get reserved memory */
	rmem_node = of_find_compatible_node(NULL, NULL, TZDRV_RESERVED_MEM_COMPAT);
	if (!rmem_node) {
		pr_err("no node for reserved memory\n");
		return;
	}

	rmem = of_reserved_mem_lookup(rmem_node);
	if (!rmem) {
		pr_err("cannot lookup reserved memory\n");
		return;
	}

	tee_shared_mem = rmem;
}

#endif

#endif

static int get_shared_memory(unsigned long *base,
		size_t *length,
		bool *allocated)
{
#ifdef CONFIG_OF

	init_tkcore_shared_mem();

	if (!tee_shared_mem)
		goto alloc_shm;

	*base = tee_shared_mem->base;
	*length = tee_shared_mem->size;

	pr_info("use reserved shared memory: [ 0x%lx 0x%lx ]\n",
			*base, (unsigned long) *length);

	*allocated = false;
#endif

	return 0;

alloc_shm:
	pr_info("use allocated shared memory\n");

	*base = __get_free_pages(GFP_KERNEL,
		DEFAULT_SHM_LENGTH_SHIFT - PAGE_SHIFT);

	if (*base == 0)
		return -ENOMEM;

	*allocated = true;
	*length = 1ul << DEFAULT_SHM_LENGTH_SHIFT;

	return 0;
}

static unsigned long register_shared_mem(struct tee_tz *ptee,
			unsigned long shm_base_pa, unsigned long length, bool cacheable)
{
	int r;
	struct invoke_tee_response tee_resp;

	unsigned long handle = 0ul;
	uint32_t register_flags;

	r = ptee->build_mem_share_handle(ptee,
		shm_base_pa, length, cacheable, &handle, &register_flags);
	if (r != 0)
		return r;

	r = ptee->invoke_tee(ptee, TEE_SMC(TKCORE_FASTCALL_ADD_SHM),
		handle, length, register_flags, 0, &tee_resp);
	if (r) {
		ptee->free_mem_share_handle(ptee, handle);
		return r;
	}

	return tee_resp.a0;
}

static unsigned long register_log_buffer(struct tee_tz *ptee,
			unsigned long log_buf_pa, unsigned long length,
			bool cacheable, bool use_ffa_mem_api)
{
	int r;
	unsigned long handle = 0ul;
	uint32_t register_flags;

	struct invoke_tee_response tee_resp;

	r = ptee->build_mem_share_handle(ptee, log_buf_pa, length,
		cacheable, &handle, &register_flags);
	if (r != 0)
		return r;

	r = ptee->invoke_tee(ptee, TEE_SMC(TKCORE_FASTCALL_SET_LOG_BUFFER),
		handle, length, register_flags, 0, &tee_resp);
	if (r) {
		pr_err("setup tee log buffer failed with 0x%x\n", r);
		ptee->free_mem_share_handle(ptee, handle);
		return r;
	}

	return tee_resp.a0;
}

static int retrieve_tos_revision(struct tee_tz *ptee,
					uint32_t *maj, uint32_t *mid, uint32_t *min)
{
	int r;
	struct invoke_tee_response tee_resp;

	r = ptee->invoke_tee(ptee, TEE_SMC(CALL_GET_OS_REVISION),
			0, 0, 0, 0, &tee_resp);
	if (r)
		return r;

	if (tee_resp.a3 == 0) {
		*maj = 0;
		*mid = tee_resp.a0;
		*min = tee_resp.a1;
	} else {
		*maj = tee_resp.a0;
		*mid = tee_resp.a1;
		*min = tee_resp.a2;
	}

	return 0;
}

static void init_tos_version(struct tee_tz *ptee)
{
	struct tee *tee = ptee->tee;

	retrieve_tos_revision(ptee,
		&tee->version.maj, &tee->version.mid, &tee->version.min);

	pr_info("tkcoreos-rev: %d.%d.%d-gp\n",
			tee->version.maj, tee->version.mid ,tee->version.min);
}

/* configure_shm - Negotiate Shared Memory configuration with teetz. */
static int configure_shm(struct tee_tz *ptee)
{
	int ret = 0;

	unsigned long shm_base;
	size_t shm_length;
	bool shm_alloced = false;

	if (get_shared_memory(&shm_base, &shm_length, &shm_alloced)) {
		return -ENOMEM;
	}

	ptee->shm_cached = true;

	if (shm_alloced) {
		ptee->shm_vaddr = (void *) shm_base;
		ptee->shm_paddr = __pa(shm_base);
	} else {
		ptee->shm_paddr = shm_base;
		ptee->shm_vaddr = tee_map_cached_shm(ptee->shm_paddr,
				shm_length);
		if (ptee->shm_vaddr == NULL) {
			pr_warn("map shared mem failed\n");
			ret = -ENOMEM;
			goto out;
		}
	}

	ptee->shm_pool = tee_shm_pool_create(DEV, shm_length,
		ptee->shm_vaddr, ptee->shm_paddr);
	if (!ptee->shm_pool) {
		pr_warn("create shm pool failed (%zu)", shm_length);
		ret = -EINVAL;

		goto out;
	}

	if (ptee->shm_cached)
		tee_shm_pool_set_cached(ptee->shm_pool);

	/*
	 * currently shared memory is
	 * hard coded to cacheable
	 */
	ret = (int) register_shared_mem(ptee, ptee->shm_paddr, shm_length, true);
	if (ret != TEESMC_RETURN_OK) {
		pr_warn("register shm failed: %d", ret);
		ret = -EINVAL;
		goto out;
	}

out:
	if (ret != 0 && shm_alloced)
		free_pages(shm_base, DEFAULT_SHM_LENGTH_SHIFT - PAGE_SHIFT);

	return ret;
}

#define LOG_BUFFER_SIZE_SHIFT   (17)  //128KiB

static int init_log(struct tee_tz *ptee)
{
	int ret = 0, irq_num;
	struct tee *tee = ptee->tee;

	unsigned long va;
	size_t length = 1ul << LOG_BUFFER_SIZE_SHIFT;

#ifdef CONFIG_OF
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL,
						"trustkernel,tkcore");
	if (node) {
		irq_num = irq_of_parse_and_map(node, 0);
	} else {
		pr_err("device node not found\n");
		irq_num = 0;
	}
#else
	irq_num = 0;
#endif

	va = __get_free_pages(GFP_KERNEL,
		LOG_BUFFER_SIZE_SHIFT - PAGE_SHIFT);
	if (va == 0ul) {
		pr_warn("alloc log buffer failed");
		ret = -ENOMEM;
		goto err;
	}

	ret = (int) register_log_buffer(ptee, __pa(va),
		length, true, false);
	if (ret)
		goto err;

	tee->log.buffer = (void *) va;
	tee->log.length = length;
	tee->log.irq = irq_num;

	return 0;

err:
	if (va != 0ul)
		free_pages(va, LOG_BUFFER_SIZE_SHIFT - PAGE_SHIFT);

	tee->log.buffer = NULL;
	tee->log.length = 0;
	tee->log.irq = 0;

	return ret;
}

static int tz_start(struct tee *tee)
{
	int ret;
	struct tee_tz *ptee;

	if (WARN_ON(!tee || !tee->priv))
		return -EINVAL;

	if (WARN_ON(!CAPABLE(tee)))
		return -EBUSY;

	ptee = tee->priv;
	WARN_ON(ptee->started);

	ptee->started = true;

	ret = configure_shm(ptee);
	if (ret)
		goto exit;

exit:
	if (ret)
		ptee->started = false;

	return ret;
}

static int tz_stop(struct tee *tee)
{
	struct tee_tz *ptee;

	WARN_ON(!tee || !tee->priv);

	ptee = tee->priv;

	if (!CAPABLE(tee)) {
		pr_err("tee: bad state\n");
		return -EBUSY;
	}

	tee_shm_pool_destroy(tee->dev, ptee->shm_pool);
	iounmap(ptee->shm_vaddr);
	ptee->started = false;

	return 0;
}

/******************************************************************************/

const struct tee_ops tee_tz_fops = {
	.type = "tz",
	.owner = THIS_MODULE,
	.start = tz_start,
	.stop = tz_stop,
	.invoke = tz_invoke,
	.cancel = tz_cancel,
	.open = tz_open,
	.close = tz_close,
	.alloc = tz_alloc,
	.free = tz_free,
	.shm_inc_ref = tz_shm_inc_ref,
};

static int build_mem_share_handle_smc(struct tee_tz *ptee,
		unsigned long mem_share_base_pa, size_t size, bool cacheable,
		unsigned long *ptr_handle, uint32_t *ptr_flags)
{
	(void) ptee;
	(void) size;

	*ptr_handle = mem_share_base_pa;
	*ptr_flags = !!cacheable;

	return 0;
}

static void free_mem_share_handle_smc(struct tee_tz *ptee,
		unsigned long handle)
{
	(void) ptee;
	(void) handle;
}

static int invoke_tee_smc(struct tee_tz *ptee, uint32_t fid,
		unsigned long a1, unsigned long a2, unsigned long a3,
		unsigned long a4, struct invoke_tee_response *tee_resp)
{
	struct arm_smccc_res res;

	(void) ptee;

	arm_smccc_smc(fid, a1, a2, a3, a4, 0, 0, 0, &res);

	tee_resp->a0 = res.a0;
	tee_resp->a1 = res.a1;
	tee_resp->a2 = res.a2;
	tee_resp->a3 = res.a3;

	return 0;
}

static int tz_tee_init(struct tee_tz *ptee, struct tee *tee,
				invoke_tee_fn invoke_tee,
				build_mem_share_handle_fn build_shm_handle,
				free_mem_share_handle_fn free_shm_handle)
{
	int ret = 0;

	ptee->started = false;
	ptee->tee = tee;

	/* initialize tee-ree shm buffer */
	ptee->shm_paddr = 0ul;
	ptee->shm_vaddr = NULL;
	ptee->shm_cached = false;
	ptee->shm_pool = NULL;

	/* initialize tee-ree shared log buffer */
	ptee->log_buffer = NULL;
	ptee->log_buffer_size = 0;

	ptee->invoke_tee = invoke_tee;
	ptee->build_mem_share_handle = build_shm_handle;
	ptee->free_mem_share_handle = free_shm_handle;

	tee_wait_queue_init(&ptee->wait_queue);

	memset(&ptee->shm_handle_db, 0,
		sizeof(ptee->shm_handle_db));

	init_tos_version(ptee);

	ret = init_log(ptee);
	if (ret != 0) {
		pr_warn("init log buffer failed: %d\n", ret);
		/*
		 * initialization failed of
		 * log buffer shouldn't affect
		 * initialization of teedrv
		 */
		ret = 0;
	}

	return ret;
}

static void tz_tee_deinit(struct tee_tz *ptee)
{
	struct tee *tee = ptee->tee;

	if (!CAPABLE(tee))
		return;

	tee_wait_queue_exit(&ptee->wait_queue);
}

static int tee_tz_new(struct device *dev, int dev_id,
		invoke_tee_fn invoke_tee,
		build_mem_share_handle_fn build_shm_handle,
		free_mem_share_handle_fn free_shm_handle,
		struct tee **ptr_tee)
{
	int ret = 0;

	struct tee *tee;
	struct tee_tz *ptee;

	ret = tee_init_task();
	if (ret < 0)
		return ret;

	tee = tee_core_alloc(dev, _TEE_TZ_NAME, dev_id, &tee_tz_fops,
				sizeof(struct tee_tz));
	if (!tee) {
		ret = -ENOMEM;
		goto bail0;
	}

	ptee = tee->priv;
	ret = tz_tee_init(ptee, tee, invoke_tee,
		build_shm_handle, free_shm_handle);
	if (ret)
		goto bail1;

	ret = tee_core_add(tee);
	if (ret)
		goto bail2;

	ret = __tee_get(tee);
	if (ret)
		goto bail2;

	*ptr_tee = tee;
	return 0;

bail2:
	tz_tee_deinit(ptee);
bail1:
	tee_core_del(tee);
bail0:
	tee_exit_task();
	return ret;
}

static void tee_tz_free(struct tee_tz *ptee)
{
	tz_tee_deinit(ptee);
	tee_core_del(ptee->tee);
	tee_exit_task();
}

static int tkcore_smc_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct tee *tee;

	ret = tee_tz_new(dev, pdev->id, invoke_tee_smc,
		build_mem_share_handle_smc, free_mem_share_handle_smc,
		&tee);
	if (ret != 0)
		return ret;

	platform_set_drvdata(pdev, tee);
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int tkcore_smc_remove(struct platform_device *pdev)
{
	struct tee *tee = platform_get_drvdata(pdev);

	tee_tz_free((struct tee_tz *) tee->priv);
	return 0;
}
#else
static void tkcore_smc_remove(struct platform_device *pdev)
{
	struct tee *tee = platform_get_drvdata(pdev);

	tee_tz_free((struct tee_tz *) tee->priv);
}
#endif

static const struct of_device_id tkcore_smc_device_id[] = {
	{
		.compatible = "trustkernel,tzdrv",
	},
	{},
};

static struct platform_driver tkcore_smc_driver = {
	.probe = tkcore_smc_probe,
	.remove = tkcore_smc_remove,

	.driver = {
		.name = "tzdrv",
		.owner = THIS_MODULE,
		.of_match_table = tkcore_smc_device_id,
	},
};

static int tkcore_smc_abi_register(void)
{
	return platform_driver_register(&tkcore_smc_driver);
}

static void tkcore_smc_abi_unregister(void)
{
	platform_driver_unregister(&tkcore_smc_driver);
}

#if IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT)

static void tkcore_ffa_remove(struct ffa_device *ffa_dev)
{
	struct tee *tee = ffa_dev_get_drvdata(ffa_dev);

	tee_tz_free((struct tee_tz *) tee->priv);
}

#define FLAG_FFA_HANDLE (1U << 1)

//#define USE_FFA_MEM_SHARE_API

static int build_mem_share_handle_ffa(struct tee_tz *ptee,
		unsigned long mem_share_base_pa, size_t size, bool cacheable,
		unsigned long *ptr_handle, uint32_t *ptr_flags)
{
	/* we currently do not use memory sharing via FFA abi */
#if defined(USE_FFA_MEM_SHARE_API)
	int r;

	struct ffa_device *ffa_dev = to_ffa_dev(ptee->tee->dev);
	const struct ffa_mem_ops *mem_ops = ffa_dev->ops->mem_ops;

	struct ffa_mem_region_attributes mem_attr = {
		.receiver = ffa_dev->vm_id,
		.attrs = FFA_MEM_RW,
	};

	struct ffa_mem_ops_args args = {
		.use_txbuf = true,
		.attrs = &mem_attr,
		.nattrs = 1,
	};

	struct sg_table sgt;

	r = sg_alloc_table(&sgt, 1, GFP_KERNEL);
	if (r) {
		pr_err("alloc sgt failed with %d\n", r);
		return r;
	}

	sg_set_page(sgt.sgl, pfn_to_page(mem_share_base_pa >> PAGE_SHIFT),
		size, mem_share_base_pa & (PAGE_SIZE - 1));

	args.sg = sgt.sgl;
	r = mem_ops->memory_share(&args);
	sg_free_table(&sgt);

	if (r != 0) {
		pr_err("share mem failed with %d\n", r);
		return r;
	}

	*ptr_handle = args.g_handle;
	*ptr_flags = (!!cacheable) | FLAG_FFA_HANDLE;

	return 0;
#else
	return build_mem_share_handle_smc(ptee, mem_share_base_pa,
		size, cacheable, ptr_handle, ptr_flags);
#endif
}

static void free_mem_share_handle_ffa(struct tee_tz *ptee,
		unsigned long handle)
{
#if defined(USE_FFA_MEM_SHARE_API)
	int r;

	struct ffa_device *ffa_dev = to_ffa_dev(ptee->tee->dev);
	const struct ffa_mem_ops *mem_ops = ffa_dev->ops->mem_ops;

	r = mem_ops->memory_reclaim(handle, 0);
	if (r != 0) {
		pr_err("reclaim memory failed with %d\n", r);
		return;
	}

#else
	free_mem_share_handle_smc(ptee, handle);
#endif
}

static int invoke_tee_ffa(struct tee_tz *ptee, uint32_t fid,
		unsigned long a1, unsigned long a2, unsigned long a3,
		unsigned long a4, struct invoke_tee_response *tee_resp)
{
	int rc;
	struct ffa_device *ffa_dev = to_ffa_dev(ptee->tee->dev);
	const struct ffa_msg_ops *msg_ops;

	struct ffa_send_direct_data data = {
		.data0 = fid,
		.data1 = a1,
		.data2 = a2,
		.data3 = a3,
		.data4 = a4,
	};

	msg_ops = ffa_dev->ops->msg_ops;
	rc = msg_ops->sync_send_receive(ffa_dev, &data);
	if (rc) {
		pr_err("Unexpected error %d\n", rc);
		return rc;
	}

	tee_resp->a0 = data.data0;
	tee_resp->a1 = data.data1;
	tee_resp->a2 = data.data2;
	tee_resp->a3 = data.data3;
	return 0;
}

static struct ffa_device *tkcore_ffa_dev = NULL;

static int tkcore_ffa_probe(struct ffa_device *ffa_dev)
{
	int rc;
	struct tee *tee;

#if !defined(CONFIG_ARM64)
	ffa_dev->ops->mode_32bit_set(ffa_dev);
#endif

	rc = tee_tz_new(&ffa_dev->dev, ffa_dev->id, invoke_tee_ffa,
		build_mem_share_handle_ffa, free_mem_share_handle_ffa,
		&tee);
	if (rc != 0)
		return rc;

	ffa_dev_set_drvdata(ffa_dev, tee);
	tkcore_ffa_dev = ffa_dev;

	pr_info("ffa: initialized driver\n");
	return 0;
}

static const struct ffa_device_id tkcore_ffa_device_id[] = {
	//8bf0ccc3-42a3-5271-860d-66580469479c
	{ UUID_INIT(0x8bf0ccc3, 0x42a3, 0x5271,
		0x86, 0x0d, 0x66, 0x58, 0x04, 0x69, 0x47, 0x9c) },
	{}
};

static struct ffa_driver tkcore_ffa_driver = {
	.name = "tkcore",
	.probe = tkcore_ffa_probe,
	.remove = tkcore_ffa_remove,
	.id_table = tkcore_ffa_device_id,
};

static int tkcore_ffa_abi_register(void)
{
	if (IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT))
		return ffa_register(&tkcore_ffa_driver);
	else
		return -EOPNOTSUPP;
}

static void tkcore_ffa_abi_unregister(void)
{
	if (IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT))
		ffa_unregister(&tkcore_ffa_driver);
}

struct ffa_device *get_tee_ffa_dev(void)
{
	return tkcore_ffa_dev;
}
EXPORT_SYMBOL(get_tee_ffa_dev);

#else
static int tkcore_ffa_abi_register(void)
{
	return -EOPNOTSUPP;
}

static void tkcore_ffa_abi_unregister(void)
{
}
#endif

static int smc_abi_rc;
static int ffa_abi_rc;

static __init int tkcore_drv_init(void)
{
	pr_info("TrustKernel TEE Driver initialization\n");

	ffa_abi_rc = tkcore_ffa_abi_register();
	smc_abi_rc = tkcore_smc_abi_register();

	return ffa_abi_rc && smc_abi_rc;
}

static __exit void tkcore_drv_exit(void)
{
	pr_info("TrustKernel TEE Driver Release\n");

	if (ffa_abi_rc)
		tkcore_ffa_abi_unregister();
	if (smc_abi_rc)
		tkcore_smc_abi_unregister();
}

#if 0
#ifndef MODULE
rootfs_initcall(tkcore_drv_int);
#endif
#endif
module_init(tkcore_drv_init);
module_exit(tkcore_drv_exit);

MODULE_AUTHOR("TrustKernel");
MODULE_DESCRIPTION("TrustKernel TKCore TZ driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
