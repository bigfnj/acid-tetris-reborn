# Port Smoke Scripts

These files are consumed by `--scripted-input=@path`. Each entry is `frame:key[:up]`, separated by commas.

- `live-line-clear.txt` exercises live movement plus the row-injection collapse smoke helper. Its
  keys come after the `--live-demo` opening collapse (32 steps: 8 per row, as in the original), since
  input is only read on live gameplay steps and the injection helper ignores presses during a
  collapse. (Before the October 2026 gameplay fix the script pressed at steps 2-8, inside the old
  48-step collapse, where the moves were refused and C restarted the collapse.) C is a developer key:
  run it with `--debug`.
- `pause-resume.txt` pauses with an `Esc` release (the original reads the in-game Esc from the key-release history), waits out the 48-step row reveal, moves Up to `Return to Game` (a release, then the pulse gate) and resumes with an Enter release (gate, 48-step conceal). Run with `--live-demo` (palette fades off) for at least 220 frames.
- `high-score-exit.txt` is intended for `--reset-setup --topout-demo`; it taps Return, Space and Backspace during the dissolve (ignored, as in the original), waits on the GAME OVER screen, holds Esc (the press does nothing) and releases it at 1006 to start the high-score path, commits the empty name with an Enter release after the 48-step table reveal (the entry row then settles through its gate), and leaves the footer with a second Enter release.

Menu actions follow the original: Enter, Up and Down act on their RELEASE, nothing acts during a reveal or conceal, and every move or exit first waits for the selected row's pulse gate (up to 17 ticks), so scripts need a key-up event and some slack.
