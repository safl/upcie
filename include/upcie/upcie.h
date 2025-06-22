// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * uPCIe header bundle
 * ===================
 *
 * This is the default umbrella header for uPCIe. Its purpose is to provide a convenient way to
 * include all uPCIe header-only libraries in one go, simplifying consumption for most use cases.
 *
 * You are not required to use this bundle — individual headers can be included selectively
 * instead. This is useful in situations where:
 *
 *  - You want to control which libc or toolchain headers are included (e.g., avoid glibc/musl assumptions).
 *  - You need to redefine system integration (e.g., MMIO access, memory handling).
 *  - You want to avoid namespace clashes with other libraries or in-house code.
 *
 * Simply swap this header with a custom version that includes only what your driver needs.
 * This pattern is intentional: the bundle provides convenience, not coupling.
 *
 * @file upcie.h
 */
#ifndef UPCIE_H
#define UPCIE_H

#define _GNU_SOURCE

#ifdef __cplusplus
extern "C" {
#endif

// Toolchain and system headers
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

// Linux UAPI
#include <linux/memfd.h>
#include <linux/vfio.h>

// uPCIe libraries
#include <upcie/bitfield.h>
#include <upcie/hostmem.h>
#include <upcie/hostmem_hugepage.h>
#include <upcie/hostmem_heap.h>
#include <upcie/hostmem_dma.h>
#include <upcie/mmio.h>
#include <upcie/pci.h>
#include <upcie/vfioctl.h>

#ifdef __cplusplus
}
#endif

#endif // UPCIE_H