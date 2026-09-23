// /***************************************************************************
// Aaru Data Preservation Suite
// ----------------------------------------------------------------------------
//
// Filename       : Internal.cs
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

using Aaru.CommonTypes.Interfaces;

namespace Aaru.Filesystems;

/// <inheritdoc />
/// <summary>Implements the Smart File System</summary>
public sealed partial class SFS
{
    /// <summary>Internal directory node for SFS directory enumeration</summary>
    sealed class SfsDirNode : IDirNode
    {
        /// <summary>Current position in the entries array</summary>
        public int Position { get; set; }

        /// <summary>Array of entry names in this directory</summary>
        public string[] Entries { get; set; }
        /// <summary>The path of this directory</summary>
        public string Path { get; set; }
    }

    /// <summary>Internal file node for SFS file reading with streaming support</summary>
    sealed class SfsFileNode : IFileNode
    {
        /// <summary>First extent block (from object.data field)</summary>
        public uint FirstExtent { get; set; }

        /// <summary>Current extent block being read</summary>
        public uint CurrentExtent { get; set; }

        /// <summary>Offset within the current extent chain</summary>
        public long ExtentOffset { get; set; }
        /// <summary>Current read position in the file</summary>
        public long Offset { get; set; }

        /// <summary>Total file size in bytes</summary>
        public long Length { get; set; }

        /// <summary>Path to the file</summary>
        public string Path { get; set; }
    }
}