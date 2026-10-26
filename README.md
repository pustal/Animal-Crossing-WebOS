# Animal Crossing Port for WebOS TVs

<img src="webos/largeIcon.png" alt="App icon" width="96" align="right">

A native LG webOS TV build of the Animal Crossing (GameCube) decompilation,
running on the TV's own GPU through OpenGL ES 3. You need your own copy of the
game: no game data is included.

Developed and tested on an LG TV with webOS 6 (Mali-G51). Install it from the
Homebrew Channel by adding [pustal's repository](https://github.com/pustal/webos-repo),
or with the `.ipk` from the [releases](https://github.com/pustal/Animal-Crossing-WebOS/releases).

## How it works

This project does not change the game code. It builds the
[ACGC Android port](https://github.com/zwaetschge/ACGC-Android-Port)
(the [PC port](https://github.com/flyngmt/ACGC-PC-Port) of
[ac-decomp](https://github.com/ACreTeam/ac-decomp) plus an OpenGL ES 3
renderer path) for webOS, with a small webOS layer in `src/webos/`:

| File | What it does |
|---|---|
| `webos_glue.c` | Picks the data folder, creates `rom/`, `save/`, `texture_pack/`, links the shaders, writes logs, logs crashes |
| `webos_present.c` | Makes each frame opaque before it is shown (the game writes alpha, which the TV's compositor would otherwise blend away), and shows an activity bar while shaders compile on first launch |
| `webos_shader_cache.c` | Rewrites the desktop GLSL 3.30 shaders to GLSL ES 3.00, and caches linked shader programs in `shader_binaries/`: the first launch compiles them (about 30 s), later launches load them in well under a second |
| `webos_tev_specialize.c` | Rewrites each of the port's TEV shader variants into straight-line code. The TV's Mali driver does not fold the port's generic shader, which made dialogue screens run at 6-10 fps; `tools/tev_equivalence/run.sh` checks the rewritten shaders render identically |
| `webos_bc_decode.c` | Lets the texture pack loader use BC1/BC3/BC7 (DXT) packs on a GPU without them: reports the extensions and decodes those uploads to RGBA on the CPU |
| `webos_gpu_profile.c` | Optional GPU timing mode for finding expensive draws (see Troubleshooting) |
| `webos_overlay.c` | In-game panel with the upload page address, PIN and this project's link (remote key 1, controller LB) |
| `transfer/` | The upload page server (cpp-httplib), setup screen, USB drive and root detection, texture pack placement; the page itself is `web/index.html`. Built as `lib/libacwebos_transfer.so` with its own private C++ runtime, because the game replaces the global `operator new` with its own heap allocator |
| `webos_remote.c` | TV remote controls (below); Back opens the pause menu instead of closing the app |
| `glad/gl.h` | Replaces the desktop GL loader: maps the few desktop-only GL calls to ES ones and routes shader calls and buffer swaps to the files above |
| `webos_forced_decls.h`, `webos_bstring.c` | Reconcile the decomp's own libc declarations with the TV's glibc |
| `cmake/GlesLinkStub.cmake` | The toolchain only ships OpenGL ES 2 libraries; the game links against a stub with the ES 3 entry points and uses the TV's real driver at run time |

The TV requirements were checked first with
[ES3-WebOS-probe](https://github.com/pustal/ES3-WebOS-probe): webOS 6 Mali
drivers expose OpenGL ES 3.2, and the port's specialised shaders render a
frame in about 16 ms at 1080p.

## Installing

1. Install `dist/org.animalcrossingwebos.app_<version>_arm.ipk` with
   [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) or
   `ares-install`.
2. Start the app. Without a disc image it shows an address and a PIN: open
   the address on a phone or computer on the same network, enter the PIN and
   upload an image of your own disc (US version, GAFE01; `.iso`, `.gcm` or
   `.ciso`). The game starts as soon as it arrives.

### The upload page

The page stays available while you play (port 47811; press **1** on the
remote, or LB on a controller, to see the address and PIN). It only answers devices on your home
network that send the PIN. It can:

- replace the disc image;
- download and upload memory-card saves (`.gci`); uploads made while the game
  runs are applied at the next start;
- show whether the app has **root** (running as root, or root commands through
  the Homebrew Channel), and each **USB drive**'s free space and access;
- create `Animal Crossing/texture_pack` on a USB drive and upload a texture
  pack folder into it (or into the TV storage). At start-up the game uses a
  pack on a USB drive automatically (a link, or a root bind mount when the app
  cannot read the drive itself).

Dolphin texture packs work as they are. The TV's Mali GPU shows ASTC 4x4 and
uncompressed DDS files directly; BC1/BC3/BC7 (DXT) files, which most packs
use, are decoded by the TV's processor the first time the game uses each
texture (`webos_bc_decode.c`, using bcdec). That can cause a brief hitch and
takes more memory than the GPU-native formats. The page skips files the
game's loader cannot read at all (DXT3, BC4-BC6) and system files.

The data folder (disc image, saves, settings, logs) is
`/media/developer/apps/usr/palm/applications/org.animalcrossingwebos.app/data`
on a developer-mode install. It survives app updates but not uninstalling.

## Controls

A game controller works as on the PC port (left stick walks, right stick is
the camera, Back/Select opens the pause menu). The TV remote, held normally:

| Remote | GameCube | | Remote | GameCube |
|---|---|---|---|---|
| Arrows | Walk (control stick) | | 0, Play | Start |
| OK, green | A | | 5 | Z |
| Red | B | | CH − / CH + | L / R |
| Blue | X | | 2 / 8 / 4 / 6 | Camera (C-stick) |
| Yellow | Y | | Back, Exit | Pause menu |
| 1 | Upload page address and PIN (LB on a controller) | | | |

A keyboard uses the PC port's layout, except the camera (T/F/G/H) and Start
(P), whose PC keys the remote sends. The layout is written to
`keybindings.ini` on first launch; edit it there if you like.

## Troubleshooting

The app writes `stdout.log` and `stderr.log` to its data folder. To measure
performance, create an empty file named `profile.enable` there
(`touch <data folder>/profile.enable`): the next launch writes a frame-time
report to `stdout.log` every 300 frames. Delete the file to turn it off.

`gpuprofile.enable` turns on a GPU timing mode instead: each draw, clear and
upload is timed on its own (the game runs much slower while it is on), the
totals per shader go to `stdout.log` as `[GPU]` lines, and the fragment
shaders that cost the most are saved as `gpu_hot_<n>.frag`.

## Building

You need Linux x86_64 (or a Linux VM/container on macOS), the webosbrew
toolchain ([buildroot-nc4 releases](https://github.com/openlgtv/buildroot-nc4/releases),
run its `relocate-sdk.sh` once), CMake 3.21+, Python 3 and
`ares-package` (`npm install -g @webos-tools/cli`).

```sh
git submodule update --init
export WEBOS_SDK=$HOME/arm-webos-linux-gnueabi_sdk-buildroot
scripts/build-webos.sh
```

The IPK and its Homebrew Channel manifest land in `dist/`. The first build
downloads the SDL-webOS release it bundles.

## Roadmap

- Optionally re-encode decoded BC textures to ETC2 to save memory
- Internal resolution setting

## Credits

- [ACreTeam/ac-decomp](https://github.com/ACreTeam/ac-decomp): the decompilation (CC0)
- [flyngmt/ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port): the PC port (MIT)
- [zwaetschge/ACGC-Android-Port](https://github.com/zwaetschge/ACGC-Android-Port): the OpenGL ES 3 / mobile path this build uses
- [webosbrew](https://www.webosbrew.org/): toolchain, SDL-webOS and the Homebrew Channel

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Animal Crossing is a
trademark of Nintendo. This project is not affiliated with or endorsed by
Nintendo.

## Licence

MIT (see [LICENSE](LICENSE)) for the code in this repository.
