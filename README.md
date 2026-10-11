# Arkham Subtitle Scale

| Store | Status |
| :--- | :---: |
| <img src="https://cdn.simpleicons.org/steam/1B6D9B" width="20" height="20" alt="Steam"> **Steam GOTY** | ✅ Tested |
| <img src="https://cdn.simpleicons.org/gogdotcom/86328A" width="20" height="20" alt="GOG"> **GOG** | ⏳ Testing |
| <img src="https://cdn.simpleicons.org/epicgames/777777" width="20" height="20" alt="Epic Games"> **Epic Games** | ⏳ Not yet |

Configurable subtitle size and vertical positioning for **Batman: Arkham Asylum GOTY (Windows)**.

**Version 0.2.1 adds GOG support**, with and without the Joker DLC patch. GOG gameplay verification is pending. Subtitle scaling at 3x has been confirmed in gameplay at 3840x2160; vertical positioning and other subtitle scenes still need verification.

<details>
<summary><strong>In-game preview — click to expand</strong><br><br></summary>

<p align="center">
  <img src="https://github.com/user-attachments/assets/2fea55b2-c157-416c-b5a0-6dcf0c886b81" alt="Batman Arkham Asylum gameplay with enlarged subtitles" width="960">
  <br><em>Subtitle scaling at 3×</em>
</p>

</details>

## Install

1. Close the game.
2. Download the ready-to-use ZIP attached to a GitHub Release. The automatically generated source archives do not contain the compiled mod.
3. Copy the three files inside the archive's `Binaries` directory into the game's `Binaries` directory, next to `ShippingPC-BmGame.exe`:
   - `dinput8.dll`
   - `ArkhamSubtitleScale.asi`
   - `ArkhamSubtitleScale.ini`
4. Launch the game normally. No compilation is required.

If another mod already supplies `dinput8.dll`, do not overwrite it. This minimal loader loads only this mod's ASI. Compatibility with other loaders is not implemented or tested.

## Settings

Edit `Binaries/ArkhamSubtitleScale.ini`, then restart the game:

```ini
[Subtitles]
Enabled=1
Scale=3.0
VerticalOffset=160
```

| Setting | Meaning |
| --- | --- |
| `Enabled` | `0` disables subtitle modifications; `1` enables them. |
| `Scale` | Size multiplier, from `0.5` to `4.0`. Use a decimal point. `1.0` is the original size. |
| `VerticalOffset` | Offset in pixels at the current rendering resolution, from `-1000` to `1000`. Positive moves down; negative moves up; `0` keeps the game's original position. |

The supplied 3x scale and 160-pixel offset are starting values for 3840x2160, not automatic resolution scaling. Large offsets can move text off-screen. Use `Scale=1.0` and `VerticalOffset=0` for the original draw size and position.

<details>
<summary><strong>Supported executable hashes</strong></summary>

Only these exact `ShippingPC-BmGame.exe` SHA256 hashes are accepted:

```text
Steam GOTY
4dac1f5e2ac6710b7378fdce74601f616f4753e3756cb5fda63c7519cc2eb028

GOG without Joker DLC patch — Testing
9483c5084c7ef02d7243616dbc31219363652fc73bb29a155a9940f5cb1cc6cd

GOG with Joker DLC patch — Testing
6d1055fbd9b947d6a173711bbd737a11b97076fdaf3b0c7cb6aebad005d7f5e6
```

All 13 subtitle hook sites match in these files. Other executables are refused. The mod does not install or enable Joker DLC.

</details>

## License

The original mod code is provided under the MIT license; see `LICENSE`. Game assets are not included. This is an unofficial community project.
