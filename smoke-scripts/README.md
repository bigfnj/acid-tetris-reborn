# Port Smoke Scripts

These files are consumed by `--scripted-input=@path`. Each entry is `frame:key[:up]`, separated by commas.

- `live-line-clear.txt` exercises live movement plus the row-injection collapse smoke helper.
- `pause-resume.txt` snapshots gameplay through `Esc`, navigates to `Return to Game`, and resumes.
- `high-score-exit.txt` is intended for `--reset-setup --topout-demo`; it uses the top-out advance/release gates, stages and releases the default-name commit after reveal, then exits through the footer gate.
