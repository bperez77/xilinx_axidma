/**
 * @file axidma_of.c
 * @date Wednesday, April 13, 2016 at 10:59:45 AM EDT
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This file contains functions for parsing the relevant device tree entries for
 * the DMA engines that are used.
 *
 * @bug No known bugs.
 **/

// Kernel Dependencies
#include <linux/of.h>               // Device tree parsing functions
#include <linux/platform_device.h>  // Platform device definitions

// Local Dependencies
#include "axidma.h"                 // Internal Definitions

/*----------------------------------------------------------------------------
 * Internal Helper Functions
 *----------------------------------------------------------------------------*/

static int axidma_parse_compatible_property(struct device_node *dma_chan_node,
        struct axidma_chan *chan, struct axidma_device *dev)
{
    struct device_node *np;

    // Shorten the name for the dma_chan_node
    np = dma_chan_node;

    // Determine if the channel is DMA or VDMA, and if it is transmit or receive
    if (of_device_is_compatible(np, "xlnx,axi-dma-mm2s-channel") > 0) {
        chan->type = AXIDMA_DMA;
        chan->dir = AXIDMA_WRITE;
        dev->num_dma_tx_chans += 1;
    } else if (of_device_is_compatible(np, "xlnx,axi-dma-s2mm-channel") > 0) {
        chan->type = AXIDMA_DMA;
        chan->dir = AXIDMA_READ;
        dev->num_dma_rx_chans += 1;
    } else if (of_device_is_compatible(np, "xlnx,axi-vdma-mm2s-channel") > 0) {
        chan->type = AXIDMA_VDMA;
        chan->dir = AXIDMA_WRITE;
        dev->num_vdma_tx_chans += 1;
    } else if (of_device_is_compatible(np, "xlnx,axi-vdma-s2mm-channel") > 0) {
        chan->type = AXIDMA_VDMA;
        chan->dir = AXIDMA_READ;
        dev->num_vdma_rx_chans += 1;
    } else if (of_find_property(np, "compatible", NULL) == NULL) {
        axidma_node_err(np, "DMA channel lacks 'compatible' property.\n");
    } else {
        axidma_node_err(np, "DMA channel has an invalid 'compatible' "
                        "property.\n");
        axidma_err("The 'compatible' property must be one of: {"
                   "xlnx,axi-dma-mm2s-channel, xlnx,axi-dma-mm2s-channel, "
                   "xlnx,axi-dma-mm2s-channel, xlnx,axi-dma-mm2s-channel}.\n");
        return -EINVAL;
    }

    return 0;
}

static int axidma_of_parse_dma_name(struct device_node *driver_node, int index,
                                    struct axidma_chan *chan)
{
    int rc;

    // Parse the index'th dma name from the 'dma-names' property
    rc = of_property_read_string_index(driver_node, "dma-names", index,
                                       &chan->name);
    if (rc < 0) {
        axidma_node_err(driver_node, "Unable to read DMA name %d from the "
                        "'dma-names' property.\n", index);
        return -EINVAL;
    }

    return 0;
}

static int axidma_of_parse_channel(struct device_node *dma_node, int channel,
        struct axidma_chan *chan, struct axidma_device *dev)
{
    int rc;
    struct device_node *dma_chan_node;
    u32 channel_id;

    // Verify that the DMA node has two channel (child) nodes, one for TX and RX
    if (of_get_child_count(dma_node) < 1) {
        axidma_node_err(dma_node, "DMA does not have any channel nodes.\n");
        return -EINVAL;
    } else if (of_get_child_count(dma_node) > 2) {
        axidma_node_err(dma_node, "DMA has more than two channel nodes.\n");
        return -EINVAL;
    }

    // Go to the child node that we're parsing
    dma_chan_node = of_get_next_child(dma_node, NULL);
    if (channel == 1) {
        dma_chan_node = of_get_next_child(dma_node, dma_chan_node);
    }

    // Read out the channel's unique device id, and put it in the structure
    if (of_find_property(dma_chan_node, "xlnx,device-id", NULL) == NULL) {
        axidma_node_err(dma_chan_node, "DMA channel is missing the "
                        "'xlnx,device-id' property.\n");
        return -EINVAL;
    }
    rc = of_property_read_u32(dma_chan_node, "xlnx,device-id", &channel_id);
    if (rc < 0) {
        axidma_err("Unable to read the 'xlnx,device-id' property.\n");
        return -EINVAL;
    }
    chan->channel_id = channel_id;

    // Use the compatible string to determine the channel's information
    rc = axidma_parse_compatible_property(dma_chan_node, chan, dev);
    if (rc < 0) {
        return rc;
    }

    return 0;
}

