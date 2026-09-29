// /***************************************************************************
// Aaru Data Preservation Suite
// ----------------------------------------------------------------------------
//
// Filename       : Helpers.cs
// Author(s)      : Natalia Portillo <claunia@claunia.com>
//
// Component      : SmartFileSystem plugin.
//
// --[ License ] --------------------------------------------------------------
//
//     This library is free software; you can redistribute it and/or modify
//     it under the terms of the GNU Lesser General Public License as
//     published by the Free Software Foundation; either version 2.1 of the
//     License, or (at your option) any later version.
//
//     This library is distributed in the hope that it will be useful, but
//     WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
//     Lesser General Public License for more details.
//
//     You should have received a copy of the GNU Lesser General Public
//     License along with this library; if not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------
// Copyright © 2011-2026 Natalia Portillo
// ****************************************************************************/

using Aaru.Helpers;

namespace Aaru.Filesystems;

/// <inheritdoc />
/// <summary>Implements the Smart File System</summary>
public sealed partial class SFS
{
    /// <summary>Validates the checksum of a block</summary>
    /// <param name="blockData">The block data to validate</param>
    /// <returns>True if the checksum is valid, false otherwise</returns>
    static bool ValidateChecksum(byte[] blockData)
    {
        // The checksum is validated by summing all 32-bit big-endian words in the block.
        // Starting with sum=1, if the result is 0, the checksum is valid.
        // Some implementations may result in -1 (0xFFFFFFFF), so we accept that too.
        uint sum = 1;

        for(var i = 0; i < blockData.Length; i += 4)
        {
            var value = BigEndianBitConverter.ToUInt32(blockData, i);
            sum += value;
        }

        // Accept 0 or -1 as valid
        return sum is 0 or 0xFFFFFFFF;
    }

    /// <summary>Calculates the block shift (log2 of block size)</summary>
    /// <param name="blockSize">The block size in bytes</param>
    /// <returns>The shift value</returns>
    static int CalculateBlockShift(uint blockSize)
    {
        var shift = 0;

        while(1u << shift < blockSize) shift++;

        return shift;
    }
}