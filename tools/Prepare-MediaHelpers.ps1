[CmdletBinding()]
param(
    [string]$DestinationDirectory = '',
    [string]$SourceArchive = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProgressPreference = 'SilentlyContinue'

# The helper identities and cached package originate from 2600th/dlss5-video-player.
# This staging implementation is Phoenix code; upstream licenses remain intact.
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $DestinationDirectory) { $DestinationDirectory = Join-Path $projectRoot 'build\current' }
$destination = [IO.Path]::GetFullPath($DestinationDirectory)
$lock = Get-Content -LiteralPath (Join-Path $projectRoot 'packaging\tool-lock.json') -Raw | ConvertFrom-Json
if ($lock.schemaVersion -ne 1) { throw 'Unsupported media-helper lock.' }
$names = @('yt-dlp.exe', 'deno.exe', 'ffmpeg.exe', 'ffprobe.exe')
$entries = @($lock.entries | Where-Object { $_.name -in $names })
if ($entries.Count -ne 4 -or @($entries.name | Select-Object -Unique).Count -ne 4) { throw 'Media-helper lock must name exactly four tools.' }

function Assert-PlainPath([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Media-helper path traverses a link: $current" }
        }
        $parent = Split-Path -Parent $current
        if ($parent -eq $current) { break }
        $current = $parent
    }
}
function Test-Tool([string]$Path, $Entry) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    Assert-PlainPath $Path
    if ((Get-Item -LiteralPath $Path).Length -ne $Entry.size) { return $false }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Entry.sha256
}

Assert-PlainPath $destination
[IO.Directory]::CreateDirectory($destination) | Out-Null
$missing = @()
foreach ($entry in $entries) {
    $target = Join-Path $destination $entry.name
    if (Test-Path -LiteralPath $target) {
        if (-not (Test-Tool $target $entry)) { throw "Existing helper differs from the lock and was preserved: $target" }
    } else { $missing += $entry }
}

$stagingParent = Join-Path $projectRoot 'build'
Assert-PlainPath $stagingParent
[IO.Directory]::CreateDirectory($stagingParent) | Out-Null
$staging = Join-Path $stagingParent ('.media-helpers-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($staging) | Out-Null
$published = [Collections.Generic.List[string]]::new()
try {
    if ($missing.Count) {
        $archiveName = 'dlss5-video-player-v0.28.3-win64.zip'
        $archiveSha256 = 'F92F846746CE765890B237163AC56259E23829C67CAB3DB87E4EE05297232D8A'
        if (-not $SourceArchive) {
            $cached = Join-Path (Split-Path -Parent $projectRoot) "dlss5_huggingface\releases\github\2600th\dlss5-video-player\dlss5-video-player-v0.28.3\$archiveName"
            if (Test-Path -LiteralPath $cached -PathType Leaf) { $SourceArchive = $cached }
            else {
                $SourceArchive = Join-Path $staging $archiveName
                Write-Host 'Downloading the pinned media-helper source package...'
                Invoke-WebRequest -Uri "https://github.com/2600th/dlss5-video-player/releases/download/dlss5-video-player-v0.28.3/$archiveName" -OutFile $SourceArchive -TimeoutSec 600 -UseBasicParsing
            }
        }
        if ((Get-FileHash -LiteralPath $SourceArchive -Algorithm SHA256).Hash -ne $archiveSha256) { throw 'Media-helper source archive checksum differs.' }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($SourceArchive))
        try {
            foreach ($entry in $missing) {
                $member = $zip.GetEntry('DLSSVideoPlayer-v0.28.3-upscaling-win64/' + $entry.name)
                if (-not $member -or $member.Length -ne $entry.size) { throw "Archive lacks the locked $($entry.name)." }
                $temporary = Join-Path $staging $entry.name
                [IO.Compression.ZipFileExtensions]::ExtractToFile($member, $temporary)
                if (-not (Test-Tool $temporary $entry)) { throw "Extracted $($entry.name) differs from the lock." }
            }
        } finally { $zip.Dispose() }
        # All new inputs are authenticated before publishing any of them.
        # File.Move without overwrite preserves a concurrently created file.
        foreach ($entry in $missing) {
            $target = Join-Path $destination $entry.name
            Assert-PlainPath $target
            [IO.File]::Move((Join-Path $staging $entry.name), $target)
            $published.Add($target)
        }
    }
    foreach ($entry in $entries) {
        if (-not (Test-Tool (Join-Path $destination $entry.name) $entry)) { throw "Final helper verification failed: $($entry.name)" }
    }
    if (-not $projectRoot.Equals($destination, [StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_LICENSES') -Destination $destination -Recurse -Force
        foreach ($name in @('THIRD_PARTY.md', 'README.md', 'LICENSE')) {
            Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $destination -Force
        }
        [IO.Directory]::CreateDirectory((Join-Path $destination 'third_party')) | Out-Null
        Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party\video-player') -Destination (Join-Path $destination 'third_party') -Recurse -Force
        [IO.Directory]::CreateDirectory((Join-Path $destination 'packaging')) | Out-Null
        Copy-Item -LiteralPath (Join-Path $projectRoot 'packaging\tool-lock.json') -Destination (Join-Path $destination 'packaging\tool-lock.json') -Force
    }
    $receipt = [ordered]@{schema='phoenix-media-helpers/1'; ready=$true; tools=@($entries | ForEach-Object {
        [ordered]@{name=$_.name; bytes=$_.size; sha256=$_.sha256.ToLowerInvariant()}
    })}
    $text = $receipt | ConvertTo-Json -Depth 4
    [IO.File]::WriteAllText((Join-Path $destination 'media-helpers.json'), $text + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    Write-Host "Verified YouTube and media helpers in $destination"
} catch {
    # Remove only tools published by this call, while they still match the lock.
    foreach ($path in $published) {
        $entry = $entries | Where-Object { $_.name -eq [IO.Path]::GetFileName($path) }
        if (Test-Tool $path $entry) { [IO.File]::Delete($path) }
    }
    throw
} finally {
    $resolved = [IO.Path]::GetFullPath($staging)
    $expectedParent = [IO.Path]::GetFullPath($stagingParent)
    if ((Split-Path -Parent $resolved) -ne $expectedParent -or -not ([IO.Path]::GetFileName($resolved)).StartsWith('.media-helpers-')) { throw 'Unsafe helper staging cleanup path.' }
    Assert-PlainPath $resolved
    foreach ($item in Get-ChildItem -LiteralPath $resolved -Recurse -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to clean staging containing a link.' }
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
