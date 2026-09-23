# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z]:?$')]
    [string]$DriveLetter,
    [string]$KnownFile,
    [switch]$ExerciseInPlaceWrite,
    [switch]$ExerciseResize,
    [ValidateRange(1, 1048576)]
    [int]$ResizeDeltaBytes = 1024,
    [ValidateRange(1, 100000)]
    [int]$OpenCloseIterations = 250
)

$ErrorActionPreference = 'Stop'
$drive = $DriveLetter.TrimEnd(':').ToUpperInvariant()
$root = "$drive`:\"
if (-not (Test-Path -LiteralPath $root)) { throw "Drive $root is not mounted." }

$results = [ordered]@{
    Date = [DateTime]::Now.ToString('o')
    Drive = $root
    RootEnumeration = $false
    ReadFile = $null
    ReadBytes = 0
    CopyOut = $false
    OpenCloseIterations = $OpenCloseIterations
    OpenCloseStress = $null
    InPlaceWrite = $null
    InPlaceWriteRestored = $null
    AppendRoundTrip = $null
    ResizeRoundTrip = $null
    OriginalLength = $null
    FinalLength = $null
    ContentRestored = $null
    WriteFileRefused = $false
    CreateDirectoryRefused = $false
    StillReadableAfterWriteProbes = $false
}

Write-Host "Enumerating $root ..."
$entries = @(Get-ChildItem -LiteralPath $root -Force -ErrorAction Stop)
$results.RootEnumeration = $true
Write-Host "Root entries: $($entries.Count)"

$file = $null
if ($KnownFile) {
    $candidate = if ([IO.Path]::IsPathRooted($KnownFile)) {
        $KnownFile
    } else {
        Join-Path $root $KnownFile
    }
    $file = Get-Item -LiteralPath $candidate -Force -ErrorAction Stop
    if ($file.PSIsContainer) { throw "$candidate is a directory, not a file." }
} else {
    $file = $entries | Where-Object { -not $_.PSIsContainer } | Select-Object -First 1
}

