/**
 * @file axidma_dma.c
 * @date Saturday, November 14, 2015 at 02:28:23 PM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This module contains the interface to the DMA engine for the AXI DMA module.
 *
 * @bug No known bugs.
 **/

// Kernel dependencies
#include <linux/delay.h>            // Milliseconds to jiffies converstion
#include <linux/wait.h>             // Completion related functions

/* <linux/signal.h> was moved to <linux/sched/signal.h> in the 4.11 kernel */
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)
#include <linux/sched.h>            // Send signal to process function
#else
#include <linux/sched/signal.h>     // send_sig_info function
#endif

#include <linux/dmaengine.h>        // DMA types and functions
#include <linux/slab.h>             // Allocation functions
#include <linux/errno.h>            // Linux error codes
#include <linux/platform_device.h>  // Platform device definitions
#include <linux/device.h>           // Device definitions and functions

/* Between 3.x and 4.x, the path to Xilinx's DMA include file changes. However,
 * in some 4.x kernels, the path is still the old one from 3.x. The macro is
 * defined by the Makefile, when specified by the user. */
#ifndef XILINX_DMA_INCLUDE_PATH_FIXUP
#include <linux/dma/xilinx_dma.h>   // Xilinx DMA config structures
#else
#include <linux/amba/xilinx_dma.h>  // Xilinx DMA config structures
#endif

// Local dependencies
#include "axidma.h"                 // Internal definitions
#include "axidma_ioctl.h"           // IOCTL interface definition and types

/*----------------------------------------------------------------------------
 * Internal Definitions
 *----------------------------------------------------------------------------*/

// The default timeout for DMA is 10 seconds
#define AXIDMA_DMA_TIMEOUT      10000

// A convenient structure to pass between prep and start transfer functions
struct axidma_transfer {
    int sg_len;                     // The length of the BD array
    struct scatterlist *sg_list;    // List of buffer descriptors
    bool wait;                      // Indicates if we should wait
    dma_cookie_t cookie;            // The DMA cookie for the transfer
    struct completion comp;         // A completion to use for waiting
    enum axidma_dir dir;            // The direction of the transfer
    enum axidma_type type;          // The type of the transfer (VDMA/DMA)
    int channel_id;                 // The ID of the channel
    int notify_signal;              // The signal to use for async transfers
    struct task_struct *process;    // The process requesting the transfer
    struct axidma_cb_data *cb_data; // The callback data struct

    // VDMA specific fields (kept as union for extensability)
    union {
        struct axidma_video_frame frame;    // Frame information for VDMA
    };
};

// The data to pass to the DMA transfer completion callback function
struct axidma_cb_data {
    int channel_id;                 // The id of the channel used
    int notify_signal;              // For async, signal to send
    struct task_struct *process;    // The process to send the signal to
    struct completion *comp;        // For sync, the notification to kernel
};

/*----------------------------------------------------------------------------
 * Enumeration Conversions
 *----------------------------------------------------------------------------*/

static char *axidma_dir_to_string(enum axidma_dir dma_dir)
{
    BUG_ON(dma_dir != AXIDMA_WRITE && dma_dir != AXIDMA_READ);
    return (dma_dir == AXIDMA_WRITE) ? "transmit" : "receive";
}

static char *axidma_type_to_string(enum axidma_type dma_type)
{
    BUG_ON(dma_type != AXIDMA_DMA && dma_type != AXIDMA_VDMA);
    return (dma_type == AXIDMA_DMA) ? "DMA" : "VDMA";
}

// Convert the AXI DMA direction enumeration to a DMA direction enumeration
static enum dma_transfer_direction axidma_to_dma_dir(enum axidma_dir dma_dir)
{
    BUG_ON(dma_dir != AXIDMA_WRITE && dma_dir != AXIDMA_READ);
    return (dma_dir == AXIDMA_WRITE) ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM;
}

/*----------------------------------------------------------------------------
 * DMA Operations Helper Functions
 *----------------------------------------------------------------------------*/

static int axidma_init_sg_entry(struct axidma_device *dev,
        struct scatterlist *sg_list, int index, void *buf, size_t buf_len)
{
    dma_addr_t dma_addr;

    // Get the DMA address from the user virtual address
    dma_addr = axidma_uservirt_to_dma(dev, buf, buf_len);
    if (dma_addr == (dma_addr_t)NULL) {
        axidma_err("Requested transfer address %p does not fall within a "
                   "previously allocated DMA buffer.\n", buf);
        return -EFAULT;
    }

