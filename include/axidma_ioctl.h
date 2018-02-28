/**
 * @file axidma_ioctl.h
 * @date Tuesday, November 24, 2015 at 09:48:17 PM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This file contains the IOCTL interface definition. This is the interface from
 * userspace to the AXI DMA device to initiate DMA transactions and get
 * information about AXI DMA devices on the system.
 **/

#ifndef AXIDMA_IOCTL_H_
#define AXIDMA_IOCTL_H_

#include <asm/ioctl.h>              // IOCTL macros

/*----------------------------------------------------------------------------
 * IOCTL Defintions
 *----------------------------------------------------------------------------*/

// The standard name for the AXI DMA device
#define AXIDMA_DEV_NAME     "axidma"

// The standard path to the AXI DMA device
#define AXIDMA_DEV_PATH     ("/dev/" AXIDMA_DEV_NAME)

/*----------------------------------------------------------------------------
 * IOCTL Argument Definitions
 *----------------------------------------------------------------------------*/

// Forward declaration of the kernel's DMA channel type (opaque to userspace)
struct dma_chan;

// Direction from the persepctive of the processor
/**
 * Enumeration for direction in a DMA transfer.
 *
 * The enumeration has two directions: write is from the processor to the
 * FPGA, and read is from the FPGA to the processor.
 **/
enum axidma_dir {
    AXIDMA_WRITE,                   ///< Transmits from memory to a device.
    AXIDMA_READ                     ///< Transmits from a device to memory.
};

/**
 * Enumeration for the type of a DMA channel.
 *
 * There are two types of channels, the standard DMA channel, and the special
 * video DMA (VDMA) channel. The VDMA channel is for transferring frame buffers
 * and other display related data.
 **/
enum axidma_type {
    AXIDMA_DMA,                     ///< Standard AXI DMA engine
    AXIDMA_VDMA                     ///< Specialized AXI video DMA enginge
};

// TODO: Channel really should not be here
struct axidma_chan {
    enum axidma_dir dir;            // The DMA direction of the channel
    enum axidma_type type;          // The DMA type of the channel
    int channel_id;                 // The identifier for the device
    const char *name;               // Name of the channel (ignore)
    struct dma_chan *chan;          // The DMA channel (ignore)
};

struct axidma_num_channels {
    int num_channels;               // Total DMA channels in the system
    int num_dma_tx_channels;        // DMA transmit channels available
    int num_dma_rx_channels;        // DMA receive channels available
    int num_vdma_tx_channels;       // VDMA transmit channels available
    int num_vdma_rx_channels;       // VDMA receive channels available
};

struct axidma_channel_info {
    struct axidma_chan *channels;   // Metadata about all available channels
};

struct axidma_signal_info {
    int signal;
    void *user_data;
};

struct axidma_register_buffer {
    int fd;                         // Anonymous file descritpor for DMA buffer
    size_t size;                    // The size of the external DMA buffer
    void *user_addr;                // User virtual address of the buffer
};

struct axidma_transaction {
    bool wait;                      // Indicates if the call is blocking
    int channel_id;                 // The id of the DMA channel to use
    void *buf;                      // The buffer used for the transaction
    size_t buf_len;                 // The length of the buffer
};

struct axidma_inout_transaction {
    bool wait;                      // Indicates if the call is blocking
    int tx_channel_id;              // The id of the transmit DMA channel
    void *tx_buf;                   // The buffer containing the data to send
    size_t tx_buf_len;              // The length of the transmit buffer
    int rx_channel_id;              // The id of the receive DMA channel
    void *rx_buf;                   // The buffer to place the data in
    size_t rx_buf_len;              // The length of the receive buffer
};

struct axidma_video_transaction {
    int channel_id;             // The id of the DMA channel to transmit video
    int num_frame_buffers;      // The number of frame buffers to use.
    void **frame_buffers;       // The frame buffer addresses to use for video
    size_t width;               // The width of the image in pixels
    size_t height;              // The height of the image in lines
    size_t depth;               // The size of each pixel in bytes
};

