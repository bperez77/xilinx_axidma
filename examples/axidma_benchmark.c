/**
 * @file axidma_benchmark.c
 * @date Thursday, November 26, 2015 at 10:29:26 PM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This is a simple program that benchmarks the AXI DMA transfer rate for
 * whatever logic is sitting on the PL fabric. It sends some data out over the
 * PL fabric via AXI DMA to whatever is sitting there, and waits to receive
 * some data back from the PL fabric.
 *
 * The program first runs a single transfer to verify that the DMA works
 * properly, then profiles the DMA engine. The program sends out a specific
 * transfer size, and gets back a potentially different receive size. It runs
 * the a given number of times to calculate the performance statistics. All of
 * these options are configurable from the command line.
 *
 * NOTE: This program assumes that there are only two DMA channels being used by
 * the PL fabric, one that consumes data and sends it to the PL fabric logic,
 * and another that sends the output of the PL fabric back to memory. If you
 * have additional DMA channels, you will need to modify the program. This
 * program will work with the AXI DMA/VDMA loopback examples (where the S2MM and
 * MM2S ports are simply connected to one another).
 *
 * @bug No known bugs.
 **/

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>             // Strlen function

#include <fcntl.h>              // Flags for open()
#include <sys/stat.h>           // Open() system call
#include <sys/types.h>          // Types for open()
#include <sys/mman.h>           // Mmap system call
#include <sys/ioctl.h>          // IOCTL system call
#include <unistd.h>             // Close() system call
#include <sys/time.h>           // Timing functions and definitions
#include <getopt.h>             // Option parsing
#include <errno.h>              // Error codes

#include "libaxidma.h"          // Interface to the AXI DMA
#include "util.h"               // Miscellaneous utilities
#include "conversion.h"         // Miscellaneous conversion utilities

/*----------------------------------------------------------------------------
 * Internal Definitons
 *----------------------------------------------------------------------------*/

// The size of data to send per transfer (1080p image, 7.24 MiB)
#define IMAGE_SIZE                  (1920 * 1080)
#define DEFAULT_TRANSFER_SIZE       ((int)(IMAGE_SIZE * sizeof(int)))

// The default number of transfers to benchmark
#define DEFAULT_NUM_TRANSFERS       1000

// The pattern that we fill into the buffers
#define TEST_PATTERN(i) ((int)(0x1234ACDE ^ (i)))

// The DMA context passed to the helper thread, who handles remainder channels

/*----------------------------------------------------------------------------
 * Command-line Interface
 *----------------------------------------------------------------------------*/

// Prints the usage for this program
static void print_usage(bool help)
{
    FILE* stream = (help) ? stdout : stderr;
    double default_size;

    fprintf(stream, "Usage: axidma_benchmark [-v] [-t <(V)DMA tx channel>] "
            "[-r <(V)DMA rx channel>] [-i <Tx transfer size (MiB)>] "
            "[-b <Tx transfer size (bytes)>] [-f <Tx frame size (HxWxD)>] "
            "[-o <Rx transfer size (MiB)>] [-s <Rx transfer size (bytes)>] "
            "[-g <Rx frame size (HxWxD)>] [-n <number transfers>]\n");
    if (!help) {
        return;
    }

    default_size = BYTE_TO_MIB(DEFAULT_TRANSFER_SIZE);
    fprintf(stream, "\t-v:\t\t\t\tUse the AXI VDMA channels instead of AXI DMA "
            "ones for the transfer.\n");
    fprintf(stream, "\t-t <DMA tx channel>:\t\t\tThe device id of the DMA "
            "channel to use for transmitting the data to the PL fabric.\n");
    fprintf(stream, "\t-r <DMA rx channel>:\t\t\tThe device id of the DMA "
            "channel to use for receiving the the data from the PL fabric.\n");
    fprintf(stream, "\t-i <transmit transfer size (MiB)>:\tThe size of the "
            "data transmit over the DMA on each transfer. Default is %0.2f "
            "MiB.\n", default_size);
    fprintf(stream, "\t-b <Tx transfer size (bytes)>:\tThe size of the "
            "data transmit over the DMA on each transfer. Default is %d "
            "bytes.\n", DEFAULT_TRANSFER_SIZE);
    fprintf(stream, "\t-f <Tx frame size (height x width x depth)>:\tThe size "
            "of the frame to transmit over VDMA on each transfer, where the "
            "depth is in bytes.");
    fprintf(stream, "\t-o <Rx transfer size (MiB)>:\tThe size of the data "
            "to receive from the DMA on each transfer. Default is %0.2f MiB.\n",
            default_size);
    fprintf(stream, "\t-s <Rx transfer size (bytes)>:\tThe size of the "
            "data to receive from the DMA on each transfer. Default is %d "
            "bytes.\n", DEFAULT_TRANSFER_SIZE);
    fprintf(stream, "\t-g <Rx frame size (height x width x depth)>:\tThe size "
            "of the frame to receive over VDMA on each transfer, where the "
            "depth is in bytes.");
    fprintf(stream, "\t-n <number transfers>:\t\t\tThe number of DMA transfers "
            "to perform to do the benchmark. Default is %d transfers.\n",
            DEFAULT_NUM_TRANSFERS);
    return;
}

