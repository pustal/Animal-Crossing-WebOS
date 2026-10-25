# Third-party notices

| Component | Where | Licence |
|---|---|---|
| [ac-decomp](https://github.com/ACreTeam/ac-decomp) by ACreTeam | game code, via `external/ACGC-Android-Port` | CC0 1.0 |
| [ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port) by flyngmt | PC port layer (`pc/`), via `external/ACGC-Android-Port` | MIT |
| [ACGC-Android-Port](https://github.com/zwaetschge/ACGC-Android-Port) by zwaetschge | OpenGL ES 3 path the webOS build compiles (`external/ACGC-Android-Port`) | see its `LICENSE` (shipped as `LICENSE-ACGC-Android-Port.txt`) |
| [FixNES](https://github.com/FIX94/fixNES) by FIX94 | in-game NES emulator (`pc/lib/fixnes`) | MIT |
| [SDL-webOS](https://github.com/webosbrew/SDL-webOS) 2.30.12 | bundled as `lib/libSDL2-2.0.so.0` | zlib |
| Khronos OpenGL ES 3 and KHR headers | `third_party/khronos/` (build only) | MIT (in each header) |

Only the components above that are compiled into the app are distributed in
the IPK. The Android port's translation tools (pyjkernel, GPL-3.0) and Android
libraries are not used.

This project contains no game data. Disc images, extracted files, text and
textures are never part of this repository or the IPK: you supply your own
copy of the game. Animal Crossing is a trademark of Nintendo; this project is
not affiliated with or endorsed by Nintendo.