struct axidma_residue {
    int channel_id;             // The id of the DMA channel
    unsigned int residue;       // The returned residue
};

/*----------------------------------------------------------------------------
 * IOCTL Interface
 *----------------------------------------------------------------------------*/

// The magic number used to distinguish IOCTL's for our device
#define AXIDMA_IOCTL_MAGIC              'W'

// The number of IOCTL's implemented, used for verification
#define AXIDMA_NUM_IOCTLS               11

/**
 * Returns the number of available DMA channels in the system.
 *
 * This writes to the structure given as input by the user, telling the
 * numbers for all DMA channels in the system.
 *
 * Outputs:
 *  - num_channels - The total number of DMA channels in the system.
 *  - num_dma_tx_channels - The number of transmit AXI DMA channels
 *  - num_dma_rx_channels - The number of receive AXI DMA channels
 *  - num_vdma_tx_channels - The number of transmit AXI VDMA channels
 *  - num_vdma_rx_channels - The number of receive AXI VDMA channels
 **/
#define AXIDMA_GET_NUM_DMA_CHANNELS     _IOW(AXIDMA_IOCTL_MAGIC, 0, \
                                             struct axidma_num_channels)

/**
 * Returns information on all DMA channels in the system.
 *
 * This function writes to the array specified in the pointer given to the
 * struct structures representing all data about a given DMA channel (device id,
 * direction, etc.). The array must be able to hold at least the number of
 * elements returned by AXIDMA_GET_NUM_DMA_CHANNELS.
 *
 * The important piece of information returned in the id for the channels.
 * This, along with the direction and type, uniquely identifies a DMA channel
 * in the system, and this is how you refer to a channel in later calls.
 *
 * Inputs:
 *  - channels - A pointer to a region of memory that can hold at least
 *               num_channels * sizeof(struct axidma_chan) bytes.
 *
 * Outputs:
 *  - channels - An array of structures of the following format:
 *  - An array of structures with the following fields:
 *       - dir - The direction of the channel (either read or write).
 *       - type - The type of the channel (either normal DMA or video DMA).
 *       - channel_id - The integer id for the channel.
 *       - chan - This field has no meaning and can be safely ignored.
 **/
#define AXIDMA_GET_DMA_CHANNELS         _IOR(AXIDMA_IOCTL_MAGIC, 1, \
                                             struct axidma_channel_info)

/**
 * Register the given signal to be sent when DMA transactions complete.
 *
 * This function sets up an asynchronous signal to be delivered to the invoking
 * process any DMA subsystem completes a transaction. If the user dispatches
 * an asynchronous transaction, and wants to know when it completes, they must
 * register a signal to be delivered.
 *
 * The signal must be one of the POSIX real time signals. So, it must be
 * between the signals SIGRTMIN and SIGRTMAX. The kernel will deliver the
 * channel id back to the userspace signal handler.
 *
 * This can be used to have a user callback function, effectively emulating an
 * interrupt in userspace. The user must register their signal handler for
 * the specified signal for this to happen.
 *
 * Inputs:
 *  - signal - The signal to send upon transaction completion.
 **/
#define AXIDMA_SET_DMA_SIGNAL           _IO(AXIDMA_IOCTL_MAGIC, 2)

/**
 * Registers the external DMA buffer with the driver, making it available to be
 * used in DMA transfers.
 *
 * Sometimes, it may be useful to use a DMA buffer that was allocated by another
 * driver. The best example of this is if you want to interact with a
 * frame buffer allocated by a DRM driver.
 *
 * However, the driver cannot simply access this DMA buffer as is. The user must
 * register the buffer with the driver, so that it can get the information from
 * the driver that originally allocated it.
 *
 * This uses the kernel's DMA buffer sharing API. Thus, the user must first tell
 * the other driver to export the DMA buffer for sharing, typically done through
 * an IOCTL. This will return a file descriptor, which the user must pass into
 * this function, along with the virtual address in userspace.
 *
 * Inputs:
 *  - fd - File descriptor corresponding to the DMA buffer share.
 *  - size - The size of the DMA buffer in bytes.
 *  - user_addr - The user virtual address of the buffer.
 **/
