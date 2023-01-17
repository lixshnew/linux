// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022 Loongson Corporation
 */

/*
 * Authors:
 *      Sui Jingfeng <suijingfeng@loongson.cn>
 */

#include <linux/pci.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include <drm/drm_vblank.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_probe_helper.h>

#include "lsdc_drv.h"
#include "lsdc_irq.h"
#include "lsdc_output.h"
#include "lsdc_debugfs.h"


static int lsdc_use_vram_helper = -1;
MODULE_PARM_DESC(use_vram_helper, "Using vram helper based driver(0 = disabled)");
module_param_named(use_vram_helper, lsdc_use_vram_helper, int, 0644);

static int lsdc_gamma = -1;
MODULE_PARM_DESC(gamma, "enable gamma (-1 = disabled (default), >0 = enabled)");
module_param_named(gamma, lsdc_gamma, int, 0644);

static int lsdc_relax_alignment = -1;
MODULE_PARM_DESC(relax_alignment,
		 "relax crtc stride alignment (-1 = disabled (default), >0 = enabled)");
module_param_named(relax_alignment, lsdc_relax_alignment, int, 0644);


static enum drm_mode_status
lsdc_device_mode_valid(struct drm_device *ddev, const struct drm_display_mode *mode)
{
	struct lsdc_device *ldev = to_lsdc(ddev);

	if (ldev->use_vram_helper)
		return drm_vram_helper_mode_valid(ddev, mode);

	return MODE_OK;
}

static const struct drm_mode_config_funcs lsdc_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.output_poll_changed = drm_fb_helper_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
	.mode_valid = lsdc_device_mode_valid,
};

static int lsdc_gem_cma_dumb_create(struct drm_file *file,
				    struct drm_device *ddev,
				    struct drm_mode_create_dumb *args)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	const struct lsdc_chip_desc *desc = ldev->desc;
	unsigned int bytes_per_pixel = (args->bpp + 7) / 8;
	unsigned int pitch = bytes_per_pixel * args->width;

	/*
	 * The dc in ls7a1000/ls2k1000/ls2k0500 require the stride be a
	 * multiple of 256 bytes which is for sake of optimize dma data
	 * transfer.
	 */
	args->pitch = roundup(pitch, desc->stride_alignment);

	return drm_gem_cma_dumb_create_internal(file, ddev, args);
}

DEFINE_DRM_GEM_CMA_FOPS(lsdc_drv_fops);

static struct drm_driver lsdc_drm_driver_cma_stub = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.lastclose = drm_fb_helper_lastclose,
	.fops = &lsdc_drv_fops,

	.name = "lsdc",
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,

	DRM_GEM_CMA_DRIVER_OPS_WITH_DUMB_CREATE(lsdc_gem_cma_dumb_create),

#ifdef CONFIG_DEBUG_FS
	.debugfs_init = lsdc_debugfs_init,
#endif
};

DEFINE_DRM_GEM_FOPS(lsdc_gem_fops);

static struct drm_driver lsdc_vram_driver_stub = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops = &lsdc_gem_fops,

	.name = "loongson-drm",
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init = lsdc_vram_mm_debugfs_init,
#endif
	.dumb_create = drm_gem_vram_driver_dumb_create,
	.dumb_map_offset = drm_gem_vram_driver_dumb_mmap_offset,
	.gem_prime_mmap = drm_gem_prime_mmap,
};

static int lsdc_modeset_init(struct lsdc_device *ldev, uint32_t num_crtc)
{
	struct drm_device *ddev = ldev->ddev;
	unsigned int i;
	int ret;

	if (ldev->has_ports_node) {
		drm_info(ddev, "Has OF graph support\n");
		ret = lsdc_attach_output(ldev, num_crtc);
		if (ret)
			return ret;
	} else {
		drm_info(ddev, "No OF graph support\n");
		for (i = 0; i < num_crtc; i++) {
			ret = lsdc_create_output(ldev, i, num_crtc);
			if (ret)
				return ret;
		}
	}

	for (i = 0; i < num_crtc; i++) {
		struct lsdc_display_pipe * const dispipe = &ldev->dispipe[i];
		struct drm_plane * const primary = &dispipe->primary;
		struct drm_plane * const cursor = &dispipe->cursor;
		struct drm_crtc * const crtc = &dispipe->crtc;
		struct lsdc_pll * const pixpll = &dispipe->pixpll;

		dispipe->index = i;

		ret = lsdc_pixpll_init(pixpll, ddev, i);
		if (ret)
			return ret;

		ret = lsdc_plane_init(ldev, primary, DRM_PLANE_TYPE_PRIMARY, i);
		if (ret)
			return ret;

		ret = lsdc_plane_init(ldev, cursor, DRM_PLANE_TYPE_CURSOR, i);
		if (ret)
			return ret;

		/*
		 * Initial all of the CRTC available, in this way the crtc
		 * index is equal to the hardware crtc index. That is:
		 * display pipe 0 => crtc0 + dvo0 + encoder0
		 * display pipe 1 => crtc1 + dvo1 + encoder1
		 */
		ret = lsdc_crtc_init(ddev, crtc, i, primary, cursor);
		if (ret)
			return ret;

		drm_info(ddev, "display pipe %u initialized\n", i);
	}

	return 0;
}

