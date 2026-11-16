# Architecture

Pluto is a fork of dwl.  dwl deliberately keeps the whole compositor in one
`dwl.c`; that is easy to read once and painful to maintain as features and
wlroots APIs accumulate.  Pluto keeps dwl's behaviour and compile-time
configuration but makes ownership and module boundaries explicit.

## The single context

All mutable compositor state lives in one instance:

```c
/* include/pluto.h */
struct Pluto {
    struct wl_display *dpy;
    struct wlr_backend *backend;
    struct wl_list clients;
    struct wl_list mons;
    Monitor *selmon;
    ...
};

extern struct Pluto P;
```

`src/main.c` defines it:

```c
struct Pluto P;
```

Rules:

1. Modules access state through `P`, never through new file-scope variables.
2. A module may define `static` helpers; it must not define mutable globals.
3. Types shared by more than one module belong in `include/pluto.h`.  Types
   private to a module stay in that module's `.c` file.
4. `include/client.h`, `include/ime.h` and `include/util.h` are the only
   remaining header-only helpers inherited from upstream.  They are included by
   `pluto.h`; do not add new state to them.

The listener objects are the one deliberate exception to "no globals".  They are
`struct wl_listener` values with no per-instance state, defined in
`src/listeners.c` and declared `extern` in `include/pluto.h`.  Keeping them in
one file makes the set of compositor events visible in one place and avoids the
same handler being registered through different static objects in different
translation units.

## Modules

| file                  | responsibility                                                        |
|-----------------------|-----------------------------------------------------------------------|
| `src/main.c`          | `main`, backend/protocol setup, event loop, spawn/chvt, shutdown      |
| `src/output.c`        | `Monitor` lifecycle, output layout, `wlr-output-management`           |
| `src/client.c`        | xdg-shell/XWayland clients, configure, map/unmap, transactions, popups|
| `src/seat.c`          | keyboard, pointer, cursor, drag-and-drop, pointer constraints         |
| `src/layer.c`         | layer-shell surfaces and session lock                                 |
| `src/layout.c`        | tags, focus stack, tile/monocle arrangement, `mfact`/`nmaster`        |
| `src/layout_math.c`   | pure box geometry for tile/monocle; no wlroots dependency             |
| `src/protocols.c`     | small protocol managers: idle inhibit, foreign-toplevel capture       |
| `src/ime.c`           | input-method v2 and text-input v3 bridge                              |
| `src/config.c`        | compile-time keybinding lookup                                        |
| `src/xwayland.c`      | XWayland surface handlers (only built with `-DXWAYLAND`)              |
| `src/listeners.c`     | stateless `wl_listener` objects shared by the modules                 |

Headers:

| file                  | contents                                                              |
|-----------------------|-----------------------------------------------------------------------|
| `include/pluto.h`     | system includes, core types, `struct Pluto`, prototypes, listener externs |
| `include/layout_math.h` | `struct Box` and the pure layout functions                          |
| `include/client.h`    | inline helpers over `Client` (upstream)                               |
| `include/ime.h`       | input-method types and prototypes                                     |
| `include/util.h`      | `die`, `ecalloc`, small xkb helper                                    |
| `config.h`            | user configuration, generated from `config.def.h`                     |

## Event flow

```
libinput/backend event
        |
        v
wl_listener in src/listeners.c
        |
        v
handler in the owning subsystem (client.c, seat.c, ...)
        |
        +--> mutate P / Client / Monitor
        |
        +--> arrange(mon)  ->  layout_math slot -> resize()
        |
        +--> transactionschedule()  ->  scene graph commit
```

`layout_math.c` is intentionally below the wlroots boundary:

```c
struct Box layout_tile_slot(struct Box area, int n, int index,
                            const struct TileOptions *opts);
```

It takes plain integers and returns a plain box, so a change to tiling maths can
be validated with `make test` without a display, a backend, or even wlroots
headers.  `src/layout.c` is the only adapter between `struct wlr_box` and
`struct Box`.

## Configuration boundary

`config.h` is included once, at the end of `include/pluto.h`, after all types
and function prototypes are visible.  It may therefore refer to `Layout`,
`Rule`, `Arg`, key symbols and handler functions, exactly like dwl.  A module
must not include `config.h` directly; it gets it through `pluto.h`, which keeps
configuration order deterministic.

Configuration is compile-time only.  There is no config parser, IPC or reload
protocol.  Rebuild after editing `config.h`.

Because `config.h` is textually included in every translation unit, each
`static const` table has a distinct address per module.  Never compare config
objects by address; compare by value, or give the table a single shared
definition.  In particular, layout identity must not rely on `&layouts[i]`.

## Adding a feature

- A new window-manager policy: add the handler to `src/layout.c` (or a new
  `src/<name>.c`), declare it in `pluto.h`, and bind it in `config.h`.
- A new protocol manager: keep the manager struct in `src/protocols.c`, add any
  listener objects to `src/listeners.c`, and add extern declarations to
  `pluto.h`.
- A new layout: add a pure slot function to `layout_math.c`/`.h`, unit test it,
  and add a thin `arrange`-style adapter in `layout.c`.
- A wlroots API bump: change only the owning subsystem module and, if the
  protocol enum set changes, `Makefile`.  Layout maths and configuration should
  not need to move.

Do not grow a new monolith: if a change touches more than one concern, add a
unit or split the file rather than adding another thousand lines to an existing
one.
