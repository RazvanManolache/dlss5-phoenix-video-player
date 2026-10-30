# Phoenix Video Player

[Download Windows x64 v1.0-beta](https://github.com/RazvanManolache/dlss5-phoenix-video-player/releases/tag/v1.0-beta) Â· [Build instructions](docs/BUILDING.md)

Native Windows player sharing Phoenix models and processing with DLSS Everything.

Your movies have acquired a few more knobs. Use responsibly. Or at least keep
the original view within reach.

- Original and Lightweight models through the direct Phoenix native SDK.
- Live strength, skin, style, color, layer and processing-budget controls.
- Matching-frame wipe, difference, side-by-side, zoom and loupe comparisons.
- Optional scaling, scene detection, motion estimation and experimental fusion.
- Resumable media jobs, authenticated caches and shared recipes with
  [DLSS5 Everything](https://github.com/RazvanManolache/DLSS5-Everything).

**Windows x64 / D3D12.** This repository contains source, tests and packaging
recipes. NVIDIA model DLLs and weights remain external. Start with the
[build instructions](docs/BUILDING.md); Phoenix's SDK is a separate dependency.

The source dependency is pinned to **Phoenix 1.0 Beta** in `dependencies.lock.json`.
Release packages include the native library and model-free kernels; supply a
compatible model separately.

The direct Phoenix adapter now sends the neural dialog's strength, tone,
structure, skin mask, style, color strength and network passes to the native
runtime. Changes affect subsequent source frames; a paused preview uses the
same direct renderer. Exports snapshot the applied recipe. Each Phoenix pass
owns independent history; the legacy chained-history switch is disabled for
this route. Everything also authors ordered stages, fusion branches, per-block
repeats and processing budgets in the shared recipe. The processing-device
selector offers native RTX and installed CPU/CUDA/Vulkan devices. Backend
replacement is transactional and retains the working session on failure.
All use the same native stage graph; every pass/branch owns its neural history.
See the addon's `docs/PIPELINE_IMPLEMENTATION.md` for recorded evidence and limits.

Local pipeline measurements cover both Phoenix models, fusion, block counts, multiple passes, optical flow
and the optional scalers. Timings include host transfers. Fusion can soften
detail; the current tiny scaler has pronounced artifacts and a high processing
cost on this example. Both remain explicit experimental choices.
Generated measurements and captures stay under ignored `artifacts/current`;
they are not published as evidence by this source snapshot.

The native player/media/cache foundation is adapted from
[2600th/dlss5-video-player](https://github.com/2600th/dlss5-video-player) under MIT.
This is substantial source reuse, including the player UI, comparison renderer,
subtitles and media/codec orchestration. Upstream deserves credit for that work.
Original project additions use the checked-in [Apache-2.0 license](LICENSE);
imported source and external components retain their own terms. See
[credits and notices](THIRD_PARTY.md). No model DLLs or weights are included in
the source import.

Live cache attachment prepares its decoder, synchronized pair and renderer
before changing playback. A failed setup preserves the active picture, queued
frame and audio; successful attachment retains the matching original reference
and displays cached output without running the model again. Decoder swaps also
retain their requested comparison geometry and optional timing mode. Prepared
network decoders timestamp their first frame when monitoring was requested.
Run `python tests/run_live_playback_checks.py` after building
`PhoenixLivePlaybackTests`. Its disposable local clips check BGRA/NV12 at 1x/2x,
eight rollback paths and eight independent compositor readbacks, without desktop
automation or an audio fixture. Packaging requires the resulting receipt to
match the current build.

Direct playback runs Original or Lightweight through the native Phoenix SDK,
using native RTX or a selected native portable worker. Portable inference retains
host readback/upload costs; these routes do not inherit the native RTX timings.
Motion estimation requires NVIDIA optical-flow support on the processing-stage
device and falls back explicitly when unavailable or below the driver's admitted
dimensions. It never supplies invented depth. Scene detection, independent neural
and output dimensions, NIS/tiny scaling, fusion and source-age policy remain in
the shared D3D12 stage graph. CPU and NVIDIA/AMD Vulkan inference were checked
with D3D12 presentation on the 5080; AMD-only presentation is unverified.
Standalone launches resolve `PHOENIX_MEDIA_RECIPE` first, then
`phoenix-media.json` beside the EXE, then Everything's last saved
`%LOCALAPPDATA%/DLSS5-Everything/Media/recipe.json`. The selected recipe is
held for the session, including renderer recreation. Invalid selected recipes
fail explicitly rather than switching models or silently displaying DLAA.
Everything publishes that shared recipe when launching the player. A developer
build can be configured once with `tools/Configure-PhoenixPlayer.ps1 -ModelDll
C:\Models\nvngx_dlssnr.dll -Model original` (or `small-fp8`), using its matching
`phoenix-runtime.json`. Add `-ExecutionDevice cpu` or an ID reported by the
packaged native device query to choose another backend. This writes a small pointer recipe; models stay at their
existing location. The toolbar and status identify direct Phoenix playback and
the model instead of reporting the legacy addon's cache state.

Original/processed, wipe, difference, side-by-side, quad, zoom and loupe use the
same completed frame; presentation changes do not evaluate the model again.
Comparison PNGs can carry a `phoenix-screenshot/1` receipt with the image hash,
completed source identity, model/runtime identity, recipe and view settings.

The experimental native media job writes authenticated segments and resumes by
replaying preceding frames to reconstruct temporal history. It processes every
decoded frame in order, disables interactive quality adaptation, verifies the
assembled frame count and preserves existing outputs. The player export path
uses this job when launched with a direct Phoenix recipe; its final mux carries
the source audio, embedded subtitles, chapters and metadata into the chosen
container. External playback subtitles remain presentation overlays.

Player range/cache jobs use that native route when a Phoenix recipe is selected.
Prepared opens look up an authenticated completed carrier without rendering a
missing one. Cache hits verify the recipe, runtime/model identity, segments and
assembled carrier; modified files are preserved and refused. Cached comparisons
scale the original to the carrier's geometry and bypass neural processing,
including the first frame. Native receipts remain separate from the imported
worker's Feature18 evidence. Completed segments are also published during live
jobs and republished after authentication on resume. Live comparisons admit the
carrier's output geometry, including 2x. Native entries participate in cache
clear and space-pressure eviction; file leases preserve running producers and
readers in other player instances. Unknown files and links are preserved.

Optional monitoring shows SDK time and decoded-frame age in the status line.
Receipts also expose presentation deadline lateness and SDK-to-Present time.
The shared controller includes eligible-source queue delay and the preceding
presentation tail. Deliberate decode prefetch and pauses are excluded from its
budget; a paused redraw does not change that tail sample. These measurements
end at the Present API return and do not measure physical display scanout or
capture/decode time before the decoded frame became available. Monitoring off
disables the added decoder timestamps and this telemetry.

The native command-line entry point is `phoenix-media.exe`:

```text
phoenix-media --recipe C:\Media\recipe.json --input C:\Media\clip.mkv --output C:\Media\new.mkv --cache C:\Media\cache --helpers C:\Tools\ffmpeg
```

Normal player builds also verify and stage `yt-dlp.exe`, `deno.exe`,
`ffmpeg.exe` and `ffprobe.exe` beside the application. These are needed for the
YouTube trailers and media decoding/export. The staging tool reuses the cached
upstream package when available, otherwise downloads its pinned release archive;
all four files must match `packaging/tool-lock.json` before they are installed.
The original helper licenses/notices accompany them.

To repair missing helpers in an existing build without reinstalling the player:

```powershell
./tools/Prepare-MediaHelpers.ps1 -DestinationDirectory ./build/current
```

`-SourceArchive` accepts a local copy of the pinned upstream release ZIP.
Existing helpers with different identities are preserved and reported. SDK-only
builds may use `-DPHOENIX_MEDIA_HELPERS=OFF`; that option does not produce a
complete player package. The shared pipeline still requires its separately
installed Phoenix runtime/model recipe for enhancement.

This command writes a video-only Matroska carrier. FFmpeg/ffprobe are native
helper executables. No Python runtime is required. Current job support is SDR
constant-frame-rate video with software encoding; arbitrary admitted frame
dimensions are supported. Direct SDR/HDR playback, the native processing dialog,
ordered stages, fusion, per-block controls, matching-launch preferences and
startup recovery have native fixture checks. HDR media export remains outside
the current SDR job route. VSR and OptiScaler integration are deferred.

`python tools/package_player.py --stage-everything --bundle
../dlss5-phoenix-addon/build/native_arch_compat_final` creates the current
model-free `artifacts/current/PhoenixVideoPlayer-Windows-x64.zip` and stages the
companion into Everything's build payload. It checks the built player/SDK
against their validation receipts, kernel and helper hashes, archive contents
and notices. The package carries relative SDK/kernel paths and requires no
developer checkout. Everything supplies the external central model pointer;
standalone users can run the included configuration script. RTX 20/30/40/50
kernel variants are included; only RTX 5080 execution was checked here.
`tests/run_package_checks.py --gpu-models` compares packaged chain/fusion output
against the validated SDK for both models. Its independent fusion check renders
full-size and half-size predictions separately and compares their restored
average with the chain result. All branches receive the same source pixels;
standalone fusion also evaluates its full-size prediction.

Development checks: `python tests/run_phoenix_checks.py --gpu-models` after
building the integration test targets. This development script generates small
temporary video fixtures and checks playback identity, comparisons, interrupted
jobs, replayed history, changed-cache refusal and decoded-pixel equality between
resumed and uninterrupted exports. Results live in
`artifacts/current/phoenix-integration-validation.json`; these fixtures do not
establish NVIDIA parity, gameplay performance or older-GPU compatibility.

The comparison check uses detailed patterns in BGRA and NV12 through the window
compositor. For neural recipes it requires a measured change from the original,
then verifies that Split and Wipe select each independently captured pair member
on the correct side. Its original-view oracle checks the submitted source as
well. Changing views must not increase the model evaluation count. Playback logs
report direct Phoenix activity, completed neural evaluations, reference presence,
comparison mode and mix separately from background cache rendering.
Run `python tests/run_comparison_checks.py` after building `PhoenixPipelineTests`.
Its receipt is `artifacts/current/comparison-validation.json`. An optional public
video can be sampled without desktop control using `--youtube-url URL --seek 43`
(also build `PhoenixYouTubeProbe`). This uses the production resolver/decoder,
then checks that decoded frame through NV12 and a smaller presentation target.
The seven native compositor readbacks are saved under
`artifacts/current/split-comparison/trailer`: `0.png` processed, `1.png` original,
`3.png` split, `4.png` wipe, `5.png` amplified difference, `6.png` side by side,
`7.png` quad. The receipt includes actual source timestamp, model/weight identity,
measured pixel changes and image hashes. These are sampled-frame renderer checks;
they do not claim to capture a separate running player session.

Audio gain is per stream using Windows
[IAudioStreamVolume](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudiostreamvolume).
A paused audio stream prepared during a seek cannot unmute or change the volume
of the active stream. Local seek, scrub and resume paths set gain before starting;
network preparation stays silent until commit applies the latest mute/volume.
Older builds' application-owned session gain is migrated once when it matches
the saved application setting, so an old mute does not stay stuck after upgrade.
A different Windows mixer gain and Windows' separate mute flag are preserved.
`PhoenixAudioVolumeTests` reproduces overlapping stream preparation and handoff
against the real endpoint without starting either stream or playing sound. The
current before/after receipt is `artifacts/current/audio-mute-validation.json`.

## Credits and source origins

| Upstream | Contribution and reuse |
| --- | --- |
| [2600th/dlss5-video-player](https://github.com/2600th/dlss5-video-player) | **Imported and adapted source:** native Windows player, media/cache foundation, comparison/loupe renderer, subtitles, export/codec orchestration and UI. Pinned revision `8dfded2bc16477eb985d4d21575832bde07b3df0`; [MIT license](third_party/video-player/LICENSE) and [origin record](third_party/video-player/ORIGIN.json) retained. |
| [Phoenix DLSS5](https://github.com/RazvanManolache/dlss5-phoenix-addon) | Native model/runtime and the shared pipeline; the direct SDK adapter and resumable Phoenix job are local additions to the imported player. |
| [kibblerz/DLSS5-Reshade-AIO](https://github.com/kibblerz/DLSS5-Reshade-AIO) | Optical-flow provider **adapted inside the external Phoenix SDK**, used by the new direct playback/export path; Apache-2.0, with NVIDIA MIT headers. |
| [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling), [Benjamin-Wegener/dlss5-distill](https://github.com/Benjamin-Wegener/dlss5-distill) | Optional SDK scaling provider and native port of a published tiny 2x architecture/checkpoint, respectively. Their implementations reside in Phoenix; their upstream terms remain separate. |
| [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS), [NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline) | DLSS technology, SDKs and interface declarations. NVIDIA created the externally supplied NVIDIA networks and models. |
| [FFmpeg](https://ffmpeg.org/), [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) | Native decode/encode/mux helpers and the upstream player's documented Windows helper-build source. The applicable LGPL/GPL and component terms depend on the supplied build. |

[Lunarc-DLSS5](https://github.com/Yukikaze1945/Lunarc-DLSS5) and
[dlss5-vulkan-fg-relay](https://github.com/Yukikaze20170315/dlss5-vulkan-fg-relay)
were studied for native media/scaling controls, pipeline ordering and recovery.
Their VSR/fusion/relay implementations have not been imported into this player;
those ideas do not establish completed feature support. Retained upstream
dependency notices and their historical scope are explained in
[THIRD_PARTY.md](THIRD_PARTY.md).
