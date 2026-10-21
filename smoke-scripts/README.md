# Port Smoke Scripts

These files are consumed by `--scripted-input=@path`. Each entry is `frame:key[:up]`, separated by commas.

- `live-line-clear.txt` exercises live movement plus the row-injection collapse smoke helper.
- `pause-resume.txt` pauses with an `Esc` release (the original reads the in-game Esc from the key-release history), waits out the 48-step row reveal, moves Up to `Return to Game` (a release, then the pulse gate) and resumes with an Enter release (gate, 48-step conceal). Run with `--live-demo` (palette fades off) for at least 220 frames.
- `high-score-exit.txt` is intended for `--reset-setup --topout-demo`; it uses the top-out advance/release gates, commits the empty name with an Enter release after the 48-step table reveal (the entry row then settles through its gate), and leaves the footer with a second Enter release.

Menu actions follow the original: Enter, Up and Down act on their RELEASE, nothing acts during a reveal or conceal, and every move or exit first waits for the selected row's pulse gate (up to 17 ticks), so scripts need a key-up event and some slack.
