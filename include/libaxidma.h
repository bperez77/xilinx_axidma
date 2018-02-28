/**
 * @file libaxidma.h
 * @date Saturday, December 05, 2015 at 10:14:44 AM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This file defines the interface to the AXI DMA device through the AXI DMA
 * library.
 **/

#ifndef LIBAXIDMA_H_
#define LIBAXIDMA_H_

/**
 * The struct representing an AXI DMA device.
 *
 * This is an opaque type to the end user, so it can only be used as a pointer
 * or handle.
 **/
struct axidma_dev;

/**
 * Type definition for an AXI DMA device.
 *
 * This is a pointer to an opaque struct, so the user cannot access any of the
 * internal fields.
 **/
typedef struct axidma_dev* axidma_dev_t;

/**
 * A structure that represents an integer array.
 *
 * This is used to give the channel id's to the user in a convenient fashion.
 **/
typedef struct array {
    int len;        ///< Length of the array
    int *data;      ///< Pointer to the memory buffer for the array
} array_t;

/**
 * Type definition for a AXI DMA callback function.
 *
 * The callback function is invoked on completion of an asynchronous transfer,
 * if requested by the user. The library will pass the channel id of the DMA
 * channel that has finished, and the generic data the user registered.
 **/
typedef void (*axidma_cb_t)(int channel_id, void *data);

/**
 * Initializes the first AXI DMA device, returning a handle to the device.
 *
 * There is only one AXI DMA device per DMA, since it represents all of the
 * available channels. Thus, this function should only be invoked once, unless
 * a call has been made to #axidma_destroy. Otherwise, this function will abort.
 *
 * This function is equivalent with axidma_init_dev(0).
 *
 * @return A handle to the AXI DMA device on success, NULL on failure.
 **/
struct axidma_dev *axidma_init();

/**
 * Initializes a AXI DMA device, returning a handle to the device.
 *
 * There is only one AXI DMA device per DMA, since it represents all of the
 * available channels. Thus, this function should only be invoked once for every
 * DMA, unless a call has been made to #axidma_destroy. Otherwise, this function
 * will abort.
 *
 * @param[in] index The index of the device, specified in the device tree.
 * @return A handle to the AXI DMA device on success, NULL on failure.
 **/
struct axidma_dev *axidma_init_dev(unsigned int index);

/**
 * Tears down and destroys one AXI DMA device, deallocating its resources.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 **/
void axidma_destroy(axidma_dev_t dev);

/**
 * Gets the available AXI DMA transmit channels, returning their channel ID's.
 *
 * In our terminology, the "transmit" direction is defined as from the processor
 * to the FPGA. This function is guaranteed to never fail.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @return An array of channel ID's of the available AXI DMA transmit channels.
 **/
const array_t *axidma_get_dma_tx(axidma_dev_t dev);

/**
 * Gets the available AXI DMA transmit channels, returning their channel ID's.
 *
 * In our terminology, the "receive" direction is defined as from the FPGA to
 * the processor. This function is guaranteed to never fail.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @return An array of channel ID's of the available AXI DMA receive channels.
 **/
const array_t *axidma_get_dma_rx(axidma_dev_t dev);

/**
 * Gets the available AXI VDMA transmit channels, returning their channel ID's.
 *
 * In our terminology, the "transmit" direction is defined as from the processor
 * to the FPGA. This function is guaranteed to never fail.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @return An array of channel ID's of the available AXI VDMA transmit channels.
 **/
const array_t *axidma_get_vdma_tx(axidma_dev_t dev);

/**
 * Gets the available AXI VDMA receive channels, returning their channel ID's.
 *
 * In our terminology, the "receive" direction is defined as from the FPGA to
 * the processor. This function is guaranteed to never fail.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @return An array of channel ID's of the available AXI VDMA receive channels.
 **/
const array_t *axidma_get_vdma_rx(axidma_dev_t dev);

/**
 * Allocates DMA buffer suitable for an AXI DMA/VDMA device of \p size bytes.
 *
 * This function allocates a DMA buffer that can be shared between the
 * processor and FPGA and is suitable for high bandwidth transfers. This means
 * that it is coherent between the FPGA and processor, and is contiguous in
 * physical memory.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] size The size of the buffer in bytes.
 * @return The address of buffer on success, NULL on failure.
 **/
void *axidma_malloc(axidma_dev_t dev, size_t size);

/**
 * Frees a DMA buffer previously allocated by #axidma_malloc.
 *
 * This function will abort if \p addr is not an address previously returned by
 * #axidma_malloc, or if \p size does not match the value used when the buffer
 * was allocated.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] addr Address of the buffer returned by #axidma_malloc.
 * @param[in] size Size of the buffer passed when it was allocated by
 *                 #axidma_malloc.
 **/
void axidma_free(axidma_dev_t dev, void *addr, size_t size);

/**
 * Registers a DMA buffer that was allocated externally, by another driver.
 *
 * An "external" DMA buffer is a DMA buffer that was allocated by another
 * driver. For example, you might want to perform DMA transfers on a frame
 * buffer allocated by a display rendering manager (DRM) driver. Registering
 * the DMA buffer allows for the AXI DMA device to access it and perform
 * transfers.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] dmabuf_fd File descriptor corresponding to the buffer. This
 *                      corresponds to the file descriptor passed to the mmap
 *                      call that allocated the buffer.
 * @param[in] user_addr Address of the external buffer.
 * @param[in] size Size of the buffer in bytes.
 * @return 0 on success, a negative integer on failure.
 **/
int axidma_register_buffer(axidma_dev_t dev, int dmabuf_fd, void *user_addr,
                           size_t size);

