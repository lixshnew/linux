# SPDX-License-Identifier: GPL-2.0

loongson-y := \
	lsdc_drv.o \
	lsdc_crtc.o \
	lsdc_irq.o \
	lsdc_plane.o \
	lsdc_pll.o \
	lsdc_i2c.o \
	lsdc_output.o \
	lsdc_platform_drv.o

loongson-$(CONFIG_DRM_LOONGSON_PCI_DRIVER) += lsdc_pci_drv.o
loongson-$(CONFIG_DEBUG_FS) += lsdc_debugfs.o

obj-$(CONFIG_DRM_LOONGSON) += loongson.o
