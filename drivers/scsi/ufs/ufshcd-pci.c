/*
 * Universal Flash Storage Host controller PCI glue driver
 *
 * This code is based on drivers/scsi/ufs/ufshcd-pci.c
 * Copyright (C) 2011-2013 Samsung India Software Operations
 *
 * Authors:
 *	Santosh Yaraganavi <santosh.sy@samsung.com>
 *	Vinayak Holikatti <h.vinayak@samsung.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * See the COPYING file in the top-level directory or visit
 * <http://www.gnu.org/licenses/gpl-2.0.html>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * This program is provided "AS IS" and "WITH ALL FAULTS" and
 * without warranty of any kind. You are solely responsible for
 * determining the appropriateness of using and distributing
 * the program and assume all risks associated with your exercise
 * of rights with respect to the program, including but not limited
 * to infringement of third party rights, the risks and costs of
 * program errors, damage to or loss of data, programs or equipment,
 * and unavailability or interruption of operations. Under no
 * circumstances will the contributor of this Program be liable for
 * any damages of any kind arising from your use or distribution of
 * this program.
 */

#include "ufshcd.h"
#include <linux/pci.h>
#include <linux/pm_runtime.h>

static int ufs_intel_disable_lcc(struct ufs_hba *hba)
{
	u32 attr = UIC_ARG_MIB(PA_LOCAL_TX_LCC_ENABLE);
	u32 lcc_enable = 0;

	ufshcd_dme_get(hba, attr, &lcc_enable);
	if (lcc_enable)
		ufshcd_dme_set(hba, attr, 0);

	return 0;
}

static int ufs_intel_link_startup_notify(struct ufs_hba *hba,
					 enum ufs_notify_change_status status)
{
	int err = 0;

	switch (status) {
	case PRE_CHANGE:
		err = ufs_intel_disable_lcc(hba);
		break;
	case POST_CHANGE:
		break;
	default:
		break;
	}

	return err;
}

static int ufs_intel_pwr_change_notify(struct ufs_hba *hba,
				enum ufs_notify_change_status notify,
				struct ufs_pa_layer_attr *desired_pwr_info,
				struct ufs_pa_layer_attr *final_pwr_info)
{
	struct pci_dev *pdev = to_pci_dev(hba->dev);
	int ret = 0;

	if (!desired_pwr_info || !final_pwr_info) {
		ret = -EINVAL;
		goto out;
	}

	switch (notify) {
	case PRE_CHANGE:
		dev_dbg(hba->dev, "PWR change PRE_CHANGE start\n");
		memcpy(final_pwr_info, desired_pwr_info,
				sizeof(struct ufs_pa_layer_attr));

		if (pdev->device == 0x9DFA &&
		    (final_pwr_info->pwr_tx == FASTAUTO_MODE ||
		     final_pwr_info->pwr_tx == FAST_MODE ||
		     final_pwr_info->pwr_rx == FASTAUTO_MODE ||
		     final_pwr_info->pwr_rx == FAST_MODE)) {
			/* Currently, only RATE A is supported for HS mode */
			final_pwr_info->hs_rate = PA_HS_MODE_A;
		}
		break;
	case POST_CHANGE:
		break;
	default:
		ret = -EINVAL;
		break;
	}

out:
	return ret;
}

static struct ufs_hba_variant_ops ufs_intel_cnl_hba_vops = {
	.name                   = "intel-pci",
	.link_startup_notify	= ufs_intel_link_startup_notify,
	.pwr_change_notify	= ufs_intel_pwr_change_notify,
};

#ifdef CONFIG_PM_SLEEP
/**
 * ufshcd_pci_suspend - suspend power management function
 * @dev: pointer to PCI device handle
 *
 * Returns 0 if successful
 * Returns non-zero otherwise
 */
static int ufshcd_pci_suspend(struct device *dev)
{
	return ufshcd_system_suspend(dev_get_drvdata(dev));
}

/**
 * ufshcd_pci_resume - resume power management function
 * @dev: pointer to PCI device handle
 *
 * Returns 0 if successful
 * Returns non-zero otherwise
 */
static int ufshcd_pci_resume(struct device *dev)
{
	return ufshcd_system_resume(dev_get_drvdata(dev));
}
#endif /* !CONFIG_PM_SLEEP */

