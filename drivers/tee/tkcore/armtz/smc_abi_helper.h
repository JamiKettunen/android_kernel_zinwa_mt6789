// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2018 TrustKernel Incorporated
 */

#ifndef SMC_ABI_HELPER_H_
#define SMC_ABI_HELPER_H_

#include <arm_common/teesmc.h>

#define _xconcat(x, y) x ## y
#define xconcat(x, y) _xconcat(x, y)

#if defined(CONFIG_ARM64)

#define teesmc_arg		teesmc64_arg
#define teesmc_param	teesmc64_param
#define TEESMC_GET_PARAMS(arg)		TEESMC64_GET_PARAMS(arg)
#define TEESMC_GET_ARG_SIZE(arg)	TEESMC64_GET_ARG_SIZE(arg)

#define TEE_SMC(fid)	xconcat(TEESMC64_, fid)

#else

#define teesmc_arg		teesmc32_arg
#define teesmc_param	teesmc32_param
#define TEESMC_GET_PARAMS(arg)		TEESMC32_GET_PARAMS(arg)
#define TEESMC_GET_ARG_SIZE(arg)	TEESMC32_GET_ARG_SIZE(arg)

#define TEE_SMC(fid)	xconcat(TEESMC32_, fid)
#endif

#endif
