/**
 * @file axidma_display_image.c
 * @date Friday, November 27, 2015 at 10:22:20 PM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This program displays the input image onto the display. The program
 * assumes that the PL fabric is programmed to have an AXI DMA module hooked
 * up to either a VGA or HDMI controller.
 *
 * The program reads the image into DMA memory, then repeatedly sends it out
 * over the PL fabric to display it on the device.
 *
 * @bug No known bugs.
 **/

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include <fcntl.h>              // Flags for open()
#include <sys/stat.h>           // Open() system call
#include <sys/types.h>          // Types for open()
#include <sys/mman.h>           // Mmap system call
#include <sys/ioctl.h>          // IOCTL system call
#include <unistd.h>             // Close() system call
#include <sys/time.h>           // Timing functions and definitions
#include <getopt.h>             // Option parsing
#include <errno.h>              // Error codes
#include <signal.h>             // Signal handling functions
#include <string.h>             // Memory copy/move functions

#include "axidma_ioctl.h"       // The AXI DMA IOCTL interface

// The default image size is 640x480
#define DEFAULT_IMAGE_WIDTH     640
#define DEFAULT_IMAGE_HEIGHT    480

/* Indicates if the program is still running. Used to communicate between the
 * signal handlers and the main thread. */
static volatile bool running = true;

void signal_handler(int signal)
{
    switch (signal) {
        case SIGINT:
        case SIGTERM:
        case SIGQUIT:
            running = false;
            break;

        default:
            break;
    }
}

static void print_usage(bool help)
{
    FILE *stream = (help) ? stdout : stderr;

    fprintf(stream, "Usage: axidma_display_image <image path> [-w <image "
            "width>] [-i <image height>].\n");

    if (!help) {
        return;
    }

    fprintf(stream, "\t<image path>:\t\tThe path to the image file to display. "
            "Can either be a relative or absolute path.\n");
    fprintf(stream, "\t-w <image width>:\tThe width of the image file. Default "
            "is %d.\n", DEFAULT_IMAGE_WIDTH);
    fprintf(stream, "\t-i <image height>:\tThe height of the image file. "
            "Default is %d.\n", DEFAULT_IMAGE_HEIGHT);
    fprintf(stream, "\t-t <DMA tx channel>:\tThe device id of the "
            "DMA channel to use for transmitting the image. Default is to use "
            "the lowest numbered channel available.\n");
    return;
}

// Parses the arg string as a double for the given option
static int parse_int(char option, char *arg_str, int *data)
{
    int rc;

    rc = sscanf(optarg, "%d", data);
    if (rc < 0) {
        perror("Unable to parse argument");
        return rc;
    } else if (rc != 1) {
        fprintf(stderr, "Error: Unable to parse argument '-%c %s' as an "
                "integer.\n", option, arg_str);
        print_usage(false);
        return -EINVAL;
    }

    return 0;
}

static int parse_args(int argc, char **argv, char **image_path,
                      int *image_width, int *image_height, int *tx_channel)
{
    int rc;
    int width, height, tx_channel_id;
    char option;

    // Check that there are enough command line arguments
    if (argc < 2) {
        fprintf(stderr, "Error: Too few command line arguments.\n");
        print_usage(false);
        return -EINVAL;
    }

    /* Set the default image width and height and the dummy value for
     * the transmit channel id. */
    *image_width = DEFAULT_IMAGE_WIDTH;
    *image_height = DEFAULT_IMAGE_HEIGHT;
    *tx_channel = -1;

    while ((option = getopt(argc, argv, "w:i:t:h")) != (char)-1)
    {
        switch (option)
        {
            // Parse the image width
            case 'w':
                rc = parse_int(option, optarg, &width);
                if (rc < 0) {
                    return rc;
                }
                *image_width = width;
                break;

            // Parse the image height
            case 'i':
                rc = parse_int(option, optarg, &height);
                if (rc < 0) {
                    return rc;
                }
                *image_height = height;
                break;

            // Parse the transmit channel device id
            case 't':
                rc = parse_int(option, optarg, &tx_channel_id);
                if (rc < 0) {
                    return rc;
                }
                *tx_channel = tx_channel_id;
                break;

            case 'h':
                print_usage(true);
                exit(0);

            default:
                print_usage(false);
                return -EINVAL;
        }
    }

    // Check if there are too many command line arguments remaining
    if (optind != argc-1) {
        fprintf(stderr, "Error: Too many command line arguments.\n");
        print_usage(false);
        return -EINVAL;
    }

    // Check that the image dimensions are non-zero
    if (*image_width <= 0) {
        fprintf(stderr, "Error: Image width must be positive.\n");
        print_usage(false);
        return -EINVAL;
    }
    if (*image_height <= 0) {
        fprintf(stderr, "Error: Image hieght must be positive.\n");
        print_usage(false);
        return -EINVAL;
    }

    // Parse out the image path
    *image_path = argv[optind];
    return 0;
}