#ifdef CONFIG_PM
static int ufshcd_pci_runtime_suspend(struct device *dev)
{
	return ufshcd_runtime_suspend(dev_get_drvdata(dev));
}
static int ufshcd_pci_runtime_resume(struct device *dev)
{
	return ufshcd_runtime_resume(dev_get_drvdata(dev));
}
static int ufshcd_pci_runtime_idle(struct device *dev)
{
	return ufshcd_runtime_idle(dev_get_drvdata(dev));
}
#endif /* !CONFIG_PM */

/**
 * ufshcd_pci_shutdown - main function to put the controller in reset state
 * @pdev: pointer to PCI device handle
 */
static void ufshcd_pci_shutdown(struct pci_dev *pdev)
{
	ufshcd_shutdown((struct ufs_hba *)pci_get_drvdata(pdev));
}

/**
 * ufshcd_pci_remove - de-allocate PCI/SCSI host and host memory space
 *		data structure memory
 * @pdev: pointer to PCI handle
 */
static void ufshcd_pci_remove(struct pci_dev *pdev)
{
	struct ufs_hba *hba = pci_get_drvdata(pdev);

	pm_runtime_forbid(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);
	ufshcd_remove(hba);
	ufshcd_dealloc_host(hba);
}

/**
 * ufshcd_pci_probe - probe routine of the driver
 * @pdev: pointer to PCI device handle
 * @id: PCI device id
 *
 * Returns 0 on success, non-zero value on failure
 */
static int
ufshcd_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ufs_hba *hba;
	void __iomem *mmio_base;
	int err;

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "pcim_enable_device failed\n");
		return err;
	}

	pci_set_master(pdev);

	err = pcim_iomap_regions(pdev, 1 << 0, UFSHCD);
	if (err < 0) {
		dev_err(&pdev->dev, "request and iomap failed\n");
		return err;
	}

	mmio_base = pcim_iomap_table(pdev)[0];

	err = ufshcd_alloc_host(&pdev->dev, &hba);
	if (err) {
		dev_err(&pdev->dev, "Allocation failed\n");
		return err;
	}

	hba->vops = (struct ufs_hba_variant_ops *)id->driver_data;

	err = ufshcd_init(hba, mmio_base, pdev->irq);
	if (err) {
		dev_err(&pdev->dev, "Initialization failed\n");
		ufshcd_dealloc_host(hba);
		return err;
	}

	pci_set_drvdata(pdev, hba);
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_allow(&pdev->dev);

	return 0;
}

static const struct dev_pm_ops ufshcd_pci_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ufshcd_pci_suspend,
				ufshcd_pci_resume)
	SET_RUNTIME_PM_OPS(ufshcd_pci_runtime_suspend,
			   ufshcd_pci_runtime_resume,
			   ufshcd_pci_runtime_idle)
};

static const struct pci_device_id ufshcd_pci_tbl[] = {
	{ PCI_VENDOR_ID_SAMSUNG, 0xC00C, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ PCI_VDEVICE(INTEL, 0x9DFA), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ PCI_VDEVICE(INTEL, 0x34FA), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ PCI_VDEVICE(INTEL, 0x34FD), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ PCI_VDEVICE(INTEL, 0x38FA), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ PCI_VDEVICE(INTEL, 0xA0FA), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ PCI_VDEVICE(INTEL, 0xA0FF), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ PCI_VDEVICE(INTEL, 0x4B90), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ PCI_VDEVICE(INTEL, 0x4B95), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ PCI_VDEVICE(INTEL, 0x43FA), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ PCI_VDEVICE(INTEL, 0x43FF), (kernel_ulong_t)&ufs_intel_cnl_hba_vops },
	{ }	/* terminate list */
};

MODULE_DEVICE_TABLE(pci, ufshcd_pci_tbl);

static struct pci_driver ufshcd_pci_driver = {
	.name = UFSHCD,
	.id_table = ufshcd_pci_tbl,
	.probe = ufshcd_pci_probe,
	.remove = ufshcd_pci_remove,
	.shutdown = ufshcd_pci_shutdown,
	.driver = {
		.pm = &ufshcd_pci_pm_ops
	},
};

module_pci_driver(ufshcd_pci_driver);

MODULE_AUTHOR("Santosh Yaragnavi <santosh.sy@samsung.com>");
MODULE_AUTHOR("Vinayak Holikatti <h.vinayak@samsung.com>");
MODULE_DESCRIPTION("UFS host controller PCI glue driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(UFSHCD_DRIVER_VERSION);
