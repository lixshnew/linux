// SPDX-License-Identifier: GPL-2.0-only
/*******************************************************************************
  This contains the functions to handle the pci driver.

  Copyright (C) 2011-2012  Vayavya Labs Pvt Ltd


  Author: Rayagond Kokatanur <rayagond@vayavyalabs.com>
  Author: Giuseppe Cavallaro <peppe.cavallaro@st.com>
*******************************************************************************/

#include <linux/clk-provider.h>
#include <linux/pci.h>
#include <linux/dmi.h>

#include "stmmac.h"

struct stmmac_pci_info {
	int (*setup)(struct pci_dev *pdev, struct plat_stmmacenet_data *plat);
};

static void common_default_data(struct plat_stmmacenet_data *plat)
{
	plat->clk_csr = 2;	/* clk_csr_i = 20-35MHz & MDC = clk_csr_i/16 */
	plat->has_gmac = 1;
	plat->force_sf_dma_mode = 1;

	plat->mdio_bus_data->needs_reset = true;

	/* Set default value for multicast hash bins */
	plat->multicast_filter_bins = HASH_TABLE_SIZE;

	/* Set default value for unicast filter entries */
	plat->unicast_filter_entries = 1;

	/* Set the maxmtu to a default of JUMBO_LEN */
	plat->maxmtu = JUMBO_LEN;

	/* Set default number of RX and TX queues to use */
	plat->tx_queues_to_use = 1;
	plat->rx_queues_to_use = 1;

	/* Disable Priority config by default */
	plat->tx_queues_cfg[0].use_prio = false;
	plat->rx_queues_cfg[0].use_prio = false;

	/* Disable RX queues routing by default */
	plat->rx_queues_cfg[0].pkt_route = 0x0;
}

static int stmmac_default_data(struct pci_dev *pdev,
			       struct plat_stmmacenet_data *plat)
{
	/* Set common default data first */
	common_default_data(plat);

	plat->bus_id = 1;
	plat->phy_addr = 0;
	plat->phy_interface = PHY_INTERFACE_MODE_GMII;

	plat->dma_cfg->pbl = 32;
	plat->dma_cfg->pblx8 = true;
	/* TODO: AXI */

	return 0;
}

static const struct stmmac_pci_info stmmac_pci_info = {
	.setup = stmmac_default_data,
};

static int snps_gmac5_default_data(struct pci_dev *pdev,
				   struct plat_stmmacenet_data *plat)
{
	int i;

	plat->clk_csr = 5;
	plat->has_gmac4 = 1;
	plat->force_sf_dma_mode = 1;
	plat->tso_en = 1;
	plat->pmt = 1;

	/* Set default value for multicast hash bins */
	plat->multicast_filter_bins = HASH_TABLE_SIZE;

	/* Set default value for unicast filter entries */
	plat->unicast_filter_entries = 1;

	/* Set the maxmtu to a default of JUMBO_LEN */
	plat->maxmtu = JUMBO_LEN;

	/* Set default number of RX and TX queues to use */
	plat->tx_queues_to_use = 4;
	plat->rx_queues_to_use = 4;

	plat->tx_sched_algorithm = MTL_TX_ALGORITHM_WRR;
	for (i = 0; i < plat->tx_queues_to_use; i++) {
		plat->tx_queues_cfg[i].use_prio = false;
		plat->tx_queues_cfg[i].mode_to_use = MTL_QUEUE_DCB;
		plat->tx_queues_cfg[i].weight = 25;
		if (i > 0)
			plat->tx_queues_cfg[i].tbs_en = 1;
	}

	plat->rx_sched_algorithm = MTL_RX_ALGORITHM_SP;
	for (i = 0; i < plat->rx_queues_to_use; i++) {
		plat->rx_queues_cfg[i].use_prio = false;
		plat->rx_queues_cfg[i].mode_to_use = MTL_QUEUE_DCB;
		plat->rx_queues_cfg[i].pkt_route = 0x0;
		plat->rx_queues_cfg[i].chan = i;
	}

	plat->bus_id = 1;
	plat->phy_addr = -1;
	plat->phy_interface = PHY_INTERFACE_MODE_GMII;

	plat->dma_cfg->pbl = 32;
	plat->dma_cfg->pblx8 = true;

