// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Bit-level utilities for systems programming
 * ===========================================
 *
 * A lightweight header providing helpers for working with individual bits and bitfields in
 * low-level code. Designed for clarity and performance in systems, protocol, and register-level
 * programming.
 *
 * This header currently includes a bitfield extraction function and is intended to grow with
 * additional bit manipulation utilities over time.
 */
#include <stdint.h>

#ifndef UPCIE_BITS_H
#define UPCIE_BITS_H

/**
 * Extracts a bitfield from a 64-bit integer.
 *
 * @param val    The input 64-bit value to extract bits from.
 * @param offset The bit offset (starting from LSB at position 0).
 * @param width  The width of the bitfield in bits (1–64).
 *
 * @return The extracted bitfield, right-aligned to bit position 0.
 *
 * Example:
 *   bits(0xFF00, 8, 8) → 0xFF
 *
 * Note: Behavior is undefined if width is 0 or offset + width > 64.
 */
static inline uint64_t
bits(uint64_t val, uint8_t offset, uint8_t width)
{
	return (val >> offset) & ((1ULL << width) - 1);
}

#endif ///< UPCIE_BITS_H