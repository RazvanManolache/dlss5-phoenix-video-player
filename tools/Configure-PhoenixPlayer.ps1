[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ModelDll,
    [ValidateSet('original','small-fp8')][string]$Model = 'original',
    [string]$ExecutionDevice = 'native',
    [string]$PlayerDirectory = (Join-Path $PSScriptRoot '../build/current'),
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$player = (Resolve-Path -LiteralPath $PlayerDirectory).Path
$weights = (Resolve-Path -LiteralPath $ModelDll).Path
$manifestPath = Join-Path $player 'phoenix-runtime.json'
$package = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($package.schema -ne 'phoenix-media-runtime/1') { throw 'Unsupported Phoenix runtime manifest.' }
$library = [IO.Path]::GetFullPath([IO.Path]::Combine($player, $package.native_library))
$bundle = [IO.Path]::GetFullPath([IO.Path]::Combine($player, $package.bundle_path))
if (!(Test-Path -LiteralPath $library -PathType Leaf) -or !(Test-Path -LiteralPath $bundle -PathType Container)) {
    throw 'Install the matching Phoenix SDK and kernel bundle first.'
}
$libraryHash = (Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash.ToLowerInvariant()
if ($libraryHash -ne $package.native_library_sha256) { throw 'Phoenix native library checksum differs from the manifest.' }
if ([IO.Path]::GetExtension($weights) -ne '.dll') { throw 'Select the externally installed neural model DLL.' }
$document = [ordered]@{
    schema = 'phoenix-media/1'
    native_library = $library
    native_library_sha256 = $libraryHash
    runtime = [ordered]@{
        bundle_path = $bundle
        weight_source = [ordered]@{ kind = 'nvidia_dll'; path = $weights
            sha256 = (Get-FileHash -LiteralPath $weights -Algorithm SHA256).Hash.ToLowerInvariant(); model = $Model }
    }
    pipeline = [ordered]@{ MotionEstimation = $true; MotionProvider = 0; FlowAnalysisHeight = 180
        SceneDetection = $true; PipelineMonitoring = 0 }
}
if ($package.portable_runtime) {
    $portableRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($player, $package.portable_runtime.root))
    $files = @{}
    foreach ($property in $package.portable_runtime.files.PSObject.Properties) {
        $relative = [string]$property.Name
        if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains('..') -or $relative.Contains(':')) { throw 'Invalid portable dependency path.' }
        $dependency = [IO.Path]::GetFullPath([IO.Path]::Combine($portableRoot, $relative))
        if ((Get-FileHash -LiteralPath $dependency -Algorithm SHA256).Hash.ToLowerInvariant() -ne $property.Value) { throw "Portable dependency checksum differs: $relative" }
        $files[$relative] = $property.Value
    }
    $query = Join-Path $portableRoot 'phoenix_cpp_devices.exe'
    $inventory = & $query $portableRoot | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw 'Native device discovery failed.' }
    $document.execution_devices = @($inventory.devices | Where-Object { !$_.PSObject.Properties['capable'] -or $_.capable })
    $document.portable_package = [ordered]@{ runtime_root = $portableRoot; runtime_sha256 = $files['phoenix_runtime_cpp.dll']; files = $files; allow_unvalidated = $true }
    if ($ExecutionDevice -ne 'native') {
        if (!($document.execution_devices | Where-Object id -eq $ExecutionDevice)) { throw 'Choose a device reported by the native query.' }
        $selection = [ordered]@{}; foreach ($key in $document.portable_package.Keys) { $selection[$key] = $document.portable_package[$key] }
        $selection.device = $ExecutionDevice
        $selection.backend = if ($ExecutionDevice -eq 'cpu') { 'cpu' } else { $ExecutionDevice.Split(':')[0] }
        $document.runtime.portable_runtime = $selection
    }
} elseif ($ExecutionDevice -ne 'native') { throw 'Install the complete portable player package first.' }
$destination = Join-Path $player 'phoenix-media.json'
$text = ($document | ConvertTo-Json -Depth 20) + "`n"
if ((Test-Path -LiteralPath $destination) -and !$Force) {
    if ((Get-Content -LiteralPath $destination -Raw) -eq $text) { Write-Output 'Phoenix player configuration already matches.'; return }
    throw 'A standalone recipe already exists. Use -Force to replace it deliberately.'
}
$temporary = Join-Path $player ('.phoenix-media-' + [Guid]::NewGuid().ToString('N') + '.tmp')
try {
    [IO.File]::WriteAllText($temporary, $text, [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary, $destination, [bool]$Force)
} finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
Write-Output "Phoenix standalone playback configured: $Model. Models stay at their existing path."
