/**
 * @file libaxidma.c
 * @date Saturday, December 05, 2015 at 10:10:39 AM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This is a simple library that wraps around the AXI DMA module,
 * allowing for the user to abstract away from the finer grained details.
 *
 * @bug No known bugs.
 **/

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <fcntl.h>              // Flags for open()
#include <sys/stat.h>           // Open() system call
#include <sys/types.h>          // Types for open()
#include <sys/mman.h>           // Mmap system call
#include <sys/ioctl.h>          // IOCTL system call
#include <unistd.h>             // Close() system call
#include <errno.h>              // Error codes
#include <signal.h>             // Signal handling functions

#include "libaxidma.h"          // Local definitions
#include "axidma_ioctl.h"       // The IOCTL interface to AXI DMA

/*----------------------------------------------------------------------------
 * Internal definitions
 *----------------------------------------------------------------------------*/

// A structure that holds metadata about each channel
typedef struct dma_channel {
    enum axidma_dir dir;        ///< Direction of the channel
    enum axidma_type type;      ///< Type of the channel
    int channel_id;             ///< Integer id of the channel.
    axidma_cb_t callback;       ///< Callback function for channel completion
    void *user_data;            ///< User data to pass to the callback
} dma_channel_t;

// The structure that represents the AXI DMA device
struct axidma_dev {
    int fd;                     ///< File descriptor for the device
    array_t dma_tx_chans;       ///< Channel id's for the DMA transmit channels
    array_t dma_rx_chans;       ///< Channel id's for the DMA receive channels
    array_t vdma_tx_chans;      ///< Channel id's for the VDMA transmit channels
    array_t vdma_rx_chans;      ///< Channel id's for the VDMA receive channels
    int num_channels;           ///< The total number of DMA channels
    dma_channel_t *channels;    ///< All of the VDMA/DMA channels in the system
};

/*----------------------------------------------------------------------------
 * Private Helper Functions
 *----------------------------------------------------------------------------*/

/* Categorizes the DMA channels by their type and direction, getting their ID's
 * and placing them into separate arrays. */
static int categorize_channels(axidma_dev_t dev,
        struct axidma_chan *channels, struct axidma_num_channels *num_chan)
{
    int i;
    struct axidma_chan *chan;
    dma_channel_t *dma_chan;

    // Allocate an array for all the channel metadata
    dev->channels = malloc(num_chan->num_channels * sizeof(dev->channels[0]));
    if  (dev->channels == NULL) {
        return -ENOMEM;
    }

    // Allocate arrays for the DMA channel ids
    dev->dma_tx_chans.data = malloc(num_chan->num_dma_tx_channels *
            sizeof(dev->dma_tx_chans.data[0]));
    if (dev->dma_tx_chans.data == NULL) {
        free(dev->channels);
        return -ENOMEM;
    }
    dev->dma_rx_chans.data = malloc(num_chan->num_dma_rx_channels *
            sizeof(dev->dma_rx_chans.data[0]));
    if (dev->dma_rx_chans.data == NULL) {
        free(dev->channels);
        free(dev->dma_tx_chans.data);
        return -ENOMEM;
    }

    // Allocate arrays for the VDMA channel ids
    dev->vdma_tx_chans.data = malloc(num_chan->num_vdma_tx_channels *
            sizeof(dev->vdma_tx_chans.data[0]));
    if (dev->vdma_tx_chans.data == NULL) {
        free(dev->channels);
        free(dev->dma_tx_chans.data);
        free(dev->dma_rx_chans.data);
        return -ENOMEM;
    }
    dev->vdma_rx_chans.data = malloc(num_chan->num_vdma_rx_channels *
            sizeof(dev->vdma_rx_chans.data[0]));
    if (dev->vdma_rx_chans.data == NULL) {
        free(dev->channels);
        free(dev->dma_tx_chans.data);
        free(dev->dma_rx_chans.data);
        free(dev->vdma_tx_chans.data);
        return -ENOMEM;
    }

