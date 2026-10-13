# Arkham Subtitle Scale 0.3.0

| Store | Status |
| :--- | :---: |
| <img src="https://cdn.simpleicons.org/steam/1B6D9B" width="20" height="20" alt="Steam"> **Steam GOTY** | ✅ Tested |
| <img src="https://cdn.simpleicons.org/gogdotcom/86328A" width="20" height="20" alt="GOG"> **GOG** | ✅ Tested |
| <img src="https://cdn.simpleicons.org/epicgames/777777" width="20" height="20" alt="Epic Games"> **Epic Games** | ⏳ Not yet |

Configurable subtitle size and separate gameplay/video positioning for **Batman:
Arkham Asylum GOTY (Windows)**, now with high-resolution original font glyphs.

Version 0.3.0 reads original Rockwell WGL vector outlines from installed startup
packages and reconstructs SmallFont/MediumFont atlases at 4x resolution.
Direct3D9 uses them with the game's original character cells and normalized UVs.
The game keeps its font renderer, spacing, baselines, wrapping, timing, colors
and positioning. Native drop shadows are reconstructed too.

Font uploads are recognized once and bindings reuse the cached result. Unrelated
menu/scene textures pass through without texture queries or GPU readback during
ordinary rendering. Direct3D calls run outside the mod's metadata mutex. The mod
hooks the game's real device creation and creates no additional probe HAL device.

One binary handles **English, German, French, Spanish and Italian**, plus the
provided Russian localization with `Startup_RUS.upk`. Russian uses its real
Rockwell outlines. The mod scans installed `Startup_*.upk` files and matches font
texture contents, so changing languages or installing that Russian localization
requires only restarting the game. Other translations have not been tested.
The mod uses the game's existing bounded LZO decoder and standard Windows GDI+
and Direct3D9. It verifies the decoder's complete code and switch table before
calling it, accounting for address relocation. A validation or decompression
failure selects the original bitmap scaling. No additional third-party library,
Python, system Rockwell font or downloaded font assets are required. The release
includes no extracted game fonts, packages or decoder implementation.

Native font binding has been confirmed in-game on the installed Steam GOTY
build, and the preview below shows the current 3x subtitle settings. Automated
checks passed on all six real localization packages and a real Direct3D9 device:
direct/staged uploads, GPU pixel equality, failure fallback, release and reset.
The menu rendering path also passes 20,000 unrelated texture bindings without
late inspections or GPU readback. GOG native rendering, Russian in-game character
handling and other subtitle scenes still need in-game verification.

<details>
<summary><strong>In-game preview — click to expand</strong><br><br></summary>

<p align="center">
  <img src="https://raw.githubusercontent.com/TheTronuo/ArkhamAsylumSubtitleFix/main/assets/subtitles-0.3.0.png" alt="Batman Arkham Asylum 0.3.0 gameplay with enlarged subtitles and original high-resolution font glyphs" width="960">
  <br><em>Version 0.3.0 — subtitle scaling at 3×</em>
</p>

</details>

## Install or update

1. Close the game.
2. Copy the three files from the archive's `Binaries` folder into the game's
   `Binaries` folder beside `ShippingPC-BmGame.exe`.
3. When updating, keep your INI to preserve scale and offsets. Add `NativeFont=1`
   under `[Subtitles]`, or leave it absent (the default is enabled).
4. Launch the game normally.

If another mod supplies `dinput8.dll`, keep that loader and check its ASI loading
support. The supplied proxy loads only `ArkhamSubtitleScale.asi`. Third-party
graphics hook/loader compatibility has not been verified.

## Settings

```ini
[Subtitles]
Enabled=1
NativeFont=1
Scale=3.0
VerticalOffset=100
CutsceneVerticalOffset=-50
```

| Setting | Meaning |
| --- | --- |
| `Enabled` | `0` disables the mod; `1` enables it. |
| `NativeFont` | `1` reconstructs original outlines; `0` forces the previous bitmap scaling. Defaults to `1` when absent. |
| `Scale` | Size multiplier from `0.5` to `4.0`, using a decimal point. |
| `VerticalOffset` | Gameplay subtitle offset in rendered pixels; positive moves down, negative moves up. Range `-1000` to `1000`. |
| `CutsceneVerticalOffset` | Video subtitle offset with the same range; inherits `VerticalOffset` when absent. |

Restart after changing settings. In-engine cinematics using the gameplay renderer
use `VerticalOffset`. Supplied offsets are starting values; updates should retain
the user's values.

## Automatic fallback and diagnostics

If extraction or graphics hook initialization fails, the previous bitmap scaling
still runs with the configured scale and offsets. If creation/upload/binding of
an HD GPU texture fails, that font uses its original texture. Unmatched textures
pass through unchanged. Failed GPU allocation is not retried every draw; a new
source upload allows another attempt. No substitute typefaces or fake letters
are used. Native failure does not bypass EXE/hook/settings validation.

Read `Binaries/ArkhamSubtitleScale.log` after displaying subtitles. This line
confirms that a reconstructed texture actually reached Direct3D9:

```text
Native 4x font texture bound successfully. Original game renderer and glyph metrics retained.
```

`Prepared ... native font atlases` confirms CPU preparation only. Fallback causes
are logged. If the game's upload path never matches a font, bitmap rendering
continues and no successful native binding is reported. Use `NativeFont=0` to
compare the same subtitle with the old bitmap rendering.

Canvas text sharing SmallFont/MediumFont can also become sharper, keeping its
original size/layout. Control cells and whitespace retain native behavior. The
patch does not add character mappings to the game's Font objects. The provided
Russian bitmap remap contains А–Я/а–я but no explicit Ё/ё entries, although those
outlines exist in its vector library; original handling is preserved and needs
in-game verification.

## Supported executables

The three exact hashes and 13 subtitle hook sites from 0.2.2 are retained. The
installed Steam GOTY EXE was checked locally and native font binding was confirmed
in-game. GOG scaling was previously tested; its new native font mode has not been
tested in-game.

```text
Steam GOTY:
4dac1f5e2ac6710b7378fdce74601f616f4753e3756cb5fda63c7519cc2eb028
GOG without Joker DLC patch:
9483c5084c7ef02d7243616dbc31219363652fc73bb29a155a9940f5cb1cc6cd
GOG with Joker DLC patch:
6d1055fbd9b947d6a173711bbd737a11b97076fdaf3b0c7cb6aebad005d7f5e6
```

Unsupported executables, conflicting hook bytes and invalid settings are refused.

## Uninstall or rollback

Restore the backed-up ASI/loader/INI, or set `NativeFont=0` for the old rendering
inside 0.3.0. Set `Enabled=0` to disable all subtitle changes. Restart afterward.
EXE and UPK files are never edited.

## Build and validation

Run `build.cmd "absolute-path-to-ShippingPC-BmGame.exe"` with MSVC x86 tools.
Run `build\FontTests.exe "absolute-game-root" "optional-Startup_RUS.upk-path"`
from the source directory for package and GPU checks. Add `-exe "game-EXE-path"`
to test another supported EXE's decoder. Tests map the supplied EXE without
starting the game, relocate the isolated decoder in that test process, and
check invalid decoder rejection and guarded input/output buffers. The original
EXE file is never modified. Tests create hidden disposable graphics windows and
atlas previews under `build\font-previews`.

Original code: MIT (`LICENSE`). This is an unofficial community project.
