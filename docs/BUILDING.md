# Building Phoenix Video Player

The player targets Windows x64 with D3D12. The source repository does not carry
model data, SDK binaries, downloaded media helpers or build output.

## Requirements

- [Git](https://git-scm.com/downloads/win).
- [Visual Studio 2022 or newer / Build Tools](https://visualstudio.microsoft.com/downloads/),
  with Desktop development with C++, MSVC, and a Windows 10/11 SDK.
- [CMake 3.24 or newer](https://cmake.org/download/).
- [PowerShell 7](https://github.com/PowerShell/PowerShell/releases) for pinned media-helper staging.
- [NVIDIA DLSS SDK source](https://github.com/NVIDIA/DLSS), including its Windows x64
  libraries. The development build used revision
  `a291cc7d2cc642a51566f3dfd5376f635cd1b284`.
- Matching [Phoenix SDK source](https://github.com/RazvanManolache/dlss5-phoenix-addon)
  for `native_runtime/phoenix_native.h`, pipeline headers and the retained JSON header.
  A built Phoenix runtime/kernel package is also needed to apply enhancement at runtime.

The Optical Flow and NVENC interface headers included here retain their MIT
notices. SDKs and separately supplied binaries keep their own license terms;
see [THIRD_PARTY.md](../THIRD_PARTY.md).

## Build the player

One possible sibling layout is:

```text
workspace/
  dlss5-phoenix-video-player/
  dlss5-phoenix-addon/
  DLSS/
```

From a Developer PowerShell for Visual Studio, in the player checkout:

```powershell
git clone https://github.com/NVIDIA/DLSS.git ../DLSS
git -C ../DLSS checkout a291cc7d2cc642a51566f3dfd5376f635cd1b284
git clone https://github.com/RazvanManolache/dlss5-phoenix-addon.git ../dlss5-phoenix-addon
git -C ../dlss5-phoenix-addon checkout dfc0fc03b05ec3615b018f11f7bc7e8eee744ef7

cmake -S . -B build/current -G "Visual Studio 17 2022" -A x64 `
  -DDLSS_SDK=../DLSS -DPHOENIX_SOURCE=../dlss5-phoenix-addon `
  -DBUILD_TESTING=OFF
cmake --build build/current --config Release --target DLSSVideoPlayer PhoenixMediaCli
```

Use your installed Visual Studio generator when it differs; `cmake --help`
lists the available generators. Keep `PHOENIX_SOURCE` pointed at source matching
the runtime package you intend to use. The old `vs2022` presets pin one specific
MSVC toolset; the explicit commands above let CMake use your installed toolset.

The output names are `PhoenixVideoPlayer.exe` and `phoenix-media.exe` under
`build/current/Release` for this multi-configuration generator. Ninja builds put
them directly in their build directory.

The normal player build verifies and stages four hash-pinned helpers: FFmpeg,
ffprobe, yt-dlp and Deno. It may download the pinned upstream package; it does
not import that package's model or legacy addon. To supply an existing copy:

```powershell
./tools/Prepare-MediaHelpers.ps1 -DestinationDirectory ./build/current/Release `
  -SourceArchive C:\Downloads\upstream-player.zip
```

The CMake build also stages NVIDIA DLSS feature DLLs from your SDK for inherited
optional comparison paths. These are local build outputs, not repository files.
A complete Phoenix distribution is assembled separately by
`tools/package_player.py`, using a matching validated Phoenix runtime/kernel
bundle. That packager expects the Phoenix source checkout beside this one;
`--stage-everything` additionally expects the Everything checkout and changes
its local `runtime/media` payload. Omit that option for standalone packaging.

## Models and runtime

The compile step does not require a model. Enhancement requires a compatible,
separately supplied model and matching runtime manifest. Everything supplies a
shared model recipe when launching the player. For standalone packaged use:

```powershell
./Configure-PhoenixPlayer.ps1 -PlayerDirectory . `
  -ModelDll C:\Models\nvngx_dlssnr.dll -Model original
```

The script in a source checkout lives under `tools/`; run it against the complete
packaged player directory. It writes pointers and settings, not model copies.
Do not commit generated recipes, model files, personal media, caches or captures.

## Tests and validation

`PHOENIX_PIPELINE_TESTS=ON` builds the direct Phoenix test executables.
`BUILD_TESTING=ON` builds the inherited CTest suites. GPU, compositor, audio and
model checks require suitable local hardware and dependencies; compilation does
not establish that they passed. The scripts under `tests/run_*checks.py` describe
the current explicit integration checks. Generated receipts and captures belong
under ignored `artifacts/current`.

The source publication itself does not rerun GPU/player benchmarks. Historical
development claims in the README describe the original checked environment and
are not fresh-machine or all-GPU qualification.

The 1.0 Beta release uses numeric Windows resource version `1.0.0` and Git tag
`v1.0-beta`. The maintained packager binds this explicitly with
`--release --release-tag v1.0-beta`; it verifies the tag resolves to the packaged commit.