    // Place the DMA channel ID's into the appropiate array
    dev->num_channels = num_chan->num_channels;
    for (i = 0; i < num_chan->num_channels; i++)
    {
        // Based on the current channels's type and direction, select the array
        array_t *array = NULL;
        chan = &channels[i];
        if (chan->dir == AXIDMA_WRITE && chan->type == AXIDMA_DMA) {
            array = &dev->dma_tx_chans;
        } else if (chan->dir == AXIDMA_READ && chan->type == AXIDMA_DMA) {
            array = &dev->dma_rx_chans;
        } else if (chan->dir == AXIDMA_WRITE && chan->type == AXIDMA_VDMA) {
            array = &dev->vdma_tx_chans;
        } else if (chan->dir == AXIDMA_READ && chan->type == AXIDMA_VDMA) {
            array = &dev->vdma_rx_chans;
        }
        assert(array != NULL);

        // Assign the ID for the channel into the appropiate array
        array->data[array->len] = chan->channel_id;
        array->len += 1;

        // Construct the DMA channel structure
        dma_chan = &dev->channels[i];
        dma_chan->dir = chan->dir;
        dma_chan->type = chan->type;
        dma_chan->channel_id = chan->channel_id;
        dma_chan->callback = NULL;
        dma_chan->user_data = NULL;
    }

    // Assign the length of the arrays

    return 0;
}


/* Probes the AXI DMA driver for all of the available channels. It places
 * returns an array of axidma_channel structures. */
static int probe_channels(axidma_dev_t dev)
{
    int rc;
    struct axidma_chan *channels;
    struct axidma_num_channels num_chan;
    struct axidma_channel_info channel_info;

    // Query the module for the total number of DMA channels
    rc = ioctl(dev->fd, AXIDMA_GET_NUM_DMA_CHANNELS, &num_chan);
    if (rc < 0) {
        perror("Unable to get the number of DMA channels");
        return rc;
    } else if (num_chan.num_channels == 0) {
        fprintf(stderr, "No DMA channels are present.\n");
        return -ENODEV;
    }

    // Allocate an array to hold the channel meta-data
    channels = malloc(num_chan.num_channels * sizeof(channels[0]));
    if (channels == NULL) {
        return -ENOMEM;
    }

    // Get the metdata about all the available channels
    channel_info.channels = channels;
    rc = ioctl(dev->fd, AXIDMA_GET_DMA_CHANNELS, &channel_info);
    if (rc < 0) {
        perror("Unable to get DMA channel information");
        free(channels);
        return rc;
    }

    // Extract the channel id's, and organize them by type
    rc = categorize_channels(dev, channels, &num_chan);
    free(channels);

    return rc;
}

static void axidma_callback(int signal, siginfo_t *siginfo, void *context)
{
    struct axidma_dev *dev = siginfo->si_ptr;
    int channel_id = siginfo->si_errno;
    dma_channel_t *chan;

    assert(dev != NULL);
    assert(channel_id >= 0);
    assert(channel_id < dev->num_channels);

    // Silence the compiler
    (void)signal;
    (void)context;

    // If the user defined a callback for a given channel, invoke it
    chan = &dev->channels[channel_id];
    if (chan->callback != NULL) {
        chan->callback(channel_id, chan->user_data);
    }

    return;
}

/* Sets up a signal handler for the lowest real-time signal to be delivered
 * whenever any asynchronous DMA transaction compeletes. */
