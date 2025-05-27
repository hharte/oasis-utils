/* tests/test_oasis_alloc.cpp */
#include "gtest/gtest.h"

#include "oasis.h"       /* For oasis_alloc_map_t definition */

#include <stdlib.h>      /* For malloc, free */
#include <string.h>      /* For memset, memcpy */
#include <vector>        /* For convenience in managing map data */
#include <limits.h>      /* For SIZE_MAX (though not directly used in this version) */


/*
 * Fixture for oasis_alloc tests.
 * Manages the lifecycle of an oasis_alloc_map_t for each test.
 */
class OasisAllocTest : public ::testing::Test {
protected:
    oasis_alloc_map_t test_map;
    std::vector<uint8_t> map_buffer; /* Owns the memory for the map */

    /*
     * Helper to initialize the test_map.
     * @param num_bytes The size of the allocation map in bytes.
     * @param initial_data Optional data to fill the map. If nullptr, map is zeroed (all free).
     */
    void CreateMap(size_t num_bytes, const uint8_t* initial_data = nullptr) {
        map_buffer.resize(num_bytes);
        if (initial_data) {
            memcpy(map_buffer.data(), initial_data, num_bytes);
        }
        else {
            /* For MSB-first, 0x00 still means all blocks in the byte are free (0). */
            memset(map_buffer.data(), 0, num_bytes);
        }
        test_map.map_data = map_buffer.data();
        test_map.map_size_bytes = num_bytes;
    }

    void SetUp() override {
        /* Default map for simple tests, can be overridden by calling CreateMap in the test */
        test_map.map_data = nullptr;
        test_map.map_size_bytes = 0;
        map_buffer.clear();
    }

    void TearDown() override {
        /* map_buffer will be cleared automatically.
           No manual free needed as map_data points to vector's data. */
    }
};

/* --- New Tests for get_allocation_map_maximum_blocks --- */
TEST_F(OasisAllocTest, GetAllocationMapMaximumBlocks) {
    CreateMap(1); /* 1 byte = 8 blocks representable */
    ASSERT_EQ(get_allocation_map_maximum_blocks(&test_map), static_cast<size_t>(8));

    CreateMap(10); /* 10 bytes = 80 blocks representable */
    ASSERT_EQ(get_allocation_map_maximum_blocks(&test_map), static_cast<size_t>(80));

    CreateMap(0); /* 0 bytes = 0 blocks representable */
    ASSERT_EQ(get_allocation_map_maximum_blocks(&test_map), static_cast<size_t>(0));

    ASSERT_EQ(get_allocation_map_maximum_blocks(nullptr), static_cast<size_t>(0));

    oasis_alloc_map_t null_map_data_struct = { nullptr, 5 }; /* map_data is NULL */
    ASSERT_EQ(get_allocation_map_maximum_blocks(&null_map_data_struct), static_cast<size_t>(0));
}


TEST_F(OasisAllocTest, IsBlockInRange) {
    CreateMap(2); /* 16 blocks (0-15) representable by the map */
    EXPECT_TRUE(is_block_in_range(&test_map, 0));
    EXPECT_TRUE(is_block_in_range(&test_map, 7));
    EXPECT_TRUE(is_block_in_range(&test_map, 15));
    EXPECT_FALSE(is_block_in_range(&test_map, 16)); /* Beyond map capacity */
    EXPECT_FALSE(is_block_in_range(&test_map, 100));
    EXPECT_FALSE(is_block_in_range(nullptr, 5));
}

