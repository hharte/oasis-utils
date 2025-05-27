/*
 * oasis_alloc.c - Implementation of OASIS Allocation Map Management Library
 *
 * Provides functions to allocate and deallocate blocks using a bitmap
 * representation of disk space, following OASIS conventions.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 *
 * Based on information from oasis.h and OASIS Macro Assembler Reference Manual.
 */

#include "oasis.h"

#include <stdlib.h>   /* For malloc, free */
#include <string.h>   /* For memset */
#include <limits.h>   /* For SIZE_MAX */
#include <assert.h>   /* For basic sanity checks */

#define BITS_PER_BYTE 8

/* --- Helper Functions --- */

/**
 * @brief Calculates the total number of blocks the allocation map can represent based on its size.
 */
size_t get_allocation_map_maximum_blocks(const oasis_alloc_map_t* map) {
	if (!map || !map->map_data) {
		return 0;
	}
	return map->map_size_bytes * BITS_PER_BYTE;
}

/* Checks if a given block number is within the valid range for the map. */
bool is_block_in_range(const oasis_alloc_map_t* map, size_t block_num) {
	if (!map) {
		return false;
	}
	/* Check against the maximum number of blocks the map can represent */
	return block_num < get_allocation_map_maximum_blocks(map);
}

/* Gets the allocation state of a single block. */
int get_block_state(const oasis_alloc_map_t* map, size_t block_num, int* state) {
	size_t byte_index;
	uint8_t bit_mask;
	uint8_t byte_value;

	if (!map || !map->map_data || !state) {
		return -1; /* Invalid arguments */
	}

	if (!is_block_in_range(map, block_num)) {
		return -1; /* Block number out of range */
	}

	byte_index = block_num / BITS_PER_BYTE;
	/* MSB is block 0 (N), LSB is block 7 (N+7) within the byte */
	bit_mask = (uint8_t)(1 << (BITS_PER_BYTE - 1 - (block_num % BITS_PER_BYTE)));

	byte_value = map->map_data[byte_index];

	*state = (byte_value & bit_mask) ? 1 : 0; /* 1 if bit is set (allocated), 0 if clear (free) */

	return 0; /* Success */
}

/* Sets the allocation state of a single block. */
int set_block_state(oasis_alloc_map_t* map, size_t block_num, int state) {
	size_t byte_index;
	uint8_t bit_mask;

	if (!map || !map->map_data) {
		return -1; /* Invalid arguments */
	}

	if (!is_block_in_range(map, block_num)) {
		return -1; /* Block number out of range */
	}

	if (state != 0 && state != 1) {
		return -1; /* Invalid state value */
	}

	byte_index = block_num / BITS_PER_BYTE;
	/* MSB is block 0 (N), LSB is block 7 (N+7) within the byte */
	bit_mask = (uint8_t)(1 << (BITS_PER_BYTE - 1 - (block_num % BITS_PER_BYTE)));

	if (state == 1) {
		/* Allocate: Set the bit */
		map->map_data[byte_index] |= bit_mask;
	}
	else {
		/* Deallocate: Clear the bit */
		map->map_data[byte_index] &= ~bit_mask;
	}

	return 0; /* Success */
}

/* Finds the size (in blocks) of the largest contiguous free chunk in the map. */
size_t find_largest_free_contiguous_blocks(const oasis_alloc_map_t* map) {
	size_t total_blocks;
	size_t max_free_chunk = 0;
	size_t current_free_chunk = 0;
	size_t i;
	int state;

	if (!map || !map->map_data) {
		return 0;
	}

	total_blocks = get_allocation_map_maximum_blocks(map);

	for (i = 0; i < total_blocks; ++i) {
		if (get_block_state(map, i, &state) == 0 && state == 0) {
			/* Block is free */
			current_free_chunk++;
		}
		else {
			/* Block is allocated or error reading state */
			if (current_free_chunk > max_free_chunk) {
				max_free_chunk = current_free_chunk;
			}
			current_free_chunk = 0; /* Reset count */
		}
	}

	/* Check if the last run was the largest */
	if (current_free_chunk > max_free_chunk) {
		max_free_chunk = current_free_chunk;
	}

	return max_free_chunk;
}

/* Counts the total number of free blocks in the allocation map. */
size_t count_total_free_blocks(const oasis_alloc_map_t* map) {
	size_t total_blocks;
	size_t free_count = 0;
	size_t i;
	int state;

	if (!map || !map->map_data) {
		return 0;
	}

	total_blocks = get_allocation_map_maximum_blocks(map);

	for (i = 0; i < total_blocks; ++i) {
		if (get_block_state(map, i, &state) == 0 && state == 0) {
			free_count++;
		}
	}

	return free_count;
}


