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
 * Command-line Interface
 *----------------------------------------------------------------------------*/

// Prints the usage for this program
static void print_usage(bool help)
{
    FILE* stream = (help) ? stdout : stderr;

    fprintf(stream, "Usage: test_axidma -s hello_world!\n");
    if (!help) {
        return;
    }
    return;
}

static int parse_args(int argc, char **argv, int *tx_channel, int *rx_channel,
        char **send_buf, size_t *tx_size, size_t *rx_size)
{
    double double_arg;
    int int_arg;
    char option;

    // Set the default data size and number of transfers
    *tx_channel = -1;
    *rx_channel = -1;
    *tx_size    = 0;
    *rx_size    = 0;
    *send_buf   = NULL;

    while ((option = getopt(argc, argv, "vt:r:i:b:f:o:s:g:n:h")) != (char)-1)
    {
        switch (option)
        {
            case 's':
                *tx_size = strlen(optarg) + 1;
                *send_buf = (char*)malloc(*tx_size);
                memcpy(*send_buf, optarg, *tx_size);
                *rx_size = *tx_size;
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

    if ((*tx_size == 0) ||
        (*rx_size == 0)) {
        fprintf(stderr, "Error: tx_size == 0 or rx_size == 0\n");
        return -EINVAL;
    }

    return 0;
}

static int single_transfer_test(axidma_dev_t dev, int tx_channel, void *tx_buf,
        int tx_size, int rx_channel, void *rx_buf, int rx_size, char* send_buf)
{
    struct axidma_video_frame *tx_frame = NULL;
    struct axidma_video_frame *rx_frame = NULL;
    int rc;
    {
        memcpy(tx_buf, send_buf, tx_size);
    }

    // Perform the DMA transaction
    rc = axidma_twoway_transfer(dev, tx_channel, tx_buf, tx_size, tx_frame,
            rx_channel, rx_buf, rx_size, rx_frame, true);
    if (rc < 0) {
        return rc;
    }

    {
        char *tmp_buf = (char*)rx_buf;
        for(int i=0;i<tx_size;i++) {
            printf("rx_buf[%d] = %c \n", i, tmp_buf[i]);
        }
        printf("recive data : %s \n", tmp_buf);
    }

    return 0;
}

/*----------------------------------------------------------------------------
 * Main Function
 *----------------------------------------------------------------------------*/

int main(int argc, char **argv)
{
    int rc;
    int tx_channel, rx_channel;
    size_t tx_size, rx_size;
    char *tx_buf, *rx_buf, *send_buf;
    axidma_dev_t axidma_dev;
    const array_t *tx_chans, *rx_chans;

    // Check if the user overrided the default transfer size and number
    if (parse_args(argc, argv, &tx_channel, 
            &rx_channel, &send_buf, &tx_size, &rx_size) < 0) {
        rc = 1;
        goto ret;
    }
    printf("AXI DMA Benchmark Parameters:\n");

    printf("\tTransmit Buffer Size: %ld B\n", tx_size);
    printf("\tReceive Buffer Size: %ld B\n", rx_size);

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
    tx_chans = axidma_get_dma_tx(axidma_dev);
    rx_chans = axidma_get_dma_rx(axidma_dev);

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
            rx_channel, rx_buf, rx_size, send_buf);
    if (rc < 0) {
        goto free_rx_buf;
    }
    printf("Single transfer test successfully completed!\n");

free_rx_buf:
    axidma_free(axidma_dev, rx_buf, rx_size);
free_tx_buf:
    axidma_free(axidma_dev, tx_buf, tx_size);
destroy_axidma:
    axidma_destroy(axidma_dev);
ret:
    return rc;
}
