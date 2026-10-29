# Phoenix Video Player 0.1.0

The first standalone Windows x64 release of this Phoenix adaptation.

- Original and Lightweight models through the native RTX library, plus installed portable CPU/CUDA/Vulkan workers.
- Live appearance/skin/style controls, comparison views, processing budgets and optional spatial scaling.
- Resumable SDR media exports, authenticated frame caches and matching original/processed inspection.
- Phoenix source pinned to 0.19; binaries and model-free kernels are included, models remain external.
- Package updates preserve local recipes and refuse to overwrite modified managed files.

The native player/media foundation is adapted from [2600th/dlss5-video-player](https://github.com/2600th/dlss5-video-player), under MIT. Original additions use Apache-2.0; bundled tools retain their licenses and notices.

Extract the Windows ZIP and use Configure-PhoenixPlayer.ps1 with your separately supplied compatible model. Everything can also launch the player using its central model library. No model DLLs, extracted weights, private media or saved settings are bundled.

Fresh validation covers native/shared pipeline checks, comparison pixels, decoder-attachment rollback, matching cached playback, backend replacement and resumable-export pixels. Package execution is checked independently against the validated SDK. These small fixtures do not establish movie performance, discrete AMD/Intel coverage, HDR display accuracy or game compatibility. Scaling/fusion options remain experimental where indicated.
