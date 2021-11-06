/**
 * @file axi_dma.c
 * @date Saturday, November 14, 2015 at 11:20:00 AM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This file contains the top level functions for AXI DMA module.
 *
 * @bug No known bugs.
 **/

// Kernel dependencies
#include <linux/module.h>           // Module init and exit macros
#include <linux/moduleparam.h>      // Module param macro
#include <linux/of.h>               // Struct of_device_id
#include <linux/slab.h>             // Allocation functions
#include <linux/stat.h>             // Module parameter permission values
#include <linux/platform_device.h>  // Platform device definitions

// Local dependencies
#include "axidma.h"                 // Internal definitions

/*----------------------------------------------------------------------------
 * Module Parameters
 *----------------------------------------------------------------------------*/

// The name to use for the character device. This is "axidma" by default.
static char *chrdev_name = CHRDEV_NAME;
module_param(chrdev_name, charp, S_IRUGO);

// The minor number to use for the character device. 0 by default.
static int minor_num = MINOR_NUMBER;
module_param(minor_num, int, S_IRUGO);

/*----------------------------------------------------------------------------
 * Platform Device Functions
 *----------------------------------------------------------------------------*/

static int axidma_probe(struct platform_device *pdev)
{
    int rc;
    struct axidma_device *axidma_dev;

    // Allocate a AXI DMA device structure to hold metadata about the DMA
    axidma_dev = kmalloc(sizeof(*axidma_dev), GFP_KERNEL);
    if (axidma_dev == NULL) {
        axidma_err("Unable to allocate the AXI DMA device structure.\n");
        return -ENOMEM;
    }
    axidma_dev->pdev = pdev;

    // Initialize the DMA interface
    rc = axidma_dma_init(pdev, axidma_dev);
    if (rc < 0) {
        goto free_axidma_dev;
    }

    // Assign the character device name, minor number, and number of devices
    axidma_dev->chrdev_name = chrdev_name;
    axidma_dev->minor_num = minor_num;
    axidma_dev->num_devices = NUM_DEVICES;

    // Initialize the character device for the module.
    rc = axidma_chrdev_init(axidma_dev);
    if (rc < 0) {
        goto destroy_dma_dev;
    }

    // Set the private data in the device to the AXI DMA device structure
    dev_set_drvdata(&pdev->dev, axidma_dev);
    return 0;

destroy_dma_dev:
    axidma_dma_exit(axidma_dev);
free_axidma_dev:
    kfree(axidma_dev);
    return -ENOSYS;
}

static int axidma_remove(struct platform_device *pdev)
{
    struct axidma_device *axidma_dev;

    // Get the AXI DMA device structure from the device's private data
    axidma_dev = dev_get_drvdata(&pdev->dev);

    // Cleanup the character device structures
    axidma_chrdev_exit(axidma_dev);

    // Cleanup the DMA structures
    axidma_dma_exit(axidma_dev);

    // Free the device structure
    kfree(axidma_dev);
    return 0;
}

static const struct of_device_id axidma_compatible_of_ids[] = {
    { .compatible = "xlnx,axidma-chrdev" },
    {}
};

static struct platform_driver axidma_driver = {
    .driver = {
        .name = MODULE_NAME,
        .owner = THIS_MODULE,
        .of_match_table = axidma_compatible_of_ids,
    },
    .probe = axidma_probe,
    .remove = axidma_remove,
};

/*----------------------------------------------------------------------------
 * Module Initialization and Exit
 *----------------------------------------------------------------------------*/

static int __init axidma_init(void)
{
    return platform_driver_register(&axidma_driver);
}

static void __exit axidma_exit(void)
{
    return platform_driver_unregister(&axidma_driver);
}

module_init(axidma_init);
module_exit(axidma_exit);

MODULE_AUTHOR("Brandon Perez");
MODULE_AUTHOR("Jared Choi");

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Module to provide a userspace interface for transferring "
                   "data from the processor to the logic fabric via AXI DMA.");
