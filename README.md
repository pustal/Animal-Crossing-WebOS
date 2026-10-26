# Animal Crossing Port for WebOS TVs

<img src="webos/largeIcon.png" alt="App icon" width="96" align="right">

A native LG webOS TV build of the Animal Crossing (GameCube) decompilation,
running on the TV's own GPU through OpenGL ES 3. You need your own copy of the
game: no game data is included.

> **Status: early.** The game boots and runs on webOS 6. The file-transfer
> page and USB support are still to come (see [Roadmap](#roadmap)).

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
| `webos_gpu_profile.c` | Optional GPU timing mode for finding expensive draws (see Troubleshooting) |
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
2. Start the app once. It creates its data folder (`$HOME/data` of the app)
   with these subfolders:

   ```
   rom/            put GAFE01.iso here (the US disc image)
   save/card_a/    memory card A (GCI saves)
   save/card_b/
   texture_pack/   optional custom textures
   stdout.log, stderr.log, settings.ini
   ```

3. Until the transfer page exists, copy the disc image over SSH (rooted TV).
   On a developer-mode install the folder is
   `/media/developer/apps/usr/palm/applications/org.animalcrossingwebos.app/data`:
   `scp GAFE01.iso root@<tv>:<that folder>/rom/`.

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

- Transfer page in the browser (upload the disc image and saves), on its own port
- USB drive support: report free space, load `Animal Crossing/texture_pack/` from it
- Internal resolution setting, CPU load check

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