// TODO: Should really check if real time signal is being used
static int setup_dma_callback(axidma_dev_t dev)
{
    int rc;
    struct axidma_signal_info sig_info;
    struct sigaction sigact;

    // Register a signal handler for the real-time signal
    sigact.sa_sigaction = axidma_callback;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_RESTART | SA_SIGINFO;
    rc = sigaction(SIGRTMIN, &sigact, NULL);
    if (rc < 0) {
        perror("Failed to register DMA callback");
        return rc;
    }

    sig_info.signal = SIGRTMIN;
    sig_info.user_data = dev;

    // Tell the driver to deliver us SIGRTMIN and dev upon DMA completion
    rc = ioctl(dev->fd, AXIDMA_SET_DMA_SIGNAL, &sig_info);
    if (rc < 0) {
        perror("Failed to set the DMA callback signal");
        return rc;
    }

    return 0;
}

// Finds the DMA channel with the given id
static dma_channel_t *find_channel(axidma_dev_t dev, int channel_id)
{
    int i;
    dma_channel_t *dma_chan;

    for (i = 0; i < dev->num_channels; i++)
    {
        dma_chan = &dev->channels[i];
        if (dma_chan->channel_id == channel_id) {
            return dma_chan;
        }
    }

    return NULL;
}

// Converts the AXI DMA direction to the corresponding ioctl for the transfer
static unsigned long dir_to_ioctl(enum axidma_dir dir)
{
    switch (dir)
    {
        case AXIDMA_READ:
            return AXIDMA_DMA_READ;
        case AXIDMA_WRITE:
            return AXIDMA_DMA_WRITE;
    }

    assert(false);
    return 0;
}

/*----------------------------------------------------------------------------
 * Public Interface
 *----------------------------------------------------------------------------*/

struct axidma_dev *axidma_init()
{
    return axidma_init_dev(0);
}

/* Initializes the AXI DMA device, returning a new handle to the
 * axidma_device. */
struct axidma_dev *axidma_init_dev(unsigned int index)
{
    const unsigned int pathlen = sizeof(AXIDMA_DEV_PATH) + 10;
    char path[pathlen];
    struct axidma_dev *dev;

    dev = calloc(1, sizeof(struct axidma_dev));
    if (dev == NULL) {
        perror("Can't allocate AXI DMA structure");
        return NULL;
    }

    // Open the AXI DMA device
    if (index) {
        snprintf(path, pathlen, "%s%d", AXIDMA_DEV_PATH, index);
        dev->fd = open(path, O_RDWR|O_EXCL);
    }
    else
        dev->fd = open(AXIDMA_DEV_PATH, O_RDWR|O_EXCL);

    if (dev->fd < 0) {
        perror("Error opening AXI DMA device");
        fprintf(stderr, "Expected the AXI DMA device at the path `%s`\n",
                index ? path : AXIDMA_DEV_PATH);
        return NULL;
    }

    // Query the AXIDMA device for all of its channels
    if (probe_channels(dev) < 0) {
        close(dev->fd);
        return NULL;
    }

    // TODO: Should really check that signal is not already taken
    /* Setup a real-time signal to indicate when transactions have completed,
     * and request the driver to send them to us. */
    if (setup_dma_callback(dev) < 0) {
        close(dev->fd);
        return NULL;
    }

    // Return the AXI DMA device to the user
    return dev;
}

// Tears down the given AXI DMA device structure
void axidma_destroy(axidma_dev_t dev)
{
    // Free the arrays used for channel id's and channel metadata
    free(dev->vdma_rx_chans.data);
    free(dev->vdma_tx_chans.data);
    free(dev->dma_rx_chans.data);
    free(dev->dma_tx_chans.data);
    free(dev->channels);

    // Close the AXI DMA device
    if (close(dev->fd) < 0) {
        perror("Failed to close the AXI DMA device");
        assert(false);
    }

    return;
}

// Returns an array of all the available AXI DMA transmit channels
const array_t *axidma_get_dma_tx(axidma_dev_t dev)
{
    return &dev->dma_tx_chans;
}

// Returns an array of all the available AXI DMA receive channels
const array_t *axidma_get_dma_rx(axidma_dev_t dev)
{
    return &dev->dma_rx_chans;
}