#define AXIDMA_REGISTER_BUFFER          _IOR(AXIDMA_IOCTL_MAGIC, 3, \
                                             struct axidma_register_buffer)

/**
 * Receives the data from the logic fabric into the processing system.
 *
 * This function receives data from a device on the PL fabric through
 * AXI DMA into memory. The device id should be an id that is returned by the
 * get dma channels ioctl. The user can specify if the call should wait for the
 * transfer to complete, or if it should return immediately.
 *
 * The specified buffer must be within an address range that was allocated by a
 * call to mmap with the AXI DMA device. Also, the buffer must be able to hold
 * at least `buf_len` bytes.
 *
 * Inputs:
 *  - wait - Indicates if the call should be blocking or non-blocking
 *  - channel_id - The id for the channel you want receive data over.
 *  - buf - The address of the buffer you want to receive the data in.
 *  - buf_len - The number of bytes to receive.
 **/
#define AXIDMA_DMA_READ                 _IOR(AXIDMA_IOCTL_MAGIC, 4, \
                                             struct axidma_transaction)

/**
 * Sends the given data from the processing system to the logic fabric.
 *
 * This function sends data from memory to a device on the PL fabric through
 * AXI DMA. The device id should be an id that is returned by the get dma
 * channels ioctl. The user can specify if the call should wait for the transfer
 * to complete, or if it should return immediately.
 *
 * The specified buffer must be within an address range that was allocated by a
 * call to mmap with the AXI DMA device. Also, the buffer must be able to hold
 * at least `buf_len` bytes.
 *
 * Inputs:
 *  - wait - Indicates if the call should be blocking or non-blocking
 *  - channel_id - The id for the channel you want to send data over.
 *  - buf - The address of the data you want to send.
 *  - buf_len - The number of bytes to send.
 **/
#define AXIDMA_DMA_WRITE                _IOR(AXIDMA_IOCTL_MAGIC, 5, \
                                             struct axidma_transaction)

/**
 * Performs a two-way transfer between the logic fabric and processing system.
 *
 * This function both sends data to the PL and receives data from the PL fabric.
 * It is intended for DMA transfers that are tightly coupled together
 * (e.g. converting an image to grayscale on the PL fabric). The device id's for
 * both channels should be ones that are returned by the get dma ioctl. The user
 * can specify if the call should block. If it blocks, it will wait until the
 * receive transaction completes.
 *
 * The specified buffers must be within an address range that was allocated by a
 * call to mmap with the AXI DMA device. Also, each buffer must be able to hold
 * at least the number of bytes that are being transfered.
 *
 * Inputs:
 *  - wait - Indicates if the call should be blocking or non-blocking
 *  - tx_channel_id - The id for the channel you want transmit data on.
 *  - tx_buf - The address of the data you want to send.
 *  - tx_buf_len - The number of bytes you want to send.
 *  - rx_buf - The address of the buffer you want to receive data in.
 *  - rx_buf_len - The number of bytes you want to receive.
 **/
#define AXIDMA_DMA_READWRITE            _IOR(AXIDMA_IOCTL_MAGIC, 6, \
                                             struct axidma_inout_transaction)