static int find_dma_channel(int axidma_fd)
{
    int rc, i;
    struct axidma_chan chan;
    struct axidma_num_channels num_chan;
    struct axidma_channel_info channel_info;

    // Find the number of channels and allocate a buffer to hold their data
    rc = ioctl(axidma_fd, AXIDMA_GET_NUM_DMA_CHANNELS, &num_chan);
    if (rc < 0) {
        perror("Unable to get the number of DMA channels");
        return rc;
    } else if (num_chan.num_channels == 0) {
        fprintf(stderr, "No DMA channels are present.\n");
        return -ENODEV;
    }

    // Get the metdata about all the available channels
    channel_info.channels = malloc(num_chan.num_channels * sizeof(*channel_info.channels));
    if (channel_info.channels == NULL) {
        fprintf(stderr, "Unable to allocate channel information buffer.\n");
        return -ENOMEM;
    }
    rc = ioctl(axidma_fd, AXIDMA_GET_DMA_CHANNELS, &channel_info);
    if (rc < 0) {
        perror("Unable to get DMA channel information");
        free(channel_info.channels);
        return rc;
    }

    // Search for the first available transmit DMA channel
    for (i = 0; i < num_chan.num_channels; i++)
    {
        chan = channel_info.channels[i];
        if (chan.dir == AXIDMA_WRITE && chan.type == AXIDMA_DMA) {
            free(channel_info.channels);
            return chan.channel_id;
        }
    }

    free(channel_info.channels);
    fprintf(stderr, "No transmit DMA channels are present.\n");
    return -ENODEV;
}

int main(int argc, char **argv)
{
    int rc;
    char *image_path;
    int axidma_fd, image_fd;
    int tx_channel;
    int image_width, image_height, image_byte_size;
    char *image_buf;
    int bytes_remain;
    sigset_t sig_mask;
    struct stat image_stat;
    struct axidma_video_transaction trans;
    struct axidma_chan chan_info;

    // Allow for the user to interrupt the display
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGQUIT, signal_handler);

    // Parse out the image path and image size
    if (parse_args(argc, argv, &image_path, &image_width, &image_height,
                   &tx_channel) < 0) {
        rc = 1;
        goto ret;
    }
    image_byte_size = image_width * image_height * sizeof(int);

    // Try opening the image
    image_fd = open(image_path, O_RDONLY);
    if (image_fd < 0) {
        perror("Error opening image file");
        fprintf(stderr, "Image File: %s.\n", image_path);
        rc = 1;
        goto ret;
    }

    // Open the AXI dma device, initializing anything necessary
    axidma_fd = open("/dev/axidma", O_RDWR|O_EXCL);
    if (axidma_fd < 0) {
        perror("Error opening AXI DMA device");
        rc = 1;
        goto close_image;
    }

    /* If the user didn't specify the transmit channel, use the lowest numbered
     * one by default. */
    if (tx_channel == -1) {
        tx_channel = find_dma_channel(axidma_fd);
        if (tx_channel < 0) {
            rc = tx_channel;
            goto close_image;
        }
    }

    // Map two frame buffers to display the image
    image_buf = mmap(NULL, image_byte_size, PROT_READ|PROT_WRITE,
                      MAP_SHARED, axidma_fd, 0);
    if (image_buf == MAP_FAILED) {
        perror("Unable to mmap memory region from AXI DMA device");
        rc = 1;
        goto close_axidma;
    }

    // Check the file size of the image
    rc = fstat(image_fd, &image_stat);
    if (rc < 0) {
        perror("Unable to get file statistics");
        rc = 1;
        goto free_image_buf;
    } else if (image_stat.st_size < (off_t)image_byte_size) {
        fprintf(stderr, "Error: File is not large enough for a %dx%d image.\n",
                image_width, image_height);
        rc = 1;
        goto free_image_buf;
    } else if (image_stat.st_size > (off_t)image_byte_size) {
        printf("Warning: File is too large for a %dx%d image. It will be "
               "truncated.\n", image_width, image_height);
    }

    // Read the image into the buffer (accounting for EINTR's)
    bytes_remain = image_byte_size;
    do {
        rc = read(image_fd, image_buf+image_byte_size-bytes_remain,
                  bytes_remain);
        bytes_remain = (rc > 0) ? bytes_remain - rc : bytes_remain;
    } while ((rc > 0 || rc == -EINTR) && bytes_remain > 0);

    // Check for errors
    if (rc < 0) {
        perror("Unable to read image file");
        rc = -1;
        goto free_image_buf;
    }

    // Initiate a video transfer to the PL fabric
    trans.channel_id = tx_channel;
    trans.num_frame_buffers = 1;
    trans.frame_buffers = (void **)&image_buf;
    trans.frame.width = image_width;
    trans.frame.height = image_height;
    trans.frame.depth = sizeof(int);
    if (ioctl(axidma_fd, AXIDMA_DMA_VIDEO_WRITE, &trans) < 0) {
        perror("Failed to perform a DMA video write transaction");
        rc = -1;
        goto free_image_buf;
    }
    printf("Image display beginning.\n");

    // Wait until the user interrupts us
    sigemptyset(&sig_mask);
    while (running)
    {
        sigsuspend(&sig_mask);
    }
    printf("Display shutting down.\n");

    // Stop the DMA transfer
    chan_info.channel_id = tx_channel;
    chan_info.dir = AXIDMA_WRITE;
    chan_info.type = AXIDMA_DMA;
    if (ioctl(axidma_fd, AXIDMA_STOP_DMA_CHANNEL, &chan_info) < 0) {
        perror("Unable to stop DMA transmit transfer");
        rc = -1;
        goto free_image_buf;
    }

    rc = 0;

free_image_buf:
    munmap(image_buf, image_byte_size);
close_axidma:
    close(axidma_fd);
close_image:
    close(image_fd);
ret:
    return rc;
}