TEST_F(OasisAllocTest, GetAndSetBlockStateMSBFirst) {
    /*
     * Convention: MSB is block N (e.g. Block 0 is bit 7), LSB is block N+7 (e.g. Block 7 is bit 0).
     * Initial data:
     * Block 0 allocated: Byte 0, bit 7 set -> 0x80
     * Block 15 allocated: Byte 1, bit 0 set (since 15 % 8 = 7, target bit is 7-7=0) -> 0x01
     */
    uint8_t initial_data[] = { 0x80, 0x01 };
    CreateMap(2, initial_data); /* 2 bytes = 16 blocks */
    int state;

    /* Test get_block_state */
    ASSERT_EQ(get_block_state(&test_map, 0, &state), 0); EXPECT_EQ(state, 1);  /* Block 0 is MSB of byte 0 */
    ASSERT_EQ(get_block_state(&test_map, 1, &state), 0); EXPECT_EQ(state, 0);  /* Block 1 is bit 6 of byte 0 */
    ASSERT_EQ(get_block_state(&test_map, 7, &state), 0); EXPECT_EQ(state, 0);  /* Block 7 is LSB of byte 0 */

    ASSERT_EQ(get_block_state(&test_map, 8, &state), 0); EXPECT_EQ(state, 0);  /* Block 8 is MSB of byte 1 */
    ASSERT_EQ(get_block_state(&test_map, 14, &state), 0); EXPECT_EQ(state, 0); /* Block 14 is bit 1 of byte 1 */
    ASSERT_EQ(get_block_state(&test_map, 15, &state), 0); EXPECT_EQ(state, 1); /* Block 15 is LSB of byte 1 */


    ASSERT_EQ(get_block_state(&test_map, 16, &state), -1); /* Out of range */
    ASSERT_EQ(get_block_state(nullptr, 0, &state), -1);
    ASSERT_EQ(get_block_state(&test_map, 0, nullptr), -1);


    /* Test set_block_state */
    /* Allocate block 5. block_num % 8 = 5. Target bit = 7-5 = 2. Mask = 1 << 2 = 0x04. */
    /* map_buffer[0] was 0x80 (10000000). Or with 0x04 -> 10000100 (0x84). */
    ASSERT_EQ(set_block_state(&test_map, 5, 1), 0);
    ASSERT_EQ(get_block_state(&test_map, 5, &state), 0);
    EXPECT_EQ(state, 1);
    EXPECT_EQ(map_buffer[0], 0x84); /* Block 0 (bit 7) and Block 5 (bit 2) set */

    /* Deallocate block 15. block_num % 8 = 7. Target bit = 7-7 = 0. Mask = 1 << 0 = 0x01. */
    /* map_buffer[1] was 0x01 (00000001). And with ~0x01 (11111110) -> 00000000 (0x00). */
    ASSERT_EQ(set_block_state(&test_map, 15, 0), 0);
    ASSERT_EQ(get_block_state(&test_map, 15, &state), 0);
    EXPECT_EQ(state, 0);
    EXPECT_EQ(map_buffer[1], 0x00);

    EXPECT_EQ(set_block_state(&test_map, 16, 1), -1); /* Out of range */
    EXPECT_EQ(set_block_state(&test_map, 5, 2), -1);  /* Invalid state */
    EXPECT_EQ(set_block_state(nullptr, 0, 1), -1);
}

TEST_F(OasisAllocTest, FindLargestFreeContiguousBlocksMSBFirst) {
    uint8_t data1[] = { 0xFF }; /* All allocated */
    CreateMap(1, data1);
    EXPECT_EQ(find_largest_free_contiguous_blocks(&test_map), static_cast<size_t>(0));

    uint8_t data2[] = { 0x00 }; /* All free */
    CreateMap(1, data2);
    EXPECT_EQ(find_largest_free_contiguous_blocks(&test_map), static_cast<size_t>(8));

    /*
     * data3: 0x8C -> 10001100
     * MSB-first bit interpretation for blocks 0-7:
     * B0 (bit 7): 1 (A)
     * B1 (bit 6): 0 (F)
     * B2 (bit 5): 0 (F)
     * B3 (bit 4): 0 (F)  Chunk [B1-B3], size 3
     * B4 (bit 3): 1 (A)
     * B5 (bit 2): 1 (A)
     * B6 (bit 1): 0 (F)
     * B7 (bit 0): 0 (F)  Chunk [B6-B7], size 2
     * Largest free chunk is 3.
    */
    uint8_t data3[] = { 0b10001100 };
    CreateMap(1, data3);
    EXPECT_EQ(find_largest_free_contiguous_blocks(&test_map), static_cast<size_t>(3));

    /*
     * data4: 0x62 -> 01100010
     * MSB-first bit interpretation for blocks 0-7:
     * B0 (bit 7): 0 (F)  Chunk [B0-B0], size 1
     * B1 (bit 6): 1 (A)
     * B2 (bit 5): 1 (A)
     * B3 (bit 4): 0 (F)
     * B4 (bit 3): 0 (F)
     * B5 (bit 2): 0 (F)  Chunk [B3-B5], size 3
     * B6 (bit 1): 1 (A)
     * B7 (bit 0): 0 (F)  Chunk [B7-B7], size 1
     * Largest free chunk is 3.
    */
    uint8_t data4[] = { 0b01100010 };
    CreateMap(1, data4);
    EXPECT_EQ(find_largest_free_contiguous_blocks(&test_map), static_cast<size_t>(3));

    uint8_t data5[] = { 0xFF, 0x00, 0xFF }; /* Middle byte (blocks 8-15) is all free */
    CreateMap(3, data5);
    EXPECT_EQ(find_largest_free_contiguous_blocks(&test_map), static_cast<size_t>(8));

    CreateMap(0); /* Empty map */
    EXPECT_EQ(find_largest_free_contiguous_blocks(&test_map), static_cast<size_t>(0));
    EXPECT_EQ(find_largest_free_contiguous_blocks(nullptr), static_cast<size_t>(0));
}

