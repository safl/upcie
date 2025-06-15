// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Helpers for 32-bit and 64-bit MMIO read/write access.
 *
 * These functions perform direct memory access to device registers via a memory-mapped
 * I/O (MMIO) region. Each function uses `volatile` semantics to ensure memory access
 * ordering and prevent compiler optimizations that could interfere with hardware communication.
 *
 * The `region` pointer typically refers to the base of a memory-mapped PCI BAR (e.g., from
 * `struct pci_func_bar.region`).
 *
 * @file mmio.h
 *
 */

/**
 * Read a 32-bit value from an MMIO region at the given offset
 *
 * @param region  Pointer to the base of the MMIO region
 * @param offset  Byte offset from the base
 * @return        32-bit value read from the region
 */
static inline uint32_t
mmio_read32(void *region, uint32_t offset)
{
	return *(volatile uint32_t *)((uintptr_t)region + offset);
}

/**
 * Write a 32-bit value to an MMIO region at the given offset
 *
 * @param region  Pointer to the base of the MMIO region
 * @param offset  Byte offset from the base
 * @param value   32-bit value to write
 */
static inline void
mmio_write32(void *region, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)((uintptr_t)region + offset) = value;
}

/**
 * Read a 64-bit value from an MMIO region at the given offset
 *
 * @param region  Pointer to the base of the MMIO region
 * @param offset  Byte offset from the base
 * @return        64-bit value read from the region
 */
static inline uint64_t
mmio_read64(void *region, uint32_t offset)
{
	return *(volatile uint64_t *)((uintptr_t)region + offset);
}

/**
 * Write a 64-bit value to an MMIO region at the given offset
 *
 * @param region  Pointer to the base of the MMIO region
 * @param offset  Byte offset from the base
 * @param value   64-bit value to write
 */
static inline void
mmio_write64(void *region, uint32_t offset, uint64_t value)
{
	*(volatile uint64_t *)((uintptr_t)region + offset) = value;
}
