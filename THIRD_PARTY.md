# Third-party components

This repository contains project source code under the MIT License, but it interoperates with third-party software governed by separate licences and terms.

## NVIDIA DLSS / NGX and Optical Flow

The build obtains the official NVIDIA DLSS SDK into `external/DLSS`. NVIDIA files are not relicensed by this project. A public package may contain only the official DLSS runtime supplied with that SDK, together with its required notices, and only when NVIDIA's current licence permits the intended distribution.

The project uses NVIDIA Optical Flow SDK source locally for optical-flow integration. Its files remain subject to NVIDIA's terms and are not relicensed by this project.

## FFmpeg

The build reuses an installed FFmpeg or downloads a Windows FFmpeg build for local use. FFmpeg and distributed builds are governed by their own licence/configuration. A release pack must include the licence and notices that accompany the selected FFmpeg build.

## ReShade

ReShade is optional and separate from native DLSS SR/FG. It is not included in the public package.

## Optional RenoDX / NR runtime

RenoDX, `nvngx_dlssnr.dll`, ReShade and related runtime files are not included in this repository or public package. Users obtain and use them separately under the terms applicable to those files.