TEST_F(OasisAllocTest, CountTotalFreeBlocksMSBFirst) {
    uint8_t data1[] = { 0xFF }; /* 0 free */
    CreateMap(1, data1);
    EXPECT_EQ(count_total_free_blocks(&test_map), static_cast<size_t>(0));

    uint8_t data2[] = { 0x00 }; /* 8 free */
    CreateMap(1, data2);
    EXPECT_EQ(count_total_free_blocks(&test_map), static_cast<size_t>(8));

    /* data3: 0xAA -> 10101010
     * B0(A), B1(F), B2(A), B3(F), B4(A), B5(F), B6(A), B7(F) -> 4 free blocks
     */
    uint8_t data3[] = { 0b10101010 };
    CreateMap(1, data3);
    EXPECT_EQ(count_total_free_blocks(&test_map), static_cast<size_t>(4));

    uint8_t data5[] = { 0xFF, 0x00, 0xFF }; /* Middle byte (8 blocks) free */
    CreateMap(3, data5);
    EXPECT_EQ(count_total_free_blocks(&test_map), static_cast<size_t>(8));

    CreateMap(0); /* Empty map */
    EXPECT_EQ(count_total_free_blocks(&test_map), static_cast<size_t>(0));
    EXPECT_EQ(count_total_free_blocks(nullptr), static_cast<size_t>(0));
}

TEST_F(OasisAllocTest, AllocateBlocksSimpleMSBFirst) {
    CreateMap(2); /* 16 blocks, all 0x00 (free) */
    int start_block;
    int state;

    /* Allocate blocks 0,1,2,3,4 (5 blocks) */
    /* These are bits 7,6,5,4,3 of byte 0 */
    /* Expected map_buffer[0] = 11111000 (0xF8) */
    start_block = allocate_blocks(&test_map, 5);
    ASSERT_EQ(start_block, 0);
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(get_block_state(&test_map, i, &state), 0);
        EXPECT_EQ(state, 1);
    }
    ASSERT_EQ(get_block_state(&test_map, 5, &state), 0); /* Block 5 should be free */
    EXPECT_EQ(state, 0);
    EXPECT_EQ(map_buffer[0], 0xF8);

    /* Allocate blocks 5,6,7 (3 blocks) */
    /* These are bits 2,1,0 of byte 0 */
    /* map_buffer[0] was 0xF8 (11111000). OR with 00000111 (0x07) -> 11111111 (0xFF) */
    start_block = allocate_blocks(&test_map, 3);
    ASSERT_EQ(start_block, 5);
    EXPECT_EQ(map_buffer[0], 0xFF);
}