/**
 * Unregisters an external DMA buffer that was previously registered by
 * #axidma_register_buffer.
 *
 * If \p user_addr is not has not been previously registered with a call to
 * #axidma_register_buffer, then this function will abort.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] user_addr Address of the external buffer. This must have
 *                      previously been registered wtih a call to
 *                      #axidma_register_buffer.
 **/
void axidma_unregister_buffer(axidma_dev_t dev, void *user_addr);


/**
 * Registers a user callback function to be invoked upon completion of an
 * asynchronous transfer for the specified DMA channel.
 *
 * The callback will be invoked with a POSIX real-time signal, so it will
 * happen as soon as possible to the completion. The \p data will be passed to
 * the callback function. This function can never fail.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] channel DMA channel to register the callback for.
 * @param[in] callback Callback function invoked when the asynchronous transfer
 *                     completes.
 * @param[in] data Generic user data that is passed to the callback function.
 **/
void axidma_set_callback(axidma_dev_t dev, int channel, axidma_cb_t callback,
                         void *data);

/**
 * Performs a single DMA transfer in the specified direction on the DMA channel.
 *
 * This function will perform a single DMA transfer using the specified buffer.
 * If wait is false, then this function will be non-blocking, and if the user
 * registered a callback function, it will be invoked upon completion of the
 * transfer.
 *
 * The addresses \p buf and \p buf+\p len must be within a buffer that was
 * previously allocated by #axidma_malloc or registered with
 * #axidma_register_buffer. This function will abort if the channel is invalid.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] channel DMA channel the transfer is performed on.
 * @param[in] buf Address of the DMA buffer to transfer, previously allocated by
 *                #axidma_malloc or registered with #axidma_register_buffer.
 * @param[in] len Number of bytes that will be transfered.
 * @param[in] wait Indicates if the transfer should be synchronous or
 *                 asynchronous. If true, this function will block.
 * @return 0 upon success, a negative number on failure.
 **/
int axidma_oneway_transfer(axidma_dev_t dev, int channel, void *buf, size_t len,
        bool wait);

/**
 * Performs a two coupled DMA transfers, one in the receive direction, the other
 * in the transmit direction.
 *
 * This function will perform a receive and transmit DMA transfer using the
 * specified buffers. If wait is false, the user's callback function will be
 * invoked on the channels for which a callback was registered.
 *
 * This function will abort if either of the specified channels do not exist,
 * or if the channel does not support the direction requested.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] tx_channel DMA channel the transmit transfer is performed on.
 * @param[in] tx_buf Address of the DMA buffer to transmit, previously allocated
 *                   by #axidma_malloc or registered with
 *                   #axidma_register_buffer.
 * @param[in] tx_len Number of bytes to transmit from \p tx_buf.
 * @param[in] rx_channel DMA channel the receive transfer is performed on.
 * @param[in] rx_buf Address of the DMA buffer to receive, previously allocated
 *                   by #axidma_malloc or registered with
 *                   #axidma_register_buffer.
 * @param[in] rx_len Number of bytes to receive into \p rx_buf.
 * @param[in] wait Indicates if the transfer should be synchronous or
 *                 asynchronous. If true, this function will block.
 * @return 0 upon success, a negative number on failure.
 **/
int axidma_twoway_transfer(axidma_dev_t dev, int tx_channel, void *tx_buf,
        size_t tx_len, int rx_channel, void *rx_buf, size_t rx_len, bool wait);

/**
 * Starts a video DMA (VDMA) loop/continuous transfer on the given channel.
 *
 * A video loop transfer differs from a typical DMA transfer in that it is
 * cyclic, and ends only when requested by the user. A video loop transfer will
 * continuously transmit/receive the frame buffers, transmitting the first
 * buffer, then the second, etc., and then repeating from the beginning once the
 * last buffer is reached. This is suitable when continuously sending data to a
 * display, or continuous receiving data from a camera.
 *
 * This function supports an arbitrary number of frame buffers, allowing
 * for both double-buffering and triple-buffering. This function is
 * non-blocking, and returns immediately. The only way to stop the transfer is
 * via a call to #axidma_stop_transfer.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] display_channel DMA channel the video transfer will take place
 *                            on. This must be a VDMA channel.
 * @param[in] width The number of pixels in a row of the frame buffer.
 * @param[in] height The number rows in the frame buffer.
 * @param[in] depth The number of bytes in a pixel.
 * @param[in] frame_buffers A list of frame buffer addresses.
 * @param[in] num_buffers The number of buffers in \p frame_buffers. This must
 *                        match the length of the list.
 * @return 0 upon success, a negative number on failure.
 **/
int axidma_video_transfer(axidma_dev_t dev, int display_channel, size_t width,
        size_t height, size_t depth, void **frame_buffers, int num_buffers);

/**
 * Get the residue of the last transaction
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] channel DMA channel.
 * @param[in] residue A pointer to store the returned residue.
 * @return 0 upon success, a negative number on failure.
 **/
int axidma_get_residue(axidma_dev_t dev, int channel, unsigned int *residue);

/**
 * Stops the DMA transfer on specified DMA channel.
 *
 * This function stops transfers on either DMA or VDMA channels.
 *
 * This function will abort if the channel is invalid, or if the DMA channel
 * currently has no running transaction on it.
 *
 * @param[in] dev An #axidma_dev_t returned by #axidma_init.
 * @param[in] channel DMA channel to stop the transfer on.
 **/
void axidma_stop_transfer(axidma_dev_t dev, int channel);

#endif /* LIBAXIDMA_H_ */
