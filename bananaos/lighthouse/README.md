# Lighthouse companion screen for bananaOS (Anbernic RG DS)

A companion screen for [HarbourMasters/Lighthouse](https://github.com/HarbourMasters/Lighthouse) (the Banjo-Kazooie PC port)
on dual-screen handhelds running **bananaOS**: the game stays on the top panel, the bottom panel shows Banjo's stats.

> **This is the Linux / bananaOS version**, not the Android app. The Android companion display lives in `android/` on the `android` branch
> of this repository; this screen's layout is taken from it. Nothing else on this branch is used here.

What the bottom panel shows (only while playing, black in cutscenes, the title screen and the pause menu):
lives, health honeycombs, notes, eggs, red and gold feathers, Jiggies, Mumbo tokens, the five Jinjos (or the total completion in
Gruntilda's Lair and Spiral Mountain) and the level's name, all drawn with the game's own sprites and bold font.
The pictures are read from the game's loaded assets at run time, so they come from your own ROM and nothing is bundled here.

## How it works

bananaOS' SDL has a mode that puts one window over both panels, but only for software rendering, so it can't carry Lighthouse's
OpenGL. Instead the game keeps its normal KMSDRM window on the top panel and `src/port/BananaPanel.cpp` takes the DRM fd that SDL
holds, gives the bottom panel's connector (DSI-1) its own CRTC and a dumb buffer, and draws the stats there in software from a
thread of its own. The game thread only copies a snapshot of the state once a frame (`BananaPanel_GameTick`).
The same trick works for any OpenGL game on bananaOS that wants the other panel.

It is off unless `BANANA_PANEL=1` is set, and does nothing on other systems (the hook is a weak symbol).

## Files

| | |
|---|---|
| `src/port/BananaPanel.cpp` | the companion screen; goes in `src/port/` of Lighthouse (picked up by the source glob) |
| `patches/lighthouse-bananaos.patch` | CMake: link libdrm. `gameloop.c`: `GetCurrentGameMode()` and the per-frame tick. Apply in the Lighthouse root |
| `patches/libultraship-bananaos.patch` | calls `BananaPanel_Start` once the GL window exists. Apply inside `libultraship/` |
| `Lighthouse (2 screens).sh` | launcher: lights the bottom panel's backlight and sets `BANANA_PANEL=1` |
| `apply.sh` | applies the above to a Lighthouse checkout |

Made against Lighthouse `bdb8cd9` (v1.2.2) and the [JeodC/libultraship](https://github.com/JeodC/libultraship) `bk-lighthouse` branch.

## Building (cross-compiling for the device)

A Debian trixie arm64 sysroot with `libsdl2-dev libdrm-dev libgles-dev libzip-dev libtinyxml2-dev libspdlog-dev libfmt-dev ...`:

```sh
./apply.sh /path/to/Lighthouse
cd /path/to/Lighthouse
cmake -S . -B build -DUSE_OPENGLES=ON -DCPU_OPTION=-mcpu=cortex-a55 -DCMAKE_TOOLCHAIN_FILE=<aarch64 toolchain>
cmake --build build --target Lighthouse -j8      # only the game; TorchExternal is a host tool
```

On the device, install the Lighthouse port (with its `bk.o2r`), put the binary next to it and use the launcher.

## Credits

Banjo-Kazooie is Rare/Nintendo's. Lighthouse by HarbourMasters, the Linux handheld build by Jeod, the companion screen layout from BanjoRecomp-Android.
You need your own copy of the game.