TEST_F(OasisAllocTest, AllocateBlocksBestFitMSBFirst) {
    /*
     * MSB-first convention. (Bit 7=Block N, Bit 0=Block N+7)
     * Initial map state:
     * Byte 0: 0x3E -> 00111110 -> Blocks [0-1] Free (size 2), Block [7] Free (size 1)
     * Byte 1: 0x07 -> 00000111 -> Blocks [8-12] Free (size 5)
     * Byte 2: 0xC1 -> 11000001 -> Blocks [18-22] Free (size 5)
     */
    uint8_t initial_data[] = { 0x3E, 0x07, 0xC1 };
    CreateMap(3, initial_data);
    int start_block;

    /* 1. Allocate 2 blocks.
     * Candidates: [0-1] (size 2), [7-12] (size 6), [18-22] (size 5)
     * Best fit (smallest that fits) is [0-1].
     */
    start_block = allocate_blocks(&test_map, 2);
    ASSERT_EQ(start_block, 0);
    /* map_buffer[0] was 0x3E (00111110). Alloc B0,B1 (bits 7,6) -> mask 11000000
     * 00111110 | 11000000 = 11111110 (0xFE)
     */
    EXPECT_EQ(map_buffer[0], 0xFE);

    /* State after 1st allocation:
     * Byte 0: 0xFE (11111110) -> Block [7] Free (size 1)
     * Byte 1: 0x07 (00000111) -> Blocks [8-12] Free (size 5)
     * Byte 2: 0xC1 (11000001) -> Blocks [18-22] Free (size 5)
     * Contiguous free chunks: [7-12] (size 6), [18-22] (size 5)
     */

     /* 2. Allocate 4 blocks.
      * Candidates: [7-12] (size 6), [18-22] (size 5)
      * Best fit (smallest that fits) is [18-22].
      */
    start_block = allocate_blocks(&test_map, 4);
    ASSERT_EQ(start_block, 18); /* Updated expectation */
    /* map_buffer[1] (0x07) is UNCHANGED.
     * map_buffer[2] was 0xC1 (11000001). Alloc B18,B19,B20,B21 (bits 5,4,3,2 of byte 2) -> mask 00111100
     * 11000001 | 00111100 = 11111101 (0xFD)
     */
    EXPECT_EQ(map_buffer[1], 0x07); /* Updated expectation */
    EXPECT_EQ(map_buffer[2], 0xFD); /* Added expectation for changed byte */


    /* State after 2nd allocation:
     * Byte 0: 0xFE (11111110) -> Block [7] Free (size 1)
     * Byte 1: 0x07 (00000111) -> Blocks [8-12] Free (size 5)
     * Byte 2: 0xFD (11111101) -> Block [22] Free (size 1)
     * Contiguous free chunks: [7-12] (size 6), [22-22] (size 1)
     */

     /* 3. Allocate 1 block.
      * Candidates: [7-12] (size 6), [22-22] (size 1)
      * Best fit (smallest that fits) is [22-22].
      */
    start_block = allocate_blocks(&test_map, 1);
    ASSERT_EQ(start_block, 22); /* Updated expectation */
    /* map_buffer[0] (0xFE) is UNCHANGED.
     * map_buffer[1] (0x07) is UNCHANGED.
     * map_buffer[2] was 0xFD (11111101). Alloc B22 (bit 1 of byte 2) -> mask 00000010
     * 11111101 | 00000010 = 11111111 (0xFF)
     */
    EXPECT_EQ(map_buffer[0], 0xFE); /* Updated expectation */
    EXPECT_EQ(map_buffer[2], 0xFF); /* Updated expectation for changed byte */


    /* State after 3rd allocation:
     * Byte 0: 0xFE (11111110) -> Block [7] Free (size 1)
     * Byte 1: 0x07 (00000111) -> Blocks [8-12] Free (size 5)
     * Byte 2: 0xFF (11111111) -> All blocks in byte 2 allocated
     * Contiguous free chunks: [7-12] (size 6)
     */

     /* 4. Allocate 5 blocks.
      * Candidate: [7-12] (size 6)
      * Best fit is [7-12].
      */
    start_block = allocate_blocks(&test_map, 5);
    ASSERT_EQ(start_block, 7); /* Corrected: B7 from map_buffer[0], B8-B11 from map_buffer[1] */
    /* map_buffer[0] was 0xFE (11111110). Alloc B7 (bit 0) -> mask 00000001
     * 11111110 | 00000001 = 11111111 (0xFF)
     */
    EXPECT_EQ(map_buffer[0], 0xFF);
    /* map_buffer[1] was 0x07 (00000111). Alloc B8,B9,B10,B11 (bits 7,6,5,4) -> mask 11110000
     * 00000111 | 11110000 = 11110111 (0xF7)
     */
    EXPECT_EQ(map_buffer[1], 0xF7);
    /* map_buffer[2] (0xFF) is UNCHANGED. */
    EXPECT_EQ(map_buffer[2], 0xFF);
}


TEST_F(OasisAllocTest, AllocateBlocksEdgeCasesMSBFirst) {
    CreateMap(1); /* 8 blocks, all free (0x00) */

    EXPECT_EQ(allocate_blocks(&test_map, 0), -1);   /* Allocate 0 blocks */
    EXPECT_EQ(allocate_blocks(&test_map, 9), -1);   /* Allocate more than available */

    EXPECT_EQ(allocate_blocks(&test_map, 8), 0);    /* Allocate all 8 blocks (0-7) */
    /* All blocks 0-7 allocated. Bits 7 down to 0 should be 1. -> 11111111 (0xFF) */
    EXPECT_EQ(map_buffer[0], 0xFF);
    EXPECT_EQ(allocate_blocks(&test_map, 1), -1);   /* Should be full */

    EXPECT_EQ(allocate_blocks(nullptr, 1), -1);     /* NULL map */
    oasis_alloc_map_t map_no_data = { nullptr, 1 };
    EXPECT_EQ(allocate_blocks(&map_no_data, 1), -1);/* map_data is NULL */
}

