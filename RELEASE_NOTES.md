# Phoenix Video Player 1.0 Beta

- **Quiet update notice:** the menu links to newer GitHub releases, including numbered betas. Opening the link keeps the notice visible until the installed version catches up.
- Updated to Phoenix DLSS5 1.0 Beta and its rebuilt native SDK, model-free kernels and portable runtime companions.
- Original and Lightweight models, live appearance/skin/style controls, comparison views, processing budgets and optional spatial scaling.
- Native RTX plus available CPU/CUDA/Vulkan workers, resumable SDR exports and authenticated matching-frame caches.
- Package updates preserve local recipes and refuse to overwrite modified managed files.

The player foundation is adapted from [2600th/dlss5-video-player](https://github.com/2600th/dlss5-video-player), under MIT. Original additions use Apache-2.0; bundled tools retain their licenses and notices.

Extract the Windows ZIP and use Configure-PhoenixPlayer.ps1 with your separately supplied compatible model. Everything can also launch the player with its central model library. No NVIDIA model DLLs, weights, private media or saved settings are bundled.

This is a beta prerelease. Validation includes the checks recorded in VALIDATION.json; it does not establish movie throughput, older physical RTX coverage, discrete AMD/Intel support or HDR display accuracy. Phoenix's isolated benchmark results are not player FPS guarantees.