static int axidma_check_unique_ids(struct axidma_device *dev)
{
    int i, j;
    struct axidma_chan *chan1, *chan2;

    // For each channel, check that its ID does not match any other channel's
    for (i = 0; i < dev->num_chans; i++)
    {
        chan1 = &dev->channels[i];
        for (j = i+1; j < dev->num_chans; j++)
        {
            chan2 = &dev->channels[j];
            if (chan1->channel_id == chan2->channel_id) {
                axidma_err("Channels %d and %d in the 'dmas' list have the "
                           "same channel id.\n", i, j);
                return -EINVAL;
            }
        }
    }

    return 0;
}

/*----------------------------------------------------------------------------
 * Public Interface
 *----------------------------------------------------------------------------*/

int axidma_of_num_channels(struct platform_device *pdev)
{
    int num_dmas, num_dma_names;
    struct device_node *driver_node;

    // Get the device tree node for the driver
    driver_node = pdev->dev.of_node;

    // Check that the device tree node has the 'dmas' and 'dma-names' properties
    if (of_find_property(driver_node, "dma-names", NULL) == NULL) {
        axidma_node_err(driver_node, "Property 'dma-names' is missing.\n");
        return -EINVAL;
    } else if (of_find_property(driver_node, "dmas", NULL) == NULL) {
        axidma_node_err(driver_node, "Property 'dmas' is missing.\n");
        return -EINVAL;
    }

    // Get the length of the properties, and make sure they are not empty
    num_dma_names = of_property_count_strings(driver_node, "dma-names");
    if (num_dma_names < 0) {
        axidma_node_err(driver_node, "Unable to get the 'dma-names' property "
                        "length.\n");
        return -EINVAL;
    } else if (num_dma_names == 0) {
        axidma_node_err(driver_node, "'dma-names' property is empty.\n");
        return -EINVAL;
    }
    num_dmas = of_count_phandle_with_args(driver_node, "dmas", "#dma-cells");
    if (num_dmas < 0) {
        axidma_node_err(driver_node, "Unable to get the 'dmas' property "
                        "length.\n");
        return -EINVAL;
    } else if (num_dmas == 0) {
        axidma_node_err(driver_node, "'dmas' property is empty.\n");
        return -EINVAL;
    }

    // Check that the number of entries in each property matches
    if (num_dma_names != num_dmas) {
        axidma_node_err(driver_node, "Length of 'dma-names' and 'dmas' "
                        "properties differ.\n");
        return -EINVAL;
    }

    return num_dma_names;
}

int axidma_of_parse_dma_nodes(struct platform_device *pdev,
                              struct axidma_device *dev)
{
    int i, rc;
    int channel;
    struct of_phandle_args phandle_args;
    struct device_node *driver_node, *dma_node;

    // Get the device tree node for the driver
    driver_node = pdev->dev.of_node;

    // Initialize the channel type counts
    dev->num_dma_tx_chans = 0;
    dev->num_dma_rx_chans = 0;
    dev->num_vdma_tx_chans = 0;
    dev->num_vdma_rx_chans = 0;

    rc = of_property_read_u32(driver_node, "index", &dev->chrdev_index);

    if (rc < 0) {
        if (rc != -EINVAL)
            axidma_node_err(driver_node, "Invalid index property, ignoring.\n");
        dev->chrdev_index = -1;
    }

    /* For each DMA channel specified in the deivce tree, parse out the
     * information about the channel, namely its direction and type. */
    for (i = 0; i < dev->num_chans; i++)
    {
        // Get the phanlde to the DMA channel
        rc = of_parse_phandle_with_args(driver_node, "dmas", "#dma-cells", i,
                                        &phandle_args);
        if (rc < 0) {
            axidma_node_err(driver_node, "Unable to get phandle %d from the "
                            "'dmas' property.\n", i);
            return rc;
        }

        // Check that the phandle has the expected arguments
        dma_node = phandle_args.np;
        channel = phandle_args.args[0];
        if (phandle_args.args_count < 1) {
            axidma_node_err(driver_node, "Phandle %d in the 'dmas' property is "
                            "is missing the channel direciton argument.\n", i);
            return -EINVAL;
        } else if (channel != 0 && channel != 1) {
            axidma_node_err(driver_node, "Phandle %d in the 'dmas' property "
                            "has an invalid channel (argument 0).\n", i);
            return -EINVAL;
        }

        // Parse out the information about the channel
        rc = axidma_of_parse_channel(dma_node, channel, &dev->channels[i], dev);
        if (rc < 0) {
            return rc;
        }

        // Parse the name of the channel
        rc = axidma_of_parse_dma_name(driver_node, i, &dev->channels[i]);
        if (rc < 0) {
            return rc;
        }
    }

    // Check that all channels have unique channel ID's
    return axidma_check_unique_ids(dev);
}
