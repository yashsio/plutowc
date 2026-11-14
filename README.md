# Pluto

Pluto is a small, dynamic tiling compositor for [Wayland], built on
[wlroots].  It is a fork of [dwl] that keeps dwl's compact,
compile-time-configured model and its full protocol/feature coverage, while
splitting the implementation into explicit, separately testable subsystems.

The layout model is dwm/Pluto-style: nine tags per output, a master/stack
`tile` layout, `monocle`, and a floating layout.  Gaps, borders, `mfact` and
`nmaster` are compile-time configuration in `config.h`.

- One `struct Pluto P` owns all compositor state.
- One source file per subsystem under `src/`.
- Pure layout geometry lives in `src/layout_math.c` and is unit tested.
- No mutable file-scope globals in the modules.
- Upstream dwl history is retained so the fork stays rebasable.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the module map and invariants, and
[PORTING.md](PORTING.md) for the upstream-tracking workflow.

## Requirements

- wlroots 0.20
- wayland-server, wayland-protocols, wayland-scanner
- libinput, libxkbcommon, pixman, libdrm
- XWayland support is optional and additionally needs xcb and xcb-icccm

## Build

```sh
make            # builds ./pluto and generates protocol headers in build/
make test       # runs the pure layout-geometry tests
sudo make install
```

`make` creates `config.h` from `config.def.h` if it does not exist.  Edit
`config.h` for your keybindings, rules, colors, gaps and layout defaults, then
rebuild.  Everything is compile-time configuration; there is no runtime config
file.

XWayland is disabled by default.  Enable it in `config.mk`:

```make
XWAYLAND = -DXWAYLAND
XLIBS = xcb xcb-icccm
```

## Running

Start `pluto` from a TTY, or select it from your display manager:

```sh
pluto                          # start the compositor
pluto -s "foot &"              # run a startup command
pluto -d                       # debug logging
pluto -v                       # print the version
```

## Layout

`layouts[]` in `config.h` selects the layouts bound to `setlayout`:

| symbol | layout   | description                                  |
|--------|----------|----------------------------------------------|
| `[]=`  | `tile`   | `nmaster` master clients left, stack right   |
| `[M]`  | `monocle`| every tiled client fills the work area       |
| `><>`  | floating | clients keep their own geometry              |

`gappx` is the space between tiled clients and `outergappx` the space between
clients and the usable-area edge.  `borderpx` is the server-side border width.

## License

Pluto is distributed under the GNU General Public License v3.0 or later, the
same license as dwl.  See [LICENSE](LICENSE).  The files derived from dwm,
sway and tinywl retain their original licenses in `LICENSE.dwm`,
`LICENSE.sway` and `LICENSE.tinywl`.

[Wayland]: https://wayland.freedesktop.org/
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots
[dwl]: https://codeberg.org/dwl/dwl