    // Initialize the scatter-gather table entry
    sg_dma_address(&sg_list[index]) = dma_addr;
    sg_dma_len(&sg_list[index]) = buf_len;

    return 0;
}

static struct axidma_chan *axidma_get_chan(struct axidma_device *dev,
        int channel_id)
{
    int i;
    struct axidma_chan *chan;

    // Find the channel with the given ID that matches the type and direction
    for (i = 0; i < dev->num_chans; i++)
    {
        chan = &dev->channels[i];
        if (chan->channel_id == channel_id) {
            return chan;
        }
    }

    return NULL;
}

static void axidma_dma_callback(void *data)
{
    struct axidma_cb_data *cb_data;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 20, 0)
    struct siginfo sig_info;
#else
    struct kernel_siginfo sig_info;
#endif

    /* For synchronous transfers, notify the kernel thread waiting. For
     * asynchronous transfers, send a signal to userspace if requested. */
    cb_data = data;
    if (cb_data->comp != NULL) {
        complete(cb_data->comp);
    } else if (VALID_NOTIFY_SIGNAL(cb_data->notify_signal)) {
        memset(&sig_info, 0, sizeof(sig_info));
        sig_info.si_signo = cb_data->notify_signal;
        sig_info.si_code = SI_QUEUE;
        sig_info.si_int = cb_data->channel_id;
        send_sig_info(cb_data->notify_signal, &sig_info, cb_data->process);
    }
}

// Setup the config structure for VDMA
static void axidma_setup_vdma_config(struct xilinx_vdma_config *dma_config)
{
    memset(dma_config, 0, sizeof(*dma_config));
    dma_config->frm_dly = 0;            // Number of frames to delay
    dma_config->gen_lock = 0;           // Genlock, VDMA runs on fsyncs
    dma_config->master = 0;             // VDMA is the genlock master
    dma_config->frm_cnt_en = 1;         // Interrupt based on frame count
    dma_config->park = 0;               // Continuously process all frames
    dma_config->park_frm = 0;           // Frame to stop (park) at (N/A)
    dma_config->coalesc = 1;            // Interrupt after one frame completion
    dma_config->delay = 0;              // Disable the delay counter interrupt
    dma_config->reset = 0;              // Don't reset the channel
    dma_config->ext_fsync = 0;          // VDMA handles synchronizes itself
    return;
}

static int axidma_prep_transfer(struct axidma_chan *axidma_chan,
                                struct axidma_transfer *dma_tfr)
{
    struct dma_chan *chan;
    struct dma_device *dma_dev;
    struct dma_async_tx_descriptor *dma_txnd;
    struct completion *dma_comp;
    struct xilinx_vdma_config vdma_config;
    struct axidma_cb_data *cb_data;
    struct dma_interleaved_template dma_template;
    enum dma_transfer_direction dma_dir;
    enum dma_ctrl_flags dma_flags;
    struct scatterlist *sg_list;
    int sg_len;
    dma_cookie_t dma_cookie;
    char *direction, *type;
    int rc;

    // Get the fields from the structures
    chan = axidma_chan->chan;
    dma_comp = &dma_tfr->comp;
    dma_dir = axidma_to_dma_dir(dma_tfr->dir);
    dma_dev = chan->device;
    sg_list = dma_tfr->sg_list;
    sg_len = dma_tfr->sg_len;
    direction = axidma_dir_to_string(dma_tfr->dir);
    type = axidma_type_to_string(dma_tfr->type);
    cb_data = dma_tfr->cb_data;

    /* For VDMA transfers, we configure the channel, then prepare an interlaved
     * transfer. For DMA, we simply prepare a slave scatter-gather transfer. */
    dma_flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    if (dma_tfr->type == AXIDMA_DMA) {
        dma_txnd = dmaengine_prep_slave_sg(chan, sg_list, sg_len, dma_dir,
                                           dma_flags);
    } else {
        axidma_setup_vdma_config(&vdma_config);
        rc = xilinx_vdma_channel_set_config(chan, &vdma_config);
        if (rc < 0) {
            axidma_err("Unable to set the config for channel.\n");
            goto stop_dma;
        }

        memset(&dma_template, 0, sizeof(dma_template));
        dma_template.dst_start = sg_dma_address(&sg_list[0]);
        dma_template.src_start = sg_dma_address(&sg_list[0]);
        dma_template.dir = dma_dir;
        dma_template.numf = dma_tfr->frame.height;
        dma_template.frame_size = 1;
        dma_template.sgl[0].size = dma_tfr->frame.width *
                dma_tfr->frame.depth;
        dma_template.sgl[0].icg = 0;
        dma_txnd = dmaengine_prep_interleaved_dma(chan, &dma_template,
                dma_flags);
    }
    if (dma_txnd == NULL) {
        axidma_err("Unable to prepare the dma engine for the %s %s buffer.\n",
                   type, direction);
        rc = -EBUSY;
        goto stop_dma;
    }