/* Parses the command line arguments overriding the default transfer sizes,
 * and number of transfer to use for the benchmark if specified. */
static int parse_args(int argc, char **argv, int *tx_channel, int *rx_channel,
        size_t *tx_size, struct axidma_video_frame *tx_frame, size_t *rx_size,
        struct axidma_video_frame *rx_frame, int *num_transfers, bool *use_vdma)
{
    double double_arg;
    int int_arg;
    char option;
    bool tx_frame_specified, rx_frame_specified;

    // Set the default data size and number of transfers
    *use_vdma = false;
    *tx_channel = -1;
    *rx_channel = -1;
    *tx_size = DEFAULT_TRANSFER_SIZE;
    tx_frame->height = -1;
    tx_frame->width = -1;
    tx_frame->depth = -1;
    *rx_size = DEFAULT_TRANSFER_SIZE;
    rx_frame->height = -1;
    rx_frame->width = -1;
    rx_frame->depth = -1;
    *num_transfers = DEFAULT_NUM_TRANSFERS;

    while ((option = getopt(argc, argv, "vt:r:i:b:f:o:s:g:n:h")) != (char)-1)
    {
        switch (option)
        {
            case 'v':
                *use_vdma = true;
                break;

            // Parse the transmit channel argument
            case 't':
                if (parse_int(option, optarg, &int_arg) < 0) {
                    print_usage(false);
                    return -EINVAL;
                }
                *tx_channel = int_arg;
                break;

            // Parse the transmit transfer size argument
            case 'r':
                if (parse_int(option, optarg, &int_arg) < 0) {
                    print_usage(false);
                    return -EINVAL;
                }
                *rx_channel = int_arg;
                break;

            // Parse the transmit transfer size argument
            case 'i':
                if (parse_double(option, optarg, &double_arg) < 0) {
                    print_usage(false);
                    return -EINVAL;
                }
                *tx_size = MIB_TO_BYTE(double_arg);
                break;

            // Parse the transmit transfer size argument
            case 'b':
                if (parse_int(option, optarg, &int_arg) < 0) {
                    print_usage(false);
                    return -EINVAL;
                }
                *tx_size = int_arg;
                break;

            // Parse the transmit frame size option
            case 'f':
                if (strlen(optarg) == 0) {
                    fprintf(stderr, "The -f option requires an argument.\n");
                    print_usage(false);
                    return -EINVAL;
                } else if (parse_resolution(option, optarg, &tx_frame->height,
                        &tx_frame->width, &tx_frame->depth) < 0) {
                    print_usage(false);
                    return -EINVAL;
                }
                *tx_size = tx_frame->height * tx_frame->width * tx_frame->depth;
                tx_frame_specified = true;

                break;

            // Parse the receive transfer size argument
            case 'o':
                if (parse_double(option, optarg, &double_arg) < 0) {
                    print_usage(false);
                    return -EINVAL;
                }
                *rx_size = MIB_TO_BYTE(double_arg);
                break;

            // Parse the receive transfer size argument
            case 's':
                if (parse_int(option, optarg, &int_arg) < 0) {
                    print_usage(false);
                    return -EINVAL;
                }
                *rx_size = int_arg;
                break;

            // Parse the receive frame size option
            case 'g':
                if (strlen(optarg) == 0) {
                    fprintf(stderr, "The -g option requires an argument.\n");
                    print_usage(false);
                    return -EINVAL;
                } else if (parse_resolution(option, optarg, &rx_frame->height,
                        &rx_frame->width, &rx_frame->depth) < 0) {
                    print_usage(false);
                    return -EINVAL;
                }
                *rx_size = rx_frame->height * rx_frame->width * rx_frame->depth;
                rx_frame_specified = true;

                break;

            // Parse the number of transfers argument
            case 'n':
                if (parse_int(option, optarg, &int_arg) < 0) {
                    print_usage(false);
                    return -EINVAL;
                }
                *num_transfers = int_arg;
                break;

            // Print detailed usage message
            case 'h':
                print_usage(true);
                exit(0);

            default:
                print_usage(false);
                return -EINVAL;
        }
    }

