/*
 * oasis_alloc.h - OASIS Allocation Map Management Library Interface
 *
 * This header file declares functions for managing the OASIS disk allocation map.
 * It provides utilities to allocate, deallocate, and query the state of
 * 1K blocks using a bitmap representation.
 * Part of the oasis-utils library.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef OASIS_ALLOC_H_
#define OASIS_ALLOC_H_

#include "oasis.h"
#include <stdio.h>

/* --- Debugging --- */
/* Define LIBOASIS_DEBUG to enable debug printf statements */
//#define LIBOASIS_DEBUG /* User can define this in their build if needed */

#ifdef LIBOASIS_DEBUG
/* DEBUG_PRINTF expands to printf when LIBOASIS_DEBUG is defined */
#define DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
/* DEBUG_PRINTF expands to nothing when LIBOASIS_DEBUG is not defined */
/* Using a do-while(0) wrapper ensures it behaves like a statement */
#define DEBUG_PRINTF(...) do { } while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- Helper Functions --- */

/**
 * @brief Calculates the total number of blocks the allocation map can represent based on its size.
 *
 * @param map A pointer to the allocation map structure.
 * @return The total number of blocks (bits) the map can track, or 0 if map is NULL or map_data is NULL.
 */
size_t get_allocation_map_maximum_blocks(const oasis_alloc_map_t* map);

/**
 * @brief Checks if a given block number is within the valid range for the map.
 *
 * This function checks if the `block_num` is less than the total number of
 * blocks the `map` can represent (derived from `map_size_bytes`).
 *
 * @param map         A pointer to the allocation map structure.
 * @param block_num   The block number to check.
 * @return true if the block number is valid relative to map capacity, false otherwise or if map is NULL.
 */
bool is_block_in_range(const oasis_alloc_map_t* map, size_t block_num);

/**
 * @brief Gets the allocation state of a single block.
 *
 * @param map         A pointer to the allocation map structure.
 * @param block_num   The block number whose state is to be retrieved.
 * @param state       A pointer to an integer where the state will be stored
 * (0 for free, 1 for allocated).
 * @return 0 on success, -1 on failure (e.g., invalid block number, NULL map or state pointer).
 */
int get_block_state(const oasis_alloc_map_t* map, size_t block_num, int* state);

/**
 * @brief Sets the allocation state of a single block.
 *
 * @param map         A pointer to the allocation map structure.
 * @param block_num   The block number whose state is to be set.
 * @param state       The desired state (0 to free, 1 to allocate).
 * @return 0 on success, -1 on failure (e.g., invalid block number, invalid state value, NULL map).
 */
int set_block_state(oasis_alloc_map_t* map, size_t block_num, int state);

/**
 * @brief Finds the size (in blocks) of the largest contiguous free chunk in the map.
 *
 * @param map A pointer to the allocation map structure.
 * @return The number of blocks in the largest free chunk, or 0 if map is NULL or no free blocks exist.
 */
size_t find_largest_free_contiguous_blocks(const oasis_alloc_map_t* map);

/**
 * @brief Counts the total number of free blocks in the allocation map.
 *
 * @param map A pointer to the allocation map structure.
 * @return The total count of free blocks (bits set to 0), or 0 if map is NULL.
 */
size_t count_total_free_blocks(const oasis_alloc_map_t* map);

/**
 * @brief Prints a representation of the allocation map to standard output (if DEBUG_PRINTF is enabled).
 *
 * Also displays the total free blocks and the size of the largest
 * contiguous free block. Output is conditional on LIBOASIS_DEBUG being defined.
 *
 * @param map A pointer to the allocation map structure.
 */
void print_map(const oasis_alloc_map_t* map);


/* --- Core Allocation/Deallocation Functions --- */

/**
 * @brief Allocates a sequence of contiguous blocks using a "best fit" strategy.
 *
 * Searches the entire map for all free contiguous blocks large enough to
 * satisfy the request and chooses the one whose size is closest to (but not
 * less than) the requested size. If multiple such blocks exist, the one
 * with the lowest starting block number is chosen.
 *
 * @param map         A pointer to the allocation map structure.
 * @param num_blocks  The number of contiguous blocks to allocate. Must be > 0.
 * @return The starting block number of the allocated sequence on success.
 * Returns -1 on failure (e.g., invalid input, no suitable space found, NULL map).
 */
int allocate_blocks(oasis_alloc_map_t* map, size_t num_blocks);

/**
 * @brief Deallocates a sequence of contiguous blocks.
 *
 * Ensures all blocks in the specified range are currently allocated before
 * marking them as free.
 *
 * @param map         A pointer to the allocation map structure.
 * @param start_block The starting block number of the sequence to deallocate.
 * @param num_blocks  The number of contiguous blocks to deallocate. Must be > 0.
 * @return 0 on success.
 * Returns -1 on failure (e.g., invalid input, range out of bounds,
 * trying to deallocate blocks that are not currently allocated, NULL map).
 */
int deallocate_blocks(oasis_alloc_map_t* map, size_t start_block, size_t num_blocks);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OASIS_ALLOC_H_ */
