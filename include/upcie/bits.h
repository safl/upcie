// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Bitfield utilities for systems programming
 * ==========================================
 *
 * A lightweight header providing helpers for working with individual bits and bitfields in
 * low-level code. Designed for clarity and performance in systems, protocol, and register-level
 * programming.
 *
 * This header currently includes a bitfield extraction function and is intended to grow with
 * additional bit manipulation utilities over time.
 */

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
bitfield_get(uint64_t val, uint8_t offset, uint8_t width)
{
	return (val >> offset) & ((1ULL << width) - 1);
}

/**
 * Sets a bitfield in a 64-bit integer.
 *
 * @param val    The original 64-bit value to modify.
 * @param offset The bit offset (starting from LSB at position 0).
 * @param width  The width of the bitfield in bits (1–64).
 * @param field  The value to insert into the bitfield (must fit in width).
 *
 * @return The modified 64-bit value with the specified bitfield updated.
 *
 * Example:
 *   bitfield_set(0x0000, 8, 8, 0xFF) → 0xFF00
 *
 * Note: Behavior is undefined if width is 0, offset + width > 64,
 *       or if field contains bits outside the specified width.
 */
static inline uint64_t
bitfield_set(uint64_t val, uint8_t offset, uint8_t width, uint64_t field)
{
	uint64_t mask = ((1ULL << width) - 1) << offset;
	return (val & ~mask) | ((field << offset) & mask);
}
