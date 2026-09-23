// /***************************************************************************
// Aaru Data Preservation Suite
// ----------------------------------------------------------------------------
//
// Filename       : Super.cs
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
using Aaru.CommonTypes.Structs;
using Aaru.Logging;

namespace Aaru.Filesystems;

/// <inheritdoc />
public sealed partial class SFS
{
    /// <inheritdoc />
    public ErrorNumber StatFs(out FileSystemInfo stat)
    {
        stat = null;

        if(!_mounted) return ErrorNumber.AccessDenied;

        AaruLogging.Debug(MODULE_NAME, "StatFs: returning filesystem statistics");

        stat = new FileSystemInfo
        {
            Blocks         = _rootBlock.totalblocks,
            FreeBlocks     = _rootInfo.freeBlocks,
            FilenameLength = 107, // SFS maximum filename length
            Type           = FS_TYPE,
            Files          = 0, // Not tracked in SFS
            FreeFiles      = 0, // Not tracked in SFS
            PluginId       = Id
        };

        AaruLogging.Debug(MODULE_NAME,
                          "StatFs complete: totalBlocks={0}, freeBlocks={1}, blockSize={2}",
                          stat.Blocks,
                          stat.FreeBlocks,
                          _blockSize);

        return ErrorNumber.NoError;
    }
}

