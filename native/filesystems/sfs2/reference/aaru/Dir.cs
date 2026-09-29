// /***************************************************************************
// Aaru Data Preservation Suite
// ----------------------------------------------------------------------------
//
// Filename       : Dir.cs
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
using System.Linq;
using Aaru.CommonTypes.Enums;
using Aaru.CommonTypes.Interfaces;
using Aaru.Helpers;
using Aaru.Logging;

namespace Aaru.Filesystems;

/// <inheritdoc />
/// <summary>Implements the Smart File System</summary>
public sealed partial class SFS
{
    /// <inheritdoc />
    public ErrorNumber OpenDir(string path, out IDirNode node)
    {
        node = null;

        if(!_mounted) return ErrorNumber.AccessDenied;

        // Normalize the path
        string normalizedPath = path ?? "/";

        if(normalizedPath == "" || normalizedPath == ".") normalizedPath = "/";

        // Root directory - return cached entries
        if(normalizedPath == "/" || string.Equals(normalizedPath, "/", StringComparison.OrdinalIgnoreCase))
        {
            if(_rootDirectoryCache.Count == 0) return ErrorNumber.NoSuchFile;

            node = new SfsDirNode
            {
                Path     = "/",
                Position = 0,
                Entries  = _rootDirectoryCache.Keys.ToArray()
            };

            return ErrorNumber.NoError;
        }

        // Subdirectory traversal
        string pathWithoutLeadingSlash = normalizedPath.StartsWith("/", StringComparison.Ordinal)
                                             ? normalizedPath[1..]
                                             : normalizedPath;

        string[] pathComponents = pathWithoutLeadingSlash.Split('/', StringSplitOptions.RemoveEmptyEntries);

        if(pathComponents.Length == 0) return ErrorNumber.InvalidArgument;

        AaruLogging.Debug(MODULE_NAME, "OpenDir: Traversing path with {0} components", pathComponents.Length);

        // Start from root directory cache
        Dictionary<string, uint> currentEntries = _rootDirectoryCache;

        // Traverse each path component
        foreach(string component in pathComponents)
        {
            AaruLogging.Debug(MODULE_NAME, "OpenDir: Navigating to component '{0}'", component);

            // Find the component in current directory (handle case sensitivity)
            string foundKey = null;

            foreach(string key in currentEntries.Keys)
            {
                if(string.Equals(key,
                                 component,
                                 _caseSensitive ? StringComparison.Ordinal : StringComparison.OrdinalIgnoreCase))
                {
                    foundKey = key;

                    break;
                }
            }

            if(foundKey == null)
            {
                AaruLogging.Debug(MODULE_NAME, "OpenDir: Component '{0}' not found in directory", component);

                return ErrorNumber.NoSuchFile;
            }

            uint objectNode = currentEntries[foundKey];

            AaruLogging.Debug(MODULE_NAME, "OpenDir: Component '{0}' found with node {1}", component, objectNode);

            // Read the object to check if it's a directory and get its contents
            ErrorNumber errno = ReadDirectoryContents(objectNode, out Dictionary<string, uint> childEntries);

            if(errno != ErrorNumber.NoError)
            {
                AaruLogging.Debug(MODULE_NAME, "OpenDir: Error reading directory contents: {0}", errno);

                return errno;
            }

            currentEntries = childEntries;
        }

        // Create directory node with the entries we found
        node = new SfsDirNode
        {
            Path     = normalizedPath,
            Position = 0,
            Entries  = currentEntries.Keys.ToArray()
        };

        AaruLogging.Debug(MODULE_NAME,
                          "OpenDir: Successfully opened directory '{0}' with {1} entries",
                          normalizedPath,
                          currentEntries.Count);

        return ErrorNumber.NoError;
    }

    /// <inheritdoc />
    public ErrorNumber CloseDir(IDirNode node)
    {
        if(node is not SfsDirNode sfsNode) return ErrorNumber.InvalidArgument;

        sfsNode.Position = -1;
        sfsNode.Entries  = null;

        return ErrorNumber.NoError;
    }

    /// <inheritdoc />
    public ErrorNumber ReadDir(IDirNode node, out string filename)
    {
        filename = null;

        if(!_mounted) return ErrorNumber.AccessDenied;

        if(node is not SfsDirNode sfsNode) return ErrorNumber.InvalidArgument;

        if(sfsNode.Position < 0) return ErrorNumber.InvalidArgument;

        if(sfsNode.Position >= sfsNode.Entries.Length) return ErrorNumber.NoError;

        filename = sfsNode.Entries[sfsNode.Position++];

        return ErrorNumber.NoError;
    }

    /// <summary>Reads the contents of a directory given its object node number</summary>
    /// <param name="objectNode">The object node number of the directory</param>
    /// <param name="entries">Output dictionary of entries (name -> object node)</param>
    /// <returns>Error code indicating success or failure</returns>
    ErrorNumber ReadDirectoryContents(uint objectNode, out Dictionary<string, uint> entries)
    {
        entries = new Dictionary<string, uint>();

        // Find the object container for this node
        ErrorNumber errno = FindObjectNode(objectNode, out uint objectBlock);

        if(errno != ErrorNumber.NoError) return errno;

        errno = ReadBlock(objectBlock, out byte[] objectData);

        if(errno != ErrorNumber.NoError) return errno;

        // Find the object in the container
        errno = FindObjectInContainer(objectData, objectNode, out int objectOffset);

        if(errno != ErrorNumber.NoError) return errno;

        // Check if this is a directory
        // SFS\0: bits at offset 24, SFS\2: bits at offset 26
        int  bitsOffset = _isSfs2 ? 26 : 24;
        byte bits       = objectData[objectOffset + bitsOffset];

        if((bits & (byte)ObjectBits.Directory) == 0)
        {
            AaruLogging.Debug(MODULE_NAME, "Object {0} is not a directory", objectNode);

            return ErrorNumber.NotDirectory;
        }

        // Read first directory block pointer from the object
        // For directories: hashtable is at offset 12, firstdirblock is at offset 16
        int firstDirBlockOffset = objectOffset + 16;
        var firstDirBlock       = BigEndianBitConverter.ToUInt32(objectData, firstDirBlockOffset);

        AaruLogging.Debug(MODULE_NAME, "Directory first block: {0}", firstDirBlock);

        // Traverse the directory chain
        uint nextBlock = firstDirBlock;

        while(nextBlock != 0)
        {
            errno = ReadBlock(nextBlock, out byte[] dirBlockData);

            if(errno != ErrorNumber.NoError) return errno;

            // Validate block
            var blockId = BigEndianBitConverter.ToUInt32(dirBlockData, 0);

            if(blockId != OBJECTCONTAINER_ID)
            {
                AaruLogging.Debug(MODULE_NAME, "Invalid block ID in directory block: 0x{0:X8}", blockId);

                break;
            }

            // Parse objects in this container
            ParseObjectContainerToDict(dirBlockData, entries);

            // Get next block in chain
            // ObjectContainer: header(12) + parent(4) + next(4) + previous(4)
            nextBlock = BigEndianBitConverter.ToUInt32(dirBlockData, 16);
        }

        return ErrorNumber.NoError;
    }
}