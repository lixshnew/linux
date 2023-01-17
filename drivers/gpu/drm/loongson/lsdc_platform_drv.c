// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2020 Loongson Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 */

/*
 * Authors:
 *      Sui Jingfeng <suijingfeng@loongson.cn>
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/console.h>

#include <drm/drm_print.h>
#include <drm/drm_drv.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>

#include <linux/of_device.h>

#include "lsdc_drv.h"

static const struct lsdc_chip_desc dc_in_ls2k1000 = {
	.chip = LSDC_CHIP_2K1000,
	.num_of_crtc = LSDC_NUM_CRTC,
	/* ls2k1000 user manual say the max pixel clock can be about 200MHz */
	.max_pixel_clk = 200000,
	.max_width = 2560,
	.max_height = 2048,
	.num_of_hw_cursor = 1,
	.hw_cursor_w = 32,
	.hw_cursor_h = 32,
	.stride_alignment = 256,
	.has_builtin_i2c = false,
	.has_vram = false,
	.broken_gamma = true,
};

static const struct lsdc_chip_desc dc_in_ls2k0500 = {
	.chip = LSDC_CHIP_2K0500,
	.num_of_crtc = LSDC_NUM_CRTC,
	.max_pixel_clk = 200000,
	.max_width = 2048,
	.max_height = 2048,
	.num_of_hw_cursor = 1,
	.hw_cursor_w = 32,
	.hw_cursor_h = 32,
	.stride_alignment = 256,
	.has_builtin_i2c = false,
	.has_vram = false,
	.broken_gamma = true,
};

const struct lsdc_chip_desc *
lsdc_detect_platform_chip(struct platform_device *pdev)
{
	struct device_node *np;
	const struct lsdc_chip_desc *desc = NULL;

	desc = of_device_get_match_data(&pdev->dev);
	if (desc && desc->chip < LSDC_CHIP_LAST) {
		DRM_INFO("%s DC found\n", desc->chip == LSDC_CHIP_2K1000 ? "LS2K1000" :
					desc->chip == LSDC_CHIP_7A1000 ? "LS7A1000" :
					desc->chip == LSDC_CHIP_2K0500 ? "LS2K0500" :
					desc->chip == LSDC_CHIP_7A2000 ? "LS7A2000" : 
					"UNKNOWN");
		return desc;
	}

	for_each_compatible_node(np, NULL, "loongson,ls2k") {
		const char *model = NULL;

		if (!of_device_is_available(np))
			continue;

		of_property_read_string(np, "model", &model);
		if (!strncmp(model, "loongson,2k500", 27)) {
			desc = &dc_in_ls2k0500;
			DRM_INFO("LS2K0500 DC found\n");
		} else {
			desc = &dc_in_ls2k1000;
			DRM_INFO("LS2K1000 DC found\n");
		}

		DRM_INFO("Loongson 2K series SoC detected.\n");

		of_node_put(np);

		break;
	}

	return desc;
}

static int lsdc_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lsdc_device *ldev;
	struct resource *res;
	int ret;

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "Failed to set dma mask\n");
		return ret;
	}

	ldev = kzalloc(sizeof(struct lsdc_device), GFP_KERNEL);
	if (ldev == NULL)
		return -ENOMEM;

	ldev->desc = lsdc_detect_platform_chip(pdev);
	if (ldev->desc == NULL) {
		dev_err(dev, "unknown dc chip core\n");
		goto err_free;
	}

	ldev->dev = dev;
	dev_set_drvdata(dev, ldev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ldev->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(ldev->reg_base)) {
		dev_err(dev, "Unable to get lsdc registers\n");
		ret = PTR_ERR(ldev->reg_base);
		goto err_free;
	}

	ldev->irq = platform_get_irq(pdev, 0);
	if (ldev->irq < 0) {
		dev_err(dev, "failed to get irq\n");
		ret = ldev->irq;
		goto err_free;
	}

	if (dev->of_node) {
		ldev->relax_alignment = 
			of_property_read_bool(dev->of_node, "lsdc,relax_alignment");
	}

	ret = lsdc_common_probe(ldev);
	if (ret) {
		goto err_free;
	}

	return 0;

err_free:
	kfree(ldev);
	return ret;
}

static int lsdc_platform_remove(struct platform_device *pdev)
{
	struct lsdc_device *ldev = dev_get_drvdata(&pdev->dev);

	lsdc_common_remove(ldev);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

#ifdef CONFIG_PM

static int lsdc_drm_suspend(struct device *dev)
{
	struct lsdc_device *ldev = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(ldev->ddev);
}

static int lsdc_drm_resume(struct device *dev)
{
	struct lsdc_device *ldev = dev_get_drvdata(dev);

	return drm_mode_config_helper_resume(ldev->ddev);
}

#endif

static SIMPLE_DEV_PM_OPS(lsdc_pm_ops, lsdc_drm_suspend, lsdc_drm_resume);

static const struct of_device_id lsdc_dt_ids[] = {
	{ .compatible = "loongson,display-subsystem", },
	{ .compatible = "loongson,ls-fb", },
	{ .compatible = "loongson,ls2k1000-dc", .data = (void *)&dc_in_ls2k1000, },
	{ .compatible = "loongson,ls2k0500-dc", .data = (void *)&dc_in_ls2k0500, },
	{}
};
MODULE_DEVICE_TABLE(of, lsdc_dt_ids);

struct platform_driver lsdc_platform_driver = {
	.probe = lsdc_platform_probe,
	.remove = lsdc_platform_remove,
	.driver = {
		.name = DRIVER_NAME,
		.pm = &lsdc_pm_ops,
		.of_match_table = of_match_ptr(lsdc_dt_ids),
	},
};

static int __init lsdc_init(void)
{
	int ret;

	if (vgacon_text_force()) {
		DRM_ERROR("VGACON disables loongson kernel modesetting.\n");
		return -EINVAL;
	}

	ret = platform_driver_register(&lsdc_platform_driver);

	return ret;
}
module_init(lsdc_init);

static void __exit lsdc_exit(void)
{
	platform_driver_unregister(&lsdc_platform_driver);
}
module_exit(lsdc_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL v2");