    if ((*tx_channel == -1) ^ (*rx_channel == -1)) {
        fprintf(stderr, "Error: If one of -r/-t is specified, then both must "
                "be.\n");
        return -EINVAL;
    }

    if ((*tx_size == DEFAULT_TRANSFER_SIZE) ^
        (*rx_size == DEFAULT_TRANSFER_SIZE)) {
        fprintf(stderr, "Error: If one of -i/-b or -o/-s is specified, then "
                "both most be.\n");
        return -EINVAL;
    }

    if (*use_vdma && (!tx_frame_specified || !rx_frame_specified)) {
        fprintf(stderr, "Error: If -v is specified, then both -f and -g must "
                "also be specified.\n");
        return -EINVAL;
    }

    return 0;
}

/*----------------------------------------------------------------------------
 * Verification Test
 *----------------------------------------------------------------------------*/

/* Initialize the two buffers, filling buffers with a preset but "random"
 * pattern. */
static void init_data(char *tx_buf, char *rx_buf, size_t tx_buf_size,
                      size_t rx_buf_size)
{
    size_t i;
    int *transmit_buffer, *receive_buffer;

    transmit_buffer = (int *)tx_buf;
    receive_buffer = (int *)rx_buf;

    // Fill the buffer with integer patterns
    for (i = 0; i < tx_buf_size / sizeof(int); i++)
    {
        transmit_buffer[i] = TEST_PATTERN(i);
    }

    // Fill in any leftover bytes if it's not aligned
    for (i = 0; i < tx_buf_size % sizeof(int); i++)
    {
        tx_buf[i] = TEST_PATTERN(i + tx_buf_size / sizeof(int));
    }

    // Fill the buffer with integer patterns
    for (i = 0; i < rx_buf_size / sizeof(int); i++)
    {
        receive_buffer[i] = TEST_PATTERN(i + tx_buf_size);
    }

    // Fill in any leftover bytes if it's not aligned
    for (i = 0; i < rx_buf_size % sizeof(int); i++)
    {
        rx_buf[i] = TEST_PATTERN(i + tx_buf_size + rx_buf_size / sizeof(int));
    }

    return;
}

/* Verify the two buffers. For transmit, verify that it is unchanged. For
 * receive, we don't know the PL fabric function, so the best we can
 * do is check if it changed and warn the user if it is not. */
