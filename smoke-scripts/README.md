# Port Smoke Scripts

These files are consumed by `--scripted-input=@path`. Each entry is `frame:key[:up]`, separated by commas.

- `live-line-clear.txt` exercises live movement plus the row-injection collapse smoke helper.
- `pause-resume.txt` snapshots gameplay through `Esc`, navigates to `Return to Game`, and resumes.
- `high-score-exit.txt` is intended for `--reset-setup --topout-demo`; it taps Return, Space and Backspace during the dissolve (ignored, as in the original), waits on the GAME OVER screen, holds Esc (the press does nothing) and releases it at 1006 to start the high-score path, then stages and releases the default-name commit after reveal and exits through the footer gate.
