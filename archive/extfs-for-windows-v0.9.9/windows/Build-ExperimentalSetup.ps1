# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [switch]$SkipCodeAnalysis,
    [switch]$KeepExistingTestCertificate
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$packagesDir = Join-Path $root 'packages'
$release = Join-Path $root 'release\driver'
$driver = Join-Path $release 'extfs.sys'
$inf = Join-Path $release 'extfs.inf'
$catalog = Join-Path $release 'extfs.cat'
$certificateFile = Join-Path $release 'extfs-test.cer'
$buildScript = Join-Path $PSScriptRoot 'Build-And-Validate.ps1'
$packageVersionFile = Join-Path $root 'VERSION'
if (-not (Test-Path -LiteralPath $packageVersionFile)) {
    throw "Package version file is missing: $packageVersionFile"
}
$packageVersion = (Get-Content -LiteralPath $packageVersionFile -Raw).Trim()
if ($packageVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "Invalid package version '$packageVersion' in $packageVersionFile."
}

function Find-WindowsKitTool {
    param([Parameter(Mandatory)][string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    foreach ($searchRoot in @(
        $packagesDir,
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10')
    )) {
        if (-not (Test-Path -LiteralPath $searchRoot)) { continue }
        $candidates = @(Get-ChildItem -LiteralPath $searchRoot -Filter $Name -File -Recurse `
            -ErrorAction SilentlyContinue | Sort-Object FullName -Descending)
        if ($candidates.Count -eq 0) { continue }
        $candidate = $candidates |
            Where-Object FullName -Match '\\x64\\' |
            Select-Object -First 1
        if (-not $candidate) { $candidate = $candidates | Select-Object -First 1 }
        if ($candidate) { return $candidate.FullName }
    }
    throw "$Name was not found in the restored WDK packages or installed Windows Kits."
}

function Find-MakeNSIS {
    $command = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in @(
        (Join-Path $env:ProgramFiles 'NSIS\makensis.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe')
    )) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'makensis.exe was not found. Install NSIS to build the setup executable.'
}

function Invoke-BoundedSignatureInspection {
    param(
        [Parameter(Mandatory)][string]$Tool,
        [Parameter(Mandatory)][string]$File,
        [Parameter(Mandatory)][string]$ExpectedThumbprint,
        [string]$Catalog,
        [ValidateRange(5, 300)][int]$TimeoutSeconds = 45
    )

    $job = Start-Job -ScriptBlock {
        param($Verifier, $Target, $PackageCatalog)
        $signerThumbprint = $null
        if (-not $PackageCatalog) {
            $signature = Get-AuthenticodeSignature -LiteralPath $Target
            if ($signature.SignerCertificate) {
                $signerThumbprint = $signature.SignerCertificate.Thumbprint
            }
        }
        if ($PackageCatalog) {
            $output = @(& $Verifier verify /v /pa /c $PackageCatalog $Target 2>&1)
        } else {
            $output = @(& $Verifier verify /v /pa $Target 2>&1)
        }
        [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output = ($output | Out-String)
            SignerThumbprint = $signerThumbprint
        }
    } -ArgumentList $Tool, $File, $Catalog
    try {
        if (-not (Wait-Job -Job $job -Timeout $TimeoutSeconds)) {
            Stop-Job -Job $job -ErrorAction SilentlyContinue
            throw "Timed out after $TimeoutSeconds seconds inspecting $File."
        }
        $result = Receive-Job -Job $job -Wait -ErrorAction Stop
    } finally {
        Remove-Job -Job $job -Force -ErrorAction SilentlyContinue
    }

    Write-Host $result.Output
    if ($Catalog -and $result.Output -notmatch 'File is signed in catalog:') {
        throw "Catalog membership was not confirmed for $File."
    }
    if (-not $Catalog -and
        $result.SignerThumbprint -ne $ExpectedThumbprint) {
        throw "Signer thumbprint mismatch for $File; found '$($result.SignerThumbprint)'."
    }
    if ($result.ExitCode -eq 0) { return }

    # The package is intentionally self-signed. An untrusted-root result is
    # acceptable only after SignTool has validated the file/signature/catalog
    # far enough to reach chain policy. Every other nonzero result remains fatal.
    $untrustedRoot = (
        $result.Output -match 'terminated in a root' -and
        $result.Output -match 'certificate which is not trusted'
    )
    if (-not $untrustedRoot) {
        throw "Signature inspection failed for $File with exit code $($result.ExitCode)."
    }
    Write-Host "Signature and hash inspection reached the expected self-signed trust boundary for $File."
}

& $buildScript -Configuration Release -Platform $Platform -SkipCodeAnalysis:$SkipCodeAnalysis

$signtool = Find-WindowsKitTool -Name 'signtool.exe'
$inf2cat = Find-WindowsKitTool -Name 'Inf2Cat.exe'
$makensis = Find-MakeNSIS

foreach ($required in @($driver, $inf, $catalog)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required staged driver-package file is missing: $required" }
}

$cert = $null
if ($KeepExistingTestCertificate -and (Test-Path -LiteralPath $certificateFile)) {
    $fileCert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificateFile)
    $cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object Thumbprint -eq $fileCert.Thumbprint |
        Select-Object -First 1
    $fileCert.Dispose()
}
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert `
        -Subject "CN=ExtFS $packageVersion Experimental Test Signing" `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -KeyExportPolicy Exportable -KeyLength 3072 -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(2)
    Export-Certificate -Cert $cert -FilePath $certificateFile -Force | Out-Null
}

$unsignedDriverHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $driver).Hash
Write-Host "Signing the ExtFS driver with test certificate $($cert.Thumbprint)..."
& $signtool sign /v /fd SHA256 /sha1 $cert.Thumbprint /s My $driver
if ($LASTEXITCODE -ne 0) { throw "SignTool signing failed for $driver with exit code $LASTEXITCODE." }
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $driver).Hash -eq $unsignedDriverHash) {
    throw "Signing did not change $driver; refusing to package an unsigned driver."
}

# The catalog must hash the final signed SYS. Regenerate it after embedded
# signing, then sign the catalog itself.
Remove-Item -LiteralPath $catalog -Force
$inf2catOs = if ($Platform -eq 'ARM64') {
    '10_VB_ARM64,10_CO_ARM64,10_NI_ARM64,10_GE_ARM64,10_25H2_ARM64'
} else {
    '10_X64,10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64,10_25H2_X64'
}
& $inf2cat "/driver:$release" "/os:$inf2catOs" /uselocaltime
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat regeneration failed with exit code $LASTEXITCODE." }
if (-not (Test-Path -LiteralPath $catalog)) { throw "Inf2Cat did not regenerate $catalog." }

$unsignedCatalogHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $catalog).Hash
Write-Host "Signing the regenerated ExtFS catalog..."
& $signtool sign /v /fd SHA256 /sha1 $cert.Thumbprint /s My $catalog
if ($LASTEXITCODE -ne 0) { throw "SignTool signing failed for $catalog with exit code $LASTEXITCODE." }
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $catalog).Hash -eq $unsignedCatalogHash) {
    throw "Signing did not change $catalog; refusing to package an unsigned catalog."
}

# Inspect embedded signatures and catalog membership without mutating either
# the user or machine trust store. Each inspection is isolated and bounded.
foreach ($file in @($driver, $catalog)) {
    Invoke-BoundedSignatureInspection -Tool $signtool -File $file `
        -ExpectedThumbprint $cert.Thumbprint
}
Invoke-BoundedSignatureInspection -Tool $signtool -File $driver `
    -Catalog $catalog -ExpectedThumbprint $cert.Thumbprint
Invoke-BoundedSignatureInspection -Tool $signtool -File $inf `
    -Catalog $catalog -ExpectedThumbprint $cert.Thumbprint

$architectureSlug = if ($Platform -eq 'ARM64') { 'arm64' } else { 'x64' }
$setupName = "ExtFS-for-Windows-$packageVersion-experimental-$architectureSlug-setup.exe"
$installerScript = Join-Path $PSScriptRoot 'installer\extfs-installer.nsi'
Push-Location (Split-Path -Parent $installerScript)
try {
    & $makensis "/DTARGET_ARCH=$Platform" "/DPACKAGE_VERSION=$packageVersion" $installerScript
    if ($LASTEXITCODE -ne 0) { throw "NSIS failed with exit code $LASTEXITCODE." }
    $setup = Join-Path (Get-Location) $setupName
    if (-not (Test-Path -LiteralPath $setup)) { throw 'NSIS did not produce the expected setup file.' }
    Copy-Item -LiteralPath $setup -Destination (Join-Path $root (Split-Path $setup -Leaf)) -Force
} finally {
    Pop-Location
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $driver).Hash
$catHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $catalog).Hash
$reportPath = Join-Path $release 'build-report.txt'
if (Test-Path -LiteralPath $reportPath) {
    Add-Content -LiteralPath $reportPath -Encoding UTF8 -Value @(
        "PackageVersion: $packageVersion"
        "FinalSignedDriverSHA256: $hash"
        "FinalSignedCatalogSHA256: $catHash"
        "CatalogMembershipVerified: True"
        "FinalArchitecture: $Platform"
    )
}
Write-Host ''
Write-Host "Experimental package $packageVersion completed."
Write-Host "Signed driver SHA-256: $hash"
Write-Host "Signed catalog SHA-256: $catHash"
Write-Host "Setup: $(Join-Path $root $setupName)"
