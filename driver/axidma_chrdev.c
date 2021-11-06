/**
 * @file axidma_chrdev.c
 * @date Saturday, November 14, 2015 at 12:03:13 PM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This file contains the implementation of the character device for the AXI DMA
 * module.
 *
 * @bug No known bugs.
 **/

// Kernel dependencies
#include <linux/version.h>
#include <linux/list.h>         // Linked list definitions and functions
#include <linux/sched.h>        // `Current` global variable for current task
#include <linux/device.h>       // Device and class creation functions
#include <linux/cdev.h>         // Character device functions
#include <linux/ioctl.h>        // IOCTL macros and definitions
#include <linux/fs.h>           // File operations and file types
#include <linux/mm.h>           // Memory types and remapping functions
#include <linux/uaccess.h>      // Userspace memory access functions
#include <linux/slab.h>         // Kernel allocation functions
#include <linux/errno.h>        // Linux error codes
#include <linux/of_device.h>    // Device tree device related functions

#include <linux/dma-buf.h>      // DMA shared buffers interface
#include <linux/scatterlist.h>  // Scatter-gather table definitions

// Local dependencies
#include "axidma.h"             // Local definitions
#include "axidma_ioctl.h"       // IOCTL interface for the device

/*----------------------------------------------------------------------------
 * Internal Definitions
 *----------------------------------------------------------------------------*/

// TODO: Maybe this can be improved?
static struct axidma_device *axidma_dev;

// A structure that represents a DMA buffer allocation
struct axidma_dma_allocation {
    size_t size;                // Size of the buffer
    void *user_addr;            // User virtual address of the buffer
    void *kern_addr;            // Kernel virtual address of the buffer
    dma_addr_t dma_addr;        // DMA bus address of the buffer
    struct list_head list;      // List node pointers for allocation list
};

/* A structure that represents a DMA buffer allocation imported from another
 * driver in the kernel, through the DMA buffer sharing interface. */
struct axidma_external_allocation {
    int fd;                                 // File descritpor for buffer share
    struct dma_buf *dma_buf;                // Structure representing the buffer
    struct dma_buf_attachment *dma_attach;  // Structre represnting attachment
    size_t size;                            // Total size of the buffer
    void *user_addr;                        // Buffer's user virtual address
    struct sg_table *sg_table;              // DMA scatter-gather table
    struct list_head list;                  // Node pointers for the list
};

/*----------------------------------------------------------------------------
 * VMA Operations
 *----------------------------------------------------------------------------*/

static bool valid_dma_request(void *dma_start, size_t dma_size, void *user_addr,
                              size_t user_size)
{
    return dma_start <= user_addr &&
           (char *)user_addr + user_size <= (char *)dma_start + dma_size;
}

/* Converts the given user space virtual address to a DMA address. If the
 * conversion is unsuccessful, then (dma_addr_t)NULL is returned. */
dma_addr_t axidma_uservirt_to_dma(struct axidma_device *dev, void *user_addr,
                                  size_t size)
{
    bool valid;
    dma_addr_t offset;
    struct list_head *iter;
    struct axidma_dma_allocation *dma_alloc;
    struct axidma_external_allocation *dma_ext_alloc;

    // First iterate over DMA buffers allocated by this driver
    list_for_each(iter, &dev->dmabuf_list)
    {
        dma_alloc = container_of(iter, struct axidma_dma_allocation, list);
        valid = valid_dma_request(dma_alloc->user_addr, dma_alloc->size,
                                  user_addr, size);
        if (valid) {
            offset = (dma_addr_t)(user_addr - dma_alloc->user_addr);
            return dma_alloc->dma_addr + offset;
        }
    }

    // Otherwise, iterate over the DMA buffers allocated by other drivers
    list_for_each(iter, &dev->external_dmabufs)
    {
        dma_ext_alloc = container_of(iter, struct axidma_external_allocation,
                                     list);
        valid = valid_dma_request(dma_ext_alloc->user_addr, dma_ext_alloc->size,
                                  user_addr, size);
        if (valid) {
            offset = (dma_addr_t)(user_addr - dma_ext_alloc->user_addr);
            return sg_dma_address(&dma_ext_alloc->sg_table->sgl[0]) + offset;
        }
    }

    // No matching allocation was found
    return (dma_addr_t)NULL;
}