// Returns an array of all the available AXI VDMA transmit channels
const array_t *axidma_get_vdma_tx(axidma_dev_t dev)
{
    return &dev->vdma_tx_chans;
}

// Returns an array of all the available AXI VDMA receive channels
const array_t *axidma_get_vdma_rx(axidma_dev_t dev)
{
    return &dev->vdma_rx_chans;
}

/* Allocates a region of memory suitable for use with the AXI DMA driver. Note
 * that this is a quite expensive operation, and should be done at initalization
 * time. */
void *axidma_malloc(axidma_dev_t dev, size_t size)
{
    void *addr;

    // Call the device's mmap method to allocate the memory region
    addr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, dev->fd, 0);
    if (addr == MAP_FAILED) {
        return NULL;
    }

    return addr;
}

/* This frees a region of memory that was allocated with a call to
 * axidma_malloc. The size passed in here must match the one used for that
 * call, or this function will throw an exception. */
void axidma_free(axidma_dev_t dev, void *addr, size_t size)
{
    // Silence the compiler
    (void)dev;

    if (munmap(addr, size) < 0) {
        perror("Failed to free the AXI DMA memory mapped region");
        assert(false);
    }

    return;
}

/* Sets up a callback function to be called whenever the transaction completes
 * on the given channel for asynchronous transfers. */
void axidma_set_callback(axidma_dev_t dev, int channel, axidma_cb_t callback,
                        void *data)
{
    dma_channel_t *chan;

    assert(find_channel(dev, channel) != NULL);

    chan = &dev->channels[channel];
    chan->callback = callback;
    chan->user_data = data;

    return;
}

/* Registers a DMA buffer allocated by another driver with the AXI DMA driver.
 * This allows it to be used in DMA transfers later on. The user must make sure
 * that the driver that allocated the buffer has exported it. The file
 * descriptor is the one that is returned by the other driver's export. */
int axidma_register_buffer(axidma_dev_t dev, int dmabuf_fd, void *user_addr,
                           size_t size)
{
    int rc;
    struct axidma_register_buffer register_buffer;

    // Setup the argument structure to the IOCTL
    register_buffer.fd = dmabuf_fd;
    register_buffer.size = size;
    register_buffer.user_addr = user_addr;

    // Perform the buffer registration with the driver
    rc = ioctl(dev->fd, AXIDMA_REGISTER_BUFFER, &register_buffer);
    if (rc < 0) {
        perror("Failed to register the external DMA buffer");
    }

    return rc;
}

/* Unregisters a DMA buffer preivously registered with the driver. This is
 * required to clean up the kernel data structures. */
void axidma_unregister_buffer(axidma_dev_t dev, void *user_addr)
{
    int rc;

    // Perform the deregistration with the driver
    rc = ioctl(dev->fd, AXIDMA_UNREGISTER_BUFFER, user_addr);
    if (rc < 0) {
        perror("Failed to unregister the external DMA buffer");
        assert(false);
    }

    return;
}


/* This performs a one-way transfer over AXI DMA, the direction being specified
 * by the user. The user determines if this is blocking or not with `wait. */
int axidma_oneway_transfer(axidma_dev_t dev, int channel, void *buf,
        size_t len, bool wait)
{
    int rc;
    struct axidma_transaction trans;
    unsigned long axidma_cmd;
    dma_channel_t *dma_chan;

    assert(find_channel(dev, channel) != NULL);

    // Setup the argument structure to the IOCTL
    dma_chan = find_channel(dev, channel);
    trans.wait = wait;
    trans.channel_id = channel;
    trans.buf = buf;
    trans.buf_len = len;
    axidma_cmd = dir_to_ioctl(dma_chan->dir);

    // Perform the given transfer
    rc = ioctl(dev->fd, axidma_cmd, &trans);
    if (rc < 0) {
        perror("Failed to perform the AXI DMA transfer");
        return rc;
    }

    return 0;
}

