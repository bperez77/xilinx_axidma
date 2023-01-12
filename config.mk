# Makefile Configuration Template
#
# This file is a template for the 'config.mk' file, which stores the
# values of variables used in the Makefile. This can be used instead of
# specifying the variables on the command line, which can quickly become
# cumbersome.
#
# To make use of this functionality, copy this file to 'config.mk', and then
# fill in the values as desired. Any of these variables can be overridden from
# the command line, so the variables are not permanent.

# TODO: Copy this file to 'config.mk' and uncomment and assign the variables

################################################################################
# Cross Compilation Options
################################################################################

# This controls the cross compiler that is used to compile all of the code. This
# should be a compiler prefix (e.g. `arm-linux-gnueabihf-`). The code will be
# compiled with the program `$(CROSS_COMPILE)gcc`
CROSS_COMPILE = aarch64-linux-gnu-

# This variable informs the kernel Makefile what architecture you're targeting.
# This is required when building the driver, if CROSS_COMPILE is defined.
ARCH = aarch64

################################################################################
# Build Options
################################################################################

# The path to the top-level directory of kernel source tree that you want to
# compile the driver against. If unspecified, the system default
# `/lib/modules/$(uname -r)/build` is used. If CROSS_COMPILE is defined, then
# this variable must also be defined. This path can be absolute, or relative.
KBUILD_DIR = /home/shun/linux-xlnx

# The path to the output directory, where all of the compiled files, the
# driver's kernel object, the example executables, and the AXI DMA shared
# library file are placed. If unspecified, the files are stored in `outputs` in
# the top level of the repository. This path can either be relative or absolute.
OUTPUT_DIR = outputs

# This specifies to fixup the path that the file `xilinx_dma.h` is located at in
# the kernel that you're compiling against. The location of this header file was
# changed between the 3.x and 4.x Xilinx kernel versions. However, some 4.x
# kernels still have the file at the old location. This variable specifies the
# Makefile to define a macro that fixes this issue. Uncomment this (no value is
# required), if the driver fails to compile with an error like:
#     `fatal error: linux/dma/xilinx_dma.h: No such file or directory`
#XILINX_DMA_INCLUDE_PATH_FIXUP = yes