static int axidma_get_external(struct axidma_device *dev,
                               struct axidma_register_buffer *ext_buf)
{
    int rc;
    struct axidma_external_allocation *dma_alloc;

    // Allocate a structure to store information about the external buffer
    dma_alloc = kmalloc(sizeof(*dma_alloc), GFP_KERNEL);
    if (dma_alloc == NULL) {
        axidma_err("Unable to allocate external DMA allocation structure.\n");
        return -ENOMEM;
    }

    // Get the DMA buffer corresponding to the anonymous file descriptor
    dma_alloc->fd = ext_buf->fd;
    dma_alloc->dma_buf = dma_buf_get(ext_buf->fd);
    if (IS_ERR(dma_alloc->dma_buf)) {
        axidma_err("Unable to find the external DMA buffer.\n");
        rc = PTR_ERR(dma_alloc->dma_buf);
        goto free_ext_alloc;
    }

    // Attach ourselves to the DMA buffer, indicating usage
    dma_alloc->dma_attach = dma_buf_attach(dma_alloc->dma_buf, dev->device);
    if (IS_ERR(dma_alloc->dma_attach)) {
        axidma_err("Unable to attach to the external DMA buffer.\n");
        rc = PTR_ERR(dma_alloc->dma_attach);
        goto put_ext_dma;
    }

    // TODO: Maybe this should only be done on-demand
    // Map the DMA buffer for the life of our attachment
    dma_alloc->sg_table = dma_buf_map_attachment(dma_alloc->dma_attach,
                                                 DMA_BIDIRECTIONAL);
    if (IS_ERR(dma_alloc->sg_table)) {
        axidma_err("Unable to map external DMA buffer for usage.\n");
        rc = PTR_ERR(dma_alloc->sg_table);
        goto detach_ext_dma;
    }

    // The allocation is expected to be one contiguous memory region
    if (dma_alloc->sg_table->nents != 1) {
        axidma_err("External DMA allocations must a single contiguous region "
                   "of physical memory.\n");
        rc = -EINVAL;
        goto unmap_ext_dma;
    }

    // Add ourselves the driver's list of external allocations
    dma_alloc->size = ext_buf->size;
    dma_alloc->user_addr = ext_buf->user_addr;
    list_add(&dma_alloc->list, &dev->external_dmabufs);
    return 0;

unmap_ext_dma:
    dma_buf_unmap_attachment(dma_alloc->dma_attach, dma_alloc->sg_table,
                             DMA_BIDIRECTIONAL);
detach_ext_dma:
    dma_buf_detach(dma_alloc->dma_buf, dma_alloc->dma_attach);
put_ext_dma:
    dma_buf_put(dma_alloc->dma_buf);
free_ext_alloc:
    kfree(dma_alloc);
    return rc;
}

static int axidma_put_external(struct axidma_device *dev, void *user_addr)
{
    void *end_user_addr;
    struct list_head *iter;
    struct axidma_external_allocation *dma_alloc;

    // Find the allocation corresponding to the user address
    list_for_each(iter, &dev->external_dmabufs)
    {
        dma_alloc = container_of(iter, struct axidma_external_allocation, list);
        end_user_addr = (char *)dma_alloc->user_addr + dma_alloc->size;

        if (dma_alloc->user_addr <= user_addr && user_addr <= end_user_addr) {
            // Unmap the buffer, and detach ourselves from it
            dma_buf_unmap_attachment(dma_alloc->dma_attach,
                    dma_alloc->sg_table, DMA_BIDIRECTIONAL);
            dma_buf_detach(dma_alloc->dma_buf, dma_alloc->dma_attach);
            dma_buf_put(dma_alloc->dma_buf);

            // Free the allocation structure
            kfree(dma_alloc);
            return 0;
        }
    }

    return -ENOENT;
}