	/* Axi Configuration */
	plat->axi = devm_kzalloc(&pdev->dev, sizeof(*plat->axi), GFP_KERNEL);
	if (!plat->axi)
		return -ENOMEM;

	plat->axi->axi_wr_osr_lmt = 31;
	plat->axi->axi_rd_osr_lmt = 31;

	plat->axi->axi_fb = false;
	plat->axi->axi_blen[0] = 4;
	plat->axi->axi_blen[1] = 8;
	plat->axi->axi_blen[2] = 16;
	plat->axi->axi_blen[3] = 32;

	return 0;
}

static const struct stmmac_pci_info snps_gmac5_pci_info = {
	.setup = snps_gmac5_default_data,
};

static int loongson_default_data(struct pci_dev *pdev,
				struct plat_stmmacenet_data *plat)
{
	struct device_node *np = pdev->dev.of_node;

	/* Set common default data first */
	common_default_data(plat);

	plat->bus_id = PCI_DEVID(pdev->bus->number, pdev->devfn);
	plat->phy_addr = -1;
	plat->interface = PHY_INTERFACE_MODE_RGMII_ID;

	plat->dma_cfg->pbl = 32;
	plat->dma_cfg->pblx8 = true;
	/* ls2h,ls2k,ls7a mcast filter register is 256bit. */
	plat->multicast_filter_bins = 256;
	/* AXI (TODO) */
	plat->clk_ref_rate = 125000000;
	plat->clk_ptp_rate = 125000000;

	if (of_property_read_u32(np, "max-speed", &plat->max_speed))
		plat->max_speed = -1;

	return 0;
}

static struct stmmac_pci_info loongson_pci_info = {
	.setup = loongson_default_data,
};

static void loongson_gnet_fix_speed(void *priv, unsigned int speed)
{
	static bool gnet_speed_flag;
	struct device *dev = priv;
	struct net_device *ndev = (struct net_device *)dev_get_drvdata(dev);

	if ((speed == SPEED_1000) && !gnet_speed_flag) {
		gnet_speed_flag = true;
		/* reset phy */
		phy_set_bits(ndev->phydev, 0 /*MII_BMCR*/, 0x200 /*BMCR_ANRESTART*/);
	} else {
		gnet_speed_flag = false;
	}
}

static int loongson_gnet_data(struct pci_dev *pdev,
				struct plat_stmmacenet_data *plat)
{
	struct device_node *np = pdev->dev.of_node;

	/* Set common default data first */
	common_default_data(plat);

	plat->bus_id = PCI_DEVID(pdev->bus->number, pdev->devfn);

	/* 7A2000 GNET internal PHY address is fixed */
	plat->mdio_bus_data->phy_mask = 0xfffffffb;
	plat->phy_addr = 2;

	/* 7A2000 GNET should use PHY_INTERFACE_MODE_GMII */
	plat->interface = PHY_INTERFACE_MODE_GMII;

	plat->dma_cfg->pbl = 32;
	plat->dma_cfg->pblx8 = true;
	/* 7A2000 mcast filter register is 256bit. */
	plat->multicast_filter_bins = 256;

	/* GNET 1000M speed need workaround */
	plat->fix_mac_speed = loongson_gnet_fix_speed;
	/* used to get netdev pointer address */
	plat->bsp_priv = (void *)&pdev->dev;

	plat->clk_ref_rate = 125000000;
	plat->clk_ptp_rate = 125000000;

	if (of_property_read_u32(np, "max-speed", &plat->max_speed))
		plat->max_speed = -1;

	return 0;
}

static struct stmmac_pci_info loongson_gnet_pci_info = {
	.setup = loongson_gnet_data,
};

/**
 * stmmac_pci_probe
 *
 * @pdev: pci device pointer
 * @id: pointer to table of device id/id's.
 *
 * Description: This probing function gets called for all PCI devices which
 * match the ID table and are not "owned" by other driver yet. This function
 * gets passed a "struct pci_dev *" for each device whose entry in the ID table
 * matches the device. The probe functions returns zero when the driver choose
 * to take "ownership" of the device or an error code(-ve no) otherwise.
 */