/* Prints a representation of the allocation map. */
void print_map(const oasis_alloc_map_t* map) {
	size_t total_blocks;
	size_t i;
	int state;
#ifdef LIBOASIS_DEBUG
	size_t free_blocks;
	size_t largest_chunk;
#endif /* LIBOASIS_DEBUG */

	DEBUG_PRINTF("\n--- Allocation Map Start ---\n");

	if (!map || !map->map_data) {
		DEBUG_PRINTF("Error: Cannot print NULL map.\n");
		DEBUG_PRINTF("--- Allocation Map End ---\n");
		return;
	}

	total_blocks = get_allocation_map_maximum_blocks(map);
	DEBUG_PRINTF("Allocation Map (%zu bytes, %zu blocks):\n", map->map_size_bytes, total_blocks);
	DEBUG_PRINTF("--------------------------------------------------\n");

	for (i = 0; i < total_blocks; ++i) {
		if (get_block_state(map, i, &state) == 0) {
			DEBUG_PRINTF("%d", state);
		}
		else {
			DEBUG_PRINTF("E"); /* Indicate error reading state */
		}
		/* Add spacing for readability */
		if ((i + 1) % 8 == 0) {
			DEBUG_PRINTF(" ");
		}
		if ((i + 1) % 64 == 0) {
			DEBUG_PRINTF("\n");
		}
	}
	if (total_blocks % 64 != 0) {
		DEBUG_PRINTF("\n"); /* Ensure newline at the end */
	}

#ifdef LIBOASIS_DEBUG
	free_blocks = count_total_free_blocks(map);
	largest_chunk = find_largest_free_contiguous_blocks(map);

	DEBUG_PRINTF("--------------------------------------------------\n");
	DEBUG_PRINTF("Total Free Blocks: %zu\n", free_blocks);
	DEBUG_PRINTF("Largest Contiguous Free Chunk: %zu blocks\n", largest_chunk);
	DEBUG_PRINTF("--------------------------------------------------\n");
	DEBUG_PRINTF("--- Allocation Map End ---\n");
#endif /* LIBOASIS_DEBUG */
}


/* --- Core Allocation/Deallocation Functions --- */

/* Allocates a sequence of contiguous blocks using a "best fit" strategy. */
int allocate_blocks(oasis_alloc_map_t* map, size_t num_blocks) {
	size_t total_blocks;
	size_t current_start_block = 0; /* Initialized */
	size_t current_free_count = 0;
	size_t best_start_block = (size_t)-1;
	size_t best_chunk_size = SIZE_MAX;
	size_t i;
	int state;
	size_t j; /* Declare j for rollback loop */

	/* Use the simplified debug macro */
	DEBUG_PRINTF("allocate_blocks: Requesting %zu blocks.\n", num_blocks);
	/* print_map(map); // print_map internally uses DEBUG_PRINTF */

	if (!map || !map->map_data || num_blocks == 0) {
		/* Use fprintf for errors that should always be shown */
		fprintf(stderr, "allocate_blocks error: Invalid arguments. map=%p, map_data=%p, num_blocks=%zu\n",
			(void*)map, (map ? (void*)map->map_data : NULL), num_blocks);
		return -1;
	}

	total_blocks = get_allocation_map_maximum_blocks(map);
	DEBUG_PRINTF("allocate_blocks: Total blocks in map: %zu\n", total_blocks);


	if (num_blocks > total_blocks) {
		fprintf(stderr, "allocate_blocks error: num_blocks (%zu) > total_blocks (%zu). Cannot allocate.\n", num_blocks, total_blocks);
		return -1;
	}

	for (i = 0; i < total_blocks; ++i) {
		if (get_block_state(map, i, &state) == 0 && state == 0) {
			if (current_free_count == 0) {
				current_start_block = i;
				DEBUG_PRINTF("allocate_blocks: Start of new free chunk at block %zu\n", current_start_block);
			}
			current_free_count++;
		}
		else {
			/* DEBUG_PRINTF("allocate_blocks: Block %zu is allocated or error (state=%d). Current free count was %zu starting at %zu.\n", i, state, current_free_count, current_start_block); */
			if (current_free_count >= num_blocks) {
				DEBUG_PRINTF("allocate_blocks: Chunk ending at %zu (size %zu, start %zu) is candidate. best_chunk_size=%zu, best_start_block=%zu\n",
					i - 1, current_free_count, current_start_block, best_chunk_size, best_start_block);
				if (current_free_count < best_chunk_size) {
					best_chunk_size = current_free_count;
					best_start_block = current_start_block;
					DEBUG_PRINTF("allocate_blocks: New best fit: start=%zu, size=%zu\n", best_start_block, best_chunk_size);
				}
				else if (current_free_count == best_chunk_size) {
					if (best_start_block == (size_t)-1 || current_start_block < best_start_block) {
						best_start_block = current_start_block; /* Prefer earlier block for same best size */
						DEBUG_PRINTF("allocate_blocks: Tie for best fit size, chose earlier start: start=%zu, size=%zu\n", best_start_block, best_chunk_size);
					}
				}
			}
			current_free_count = 0;
		}
	}

	/* Check the last chunk after the loop finishes */
	DEBUG_PRINTF("allocate_blocks: After loop. Current free count was %zu starting at %zu.\n", current_free_count, current_start_block);
	if (current_free_count >= num_blocks) {
		DEBUG_PRINTF("allocate_blocks: Final chunk (size %zu, start %zu) is candidate. best_chunk_size=%zu, best_start_block=%zu\n",
			current_free_count, current_start_block, best_chunk_size, best_start_block);
		if (current_free_count < best_chunk_size) {
			best_chunk_size = current_free_count;
			best_start_block = current_start_block;
			DEBUG_PRINTF("allocate_blocks: New best fit (final chunk): start=%zu, size=%zu\n", best_start_block, best_chunk_size);
		}
		else if (current_free_count == best_chunk_size) {
			if (best_start_block == (size_t)-1 || current_start_block < best_start_block) {
				best_start_block = current_start_block;
				DEBUG_PRINTF("allocate_blocks: Tie for best fit size (final chunk), chose earlier start: start=%zu, size=%zu\n", best_start_block, best_chunk_size);
			}
		}
	}

	if (best_start_block != (size_t)-1) {
		DEBUG_PRINTF("allocate_blocks: Allocating %zu blocks starting at %zu (chunk size was %zu)\n", num_blocks, best_start_block, best_chunk_size);
		for (i = 0; i < num_blocks; ++i) {
			if (set_block_state(map, best_start_block + i, 1) != 0) {
				fprintf(stderr, "Error: Failed to set block state during allocation. Attempting rollback.\n");
				/* Use local variable j for rollback */
				for (j = 0; j < i; ++j) {
					set_block_state(map, best_start_block + j, 0);
				}
				return -1;
			}
		}
		return (int)best_start_block;
	}
	else {
		fprintf(stderr, "allocate_blocks error: No suitable block found for %zu blocks.\n", num_blocks);
		return -1;
	}
}