static void axidma_vma_close(struct vm_area_struct *vma)
{
    struct axidma_device *dev;
    struct axidma_dma_allocation *dma_alloc;

    // Get the AXI DMA allocation data and free the DMA buffer
    dev = axidma_dev;
    dma_alloc = vma->vm_private_data;
    dma_free_coherent(&dev->pdev->dev, dma_alloc->size, dma_alloc->kern_addr,
                      dma_alloc->dma_addr);

    // Remove the allocation from the list, and free the structure
    list_del(&dma_alloc->list);
    kfree(dma_alloc);

    return;
}

// The VMA operations for the AXI DMA device
static const struct vm_operations_struct axidma_vm_ops = {
    .close = axidma_vma_close,
};

/*----------------------------------------------------------------------------
 * File Operations
 *----------------------------------------------------------------------------*/

static int axidma_open(struct inode *inode, struct file *file)
{
    // Only the root user can open this device, and it must be exclusive
    if (!capable(CAP_SYS_ADMIN)) {
        axidma_err("Only root can open this device.");
        return -EACCES;
    } else if (!(file->f_flags & O_EXCL)) {
        axidma_err("O_EXCL must be specified for open()\n");
        return -EINVAL;
    }

    // Place the axidma structure in the private data of the file
    file->private_data = (void *)axidma_dev;
    return 0;
}

static int axidma_release(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}

static int axidma_mmap(struct file *file, struct vm_area_struct *vma)
{
    int rc;
    struct axidma_device *dev;
    struct axidma_dma_allocation *dma_alloc;

    // Get the axidma device structure
    dev = file->private_data;

    // Allocate a structure to store data about the DMA mapping
    dma_alloc = kmalloc(sizeof(*dma_alloc), GFP_KERNEL);
    if (dma_alloc == NULL) {
        axidma_err("Unable to allocate VMA data structure.");
        rc = -ENOMEM;
        goto ret;
    }

    // Set the user virtual address and the size
    dma_alloc->size = vma->vm_end - vma->vm_start;
    dma_alloc->user_addr = (void *)vma->vm_start;

    // Configure the DMA device
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
    of_dma_configure(dev->device, NULL);
#else
    of_dma_configure(&dev->pdev->dev, NULL, true);
#endif

    // Allocate the requested region a contiguous and uncached for DMA
    dma_alloc->kern_addr = dma_alloc_coherent(&dev->pdev->dev, dma_alloc->size,
                                              &dma_alloc->dma_addr, GFP_KERNEL);
    if (dma_alloc->kern_addr == NULL) {
        axidma_err("Unable to allocate contiguous DMA memory region of size "
                   "%zu.\n", dma_alloc->size);
        axidma_err("Please make sure that you specified cma=<size> on the "
                   "kernel command line, and the size is large enough.\n");
        rc = -ENOMEM;
        goto free_vma_data;
    }

    // Map the region into userspace
    rc = dma_mmap_coherent(&dev->pdev->dev, vma, dma_alloc->kern_addr,
                           dma_alloc->dma_addr, dma_alloc->size);
    if (rc < 0) {
        axidma_err("Unable to remap address %p to userspace address %p, size "
                   "%zu.\n", dma_alloc->kern_addr, dma_alloc->user_addr,
                   dma_alloc->size);
        goto free_dma_region;
    }

    /* Override the VMA close with our call, so that we can free the DMA region
     * when the memory region is closed. Pass in the data to do so. */
    vma->vm_ops = &axidma_vm_ops;
    vma->vm_private_data = dma_alloc;

    // Do not copy this memory region if this process is forked.
    /* TODO: Figure out the proper way to actually handle multiple processes
     * referring to the DMA buffer. */
    vma->vm_flags |= VM_DONTCOPY;

    // Add the allocation to the driver's list of DMA buffers
    list_add(&dma_alloc->list, &dev->dmabuf_list);
    return 0;

free_dma_region:
    dma_free_coherent(&dev->pdev->dev, dma_alloc->size, dma_alloc->kern_addr,
                      dma_alloc->dma_addr);
free_vma_data:
    kfree(dma_alloc);
ret:
    return rc;
}