static int stmmac_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct stmmac_pci_info *info = (struct stmmac_pci_info *)id->driver_data;
	struct plat_stmmacenet_data *plat;
	struct stmmac_resources res;
	int i;
	int ret;

	plat = devm_kzalloc(&pdev->dev, sizeof(*plat), GFP_KERNEL);
	if (!plat)
		return -ENOMEM;

	plat->mdio_bus_data = devm_kzalloc(&pdev->dev,
					   sizeof(*plat->mdio_bus_data),
					   GFP_KERNEL);
	if (!plat->mdio_bus_data)
		return -ENOMEM;

	plat->dma_cfg = devm_kzalloc(&pdev->dev, sizeof(*plat->dma_cfg),
				     GFP_KERNEL);
	if (!plat->dma_cfg)
		return -ENOMEM;

	/* Enable pci device */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "%s: ERROR: failed to enable device\n",
			__func__);
		return ret;
	}

	/* Get the base address of device */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (pci_resource_len(pdev, i) == 0)
			continue;
		ret = pcim_iomap_regions(pdev, BIT(i), pci_name(pdev));
		if (ret)
			return ret;
		break;
	}

	pci_set_master(pdev);

	ret = info->setup(pdev, plat);
	if (ret)
		return ret;

	pci_enable_msi(pdev);

	memset(&res, 0, sizeof(res));
	res.addr = pcim_iomap_table(pdev)[i];
	res.wol_irq = pdev->irq;
	res.irq = pdev->irq;

	return stmmac_dvr_probe(&pdev->dev, plat, &res);
}

/**
 * stmmac_pci_remove
 *
 * @pdev: platform device pointer
 * Description: this function calls the main to free the net resources
 * and releases the PCI resources.
 */
static void stmmac_pci_remove(struct pci_dev *pdev)
{
	int i;

	stmmac_dvr_remove(&pdev->dev);

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (pci_resource_len(pdev, i) == 0)
			continue;
		pcim_iounmap_regions(pdev, BIT(i));
		break;
	}

	pci_disable_device(pdev);
}

static int __maybe_unused stmmac_pci_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	ret = stmmac_suspend(dev);
	if (ret)
		return ret;

	ret = pci_save_state(pdev);
	if (ret)
		return ret;

	pci_disable_device(pdev);
	pci_wake_from_d3(pdev, true);
	return 0;
}

static int __maybe_unused stmmac_pci_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	pci_restore_state(pdev);
	pci_set_power_state(pdev, PCI_D0);

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	return stmmac_resume(dev);
}

static SIMPLE_DEV_PM_OPS(stmmac_pm_ops, stmmac_pci_suspend, stmmac_pci_resume);

/* synthetic ID, no official vendor */
#define PCI_VENDOR_ID_STMMAC		0x0700

#define PCI_DEVICE_ID_STMMAC_STMMAC		0x1108
#define PCI_DEVICE_ID_SYNOPSYS_GMAC5_ID		0x7102

static const struct pci_device_id stmmac_id_table[] = {
	{ PCI_DEVICE_DATA(STMMAC, STMMAC, &stmmac_pci_info) },
	{ PCI_DEVICE_DATA(STMICRO, MAC, &stmmac_pci_info) },
	{ PCI_DEVICE_DATA(SYNOPSYS, GMAC5_ID, &snps_gmac5_pci_info) },
	{ PCI_DEVICE_DATA(LOONGSON, GMAC, &loongson_pci_info) },
	{ PCI_DEVICE_DATA(LOONGSON, GNET, &loongson_gnet_pci_info) },
	{}
};

MODULE_DEVICE_TABLE(pci, stmmac_id_table);

static struct pci_driver stmmac_pci_driver = {
	.name = STMMAC_RESOURCE_NAME,
	.id_table = stmmac_id_table,
	.probe = stmmac_pci_probe,
	.remove = stmmac_pci_remove,
	.driver         = {
		.pm     = &stmmac_pm_ops,
	},
};

module_pci_driver(stmmac_pci_driver);

MODULE_DESCRIPTION("STMMAC 10/100/1000 Ethernet PCI driver");
MODULE_AUTHOR("Rayagond Kokatanur <rayagond.kokatanur@vayavyalabs.com>");
MODULE_AUTHOR("Giuseppe Cavallaro <peppe.cavallaro@st.com>");
MODULE_LICENSE("GPL");
