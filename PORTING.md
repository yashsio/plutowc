# Tracking and porting upstream

Pluto is a fork of [dwl].  This repository has a single `main` branch and does
not retain upstream dwl history or configure an upstream remote.  It is based
on dwl `5f12a5c` (`0.10-dev`, targeting wlroots 0.20).

The goal is to keep upstream fixes portable: find the upstream function, then
apply the change to the module that owns it.  Keep a separate dwl clone as the
reference rather than adding an upstream remote to this repository.

## Syncing with upstream

```sh
git clone https://codeberg.org/dwl/dwl /tmp/dwl-upstream
cd /tmp/dwl-upstream
git log --oneline -- dwl.c
git diff <old>..<new> -- dwl.c
```

Port each change into the module below.  XWayland-only changes stay inside the
`#ifdef XWAYLAND` blocks or `src/xwayland.c`.

Use the upstream function name to find the module that owns it:

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

`VERSION` in `config.mk` is Pluto's own release line (`0.1.0`).  This
repository does not carry dwl tags, and `--version` is not derived from them.

## Keeping the fork healthy

- Every new mutable state field goes into `struct Pluto`; no new file-scope
  variables.
- Every new layout-geometry rule goes into `layout_math.c` with a test.
- Every new wlroots call goes into the subsystem that already talks to that
  part of wlroots.  `layout_math.c` must stay wlroots-free.
- Every new protocol manager gets its own struct and listener objects; do not
  bolt it onto `main.c`.
- A change that does not fit one module is a signal that the boundary is wrong.
  Fix the boundary rather than copying a large upstream block into one file.

[dwl]: https://codeberg.org/dwl/dwl