$originalHash = $null
if ($file) {
    $results.OriginalLength = [Int64]$file.Length
    $originalHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash

    $stream = [IO.File]::Open($file.FullName, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $buffer = New-Object byte[] 65536
        $count = $stream.Read($buffer, 0, $buffer.Length)
        $results.ReadFile = $file.FullName
        $results.ReadBytes = $count
    } finally {
        $stream.Dispose()
    }

    $copyDir = Join-Path $env:TEMP 'ExtFS-Qualification'
    New-Item -ItemType Directory -Path $copyDir -Force | Out-Null
    $copyPath = Join-Path $copyDir $file.Name
    Copy-Item -LiteralPath $file.FullName -Destination $copyPath -Force -ErrorAction Stop
    if ((Get-Item -LiteralPath $copyPath).Length -ne $file.Length) {
        throw 'The copied file length does not match the source length.'
    }
    $results.CopyOut = $true

    # Repeatedly create and close FILE_OBJECTs for the same inode. This is a
    # practical regression probe for FCB reference/reclamation mistakes.
    for ($iteration = 0; $iteration -lt $OpenCloseIterations; ++$iteration) {
        $probe = [IO.File]::Open($file.FullName, [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            if ($probe.Length -gt 0) { [void]$probe.ReadByte() }
        } finally {
            $probe.Dispose()
        }
    }
    $results.OpenCloseStress = $true

    if ($ExerciseInPlaceWrite) {
        if ($file.Length -lt 8) { throw 'Known file must be at least 8 bytes for the in-place write test.' }
        $rw = [IO.File]::Open($file.FullName, [IO.FileMode]::Open,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
        $original = New-Object byte[] 8
        $haveOriginal = $false
        try {
            if ($rw.Read($original, 0, 8) -ne 8) { throw 'Could not read write-test prefix.' }
            $haveOriginal = $true
            $marker = [Text.Encoding]::ASCII.GetBytes('EXTFS093')
            $rw.Position = 0
            $rw.Write($marker, 0, $marker.Length)
            $rw.Flush()
            $rw.Position = 0
            $check = New-Object byte[] 8
            if ($rw.Read($check, 0, 8) -ne 8 -or
                [Convert]::ToBase64String($marker) -ne [Convert]::ToBase64String($check)) {
                throw 'In-place write verification failed.'
            }
            $results.InPlaceWrite = $true
        } finally {
            if ($haveOriginal) {
                $rw.Position = 0
                $rw.Write($original, 0, $original.Length)
                $rw.Flush()
                $rw.Position = 0
                $restored = New-Object byte[] 8
                if ($rw.Read($restored, 0, 8) -ne 8 -or
                    [Convert]::ToBase64String($original) -ne [Convert]::ToBase64String($restored)) {
                    $results.InPlaceWriteRestored = $false
                    $rw.Dispose()
                    throw 'Original bytes were not restored after the write test.'
                }
                $results.InPlaceWriteRestored = $true
            }
            $rw.Dispose()
        }
    }

    if ($ExerciseResize) {
        $originalLength = [Int64]$file.Length
        if ($originalLength -gt [Int64]::MaxValue - $ResizeDeltaBytes - 8) {
            throw 'Known file is too large for the reversible resize qualification probe.'
        }

        try {
            # FileMode.Append requests append semantics from Windows. The marker
            # is verified at the new EOF and then removed by restoring old EOF.
            $marker = [Text.Encoding]::ASCII.GetBytes('EXTFS093')
            $append = [IO.File]::Open($file.FullName, [IO.FileMode]::Append,
                [IO.FileAccess]::Write, [IO.FileShare]::Read)
            try {
                $append.Write($marker, 0, $marker.Length)
                $append.Flush()
            } finally {
                $append.Dispose()
            }

            $verifyAppend = [IO.File]::Open($file.FullName, [IO.FileMode]::Open,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
            try {
                if ($verifyAppend.Length -ne $originalLength + $marker.Length) {
                    throw 'Append qualification produced the wrong EOF.'
                }
                $verifyAppend.Position = $originalLength
                $tail = New-Object byte[] $marker.Length
                if ($verifyAppend.Read($tail, 0, $tail.Length) -ne $tail.Length -or
                    [Convert]::ToBase64String($tail) -ne [Convert]::ToBase64String($marker)) {
                    throw 'Append qualification marker could not be read back.'
                }
                $verifyAppend.SetLength($originalLength)
                $verifyAppend.Flush()
            } finally {
                $verifyAppend.Dispose()
            }
            $results.AppendRoundTrip = $true

            # Exercise FileEndOfFileInformation growth and shrink without
            # changing existing bytes. Newly exposed bytes must read as zero.
            $rw = [IO.File]::Open($file.FullName, [IO.FileMode]::Open,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
            try {
                $grownLength = $originalLength + $ResizeDeltaBytes
                $rw.SetLength($grownLength)
                $rw.Flush()
                if ($rw.Length -ne $grownLength) { throw 'EOF growth did not persist.' }
                $rw.Position = $originalLength
                $zeroProbe = New-Object byte[] $ResizeDeltaBytes
                if ($rw.Read($zeroProbe, 0, $zeroProbe.Length) -ne $zeroProbe.Length) {
                    throw 'Could not read the newly exposed growth range.'
                }
                foreach ($b in $zeroProbe) {
                    if ($b -ne 0) { throw 'Newly exposed EOF growth bytes were not zero-filled.' }
                }
                $rw.SetLength($originalLength)
                $rw.Flush()
                if ($rw.Length -ne $originalLength) { throw 'EOF shrink did not restore the original length.' }
            } finally {
                $rw.Dispose()
            }
            $results.ResizeRoundTrip = $true
        } finally {
            # If an assertion or I/O failure occurs after append/growth, make a
            # best-effort attempt to restore the original EOF. A cleanup error
            # must not hide the original qualification failure; if the probe
            # otherwise succeeds, the final length/hash check below still makes
            # incomplete restoration fatal.
            try {
                $restore = [IO.File]::Open($file.FullName, [IO.FileMode]::Open,
                    [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
                try {
                    if ($restore.Length -ne $originalLength) {
                        $restore.SetLength($originalLength)
                        $restore.Flush()
                    }
                } finally {
                    $restore.Dispose()
                }
            } catch {
                Write-Warning "Could not restore the original EOF after the resize probe: $_"
            }
        }
    }

    $finalFile = Get-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
    $results.FinalLength = [Int64]$finalFile.Length
    if ($ExerciseInPlaceWrite -or $ExerciseResize) {
        $finalHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
        if ($finalFile.Length -ne $results.OriginalLength -or $finalHash -ne $originalHash) {
            throw 'Qualification probes did not restore the known file byte-for-byte.'
        }
        $results.ContentRestored = $true
    }
}

if (($ExerciseInPlaceWrite -or $ExerciseResize) -and -not $file) {
    throw 'Write/resize qualification requires -KnownFile or a root-level regular file.'
}

$probeFile = Join-Path $root '__extfs_write_probe.tmp'
try {
    [IO.File]::WriteAllText($probeFile, 'ExtFS write probe')
    Remove-Item -LiteralPath $probeFile -Force -ErrorAction SilentlyContinue
    throw 'WRITE-SAFETY FAILURE: creating a file on the ExtFS volume unexpectedly succeeded.'
} catch {
    if ($_.Exception.Message -like 'WRITE-SAFETY FAILURE:*') { throw }
    $results.WriteFileRefused = $true
}

$probeDirectory = Join-Path $root '__extfs_directory_probe'
try {
    [IO.Directory]::CreateDirectory($probeDirectory) | Out-Null
    Remove-Item -LiteralPath $probeDirectory -Force -Recurse -ErrorAction SilentlyContinue
    throw 'WRITE-SAFETY FAILURE: creating a directory on the ExtFS volume unexpectedly succeeded.'
} catch {
    if ($_.Exception.Message -like 'WRITE-SAFETY FAILURE:*') { throw }
    $results.CreateDirectoryRefused = $true
}

@(Get-ChildItem -LiteralPath $root -Force -ErrorAction Stop) | Out-Null
$results.StillReadableAfterWriteProbes = $true

$report = Join-Path $env:TEMP "ExtFS-qualification-$drive-$([DateTime]::Now.ToString('yyyyMMdd-HHmmss')).json"
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $report -Encoding UTF8

Write-Host ''
Write-Host 'ExtFS 0.9.3 qualification probe passed.'
Write-Host "Report: $report"
$results | Format-List | Out-Host
