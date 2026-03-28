# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the input core drivers.
#

# Each configuration option enables a list of files.
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/
ccflags-$(CONFIG_DRM_MEDIATEK_V2) += -I$(srctree)/drivers/gpu/drm/mediatek/mediatek_v2/
obj-$(CONFIG_KEYBOARD_BBQX0_KEYBOARD) += bbqX0kbd.o
bbqX0kbd-y   +=  bbqX0kbd_main.o
bbqX0kbd-y   +=  bbqX0kbd_i2cHelper.o
