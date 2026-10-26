# Third-party notices

| Component | Where | Licence |
|---|---|---|
| [ac-decomp](https://github.com/ACreTeam/ac-decomp) by ACreTeam | game code, via `external/ACGC-Android-Port` | CC0 1.0 |
| [ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port) by flyngmt | PC port layer (`pc/`), via `external/ACGC-Android-Port` | MIT |
| [ACGC-Android-Port](https://github.com/zwaetschge/ACGC-Android-Port) by zwaetschge | OpenGL ES 3 path the webOS build compiles (`external/ACGC-Android-Port`) | see its `LICENSE` (shipped as `LICENSE-ACGC-Android-Port.txt`) |
| [FixNES](https://github.com/FIX94/fixNES) by FIX94 | in-game NES emulator (`pc/lib/fixnes`) | MIT |
| [SDL-webOS](https://github.com/webosbrew/SDL-webOS) 2.30.12 | bundled as `lib/libSDL2-2.0.so.0` | zlib |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.63.0 by yhirose | upload page server (`third_party/cpp-httplib/`) | MIT (`third_party/cpp-httplib/LICENSE`) |
| [font8x8](https://github.com/dhepper/font8x8) by Daniel Hepper | setup screen and overlay font (`src/webos/transfer/BitmapFont.cpp`) | Public domain |
| [bcdec](https://github.com/iOrange/bcdec) by Sergii Kudlai | BC1-BC7 texture decoding (`third_party/bcdec/`) | MIT or Unlicense (`third_party/bcdec/LICENSE`) |
| Khronos OpenGL ES 3 and KHR headers | `third_party/khronos/` (build only) | MIT (in each header) |

Only the components above that are compiled into the app are distributed in
the IPK. The Android port's translation tools (pyjkernel, GPL-3.0) and Android
libraries are not used.

This project contains no game data. Disc images, extracted files, text and
textures are never part of this repository or the IPK: you supply your own
copy of the game. Animal Crossing is a trademark of Nintendo; this project is
not affiliated with or endorsed by Nintendo.
