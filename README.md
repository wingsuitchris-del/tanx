# TANX

A two-player, turn-based tank artillery game inspired by classic Amiga-era
games of the early 1990s (in particular *Tanx* by Gary Roberts, 1991).
Destructible terrain, wind and gravity, special weapons, map pickups, and
a fully wood-panelled retro HUD — built from scratch in C++ on top of
[olc::PixelGameEngine](https://github.com/OneLoneCoder/olcPixelGameEngine).

## Features

- **Destructible, proceduraly-generated terrain** — choose Mountains or
  Foothills, or let it randomise each round
- **Configurable wind & gravity** — None/Light/Medium/Strong/Random for
  both, set per match from the main menu
- **Five weapons** beyond your standard shell:
  - **High Explosive** — double-radius blast
  - **Cluster** — splits into three shells at the apex of flight
  - **Laser** — an instant beam that cuts a trench through the terrain
  - **Ballistics Computer** — numerically solves the angle/power needed
    for a direct hit (wind not included — that part's still on you)
  - **Shield** — three layers of protection that visibly degrade (thick
    blue → orange → red) from hits *and* from time, whichever comes first
- **Map pickups** — a Mystery box (random ammo) and Health box spawn one
  at a time; shoot them to deny your opponent, or drive over them to claim
  the reward
- **Type-in or click controls** — set angle/power by typing digits,
  clicking +/- buttons, or holding the classic A/D/W/S keys
- **A wood-panelled, hand-drawn HUD** in the style of the original Amiga
  artillery games, right down to the plunger fire button and white flag
  surrender control
- **Procedural sound** — every sound effect (explosion, shell whistle,
  plunger click) is synthesised at startup, no audio assets required

## Controls

| Action | Input |
|---|---|
| Adjust angle | `A` / `D`, or click the `-`/`+` buttons, or type `0-9` |
| Adjust power | `W` / `S`, or click the `-`/`+` buttons, or type `0-9` |
| Move tank | `←` / `→` (costs movement budget) |
| Select weapon | Click a weapon box in the HUD |
| Fire | `SPACE`, or click the plunger |
| Skip turn | `ENTER` |
| Surrender | Click the white flag |

## Building from source

TANX is a single C++ source file (`tanx.cpp`) plus two bundled
single-header libraries — there's nothing to install beyond a C++17
compiler and (on macOS/Linux) libpng. The included `Makefile` auto-detects
your platform.

### macOS

Requires Xcode Command Line Tools and [Homebrew](https://brew.sh) (for libpng):

```sh
xcode-select --install
brew install libpng
make
./tanx
```

### Linux

Requires a C++17 compiler and the X11/OpenGL/libpng development headers:

```sh
# Debian/Ubuntu
sudo apt install build-essential libx11-dev libgl1-mesa-dev libpng-dev

make
./tanx
```

### Windows

Requires a MinGW-w64 toolchain (the easiest path is via
[MSYS2](https://www.msys2.org/)):

1. Install MSYS2, then from an MSYS2 **MinGW64** shell:
   ```sh
   pacman -S mingw-w64-x86_64-gcc make
   ```
2. From the project folder:
   ```sh
   make
   ./tanx.exe
   ```

No libpng is needed on Windows — the engine uses the built-in Windows GDI+
for image handling instead.

**Alternative (Visual Studio / Code::Blocks):** create a new C++ project,
add `tanx.cpp` as the only source file, set the C++ standard to C++17, and
link against: `user32 gdi32 opengl32 gdiplus shlwapi dwmapi`. No project
configuration beyond that is required — `olcPixelGameEngine.h` and
`miniaudio.h` handle the rest internally.

## Project structure

```
tanx.cpp                 Game source (single file)
olcPixelGameEngine.h      Graphics/window/input engine (third-party, bundled)
miniaudio.h               Cross-platform audio engine (third-party, bundled)
Makefile                  Cross-platform build file (macOS/Linux/Windows)
LICENSE                   License for this project's own code
THIRD_PARTY_LICENSES.md   License & attribution for the bundled libraries
```

## Credits

- Built on [olc::PixelGameEngine](https://github.com/OneLoneCoder/olcPixelGameEngine)
  by [OneLoneCoder.com](https://www.onelonecoder.com) (Javidx9)
- Audio powered by [miniaudio](https://miniaud.io) by David Reid
- Inspired by *Tanx* (1991) by Gary Roberts

See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for full license
details on the bundled libraries.

## License

This project's own code is released under the OLC-3 license — see
[LICENSE](LICENSE).
