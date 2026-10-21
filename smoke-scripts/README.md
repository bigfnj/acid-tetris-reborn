# Port Smoke Scripts

These files are consumed by `--scripted-input=@path`. Each entry is `frame:key[:up]`, separated by commas.

- `live-line-clear.txt` exercises live movement plus the row-injection collapse smoke helper. Its
  keys come after the `--live-demo` opening collapse (32 steps: 8 per row, as in the original), since
  input is only read on live gameplay steps and the injection helper ignores presses during a
  collapse. (Before the October 2026 gameplay fix the script pressed at steps 2-8, inside the old
  48-step collapse, where the moves were refused and C restarted the collapse.)
- `pause-resume.txt` snapshots gameplay through `Esc`, navigates to `Return to Game`, and resumes.
- `high-score-exit.txt` is intended for `--reset-setup --topout-demo`; it uses the top-out advance/release gates, stages and releases the default-name commit after reveal, then exits through the footer gate.