/* Verifies that the pointer can be read and/or written to with the given size.
 * The user specifies the mode, either readonly, or not (read-write). */
static bool axidma_access_ok(const void __user *arg, size_t size, bool readonly)
{
    // Note that VERIFY_WRITE implies VERIFY_WRITE, so read-write is handled
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
    if (!readonly && !access_ok(VERIFY_WRITE, arg, size)) {
        axidma_err("Argument address %p, size %zu cannot be written to.\n",
                   arg, size);
        return false;
    } else if (!access_ok(VERIFY_READ, arg, size)) {
        axidma_err("Argument address %p, size %zu cannot be read from.\n",
                   arg, size);
        return false;
    }
#else
    if (!readonly && !access_ok(arg, size)) {
        axidma_err("Argument address %p, size %zu cannot be written to.\n",
                   arg, size);
        return false;
    } else if (!access_ok(arg, size)) {
        axidma_err("Argument address %p, size %zu cannot be read from.\n",
                   arg, size);
        return false;
    }
#endif

    return true;
}

static long axidma_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long rc;
    size_t size;
    void *__user arg_ptr;
    struct axidma_device *dev;
    struct axidma_num_channels num_chans;
    struct axidma_channel_info usr_chans, kern_chans;
    struct axidma_register_buffer ext_buf;
    struct axidma_transaction trans;
    struct axidma_inout_transaction inout_trans;
    struct axidma_video_transaction video_trans, *__user user_video_trans;
    struct axidma_chan chan_info;

    // Coerce the arguement as a userspace pointer
    arg_ptr = (void __user *)arg;

    // Verify that this IOCTL is intended for our device, and is in range
    if (_IOC_TYPE(cmd) != AXIDMA_IOCTL_MAGIC) {
        axidma_err("IOCTL command magic number does not match.\n");
        return -ENOTTY;
    } else if (_IOC_NR(cmd) >= AXIDMA_NUM_IOCTLS) {
        axidma_err("IOCTL command is out of range for this device.\n");
        return -ENOTTY;
    }

    // Verify the input argument
    if ((_IOC_DIR(cmd) & _IOC_READ)) {
        if (!axidma_access_ok(arg_ptr, _IOC_SIZE(cmd), false)) {
            return -EFAULT;
        }
    } else if (_IOC_DIR(cmd) & _IOC_WRITE) {
        if (!axidma_access_ok(arg_ptr, _IOC_SIZE(cmd), true)) {
            return -EFAULT;
        }
    }

    // Get the axidma device from the file
    dev = file->private_data;

    // Perform the specified command
    switch (cmd) {
        case AXIDMA_GET_NUM_DMA_CHANNELS:
            axidma_get_num_channels(dev, &num_chans);
            if (copy_to_user(arg_ptr, &num_chans, sizeof(num_chans)) != 0) {
                axidma_err("Unable to copy channel info to userspace for "
                           "AXIDMA_GET_NUM_DMA_CHANNELS.\n");
                return -EFAULT;
            }
            rc = 0;
            break;

        case AXIDMA_GET_DMA_CHANNELS:
            if (copy_from_user(&usr_chans, arg_ptr, sizeof(usr_chans)) != 0) {
                axidma_err("Unable to copy channel buffer address from "
                           "userspace for AXIDMA_GET_DMA_CHANNELS.\n");
                return -EFAULT;
            }

            // Copy the channels array to userspace
            axidma_get_num_channels(dev, &num_chans);
            axidma_get_channel_info(dev, &kern_chans);
            size = num_chans.num_channels * sizeof(kern_chans.channels[0]);
            if (copy_to_user(usr_chans.channels, kern_chans.channels, size)) {
                axidma_err("Unable to copy channel ids to userspace for "
                           "AXIDMA_GET_DMA_CHANNELS.\n");
                return -EFAULT;
            }

            rc = 0;
            break;

        case AXIDMA_SET_DMA_SIGNAL:
            rc = axidma_set_signal(dev, arg);
            break;

        case AXIDMA_REGISTER_BUFFER:
            if (copy_from_user(&ext_buf, arg_ptr, sizeof(ext_buf)) != 0) {
                axidma_err("Unable to copy external buffer info from userspace "
                           "for AXIDMA_REGISTER_BUFFER.\n");
                return -EFAULT;
            }
            rc = axidma_get_external(dev, &ext_buf);
            break;

        case AXIDMA_DMA_READ:
            if (copy_from_user(&trans, arg_ptr, sizeof(trans)) != 0) {
                axidma_err("Unable to copy transfer info from userspace for "
                           "AXIDMA_DMA_READ.\n");
                return -EFAULT;
            }
            rc = axidma_read_transfer(dev, &trans);
            break;

        case AXIDMA_DMA_WRITE:
            if (copy_from_user(&trans, arg_ptr, sizeof(trans)) != 0) {
                axidma_err("Unable to copy transfer info from userspace for "
                           "AXIDMA_DMA_WRITE.\n");
                return -EFAULT;
            }
            rc = axidma_write_transfer(dev, &trans);
            break;

        case AXIDMA_DMA_READWRITE:
            if (copy_from_user(&inout_trans, arg_ptr,
                               sizeof(inout_trans)) != 0) {
                axidma_err("Unable to copy transfer info from userspace for "
                           "AXIDMA_DMA_READWRITE.\n");
                return -EFAULT;
            }
            rc = axidma_rw_transfer(dev, &inout_trans);
            break;

        case AXIDMA_DMA_VIDEO_READ:
            if (copy_from_user(&video_trans, arg_ptr,
                               sizeof(video_trans)) != 0) {
                axidma_err("Unable to copy transfer info from userspace for "
                           "AXIDMA_DMA_VIDEO_READ.\n");
                return -EFAULT;
            }

            // Allocate a kernel-space array for the frame buffers
            size = video_trans.num_frame_buffers *
                   sizeof(video_trans.frame_buffers[0]);
            video_trans.frame_buffers = kmalloc(size, GFP_KERNEL);
            if (video_trans.frame_buffers == NULL) {
                axidma_err("Unable to allocate array for the frame buffers.\n");
                return -ENOMEM;
            }

            // Copy the frame buffer array from user space to kernel space
            user_video_trans = (struct axidma_video_transaction *__user)arg_ptr;
            if (copy_from_user(video_trans.frame_buffers,
                        user_video_trans->frame_buffers, size) != 0) {
                axidma_err("Unable to copy the frame buffer array from "
                        "userspace for AXIDMA_VIDEO_READ.\n");
                kfree(video_trans.frame_buffers);
                return -EFAULT;
            }

            rc = axidma_video_transfer(dev, &video_trans, AXIDMA_READ);
            kfree(video_trans.frame_buffers);
            break;

        case AXIDMA_DMA_VIDEO_WRITE:
            if (copy_from_user(&video_trans, arg_ptr,
                               sizeof(video_trans)) != 0) {
                axidma_err("Unable to copy transfer info from userspace for "
                           "AXIDMA_VIDEO_WRITE.\n");
                return -EFAULT;
            }

            // Allocate a kernel-space array for the frame buffers
            size = video_trans.num_frame_buffers *
                   sizeof(video_trans.frame_buffers[0]);
            video_trans.frame_buffers = kmalloc(size, GFP_KERNEL);
            if (video_trans.frame_buffers == NULL) {
                axidma_err("Unable to allocate array for the frame buffers.\n");
                return -ENOMEM;
            }

            // Copy the frame buffer array from user space to kernel space
            user_video_trans = (struct axidma_video_transaction *__user)arg_ptr;
            if (copy_from_user(video_trans.frame_buffers,
                        user_video_trans->frame_buffers, size) != 0) {
                axidma_err("Unable to copy the frame buffer array from "
                        "userspace for AXIDMA_VIDEO_READ.\n");
                kfree(video_trans.frame_buffers);
                return -EFAULT;
            }

            rc = axidma_video_transfer(dev, &video_trans, AXIDMA_WRITE);
            kfree(video_trans.frame_buffers);
            break;

        case AXIDMA_STOP_DMA_CHANNEL:
            if (copy_from_user(&chan_info, arg_ptr, sizeof(chan_info)) != 0) {
                axidma_err("Unable to channel info from userspace for "
                           "AXIDMA_STOP_DMA_CHANNEL.\n");
            }
            rc = axidma_stop_channel(dev, &chan_info);
            break;

        case AXIDMA_UNREGISTER_BUFFER:
            rc = axidma_put_external(dev, (void *)arg);
            break;

        // Invalid command (already handled in preamble)
        default:
            return -ENOTTY;
    }

    return rc;
}

