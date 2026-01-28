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

#ifndef TEE_CLIENT_API_H
#define TEE_CLIENT_API_H

/* for geniezone */
#define teec_result tkcore_TEEC_Result
#define teec_uuid tkcore_TEEC_UUID
#define teec_context tkcore_TEEC_Context
#define teec_session tkcore_TEEC_Session
#define teec_shared_memory tkcore_TEEC_SharedMemory
#define teec_temp_memory_reference tkcore_TEEC_TempMemoryReference
#define teec_registered_memory_reference tkcore_TEEC_RegisteredMemoryReference
#define teec_value tkcore_TEEC_Value
#define teec_parameter tkcore_TEEC_Parameter
#define teec_operation tkcore_TEEC_Operation

#define teec_initialize_context tkcore_TEEC_InitializeContext
#define teec_finalize_context tkcore_TEEC_FinalizeContext
#define teec_register_shared_memory tkcore_TEEC_RegisterSharedMemory
#define teec_allocate_shared_memory tkcore_TEEC_AllocateSharedMemory
#define teec_release_shared_memory tkcore_TEEC_ReleaseSharedMemory
#define teec_open_session tkcore_TEEC_OpenSession
#define teec_close_session tkcore_TEEC_CloseSession
#define teec_invoke_command tkcore_TEEC_InvokeCommand
#define teec_request_cancellation tkcore_TEEC_RequestCancellation

#include "tee_kernel_api.h"

#endif