    /* If we're going to wait for this channel, initialize the completion for
     * the channel, and setup the callback to complete it. */
    cb_data->channel_id = dma_tfr->channel_id;
    if (dma_tfr->wait) {
        cb_data->comp = dma_comp;
        cb_data->notify_signal = -1;
        cb_data->process = NULL;
        init_completion(cb_data->comp);
        dma_txnd->callback_param = cb_data;
        dma_txnd->callback = axidma_dma_callback;
    } else {
        cb_data->comp = NULL;
        cb_data->notify_signal = dma_tfr->notify_signal;
        cb_data->process = dma_tfr->process;
        dma_txnd->callback_param = cb_data;
        dma_txnd->callback = axidma_dma_callback;
    }
    dma_cookie = dmaengine_submit(dma_txnd);
    if (dma_submit_error(dma_cookie)) {
        axidma_err("Unable to submit the %s %s transaction to the engine.\n",
                   direction, type);
        rc = -EBUSY;
        goto stop_dma;
    }

    // Return the DMA cookie for the transaction
    dma_tfr->cookie = dma_cookie;
    return 0;

stop_dma:
    dmaengine_terminate_all(chan);
    return rc;
}

static int axidma_start_transfer(struct axidma_chan *chan,
                                 struct axidma_transfer *dma_tfr)
{
    struct completion *dma_comp;
    dma_cookie_t dma_cookie;
    enum dma_status status;
    char *direction, *type;
    unsigned long timeout, time_remain;
    int rc;

    // Get the fields from the structures
    dma_comp = &dma_tfr->comp;
    dma_cookie = dma_tfr->cookie;
    direction = axidma_dir_to_string(dma_tfr->dir);
    type = axidma_type_to_string(dma_tfr->type);

    // Flush all pending transaction in the dma engine for this channel
    dma_async_issue_pending(chan->chan);

    // Wait for the completion timeout or the DMA to complete
    if (dma_tfr->wait) {
        timeout = msecs_to_jiffies(AXIDMA_DMA_TIMEOUT);
        time_remain = wait_for_completion_timeout(dma_comp, timeout);
        status = dma_async_is_tx_complete(chan->chan, dma_cookie, NULL, NULL);

        if (time_remain == 0) {
            axidma_err("%s %s transaction timed out.\n", type, direction);
            rc = -ETIME;
            goto stop_dma;
        } else if (status != DMA_COMPLETE) {
            axidma_err("%s %s transaction did not succceed. Status is %d.\n",
                       type, direction, status);
            rc = -EBUSY;
            goto stop_dma;
        }
    }

    return 0;

stop_dma:
    dmaengine_terminate_all(chan->chan);
    return rc;
}

/*----------------------------------------------------------------------------
 * DMA Operations (Public Interface)
 *----------------------------------------------------------------------------*/

void axidma_get_num_channels(struct axidma_device *dev,
                             struct axidma_num_channels *num_chans)
{
    num_chans->num_channels = dev->num_chans;
    num_chans->num_dma_tx_channels = dev->num_dma_tx_chans;
    num_chans->num_dma_rx_channels = dev->num_dma_rx_chans;
    num_chans->num_vdma_tx_channels = dev->num_vdma_tx_chans;
    num_chans->num_vdma_rx_channels = dev->num_vdma_rx_chans;
    return;
}

void axidma_get_channel_info(struct axidma_device *dev,
                             struct axidma_channel_info *chan_info)
{
    chan_info->channels = dev->channels;
    return;
}

int axidma_set_signal(struct axidma_device *dev, int signal)
{
    // Verify the signal is a real-time one
    if (!VALID_NOTIFY_SIGNAL(signal)) {
        axidma_err("Invalid signal %d requested for DMA notification.\n",
                   signal);
        axidma_err("You must specify one of the POSIX real-time signals.\n");
        return -EINVAL;
    }

    dev->notify_signal = signal;
    return 0;
}

int axidma_read_transfer(struct axidma_device *dev,
                         struct axidma_transaction *trans)
{
    int rc;
    struct axidma_chan *rx_chan;
    struct scatterlist sg_list;
    struct axidma_transfer rx_tfr;

