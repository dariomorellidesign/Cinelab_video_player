# Neural Rendering with ReShade and RenoDX

This is an **optional, experimental** path. CineLab itself provides video playback, motion reconstruction, DLSS Super Resolution and supported Frame Generation. It does not ship ReShade, RenoDX, DLSS NR or any proprietary neural-rendering DLL.

## Before you begin

- Use a separate copy of the CineLab folder for experiments. Keep the clean release unchanged so you can compare and recover easily.
- Use Windows x64 and an NVIDIA RTX 50-series GPU with a current driver.
- Obtain every optional component from its publisher or another source whose licence permits your use. Do not use unknown DLL bundles.
- Do not use injection-based components with online or anti-cheat protected software. CineLab is an offline video player, but the same files must not be copied into protected games.

## The simple workflow

1. Extract CineLab to a dedicated folder, for example `C:\CineLab-NR-Test`.
2. Start CineLab once and confirm that normal playback works with **SR Off**.
3. Install a compatible ReShade build with add-on support for `DLSSVideoPlayer.exe`, selecting the DirectX 10/11/12 path when asked by its installer.
4. Add the compatible RenoDX/NR components supplied by their own provider to this same test folder. Keep components from one matching release together.
5. Start CineLab and press **Home** to open ReShade. Open **Add-ons**, locate the Neural Rendering panel, enable it, then return to the video.
6. Use the player’s **SR** control as the carrier path required by the optional component. Compare it against **SR Off** with the same scene and output resolution.

## Using third-party one-click installers

Some community tools automate ReShade and related optional files. They are not CineLab dependencies and are not bundled, validated or supported by this project. In particular, a tool may download closed-source or leaked runtime components; read its documentation, licences and release notes before running it.

For CineLab, choose a tool only if it lets you select `DLSSVideoPlayer.exe` in the dedicated test folder and makes its changes reversible. Keep its manifest/log files, use its removal command before changing versions, and never let it overwrite your clean CineLab install.

## Verification and troubleshooting

The expected sequence is:

1. CineLab opens and plays normally with SR Off.
2. ReShade opens with **Home**.
3. The add-on is visible on ReShade’s **Add-ons** page.
4. Enabling the optional NR path visibly changes the image while the player remains responsive.

If CineLab fails to start after adding optional files, close it, move the added files out of the test folder, and confirm the clean player still starts. Save `DLSSVideoPlayer.log` and `ReShade.log` before changing anything else; remove personal paths before sharing logs in an issue.

## What CineLab can support

CineLab can document a reproducible configuration and help diagnose the player, motion-vector and presentation path. It cannot redistribute restricted runtimes, verify the provenance of third-party DLLs, or guarantee that an external add-on will work with every driver and version.