static int lsdc_mode_config_init(struct lsdc_device *ldev)
{
	const struct lsdc_chip_desc * const descp = ldev->desc;
	struct drm_device *ddev = ldev->ddev;
	int ret;

	ret = drmm_mode_config_init(ddev);
	if (ret)
		return ret;

	ddev->mode_config.funcs = &lsdc_mode_config_funcs;
	ddev->mode_config.min_width = 1;
	ddev->mode_config.min_height = 1;
	ddev->mode_config.max_width = 4096;
	ddev->mode_config.max_height = 4096;
	ddev->mode_config.preferred_depth = 24;
	ddev->mode_config.prefer_shadow = ldev->use_vram_helper;

	ddev->mode_config.cursor_width = descp->hw_cursor_h;
	ddev->mode_config.cursor_height = descp->hw_cursor_h;

	if (ldev->vram_base)
		ddev->mode_config.fb_base = ldev->vram_base;

	return lsdc_modeset_init(ldev, descp->num_of_crtc);
}

static void lsdc_mode_config_fini(struct drm_device *ddev)
{
	drm_atomic_helper_shutdown(ddev);

	drm_mode_config_cleanup(ddev);
}

static int lsdc_remove_conflicting_framebuffers(const struct drm_driver *drv)
{
	struct apertures_struct *ap;

	ap = alloc_apertures(1);
	if (!ap)
		return -ENOMEM;

	/* lsdc is a pci device, but it don't have a dedicate vram bar because
	 * of historic reason. The display controller is ported from Loongson
	 * 2H series SoC which date back to 2012.
	 * And simplefb node may have been located anywhere in memory.
	 */

	ap->ranges[0].base = 0;
	ap->ranges[0].size = ~0;

	return drm_fb_helper_remove_conflicting_framebuffers(ap, "loongsondrmfb", false);
}

static void lsdc_of_probe(struct lsdc_device *ldev, struct device_node *np)
{
	struct device_node *ports;

	if (!np) {
		ldev->has_dt = false;
		ldev->has_ports_node = false;
		dev_info(ldev->dev, "don't has DT support\n");
		return;
	}

	ldev->has_dt = true;
	ports = of_get_child_by_name(np, "ports");
	ldev->has_ports_node = ports ? true : false;
	of_node_put(ports);
}

int lsdc_common_probe(struct lsdc_device *ldev)
{
	struct drm_driver *driver;
	struct drm_device *ddev;
	int ret;

	if (lsdc_use_vram_helper > 0)
		ldev->use_vram_helper = true;
	else if ((lsdc_use_vram_helper < 0) && ldev->desc->has_vram)
		ldev->use_vram_helper = true;
	else
		ldev->use_vram_helper = false;

	if (!ldev->desc->broken_gamma)
		ldev->enable_gamma = true;
	else
		ldev->enable_gamma = lsdc_gamma > 0 ? true : false;

	if (!ldev->relax_alignment)
		ldev->relax_alignment = lsdc_relax_alignment > 0 ? true : false;

	lsdc_of_probe(ldev, ldev->dev->of_node);

	if (ldev->has_dt) {
		/* Get the optional framebuffer memory resource */
		ret = of_reserved_mem_device_init(ldev->dev);
		if (ret && (ret != -ENODEV))
			return ret;
	}

	if (ldev->use_vram_helper)
		driver = &lsdc_vram_driver_stub;
	else
		driver = &lsdc_drm_driver_cma_stub;

	lsdc_remove_conflicting_framebuffers(driver);

	ddev = drm_dev_alloc(driver, ldev->dev);
	if (IS_ERR(ddev))
		return PTR_ERR(ddev);

	ldev->ddev = ddev;
	ddev->dev_private = ldev;

	if (ldev->use_vram_helper) {
		ret = drmm_vram_helper_init(ddev, ldev->vram_base, ldev->vram_size);
		if (ret) {
			drm_err(ddev, "vram helper init failed: %d\n", ret);
			goto err_kms;
		}
	};

	ret = lsdc_mode_config_init(ldev);
	if (ret) {
		drm_dbg(ddev, "%s: %d\n", __func__, ret);
		goto err_kms;
	}

	ret = devm_request_threaded_irq(ldev->dev, ldev->irq,
					lsdc_irq_handler_cb,
					lsdc_irq_thread_cb,
					IRQF_ONESHOT, NULL,
					ddev);
	if (ret) {
		drm_err(ddev, "Failed to register lsdc interrupt\n");
		goto err_kms;
	}

	ret = drm_vblank_init(ddev, ldev->desc->num_of_crtc);
	if (ret)
		goto err_kms;

	drm_mode_config_reset(ddev);

	drm_kms_helper_poll_init(ddev);

	ret = drm_dev_register(ddev, 0);
	if (ret)
		goto err_poll_fini;

	drm_fbdev_generic_setup(ddev, 32);

	return 0;

err_poll_fini:
	drm_kms_helper_poll_fini(ddev);
err_kms:
	drm_dev_put(ddev);

	return ret;
}

int lsdc_common_remove(struct lsdc_device *ldev)
{
	struct drm_device *ddev = ldev->ddev;

	drm_dev_unregister(ddev);

	drm_kms_helper_poll_fini(ddev);

	devm_free_irq(ddev->dev, ldev->irq, ddev);

	lsdc_mode_config_fini(ddev);

	drm_dev_put(ddev);

	return 0;
}

