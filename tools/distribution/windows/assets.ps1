# Selection policy comes from the same constants used by the package builder.
$policy = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'asset-policy.json') -Raw | ConvertFrom-Json
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Assert-PlainPath([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked paths are not accepted: $Path" }
        }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}

function File-Hash([string]$Path, [string]$Expected = '') {
    Assert-PlainPath $Path
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
    if ($Expected -and $hash -ne $Expected) { throw "Unsupported or damaged file: $Path" }
    return $hash
}

function Import-GameAssets([string]$Source, [string]$Viewer, [string]$Defaults, [string]$Pack) {
    $sourceRoot = [IO.Path]::GetFullPath($Source)
    $dataRoot = Join-Path $sourceRoot 'data'
    $plan = @{}
    $prepared = Test-Path -LiteralPath (Join-Path $sourceRoot 'TM_native.dll')
    $plan['TM_native.dll'] = Join-Path $sourceRoot $(if ($prepared) { 'TM_native.dll' } else { 'TM.dll' })
    $plan['polcore_native.dll'] = if ($prepared) { Join-Path $sourceRoot 'polcore_native.dll' } else { Join-Path $Viewer 'viewer/com/polcore.dll' }
    foreach ($entry in $policy.dll_hashes.PSObject.Properties) { $null = File-Hash $plan[$entry.Name] $entry.Value }
    Assert-PlainPath $dataRoot
    foreach ($file in Get-ChildItem -LiteralPath $dataRoot -Recurse -File) {
        $name = $file.FullName.Substring($dataRoot.TrimEnd('\', '/').Length + 1).Replace('\', '/')
        foreach ($pattern in $policy.original_patterns) {
            if ($name -match $pattern) { $plan["data/$name"] = $file.FullName; break }
        }
    }
    if (-not $prepared) { $plan['data/utf/env.dat'] = Join-Path $Viewer 'data/utf/env.dat' }
    foreach ($name in @('data/gW201.dat', 'data/utf/env.dat')) {
        if (-not $plan.ContainsKey($name)) { throw "Missing original asset: $name" }
    }
    $null = File-Hash $Defaults $policy.default_options_hash
    $plan['pub/home00/option.bin'] = $Defaults
    $records = @()
    if ($Pack) {
        Assert-PlainPath $Pack
        $document = Get-Content -LiteralPath $Pack -Raw | ConvertFrom-Json
        if ($document.version -ne 1 -or @($document.textures).Count -gt 2048) { throw 'Unsupported texture manifest' }
        $packRoot = Split-Path -Parent ([IO.Path]::GetFullPath($Pack))
        foreach ($record in $document.textures) {
            $name = [string]$record.file
            if (-not $name -or $name -match '(^/|\\|:|(^|/)\.\.?(/|$)|//)' -or $name -notmatch '\.png$') { throw "Unsafe texture path: $name" }
            $path = Join-Path $packRoot $name
            $null = File-Hash $path $record.sha256
            $plan["textures-hd/$name"] = $path
            $clean = [ordered]@{}
            foreach ($key in @('id', 'source_sha256', 'sha256', 'file', 'logical_size', 'physical_size')) { $clean[$key] = $record.$key }
            $records += $clean
        }
    }
    # Complete validation and conflict checks before writing any destination file.
    foreach ($entry in $plan.GetEnumerator()) {
        $hash = File-Hash $entry.Value
        $target = Join-Path $runtime $entry.Key
        Assert-PlainPath $target
        if ((Test-Path -LiteralPath $target) -and (File-Hash $target) -ne $hash) { throw "Existing file differs; use a fresh extraction: $target" }
    }
    $stage = Join-Path $clientRoot ('.import-' + [guid]::NewGuid().ToString('N'))
    Assert-PlainPath $stage
    $null = New-Item -ItemType Directory -Path $stage
    try {
        foreach ($entry in $plan.GetEnumerator()) {
            $target = Join-Path $stage $entry.Key
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force
            Copy-Item -LiteralPath $entry.Value -Destination $target
        }
        $packName = 'textures/pack.json'
        if ($Pack) {
            $packName = 'textures-hd/pack.json'
            $text = [ordered]@{version = 1; textures = @($records)} | ConvertTo-Json -Depth 8
            [IO.File]::WriteAllText((Join-Path $stage $packName), $text, $utf8)
        }
        # Wait for this GUI-subsystem executable before using its status or cleaning up.
        & $loader --config $config --runtime-dir $stage --texture-pack (Join-Path $stage $packName) --check | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Imported files did not pass loader validation' }
        if ($Pack -and (Test-Path -LiteralPath (Join-Path $runtime $packName))) {
            # Preserve a previously installed pack; changing it requires a fresh extraction.
            if ((File-Hash (Join-Path $stage $packName)) -ne (File-Hash (Join-Path $runtime $packName))) { throw 'An HD manifest already exists; use a fresh extraction' }
        }
        foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
            $name = $file.FullName.Substring($stage.Length + 1)
            $target = Join-Path $runtime $name
            Assert-PlainPath $target
            if (-not (Test-Path -LiteralPath $target)) {
                $null = New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force
                Move-Item -LiteralPath $file.FullName -Destination $target
            }
        }
        if ($Pack) {
            $text = [IO.File]::ReadAllText($config)
            $text = [regex]::Replace($text, '(?m)^texture-pack=[^\r\n]*', 'texture-pack=textures-hd/pack.json')
            [IO.File]::WriteAllText($config, $text, $utf8)
        }
    } finally {
        $absoluteStage = [IO.Path]::GetFullPath($stage)
        $allowedRoot = [IO.Path]::GetFullPath($clientRoot).TrimEnd('\') + '\'
        if (-not $absoluteStage.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid import cleanup path' }
        Remove-Item -LiteralPath $absoluteStage -Recurse -Force
    }
}
