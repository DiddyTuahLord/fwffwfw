# Popout Mini Player for foobar2000

A floating, see-through mini player for foobar2000 2.x on Windows. It shows the track title and artist, a progress bar you can click or drag to seek, and previous, play/pause and next buttons. The background is a rounded tinted card you can make fully transparent.

```
╭──────────────────────────────────────────╮
│  Track title                           × │
│  Artist                                  │
│  ━━━━━━━━━━━━━━●──────────────────────── │
│  1:23          ⏮     ( ▶ )     ⏭    4:56 │
╰──────────────────────────────────────────╯
```

## Get the component

foobar2000 components are Windows DLLs, so the code has to be compiled once. Pick whichever route is easier for you.

### Option A: let GitHub build it (nothing to install)

1. Create a new repository on GitHub (private is fine).
2. Upload everything in this folder, including the `.github` folder. If your uploader skips that folder, use **Add file → Create new file**, name it `.github/workflows/build.yml`, and paste in the contents of that file.
3. Open the **Actions** tab. The **Build component** run starts on its own; you can also start it with **Run workflow**.
4. When it finishes (a few minutes), open the run and download the **foo_popout_player** artifact. Unzip it to get `foo_popout_player.fb2k-component`.
5. Install it: double-click the file, or in foobar2000 go to **Preferences → Components → Install…**, then restart foobar2000 when asked.

### Option B: build it with Visual Studio

1. Install Visual Studio 2022 or 2026 (the free Community edition works) with the **Desktop development with C++** workload.
2. Get the foobar2000 SDK, either by running
   `powershell -ExecutionPolicy Bypass -File scripts\get-sdk.ps1`
   or by downloading SDK-2026-10-01 from https://www.foobar2000.org/SDK and unpacking it into the `sdk` folder, so that `sdk\foobar2000\SDK\foobar2000.h` exists.
3. Open `foo_popout_player.sln`, choose **Release** and **x64** (64-bit foobar2000) or **x86** (32-bit), and build. Visual Studio 2026 may offer to retarget the projects; accept it.
4. Run `powershell -ExecutionPolicy Bypass -File scripts\package.ps1` to create `build\foo_popout_player.fb2k-component`, then install it as in step 5 above.

## Using it

- **Show or hide:** View → Popout mini player. To give it a keyboard shortcut, open Preferences → Keyboard Shortcuts, click Add, and pick View → Popout mini player.
- **Move it:** drag anywhere on the card. It snaps to screen edges and remembers where you left it.
- **Seek:** click or drag along the progress bar.
- **Hide it:** the × in the top-right corner appears when the mouse is over the card.
- **Options (right-click the card):**
  - **Background:** fully transparent, 25%, 50% or 75% tint. With "Fully transparent", the card fades in while the mouse is over it so you can see where to grab it.
  - **Always on top**
  - **Auto-hide, pop up on track change:** the player appears when a new track starts (including new songs on internet radio) and fades out after 5 seconds. Move the mouse onto it to keep it open.
  - **Show foobar2000:** brings the main window back, even from the tray.

## Changing what it shows

The two text lines are foobar2000 title formatting scripts near the top of `PopoutWindow` in `src/popout_window.cpp`:

```cpp
compiler->compile_safe(m_scriptTitle, "%title%");
compiler->compile_safe(m_scriptArtist, "[%artist%]");
```

For example, `"[%artist% - ]%album%"` puts the album on the second line. The size of the card and the position of everything on it are in the `layout` namespace in the same file.

## Requirements

foobar2000 2.0 or newer, 32-bit or 64-bit. The package contains both versions.

## Project layout

| Path | What it is |
| --- | --- |
| `src/main.cpp` | Component info, the View menu command, startup and shutdown |
| `src/popout_window.cpp` | The window: drawing, buttons, seeking, dragging, fading, settings |
| `foo_popout_player.sln`, `.vcxproj` | Visual Studio solution; links the SDK's pfc, foobar2000_SDK and component_client libraries |
| `.github/workflows/build.yml` | The GitHub build |
| `scripts/get-sdk.ps1` | Downloads and unpacks the SDK into `sdk` |
| `scripts/package.ps1` | Packs the 32-bit and 64-bit DLLs into a `.fb2k-component` |

## Troubleshooting

- **"The build tools for v143 cannot be found"**: you have Visual Studio 2026 only. Right-click the solution, choose **Retarget solution**, or build from a Developer Command Prompt with `msbuild foo_popout_player.sln -p:Configuration=Release -p:Platform=x64 -p:PlatformToolset=v145`.
- **The SDK download fails**: download SDK-2026-10-01 by hand from https://www.foobar2000.org/SDK and unpack it into `sdk`.
- **Nothing appears after installing**: check View → Popout mini player. With auto-hide on, the player only shows up when a track starts.