static int verify_data(char *tx_buf, char *rx_buf, size_t tx_buf_size,
                       size_t rx_buf_size)
{
    int *transmit_buffer, *receive_buffer;
    size_t i, rx_data_same, rx_data_units;
    double match_fraction;

    transmit_buffer = (int *)tx_buf;
    receive_buffer = (int *)rx_buf;

    // Verify words in the transmit buffer
    for (i = 0; i < tx_buf_size / sizeof(int); i++)
    {
        if (transmit_buffer[i] != TEST_PATTERN(i)) {
            fprintf(stderr, "Test failed! The transmit buffer was overwritten "
                    "at byte %zu.\n", i);
            fprintf(stderr, "Expected 0x%08x, found 0x%08x.\n", TEST_PATTERN(i),
                    tx_buf[i]);
            return -EINVAL;
        }
    }

    // Verify any leftover bytes in the buffer
    for (i = 0; i < tx_buf_size % sizeof(int); i++)
    {
        if (tx_buf[i] != TEST_PATTERN(i + tx_buf_size / sizeof(int))) {
            fprintf(stderr, "Test failed! The transmit buffer was overwritten "
                    "at byte %zu.\n", i);
            fprintf(stderr, "Expected 0x%08x, found 0x%08x.\n", TEST_PATTERN(i),
                    tx_buf[i]);
            return -EINVAL;
        }
    }

    // Verify words in the receive buffer
    rx_data_same = 0;
    for (i = 0; i < rx_buf_size / sizeof(int); i++)
    {
        if (receive_buffer[i] == TEST_PATTERN(i+tx_buf_size)) {
            rx_data_same += 1;
        }
    }

    // Verify any leftover bytes in the buffer
    for (i = 0; i < rx_buf_size % sizeof(int); i++)
    {
        if (rx_buf[i] == TEST_PATTERN(i+tx_buf_size+rx_buf_size/sizeof(int))) {
            rx_data_same += 1;
        }
    }

    // Warn the user if more than 10% of the pixels match the test pattern
    rx_data_units = rx_buf_size / sizeof(int) + rx_buf_size % sizeof(int);
    if (rx_data_same == rx_data_units) {
        fprintf(stderr, "Test Failed! The receive buffer was not updated.\n");
        return -EINVAL;
    } else if (rx_data_same >= rx_data_units / 10) {
        match_fraction = ((double)rx_data_same) / ((double)rx_data_units);
        printf("Warning: %0.2f%% of the receive buffer matches the "
               "initialization pattern.\n", match_fraction * 100.0);
        printf("This may mean that the receive buffer was not properly "
               "updated.\n");
    }

    return 0;
}

static int single_transfer_test(axidma_dev_t dev, int tx_channel, void *tx_buf,
        int tx_size, struct axidma_video_frame *tx_frame, int rx_channel,
        void *rx_buf, int rx_size, struct axidma_video_frame *rx_frame)
{
    int rc;

    // Initialize the buffer region we're going to transmit
    init_data(tx_buf, rx_buf, tx_size, rx_size);

    // Perform the DMA transaction
    rc = axidma_twoway_transfer(dev, tx_channel, tx_buf, tx_size, tx_frame,
            rx_channel, rx_buf, rx_size, rx_frame, true);
    if (rc < 0) {
        return rc;
    }

    // Verify that the data in the buffer changed
    return verify_data(tx_buf, rx_buf, tx_size, rx_size);
}

/*----------------------------------------------------------------------------
 * Benchmarking Test
 *----------------------------------------------------------------------------*/

/* Profiles the transfer and receive rates for the DMA, reporting the throughput
 * of each channel in MiB/s. */
static int time_dma(axidma_dev_t dev, int tx_channel, void *tx_buf, int tx_size,
        struct axidma_video_frame *tx_frame, int rx_channel, void *rx_buf,
        int rx_size, struct axidma_video_frame *rx_frame, int num_transfers)
{
    int i, rc;
    struct timeval start_time, end_time;
    double elapsed_time, tx_data_rate, rx_data_rate;

    // Begin timing
    gettimeofday(&start_time, NULL);

    // Perform n transfers
    for (i = 0; i < num_transfers; i++)
    {
        rc = axidma_twoway_transfer(dev, tx_channel, tx_buf, tx_size, tx_frame,
                rx_channel, rx_buf, rx_size, rx_frame, true);
        if (rc < 0) {
            fprintf(stderr, "DMA failed on transfer %d, not reporting timing "
                    "results.\n", i+1);
            return rc;
        }
    }

    // End timing
    gettimeofday(&end_time, NULL);

    // Compute the throughput of each channel
    elapsed_time = TVAL_TO_SEC(end_time) - TVAL_TO_SEC(start_time);
    tx_data_rate = BYTE_TO_MIB(tx_size) * num_transfers / elapsed_time;
    rx_data_rate = BYTE_TO_MIB(rx_size) * num_transfers / elapsed_time;

    // Report the statistics to the user
    printf("DMA Timing Statistics:\n");
    printf("\tElapsed Time: %0.2f s\n", elapsed_time);
    printf("\tTransmit Throughput: %0.2f MiB/s\n", tx_data_rate);
    printf("\tReceive Throughput: %0.2f MiB/s\n", rx_data_rate);
    printf("\tTotal Throughput: %0.2f MiB/s\n", tx_data_rate + rx_data_rate);

    return 0;
}

/*----------------------------------------------------------------------------
 * Main Function
 *----------------------------------------------------------------------------*/