    // Get the channel with the given channel id
    rx_chan = axidma_get_chan(dev, trans->channel_id);
    if (rx_chan == NULL || rx_chan->dir != AXIDMA_READ) {
        axidma_err("Invalid device id %d for DMA receive channel.\n",
                   trans->channel_id);
        return -ENODEV;
    }

    // Setup the scatter-gather list for the transfer (only one entry)
    sg_init_table(&sg_list, 1);
    rc = axidma_init_sg_entry(dev, &sg_list, 0, trans->buf,
                              trans->buf_len);
    if (rc < 0) {
        return rc;
    }

    // Setup receive transfer structure for DMA
    rx_tfr.sg_list = &sg_list;
    rx_tfr.sg_len = 1;
    rx_tfr.dir = rx_chan->dir;
    rx_tfr.type = rx_chan->type;
    rx_tfr.wait = trans->wait;
    rx_tfr.channel_id = trans->channel_id;
    rx_tfr.notify_signal = dev->notify_signal;
    rx_tfr.process = get_current();
    rx_tfr.cb_data = &dev->cb_data[trans->channel_id];

    // Prepare the receive transfer
    rc = axidma_prep_transfer(rx_chan, &rx_tfr);
    if (rc < 0) {
        return rc;
    }

    // Submit the receive transfer, and wait for it to complete
    rc = axidma_start_transfer(rx_chan, &rx_tfr);
    if (rc < 0) {
        return rc;
    }

    return 0;
}

int axidma_write_transfer(struct axidma_device *dev,
                          struct axidma_transaction *trans)
{
    int rc;
    struct axidma_chan *tx_chan;
    struct scatterlist sg_list;
    struct axidma_transfer tx_tfr;

    // Get the channel with the given id
    tx_chan = axidma_get_chan(dev, trans->channel_id);
    if (tx_chan == NULL || tx_chan->dir != AXIDMA_WRITE) {
        axidma_err("Invalid device id %d for DMA transmit channel.\n",
                   trans->channel_id);
        return -ENODEV;
    }

    // Setup the scatter-gather list for the transfer (only one entry)
    sg_init_table(&sg_list, 1);
    rc = axidma_init_sg_entry(dev, &sg_list, 0, trans->buf,
                              trans->buf_len);
    if (rc < 0) {
        return rc;
    }

    // Setup transmit transfer structure for DMA
    tx_tfr.sg_list = &sg_list;
    tx_tfr.sg_len = 1;
    tx_tfr.dir = tx_chan->dir;
    tx_tfr.type = tx_chan->type;
    tx_tfr.wait = trans->wait;
    tx_tfr.channel_id = trans->channel_id;
    tx_tfr.notify_signal = dev->notify_signal;
    tx_tfr.process = get_current();
    tx_tfr.cb_data = &dev->cb_data[trans->channel_id];

    // Prepare the transmit transfer
    rc = axidma_prep_transfer(tx_chan, &tx_tfr);
    if (rc < 0) {
        return rc;
    }

    // Submit the transmit transfer, and wait for it to complete
    rc = axidma_start_transfer(tx_chan, &tx_tfr);
    if (rc < 0) {
        return rc;
    }

    return 0;
}

/* Transfers data from the given source buffer out to the AXI DMA device, and
 * places the data received into the receive buffer. */
int axidma_rw_transfer(struct axidma_device *dev,
                       struct axidma_inout_transaction *trans)
{
    int rc;
    struct axidma_chan *tx_chan, *rx_chan;
    struct scatterlist tx_sg_list, rx_sg_list;
    struct axidma_transfer tx_tfr, rx_tfr;

    // Get the transmit and receive channels with the given ids.
    tx_chan = axidma_get_chan(dev, trans->tx_channel_id);
    if (tx_chan == NULL || tx_chan->dir != AXIDMA_WRITE) {
        axidma_err("Invalid device id %d for DMA transmit channel.\n",
                   trans->tx_channel_id);
        return -ENODEV;
    }

    rx_chan = axidma_get_chan(dev, trans->rx_channel_id);
    if (rx_chan == NULL || rx_chan->dir != AXIDMA_READ) {
        axidma_err("Invalid device id %d for DMA receive channel.\n",
                   trans->rx_channel_id);
        return -ENODEV;
    }

