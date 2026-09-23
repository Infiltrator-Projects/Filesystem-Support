/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sfs2_core.h"
#include <stdio.h>

static int fail(const char *message)
{
    fprintf(stderr, "sfs2 core test: %s\n", message);
    return 1;
}

int main(void)
{
    ifs_sfs2_u32 high = 0U;
    ifs_sfs2_u16 low = 0U;
    const ifs_sfs2_u64 size = 0x0000123456789ABCULL;

    if (IFS_SFS2_OBJECT_FIXED_SIZE != 27U ||
        IFS_SFS2_EXTENT_NODE_SIZE != 16U)
        return fail("SFS2 structure sizes are wrong");

    if (ifs_sfs2_validate_root_layout(
            IFS_SFS2_ROOT_ID, IFS_SFS2_STRUCTURE_VERSION,
            512U, 100000U, 3U, 4U, 10U, 5U, 6U) != IFS_SFS2_ROOT_OK)
        return fail("valid SFS2 root layout rejected");

    if (ifs_sfs2_validate_root_layout(
            0x53465300U, 3U,
            512U, 100000U, 3U, 4U, 10U, 5U, 6U) == IFS_SFS2_ROOT_OK)
        return fail("SFS0 was accepted as SFS2");

    if (ifs_sfs2_encode_file_size(size, &high, &low) != 0)
        return fail("valid 48-bit file size rejected");

    if (ifs_sfs2_decode_file_size(high, low) != size)
        return fail("48-bit file size roundtrip failed");

    if (ifs_sfs2_encode_file_size(
            IFS_SFS2_MAX_FILE_SIZE + 1U, &high, &low) == 0)
        return fail("oversized SFS2 file size accepted");


    if (ifs_sfs2_select_root_copy(1, 7U, 1, 8U) != 1)
        return fail("newer SFS2 backup root was not selected");
    if (ifs_sfs2_select_root_copy(1, 8U, 1, 7U) != 0)
        return fail("newer SFS2 primary root was not selected");
    if (ifs_sfs2_validate_extent(100U, 200U, 32U, 1000U) != 0)
        return fail("valid SFS2 extent rejected");
    if (ifs_sfs2_validate_extent(990U, 0U, 20U, 1000U) == 0)
        return fail("overrunning SFS2 extent accepted");
    {
        static const unsigned char tail[] = {
            'f','i','l','e',0,'c','o','m','m','e','n','t',0,0
        };
        ifs_sfs2_u32 record_bytes = 0U;
        ifs_sfs2_u32 name_bytes = 0U;
        if (ifs_sfs2_object_record_layout(
                tail, sizeof(tail), &record_bytes, &name_bytes) !=
                IFS_SFS2_OBJECT_RECORD_OK ||
            name_bytes != 4U || record_bytes != 40U)
            return fail("valid SFS2 object record rejected");
    }
    {
        unsigned char block[512] = {0};
        block[0] = 0x53; block[1] = 0x46; block[2] = 0x53; block[3] = 0x02;
        block[8] = 0x00; block[9] = 0x00; block[10] = 0x00; block[11] = 0x05;
        {
            ifs_sfs2_u32 sum = ifs_sfs2_calculate_block_checksum(block, sizeof(block));
            block[4] = (unsigned char)(sum >> 24);
            block[5] = (unsigned char)(sum >> 16);
            block[6] = (unsigned char)(sum >> 8);
            block[7] = (unsigned char)sum;
        }
        if (!ifs_sfs2_validate_block_header(
                block, sizeof(block), 5U, IFS_SFS2_ROOT_ID))
            return fail("valid SFS2 block header rejected");
    }


    {
        unsigned char raw[IFS_SFS2_ROOT_BYTES] = {0};
        IfsSfs2RootRecord root;

        raw[0]=0x53; raw[1]=0x46; raw[2]=0x53; raw[3]=0x02;
        raw[13]=4U;
        raw[15]=9U;
        raw[39]=0x10U;
        raw[47]=0x20U;
        raw[50]=0x10U;
        raw[52]=0x00; raw[53]=0x00; raw[54]=0x02; raw[55]=0x00;
        raw[99]=3U;
        raw[103]=4U;
        raw[107]=10U;
        raw[111]=5U;
        raw[115]=6U;

        if (ifs_sfs2_decode_root(raw, sizeof(raw), &root) != 0)
            return fail("SFS2 root decoder failed");
        if (root.block_id != IFS_SFS2_ROOT_ID ||
            root.version != IFS_SFS2_STRUCTURE_VERSION ||
            root.sequence != 9U ||
            root.first_byte != 0x10U ||
            root.last_byte != 0x20U ||
            root.total_blocks != 4096U ||
            root.block_size != 512U ||
            root.bitmap_base != 3U ||
            root.adminspace_container != 4U ||
            root.root_object_container != 10U ||
            root.extent_bnode_root != 5U ||
            root.object_node_root != 6U)
            return fail("SFS2 root decoder returned wrong fields");
        if (ifs_sfs2_validate_root_record(&root) != 0)
            return fail("decoded SFS2 root rejected");
    }


    {
        ifs_sfs2_u32 blocks_per_bitmap = 0U;
        ifs_sfs2_u32 bitmap_blocks = 0U;

        if (ifs_sfs2_compute_bitmap_layout(
                512U, 100000U,
                &blocks_per_bitmap, &bitmap_blocks) != 0)
            return fail("valid SFS2 bitmap geometry rejected");
        if (blocks_per_bitmap != 4000U || bitmap_blocks != 25U)
            return fail("SFS2 bitmap geometry wrong");

        if (ifs_sfs2_compute_bitmap_layout(
                4096U, 100000U,
                &blocks_per_bitmap, &bitmap_blocks) != 0)
            return fail("valid 4K SFS2 bitmap geometry rejected");
        if (blocks_per_bitmap != 32672U || bitmap_blocks != 4U)
            return fail("4K SFS2 bitmap geometry wrong");

        if (ifs_sfs2_compute_bitmap_layout(
                768U, 100000U,
                &blocks_per_bitmap, &bitmap_blocks) == 0)
            return fail("invalid SFS2 bitmap block size accepted");
    }

    return 0;
}
