# Copyright (c) 2015-2018 TrustKernel Incorporated
# All Rights Reserved.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# version 2 as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.

obj-$(CONFIG_TRUSTKERNEL_TEE_SUPPORT) += tkcore.o

ifneq ($(DEVICE_MODULES_PATH),)
TKCORE_ROOT := $(DEVICE_MODULES_PATH)/drivers/tee/tkcore
else
TKCORE_ROOT := $(srctree)/drivers/tee/tkcore
endif

ccflags-y += \
	-I$(TKCORE_ROOT)/include/linux \
	-I$(TKCORE_ROOT)/include \
	-I$(TKCORE_ROOT)

tkcore-y := \
	tee_core.o \
	tee_context.o \
	tee_session.o \
	tee_shm.o \
	tee_supp_com.o \
	tee_sysfs.o \
	tee_kernel_api.o \
	tee_wait_queue.o \
	tee_procfs.o \
	plat.o

tkcore-$(if $(filter y m,$(CONFIG_TRUSTKERNEL_TEE_FP_SUPPORT)),y,n) += peridev.o