/* Deallocates a sequence of contiguous blocks. */
int deallocate_blocks(oasis_alloc_map_t* map, size_t start_block, size_t num_blocks) {
	size_t total_blocks;
	size_t end_block; /* Inclusive end block number */
	size_t i;
	int state;

	if (!map || !map->map_data || num_blocks == 0) {
		fprintf(stderr, "deallocate_blocks error: Invalid arguments.\n");
		return -1; /* Invalid arguments */
	}

	total_blocks = get_allocation_map_maximum_blocks(map);

	/* Check for potential overflow before calculating end_block */
	if (start_block >= total_blocks || num_blocks > total_blocks - start_block) {
		fprintf(stderr, "deallocate_blocks error: Range calculation would overflow or start block %zu invalid for total %zu.\n", start_block, total_blocks);
		return -1; /* Range calculation would overflow or start is invalid */
	}
	end_block = start_block + num_blocks - 1;


	/* Check if the entire range is within the map bounds */
	/* Note: is_block_in_range checks < total_blocks, so start_block and end_block must be less than total_blocks */
	if (!is_block_in_range(map, start_block) || !is_block_in_range(map, end_block)) {
		fprintf(stderr, "deallocate_blocks error: Range [%zu, %zu] extends beyond map boundaries (0 to %zu).\n", start_block, end_block, total_blocks - 1);
		return -1; /* Range extends beyond map boundaries */
	}

	/* --- Pass 1: Verify all blocks in the range are currently allocated --- */
	for (i = start_block; i <= end_block; ++i) {
		if (get_block_state(map, i, &state) != 0) {
			/* Error reading state - treat as failure */
			fprintf(stderr, "Error: Failed to read block state during deallocation check for block %zu.\n", i);
			return -1;
		}
		if (state == 0) {
			/* Found a block within the range that is already free */
			/* Mimics SC 28 error: trying to deallocate unallocated space */
			fprintf(stderr, "Error: Attempting to deallocate block %zu which is already free.\n", i);
			return -1;
		}
	}

	DEBUG_PRINTF("deallocate_blocks: Deallocating %zu blocks starting at %zu.\n", num_blocks, start_block);

	/* --- Pass 2: Deallocate the blocks --- */
	for (i = start_block; i <= end_block; ++i) {
		if (set_block_state(map, i, 0) != 0) {
			/* Should ideally not happen if range checks and state checks passed */
			/* Rollback is difficult and potentially problematic here */
			fprintf(stderr, "Error: Failed to set block state during deallocation for block %zu. Map may be in inconsistent state.\n", i);
			/* Continue trying to free the rest, but report failure */
			return -1; /* Indicate failure */
		}
	}

	return 0; /* Success */
}
