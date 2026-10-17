# 0.3.1-rc.1

- Restore Direct3D9 device hooks after Reset replaces method dispatch during
  fullscreen/windowed or resolution switches. Refresh original delegates so
  high-resolution native font binding continues after each switch.
- Always enable diagnostic logging in this prerelease for native texture
  binding, presentation sizes, and device reset hook state; no INI change needed.
- Test repeated Reset dispatch replacement and native binding on a real D3D9
  device, both with and without diagnostic logging.

# 0.3.0

- Menu performance fix: unrelated textures bypass font inspection/readback;
  driver calls run outside the metadata mutex; initialization creates no probe
  HAL device. Known fonts remain usable by UE3's deferred text rendering.
- Reconstruct SmallFont/MediumFont atlases at 4x resolution from the original
  Rockwell WGL outlines in the user's installed startup packages.
- Use the same mechanism for INT/DEU/FRA/ESN/ITA and the supplied Russian package.
  Match source texture contents to select the appropriate localized atlas.
- Keep the game's native glyph cells, font renderer and subtitle layout.
- Use the game's existing bounded LZO decoder after complete code/switch-table
  validation. No third-party decompression library is bundled or linked.
- Add automatic fallback to the previous bitmap scaling after native preparation
  or GPU failures, and `NativeFont=0` to force the previous rendering.
- Retain size/offset settings, separate video positioning and supported EXE hashes.
- Add resource tests for all six actual packages and real Direct3D9 checks for
  direct/staged uploads, exact GPU pixels, resource release, device reset and
  forced allocation/binding failure fallback.
- Verify the decoder on all three supported EXEs, reject altered code, and check
  truncated/oversized streams plus 4096 malformed inputs with guarded buffers.

Steam GOTY native texture binding confirmed in-game; preview updated.
GOG native rendering and Russian in-game character handling remain unverified.
