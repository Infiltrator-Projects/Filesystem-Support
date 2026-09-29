// /***************************************************************************
// Aaru Data Preservation Suite
// ----------------------------------------------------------------------------
//
// Filename       : Block.cs
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

using Aaru.CommonTypes.Enums;

namespace Aaru.Filesystems;

/// <inheritdoc />
/// <summary>Implements the Smart File System</summary>
public sealed partial class SFS
{
    /// <summary>Reads a filesystem block</summary>
    /// <param name="blockNumber">The block number to read</param>
    /// <param name="data">The block data</param>
    /// <returns>Error code indicating success or failure</returns>
    ErrorNumber ReadBlock(uint blockNumber, out byte[] data)
    {
        data = null;

        // Calculate sectors per block
        uint sectorsPerBlock = _blockSize > 0 ? _blockSize / _imagePlugin.Info.SectorSize : 1;

        if(sectorsPerBlock == 0) sectorsPerBlock = 1;

        // If block size is not yet known (during initial mount), read one sector
        if(_blockSize == 0) return _imagePlugin.ReadSector(_partition.Start + blockNumber, false, out data, out _);

        // Calculate the sector address
        ulong sectorAddress = _partition.Start + (ulong)blockNumber * sectorsPerBlock;

        ErrorNumber errno = _imagePlugin.ReadSectors(sectorAddress,
                                                     false,
                                                     sectorsPerBlock,
                                                     out data,
                                                     out SectorStatus[] _);

        return errno;
    }
}