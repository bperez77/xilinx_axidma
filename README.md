# Xilinx AXI DMA Driver and Library (Quick Start Guide)

[![Build Status](https://travis-ci.org/bperez77/xilinx_axidma.svg?branch=master)](https://travis-ci.org/bperez77/xilinx_axidma)

## Overview

A zero-copy, high-bandwidth Linux driver and userspace interface library for Xilinx's AXI DMA and VDMA IP blocks. The purpose of this software stack is to allow userspace Linux applications to interact with hardware on the FPGA fabric. The driver and userspace library act as a generic layer between the procesor and FPGA, and abstracts away the details of setting up DMA transactions. The pupose of AXI DMA and VDMA IP blocks is to serve as bridges for communication between the processing system and the FPGA, through one of the DMA ports on the Zynq processing system.

The driver enables userspace application to allocate zero-copy, physically contiguous DMA buffers for transfers, allowing for high bandwidth communication between the FPGA and ARM core. The driver exposes its functionality via a character device, which the library interacts with.

This driver supports 4.x version Xilinx kernels. It has been tested with the mainline Xilinx kernel, and the Analog Devices' kernel on the Zedboard. The driver should work with any 4.x kernel and any board that uses a Zynq-7000 series processing system.

## Fork with Multi-Process Support

At the moment, the driver only supports a single process accessing the AXI DMA/VDMA IP engines. There is a fork of the repository that has support for multiple processes accessing independent DMA engines by [@corna](https://github.com/corna). You can find that fork [here](https://github.com/corna/xilinx_axidma). Independent DMA engines can be opened by a single process; however, each engine still can only be used by one process at a time. Thus, two processes can not share the TX and RX channels of an engine.

In the future, the driver will have proper synchronization for both multiple processes and multiple threads. In the meantime, if you need multi-process support, the fork is a good workaround.

## Features

1. Zero-copy transmit (processor to FPGA), receive (FPGA to processor), and two-way combined DMA transfers.
2. Support for transfers with Xilinx's AXI DMA and AXI VDMA (video DMA) IP blocks.
3. Allocation of DMA buffers that are contiguous in physical memory, allowing for high-bandwidth DMA transfers, through the kernel's contiguous memory allocator (CMA).
4. Allocation of memory that is coherent between the FPGA and processor, by disabling caching for those pages in the DMA buffer.
4. Synchronous and asynchronous modes for transfers.
5. Registration of callback functions that are called when an asynchronous transfer completes.
6. Delivery of a POSIX real-time signal upon completion of an asynchronous transfer.
7. Support for DMA buffer sharing, or external DMA buffers. Currently the driver can only import a DMA buffer from another driver. This is useful, for example, when transfers need to be done with a frame buffer allocated by a DRM driver.

## Setting Up the Driver

### Linux Kernel

The driver depends on the contiguous memory allocator (CMA), Xilinx's DMA and VDMA driver, and DMA buffer sharing. These must be enabled in the kernel the driver is complied against. These should be enabled by default in any Xilinx Linux kenrel fork. To be sure, make sure to double check the kernel configuration by running `make menuconfig` or by opening the `.config` file at the top level of your kernel source tree. The following options should be enabled:
```bash
CONFIG_CMA=y
CONFIG_DMA_CMA=y
CONFIG_XILINX_DMAENGINES=y
CONFIG_XILINX_AXIDMA=y
CONFIG_XILINX_AXIVDMA=y
CONFIG_DMA_SHARED_BUFFER=y
```

### Device Tree

The driver requires a node in the device tree. This node describes the DMA channels that the driver has exclusive access. It is also used to probe the driver, so the driver is activited only when this node is present. The node has the following properties:
* `compatible` - This must be the string "xlnx,axidma-chrdev". This is used to match the driver with the device tree node.
* `dmas` - A list of phandles (references to other device tree nodes) of Xilinx AXI DMA or VDMA device tree nodes, followed by either 0 or 1. This refers to the child node inside of the Xilinx AXI DMA/VDMA device tree node, 0 of course being the first child node.
* `dma-names` - A list of names for the DMA channels. The names can be completely arbitrary, but they must be unique. This is required by the DMA interface function `dma_request_slave_channel()`, but is otherwise unused by the driver. In the future, the driver will use the names in printed messages.
* `index` - A non-negative integer which represents the index of this DMA device, optional if only one DMA device is present. If unset, defaults to 0.  The indexes are not required to be consecutive, but they must be unique. The corresponding character device will appear as `/dev/axidma` when index is 0 or `/dev/axidma<index>` otherwise.

For the Xilinx AXI DMA/VDMA device tree nodes, the only requirement is that the `device-id` property is unique, but they can be completely arbitrary. This is how the channels are referred to in both the driver and from userspace. For more information on creating AXI DMA/VDMA device tree nodes, consult the kernel [documentation](https://github.com/Xilinx/linux-xlnx/blob/master/Documentation/devicetree/bindings/dma/xilinx/xilinx_dma.txt) for them.

Here is a simple example of the device tree nodes for a system with a single AXI DMA IP, with both a transmit and receive channel. Note you will need to adjust this for your kernel tree and setup:
```
axidma_chrdev: axidma_chrdev@0 {
    compatible = "xlnx,axidma-chrdev";
    dmas = <&axi_dma_0 0 &axi_dma_0 1>;
    dma-names = "tx_channel", "rx_channel";
};

axi_dma_0: axidma0@40400000 {
    #dma-cells = <1>;
    compatible = "xlnx,axi-dma", "xlnx,axi-dma-6.03.a", "xlnx,axi-dma-1.00.a";
    reg = <0x40400000 0x10000>;
    clocks = <&clkc 15>, <&clkc 15>, <&clkc 15>, <&clkc 15>;
    clock-names = "s_axi_lite_aclk", "m_axi_sg_aclk", "m_axi_mm2s_aclk", "m_axi_s2mm_aclk";    
    xlnx,include-sg;    
    xlnx,addrwidth = <32>;

    dma-mm2s-channel@40400000 {
        compatible = "xlnx,axi-dma-mm2s-channel";
        dma-channels = <1>;
        xlnx,datawidth = <64>;
        xlnx,device-id = <0>;
        interrupt-parent = <&intc>;
        interrupts = <0 29 4>;
    };
    
    dma-s2mm-channel@40400000 {
        compatible = "xlnx,axi-dma-s2mm-channel";
        dma-channels = <1>;
        xlnx,datawidth = <64>;
        xlnx,device-id = <1>;
        interrupt-parent = <&intc>;
        interrupts = <0 30 4>;
    };
};
```

### Kernel Command Line

The contiguous memory allocator works by reserving a pool of memory for contiguous memory allocations that it uses when requested. By default this size is too small for typical uses with this driver. The size can be changed by appending `cma=<size>M` to the kernel command line arguments. This sets the pool's size to `size` MBs.

The kernel command line can be updated by changing the device tree or from the U-Boot console. For example, to set the CMA's pool size to 25 MB from the U-Boot console:
```
setenv bootargs "${bootargs} cma=25M"
```

**NOTE:** In the future, specifying the CMA region size will be moved into the device tree, so this will not be necessary.

## Compilation

### Makefile Variables

The Makefile supports both native and cross compilation, and is repsonsible for compiling the driver and a few example programs. It has the following variables:
* `CROSS_COMPILE` - The prefix for the compiler to use for cross-compilation.
* `ARCH` - The architecture being compiled for. Required for compiling the driver when cross-compiling.
* `KBUILD_DIR` - The path to the kernel source tree to compile the driver against. The kernel must already be built. Required for compiling the driver.
* `OUTPUT_DIR` - The path to the output directory to place the generated files in. Defaults to `outputs` in the top-level directory.
* `XILINX_DMA_INCLUDE_PATH_FIXUP` - This specifies to fixup the issue with the location of the `xilinx_dma.h` header file in the kernel being compiled against. Specify this if you see an include error when compiling the driver.

For a complete list of targets, and a more complete description of the options, run:
```bash
make help
```

The Makefile also supports specifying these variables in a configuration file. The repository distirbutes a template for this file at `config_template.mk`. To use this file, copy it to `config.mk`, and uncomment and fill in the varaibles as needed.

### Compiling the Driver

The Makefile supports out-of-tree compilation for the driver. To compile the driver, you must specify the path to a kernel source tree that is already built. Also, you can specify any cross-compilation options as necessary. For example, to compile the driver for ARM:
```bash
make CROSS_COMPILE=arm-linux-gnueabihf- ARCH=arm KBUILD_DIR=</path/to/kernel/tree> driver
```

This will generate a kernel object for the driver at `outputs/axidma.ko`.

### Compiling the Examples

The driver and library come with several example programs that show how to use the API. There's a program that benchmarks a two-way transfer, one that transmits a file over a channel, and one that displays an image (assuming the proper hardware is there). Consult the command line help for each program on usage. To cross-compile the examples for ARM:
```bash
make CROSS_COMPILE=arm-linux-gnueabihf- ARCH=arm examples
```

This will generate executables for the examples under `outputs`.

### Compiling and Using the Library

The userspace library is compiled the typical shared object file. To compile the library for ARM:
```bash
make CROSS_COMPILE=arm-linux-gnueabihf- ARCH=arm library
```

This will generate a shared object file for the AXI DMA library at `outputs/libaxidma.so`.

To use the library with your application, you need to compile your program against the library, by specifying the flags `-I </path/to/repo>/include -L </path/to/repo>/outputs/ -l axidma` when compiling. Then, so the executable can find the library at runtime, you need to copy the library file to the board's filesytem. You can either copy to one of the default system library directories, such as `/usr/lib/`, or you can add the `outputs` directory to the `LD_LIBRARY_PATH` environment variable. See how the examples programs are compiled in their [Makefile](https://github.com/bperez77/xilinx_axidma/blob/master/examples/Makefile).

### Generating the Library Documentation

The userspace library has Doxygen documentation for its interface. To generate and view this documentation, run:
```bash
make library_docs
```

This will generate both HTML and Latex documentation for the AXI DMA library under `docs`. This will also open up the HTML documentation in Firefox.

## Running an Application

Before you run an application that uses the AXI DMA software stack, you must make sure that the driver has been inserted into the kernel. This can be accomplished with:
```bash
insmod axidma.ko
```

This should create a character device for the driver at `/dev/axidma`. To verify that the insertion was successful, run `dmesg` to view the kernel log messages. You should see a message like the following:
```
axidma: axidma_dma.c: 672: DMA: Found 1 transmit channels and 1 receive channels.
axidma: axidma_dma.c: 674: VDMA: Found 0 transmit channels and 0 receive channels.
```

Naturally, the numbers will vary based on your specific configuration.

## Debugging Issues with the Software Stack

The driver prints out a detailed message every time that it encounters an error to the kernel log message buffer. If the library says that an error occured, run `dmesg` to see the kernel log. The driver will print out a detailed message, along with the file, function, and line number that the error occured on.

There is an issue with the location of the Xilinx DMA header file, `xilinx_dma.h` in the Xilinx kernels. In between the 3.x and 4.x version, the location of this file changed, but certain 4.x kernels still use the old location from the 3.x kernel. If you see an error message like the following when compiling the driver, then specify the `XILINX_DMA_INCLUDE_PATH_FIXUP` Makefile variable when compiling:
```
In file included from /home/bmperez/projects/xilinx_axidma/driver/axidma_dma.c:24:0:
/home/bmperez/projects/xilinx_axidma/driver/version_portability.h:25:68: fatal error: linux/dma/xilinx_dma.h: No such file or directory
 #include <linux/dma/xilinx_dma.h>   // Xilinx DMA config structures
                                                                    ^
compilation terminated.
```

## Using the Driver with a PetaLinux Kernel

For how to add the driver to a PetaLinux project and build it against a PetaLinux kernel, see [issue #24](https://github.com/bperez77/xilinx_axidma/issues/24).

## Limitations/To-Do's

1. There is currently no explicit support for concurrency, so only one thread should access the driver at a time.
2. The character device is opened in exclusive mode, so only one process can access the driver at a time.
3. The driver cannot export DMA buffers for sharing, it only supports importing at the moment.
4. There is no support for multi-channel mode.

## Additional Information

For more detailed information, including how to boot Linux on the Zedboard, and how to run a simple DMA loopback example to test the software stack's functionality, see the [wiki](https://github.com/bperez77/xilinx_axidma/wiki).

## References

Xilinx's AXI VDMA and DMA Driver - [Source Code](https://github.com/Xilinx/linux-xlnx/blob/master/drivers/dma/xilinx/xilinx_dma.c)

Xilinx's AXI DMA Test Driver (Shows How to Write a Driver to Use Xilinx's AXI DMA Driver) - [Source Code](https://github.com/Xilinx/linux-xlnx/blob/master/drivers/dma/xilinx/axidmatest.c)

Documentation on Xilinx AXI DMA Device Tree Node Bindings - [Documentation](https://github.com/Xilinx/linux-xlnx/blob/master/Documentation/devicetree/bindings/dma/xilinx/xilinx_dma.txt)

Documentation for DMA Device Tree Bindings - [Documentation](https://github.com/Xilinx/linux-xlnx/blob/master/Documentation/devicetree/bindings/dma/dma.txt)

Documentation for CMA (reserved memory regions) Device Tree Bindings - [Documentation](https://github.com/Xilinx/linux-xlnx/blob/master/Documentation/devicetree/bindings/reserved-memory/reserved-memory.txt)

Documentation on DMA Buffer Sharing - [Documentation](https://github.com/Xilinx/linux-xlnx/blob/master/Documentation/dma-buf-sharing.txt)

Documentation for Xilinx's AXI DMA IP Core (Vivado 2015.2, Version 7.1) - [Documentation](http://www.xilinx.com/support/documentation/ip_documentation/axi_dma/v7_1/pg021_axi_dma.pdf)

Documentation for Xilinx's AXI VDMA IP Core (Vivado 2015.2, Version 6.2) - [Documentation](http://www.xilinx.com/support/documentation/ip_documentation/axi_vdma/v6_2/pg020_axi_vdma.pdf)

## License (MIT)

The MIT License (MIT)

Copyright (c) 2015-2016 Brandon Perez <bmperez@andrew.cmu.edu> <br>
Copyright (c) 2015-2016 Jared Choi 

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
