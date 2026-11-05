# Easy Connect 0.2.0 UI previews

These PNGs are renders of the actual C drawing code using the host InkView test
adapter, the PB632 native theme resources and the native Roboto application-title
font. They are not physical-device screenshots. Other readers can use different
sizes and firmware styling.

- UI source: release `0.2.0`, commit `6f44a30d80195bdc30f0c8d9911789696ab53db8`.
- Portrait canvas: 1072 × 1448, matching Touch HD 3.
- Generator: `tools/preview-ui.py`, with `--width 1072 --height 1448`,
  `--theme-reference work/native-ui-reference/pb632` and
  `--native-font work/native-ui-reference/pb632/Roboto-Regular.ttf`.
- Example configuration: SSH “Key or password”, FTP “Password”, both stopped.
- `192.0.2.1` and `ec-preview` are example connection values, not reader details.

Only the final PNG previews are published. Firmware theme archives, extracted
bitmap resources and font files remain outside the repository and downloads.
