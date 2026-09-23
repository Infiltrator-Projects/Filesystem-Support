/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sfs_core.h"
#include <stdio.h>
static int fail(const char *m){ fprintf(stderr,"sfs core test: %s\n",m); return 1; }
int main(void)
{
    ifs_sfs_u32 cap=0U,count=0U;
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,3U,512U,100000U,3U,4U,10U,5U,6U)!=IFS_SFS_ROOT_OK)
        return fail("valid root rejected");
    if (ifs_sfs_validate_root_layout(0U,3U,512U,100000U,3U,4U,10U,5U,6U)!=IFS_SFS_ROOT_BAD_ID)
        return fail("bad id accepted");
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,4U,512U,100000U,3U,4U,10U,5U,6U)!=IFS_SFS_ROOT_BAD_VERSION)
        return fail("SFS2 accepted as SFS");
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,3U,768U,100000U,3U,4U,10U,5U,6U)!=IFS_SFS_ROOT_INVALID_BLOCK_SIZE)
        return fail("invalid block size accepted");
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,3U,512U,100U,100U,4U,10U,5U,6U)!=IFS_SFS_ROOT_BLOCK_REFERENCE_OUT_OF_RANGE)
        return fail("bad block reference accepted");
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,3U,512U,100U,3U,4U,98U,5U,6U)!=IFS_SFS_ROOT_TRANSACTION_BLOCK_OUT_OF_RANGE)
        return fail("bad transaction block accepted");
    if (ifs_sfs_compute_bitmap_layout(512U,100000U,&cap,&count)!=0 || cap!=4000U || count!=25U)
        return fail("bitmap geometry wrong");
    {
        static const unsigned char valid_tail[] = {
            'n', 'a', 'm', 'e', 0, 'c', 'o', 'm', 'm', 'e', 'n', 't', 0, 0
        };
        static const unsigned char no_name_end[] = { 'b', 'a', 'd' };
        static const unsigned char no_comment_end[] = { 'o', 'k', 0, 'x' };
        unsigned char long_name[108];
        ifs_sfs_u32 record_bytes = 0U;
        ifs_sfs_u32 name_bytes = 0U;
        unsigned int index;

        if (ifs_sfs_object_record_layout(
                valid_tail, sizeof(valid_tail), 25U,
                &record_bytes, &name_bytes) != IFS_SFS_OBJECT_RECORD_OK ||
            name_bytes != 4U || record_bytes != 38U)
            return fail("valid object record layout was rejected");

        if (ifs_sfs_object_record_layout(
                no_name_end, sizeof(no_name_end), 25U,
                &record_bytes, &name_bytes) !=
            IFS_SFS_OBJECT_RECORD_NAME_UNTERMINATED)
            return fail("unterminated object name was accepted");

        if (ifs_sfs_object_record_layout(
                no_comment_end, sizeof(no_comment_end), 25U,
                &record_bytes, &name_bytes) !=
            IFS_SFS_OBJECT_RECORD_COMMENT_UNTERMINATED)
            return fail("unterminated object comment was accepted");

        for (index = 0U; index < 106U; index++)
            long_name[index] = 'x';
        long_name[106] = 0;
        long_name[107] = 0;
        if (ifs_sfs_object_record_layout(
                long_name, sizeof(long_name), 25U,
                &record_bytes, &name_bytes) !=
            IFS_SFS_OBJECT_RECORD_NAME_TOO_LONG)
            return fail("oversized object name was accepted");
    }


    if (ifs_sfs_has_allocation_headroom(8U, 1U, 16U) != 0)
        return fail("allocation reserve underflow was accepted");
    if (ifs_sfs_has_allocation_headroom(17U, 1U, 16U) == 0)
        return fail("valid allocation headroom was rejected");
    if (ifs_sfs_has_allocation_headroom(20U, 5U, 16U) != 0)
        return fail("allocation consumed always-free reserve");

    {
        ifs_sfs_u32 mask = 0U;

        if (ifs_sfs_adminspace_block_mask(100U, 100U, &mask) != 0 ||
            mask != 0x80000000U)
            return fail("first admin-space bit mapping is wrong");
        if (ifs_sfs_adminspace_block_mask(100U, 131U, &mask) != 0 ||
            mask != 0x00000001U)
            return fail("last admin-space bit mapping is wrong");
        if (ifs_sfs_adminspace_block_mask(0U, 4U, &mask) == 0)
            return fail("unused admin-space descriptor accepted");
        if (ifs_sfs_adminspace_block_mask(100U, 132U, &mask) == 0)
            return fail("out-of-range admin-space block accepted");
    }


    if (ifs_sfs_bitmap_word_find_set(0x40000000U, 0U) != 1 ||
        ifs_sfs_bitmap_word_find_set(0x40000000U, 2U) != -1)
        return fail("bitmap word MSB ordering is wrong");
    if (ifs_sfs_bitmap_word_find_zero(0xbfffffffU, 0U) != 1)
        return fail("bitmap zero-bit search is wrong");
    if (ifs_sfs_bitmap_word_set(0U, 1U, 3U) != 0x70000000U)
        return fail("bitmap range set is wrong");
    if (ifs_sfs_bitmap_word_clear(0xffffffffU, 30U, 8U) !=
        0xfffffffcU)
        return fail("bitmap range clear/clamp is wrong");


    {
        ifs_sfs_u32 new_free = 0U;

        if (ifs_sfs_free_count_after_allocate(10U, 4U, &new_free) != 0 ||
            new_free != 6U)
            return fail("valid free-block allocation accounting failed");
        if (ifs_sfs_free_count_after_allocate(3U, 4U, &new_free) == 0)
            return fail("free-block allocation underflow was accepted");
        if (ifs_sfs_free_count_after_release(
                90U, 10U, 100U, &new_free) != 0 || new_free != 100U)
            return fail("valid free-block release accounting failed");
        if (ifs_sfs_free_count_after_release(
                95U, 6U, 100U, &new_free) == 0)
            return fail("free-block release overflow was accepted");
    }


    {
        unsigned char block[12] = {
            0x12U, 0x34U, 0x56U, 0x78U,
            0U, 0U, 0U, 0U,
            0U, 0U, 0U, 5U
        };
        ifs_sfs_u32 checksum =
            ifs_sfs_calculate_block_checksum(block, sizeof(block));

        block[4] = (unsigned char)(checksum >> 24);
        block[5] = (unsigned char)(checksum >> 16);
        block[6] = (unsigned char)(checksum >> 8);
        block[7] = (unsigned char)checksum;

        if (ifs_sfs_validate_block_header(
                block, sizeof(block), 5U, 0x12345678U) == 0)
            return fail("valid canonical block checksum was rejected");
        block[11] = 6U;
        if (ifs_sfs_validate_block_header(
                block, sizeof(block), 5U, 0x12345678U) != 0)
            return fail("wrong SFS ownblock was accepted");
    }


    {
        ifs_sfs_u32 slot = 0U;

        if (ifs_sfs_node_leaf_slot(512U, 100U, 100U, &slot) != 0 ||
            slot != 0U)
            return fail("node leaf slot start is wrong");
        if (ifs_sfs_node_leaf_slot(512U, 100U, 148U, &slot) != 0 ||
            slot != 48U)
            return fail("node leaf slot end is wrong");
        if (ifs_sfs_node_leaf_slot(512U, 100U, 149U, &slot) == 0)
            return fail("out-of-range node leaf slot was accepted");
        if (ifs_sfs_node_leaf_slot(512U, 100U, 99U, &slot) == 0)
            return fail("node number before leaf base was accepted");

        if (ifs_sfs_node_index_slot(
                512U, 100U, 50U, 100U, &slot) != 0 || slot != 0U)
            return fail("node index slot start is wrong");
        if (ifs_sfs_node_index_slot(
                512U, 100U, 50U, 6249U, &slot) != 0 || slot != 122U)
            return fail("node index slot end is wrong");
        if (ifs_sfs_node_index_slot(
                512U, 100U, 50U, 6250U, &slot) == 0)
            return fail("out-of-range node index slot was accepted");
        if (ifs_sfs_node_index_slot(
                512U, 100U, 0U, 100U, &slot) == 0)
            return fail("zero node span was accepted");
    }


    {
        ifs_sfs_u32 capacity = 0U;

        if (ifs_sfs_validate_btree_layout(
                512U, 10U, 14U, 1, &capacity) != 0 ||
            capacity != 35U)
            return fail("valid btree extent layout was rejected");
        if (ifs_sfs_validate_btree_layout(
                512U, 62U, 8U, 0, &capacity) != 0 ||
            capacity != 62U)
            return fail("valid btree internal layout was rejected");
        if (ifs_sfs_validate_btree_layout(
                512U, 36U, 14U, 1, &capacity) == 0)
            return fail("btree nodecount overflow was accepted");
        if (ifs_sfs_validate_btree_layout(
                512U, 1U, 12U, 1, &capacity) == 0)
            return fail("short extent btree node was accepted");
        if (ifs_sfs_validate_btree_layout(
                512U, 1U, 7U, 0, &capacity) == 0)
            return fail("odd short internal btree node was accepted");
    }


    if (ifs_sfs_validate_extent(100U, 200U, 10U, 1000U) != 0)
        return fail("valid SFS extent was rejected");
    if (ifs_sfs_validate_extent(100U, 200U, 0U, 1000U) == 0)
        return fail("zero-length SFS extent was accepted");
    if (ifs_sfs_validate_extent(995U, 0U, 10U, 1000U) == 0)
        return fail("out-of-volume SFS extent was accepted");
    if (ifs_sfs_validate_extent(100U, 100U, 10U, 1000U) == 0)
        return fail("self-linked SFS extent was accepted");
    if (ifs_sfs_validate_extent(100U, 1000U, 10U, 1000U) == 0)
        return fail("out-of-volume next extent was accepted");

    {
        ifs_sfs_u32 counter = 0U;

        if (ifs_sfs_adjust_counter(10U, -4, &counter) != 0 ||
            counter != 6U)
            return fail("valid SFS counter decrement failed");
        if (ifs_sfs_adjust_counter(3U, -4, &counter) == 0)
            return fail("SFS counter underflow was accepted");
        if (ifs_sfs_adjust_counter(0xfffffffeU, 1, &counter) != 0 ||
            counter != 0xffffffffU)
            return fail("valid SFS counter increment failed");
        if (ifs_sfs_adjust_counter(0xffffffffU, 1, &counter) == 0)
            return fail("SFS counter overflow was accepted");
    }


    if (ifs_sfs_select_root_copy(1, 4U, 1, 5U) != 1)
        return fail("newer backup root was not selected");
    if (ifs_sfs_select_root_copy(1, 5U, 1, 4U) != 0)
        return fail("newer primary root was not selected");
    if (ifs_sfs_select_root_copy(0, 0U, 1, 1U) != 1)
        return fail("valid backup root was not selected");
    if (ifs_sfs_select_root_copy(1, 1U, 0, 0U) != 0)
        return fail("valid primary root was not selected");
    if (ifs_sfs_select_root_copy(0, 0U, 0, 0U) != -1)
        return fail("invalid root pair was accepted");

    return 0;
}
