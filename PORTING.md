# Tracking and porting upstream

Pluto is a fork of [dwl].  The repository keeps dwl's history:

- `master` is the untouched upstream branch.
- `pluto` is this compositor.  It is based on dwl `5f12a5c` (`0.10-dev`,
  targeting wlroots 0.20).

The goal is to stay rebasable: upstream bug fixes and wlroots compatibility
changes should be portable in minutes, not by re-reading a 4000-line file.

## Syncing with upstream

```sh
git fetch origin                 # origin is the dwl remote
git switch pluto
git rebase origin/master         # or merge, if you prefer
```

When a conflict is in a split module, use the upstream function name to find
the module that owns it:

| upstream area                    | Pluto module |
|----------------------------------|--------------|
| `main`, `run`, `setup`, `cleanup`, `spawn`, `chvt`, `printstatus`, backend teardown | `src/main.c` |
| monitors, output layout, output-management, render loop | `src/output.c` |
| xdg-shell/XWayland clients, configure/map/unmap, transactions, popups, fullscreen | `src/client.c` |
| keyboard, pointer, cursor, drag-and-drop, constraints, pointer focus | `src/seat.c` |
| layer-shell, session lock | `src/layer.c` |
| tags, focus stack, `tile`, `monocle`, `mfact`, `nmaster`, `arrange` | `src/layout.c` |
| border/gap/column geometry (no wlroots) | `src/layout_math.c` |
| idle inhibit, foreign-toplevel capture | `src/protocols.c` |
| input-method v2 / text-input v3 | `src/ime.c` |
| keybinding lookup | `src/config.c` |
| XWayland surface handlers | `src/xwayland.c` (`#ifdef XWAYLAND`) |
| `struct wl_listener` objects | `src/listeners.c` |

If upstream adds a function, put it in the module that owns the same concern
and add its prototype to `include/pluto.h`.  Do not reintroduce a global: add
the state to `struct Pluto` (or to `Client`/`Monitor`), and use `P.`.

If upstream adds a new listener object, add its definition to
`src/listeners.c` and its `extern` declaration to `include/pluto.h`.

## wlroots upgrades

`config.mk` selects the wlroots pkg-config name:

```make
WLR_INCS = `$(PKG_CONFIG) --cflags wlroots-0.20`
WLR_LIBS = `$(PKG_CONFIG) --libs wlroots-0.20`
```

On a wlroots major bump, change `wlroots-0.20` here, update the expected API
calls in the owning module, and re-check the generated protocol headers.

wlroots 0.20 headers include enum headers from wayland-protocols (for example
`<wayland-protocols/cursor-shape-v1-enum.h>`).  Some distributions do not
install them, so the Makefile generates both the compositor protocol headers
and the enum headers into `build/`:

- `build/xdg-shell-protocol.h`
- `build/wlr-layer-shell-unstable-v1-protocol.h`
- `build/wlr-output-power-management-unstable-v1-protocol.h`
- `build/wayland-protocols/*-enum.h`

If a future wlroots release references another enum header, add a rule to the
`GEN` list in the `Makefile`.  This is intentionally the only place that knows
about generated protocol files.

## Versioning

`VERSION` in `config.mk` is Pluto's own release line (`0.1.0`).  dwl's tags
remain in the repository for history and rebasing, but they are not used for
Pluto's `--version` output.

## Keeping the fork healthy

- Every new mutable state field goes into `struct Pluto`; no new file-scope
  variables.
- Every new layout-geometry rule goes into `layout_math.c` with a test.
- Every new wlroots call goes into the subsystem that already talks to that
  part of wlroots.  `layout_math.c` must stay wlroots-free.
- Every new protocol manager gets its own struct and listener objects; do not
  bolt it onto `main.c`.
- Rebase conflicts are a signal that a module boundary is wrong.  Fix the
  boundary rather than re-copying a large upstream block into one file.

[dwl]: https://codeberg.org/dwl/dwl
