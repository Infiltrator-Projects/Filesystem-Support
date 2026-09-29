// /***************************************************************************
// Aaru Data Preservation Suite
// ----------------------------------------------------------------------------
//
// Filename       : SFS.cs
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
using System.Collections.Generic;
using System.Text;
using Aaru.CommonTypes.AaruMetadata;
using Aaru.CommonTypes.Interfaces;
using Partition = Aaru.CommonTypes.Partition;

namespace Aaru.Filesystems;

/// <inheritdoc />
/// <summary>Implements the Smart File System</summary>
public sealed partial class SFS : IReadOnlyFilesystem
{
    const string MODULE_NAME = "SFS plugin";

    /// <summary>Cache of root directory entries mapped from filename to object node</summary>
    readonly Dictionary<string, uint> _rootDirectoryCache = new();

    /// <summary>Log2 of block size for shift operations</summary>
    int _blockShift;

    /// <summary>Block size in bytes</summary>
    uint _blockSize;

    /// <summary>Whether the volume is case sensitive</summary>
    bool _caseSensitive;

    /// <summary>The encoding to use for text data</summary>
    Encoding _encoding;

    /// <summary>The media image plugin used to read from the device</summary>
    IMediaImage _imagePlugin;

    /// <summary>Indicates if this is SFS\2 format (version 4)</summary>
    bool _isSfs2;

    /// <summary>Indicates if the filesystem is currently mounted</summary>
    bool _mounted;

    /// <summary>Location of the object node tree root</summary>
    uint _objectNodeRoot;

    /// <summary>Size of the fixed part of an Object structure</summary>
    int _objectSize;

    /// <summary>The partition being mounted</summary>
    Partition _partition;

    /// <summary>The filesystem root block</summary>
    RootBlock _rootBlock;

    /// <summary>The root info structure</summary>
    RootInfo _rootInfo;

    /// <summary>Location of the root object container</summary>
    uint _rootObjectContainer;

    /// <summary>Total number of blocks in the filesystem</summary>
    uint _totalBlocks;

    /// <inheritdoc />
    public FileSystem Metadata { get; private set; }
    /// <inheritdoc />
    public IEnumerable<(string name, Type type, string description)> SupportedOptions { get; } = [];
    /// <inheritdoc />
    public Dictionary<string, string> Namespaces { get; } = [];

#region IFilesystem Members

    /// <inheritdoc />
    public string Name => Localization.SFS_Name;

    /// <inheritdoc />
    public Guid Id => new("26550C19-3671-4A2D-BC2F-F20CEB7F48DC");

    /// <inheritdoc />
    public string Author => Authors.NataliaPortillo;

#endregion
}