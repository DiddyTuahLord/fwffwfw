<#
Downloads the foobar2000 SDK and unpacks it into the "sdk" folder of this project.
Usage:  powershell -ExecutionPolicy Bypass -File scripts\get-sdk.ps1
#>
param(
    [string]$Version = '2026-10-01',
    [string]$Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) 'sdk')
)
$ErrorActionPreference = 'Stop'

$marker = Join-Path $Destination 'foobar2000\SDK\foobar2000.h'
if (Test-Path $marker) {
    Write-Host "The SDK is already in $Destination"
    return
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$archive = Join-Path ([IO.Path]::GetTempPath()) "SDK-$Version.7z"
$url = "https://www.foobar2000.org/downloads/SDK-$Version.7z"
$downloaded = $false
try {
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
    $downloaded = $true
} catch {
    Write-Warning "Download from foobar2000.org failed: $($_.Exception.Message)"
}

if ($downloaded) {
    $sevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source
    if (-not $sevenZip) {
        $candidate = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
        if (Test-Path $candidate) { $sevenZip = $candidate }
    }
    if ($sevenZip) {
        & $sevenZip x $archive "-o$Destination" -y | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip could not unpack $archive" }
    } else {
        # Recent Windows 11 builds of tar.exe can read .7z archives.
        tar -xf $archive -C $Destination
        if ($LASTEXITCODE -ne 0) { throw "Could not unpack $archive. Install 7-Zip, or unpack it into $Destination yourself." }
    }
} else {
    # Fallback: an unmodified copy of the SDK kept on GitHub.
    Write-Host 'Cloning the SDK mirror from GitHub instead'
    $temp = Join-Path ([IO.Path]::GetTempPath()) ("fb2k-sdk-" + [guid]::NewGuid())
    git clone --depth 1 https://github.com/reupen/foobar2000-sdk-unmodified.git $temp
    if ($LASTEXITCODE -ne 0) { throw 'Could not get the foobar2000 SDK.' }
    Get-ChildItem $temp -Force | Where-Object { $_.Name -ne '.git' } | Copy-Item -Destination $Destination -Recurse -Force
}

# If the archive wrapped everything in one folder, flatten it.
if (-not (Test-Path $marker)) {
    $inner = Get-ChildItem $Destination -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'foobar2000\SDK\foobar2000.h') } |
        Select-Object -First 1
    if ($inner) {
        Get-ChildItem $inner.FullName -Force | Move-Item -Destination $Destination -Force
        Remove-Item $inner.FullName -Recurse -Force
    }
}
if (-not (Test-Path $marker)) { throw "The SDK did not unpack as expected: $marker is missing." }
Write-Host "The foobar2000 SDK is ready in $Destination"
