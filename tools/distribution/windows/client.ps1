# Windows PowerShell 5.1; no Python or server runtime is needed by the client.
param(
    [ValidateSet('Play', 'Check', 'Import')][string]$Action = 'Check',
    [string]$Source,
    [string]$ViewerDir = "${env:ProgramFiles(x86)}\PlayOnline\SquareEnix\PlayOnlineViewer",
    [string]$ViewerDefaults,
    [string]$TexturePack
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$appRoot = Split-Path -Parent $PSScriptRoot
$clientRoot = Join-Path $appRoot 'client'
$runtime = Join-Path $clientRoot 'runtime'
$loader = Join-Path $clientRoot 'tmloader.exe'
$config = Join-Path $clientRoot 'tmloader.ini'
. (Join-Path $PSScriptRoot 'assets.ps1')

try {
    if ($Action -eq 'Import') {
        if (-not $Source) { $Source = (Read-Host 'Tetra Master game folder or prepared runtime folder').Trim('"') }
        if (-not $ViewerDefaults) { $ViewerDefaults = Join-Path $ViewerDir 'default/pub/home/option.bin' }
        Import-GameAssets $Source $ViewerDir $ViewerDefaults $TexturePack
        Write-Host 'Imported client files with fresh Viewer settings.'
        exit 0
    }
    if (-not (Test-Path -LiteralPath (Join-Path $runtime 'TM_native.dll'))) {
        throw 'Game files are missing. Run Import Game Files.cmd first.'
    }
    # Piping waits for GUI-subsystem executables in Windows PowerShell 5.1.
    & $loader --config $config --check | Out-Host
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if ($Action -eq 'Check') {
        Write-Host 'Setup checks passed. No game launched or server contacted.'
        exit 0
    }
    # Only the extracted app's own copy is labelled for legacy low-integrity writes.
    Assert-PlainPath $runtime
    & icacls $runtime /setintegritylevel '(OI)(CI)L' /Q | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare writable client files. Extract into a folder you own.' }
    foreach ($name in @('pub', 'logs', 'settings')) {
        $folder = Join-Path $runtime $name
        Assert-PlainPath $folder
        if (-not (Test-Path -LiteralPath $folder)) { continue }
        foreach ($item in Get-ChildItem -LiteralPath $folder -Recurse -Force) {
            Assert-PlainPath $item.FullName
            if (-not $item.PSIsContainer) {
                $links = @(& fsutil hardlink list $item.FullName)
                if ($LASTEXITCODE -ne 0 -or $links.Count -ne 1) { throw "Cannot label linked runtime file: $($item.FullName)" }
            }
        }
        & icacls $folder /setintegritylevel '(OI)(CI)L' /T /Q | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Cannot prepare writable folder: $folder" }
    }
    Set-Location -LiteralPath $clientRoot
    & $loader --config $config | Out-Host
    exit $LASTEXITCODE
} catch {
    Write-Host "Could not complete setup: $($_.Exception.Message)"
    exit 1
}
