# User guide

## Playback

Open a file with **Open**, `Ctrl+O`, or drag and drop. Space pauses playback. Left and Right seek by ten seconds. F11 or a double-click toggles fullscreen. The speaker button mutes audio.

## Super Resolution

**SR Off** keeps the normal native-resolution path and is the recommended setting when no scaling is required.

**DLAA** uses native resolution with DLSS temporal reconstruction. **Quality**, **Balanced**, **Performance** and **Ultra Performance** reduce internal resolution progressively to create more performance headroom. The image is then reconstructed to the selected output resolution.

Encoded video has no original game-engine motion vectors or depth buffer. The player reconstructs temporal inputs from adjacent video frames, so results depend on the source material. A scene cut, seek or decoder discontinuity resets temporal history.

## Frame Generation

Frame Generation is intended for larger display-rate multipliers. For a simple 2x increase, NVIDIA Smooth Motion in the NVIDIA App / Control Panel 3D settings is often the more appropriate first choice. With V-Sync enabled, unavailable multipliers are disabled to avoid exceeding the presentation limit.

## NVOF developer choices

The Developer section contains **NVOF Slow**, **Medium** and **Fast**.

- **Slow**: more optical-flow work and the best choice for difficult motion.
- **Medium**: the recommended default.
- **Fast**: lower cost, with less accurate motion in difficult scenes.

These are diagnostic/performance choices. They do not alter the original video file.