// The file operations for the AXI DMA device
static const struct file_operations axidma_fops = {
    .owner = THIS_MODULE,
    .open = axidma_open,
    .release = axidma_release,
    .mmap = axidma_mmap,
    .unlocked_ioctl = axidma_ioctl,
};

/*----------------------------------------------------------------------------
 * Initialization and Cleanup
 *----------------------------------------------------------------------------*/

int axidma_chrdev_init(struct axidma_device *dev)
{
    int rc;

    // Store a global pointer to the axidma device
    axidma_dev = dev;

    // Allocate a major and minor number region for the character device
    rc = alloc_chrdev_region(&dev->dev_num, dev->minor_num, dev->num_devices,
                             dev->chrdev_name);
    if (rc < 0) {
        axidma_err("Unable to allocate character device region.\n");
        goto ret;
    }

    // Create a device class for our device
    dev->dev_class = class_create(THIS_MODULE, dev->chrdev_name);
    if (IS_ERR(dev->dev_class)) {
        axidma_err("Unable to create a device class.\n");
        rc = PTR_ERR(dev->dev_class);
        goto free_chrdev_region;
    }

    /* Create a device for our module. This will create a file on the
     * filesystem, under "/dev/dev->chrdev_name". */
    dev->device = device_create(dev->dev_class, NULL, dev->dev_num, NULL,
                                dev->chrdev_name);
    if (IS_ERR(dev->device)) {
        axidma_err("Unable to create a device.\n");
        rc = PTR_ERR(dev->device);
        goto class_cleanup;
    }

    // Register our character device with the kernel
    cdev_init(&dev->chrdev, &axidma_fops);
    rc = cdev_add(&dev->chrdev, dev->dev_num, dev->num_devices);
    if (rc < 0) {
        axidma_err("Unable to add a character device.\n");
        goto device_cleanup;
    }

    // Initialize the list for DMA mmap'ed allocations
    INIT_LIST_HEAD(&dev->dmabuf_list);
    INIT_LIST_HEAD(&dev->external_dmabufs);

    return 0;

device_cleanup:
    device_destroy(dev->dev_class, dev->dev_num);
class_cleanup:
    class_destroy(dev->dev_class);
free_chrdev_region:
    unregister_chrdev_region(dev->dev_num, dev->num_devices);
ret:
    return rc;
}

void axidma_chrdev_exit(struct axidma_device *dev)
{
    // Cleanup all related character device structures
    cdev_del(&dev->chrdev);
    device_destroy(dev->dev_class, dev->dev_num);
    class_destroy(dev->dev_class);
    unregister_chrdev_region(dev->dev_num, dev->num_devices);

    return;
}
