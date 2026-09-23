# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'
$stamp = [DateTime]::Now.ToString('yyyyMMdd-HHmmss')
$folder = Join-Path $env:TEMP "ExtFS-Diagnostics-$stamp"
New-Item -ItemType Directory -Path $folder -Force | Out-Null

function Capture-Command {
    param([string]$Name, [scriptblock]$Command)
    try {
        (& $Command 2>&1 | Out-String) | Set-Content -LiteralPath (Join-Path $folder $Name) -Encoding UTF8
    } catch {
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $folder $Name) -Encoding UTF8
    }
}

Capture-Command 'host.txt' {
    [pscustomobject]@{
        ComputerName = $env:COMPUTERNAME
        MachineArchitecture = [Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE', 'Machine')
        OperatingSystem = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).Caption
        Version = [Environment]::OSVersion.Version.ToString()
    } | Format-List
}
Capture-Command 'service.txt' { sc.exe query ExtFS }
Capture-Command 'verifier.txt' { verifier.exe /querysettings }
Capture-Command 'mountvol.txt' { mountvol.exe }
Capture-Command 'driver-file.txt' {
    Get-Item "$env:SystemRoot\System32\drivers\extfs.sys" -ErrorAction Stop |
        Format-List FullName,Length,CreationTimeUtc,LastWriteTimeUtc,VersionInfo
}
Capture-Command 'driver-hash.txt' {
    Get-FileHash -Algorithm SHA256 "$env:SystemRoot\System32\drivers\extfs.sys" -ErrorAction Stop |
        Format-List
}
Capture-Command 'system-events.txt' {
    Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddHours(-24)} -ErrorAction Stop |
        Where-Object { $_.Message -match 'ExtFS|extfs|Service Control Manager|disk|volume|file system' } |
        Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,Message |
        Format-List
}
Capture-Command 'code-integrity-events.txt' {
    Get-WinEvent -FilterHashtable @{
        LogName='Microsoft-Windows-CodeIntegrity/Operational'
        StartTime=(Get-Date).AddHours(-24)
    } -ErrorAction Stop |
        Select-Object TimeCreated,Id,LevelDisplayName,Message |
        Format-List
}
Capture-Command 'dump-files.txt' {
    Get-ChildItem "$env:SystemRoot\MEMORY.DMP", "$env:SystemRoot\Minidump\*.dmp" `
        -ErrorAction SilentlyContinue |
        Select-Object FullName,Length,LastWriteTimeUtc |
        Format-Table -AutoSize
}

$zip = "$folder.zip"
Compress-Archive -LiteralPath $folder -DestinationPath $zip -Force
Write-Host "ExtFS diagnostics collected: $zip"
