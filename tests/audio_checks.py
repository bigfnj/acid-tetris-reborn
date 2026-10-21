#!/usr/bin/env python3
"""Audio behaviour checks for the port (Python 3 standard library only; Linux and Windows).

Every check drives the shipped binary headless through shipped interfaces only (--debug-state,
--scripted-input, the "SFX mix:" log line, SDL's disk audio driver, the PE import table) and
asserts an expectation taken from the reverse-engineered original, named in each check. They need
the player's own ATET.DAT beside the binary, like the other suites.

usage: python3 tests/audio_checks.py <exe> [--only name[,name...]] [--list]
Exit status 0 when every selected check passes.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

SFX_MIX = re.compile(r"SFX mix: slot=(\d+) vol%=(\d+) engine=(\d+)/64 pan=(\d+) class=(\d+)")
MUSIC_STATE = re.compile(r"music_track=(-?\d+) music_ramp=(\w+) music_ramp_left=(-?\d+) music_vol=(\d+)")


def run(exe, args, env_extra=None, timeout=600):
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    if env_extra:
        env.update(env_extra)
    proc = subprocess.run([exe] + list(args), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout, cwd=os.path.dirname(os.path.abspath(exe)))
    return proc.returncode, proc.stdout.decode("utf-8", "replace").replace("\r", "")


def pe_imports(path):
    """Lower-case DLL names in the import and delay-import tables of a PE file (None if not PE)."""
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    coff = pe + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    sections = []
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        sections.append((va, max(vsize, rsize), raw))

    def rva2off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        return None

    names = []
    for index, stride, name_field in ((1, 20, 12), (13, 32, 4)):
        rva = struct.unpack_from("<I", data, dirs + 8 * index)[0]
        off = rva2off(rva) if rva else None
        while off is not None and off + stride <= len(data):
            name_rva = struct.unpack_from("<I", data, off + name_field)[0]
            if name_rva == 0:
                break
            no = rva2off(name_rva)
            if no is None:
                break
            names.append(data[no:data.index(b"\0", no)].decode("ascii", "replace").lower())
            off += stride
    return names


def check_no_winmm(exe):
    """MU-14: music must not go through WinMM (2 x 120 ms buffers fed from the main loop); the
    original refills its mixer from the 60 Hz timer interrupt."""
    names = pe_imports(exe)
    if names is None:
        return True, "not a PE binary (non-Windows build): no WinMM path exists"
    if "winmm.dll" in names:
        return False, "imports winmm.dll (libmikmod drv_win)"
    return True, "no winmm.dll import (%d DLLs: %s)" % (len(names), ", ".join(sorted(set(names))))


def decode_raw(path, fmt, channels):
    data = open(path, "rb").read()
    if "F32" in fmt:
        n = len(data) // 4
        vals = struct.unpack("<%df" % n, data[:n * 4])
        scale = 1.0
    elif "S32" in fmt:
        n = len(data) // 4
        vals = struct.unpack("<%di" % n, data[:n * 4])
        scale = 2147483648.0
    elif "S16" in fmt:
        n = len(data) // 2
        vals = struct.unpack("<%dh" % n, data[:n * 2])
        scale = 32768.0
    elif "U8" in fmt:
        vals = [v - 128 for v in data]
        scale = 128.0
    else:
        raise ValueError("unknown disk format " + fmt)
    frames = len(vals) // channels
    loud = 0
    for f in range(frames):
        peak = max(abs(vals[f * channels + c]) for c in range(channels)) / scale
        if peak > 64.0 / 32768.0:
            loud += 1
    return frames, loud


def check_sdl_device_music(exe):
    """MU-03/MU-14: the music reaches the SDL audio device (every platform; the shipped port sent it
    to WinMM on Windows and to libmikmod's nosound driver elsewhere). Uses SDL's disk driver."""
    names = pe_imports(exe)
    if names is not None and "winmm.dll" in names:
        return False, "imports winmm.dll: music bypasses SDL (not run, it would play aloud through WinMM)"
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "device.raw")
        rc, out = run(exe, ["--reset-setup", "--no-fade", "--smoke-frames=2400"],
                      {"SDL_AUDIODRIVER": "disk", "SDL_AUDIO_DRIVER": "disk", "SDL_AUDIO_DISK_OUTPUT_FILE": raw})
        m = re.search(r"format=(\w+) channels=(\d+) freq=(\d+)", out)
        if rc != 0 or not m or not os.path.exists(raw):
            return False, "run failed (rc=%s, disk device line %s)" % (rc, "found" if m else "missing")
        fmt, channels, freq = m.group(1), int(m.group(2)), int(m.group(3))
        frames, loud = decode_raw(raw, fmt, channels)
    seconds = loud / float(freq)
    ok = seconds >= 0.5
    return ok, "%s %d ch %d Hz: %.2f s of %.2f s above -54 dBFS" % (fmt, channels, freq, seconds, frames / float(freq))


def sfx_lines(out):
    return [tuple(int(x) for x in m.groups()) for m in SFX_MIX.finditer(out)]


def check_sfx_voice_class(exe):
    """SX-03/AL-18: 0x6817 plays on voice (channels + class); the class is 0 only for the lock
    (slot 0) and the menu blips (slot 1), 1 for every other effect."""
    rc, out = run(exe, ["--live-demo", "--scripted-input=1:down,200:c,201:c:up,260:o,261:o:up", "--smoke-frames=320"])
    lines = sfx_lines(out)
    bad = [(s, c) for s, _, _, _, c in lines if c != (0 if s in (0, 1) else 1)]
    others = sorted(set(s for s, _, _, _, _ in lines if s not in (0, 1)))
    locks = [s for s, _, _, _, _ in lines if s == 0]
    ok = rc == 0 and not bad and len(others) >= 2 and len(locks) >= 1
    return ok, "slots seen %s, wrong class %s" % (sorted(set(s for s, *_ in lines)), bad[:6])


def first_lock_pan(exe, seed, script):
    rc, out = run(exe, ["--live-demo", "--start-level=0", "--piece-seed=%d" % seed, "--scripted-input=" + script,
                        "--smoke-frames=400"])
    for s, vol, eng, pan, cls in sfx_lines(out):
        if s == 0:
            return pan
    return None


def check_lock_pan(exe):
    """SX-04: lock pan = ((x - 5) * 21 + 0x80) & 0xFF on the piece's reference x (its authored 4x4
    box origin), not its leftmost column. The finding's own examples: an O against the left wall
    237 (x = -2), a T dropped from the spawn on columns 4-6 86 (x = 3), a vertical I in column 9
    170 (x = 7)."""
    cases = [
        ("O at left wall", 8, "1:left,1:down", 237),
        ("spawn T", 4, "1:down", 86),
        ("vertical I col 9", 10, "52:a,54:a:up,56:right,56:down", 170),
    ]
    results = []
    ok = True
    for name, seed, script, want in cases:
        got = first_lock_pan(exe, seed, script)
        results.append("%s %s (want %d)" % (name, got, want))
        ok = ok and got == want
    return ok, "; ".join(results)


def music_states(out):
    states = []
    for line in out.splitlines():
        if not line.startswith("Debug state:") and "Debug state:" not in line:
            continue
        m = MUSIC_STATE.search(line)
        if m:
            states.append((int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4))))
    return states


def check_music_cold_start(exe):
    """MU-09 (music side) / MU-15 context: the track loader 0x6544 always arms a 0x20-tick fade-out
    and waits it out before loading, also at startup with nothing playing (oracle frames 750-781)."""
    rc, out = run(exe, ["--reset-setup", "--no-fade", "--debug-state", "--smoke-frames=900"])
    st = music_states(out)
    first = next((i for i, s in enumerate(st) if s[1] != "none"), None)
    if first is None:
        return False, "no ramp seen"
    run_len = 0
    for s in st[first:]:
        if s[1] == "fadeout" and s[0] == -1:
            run_len += 1
        else:
            break
    after = st[first + run_len] if first + run_len < len(st) else None
    ok = run_len == 32 and after is not None and after[0] == 0 and after[1] == "fadein"
    return ok, "first ramp %s; %d steps of fade-out with no track, then %s" % (st[first], run_len, after)


def check_music_fadein_end(exe):
    """MU-08: a fade-in ends at floor(31P/32) (96 % of 100, MikMod song volume 33) and nothing
    re-applies P, also not on New Game (oracle: mp 33 after every fade-in)."""
    rc, out = run(exe, ["--reset-setup", "--no-fade", "--debug-state", "--scripted-input=900:enter,902:enter:up",
                        "--smoke-frames=1000"])
    st = music_states(out)
    last_fadein = max((i for i, s in enumerate(st) if s[1] == "fadein"), default=None)
    if last_fadein is None:
        return False, "no fade-in seen"
    settled = [s[3] for s in st[last_fadein + 2:]]
    ok = bool(settled) and all(v == 96 for v in settled)
    return ok, "after the fade-in: music_vol values %s over %d steps" % (sorted(set(settled)), len(settled))


def check_exit_fade_monotonic(exe):
    """MU-VP-01 (music side): once Exit Game runs its 0x40-tick fade, a second Enter cannot restart
    or replace it (the original ignores every key after Exit Game)."""
    downs = ",".join("%d:down,%d:down:up" % (900 + 10 * k, 902 + 10 * k) for k in range(6))
    script = downs + ",970:enter,972:enter:up,990:enter,992:enter:up"
    rc, out = run(exe, ["--reset-setup", "--no-fade", "--debug-state", "--scripted-input=" + script,
                        "--smoke-frames=1200"])
    st = music_states(out)
    start = next((i for i in range(len(st)) if i > 0 and st[i][1] == "fadeout" and st[i][2] == 64), None)
    if start is None:
        return False, "exit fade not seen"
    lefts = []
    for s in st[start:]:
        if s[1] != "fadeout":
            break
        lefts.append(s[2])
    restarts = sum(1 for a, b in zip(lefts, lefts[1:]) if b >= a)
    ok = restarts == 0 and len(lefts) >= 60
    return ok, "exit fade %d steps, %d non-decreasing steps (first %s)" % (len(lefts), restarts, lefts[:4])


def music_row_presses(exe, enter_steps):
    """Main menu, Down twice to the Music row, Enter at each step; returns the music states."""
    keys = ["900:down", "902:down:up", "910:down", "912:down:up"]
    keys += ["%d:enter,%d:enter:up" % (s, s + 2) for s in enter_steps]
    rc, out = run(exe, ["--reset-setup", "--no-fade", "--debug-state", "--scripted-input=" + ",".join(keys),
                        "--smoke-frames=1250"])
    return rc, music_states(out)


def check_music_row_queue(exe):
    """MU-VO-01 / MU-VP-03 (music side): the original's track loader 0x6544 blocks the menu, and a
    Music-row press released during it is acted on after the load as a change of its own, so the
    in-between track really loads (oracle mu-v-rowchange2: track 1 loads, then track 2) and the
    music always ends on the track the row shows. The shipped port restarted the fade-out on every
    press, skipped the in-between track, and after six quick presses kept the fifth choice playing
    while the row showed the first (label 0, music 5)."""
    rc1, two = music_row_presses(exe, [930, 945])
    loaded = []
    for s in two:
        if s[0] >= 0 and (not loaded or loaded[-1] != s[0]):
            loaded.append(s[0])
    rc2, six = music_row_presses(exe, [930 + 8 * k for k in range(6)])
    final_six = six[-1] if six else None
    settled = final_six is not None and final_six[1] == "none"
    ok = rc1 == 0 and rc2 == 0 and loaded == [0, 1, 2] and settled and final_six[0] == 0
    return ok, "two presses load %s (want [0, 1, 2]); six presses end on track %s, ramp %s (want 0, none)" % (
        loaded, final_six[0] if final_six else None, final_six[1] if final_six else None)


CHECKS = [
    ("no-winmm", check_no_winmm),
    ("sdl-device-music", check_sdl_device_music),
    ("sfx-voice-class", check_sfx_voice_class),
    ("lock-pan", check_lock_pan),
    ("music-cold-start", check_music_cold_start),
    ("music-fadein-end", check_music_fadein_end),
    ("exit-fade-monotonic", check_exit_fade_monotonic),
    ("music-row-queue", check_music_row_queue),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--only", default="")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        for name, fn in CHECKS:
            print(name)
        return 0
    only = set(x for x in a.only.split(",") if x)
    exe = os.path.abspath(a.exe)
    failures = 0
    for name, fn in CHECKS:
        if only and name not in only:
            continue
        try:
            ok, detail = fn(exe)
        except Exception as exc:  # a crash is a failure, never a skip
            ok, detail = False, "exception: %r" % (exc,)
        print("  %s: %s  %s" % ("PASS" if ok else "FAIL", name, detail))
        failures += 0 if ok else 1
    print("ALL AUDIO CHECKS PASSED" if failures == 0 else "AUDIO CHECK FAILURES: %d" % failures)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
