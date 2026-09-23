// /***************************************************************************
// Aaru Data Preservation Suite
// ----------------------------------------------------------------------------
//
// Filename       : Enums.cs
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

using System;

namespace Aaru.Filesystems;

/// <inheritdoc />
public sealed partial class SFS
{
#region Nested type: Flags

    /// <summary>Root block flags</summary>
    [Flags]
    enum Flags : byte
    {
        /// <summary>Files being deleted will first be moved to the Recycled directory</summary>
        RecycledFolder = 64,
        /// <summary>Filesystem names are treated case insensitive</summary>
        CaseSensitive = 128
    }

#endregion

#region Nested type: ObjectBits

    /// <summary>Object type bits</summary>
    [Flags]
    enum ObjectBits : byte
    {
        /// <summary>Object won't be returned by EXAMINE_NEXT or EXAMINE_ALL</summary>
        Hidden = 1,
        /// <summary>ACTION_DELETE_OBJECT will return an error for this object</summary>
        Undeletable = 2,
        /// <summary>Entries are added at the start of the directory without checking for room elsewhere</summary>
        QuickDir = 4,
        /// <summary>Ring list (partially implemented, not very useful)</summary>
        RingList = 8,
        /// <summary>Object is a hard link</summary>
        HardLink = 32,
        /// <summary>Object is a soft link (when set and HardLink is clear)</summary>
        Link = 64,
        /// <summary>Object is a directory</summary>
        Directory = 128
    }

#endregion
}