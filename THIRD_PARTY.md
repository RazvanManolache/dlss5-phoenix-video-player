# Credits and third-party notices

Original project additions use the checked-in [Apache-2.0 license](LICENSE).
Imported source and separately supplied binaries retain their original terms.
This inventory does not change a component's license or grant rights to NVIDIA
model data, vendor runtimes or media content.

## Imported native player foundation

Substantial source was imported from
[2600th/dlss5-video-player](https://github.com/2600th/dlss5-video-player) at revision
`8dfded2bc16477eb985d4d21575832bde07b3df0`, under MIT. Credit goes to the DLSS
Video Player contributors for the native application/media/cache foundation,
comparison/loupe renderer, subtitle presentation, codec/export orchestration
and UI. This is direct source reuse and adaptation, not just a design reference.
The package-source identity and hardware-report helpers under `tools/` also
retain that pinned upstream source; their scratch-directory tests add local cleanup guards.

The unmodified upstream [MIT license](third_party/video-player/LICENSE) and
[origin record](third_party/video-player/ORIGIN.json) are retained. Local changes
include Phoenix branding, direct SDK loading and color/frame-identity adapters,
same-frame comparison capture, screenshot receipts, native resumable Phoenix
jobs and integration with Everything's central model/runtime recipes.

`packaging/tool-lock.json` retains the upstream helper version/hash/provenance
records. Phoenix's `tools/Prepare-MediaHelpers.ps1` stages only the four locked
media helpers from that pinned upstream release archive and carries the
applicable original helper notices into the build. It does not import the
upstream application's bundled model or legacy addon binaries.

The imported upstream `THIRD_PARTY_LICENSES/` directory retains its original
dependency notices. Some describe the upstream application's full bundle,
including its legacy RenoDX/ReShade runtime or helper downloads; their presence
is not a statement that the current Phoenix build ships those binaries. The
direct Phoenix path does not need that legacy addon. Preserve the applicable
original texts when distributing any of their components.

## Included declarations and external components

| Component/source | Local scope and notices |
| --- | --- |
| [NVIDIA Optical Flow SDK](https://developer.nvidia.com/optical-flow-sdk) | MIT interface declarations in `external/nvof/`; [retained header license](THIRD_PARTY_LICENSES/nvidia-optical-flow-MIT.txt). The original player's optical-flow code belongs to the MIT player import; the new direct Phoenix provider is credited separately below. |
| [NVIDIA Video Codec SDK](https://developer.nvidia.com/video-codec-sdk) | MIT interface declaration in `external/nvenc/nvEncodeAPI.h`; [retained header license](THIRD_PARTY_LICENSES/nvidia-video-codec-MIT.txt). |
| [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline), [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | External SDKs, interfaces and optional runtime components; [retained Streamline interface notice](THIRD_PARTY_LICENSES/nvidia-streamline-MIT.txt). Vendor binaries/model data retain their own terms. |
| [FFmpeg](https://ffmpeg.org/), [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) | Separate native helper executables, not linked into the player. Upstream's [helper-build/source record](THIRD_PARTY_LICENSES/ffmpeg.txt), [bundled component notices](THIRD_PARTY_LICENSES/ffmpeg-bundled-notices.txt) and [GPL text](THIRD_PARTY_LICENSES/ffmpeg-GPL-3.0.txt) are retained. The actual supplied build determines applicable terms. |
| [ReShade](https://github.com/crosire/reshade), [RenoDX](https://github.com/clshortfuse/renodx), DLSS5 Feeder and its NIGos bridge ancestry | Historical upstream-runtime integration; retained [ReShade notice](THIRD_PARTY_LICENSES/reshade-BSD-3-Clause.txt), [RenoDX notice](THIRD_PARTY_LICENSES/renodx-MIT.txt) and [Feeder/bridge notice](THIRD_PARTY_LICENSES/dlss5-feeder-MIT.txt). These notices are not evidence that the current package includes the addons or permission to redistribute an unlicensed model/runtime. |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp), [Deno](https://github.com/denoland/deno) | Optional upstream network-media helper integration; retained [yt-dlp notice](THIRD_PARTY_LICENSES/yt-dlp-2026.10.10.233141.txt) and [Deno notice](THIRD_PARTY_LICENSES/deno-2.9.5.txt). |

Font/icon assets retain the license files alongside their assets. The full
source notices, including headers within imported files, remain in place.

## Shared Phoenix SDK credits

The separate [Phoenix SDK](https://github.com/RazvanManolache/dlss5-phoenix-addon)
retains its own dependency inventory, licenses and adaptation notices. Relevant
providers used by this integration include:

- [kibblerz/DLSS5-Reshade-AIO](https://github.com/kibblerz/DLSS5-Reshade-AIO): directly adapted optical-flow provider, Apache-2.0, with NVIDIA MIT interface headers.
- [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling): retained MIT SDK scaling/sharpening code, with Phoenix's D3D12 adapter.
- [Benjamin-Wegener/dlss5-distill](https://github.com/Benjamin-Wegener/dlss5-distill): Apache-2.0 published architecture and optional checkpoint, pinned to `9e3bc2f9e755f0e281b72e439f9452be0e8b7f31`; new native C++/HLSL port in Phoenix.
- [mauri870/dlssnr-pytorch](https://github.com/mauri870/dlssnr-pytorch), [DLSS-NR on Intel Xe2](https://github.com/Uzbekunknown/dlss-nr-on-intel), [DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD) and [OpenNR](https://github.com/clshortfuse/openNR): model-layout, backend-source and compatibility contributions documented by Phoenix. This player does not claim authorship of those components.

These are SDK-level dependencies/credits, not additional direct source imports
into the player's source tree. External Original/Lightweight model exports
supplied through Everything are credited to
[bdsqlsz/DLSSNR3.19.16.0](https://huggingface.co/bdsqlsz/DLSSNR3.19.16.0); the
underlying NVIDIA networks and model data remain NVIDIA's.

## Feature references

[Yukikaze1945/Lunarc-DLSS5](https://github.com/Yukikaze1945/Lunarc-DLSS5) and
[Yukikaze20170315/dlss5-vulkan-fg-relay](https://github.com/Yukikaze20170315/dlss5-vulkan-fg-relay)
were studied for native media/scaling controls, multi-resolution processing,
pipeline ordering and recovery. Their VSR, fusion and relay implementations
have not been copied into this application. VSR is deferred; Phoenix implements
its own shared fusion graph and adapters. Reading a repository does not make it a bundled
dependency or change its license.