TEST_F(OasisAllocTest, DeallocateBlocksSimpleMSBFirst) {
    /* Byte 0 (0xFF): blocks 0-7 allocated
     * Byte 1 (0xF0 -> 11110000): blocks 8,9,10,11 allocated (bits 7,6,5,4 of byte 1)
     */
    uint8_t initial_data[] = { 0xFF, 0xF0 };
    CreateMap(2, initial_data);
    int state;

    /* Deallocate blocks 2-5 (bits 5,4,3,2 of byte 0) */
    /* map_buffer[0] was 0xFF (11111111). Clear bits 5,4,3,2 (mask 00111100) -> 11000011 (0xC3) */
    ASSERT_EQ(deallocate_blocks(&test_map, 2, 4), 0);
    for (int i = 2; i <= 5; ++i) {
        ASSERT_EQ(get_block_state(&test_map, i, &state), 0);
        EXPECT_EQ(state, 0);
    }
    ASSERT_EQ(get_block_state(&test_map, 1, &state), 0); EXPECT_EQ(state, 1); /* B1 (bit 6) still alloc */
    ASSERT_EQ(get_block_state(&test_map, 6, &state), 0); EXPECT_EQ(state, 1); /* B6 (bit 1) still alloc */
    EXPECT_EQ(map_buffer[0], 0xC3);
}

TEST_F(OasisAllocTest, DeallocateBlocksErrorsMSBFirst) {
    /* Blocks 0-3 allocated (bits 7,6,5,4 of byte 0 set -> 11110000 -> 0xF0) */
    /* Blocks 4-7 free */
    uint8_t initial_data[] = { 0xF0 };
    CreateMap(1, initial_data);

    EXPECT_EQ(deallocate_blocks(&test_map, 0, 0), -1);    /* Deallocate 0 blocks */
    EXPECT_EQ(deallocate_blocks(&test_map, 6, 3), -1);    /* Out of range (block 8 is invalid for 1-byte map) */
    EXPECT_EQ(deallocate_blocks(&test_map, 8, 1), -1);    /* Start out of range */

    /* Try to deallocate blocks 4,5 which are already free */
    EXPECT_EQ(deallocate_blocks(&test_map, 4, 2), -1);

    /* Try to deallocate blocks 3 (allocated) and 4 (free) */
    EXPECT_EQ(deallocate_blocks(&test_map, 3, 2), -1);

    EXPECT_EQ(deallocate_blocks(nullptr, 0, 1), -1);    /* NULL map */
    oasis_alloc_map_t map_no_data = { nullptr, 1 };
    EXPECT_EQ(deallocate_blocks(&map_no_data, 0, 1), -1);/* map_data is NULL */
}


TEST_F(OasisAllocTest, PrintMapMSBFirst) {
    /*
     * MSB-first convention. (Bit 7=Block N, Bit 0=Block N+7)
     * Byte 0 (0xAA -> 10101010):
     * B0(A), B1(F), B2(A), B3(F), B4(A), B5(F), B6(A), B7(F)
     * Byte 1 (0xCC -> 11001100):
     * B8(A), B9(A), B10(F), B11(F), B12(A), B13(A), B14(F), B15(F)
     */
    uint8_t data[] = { 0xAA, 0xCC };
    CreateMap(2, data); /* 16 blocks */
    /* printf("Visual check for print_map (OasisAllocTest.PrintMapMSBFirst):\n"); */
    /* The GTest framework uses C++. */
    /* The following conditional print_map call can be enabled for manual inspection if needed */
    /* if (::testing::GTEST_FLAG(output) == "stdout" || ::testing::GTEST_FLAG(output) == "all") { */
    /* print_map(&test_map); */
    /* } */
    /* else { */
    /* printf(" (Run with GTest verbose flags or directly to see print_map output)\n"); */
    /* } */
    EXPECT_EQ(count_total_free_blocks(&test_map), static_cast<size_t>(8)); /* (4 from 0xAA, 4 from 0xCC) */
    /* Free chunks: B1(1), B3(1), B5(1), B7(1), B10-11(2), B14-15(2) */
    EXPECT_EQ(find_largest_free_contiguous_blocks(&test_map), static_cast<size_t>(2));
}