/**
 * Performs frame-buffer based transfers from a camera on the fabric.
 *
 * This function performs a video transfer from the logic fabric. It receives
 * the given buffers from logic fabric (intended for a camera pipeline). When it
 * reaches the end of the buffers, it loops back and receives the data again in
 * the first buffer. This is used for frame-buffer based cameras.
 *
 * All of the frame buffers must be within an address range that was allocated
 * by a call to mmap with the AXI DMA device. Also, each buffer must
 * be able to hold a frame of (width * height * depth) bytes. The input array of
 * buffers must be a memory location that holds `num_frame_buffers` addresses.
 *
 * This call is always non-blocking as the VDMA engine will run forever. In
 * order to end the transaction, you must make a call to the stop dma channel
 * ioctl.
 *
 * Inputs:
 *  - channel_id - The id for the channel you want to send data over.
 *  - num_frame_buffers - The number of frame buffers you're using.
 *  - frame_buffers - An array of the frame buffer addresses.
 *  - width - The width of the frame (image) in pixels.
 *  - height - The height of the frame in lines.
 *  - depth - The size of each pixel in the frame in bytes.
 **/
#define AXIDMA_DMA_VIDEO_READ           _IOR(AXIDMA_IOCTL_MAGIC, 7, \
                                             struct axidma_video_transaction)

/**
 * Performs frame-buffer based transfers to a display on the logic fabric.
 *
 * This function performs a video transfer to the logic fabric. It sends
 * the given buffers to logic fabric (intended for a display pipeline). When it
 * reaches the end of the buffers, it loops back and re-sends the first buffer.
 * This is used for frame-buffer based graphics.
 *
 * All of the frame buffers must be within an address range that was allocated
 * by a call to mmap with the AXI DMA device. Also, each buffer must
 * be able to hold a frame of (width * height * depth) bytes. The input array of
 * buffers must be a memory location that holds `num_frame_buffers` addresses.
 *
 * This call is always non-blocking as the VDMA engine will run forever. In
 * order to end the transaction, you must make a call to the stop dma channel
 * ioctl.
 *
 * Inputs:
 *  - channel_id - The id for the channel you want to send data over.
 *  - num_frame_buffers - The number of frame buffers you're using.
 *  - frame_buffers - An array of the frame buffer addresses.
 *  - width - The width of the frame (image) in pixels.
 *  - height - The height of the frame in lines.
 *  - depth - The size of each pixel in the frame in bytes.
 **/
#define AXIDMA_DMA_VIDEO_WRITE          _IOR(AXIDMA_IOCTL_MAGIC, 8, \
                                             struct axidma_video_transaction)

/**
 * Get the residue of the last transaction
 *
 * Inputs:
 *  - channel_id - The id for the channel you want to get the residue.
 *  - residue - The returned residue.
 **/
#define AXIDMA_DMA_RESIDUE              _IOR(AXIDMA_IOCTL_MAGIC, 10, \
                                             struct axidma_residue)

/**
 * Stops all transactions on the given DMA channel.
 *
 * This function flushes all in-progress transactions, and discards all pending
 * transactions on the given DMA channel. The specified id should be one that
 * was returned by the get dma channels ioctl.
 *
 * Inputs:
 *  - dir - The direction of the channel (either read or write).
 *  - type - The type of the channel (either normal DMA or video DMA).
 *  - channel_id - The integer id for the channel.
 *  - chan - This field is unused an can be safely left uninitialized.
 */
#define AXIDMA_STOP_DMA_CHANNEL         _IOR(AXIDMA_IOCTL_MAGIC, 9, \
                                             struct axidma_chan)

/**
 * Unregisters and external DMA buffer previously registered through an
 * AXIDMA_REGISTER_BUFFER IOCTL
 *
 * All external buffers that are registered by the user must be freed in order
 * to ensure that all kernel data structures are properly cleaned up. This
 * removes the external DMA buffer from the driver, so it can no longer be
 * used in DMA transfers after this call.
 *
 * Inputs:
 *  - user_addr - The user virtual address of the external DMA buffer.
 **/
#define AXIDMA_UNREGISTER_BUFFER        _IO(AXIDMA_IOCTL_MAGIC, 10)

#endif /* AXIDMA_IOCTL_H_ */
