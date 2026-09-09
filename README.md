# CineLab Video Player

A Windows x64 video player for NVIDIA RTX GPUs. It combines FFmpeg decoding, a Direct3D 12 renderer, NVIDIA Optical Flow, DLSS Super Resolution and DLSS Frame Generation for high-quality video playback experiments.

> **Release status:** public beta. The release package targets Windows x64 systems with an NVIDIA RTX 50-series GPU. It includes the player, decoder tools and the native DLSS SR runtime. Frame Generation availability depends on the installed NVIDIA driver and the GPU/runtime capability reported by the player.

## Features

- Opens MP4, MKV, MOV, WebM, AVI and other FFmpeg-supported formats.
- Uses a D3D12 three-frame pipeline and GPU optical flow to reconstruct video motion vectors.
- Provides SR Off, DLAA, Quality, Balanced, Performance and Ultra Performance.
- Provides DLSS Frame Generation controls when the runtime supports them.
- Includes audio, subtitles, seeking, fullscreen, image adjustments and English/Portuguese language packs.
- Offers final-image and developer views for DLSS input and motion vectors.

## Quick start

1. Download `CineLabVideoPlayer-v0.12.0-win64.zip` from GitHub Releases and extract it anywhere writable.
2. Run `DLSSVideoPlayer.exe`.
3. Click **Open** or drag a video into the window.
4. Leave **SR** off for native-resolution playback. Enable it only when you want DLSS reconstruction.
5. Open **Settings** for SR quality, Frame Generation, V-Sync, audio, subtitles and developer controls.
6. Use the **?** button for the in-player guide.

## Requirements

- Windows 10 or Windows 11, 64-bit
- NVIDIA GeForce RTX 50-series GPU
- A current NVIDIA driver

The beta may start on other RTX configurations, but the supported release target is RTX 50-series hardware.

## Optional NR / RenoDX integration

RenoDX and DLSS 5 NR are **not part of the public package**. They are optional external components with their own distribution terms. This project does not distribute or provide a bypass for proprietary NR runtimes.

If you already have a compatible, lawfully obtained installation, see [the optional integration guide](docs/OPTIONAL_NR_SETUP.md). The standard player remains fully functional without it.

## Controls

| Action | Shortcut |
| --- | --- |
| Open video | `Ctrl+O` |
| Play / pause | `Space` |
| Back / forward 10 seconds | `Left` / `Right` |
| Mute | `M` |
| Toggle SR | `D` |
| Fullscreen | `F11` or double-click video |
| Final image | `1` |
| DLSS input | `2` |
| Motion vectors | `3` |

## Building from source

See [Building](docs/BUILDING.md). The source release contains no NVIDIA, FFmpeg, ReShade, RenoDX or other third-party runtime binaries.

## Support

If the player is useful to you, voluntary support helps fund testing and maintenance. The public support link will appear here and in the GitHub sidebar once the project owner provides a Ko-fi or Buy Me a Coffee profile URL. See [support setup](docs/SUPPORT_SETUP.md).

## Project documents

- [User guide](docs/USER_GUIDE.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Building](docs/BUILDING.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Contributing](CONTRIBUTING.md)
- [Third-party notices](THIRD_PARTY.md)
- [Security policy](SECURITY.md)

## License

The project source is licensed under the [MIT License](LICENSE). NVIDIA, FFmpeg, ReShade and RenoDX are separate projects governed by their own licenses and terms. DLSS and NVIDIA are trademarks of NVIDIA Corporation. This project is not affiliated with or endorsed by NVIDIA, ReShade, RenoDX or FFmpeg.
