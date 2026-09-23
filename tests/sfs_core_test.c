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

    return 0;
}
