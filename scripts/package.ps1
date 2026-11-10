<#
Packs the Release builds into build\foo_popout_player.fb2k-component.
foobar2000 expects the 32-bit DLL in the archive root and the 64-bit DLL in an "x64" folder.
Usage:  powershell -ExecutionPolicy Bypass -File scripts\package.ps1
#>
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'

$name = 'foo_popout_player'
$Root = (Resolve-Path $Root).Path
$build = Join-Path $Root 'build'
$x86 = Join-Path $build "Win32\Release\$name.dll"
$x64 = Join-Path $build "x64\Release\$name.dll"
if (-not (Test-Path $x86) -and -not (Test-Path $x64)) {
    throw 'No Release build found. Build the solution in Release (x64 and/or x86) first.'
}

$out = Join-Path $build "$name.fb2k-component"
if (Test-Path $out) { Remove-Item $out -Force }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($out, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    if (Test-Path $x86) { [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $x86, "$name.dll") }
    if (Test-Path $x64) { [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $x64, "x64/$name.dll") }
} finally {
    $zip.Dispose()
}
Write-Host "Created $out"