    // Setup the scatter-gather list for the transfers (only one entry)
    sg_init_table(&tx_sg_list, 1);
    rc = axidma_init_sg_entry(dev, &tx_sg_list, 0, trans->tx_buf,
                              trans->tx_buf_len);
    if (rc < 0) {
        return rc;
    }
    sg_init_table(&rx_sg_list, 1);
    rc = axidma_init_sg_entry(dev, &rx_sg_list, 0, trans->rx_buf,
                              trans->rx_buf_len);
    if (rc < 0) {
        return rc;
    }

    // Setup receive and trasmit transfer structures for DMA
    tx_tfr.sg_list = &tx_sg_list,
    tx_tfr.sg_len = 1,
    tx_tfr.dir = tx_chan->dir,
    tx_tfr.type = tx_chan->type,
    tx_tfr.wait = false,
    tx_tfr.channel_id = trans->tx_channel_id,
    tx_tfr.notify_signal = dev->notify_signal,
    tx_tfr.process = get_current(),
    tx_tfr.cb_data = &dev->cb_data[trans->tx_channel_id];

    // Add in the frame information for VDMA transfers
    if (tx_chan->type == AXIDMA_VDMA) {
        memcpy(&tx_tfr.frame, &trans->tx_frame, sizeof(tx_tfr.frame));
    }

    rx_tfr.sg_list = &rx_sg_list,
    rx_tfr.sg_len = 1,
    rx_tfr.dir = rx_chan->dir,
    rx_tfr.type = rx_chan->type,
    rx_tfr.wait = trans->wait,
    rx_tfr.channel_id = trans->rx_channel_id,
    rx_tfr.notify_signal = dev->notify_signal,
    rx_tfr.process = get_current(),
    rx_tfr.cb_data = &dev->cb_data[trans->rx_channel_id];

    // Add in the frame information for VDMA transfers
    if (tx_chan->type == AXIDMA_VDMA) {
        memcpy(&rx_tfr.frame, &trans->rx_frame, sizeof(rx_tfr.frame));
    }

    // Prep both the receive and transmit transfers
    rc = axidma_prep_transfer(tx_chan, &tx_tfr);
    if (rc < 0) {
        return rc;
    }
    rc = axidma_prep_transfer(rx_chan, &rx_tfr);
    if (rc < 0) {
        return rc;
    }

    // Submit both transfers to the DMA engine, and wait on the receive transfer
    rc = axidma_start_transfer(tx_chan, &tx_tfr);
    if (rc < 0) {
        return rc;
    }
    rc = axidma_start_transfer(rx_chan, &rx_tfr);
    if (rc < 0) {
        return rc;
    }

    return 0;
}

int axidma_video_transfer(struct axidma_device *dev,
                          struct axidma_video_transaction *trans,
                          enum axidma_dir dir)
{
    int rc, i;
    size_t image_size;
    struct axidma_chan *chan;
    struct scatterlist *sg_list;

    // Setup transmit transfer structure for DMA
    struct axidma_transfer transfer = {
        .sg_len = trans->num_frame_buffers,
        .dir = dir,
        .type = AXIDMA_VDMA,
        .wait = false,
        .channel_id = trans->channel_id,
        .notify_signal = dev->notify_signal,
        .process = get_current(),
        .frame = trans->frame,
    };

    // Allocate an array to store the scatter list structures for the buffers
    transfer.sg_list = kmalloc(transfer.sg_len * sizeof(*sg_list), GFP_KERNEL);
    if (transfer.sg_list == NULL) {
        axidma_err("Unable to allocate memory for the scatter-gather list.\n");
        rc = -ENOMEM;
        goto ret;
    }

    // For each frame, setup a scatter-gather entry
    image_size = trans->frame.width * trans->frame.height * trans->frame.depth;
    for (i = 0; i < transfer.sg_len; i++)
    {
        rc = axidma_init_sg_entry(dev, transfer.sg_list, i,
                                  trans->frame_buffers[i], image_size);
        if (rc < 0) {
            goto free_sg_list;
        }
    }

    // Get the channel with the given id
    chan = axidma_get_chan(dev, trans->channel_id);
    if (chan == NULL && chan->dir != dir &&
            chan->type != AXIDMA_VDMA) {
        axidma_err("Invalid device id %d for VDMA %s channel.\n",
                   trans->channel_id, axidma_dir_to_string(chan->dir));
        rc = -ENODEV;
        goto free_sg_list;
    }
    transfer.cb_data = &dev->cb_data[trans->channel_id];

    // Prepare the transmit transfer
    rc = axidma_prep_transfer(chan, &transfer);
    if (rc < 0) {
        goto free_sg_list;
    }