int main(int argc, char **argv)
{
    int rc;
    int num_transfers;
    int tx_channel, rx_channel;
    size_t tx_size, rx_size;
    bool use_vdma;
    char *tx_buf, *rx_buf;
    axidma_dev_t axidma_dev;
    const array_t *tx_chans, *rx_chans;
    struct axidma_video_frame transmit_frame, *tx_frame, receive_frame, *rx_frame;

    printf("enter main");

    // Check if the user overrided the default transfer size and number
    if (parse_args(argc, argv, &tx_channel, &rx_channel, &tx_size,
            &transmit_frame, &rx_size, &receive_frame, &num_transfers,
            &use_vdma) < 0) {
        rc = 1;
        goto ret;
    }
    printf("AXI DMA Benchmark Parameters:\n");
    if (!use_vdma) {
        printf("\tTransmit Buffer Size: %0.2f MiB\n", BYTE_TO_MIB(tx_size));
        printf("\tReceive Buffer Size: %0.2f MiB\n", BYTE_TO_MIB(rx_size));
    } else {
        printf("\tTransmit Buffer Size: %dx%dx%d (%0.2f MiB)\n",
                transmit_frame.height, transmit_frame.width, transmit_frame.depth,
                BYTE_TO_MIB(tx_size));
        printf("\tReceive Buffer Size: %dx%dx%d (%0.2f MiB)\n",
                receive_frame.height, receive_frame.width, receive_frame.depth,
                BYTE_TO_MIB(rx_size));
    }
    printf("\tNumber of DMA Transfers: %d transfers\n\n", num_transfers);

    // Initialize the AXI DMA device
    axidma_dev = axidma_init();
    if (axidma_dev == NULL) {
        fprintf(stderr, "Failed to initialize the AXI DMA device.\n");
        rc = 1;
        goto ret;
    }

    // Map memory regions for the transmit and receive buffers
    tx_buf = axidma_malloc(axidma_dev, tx_size);
    if (tx_buf == NULL) {
        perror("Unable to allocate transmit buffer from the AXI DMA device.");
        rc = -1;
        goto destroy_axidma;
    }
    rx_buf = axidma_malloc(axidma_dev, rx_size);
    if (rx_buf == NULL) {
        perror("Unable to allocate receive buffer from the AXI DMA device");
        rc = -1;
        goto free_tx_buf;
    }

    // Get all the transmit and receive channels
    if (use_vdma) {
        tx_chans = axidma_get_vdma_tx(axidma_dev);
        rx_chans = axidma_get_vdma_rx(axidma_dev);
        tx_frame = &transmit_frame;
        rx_frame = &receive_frame;
    } else {
        tx_chans = axidma_get_dma_tx(axidma_dev);
        rx_chans = axidma_get_dma_rx(axidma_dev);
        tx_frame = NULL;
        rx_frame = NULL;
    }
    if (tx_chans->len < 1) {
        fprintf(stderr, "Error: No transmit channels were found.\n");
        rc = -ENODEV;
        goto free_rx_buf;
    }
    if (rx_chans->len < 1) {
        fprintf(stderr, "Error: No receive channels were found.\n");
        rc = -ENODEV;
        goto free_rx_buf;
    }

    /* If the user didn't specify the channels, we assume that the transmit and
     * receive channels are the lowest numbered ones. */
    if (tx_channel == -1 && rx_channel == -1) {
        tx_channel = tx_chans->data[0];
        rx_channel = rx_chans->data[0];
    }
    printf("Using transmit channel %d and receive channel %d.\n", tx_channel,
           rx_channel);

    // Transmit the buffer to DMA a single time
    rc = single_transfer_test(axidma_dev, tx_channel, tx_buf, tx_size,
            tx_frame, rx_channel, rx_buf, rx_size, rx_frame);
    if (rc < 0) {
        goto free_rx_buf;
    }
    printf("Single transfer test successfully completed!\n");

    // Time the DMA eingine
    printf("Beginning performance analysis of the DMA engine.\n\n");
    rc = time_dma(axidma_dev, tx_channel, tx_buf, tx_size, tx_frame,
            rx_channel, rx_buf, rx_size, rx_frame, num_transfers);

free_rx_buf:
    axidma_free(axidma_dev, rx_buf, rx_size);
free_tx_buf:
    axidma_free(axidma_dev, tx_buf, tx_size);
destroy_axidma:
    axidma_destroy(axidma_dev);
ret:
    return rc;
}
