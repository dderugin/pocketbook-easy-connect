# C source organization

- `ui_layout.c/.h`: logical canvas, named control/text rectangles, visibility and hit testing.
  Change geometry here; drawing and touch handling read the same definitions.
- `ui_view.c`: drawing the common header/footer and the SSH/FTP settings panels.
- `ui.c`: InkView events, editor callbacks, saved settings, notices and daemon actions.
- `ui.h`: application, editor and font state shared by the UI controller and renderer.
- `ec.h`: core API, configuration/service structures and shared limits.
- `config.c`, `credentials.c`, `services.c`, `doctor.c`: configuration/files,
  password/key state, daemon supervision, and diagnostics respectively.
- `main.c`: the command-line entry point.

UI coordinates are logical pixels, scaled from `UI_CANVAS_WIDTH`/`UI_CANVAS_HEIGHT`.
Test scripts use `press ssh-port`, `press ftp-tab`, etc. The InkView stub resolves
those control names and sends a real pointer event at the scaled button center.
Raw `tap x y` remains available for coordinate-specific checks. Geometry tests
check edge rules, overlap, hidden controls and gaps independently of rendering.

Use `.clang-format` for C formatting, for example:

```sh
clang-format -i src/*.c src/*.h tests/inkview_stub.c tests/test_ui_layout.c
```