/* This performs a two-way transfer over AXI DMA, both sending data out and
 * receiving it back over DMA. The user determines if this call is  blocking. */
int axidma_twoway_transfer(axidma_dev_t dev, int tx_channel, void *tx_buf,
        size_t tx_len, int rx_channel, void *rx_buf, size_t rx_len, bool wait)
{
    int rc;
    struct axidma_inout_transaction trans;

    assert(find_channel(dev, tx_channel) != NULL);
    assert(find_channel(dev, tx_channel)->dir == AXIDMA_WRITE);
    assert(find_channel(dev, rx_channel) != NULL);
    assert(find_channel(dev, rx_channel)->dir == AXIDMA_READ);

    // Setup the argument structure for the IOCTL
    trans.wait = wait;
    trans.tx_channel_id = tx_channel;
    trans.tx_buf = tx_buf;
    trans.tx_buf_len = tx_len;
    trans.rx_channel_id = rx_channel;
    trans.rx_buf = rx_buf;
    trans.rx_buf_len = rx_len;

    // Perform the read-write transfer
    rc = ioctl(dev->fd, AXIDMA_DMA_READWRITE, &trans);
    if (rc < 0) {
        perror("Failed to perform the AXI DMA read-write transfer");
    }

    return rc;
}

/* This function performs a video transfer over AXI DMA, setting up a VDMA
 * channel to either read from or write to given frame buffers on-demand
 * continuously. This call is always non-blocking. The transfer can only be
 * stopped with a call to axidma_stop_transfer. */
int axidma_video_transfer(axidma_dev_t dev, int display_channel, size_t width,
        size_t height, size_t depth, void **frame_buffers, int num_buffers)
{
    int rc;
    unsigned long axidma_cmd;
    struct axidma_video_transaction trans;
    dma_channel_t *dma_chan;

    assert(find_channel(dev, display_channel) != NULL);
    assert(find_channel(dev, display_channel)->type == AXIDMA_VDMA);

    // Setup the argument structure for the IOCTL
    dma_chan = find_channel(dev, display_channel);
    trans.channel_id = display_channel;
    trans.num_frame_buffers = num_buffers;
    trans.frame_buffers = frame_buffers;
    trans.width = width;
    trans.height = height;
    trans.depth = depth;
    axidma_cmd = (dma_chan->dir == AXIDMA_READ) ? AXIDMA_DMA_VIDEO_READ :
                                                  AXIDMA_DMA_VIDEO_WRITE;
    // Perform the video transfer
    rc = ioctl(dev->fd, axidma_cmd, &trans);
    if (rc < 0) {
        perror("Failed to perform the AXI DMA video write transfer");
    }

    return rc;
}

/* This function gets the residue of the last transaction. */
int axidma_get_residue(axidma_dev_t dev, int channel, unsigned int *residue) {
    int rc;
    struct axidma_residue res;

    res.channel_id = channel;
    rc = ioctl(dev->fd, AXIDMA_DMA_RESIDUE, &res);

    if (rc < 0) {
        perror("Failed to get the DMA residue");
    }
    else {
        *residue = res.residue;
    }

    return rc;
}

/* This function stops all transfers on the given channel with the given
 * direction. This function is required to stop any video transfers, or any
 * non-blocking transfers. */
void axidma_stop_transfer(axidma_dev_t dev, int channel)
{
    struct axidma_chan chan;
    dma_channel_t *dma_chan;

    assert(find_channel(dev, channel) != NULL);

    // Setup the argument structure for the IOCTL
    dma_chan = find_channel(dev, channel);
    chan.channel_id = channel;
    chan.dir = dma_chan->dir;
    chan.type = dma_chan->type;

    // Stop all transfers on the given DMA channel
    if (ioctl(dev->fd, AXIDMA_STOP_DMA_CHANNEL, &chan) < 0) {
        perror("Failed to stop the DMA channel");
        assert(false);
    }

    return;
}
