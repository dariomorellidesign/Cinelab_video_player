# NR visual comparisons

This page defines the image set used to show CineLab’s optional Neural Rendering path. Every comparison must use video that the project owns or is licensed to publish. Do not use film footage, streaming captures or other copyrighted clips without permission.

## Capture rules

- Use the same frame, output resolution, display mode and image-adjustment values for each pair.
- Pause playback before taking each image.
- Label images **Original / SR Off**, **SR + NR**, source resolution, output resolution and GPU/driver version.
- Do not sharpen, denoise or resize the exported screenshots after capture.
- Save lossless PNG files under `docs/assets/nr/` using the names below.

## Planned pairs

| Scene | Files | What the reader should inspect |
| --- | --- | --- |
| Fine film grain / texture | `grain-original.png`, `grain-nr.png` | Grain retention, local texture and edge stability. |
| Low-light gradient | `lowlight-original.png`, `lowlight-nr.png` | Banding, temporal noise and detail in dark areas. |
| Slow pan | `pan-original.png`, `pan-nr.png` | Motion coherence, trails and flicker. |
| Fine text / credits | `text-original.png`, `text-nr.png` | Letter edges and stability against a dark background. |

## Presentation layout

Add each pair as an HTML-free Markdown table so it renders on GitHub:

| Original / SR Off | SR + NR |
| --- | --- |
| `![Original](assets/nr/grain-original.png)` | `![SR + NR](assets/nr/grain-nr.png)` |

Below each pair, add the source clip licence, the exact CineLab version, GPU/driver, SR setting, optional add-on version and a short observation. Avoid claims such as “better” without naming the visible trade-off.