    // Submit the transfer, and immediately return
    rc = axidma_start_transfer(chan, &transfer);

free_sg_list:
    kfree(transfer.sg_list);
ret:
    return 0;
}

int axidma_stop_channel(struct axidma_device *dev,
                        struct axidma_chan *chan_info)
{
    struct axidma_chan *chan;

    // Get the transmit and receive channels with the given ids.
    chan = axidma_get_chan(dev, chan_info->channel_id);
    if (chan == NULL && chan->type != chan_info->type &&
            chan->dir != chan_info->dir) {
        axidma_err("Invalid channel id %d for %s %s channel.\n",
            chan_info->channel_id, axidma_type_to_string(chan_info->type),
            axidma_dir_to_string(chan_info->dir));
        return -ENODEV;
    }

    // Terminate all DMA transactions on the given channel
    return dmaengine_terminate_all(chan->chan);
}

/*----------------------------------------------------------------------------
 * Initialization and Cleanup
 *----------------------------------------------------------------------------*/

static int axidma_request_channels(struct platform_device *pdev,
                                   struct axidma_device *dev)
{
    int rc, i;
    int num_reserved_chans;
    struct axidma_chan *chan;

    // For each channel, request exclusive access to the channel
    num_reserved_chans = 0;
    for (i = 0; i < dev->num_chans; i++)
    {
        chan = &dev->channels[i];
        chan->chan = dma_request_slave_channel(&pdev->dev, chan->name);
        if (chan->chan == NULL) {
            axidma_err("Unable to get slave channel %d: %s.\n", i, chan->name);
            rc = -ENODEV;
            goto release_channels;
        }
        num_reserved_chans += 1;
    }

    return 0;

release_channels:
    // Release any channels that have been requested already so far
    for (i = 0; i < num_reserved_chans; i++)
    {
        dma_release_channel(dev->channels[i].chan);
    }
    return rc;
}

int axidma_dma_init(struct platform_device *pdev, struct axidma_device *dev)
{
    int rc;
    size_t elem_size;
    u64 dma_mask;

    dma_mask = DMA_BIT_MASK(8 * sizeof(dma_addr_t));
    rc = dma_set_coherent_mask(&dev->pdev->dev, dma_mask);
    if (rc < 0) {
        axidma_err("Unable to set the DMA coherent mask.\n");
        return rc;
    }

    // Get the number of DMA channels listed in the device tree
    dev->num_chans = axidma_of_num_channels(pdev);
    if (dev->num_chans < 0) {
        return dev->num_chans;
    }

    // Allocate an array to store all the channel metdata structures
    elem_size = sizeof(dev->channels[0]);
    dev->channels = kmalloc(dev->num_chans * elem_size, GFP_KERNEL);
    if (dev->channels == NULL) {
        axidma_err("Unable to allocate memory for channel structures.\n");
        return -ENOMEM;
    }

    // Allocate an array to store all callback structures, for async
    elem_size = sizeof(dev->cb_data[0]);
    dev->cb_data = kmalloc(dev->num_chans * elem_size, GFP_KERNEL);
    if (dev->cb_data == NULL) {
        axidma_err("Unable to allocate memory for callback structures.\n");
        rc = -ENOMEM;
        goto free_channels;
    }

    // Parse the type and direction of each DMA channel from the device tree
    rc = axidma_of_parse_dma_nodes(pdev, dev);
    if (rc < 0) {
        return rc;
    }

    // Exclusively request all of the channels in the device tree entry
    rc = axidma_request_channels(pdev, dev);
    if (rc < 0) {
        goto free_callback_data;
    }

    axidma_info("DMA: Found %d transmit channels and %d receive channels.\n",
                dev->num_dma_tx_chans, dev->num_dma_rx_chans);
    axidma_info("VDMA: Found %d transmit channels and %d receive channels.\n",
                dev->num_vdma_tx_chans, dev->num_vdma_rx_chans);
    return 0;

free_callback_data:
    kfree(dev->cb_data);
free_channels:
    kfree(dev->channels);
    return rc;
}

void axidma_dma_exit(struct axidma_device *dev)
{
    int i;
    struct dma_chan *chan;

    // Stop all running DMA transactions on all channels, and release
    for (i = 0; i < dev->num_chans; i++)
    {
        chan = dev->channels[i].chan;
        dmaengine_terminate_all(chan);
        dma_release_channel(chan);
    }

    // Free the channel and callback data arrays
    kfree(dev->channels);
    kfree(dev->cb_data);

    return;
}
