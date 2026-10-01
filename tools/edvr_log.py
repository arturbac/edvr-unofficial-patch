#!/usr/bin/env python3
"""Find the log a flight just wrote, and say whether it is the build you made.

    python tools/edvr_log.py --target steam --version
    python tools/edvr_log.py --target steam --expect-build HEAD
    python tools/edvr_log.py --target steam --grep "Stats\\[4[0-9]\\]"
    python tools/edvr_log.py --target steam --tail 80
    python tools/edvr_log.py --target frontier --tally vh
    python tools/edvr_log.py --target frontier --tally vh --frame 1
    python tools/edvr_log.py --target frontier --tally periodic --expect-build HEAD
    python tools/edvr_log.py --target frontier --tally periodic --window-ms 250
    python tools/edvr_log.py --target frontier --tally periodic --infer-runs
    python tools/edvr_log.py --target frontier --camera-census --expect-build HEAD
    python tools/edvr_log.py --target frontier --maps-sharp --expect-build HEAD
    python tools/edvr_log.py --target frontier --vscreen-fit --expect-build HEAD
    python tools/edvr_log.py --target epic --flat-upscale --expect-build HEAD
    python tools/edvr_log.py --target frontier --vr-supersampling --expect-build HEAD
    python tools/edvr_log.py --list

This is the sanctioned replacement for `Get-Content <some path> -Tail 200 |
Select-String ...`, retyped once a flight for a month.

The first line of any flight-log analysis is not "what do the counters
say", it is "is this log from the build I just installed". EDVR writes
that into every log it opens:

    version 0.14.1-93-gf78eba4 (build 68C0A1F2) -- this DLL was linked ...

--expect-build compares that against a git ref (HEAD by default) or a
literal string, and exits 2 when it does not match. A session has been
spent reading counters off a log that a stale DLL wrote; the exit code is
there so a script can refuse to go on.

Log names are edvr_<tag>_YYYYMMDD_HHMMSS.log, where the tag is `gfx` for
the d3d11 half and `vr` for the openvr half; a second process opening the
same tag in the same second gets millisecond and pid fields appended,
edvr_<tag>_YYYYMMDD_HHMMSS_mmm_pid.log. Native OpenXR logs always carry those
fields: edvr_openxr_YYYYMMDD_HHMMSS_mmm_pid.log. New native logs always land
in edvr_logs\\ beside the executable, with local-time names and UTC body
timestamps. Older native logs beside the DLL have UTC names, and no fields.
Legacy logs honor log.dir in edvr.ini, and this reads
that key rather than assuming the default -- a redirected log directory
is exactly when you would rather not be told there are no logs.

Read-only by construction: it opens files for reading and nothing else,
which is why it has no --dry-run.

--tally vh aggregates a draw-census log instead of dumping lines: it
counts eye-texture DC lines per vh= shader-content hash, splits each
hash's count by its r= render-target token (the per-eye view), and
averages the n=/i= draw arguments, with the DC frame summary lines as
the totals row. The census caps its log output at 16384 lines
(draw_census.cpp), so a long census keeps per-draw detail only for the
first frames; --tally says which frames survive only as summaries
rather than printing an empty table.

--tally periodic answers one question about one flight: which periodic work
coincides with long frames. It reads the graphics log and the runtime log
that opened nearest it (--runtime-file names one), takes every `periodic
work:` line (src/common/periodic_work.h: a summary per 30 s window and the
SLOW runs, each with the local time the run finished) and every long frame
(the graphics log's LONG FRAME lines, the runtime's native_long_cycle
lines), puts all of it on one local clock, and counts per operation the long
frames that come within --window-ms of one of its events. A long frame ends
at its line's time and lasts its ms, so an event inside it counts. The two
logs use different clocks; the report says which. The runtime log's UTC
offset is read off its own file name (local) against its first line (UTC),
not from the machine running this, so a supporter's logs convert too. Only
what the timing wrote can be matched -- a summary names one run per 30 s and
SLOW lines are limited per operation -- so a long frame with no event beside
it does not show the operation idle; the report prints the number of matches
chance alone would give, and says plainly when a log has no `periodic work:`
lines at all or the runtime log is missing. --infer-runs adds the runs nobody
logged for an operation on a timer whose cadence two logged times confirm (the
journal re-glob: every 4 s, slow every time, a logged time for one run in
three), marked est. A flight may cross midnight; a DST change inside one is
not handled.

--camera-census reads a VR flight flown with advanced.vr_camera_census = on
(design doc section 82). It prints the census's 5 s lines, the camera table, each
camera's role read from the KIND OF EACH of its logged calls (a camera object is
no identity: one was the left eye camera in the cockpit and is the world's kind-3
camera on foot; a second projection on the same object is its own role), each
logged on-foot frame's call sequence reduced to runs of (kind, caller, tone),
the eye composite draws' own constant-buffer rows, and then the OFFLINE JOIN: each
eye draw's rows matched to the rows the composer produced for every logged call
(equal within 1e-5). The cameras that match are the eye cameras; the report says
which of the six candidate signals (A call signature, B content join, C place
against the tone draw, D caller, E tangents against the advertised eye frusta,
F view) tells them from the world's camera. It ends with the STAGE 2 VERDICT, PASS
/ WARN / STOP lines on the injected flight (the world route's phase in the kind-3
cameras): (i) the eye rows must not move (|leak| below 1e-6 NDC while the frame's
phase is non-zero), (ii) the kind-3 calls' rows must carry the phase, (iii) only
kind 3 is injected, (iv) nothing ran off-thread or unreadable, (v) the roles, (vi)
nothing was injected on a frame whose window the route had shut. The verdict reads
the route's `vr world route 5s:` and `vr world route inject 5s:` lines and a few of
its own log lines too; a log that predates stage 2 gets n/a lines, never a crash.
With the census key on, the route also prints a refusal-census line a window
(`vr world route refusal 5s:`, the stage 2 experiment build; with the steady-detail key on, which
is its default, the line is printed with the census off as well, carrying only the depth check's
frame counts: such a window is `census-off`, never a census that failed): the report gets a section
with the share of the treated pixels whose history the resolver's prep refused, per
window and by cause (stale slot, masked record, ...), the state of the steady-detail
key and of the refusal view in each window, and totals by key state; (v) also checks
the weapon's role (inj-fp against the fold-in's mode counts and the struct's fov range).
A log with no census lines exits 1; the verdict never changes the exit code (read
its lines).

The census's EPISODES (design-world-camera-motion-2026-09-30.md section 6, Phase 0)
add three sections before the verdict: `== episodes ==` (per episode, one frame
sampled 30 frames after a trigger, aboard frames included: the trigger, the journal,
GuiFocus and the naming, the calls by kind and caller, the printed calls, the join of
each depth's first draw to the calls that composed its b1 rows, and the temporal
pass's chosen rows against the calls' view axes, then the facts for H1, H2 and H3),
`== on-foot naming runs ==` (the run-length histogram of the 5 s windows summed) and
`== the detour's CPU ==` (the observer halves' sampled cost). A log with none of those
lines reports exactly as before.

--maps-sharp reads a flight with experimental.on_foot_maps_sharp = on (design-world-
camera-motion-2026-09-30.md, Phase 1). It prints each map or menu the UI layer held as
a panel period (the TAKES line and the HANDS BACK line that closed it: when, how many
frames, how many eyes went through the layer-only door and how many kept the upscaler,
and why it handed back), the `on foot maps sharp 5s:` windows summed, whether the VR
world route let go with the gate and owned the world again after, and a PASS / WARN /
STOP verdict. Unlike the other readers its exit code carries the verdict: 0 for PASS
or WARN, 1 for STOP, 3 when the log has no line of the feature (the key was off, the
UI layer was not live, or the build predates it).

--vscreen-fit reads a flight with fix.vscreen_res_width = auto (design doc section 82,
the "vscreen auto-fit" entry). It prints the launch's rule line (`vScreen resolution:
auto = N wide: rule=fitted|legacy ...`: the rule that chose the width and why, from
which footprint, and which world-route condition failed when it is legacy), the
width the panel patch applied, the footprint instrument's arming line and every
`vscreen footprint 30s:` window (samples on foot and elsewhere, the screen's width in
eye pixels, its range and shape, the footprint at panel distance 1, the session
median, what is stored for the next launch and the width it would fit), then the
verdict lines, PASS / WARN / STOP / n/a: RULE (the width follows the rule's own
tokens and is what was applied), INSTRUMENT (it ran, saw the composite and read its
sources), ON FOOT, STABLE, SHAPE (the footprint's pixel aspect is 16:9), DISTANCE LAW
(the footprint at distance 1 agrees across panel distances), CALIBRATION (Sean's
3504 at distance 0.7 on a 4032 px eye) and STORED. A log with none of these lines
exits 1; the verdict never changes the exit code (read its lines).

--flat-upscale reads a flat-profile flight (design doc section 83): the game's final copy admitted by
its structure, so DLSS, FSR and TAA resolve below the output (Elite's supersampling under 1.0) whatever bloom,
depth of field and the tone variant do. It prints the key line, each `flat route:` (R, E, D), the first
admission and every decline (`flat copy structure:`), each `flat copy structure 5s:` window (the game's copies
split into the whitelist's, the structure's admissions, its declines by cause, no scene, a render size that does
not fit, the HDR route's, and the key off's; the scene, output and source sizes; the longest chain of R-sized image
passes between the scene and the copy), the stand-down and F8 warning lines with the render size's words, and the
refusals summed, then PASS / WARN / STOP / n/a lines: KEY, ADMISSION (did the instrument run), TREATED, UPSCALE
(below the output, admitted or selected, treated), TONE REFUSALS, STAND-DOWN (no-3d-scene is a silent startup; a
render size is the user's resolution; a tone refusal with declines is a game anti-aliasing chain), F8 WARNING (the
false startup warning is a STOP), CHAIN (a game anti-aliasing filter the structure declines) and ADVICE (the old
supersampling paragraph must be gone). --vr-supersampling reads a VR flight: the `vr supersampling:` line (Elite's
Supersampling below 1, from the measured render size against the eye texture), vScreen's own adoption line it
follows and the menu's note that the headset notice was queued, with NOTICE / CONSISTENT / HEADSET / FLAT lines
(a flat log carrying the notice is a STOP). Neither verdict changes the exit code (read its lines).

Exit 0 when a log was read, 1 when none was found (or --camera-census found no
census line, or --vscreen-fit no auto-fit line, or --flat-upscale no flat line, or --vr-supersampling no VR line), 2 when --expect-build did not
match (--tally periodic checks the runtime log against it too). --maps-sharp's codes
for a log it read are its own (above): 0, 1 and 3 mean a verdict, not "no log".
"""

import argparse
import bisect
import calendar
import datetime
import math
import os
import re
import statistics
import sys
import tempfile

GAME_EXE = "EliteDangerous64.exe"
LOG_RE = re.compile(r"^edvr_(?P<tag>[a-z0-9]+)_(?P<stamp>\d{8}_\d{6})"
                    r"(?:_(?P<ms>\d{3})_(?P<pid>\d+))?\.log$",
                    re.IGNORECASE)
# `version <string> (build <hex>)`, with the linked-at tail optional --
# log.cpp prints a shorter form when the timestamp will not convert.
#
# Every line Log::note() writes is prefixed `[HH:MM:SS.mmm] `, so the
# optional group is not decoration: anchored without it this matched the
# synthetic logs in the self-test and NOTHING in a real one. It still
# anchors at the start of the line rather than searching, so a sentence
# with the word "version" in it cannot be mistaken for the version note.
VERSION_RE = re.compile(r"^(?:\[[\d:.]+\]\s*)?version\s+(?P<ver>\S+)"
                        r"(?:\s+\(build\s+(?P<stamp>[0-9A-Fa-f]+)\))?")
NATIVE_VERSION_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} UTC "
    r"pid=\d+ tid=\d+ module_init,version=(?P<ver>[^,\s]+),durable_log=1$")

# Draw-census lines, written by src/d3d11/draw_census.cpp. Every note
# carries the [HH:MM:SS.mmm] prefix like anything else Log::note() writes.
# The per-draw head is fixed: "DC <frame> #<n> <type> n=.. i=.. r=.."; the
# variable tail (vs=/vh=/ia=/...) comes after. DC begin/end/frame/id and
# the DCC/DCL/DCS/DCX/DCS lines must not match: the frame-and-# anchor
# excludes them, so a grep for "] DC " counts spurious lines this doesn't.
CENSUS_DRAW_RE = re.compile(
    r"^(?:\[[\d:.]+\]\s*)?(?P<kind>DC|DCO) (?P<frame>\d+) #(?P<idx>\d+) "
    r"(?P<type>\S+) n=(?P<n>\d+) i=(?P<i>\d+) r=(?P<r>\S+)")
# vh= lives in the IA tail, which readDrawState can skip under budget
# pressure -- a draw line without it is real and lands in the "(none)"
# bucket. Anchored on whitespace: an unanchored search also matches the
# "pr=" token two fields later.
VH_RE = re.compile(r"(?:^|\s)vh=([0-9A-Fa-f]+)")
# "DC frame <n> draws=.. off=.. copies=.. disp=.. clears=.. unseen=.."
CENSUS_FRAME_RE = re.compile(
    r"^(?:\[[\d:.]+\]\s*)?DC frame (?P<frame>\d+) draws=(?P<draws>\d+) "
    r"off=(?P<off>\d+) copies=(?P<copies>\d+) disp=(?P<disp>\d+) "
    r"clears=(?P<clears>\d+) unseen=(?P<unseen>\d+)")
# "DC end census=.. draws=.. ... lines=<cap> ... truncated=<dropped>"
CENSUS_END_RE = re.compile(r"^(?:\[[\d:.]+\]\s*)?DC end\b")
CENSUS_LINES_RE = re.compile(r"\blines=(\d+)")
CENSUS_TRUNC_RE = re.compile(r"\btruncated=(\d+)")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_text(path):
    """Logs are ASCII in practice, but a shader name or a path can carry
    anything. Decode explicitly as UTF-8 -- never leave it to the console
    codepage, which is what turned an em-dash into mojibake on a public
    issue comment once."""
    with open(path, "rb") as f:
        return f.read().decode("utf-8", errors="replace")


def config_log_dir(game_dir):
    """log.dir out of the target's edvr.ini, if it sets one.

    A section-insensitive scan: the key is read as `log.dir` by the DLL,
    and the ini carries it under a [log] section as `dir`. Both spellings
    appear in the wild, so accept either rather than quietly finding
    nothing.
    """
    ini = os.path.join(game_dir, "edvr.ini")
    if not os.path.isfile(ini):
        return None
    section = ""
    try:
        text = read_text(ini)
    except OSError:
        return None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip().lower()
        val = val.strip().strip('"')
        full = key if "." in key else (section + "." + key if section else key)
        if full == "log.dir" and val:
            return val
    return None


def log_dir_for(game_dir):
    configured = config_log_dir(game_dir)
    if configured:
        return configured if os.path.isabs(configured) else \
            os.path.join(game_dir, configured)
    return os.path.join(game_dir, "edvr_logs")


def log_dirs_for(game_dir, tag):
    """Native OpenXR always follows the executable; legacy halves honor log.dir."""
    native = os.path.join(game_dir, "edvr_logs")
    legacy = log_dir_for(game_dir)
    if tag == "openxr":
        return [native]
    if tag == "all" and os.path.normcase(os.path.abspath(native)) != os.path.normcase(os.path.abspath(legacy)):
        return [legacy, native]
    return [legacy]


def find_logs(directory, tag=None):
    """Newest first. The name carries the timestamp, so it sorts without
    stat()ing anything -- and a file copied off another rig keeps the time
    it was written rather than the time it was copied."""
    if not os.path.isdir(directory):
        return []
    out = []
    for name in os.listdir(directory):
        m = LOG_RE.match(name)
        if not m:
            continue
        suffixed = m.group("ms") is not None
        if m.group("tag").lower() == "openxr" and not suffixed:
            continue   # an older native log with a UTC name, not this build's
        if tag and m.group("tag").lower() != tag.lower():
            continue
        # A legacy log opened in the same second as another carries the suffix
        # too, and sorts after the one that took the plain name.
        stamp = m.group("stamp") + ("_" + m.group("ms") if suffixed else "")
        out.append((stamp, m.group("tag"),
                    os.path.join(directory, name)))
    out.sort(reverse=True)
    return out


def version_line(text):
    """(line, version, link stamp) for the log's own version note."""
    for line in text.splitlines():
        m = VERSION_RE.search(line)
        if m:
            return line.strip(), m.group("ver"), m.group("stamp")
        m = NATIVE_VERSION_RE.match(line)
        if m:
            return line.strip(), m.group("ver"), None
    return None, None, None


def describe_cmd(ref, root):
    """The `git describe` to run for --expect-build.

    `--dirty` and a commit-ish "cannot be used together" -- git calls that
    fatal. Asking for both made every `--expect-build HEAD` fail silently
    and fall back to comparing the log against the literal string "HEAD",
    which can never match: an instrument that reports a mismatch on every
    run looks exactly like one that works. So --dirty is asked for only
    where it means something, which is HEAD.
    """
    cmd = ["git", "-C", root, "describe", "--tags", "--always"]
    if ref in ("HEAD", "", None):
        cmd.append("--dirty")
    else:
        cmd.append(ref)
    return cmd


def expected_version(ref, root):
    """A git ref becomes `git describe`; anything else is taken literally,
    so a version copied out of a release note works too."""
    import subprocess
    try:
        out = subprocess.run(describe_cmd(ref, root), capture_output=True,
                             text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return ref


def version_matches(actual, expect):
    """A build's version is `git describe` output; the log may carry a
    -dirty or -N-g<hash> suffix the expectation does not, and the commit
    hash is the part that decides. Compare on the trailing g<hash> when
    both have one, and fall back to substring either way."""
    if not actual or not expect:
        return False
    if actual == expect:
        return True
    a = re.search(r"g([0-9a-f]{7,})", actual)
    e = re.search(r"g([0-9a-f]{7,})", expect)
    if a and e:
        short = min(len(a.group(1)), len(e.group(1)))
        return a.group(1)[:short] == e.group(1)[:short]
    # `git describe` on a tagged commit prints the bare tag, with no hash.
    return expect in actual or actual in expect


def parse_census(text):
    """Split a log into census draw records, frame summaries and the
    truncation stats from its DC end line.

    Returns (draws, summaries, cap, dropped): draws are dicts with kind
    ("DC" eye-texture / "DCO" offscreen), frame, r (render-target token),
    vh (content hash or None) and the n=/i= arguments; summaries map a
    frame ordinal to the DC frame line's counters; cap and dropped come
    from the DC end line (None when the log has none).
    """
    draws = []
    summaries = {}
    cap = None
    dropped = None
    for raw in text.splitlines():
        m = CENSUS_DRAW_RE.match(raw)
        if m:
            vh = VH_RE.search(raw)
            draws.append({
                "kind": m.group("kind"),
                "frame": int(m.group("frame")),
                "r": m.group("r"),
                "vh": vh.group(1).upper() if vh else None,
                "n": int(m.group("n")),
                "i": int(m.group("i")),
            })
            continue
        m = CENSUS_FRAME_RE.match(raw)
        if m:
            summaries[int(m.group("frame"))] = m.groupdict()
            continue
        if CENSUS_END_RE.match(raw):
            lines_m = CENSUS_LINES_RE.search(raw)
            trunc_m = CENSUS_TRUNC_RE.search(raw)
            if lines_m:
                cap = int(lines_m.group(1))
            if trunc_m:
                dropped = int(trunc_m.group(1))
    return draws, summaries, cap, dropped


def tally_vh(draws, frame=None):
    """Group eye-texture DC lines by vh= hash for one tally table.

    Returns (rows, eye_total, off_total): rows are dicts sorted by count
    descending -- vh, count, sub (r= token -> count, so the per-eye
    split is visible), avg_n and avg_i -- eye_total is the number of DC
    lines the percentages divide by, off_total the DCO lines kept out of
    the table. frame restricts to one frame ordinal; DCO lines never
    enter the rows, only the off_total.
    """
    scope = [d for d in draws if frame is None or d["frame"] == frame]
    eye = [d for d in scope if d["kind"] == "DC"]
    off_total = sum(1 for d in scope if d["kind"] == "DCO")
    by_vh = {}
    for d in eye:
        row = by_vh.setdefault(d["vh"], {"count": 0, "sub": {}, "n": 0, "i": 0})
        row["count"] += 1
        row["sub"][d["r"]] = row["sub"].get(d["r"], 0) + 1
        row["n"] += d["n"]
        row["i"] += d["i"]
    rows = []
    for vh, row in by_vh.items():
        rows.append({
            "vh": vh,
            "count": row["count"],
            "sub": row["sub"],
            "avg_n": row["n"] / float(row["count"]),
            "avg_i": row["i"] / float(row["count"]),
        })
    rows.sort(key=lambda r: (-r["count"], r["vh"] or ""))
    return rows, len(eye), off_total


def print_vh_tally(text, frame):
    """The --tally vh report: per-hash table, then the summary totals.
    Returns the process exit code."""
    draws, summaries, cap, dropped = parse_census(text)
    if not draws and not summaries:
        print("[edvr] no draw-census lines (DC/DCO/DC frame) in this log")
        return 0

    detail_frames = sorted({d["frame"] for d in draws})
    summary_frames = sorted(summaries)
    if dropped:
        only_summary = [f for f in summary_frames if f not in detail_frames]
        where = (", ".join(str(f) for f in detail_frames) or "none")
        print("[edvr] census truncated: %d line(s) dropped past the %s-line "
              "cap; per-draw detail survives for frame(s) %s%s."
              % (dropped, cap or "?", where,
                 (", %s summary-only" % ", ".join(str(f) for f in only_summary))
                 if only_summary else ""))

    if frame is not None:
        scope_summary = summaries.get(frame)
        if frame not in detail_frames:
            if scope_summary is not None:
                s = scope_summary
                print("[edvr] frame %d has no per-draw lines -- only its "
                      "summary survived the census line cap:" % frame)
                print("[edvr] frame %d summary: draws=%s off=%s copies=%s "
                      "disp=%s clears=%s unseen=%s"
                      % (frame, s["draws"], s["off"], s["copies"],
                         s["disp"], s["clears"], s["unseen"]))
                return 0
            print("[edvr] frame %d appears in no census line or summary"
                  % frame)
            return 0

    rows, eye_total, off_total = tally_vh(draws, frame)
    label = "frame %d" % frame if frame is not None else "all frames"
    print("[edvr] tally vh, %s: %d eye-texture DC lines, %d offscreen DCO "
          "lines" % (label, eye_total, off_total))
    if not rows:
        print("[edvr] no eye-texture DC lines in scope")
        return 0
    sub_width = max([len("per r=")] + [
        len("  ".join("%s:%d" % (r, c) for r, c in
                      sorted(row["sub"].items(),
                             key=lambda kv: (-kv[1], kv[0]))))
        for row in rows])
    print("%-4s  %-16s  %5s  %6s  %-*s  %9s  %9s"
          % ("rank", "vh", "count", "% eye", sub_width, "per r=",
             "avg n", "avg i"))
    for rank, row in enumerate(rows, 1):
        sub = "  ".join("%s:%d" % (r, c) for r, c in
                        sorted(row["sub"].items(),
                               key=lambda kv: (-kv[1], kv[0])))
        pct = 100.0 * row["count"] / eye_total if eye_total else 0.0
        print("%-4d  %-16s  %5d  %5.1f%%  %-*s  %9.1f  %9.2f"
              % (rank, row["vh"] or "(no vh=)", row["count"], pct,
                 sub_width, sub, row["avg_n"], row["avg_i"]))
    for f in summary_frames:
        if frame is not None and f != frame:
            continue
        s = summaries[f]
        print("[edvr] frame %d summary: draws=%s off=%s copies=%s disp=%s "
              "clears=%s unseen=%s"
              % (f, s["draws"], s["off"], s["copies"], s["disp"],
                 s["clears"], s["unseen"]))
    return 0


# --camera-census: the VR camera census (advanced.vr_camera_census), one flight
# in the VR profile with the key on. The question it answers (design doc section
# 82): which signal tells an eye view's camera from the world's at the game's
# view-constant refresh, so the on-foot world route can put its sub-pixel phase into
# the world cameras (stage 2) and leave the eyes alone, and whether it did. The lines
# are written by src/d3d11/vr_camera_census_core.h (every line's text lives there;
# tools\vr_camera_census_test pins it and holds tools\camera_census_fixture.log,
# which this script's --self-test reads, to exactly what those formatters write):
#
#   vr camera census 5s: frames=.. calls=.. posts=.. off-thread=.. stale=.. inj-calls=.. kinds=.. callers=..
#       ... tone-frames=.. on-foot-frames=.. foot=yes|no|unknown|off ...
#   vr camera census: camera=0xPTR kind=K caller=+0xRVA thread=owner aspect=..
#       near=.. far=.. fov=.. bound=(..,..) offcentre=(..,..) viewport=(..,..)
#       tan=(l,r,b,t) view=0xPTR vctx=0xPTR first-call=N draw=D tone=before|after|none frame=F
#   vr camera census: changed: camera=0xPTR frame=F n=N <field>=<old>-><new> ...
#   vr camera census: sequence frame=F index=I/3 foot=.. phase=X,Y|- calls=N recorded=R truncated=T
#   vr camera census: call frame=F n=N camera=0xPTR kind=K caller=+0xRVA draw=D
#       tone=.. inj=0|1 role=scene|fp|aux|- fl=0xPRE>0xPOST view=0xPTR rows=[16 floats]   (rows 270..273 the composer wrote)
#   vr camera census: eye=E frame=F foot=.. phase=X,Y|- draw=D b1=0xPTR first=N bytes=N rows=[16] meas=(..,..)
#   vr camera census: eye-geometry eye=E frame=F seq=S frustum=[l,r,d,u] shift=(..,..)
#       expect=(..,..) expect-shifted=(..,..) leak=(..,..)
#   vr camera census: other-thread tid=T camera=0xPTR kind=K caller=+0xRVA calls=N
# the EPISODES (Phase 0; one frame sampled 30 frames after a trigger; every line is written by vrCensusPrintEpisode and the formatters below it):
#   vr camera census: episode frame=F n=N/10 trigger=key-on|foot:no>yes|naming:named>unnamed|gui:0>6 armed=F foot=.. gui=N|- named=0|1 phase=X,Y|-
#       calls=N recorded=R printed=P kinds=0:N,.. callers=+0xRVA:N,..,+more:N       (the call lines that follow are its calls)
#   vr camera census: call frame=F n=N ...                                         (the same call line, up to 120 an episode, the matched ones first)
#   vr camera census: pass-rows ep=N frame=F valid=1|0 bound=1|0|- rows=[12 floats]|- axes-match=n,n+K|- how=identity|transpose|- nearest=n|- diff=..
#   vr camera census: join ep=N sig=S depth=screen|eye eye=0|1|- size=WxH draw=D draws=N vs=0x.. ps=0x.. dw=yes|no|- b1=0xPTR|- first=N bytes=N
#       rows=read match=n,n+K|-  |  rows=- why=skip|no-b1|b1-too-small|map|staging
#   vr camera census: join-rows ep=N sig=S rows=[16 floats]                          (a signature that read its rows)
#   vr camera census: join-more ep=N signatures=N draws=N                            (signatures the table could not keep)
# and, with each 5 s line, three companions (the 5 s line itself is full at 400 characters):
#   vr camera census: episodes windows=W taken=N/10 triggers=N skipped=N state=idle|armed|live [trigger=.. armed=F sample=F]
#   vr camera census: runs windows=W frames=N named=1:n,2:n,3:n,4-8:n,9-30:n,31-89:n,90+:n unnamed=.. longest=named:N,unnamed:N open=named:N|unnamed:N|-
#   vr camera census: detour windows=W every=16 timed=observer-halves frames=N calls=N sampled=N est-ms-frame=X|-
#       obs-calls=N obs-sampled=N obs-pre-us=mean/max|- obs-post-us=.. inj-calls=N inj-sampled=N inj-pre-us=.. inj-post-us=..
# and, from the world route (src/d3d11/vr_world_route.cpp), two lines a 5 s window, back to back:
#   vr world route 5s: ... last=<verdict> jitter=on|off|idle|unnamed|no-hook|fault phase=X,Y rows=X,Y fp-mode=a/b/c
#       last-trigger=.. target=WxH hdr=WxH selection=..
#   vr world route inject 5s: inj-scene=N inj-fp=N inj-refused=N warming=N aux=N after=N unsupported=N other-kind=N
#       unreadable=N off-thread=N write-fail=N inj-kinds=none|3:N[,other:N] pair-checked=N pair-bad=N
#       inj-unnamed=N inj-shut=N
# (`unnamed`: the route is warming or owning but the frame before named no source for the screen, a map or a menu, so its camera
# window was shut; not a fault. `inj-shut` counts calls injected on a frame whose window was shut and must be 0; `inj-unnamed`
# counts frames whose window was open and that named nothing: one per world-to-map change is expected.) The route's own log
# lines the verdict quotes are listed in ROUTE_EVENT_MARKERS (`STOP at frame=`, `RELEASED the world at frame=`, ...).
# The inject line ends with `fov=<narrowest>..<widest>` (radians: the struct's field of view over the screen views of the window,
# two values when the first-person camera's tighter one is in it) or `fov=-` (no frustum was read), and, with the census key on or
# the steady-detail key on, a third line follows the two (src/d3d11/vr_world_route_math.h vrWorldFormatRefusalWindow):
#   vr world route refusal 5s: census=on|off every=N treated=N asked=N sampled=N read=N dropped=N size=WxH pixels=N refused=N
#       refused-pct=X stale-refused=N masked=N corrupt=N sentinel=N unreprojectable=N camera=N range=N depth=N weapon=N other=N
#       stale-kept=N depth-check=RAN/SKIPPED steady-detail=on|off view=on|off
# (`pixels` is what the read-back samples examined, `refused` the pixels whose history the prep refused, by cause. The stale pixels are
# two numbers: `stale-refused`, refused (with the steady-detail key off every stale pixel, with it on the ones last frame's depth did
# not confirm), and `stale-kept`, not refused (the camera term, confirmed by last frame's depth). `depth-check` is the resolves with
# the key on whose prep ran the depth check and those that could not (a reset frame is neither). The flight-3 build's `stale=` and
# `forgiven=` (its blanket form of the rule) still parse, as stale-refused and stale-kept, with no depth-check. "Ran, 0 refused" is
# pixels > 0 and refused=0, "never ran" is treated=0, asked=0 or read=0.) The route also logs a line when the steady-detail key or
# the refusal view changes (`steady-detail is ON|OFF from frame=`, `the refusal view is ON|OFF from frame=`), see ROUTE_EVENT_MARKERS.
#
# WHAT A CAMERA IS. The camera OBJECT is no identity: one object was the left eye camera in
# the cockpit (kind 5) and the world's kind-3 camera on foot (flight 1, 4.63 million calls),
# and the camera line names only the kind of its FIRST call. So the report works from the KIND
# OF EACH CALL in the logged call sequences: a camera is labelled by the kinds of its calls (it
# may have several), the world camera is the one with the most kind-3 calls before the tone, the
# eye cameras are the cameras whose calls joined an eye draw's rows, and a camera's kind-3 calls
# are grouped by the projection their composed rows carry, so a second projection on the same
# object (the first-person weapon camera: a tighter field of view and a larger near plane)
# shows as its own role.
#
# THE JOIN (B) is offline: each eye draw's b1 rows are matched to the rows of every
# logged call within CENSUS_JOIN_TOL; the cameras whose calls match ARE the eye
# cameras, and their field signature, caller, view and place against the tone draw
# are what a rule to exclude them can be built from. `foot=` is what Elite's journal
# said: the census samples a frame (a call sequence, an eye readback) only when the
# tone was drawn AND the journal, if it is read, says on foot (`off`: no journal,
# the tone alone decided); and, while the route is jittering, only when the frame's
# phase is non-zero (`phase=` is what vrWorldRouteWorldPhase said: `-` the route was
# not jittering, a pair the phase in render pixels, positive right/down).
#
# THE STAGE 2 VERDICT reads the injected flight: the eye rows must not move (|leak|
# below CENSUS_LEAK_PASS with a world phase of 1e-4 to 2e-4 NDC) while the kind-3 calls'
# rows carry the phase (their measured shift is flatProjectionJitter's: x = 2 px / W,
# y = -2 py / H, within CENSUS_PHASE_TOL), nothing but kind 3 is injected, and nothing ran
# off the render thread or unreadable. PASS, WARN or STOP a line; `n/a` for what a log
# cannot say (an older census, a route that never jittered).

CENSUS_LINE_RE = re.compile(
    r"^(?P<ts>\[[\d:.]+\])?\s*vr camera census(?P<five> 5s)?: (?P<rest>.*?)\s*$")
ROUTE_LINE_RE = re.compile(
    r"^(?P<ts>\[[\d:.]+\])?\s*vr world route (?P<inject>inject )?5s: (?P<rest>.*?)\s*$")
REFUSAL_LINE_RE = re.compile(
    r"^(?P<ts>\[[\d:.]+\])?\s*vr world route refusal 5s: (?P<rest>.*?)\s*$")
# The causes the refusal line counts, in the line's own order (src/d3d11/flat_mono_refusal.h), and what each one is. `other` is
# what the census cannot name: a refused pixel of a class the line has no token for.
REFUSAL_CAUSES = (
    ("stale-refused", "the engine slot's depth was not the pixel's (a later draw overdrew it) and the steady-detail depth check, if it is on, did not confirm the camera term"),
    ("masked", "a rig record with no usable history this frame (first seen, or after a gap)"),
    ("corrupt", "a slot code or a record number that did not survive intact"),
    ("sentinel", "no depth under the slot (the sky) or the out-of-range marker"),
    ("unreprojectable", "a moved record whose reprojection failed"),
    ("camera", "the camera term could not be formed"),
    ("range", "the reprojection left the screen or was not finite"),
    ("depth", "the pixel's own depth was not usable"),
    ("weapon", "an attached first-person pixel the weapon's map could not place"),
    ("other", "a refusal the census cannot name"),
)
CENSUS_JOIN_TOL = 1e-5
# Two shift-sign candidates whose residuals differ by less than this are a tie: rows are floats, so rounding alone moves a
# measure by about 1e-7, and with a shift of nothing (the jitter off) every candidate is the same number.
CENSUS_FIT_TIE = 2e-7
CENSUS_RUNS_SHOWN = 24    # a frame with more runs shows its first and last half of this, and says how many it left out
CENSUS_FIXTURE = "camera_census_fixture.log"
# What separates an eye call from a world-side call, read off each call's own rows (an object's first-sight line is
# the wrong place to read it: the world camera's was an eye's).
CENSUS_CALL_FIELDS = ("kind", "aspect", "fov", "near", "shift")
# The stage 2 verdict's thresholds (NDC): an eye row's leak below PASS is no leak, above STOP the world phase reached the
# eye camera (a phase of half a pixel at 5040 wide is 2e-4); a kind-3 call's measured shift must match the phase it was
# given to PHASE_TOL, and is wrong beyond PHASE_STOP.
CENSUS_LEAK_PASS = 1.0e-6
CENSUS_LEAK_STOP = 1.0e-5
CENSUS_PHASE_TOL = 1.0e-6
CENSUS_PHASE_STOP = 1.0e-5
CENSUS_PROJ_TOL = 1.0e-4   # relative: two calls whose scale and near agree to this carry one projection
CENSUS_PROJ_SHOWN = 6
# The kinds the design puts on the screen: the world is 3, the eyes 5 (design doc section 82, flight 1).
CENSUS_KIND5_PER_FRAME = 6.0       # two eyes x the three call sites +0x594E13, +0x594EAB, +0x594FE1
CENSUS_INJECTED_PER_FRAME = (54, 68)
# The role test's field-of-view ratio (kFlatCameraVrFirstPersonFovRatio in src/d3d11/flat_camera_vr.h): a screen view whose fov is at
# most this fraction of the frame's widest is the first-person camera. Flight 2's struct: 0.8203 against 0.9831 rad (ratio 0.834).
CENSUS_FP_FOV_RATIO = 0.92


def _cf(text):
    """A number as the DLL prints it. `nan` (the DLL's spelling of any
    non-finite value) and anything unreadable are NaN, so a comparison with one
    is false and it never joins."""
    try:
        return float(text)
    except (TypeError, ValueError):
        return float("nan")


def _chex(text):
    try:
        return int(text.lstrip("+"), 16)
    except (AttributeError, ValueError):
        return None


def _cint(text):
    """A non-negative integer token, else None."""
    return int(text) if text is not None and text.isdigit() else None


def _ctuple(text):
    """(a,b) or (a,b,c,d) -> floats; `-` or anything else -> None."""
    if not text or not text.startswith("(") or not text.endswith(")"):
        return None
    return tuple(_cf(p) for p in text[1:-1].split(","))


def _clist(text):
    if not text or not text.startswith("[") or not text.endswith("]"):
        return None
    return [_cf(p) for p in text[1:-1].split(",")]


def _cpair(text):
    """`x,y` (the route's and the census's phase) -> (x, y) floats, else None."""
    if not text or text.count(",") != 1:
        return None
    a, b = (_cf(p) for p in text.split(","))
    if a != a or b != b:
        return None
    return (a, b)


def _cphase(text):
    """A `phase=` token as (state, (x, y) or None): `absent` when the line has
    none (a census that predates stage 2), `off` for `-` (the route was not
    jittering that frame), `on` for a pair in render pixels (a pair of zeros is a
    jittering frame whose phase was zero: a warm-up frame)."""
    if text is None:
        return "absent", None
    if text == "-":
        return "off", None
    pair = _cpair(text)
    return ("on", pair) if pair else ("absent", None)


def _ckinds(text):
    """The route's `inj-kinds=` token: `none` -> {}, `3:54,other:2` -> {"3": 54, "other": 2}; anything else None."""
    if text is None:
        return None
    if text == "none":
        return {}
    out = {}
    for part in text.split(","):
        key, sep, value = part.partition(":")
        if not sep or not key or not value.isdigit():
            return None
        out[key] = out.get(key, 0) + int(value)
    return out


def _ckv(rest):
    kv = {}
    for token in rest.split():
        key, sep, value = token.partition("=")
        if sep and re.match(r"^[a-z][a-z0-9-]*$", key):
            kv[key] = value
    return kv


def _ckindmap(text):
    """An episode header's `kinds=` (`0:12,3:290,5:10,other:2,unreadable:1`, `-` for none) -> {0: 12, 3: 290, 5: 10, "other": 2, "unreadable": 1}; None when it is not one."""
    if text is None:
        return None
    if text == "-":
        return {}
    out = {}
    for part in text.split(","):
        key, sep, value = part.partition(":")
        if not sep or not value.isdigit():
            return None
        out[int(key) if key.isdigit() else key] = out.get(int(key) if key.isdigit() else key, 0) + int(value)
    return out


def _ccallers(text):
    """An episode header's `callers=` (`+0x58DE73:300,+0x594E13:130,+more:1`, `-` for none) -> ([(rva, n), ...], more); None when it is not one."""
    if text is None:
        return None
    if text == "-":
        return [], 0
    callers, more = [], 0
    for part in text.split(","):
        key, sep, value = part.rpartition(":")
        if not sep or not value.isdigit():
            return None
        if key == "+more":
            more += int(value)
            continue
        rva = _chex(key)
        if rva is None:
            return None
        callers.append((rva, int(value)))
    return callers, more


def _ctrigger(text):
    """A `trigger=` token: `key-on`, `foot:no>yes`, `naming:unnamed>named`, `gui:0>6` -> (kind, from, to) with from and to None for key-on."""
    if text == "key-on":
        return ("key-on", None, None)
    kind, sep, rest = (text or "").partition(":")
    if not sep or kind not in ("foot", "naming", "gui"):
        return None
    old, arrow, new = rest.partition(">")
    return (kind, old, new) if arrow and old and new else None


def _cmatch(text):
    """A `match=` / `axes-match=` token: `98,101`, `1,2,3,4,5,6+48`, `-` -> ([ordinals], how many more matched than are listed); None when it is not one."""
    if text is None:
        return None
    if text == "-":
        return [], 0
    listed, plus, extra = text.partition("+")
    if plus and not extra.isdigit():
        return None
    ordinals = []
    for part in listed.split(","):
        if not part.isdigit():
            return None
        ordinals.append(int(part))
    return ordinals, int(extra) if plus else 0


CENSUS_RUN_BINS = ("1", "2", "3", "4-8", "9-30", "31-89", "90+")


def _cbins(text):
    """A `named=` / `unnamed=` token of a runs line (`1:1,2:0,3:0,4-8:0,9-30:0,31-89:1,90+:1`) -> {bin name: runs}; None unless it has exactly the seven bins in order."""
    if text is None:
        return None
    out = {}
    for part in text.split(","):
        key, sep, value = part.rpartition(":")
        if not sep or not value.isdigit():
            return None
        out[key] = int(value)
    return out if tuple(out) == CENSUS_RUN_BINS else None


def _cpair_us(text):
    """A detour line's `mean/max` microseconds (`25/30`, `0.31/4.2`, `-`) -> (mean, max) floats, or None."""
    if not text or "/" not in text:
        return None
    a, _, b = text.partition("/")
    mean, peak = _cf(a), _cf(b)
    return (mean, peak) if mean == mean and peak == peak else None


def census_geometry(rows):
    """What a call's composed rows (rows 270..273, sixteen floats) say about the camera that made them, read back
    out of them and free of how the head is turned: the projection's x and y scale, the aspect and field of view
    they give, the near plane (rows[14]) and the off-centre shift flatCameraMeasureRowShift measures (NDC). The x
    column of the rows is p0 times one axis plus p8 times the view direction, so its length squared is p0^2 + p8^2
    and the scale is what is left after the shift is taken out. None when the rows are absent or not finite."""
    if not rows or len(rows) != 16 or any(not math.isfinite(v) for v in rows):
        return None
    f = (rows[3], rows[7], rows[11])
    ff = f[0] * f[0] + f[1] * f[1] + f[2] * f[2]
    if not ff > 1e-6:
        return None
    sx = (rows[0] * f[0] + rows[4] * f[1] + rows[8] * f[2]) / ff
    sy = (rows[1] * f[0] + rows[5] * f[1] + rows[9] * f[2]) / ff
    xs = math.sqrt(max(0.0, rows[0] ** 2 + rows[4] ** 2 + rows[8] ** 2 - sx * sx * ff))
    ys = math.sqrt(max(0.0, rows[1] ** 2 + rows[5] ** 2 + rows[9] ** 2 - sy * sy * ff))
    if not (xs > 0.0 and ys > 0.0):
        return None
    return {"shift": (sx, sy), "xs": xs, "ys": ys, "aspect": ys / xs, "fov": 2.0 * math.atan(1.0 / ys),
            "near": rows[14]}


def parse_camera_census(text):
    """A flight log's census lines, sorted into what each one is.

    Returns a dict: lines (how many census lines), windows [(stamp, text)],
    cameras {ptr: row} with order [ptr] in first-seen order, changes {ptr: [line]},
    sequences [{frame, index, calls, recorded, truncated, phase_state, phase, rows}],
    eyes [dict], geometry {(eye, frame): dict}, threads [dict], info [text]. A call
    row is {n, camera, kind, caller, draw, tone, inj, role, pre, post, view, rows,
    geo}; a value the DLL printed as `-` is None, and a token an older census never
    printed (inj, role, phase) is None or `absent`.

    The EPISODES (Phase 0): episodes [dict] (a header's fields, `rows` the call lines that followed it, `joins` the join
    signatures and their rows, `pass` the pass's rows line, `join_more`), episode_counters, runs and detour (the 5 s
    window's three companion lines; they are not windows: `windows` holds the 5 s lines only). An episode's call lines
    are NOT in `sequences`: the legacy analyses read the first three on-foot frames alone."""
    c = {"lines": 0, "unparsed": 0, "windows": [], "cameras": {}, "order": [], "changes": {},
         "sequences": [], "eyes": [], "geometry": {}, "threads": [], "info": [],
         "episodes": [], "episode_counters": [], "runs": [], "detour": []}
    current = None
    episode_by_n = {}
    for raw in text.splitlines():
        m = CENSUS_LINE_RE.match(raw)
        if not m:
            continue
        c["lines"] += 1
        rest = m.group("rest")
        if m.group("five"):
            c["windows"].append((m.group("ts") or "", rest))
            continue
        try:
            kv = _ckv(rest)
            if rest.startswith("camera="):
                ptr = _chex(kv.get("camera"))
                if ptr is None or ptr in c["cameras"]:
                    continue
                c["order"].append(ptr)
                c["cameras"][ptr] = {
                    "ptr": ptr, "kind": int(kv.get("kind", "-1")) if kv.get("kind", "-").isdigit() else None,
                    "caller": _chex(kv.get("caller")), "thread": kv.get("thread"),
                    "aspect": _cf(kv.get("aspect")), "near": _cf(kv.get("near")),
                    "far": _cf(kv.get("far")), "fov": _cf(kv.get("fov")),
                    "bound": _ctuple(kv.get("bound")), "offcentre": _ctuple(kv.get("offcentre")),
                    "viewport": _ctuple(kv.get("viewport")), "tan": _ctuple(kv.get("tan")),
                    "view": _chex(kv.get("view")), "vctx": _chex(kv.get("vctx")),
                    "first_call": kv.get("first-call"), "draw": kv.get("draw"),
                    "tone": kv.get("tone"), "frame": int(kv["frame"]) if kv.get("frame", "").isdigit() else None}
            elif rest.startswith("changed:"):
                ptr = _chex(kv.get("camera"))
                fields = {k: v for k, v in kv.items()
                          if k not in ("camera", "frame", "n") and "->" in v}
                c["changes"].setdefault(ptr, []).append(
                    {"frame": int(kv["frame"]) if kv.get("frame", "").isdigit() else None,
                     "n": kv.get("n"), "fields": fields})
            elif rest.startswith("sequence "):
                state, phase = _cphase(kv.get("phase"))
                current = {"frame": int(kv.get("frame", "-1")), "index": kv.get("index"), "foot": kv.get("foot"),
                           "phase_state": state, "phase": phase,
                           "calls": int(kv.get("calls", "0")), "recorded": int(kv.get("recorded", "0")),
                           "truncated": int(kv.get("truncated", "0")), "rows": []}
                c["sequences"].append(current)
            elif rest.startswith("call "):
                frame = int(kv.get("frame", "-1"))
                if _chex(kv.get("camera")) is None:   # nothing to join or digest, and no sequence to start for it
                    c["unparsed"] += 1
                    continue
                if current is None or current["frame"] != frame:
                    current = {"frame": frame, "index": "?", "foot": None, "phase_state": "absent", "phase": None,
                               "calls": 0, "recorded": 0, "truncated": 0, "rows": []}
                    c["sequences"].append(current)
                pre, _, post = kv.get("fl", "").partition(">")
                rows = _clist(kv.get("rows"))
                current["rows"].append({
                    "n": int(kv.get("n", "0")), "camera": _chex(kv.get("camera")),
                    "kind": int(kv["kind"]) if kv.get("kind", "-").isdigit() else None,
                    "caller": _chex(kv.get("caller")),
                    "draw": int(kv["draw"]) if kv.get("draw", "-").isdigit() else None,
                    "tone": kv.get("tone"), "pre": _chex(pre) if pre else None,
                    "post": _chex(post) if post and post != "-" else None,
                    "inj": int(kv["inj"]) if kv.get("inj") in ("0", "1") else None,
                    "role": kv.get("role"),
                    "view": _chex(kv.get("view")),
                    "rows": rows, "geo": census_geometry(rows), "frame": frame})
            elif rest.startswith("eye-geometry"):
                eye = int(kv.get("eye", "-1"))
                frame = int(kv.get("frame", "-1"))
                c["geometry"][(eye, frame)] = {
                    "known": kv.get("geometry") != "unavailable",
                    "seq": int(kv["seq"]) if kv.get("seq", "").isdigit() else None,
                    "frustum": _clist(kv.get("frustum")), "shift": _ctuple(kv.get("shift")),
                    "expect": _ctuple(kv.get("expect")),
                    "expect_shifted": _ctuple(kv.get("expect-shifted")),
                    "leak": _ctuple(kv.get("leak"))}
            elif rest.startswith("eye="):
                state, phase = _cphase(kv.get("phase"))
                c["eyes"].append({
                    "eye": int(kv.get("eye", "-1")), "frame": int(kv.get("frame", "-1")), "foot": kv.get("foot"),
                    "phase_state": state, "phase": phase,
                    "draw": int(kv["draw"]) if kv.get("draw", "-").isdigit() else None,
                    "b1": _chex(kv.get("b1")), "first": kv.get("first"), "bytes": kv.get("bytes"),
                    "rows": _clist(kv.get("rows")), "meas": _ctuple(kv.get("meas")),
                    "why": kv.get("why")})
            elif rest.startswith("other-thread"):
                c["threads"].append({"tid": kv.get("tid"), "camera": _chex(kv.get("camera")),
                                     "kind": kv.get("kind"), "caller": _chex(kv.get("caller")),
                                     "calls": kv.get("calls")})
            elif rest.startswith("episode "):
                # An episode's header; the call lines that follow it (the same frame) are its calls.
                n, _, of = kv.get("n", "0/0").partition("/")
                state, phase = _cphase(kv.get("phase"))
                trigger = _ctrigger(kv.get("trigger"))
                callers = _ccallers(kv.get("callers"))
                kinds = _ckindmap(kv.get("kinds"))
                if not n.isdigit() or trigger is None or kinds is None or callers is None:
                    c["unparsed"] += 1
                    continue
                ep = {"n": int(n), "of": int(of) if of.isdigit() else None, "frame": int(kv.get("frame", "-1")),
                      "armed": _cint(kv.get("armed")), "trigger": trigger, "foot": kv.get("foot"),
                      "gui": _cint(kv.get("gui")), "named": kv.get("named") == "1",
                      "phase_state": state, "phase": phase,
                      "calls": int(kv.get("calls", "0")), "recorded": int(kv.get("recorded", "0")),
                      "printed": int(kv.get("printed", "0")), "kinds": kinds, "callers": callers[0],
                      "callers_more": callers[1], "rows": [], "joins": [], "pass": None, "join_more": None, "join_draws": None}
                c["episodes"].append(ep)
                episode_by_n[ep["n"]] = ep
                current = ep
            elif rest.startswith("join-rows "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                sig = _cint(kv.get("sig"))
                rows = _clist(kv.get("rows"))
                row = next((j for j in ep["joins"] if j["sig"] == sig), None) if ep else None
                if row is None or rows is None or len(rows) != 16:
                    c["unparsed"] += 1
                    continue
                row["rows"] = rows
            elif rest.startswith("join-draws "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                counts = [_cint(kv.get(k)) for k in ("seen", "relevant", "views", "signatures")]
                if not ep or any(v is None for v in counts):
                    c["unparsed"] += 1
                    continue
                ep["join_draws"] = dict(zip(("seen", "relevant", "views", "signatures"), counts))
            elif rest.startswith("join-more "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                if not ep:
                    c["unparsed"] += 1
                    continue
                ep["join_more"] = {"signatures": _cint(kv.get("signatures")), "draws": _cint(kv.get("draws"))}
            elif rest.startswith("join "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                size = re.match(r"^(\d+)x(\d+)$", kv.get("size", ""))
                match = _cmatch(kv.get("match")) if "match" in kv else None
                if not ep or _cint(kv.get("sig")) is None or not size or kv.get("depth") not in ("screen", "eye") or \
                        ("match" in kv and match is None):
                    c["unparsed"] += 1
                    continue
                ep["joins"].append({
                    "sig": int(kv["sig"]), "depth": kv["depth"], "eye": _cint(kv.get("eye")),
                    "w": int(size.group(1)), "h": int(size.group(2)),
                    "draw": _cint(kv.get("draw")), "draws": _cint(kv.get("draws")),
                    "vs": _chex(kv.get("vs")), "ps": _chex(kv.get("ps")),
                    "dw": {"yes": True, "no": False}.get(kv.get("dw")),
                    "b1": _chex(kv.get("b1")), "first": _cint(kv.get("first")), "bytes": _cint(kv.get("bytes")),
                    "read": kv.get("rows") == "read", "why": kv.get("why"),
                    "match": match[0] if match else None, "match_more": match[1] if match else 0,
                    "rows": None})
            elif rest.startswith("pass-rows "):
                ep = episode_by_n.get(_cint(kv.get("ep")))
                valid = kv.get("valid") == "1"
                rows = _clist(kv.get("rows")) if valid else None
                match = _cmatch(kv.get("axes-match")) if valid else ([], 0)
                if not ep or kv.get("valid") not in ("0", "1") or (valid and (rows is None or len(rows) != 12 or match is None)):
                    c["unparsed"] += 1
                    continue
                ep["pass"] = {"valid": valid, "bound": {"1": True, "0": False}.get(kv.get("bound")), "rows": rows,
                              "match": match[0], "match_more": match[1],
                              "how": kv.get("how") if kv.get("how") in ("identity", "transpose") else None,
                              "nearest": _cint(kv.get("nearest")), "diff": _cf(kv.get("diff"))}
            elif rest.startswith("episodes "):
                taken, _, of = kv.get("taken", "").partition("/")
                if not taken.isdigit() or _cint(kv.get("triggers")) is None or _cint(kv.get("skipped")) is None:
                    c["unparsed"] += 1
                    continue
                c["episode_counters"].append({
                    "ts": m.group("ts") or "", "windows": _cint(kv.get("windows")), "taken": int(taken),
                    "of": int(of) if of.isdigit() else None, "triggers": int(kv["triggers"]), "skipped": int(kv["skipped"]),
                    "state": kv.get("state"), "trigger": _ctrigger(kv.get("trigger")),
                    "armed": _cint(kv.get("armed")), "sample": _cint(kv.get("sample"))})
            elif rest.startswith("runs "):
                named, unnamed = _cbins(kv.get("named")), _cbins(kv.get("unnamed"))
                longest = re.match(r"^named:(\d+),unnamed:(\d+)$", kv.get("longest", ""))
                opened = re.match(r"^(named|unnamed):(\d+)$", kv.get("open", ""))
                if named is None or unnamed is None or not longest or (kv.get("open") != "-" and not opened) or \
                        _cint(kv.get("frames")) is None:
                    c["unparsed"] += 1
                    continue
                c["runs"].append({
                    "ts": m.group("ts") or "", "windows": _cint(kv.get("windows")), "frames": int(kv["frames"]),
                    "named": named, "unnamed": unnamed,
                    "longest_named": int(longest.group(1)), "longest_unnamed": int(longest.group(2)),
                    "open": (opened.group(1), int(opened.group(2))) if opened else None})
            elif rest.startswith("detour "):
                c["detour"].append({
                    "ts": m.group("ts") or "", "windows": _cint(kv.get("windows")), "every": _cint(kv.get("every")),
                    "timed": kv.get("timed"), "frames": _cint(kv.get("frames")), "calls": _cint(kv.get("calls")),
                    "sampled": _cint(kv.get("sampled")),
                    "est_ms": _cf(kv.get("est-ms-frame")) if kv.get("est-ms-frame", "-") != "-" else None,
                    "modes": {name: {"calls": _cint(kv.get(name + "-calls")), "sampled": _cint(kv.get(name + "-sampled")),
                                     "pre": _cpair_us(kv.get(name + "-pre-us")), "post": _cpair_us(kv.get(name + "-post-us"))}
                              for name in ("obs", "inj")}})
            else:
                c["info"].append(rest)
        except (ValueError, TypeError, KeyError, IndexError):
            c["unparsed"] += 1   # a line cut short or garbled: counted, never fatal to the report
    return c


def parse_world_route(text):
    """The world route's 5 s lines. The route prints two a window, back to back: `vr world route 5s:` and
    `vr world route inject 5s:`; the inject line joins the route line before it BY ORDER (their timestamps are the
    boundary's and need not be read), and a window that has only one of the two is kept with the other None. Returns a
    list of {ts, route, inject} with each side a key=value dict; `tok(w, key)` reads a token from either."""
    windows = []
    for raw in text.splitlines():
        m = ROUTE_LINE_RE.match(raw)
        if not m:
            continue
        kv = _ckv(m.group("rest"))
        if m.group("inject"):
            if windows and windows[-1]["inject"] is None and windows[-1]["route"] is not None:
                windows[-1]["inject"] = kv
            else:
                windows.append({"ts": m.group("ts") or "", "route": None, "inject": kv})
        else:
            windows.append({"ts": m.group("ts") or "", "route": kv, "inject": None})
    return windows


# Lines of the route's own log the verdict quotes (their wording is the route's: src/d3d11/vr_world_route*.cpp). Each key is what the
# line means to the verdict; the needle is a phrase of it.
ROUTE_EVENT_MARKERS = (
    ("stop", "STOP at frame="),
    ("shut", "on a frame whose window the route had shut"),
    ("kind-other", "camera calls of a kind other than 3 were INJECTED"),
    ("rows-disagree", "camera rows disagree with the phase"),
    ("no-scene-call", "jitter is wanted but no scene camera call was injected"),
    ("jittered", "the world is JITTERED from frame="),
    ("window-open", "camera window was open"),
    ("released", "vr world route: RELEASED the world at frame="),
    ("excluded", "camera call EXCLUDED, not a screen view"),
    ("steady-detail", "vr world route: steady-detail is "),
    ("refusal-view", "vr world route: the refusal view is "),
)


def route_events(text):
    """{key: [line, ...]} for every line of the log that carries one of ROUTE_EVENT_MARKERS' phrases, and under "vscreen-applied" the
    panel patch's `vScreen resolution: WxH -> WxH at N site(s)` line (the on-foot screen's size, the size the route resolves at)."""
    events = {}
    for raw in text.splitlines():
        for key, needle in ROUTE_EVENT_MARKERS:
            if needle in raw:
                events.setdefault(key, []).append(raw.strip())
        if "vScreen resolution: " in raw and " -> " in raw and re.search(r"vScreen resolution: \d+x\d+ -> \d+x\d+ at \d+ site", raw):
            events.setdefault("vscreen-applied", []).append(raw.strip())
    return events


def route_tok(window, key):
    """A token of one route window, from the route line or the inject line (None when neither has it)."""
    for side in ("route", "inject"):
        kv = window[side]
        if kv is not None and key in kv:
            return kv[key]
    return None


def route_hdr(windows, events=None):
    """The size the world phase is in (render pixels): the route line's `hdr=WxH`, the commonest that is not 0x0
    (it reads 0x0 while the route has no frame); failing that the size the route's own `the world is JITTERED from
    frame=` line names (`phase (x,y) px in WxH`). (W, H, where from), or None when the log has neither."""
    sizes = {}
    for w in windows:
        m = re.match(r"^(\d+)x(\d+)$", route_tok(w, "hdr") or "")
        if m and int(m.group(1)) and int(m.group(2)):
            key = (int(m.group(1)), int(m.group(2)))
            sizes[key] = sizes.get(key, 0) + 1
    if sizes:
        w, h = sorted(sizes.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]
        return w, h, "a `vr world route 5s:` line's hdr="
    for line in (events or {}).get("jittered", []):
        m = re.search(r" px in (\d+)x(\d+),", line)
        if m and int(m.group(1)) and int(m.group(2)):
            return int(m.group(1)), int(m.group(2)), "the route's `the world is JITTERED` line"
    # The route resolves the world at the on-foot screen's own size (R = D), which the panel patch's line names: the logged size, for a
    # log whose route lines are missing or read 0x0.
    for line in (events or {}).get("vscreen-applied", []):
        m = re.search(r" -> (\d+)x(\d+) at \d+ site", line)
        if m and int(m.group(1)) and int(m.group(2)):
            return int(m.group(1)), int(m.group(2)), "the panel patch's `vScreen resolution:` line"
    return None


def _cmodes(text):
    """The route line's `fp-mode=a/b/c` (frames whose weapon fold-in ran in mode 0, 1, 2) as three ints, else None."""
    m = re.match(r"^(\d+)/(\d+)/(\d+)$", text or "")
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


def _cfov(text):
    """The inject line's `fov=` token: `lo..hi` -> (lo, hi) floats; `-` (no frustum was read) or anything else -> None."""
    m = re.match(r"^(\d+(?:\.\d+)?)\.\.(\d+(?:\.\d+)?)$", text or "")
    return (float(m.group(1)), float(m.group(2))) if m else None


def route_fov(routes):
    """(narrowest, widest, windows): the struct's field of view (radians) over the inject lines' `fov=` tokens, and how many windows
    had a range; (None, None, 0) when none did (an older build, or no frustum was read)."""
    lo = hi = None
    n = 0
    for w in routes:
        r = _cfov(route_tok(w, "fov"))
        if r:
            lo = r[0] if lo is None or r[0] < lo else lo
            hi = r[1] if hi is None or r[1] > hi else hi
            n += 1
    return lo, hi, n


def parse_refusal_windows(text):
    """The refusal census's 5 s lines (`vr world route refusal 5s:`: one a window while advanced.vr_camera_census is on and the route is
    engaged, while the steady-detail key is on and the route is engaged (its default; census=off then), and while samples of a window
    that has just ended are still draining), each joined to the route's own line and inject line
    of the same window (the route prints route, inject, refusal, in that order; a missing one is None). A list of dicts: ts, kv (every
    token), census (census=on), steady and view (the tokens' text), w and h (size=), pct (refused-pct), route and inject (the other
    two lines' tokens), the counters as ints (None when a token is absent or not a number: treated, asked, sampled, read, dropped,
    every, pixels, refused), kept (`stale-kept=`, or the flight-3 build's `forgiven=`), check_ran and check_skipped (`depth-check=RAN/SKIPPED`:
    None for a build without the depth check) and causes {name: int or None} for every name of REFUSAL_CAUSES (`stale-refused`
    reads the flight-3 build's `stale=` when the new token is absent)."""
    out = []
    route = inject = None
    for raw in text.splitlines():
        m = ROUTE_LINE_RE.match(raw)
        if m:
            if m.group("inject"):
                inject = _ckv(m.group("rest"))
            else:
                route, inject = _ckv(m.group("rest")), None
            continue
        m = REFUSAL_LINE_RE.match(raw)
        if not m:
            continue
        kv = _ckv(m.group("rest"))
        size = re.match(r"^(\d+)x(\d+)$", kv.get("size", ""))
        w = {"ts": m.group("ts") or "", "kv": kv, "census": kv.get("census") == "on", "steady": kv.get("steady-detail"),
             "view": kv.get("view"), "w": int(size.group(1)) if size else None, "h": int(size.group(2)) if size else None,
             "pct": _cf(kv.get("refused-pct")), "route": route, "inject": inject}
        for key in ("every", "treated", "asked", "sampled", "read", "dropped", "pixels", "refused"):
            w[key] = _cint(kv.get(key))
        w["kept"] = _cint(kv.get("stale-kept", kv.get("forgiven")))
        check = re.match(r"^(\d+)/(\d+)$", kv.get("depth-check", ""))
        w["check_ran"], w["check_skipped"] = (int(check.group(1)), int(check.group(2))) if check else (None, None)
        w["causes"] = {name: _cint(kv.get(name)) for name, _ in REFUSAL_CAUSES}
        if w["causes"]["stale-refused"] is None:
            w["causes"]["stale-refused"] = _cint(kv.get("stale"))
        out.append(w)
        route = inject = None
    return out


def refusal_state(w):
    """What one refusal line says happened in its window. `measured`: samples were read back and the shares are real (including a window
    that measured and found nothing refused: pixels > 0, refused=0). `census-off`: the census key was off and nothing asked for it; the
    line is there for the steady-detail key (on by default) and carries its depth-check frame counts only, so there is nothing to
    measure and nothing wrong. The rest are windows that measured nothing, each for its own reason: `idle` (the route treated no frame),
    `no-ask` (the census was on, the route treated frames and none asked for it), `no-sample` (fewer asks than one sample takes),
    `no-read` (samples were dispatched and none came back), `unreadable` (a counter did not parse)."""
    if any(w[k] is None for k in ("treated", "asked", "sampled", "read", "pixels", "refused")):
        return "unreadable"
    if w["read"] and w["pixels"]:
        return "measured"
    if not w["census"] and not (w["asked"] or w["sampled"]):
        return "census-off"
    if not w["treated"]:
        return "idle"
    if not w["asked"]:
        return "no-ask"
    if not w["sampled"]:
        return "no-sample"
    return "no-read"


def _pct(n, pixels):
    return "%.3f%%" % (100.0 * n / pixels) if pixels else "-"


def _stale_tail(steady, kept, stale_refused, ran, skipped):
    """The tail of a measured line's stale part: with the key on, how much of the stale pixels the depth check kept, and, when the line
    carries it (and the key is on, or the check counted frames), the depth check's own frames."""
    out = ""
    stale = (kept or 0) + (stale_refused or 0)
    if steady == "on" and stale:
        out += " (%.1f%% of the stale pixels)" % (100.0 * (kept or 0) / stale)
    if ran is not None and (steady == "on" or ran or skipped):
        out += "; depth-check %d ran, %d skipped" % (ran, skipped or 0)
    return out


def _key_findings(w, stamp, measured):
    """What the steady-detail key's state and the resolver's depth-check frames say to each other in one window. The frame counts are
    the resolver's own host counters, so a window needs no samples for them (a `census-off` window has none); the stale pixels are
    known only to a window that measured."""
    out = []
    causes = w["causes"]
    ran, skipped = w["check_ran"], w["check_skipped"]
    if measured and w["steady"] == "on" and ran is None and (causes["stale-refused"] or 0) > 0:
        # A build without the depth check (the flight-3 build): its key was the blanket form, which refused no stale pixel.
        out.append(("WARN", "%s: steady-detail=on and %d stale pixel(s) were still refused: the key's rule did not reach the resolver "
                    "(a build without the depth check: with it on, a stale slot takes the camera term and is counted as kept)"
                    % (stamp, causes["stale-refused"])))
    if w["steady"] == "on" and ran is not None:
        if not ran and not skipped:
            if w["treated"]:   # a window that treated nothing had nothing to check
                out.append(("WARN", "%s: steady-detail=on and the resolver counted no depth-check frame in a window that treated %d frame(s): "
                            "the key reached the route and not the resolver" % (stamp, w["treated"])))
        elif not ran:
            out.append(("WARN", "%s: steady-detail=on and the depth check never ran (0 ran, %d skipped): the resolver could not make "
                        "last frame's depth, so every stale pixel was refused as with the key off" % (stamp, skipped)))
        elif measured and not (w["kept"] or 0) and (causes["stale-refused"] or 0) > 0:
            out.append(("note", "%s: steady-detail=on, the depth check ran in %d frame(s) and kept no stale pixel (%d refused): "
                        "everything stale moved, or last frame's depth never matched" % (stamp, ran, causes["stale-refused"])))
    if w["steady"] == "off":
        if (w["kept"] or 0) > 0:
            out.append(("WARN", "%s: %d stale pixel(s) were kept while steady-detail=off: the line's key state and the resolver's disagree"
                        % (stamp, w["kept"])))
        if (ran or 0) or (skipped or 0):
            out.append(("WARN", "%s: the depth check counted frames (%d ran, %d skipped) while steady-detail=off: the line's key state and "
                        "the resolver's disagree" % (stamp, ran or 0, skipped or 0)))
    return out


def refusal_findings(windows):
    """[(level, text)] about what the refusal lines do not support believing (WARN: a number that contradicts another, a key that did not
    take effect, a census that never measured) and what is worth knowing (note). A `census-off` window is not a census that failed: only
    the steady-detail key's own checks apply to it."""
    out = []
    states = [refusal_state(w) for w in windows]
    for w, s in zip(windows, states):
        stamp = w["ts"] or "(no stamp)"
        if s == "unreadable":
            out.append(("WARN", "%s: a refusal line has a counter that is missing or not a number (an older or garbled line): its window is not counted"
                        % stamp))
        elif s == "no-ask":
            out.append(("WARN", "%s: the route treated %d frame(s) and none asked the resolver for the census: the key reached the route "
                        "(census=on) and not the resolver" % (stamp, w["treated"])))
        elif s == "no-read":
            out.append(("WARN", "%s: %d sample(s) were dispatched and none was read back: the read-back is stalled (or this is the window "
                        "the key went on in)" % (stamp, w["sampled"])))
        elif s == "census-off":
            out.extend(_key_findings(w, stamp, False))
        elif s == "measured":
            causes = w["causes"]
            if w["refused"] > w["pixels"]:
                out.append(("WARN", "%s: refused %d exceeds the pixels examined, %d: the census counts a pixel once, so one of the two is wrong"
                            % (stamp, w["refused"], w["pixels"])))
            out.extend(_key_findings(w, stamp, True))
            if (w["dropped"] or 0) > 0:
                out.append(("note", "%s: %d sample(s) were skipped because the read-back ring was full (the shares are unaffected, the sample "
                            "count is lower)" % (stamp, w["dropped"])))
            if (causes["other"] or 0) > 0:
                out.append(("note", "%s: %d refused pixel(s) are of a class the census cannot name (`other`)" % (stamp, causes["other"])))
        if w["view"] == "on":
            out.append(("note", "%s: the refusal view was painting: the headset showed the prep's classification, not the world" % stamp))
    measured = [w for w, s in zip(windows, states) if s == "measured"]
    counted = [w for w, s in zip(windows, states) if s != "census-off"]
    treated = sum(w["treated"] or 0 for w in counted)
    if counted and not measured:
        if treated:
            out.append(("WARN", "the census never measured: the route treated %d frame(s) over %d window(s) and no sample was read back"
                        % (treated, len(counted))))
        else:
            out.append(("note", "the census was on for %d window(s) and the route treated no frame in any of them: nothing was measured "
                        "(not owning the world: a ship, a menu, or the route key off)" % len(counted)))
    return out


def print_refusal_census(windows, events=None):
    """The refusal-census section of --camera-census: the share of the treated pixels whose history the resolver's prep refused, per 5 s
    window and by cause, with the steady-detail key's state and the refusal view's in each window, then totals by key state and the
    findings. Returns the findings."""
    events = events or {}
    every = next((w["every"] for w in windows if w["every"]), None)
    print("\n== refusal census (advanced.vr_camera_census: the route's own resolve, the prep's per-pixel classification, one sample in %s "
          "of the resolves that ask; shares are of the pixels the samples examined) ==" % (every or "?"))
    if not windows:
        print("none: no `vr world route refusal 5s:` line in this log (the route never engaged, or this build predates the census; with the "
              "route on the line is printed while advanced.vr_camera_census is on, and while "
              "experimental.temporal_aa_on_foot_world_steady_detail is on, which is its default)")
        return []
    states = [refusal_state(w) for w in windows]
    off_windows = [w for w, s in zip(windows, states) if s == "census-off"]
    for w, s in zip(windows, states):
        if s == "census-off":
            continue   # summarised below, one line a key state: a long flight has hundreds of them
        route = w["route"] or {}
        inject = w["inject"] or {}
        context = "route state=%s jitter=%s fp-mode %s, inj-fp %s, struct fov %s" % (
            route.get("state", "?"), route.get("jitter", "?"), route.get("fp-mode", "?"), inject.get("inj-fp", "?"), inject.get("fov", "?"))
        head = "%s steady-detail=%s view=%s census=%s" % (w["ts"] or "(no stamp)", w["steady"], w["view"], "on" if w["census"] else "off")
        if s == "measured":
            named = [(name, w["causes"][name]) for name, _ in REFUSAL_CAUSES if w["causes"][name]]
            mix = ", ".join("%s %s" % (name, _pct(n, w["pixels"])) for name, n in named) if named else "none refused"
            print("%s: MEASURED %d sample(s) of %dx%d (%d asked, %d dispatched, %d dropped), treated %d; pixels %d; refused %s (%d): %s; "
                  "stale-kept %s%s | %s"
                  % (head, w["read"], w["w"] or 0, w["h"] or 0, w["asked"], w["sampled"], w["dropped"] or 0, w["treated"], w["pixels"],
                     _pct(w["refused"], w["pixels"]), w["refused"], mix, _pct(w["kept"] or 0, w["pixels"]),
                     _stale_tail(w["steady"], w["kept"], w["causes"]["stale-refused"], w["check_ran"], w["check_skipped"]), context))
        else:
            reason = {
                "idle": "the route treated no frame in this window (nothing was measured)",
                "no-ask": "the route treated %s frame(s) and none asked for the census" % w["treated"],
                "no-sample": "%s ask(s), fewer than one sample's worth (every %s)" % (w["asked"], w["every"]),
                "no-read": "%s sample(s) dispatched, none read back" % w["sampled"],
                "unreadable": "a counter did not parse",
            }[s]
            print("%s: NOT MEASURED: %s | %s" % (head, reason, context))
    off_keys = []
    for w in off_windows:
        if w["steady"] not in off_keys:
            off_keys.append(w["steady"])
    for key in off_keys:
        group = [w for w in off_windows if w["steady"] == key]
        counted = [w for w in group if w["check_ran"] is not None]
        print("census off in %d window(s), steady-detail=%s (the line is printed for the key): the route treated %d frame(s)%s"
              % (len(group), key, sum(w["treated"] or 0 for w in group),
                 "; depth-check %d ran, %d skipped" % (sum(w["check_ran"] for w in counted), sum(w["check_skipped"] for w in counted))
                 if counted else ""))
    measured = [(w, s) for w, s in zip(windows, states) if s == "measured"]
    keys = []
    for w, _ in measured:
        if w["steady"] not in keys:
            keys.append(w["steady"])
    for key in keys:
        group = [w for w, _ in measured if w["steady"] == key]
        pixels = sum(w["pixels"] for w in group)
        refused = sum(w["refused"] for w in group)
        kept = sum(w["kept"] or 0 for w in group)
        with_check = [w for w in group if w["check_ran"] is not None]
        ran = sum(w["check_ran"] for w in with_check) if with_check else None
        skipped = sum(w["check_skipped"] for w in with_check) if with_check else None
        totals = {name: sum(w["causes"][name] or 0 for w in group) for name, _ in REFUSAL_CAUSES}
        mix = ", ".join("%s %s" % (name, _pct(n, pixels)) for name, n in totals.items() if n)
        of_refused = ", ".join("%s %.1f%%" % (name, 100.0 * n / refused) for name, n in totals.items() if n) if refused else ""
        print("totals, steady-detail=%s: %d measured window(s), %d sample(s), pixels %d, refused %s (%d)%s; stale-kept %s%s%s"
              % (key, len(group), sum(w["read"] for w in group), pixels, _pct(refused, pixels), refused,
                 ": %s" % mix if mix else " (none refused)", _pct(kept, pixels),
                 _stale_tail(key, kept, totals["stale-refused"], ran, skipped),
                 "; of the refused: %s" % of_refused if of_refused else ""))
        if len(group) > 1:
            shares = [100.0 * w["refused"] / w["pixels"] for w in group]
            print("    per-window refused share: min %.3f%%, max %.3f%% over %d window(s)" % (min(shares), max(shares), len(group)))
    seen = [(name, text) for name, text in REFUSAL_CAUSES if any((w["causes"][name] or 0) for w, _ in measured)]
    if seen:
        print("causes seen: %s" % "; ".join("%s = %s" % (name, text) for name, text in seen))
    for key in ("steady-detail", "refusal-view"):
        for line in events.get(key, [])[:6]:
            print("route log: %s" % line[:240])
    findings = refusal_findings(windows)
    for level, text in findings:
        print("%s %s" % ("!!" if level == "WARN" else "refusal note:", text))
    warns = sum(1 for level, _ in findings if level == "WARN")
    asked = len(windows) - len(off_windows)
    if warns:
        print("refusal census: WARN (%d finding(s) above)" % warns)
    elif measured:
        print("refusal census: consistent (%d of %d window(s) measured)%s" % (
            len(measured), asked, "; %d other window(s) had the census off" % len(off_windows) if off_windows else ""))
    elif off_windows and not asked:
        print("refusal census: the census key was off in all %d window(s): nothing to measure (the lines carry the steady-detail key's "
              "depth-check frames)" % len(off_windows))
    else:
        print("refusal census: nothing measured (%d window(s))" % asked)
    return findings


def census_join(c, tol=CENSUS_JOIN_TOL):
    """(B): every eye draw's rows against every logged call's rows. Returns one
    dict per eye draw: {eye, matches: [call rows], scope}, scope being `frame` (a
    call of the same frame matched), `other-frame` (only a call of another frame
    did: same camera, same pose, unusual), `none` (no call matched although the
    frame's sequence was logged), `no-sequence` (that frame's calls were not
    logged) or `no-rows` (the readback failed)."""
    by_frame = {}
    every = []
    for seq in c["sequences"]:
        for row in seq["rows"]:
            by_frame.setdefault(seq["frame"], []).append(row)
            every.append(row)

    def matching(rows, eye_rows):
        return [r for r in rows if r["rows"] and len(r["rows"]) == len(eye_rows) and
                all(abs(a - b) <= tol for a, b in zip(r["rows"], eye_rows))]

    out = []
    for eye in c["eyes"]:
        if not eye["rows"]:
            out.append({"eye": eye, "matches": [], "scope": "no-rows"})
            continue
        same = matching(by_frame.get(eye["frame"], []), eye["rows"])
        if same:
            out.append({"eye": eye, "matches": same, "scope": "frame"})
            continue
        other = matching(every, eye["rows"])
        if other:
            out.append({"eye": eye, "matches": other, "scope": "other-frame"})
        else:
            out.append({"eye": eye, "matches": [],
                        "scope": "none" if eye["frame"] in by_frame else "no-sequence"})
    return out


def census_runs(rows):
    """A frame's calls as runs of consecutive calls that share (kind, caller,
    tone): [(kind, caller, tone, count, first n, last n)]."""
    runs = []
    for r in rows:
        key = (r["kind"], r["caller"], r["tone"])
        if runs and runs[-1][:3] == key:
            runs[-1] = key + (runs[-1][3] + 1, runs[-1][4], r["n"])
        else:
            runs.append(key + (1, r["n"], r["n"]))
    return runs


def _cequal(a, b):
    """Two signature values the same, to the precision a log line holds."""
    if a is None or b is None:
        return a is b
    if isinstance(a, tuple):
        return isinstance(b, tuple) and len(a) == len(b) and \
            all(_cequal(x, y) for x, y in zip(a, b))
    if isinstance(a, int) and isinstance(b, int):
        return a == b
    if a != a or b != b:
        return False
    return abs(a - b) <= 1e-4 * max(1.0, abs(a), abs(b))


def _cg(value):
    """A value for the report: floats to six digits, tuples as (a, b)."""
    if value is None:
        return "-"
    if isinstance(value, tuple):
        return "(" + ", ".join(_cg(v) for v in value) + ")"
    if isinstance(value, float):
        return "%.6g" % value
    if isinstance(value, int):
        return "0x%X" % value if value > 0xFFFF else str(value)
    return str(value)


def _ckinds_text(kinds):
    """{3: 54, 5: 3} -> `k3 x54, k5 x3`, in kind order."""
    return ", ".join("k%s x%d" % (k, kinds[k]) for k in sorted(kinds, key=lambda k: (k is None, k or 0))) or "none"


def census_all_rows(c):
    return [r for seq in c["sequences"] for r in seq["rows"]]


def census_camera_calls(c):
    """Per camera, from the logged calls alone: {ptr: {kinds {kind: n}, frames {frame: {kind: n}}, before, after, none
    (kind-3 calls by the tone), calls, callers, views}}. The kind is the CALL's own; the camera line's kind is that of
    the first call the census ever heard and says nothing about the logged frames."""
    stats = {}
    for seq in c["sequences"]:
        for r in seq["rows"]:
            s = stats.setdefault(r["camera"], {"kinds": {}, "frames": {}, "before": 0, "after": 0, "none": 0,
                                               "calls": 0, "callers": set(), "views": set()})
            s["calls"] += 1
            s["kinds"][r["kind"]] = s["kinds"].get(r["kind"], 0) + 1
            per = s["frames"].setdefault(seq["frame"], {})
            per[r["kind"]] = per.get(r["kind"], 0) + 1
            if r["kind"] == 3 and r["tone"] in ("before", "after", "none"):
                s[r["tone"]] += 1
            s["callers"].add(r["caller"])
            s["views"].add(r["view"])
    return stats


def census_roles(c, join):
    """Who is who, from the calls. The eye cameras are the ones an eye draw's rows
    joined to. The world camera is the camera with the most kind-3 calls before the
    tone in the logged sequences (else the most kind-3 calls; the first seen on a
    tie), whatever its first-seen kind was. Every other camera with a kind-3 call is
    world-side (a pass's, a probe's). Returns {eye, world, world_side, other, stats}:
    sets of camera pointers, `world` one pointer or None, `other` the cameras with
    no kind-3 call or no logged call at all."""
    eye = set()
    for j in join:
        for m in j["matches"]:
            eye.add(m["camera"])
    stats = census_camera_calls(c)
    position = {p: i for i, p in enumerate(c["order"])}
    kind3 = {p for p, s in stats.items() if s["kinds"].get(3, 0) and p not in eye}
    world = None
    if kind3:
        world = sorted(kind3, key=lambda p: (-(stats[p]["before"] + stats[p]["none"]), -stats[p]["kinds"].get(3, 0),
                                             position.get(p, 1 << 30), p))[0]
    known = set(c["order"]) | set(stats)
    return {"eye": eye, "world": world, "world_side": kind3,
            "other": {p for p in known if p not in eye and p not in kind3}, "stats": stats}


def census_projections(rows):
    """One camera's kind-3 calls grouped by the projection their composed rows carry (scale and near): a second
    projection on the same object is a second role (the first-person weapon camera shares the scene camera's object
    with a tighter field of view and a larger near plane). Returns groups, biggest first: {xs, ys, near, shift,
    count, frames {frame: n}, roles {role: n}, injected, callers, views}. Calls with no rows are not grouped."""
    groups = []
    for r in rows:
        g = r.get("geo")
        if r["kind"] != 3 or g is None:
            continue
        for grp in groups:
            if abs(g["xs"] - grp["xs"]) <= CENSUS_PROJ_TOL * grp["xs"] and \
                    abs(g["ys"] - grp["ys"]) <= CENSUS_PROJ_TOL * grp["ys"] and \
                    abs(g["near"] - grp["near"]) <= CENSUS_PROJ_TOL * max(abs(grp["near"]), 1e-9):
                break
        else:
            grp = {"xs": g["xs"], "ys": g["ys"], "near": g["near"], "shift": g["shift"], "count": 0, "frames": {},
                   "roles": {}, "injected": 0, "callers": set(), "views": set()}
            groups.append(grp)
        grp["count"] += 1
        grp["frames"][r["frame"]] = grp["frames"].get(r["frame"], 0) + 1
        if r.get("role") is not None:
            grp["roles"][r["role"]] = grp["roles"].get(r["role"], 0) + 1
        if r.get("inj") == 1:
            grp["injected"] += 1
        grp["callers"].add(r["caller"])
        grp["views"].add(r["view"])
    return sorted(groups, key=lambda grp: (-grp["count"], grp["near"]))


def census_call_value(row, name):
    if name == "kind":
        return row["kind"]
    g = row.get("geo")
    return None if g is None else g[name]


def census_separation(c, roles):
    """(A): which fields of a call's own signature (its kind and what its composed rows say: aspect, field of view, near
    plane, off-centre shift) tell the eye cameras' calls from the world-side kind-3 calls. A field separates when no
    eye call has a value any world-side call has. Returns a list of (field, eye values, world-side values), distinct
    values only."""
    rows = census_all_rows(c)
    eye_calls = [r for r in rows if r["camera"] in roles["eye"]]
    world_calls = [r for r in rows if r["kind"] == 3 and r["camera"] not in roles["eye"]]
    out = []
    if not eye_calls or not world_calls:
        return out

    def distinct(calls, name):
        seen = []
        for r in calls:
            v = census_call_value(r, name)
            if v is not None and not any(_cequal(v, s) for s in seen):
                seen.append(v)
        return seen

    for field in CENSUS_CALL_FIELDS:
        ev, wv = distinct(eye_calls, field), distinct(world_calls, field)
        if ev and wv and all(not _cequal(e, w) for e in ev for w in wv):
            out.append((field, ev, wv))
    return out


def census_order(c, roles):
    """(C): per logged sequence, whether every kind-3 call of a world-side camera comes before every call of an eye camera,
    by position, by the tone flag and by the draw ordinal. Returns a list of dicts."""
    out = []
    for seq in c["sequences"]:
        world = [r for r in seq["rows"] if r["kind"] == 3 and r["camera"] in roles["world_side"]]
        eye = [r for r in seq["rows"] if r["camera"] in roles["eye"]]
        entry = {"frame": seq["frame"], "world": len(world), "eye": len(eye),
                 "by_position": None, "world_tone": {}, "eye_tone": {},
                 "world_last_draw": None, "eye_first_draw": None}
        if world and eye:
            entry["by_position"] = max(r["n"] for r in world) < min(r["n"] for r in eye)
        for r in world:
            entry["world_tone"][r["tone"]] = entry["world_tone"].get(r["tone"], 0) + 1
        for r in eye:
            entry["eye_tone"][r["tone"]] = entry["eye_tone"].get(r["tone"], 0) + 1
        wd = [r["draw"] for r in world if r["draw"] is not None]
        ed = [r["draw"] for r in eye if r["draw"] is not None]
        entry["world_last_draw"] = max(wd) if wd else None
        entry["eye_first_draw"] = min(ed) if ed else None
        out.append(entry)
    return out


def _cfmt_tone(counts):
    return ", ".join("%s x%d" % (k, counts[k]) for k in sorted(counts)) or "none"


def census_expected_measure(frustum, dx, dy):
    """What flatCameraMeasureRowShift reads off the rows of a projection built from
    the window {left, right, down, up} moved by (dx, dy): (-(R+L)/(R-L), -(U+D)/(U-D)),
    the model's off-centre terms p8 and p9. None when the window has no width."""
    left, right, down, up = frustum[0] + dx, frustum[1] + dx, frustum[2] + dy, frustum[3] + dy
    if right == left or up == down:
        return None
    return (-(right + left) / (right - left), -(up + down) / (up - down))


def census_shift_fit(meas, frustum, shift):
    """Which way the eye's rows carry the shift EDVR advertised. The DLL's `leak=`
    column assumes the game builds its eye camera from the frustum moved by +shift;
    that sign is not proven, so the reader tries every sign of each axis and the
    unshifted frustum and names the best fit. Returns (label, residual, runner-up
    label, its residual), residuals being the largest |measured - expected| of the
    two axes, in NDC."""
    fits = []
    for name, sx, sy in (("the advertised frustum moved by (+shift.x, +shift.y)", 1, 1),
                         ("the frustum moved by (+shift.x, -shift.y)", 1, -1),
                         ("the frustum moved by (-shift.x, +shift.y)", -1, 1),
                         ("the frustum moved by (-shift.x, -shift.y)", -1, -1),
                         ("the unshifted frustum (the shift is not in the rows)", 0, 0)):
        expected = census_expected_measure(frustum, sx * shift[0], sy * shift[1])
        if expected is None or meas is None or len(meas) != 2:
            continue
        fits.append((max(abs(meas[0] - expected[0]), abs(meas[1] - expected[1])), name))
    if not fits:
        return None
    fits.sort()
    runner = fits[1] if len(fits) > 1 else (None, None)
    return fits[0][1], fits[0][0], runner[1], runner[0]


def census_phase_ndc(phase, width, height):
    """The shift in NDC a phase in render pixels (positive right/down) is, flatProjectionJitter's rule: x = 2 px / W,
    y = -2 py / H (NDC y is up)."""
    return (2.0 * phase[0] / width, -2.0 * phase[1] / height)


def _phase_nonzero(phase):
    return phase is not None and abs(phase[0]) + abs(phase[1]) > 0.0


def census_verdict(c, routes, roles=None, events=None):
    """The stage 2 verdict (design doc section 82, stage 2): did the world phase reach the kind-3 cameras and stay out
    of the eyes? Returns [(tag, status, text)] for (i) LEAK, (ii) KIND-3 ROWS CARRY THE PHASE, (iii) INJECTED KINDS,
    (iv) OFF-THREAD / UNREADABLE, (v) the ROLES and (vi) the INJECTION WINDOW, then the notes; status is PASS, WARN,
    STOP or n/a (what this log cannot say: an older census, a route that never jittered). `events` is route_events(text)."""
    events = events or {}
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    def total(windows, key):
        values = [_cint(route_tok(w, key)) for w in windows]
        return sum(v for v in values if v is not None), sum(1 for v in values if v is not None)

    eyes, seqs = c["eyes"], c["sequences"]
    jitter_on = [w for w in routes if route_tok(w, "jitter") == "on"]
    tokens_seen = any(e["phase_state"] != "absent" for e in eyes) or any(s["phase_state"] != "absent" for s in seqs)
    jittering_seen = bool(jitter_on) or any(e["phase_state"] == "on" for e in eyes) or \
        any(s["phase_state"] == "on" for s in seqs)
    route_tokens = any(route_tok(w, "jitter") is not None for w in routes)
    older = "this log predates stage 2: no phase= token on the census lines%s" % (
        "" if not route_tokens else ", though the route line has jitter=")

    # The render size the world phase is in, from the log itself (the route's hdr=, the JITTERED line, or the panel patch's size): never
    # assumed (5040x2835 is what the legacy width gives a 4032 px eye, not what every rig has).
    size = route_hdr(routes, events)

    # ---- (i) the leak into the eye cameras ----
    nonzero = [e for e in eyes if e["phase_state"] == "on" and _phase_nonzero(e["phase"])]
    geometry = c["geometry"]
    if not tokens_seen and not route_tokens:
        add("i", "n/a", "LEAK: not judged: %s" % older)
    elif not nonzero:
        states = {}
        for e in eyes:
            states[e["phase_state"]] = states.get(e["phase_state"], 0) + 1
        reason = ("%d eye draw(s) were read back (%d with a zero phase, %d with the route not jittering)"
                  % (len(eyes), states.get("on", 0), states.get("off", 0))) if eyes else "no eye draw was read back"
        if jittering_seen:
            early = [e for e in eyes if e["phase_state"] == "off"]
            add("i", "STOP", "LEAK: no eye draw was sampled with a non-zero phase although the route was jittering%s "
                "(%s).%s The census samples a frame only when its phase is non-zero while the route jitters, so the "
                "eye rows were never read in a frame that could leak"
                % (" (jitter=on in %d route window(s))" % len(jitter_on) if jitter_on else "", reason,
                   " %d eye draw(s) were read earlier with phase=- (the route not jittering yet): the eye budget was "
                   "spent before it started; turn the census on after the route owns the world." % len(early)
                   if early else ""))
        else:
            add("i", "n/a", "LEAK: not judged: the route never jittered in this log (%s)" % reason)
    else:
        worst, measured, unmeasured, sign_fit, shifted_eyes = 0.0, 0, 0, 0, 0
        worst_at = None
        for e in nonzero:
            g = geometry.get((e["eye"], e["frame"]))
            leak = None
            if g and g["known"] and g["leak"] and all(v == v for v in g["leak"]):
                leak = max(abs(v) for v in g["leak"])
                # An eye shift that is on (the design has it off while the world is owned) makes `leak=` depend on the
                # sign the game carries it with, which is not proven: take the best fit over the signs.
                if g["shift"] and any(abs(v) > 1e-12 for v in g["shift"]) and g["frustum"] and e["meas"]:
                    shifted_eyes += 1
                    fit = census_shift_fit(e["meas"], g["frustum"], g["shift"])
                    if fit:
                        leak = min(leak, fit[1])
                        sign_fit += 1
            if leak is None:
                unmeasured += 1
                continue
            measured += 1
            if leak > worst:
                worst, worst_at = leak, (e["eye"], e["frame"])
        px = max(max(abs(e["phase"][0]), abs(e["phase"][1])) for e in nonzero)
        if size is not None:
            ndc = max(max(abs(census_phase_ndc(e["phase"], size[0], size[1])[0]), abs(census_phase_ndc(e["phase"], size[0], size[1])[1]))
                      for e in nonzero)
            base = ("%d of %d eye draw(s) had a non-zero world phase (up to %.4f px, about %.1e NDC at %dx%d)"
                    % (len(nonzero), len(eyes), px, ndc, size[0], size[1]))
        else:
            base = ("%d of %d eye draw(s) had a non-zero world phase (up to %.4f px; the render size is not in this log, so no NDC figure)"
                    % (len(nonzero), len(eyes), px))
        # the same frame's two phases (the sequence header's and the eye line's) are one number
        disagree = []
        for e in nonzero:
            for s in seqs:
                if s["frame"] == e["frame"] and s["phase_state"] == "on" and s["phase"] and \
                        (abs(s["phase"][0] - e["phase"][0]) > 1e-3 or abs(s["phase"][1] - e["phase"][1]) > 1e-3):
                    disagree.append(e["frame"])
        if not measured:
            add("i", "STOP", "LEAK: %s, but none could be measured (the eye-geometry line was missing, unavailable or not "
                "finite for %d of them)" % (base, unmeasured))
        elif worst > CENSUS_LEAK_STOP:
            add("i", "STOP", "LEAK: %s; worst |leak| %.2e NDC at eye %d frame %d, over %.0e: the world phase reached the "
                "eye camera (the baseline with no phase is 1e-8)" % (base, worst, worst_at[0], worst_at[1], CENSUS_LEAK_STOP))
        elif worst >= CENSUS_LEAK_PASS or unmeasured or disagree or shifted_eyes:
            why = []
            if worst >= CENSUS_LEAK_PASS:
                why.append("worst |leak| %.2e NDC is between %.0e and %.0e" % (worst, CENSUS_LEAK_PASS, CENSUS_LEAK_STOP))
            if unmeasured:
                why.append("%d eye draw(s) could not be measured" % unmeasured)
            if disagree:
                why.append("the sequence and eye lines of frame(s) %s name different phases"
                           % ", ".join(str(f) for f in sorted(set(disagree))))
            if shifted_eyes:
                why.append("%d eye draw(s) had the eye shift on while the world phase was non-zero (the design has it off "
                           "while owned; their leak is the best fit over the shift's signs)" % shifted_eyes)
            add("i", "WARN", "LEAK: %s; %s; worst |leak| %.2e NDC over %d measured" % (base, "; ".join(why), worst, measured))
        else:
            add("i", "PASS", "LEAK: %s; worst |leak| %.2e NDC over %d measured, below %.0e (the baseline with no phase is "
                "1e-8; a phase leaking into an eye would read 1e-4)" % (base, worst, measured, CENSUS_LEAK_PASS))

    # ---- (ii) the kind-3 calls carry the phase ----
    sampled =[s for s in seqs if s["phase_state"] == "on" and _phase_nonzero(s["phase"])]
    calls_have_inj = any(r["inj"] is not None for r in census_all_rows(c))
    if not seqs or not (tokens_seen or route_tokens):
        add("ii", "n/a", "KIND-3 ROWS CARRY THE PHASE: not judged: %s" % (older if seqs else "no call sequence was logged"))
    elif not sampled:
        if jittering_seen:
            add("ii", "STOP", "KIND-3 ROWS CARRY THE PHASE: no call sequence was logged with a non-zero phase although the "
                "route was jittering (%d sequence(s) logged, %d with phase=-, %d with a zero phase)"
                % (len(seqs), sum(1 for s in seqs if s["phase_state"] == "off"),
                   sum(1 for s in seqs if s["phase_state"] == "on")))
        else:
            add("ii", "n/a", "KIND-3 ROWS CARRY THE PHASE: not judged: the route never jittered in this log")
    elif not calls_have_inj:
        add("ii", "n/a", "KIND-3 ROWS CARRY THE PHASE: not judged: the call lines carry no inj= token (an older census)")
    elif size is None:
        add("ii", "WARN", "KIND-3 ROWS CARRY THE PHASE: the render size is unknown (no `vr world route 5s:` line has a "
            "non-zero hdr=WxH), so the phase in pixels cannot be turned into NDC")
    else:
        width, height = size[0], size[1]
        by_role = {}
        notinj, afterinj = 0, 0
        signs = {"y flipped (y = +2 py / H)": 0, "x flipped (x = -2 px / W)": 0, "both axes flipped": 0}
        bad = 0
        for s in sampled:
            exp = census_phase_ndc(s["phase"], width, height)
            for r in s["rows"]:
                if r["kind"] != 3 or r["geo"] is None or r["role"] not in ("scene", "fp"):
                    continue
                if r["inj"] != 1:
                    if r["tone"] != "after":
                        notinj += 1
                    continue
                if r["tone"] == "after":
                    afterinj += 1
                sx, sy = r["geo"]["shift"]
                err = max(abs(sx - exp[0]), abs(sy - exp[1]))
                b = by_role.setdefault(r["role"], {"n": 0, "worst": 0.0})
                b["n"] += 1
                b["worst"] = max(b["worst"], err)
                if err > CENSUS_PHASE_TOL:
                    # A call that misses its phase: which sign convention, if any, does it fit? (all the misses must fit one)
                    bad += 1
                    for label, kx, ky in (("y flipped (y = +2 py / H)", 1, -1), ("x flipped (x = -2 px / W)", -1, 1),
                                          ("both axes flipped", -1, -1)):
                        if max(abs(sx - kx * exp[0]), abs(sy - ky * exp[1])) <= CENSUS_PHASE_TOL:
                            signs[label] += 1
        checked = sum(b["n"] for b in by_role.values())
        worst = max([b["worst"] for b in by_role.values()] or [0.0])
        desc = ", ".join("%s %d call(s) worst %.2e" % ({"scene": "scene", "fp": "first-person"}[k], by_role[k]["n"],
                                                       by_role[k]["worst"]) for k in ("scene", "fp") if k in by_role)
        where = "expected: x = 2 px / W, y = -2 py / H at %dx%d (from %s)" % (width, height, size[2])
        if not checked:
            add("ii", "STOP", "KIND-3 ROWS CARRY THE PHASE: %d sequence(s) with a non-zero phase hold no injected (inj=1) "
                "scene or first-person call with rows: the phase never reached a kind-3 call (%d scene/first-person call(s) "
                "were not injected)" % (len(sampled), notinj))
        elif worst > CENSUS_PHASE_STOP:
            fit = [k for k, v in sorted(signs.items()) if bad and v == bad]
            add("ii", "STOP", "KIND-3 ROWS CARRY THE PHASE: %d injected call(s) in %d sequence(s) do not measure the phase "
                "they were given (%s; worst |measured - expected| %.2e NDC, over %.0e; %d of them miss)%s; %s"
                % (checked, len(sampled), desc, worst, CENSUS_PHASE_STOP, bad,
                   ("; the rows that miss DO fit the phase with %s" % fit[0]) if fit else "", where))
        elif worst > CENSUS_PHASE_TOL or notinj or afterinj:
            why = []
            if worst > CENSUS_PHASE_TOL:
                why.append("worst |measured - expected| %.2e NDC is between %.0e and %.0e" % (worst, CENSUS_PHASE_TOL, CENSUS_PHASE_STOP))
            if notinj:
                why.append("%d scene/first-person call(s) before the tone were not injected (inj=0)" % notinj)
            if afterinj:
                why.append("%d injected call(s) came after the tone (the window should have closed at the trigger)" % afterinj)
            add("ii", "WARN", "KIND-3 ROWS CARRY THE PHASE: %d injected call(s) in %d sequence(s) (%s); %s; %s"
                % (checked, len(sampled), desc, "; ".join(why), where))
        else:
            add("ii", "PASS", "KIND-3 ROWS CARRY THE PHASE: %d injected kind-3 call(s) in %d sequence(s) with a non-zero "
                "phase measure the phase they were given (%s), within %.0e NDC; %s"
                % (checked, len(sampled), desc, CENSUS_PHASE_TOL, where))

    # ---- (iii) only kind 3 is injected ----
    kinds_windows = [w for w in routes if _ckinds(route_tok(w, "inj-kinds")) is not None]
    injected = {}
    for w in kinds_windows:
        for k, v in _ckinds(route_tok(w, "inj-kinds")).items():
            injected[k] = injected.get(k, 0) + v
    other_injected = {k: v for k, v in injected.items() if k != "3" and v}
    census_bad = [r for r in census_all_rows(c) if r["inj"] == 1 and r["kind"] != 3]
    census_inj = sum(1 for r in census_all_rows(c) if r["inj"] == 1)
    if other_injected or census_bad or events.get("kind-other"):
        parts = []
        if other_injected:
            parts.append("the route's inj-kinds= counts %s" % ", ".join("kind %s x%d" % kv for kv in sorted(other_injected.items())))
        if census_bad:
            parts.append("%d logged call(s) with inj=1 are not kind 3 (%s)"
                         % (len(census_bad), ", ".join(sorted({"k%s" % r["kind"] for r in census_bad}))))
        if events.get("kind-other"):
            parts.append("the route logged: %s" % events["kind-other"][0][:200])
        add("iii", "STOP", "INJECTED KINDS: a kind other than 3 was injected: %s" % "; ".join(parts))
    elif not kinds_windows and not calls_have_inj:
        add("iii", "n/a", "INJECTED KINDS: not judged: no route line has inj-kinds= and no call line has inj= (%s)"
            % older.replace("this log predates stage 2: ", ""))
    elif not injected.get("3") and not census_inj:
        add("iii", "WARN", "INJECTED KINDS: nothing was injected (inj-kinds=none in %d route window(s), no call with inj=1)"
            % len(kinds_windows))
    else:
        add("iii", "PASS", "INJECTED KINDS: only kind 3 was injected (route inj-kinds=3:%d over %d window(s); %d logged call(s) "
            "with inj=1, all kind 3)" % (injected.get("3", 0), len(kinds_windows), census_inj))

    # ---- (iv) nothing off the render thread, nothing unreadable ----
    window_kv = [_ckv(line) for _, line in c["windows"]]
    census_off = sum(_cint(k.get("off-thread")) or 0 for k in window_kv)
    census_unreadable = 0
    for k in window_kv:
        m = re.search(r"unreadable:(\d+)", k.get("kinds", ""))
        if m:
            census_unreadable += int(m.group(1))
    route_off, off_n = total(routes, "off-thread")
    route_unreadable, unr_n = total(routes, "unreadable")
    parts = []
    if census_off:
        parts.append("the census counted %d call(s) off the render thread" % census_off)
    if route_off:
        parts.append("the route counted %d off-thread call(s)" % route_off)
    if route_unreadable:
        parts.append("the route counted %d call(s) whose kind could not be read" % route_unreadable)
    if census_unreadable:
        parts.append("the census counted %d call(s) whose kind could not be read" % census_unreadable)
    if parts:
        add("iv", "STOP", "OFF-THREAD / UNREADABLE: %s" % "; ".join(parts))
    elif not window_kv and not off_n and not unr_n:
        add("iv", "n/a", "OFF-THREAD / UNREADABLE: not judged: no 5 s line to read")
    elif off_n or unr_n:
        add("iv", "PASS", "OFF-THREAD / UNREADABLE: none: census off-thread 0 and no unreadable kind over %d window line(s), "
            "route off-thread 0 and unreadable 0 over %d route window(s)" % (len(window_kv), max(off_n, unr_n)))
    else:
        add("iv", "PASS", "OFF-THREAD / UNREADABLE: none on the census side (off-thread 0 and no unreadable kind over %d window "
            "line(s)); the route's off-thread= and unreadable= tokens are absent (an older route line, or none)" % len(window_kv))

    # ---- (v) the roles ----
    names = ("inj-scene", "inj-fp", "inj-refused", "warming", "aux", "after", "unsupported", "other-kind")
    sums = {n: total(routes, n) for n in names}
    role_calls = {}
    for s in sampled if sampled else []:
        for r in s["rows"]:
            if r["role"] in ("scene", "fp", "aux"):
                t = role_calls.setdefault(r["role"], [0, 0])
                t[0] += 1
                t[1] += 1 if r["inj"] == 1 else 0
    aux_injected = sum(1 for r in census_all_rows(c) if r["role"] == "aux" and r["inj"] == 1)
    if not any(v[1] for v in sums.values()) and not role_calls and not calls_have_inj:
        add("v", "n/a", "ROLES: not judged: no route token and no role= on a call line (%s)"
            % older.replace("this log predates stage 2: ", ""))
    else:
        route_text = ", ".join("%s %d" % (n, sums[n][0]) for n in names if sums[n][1]) or "no route token"
        call_text = ", ".join("%s %d (%d injected)" % ({"scene": "scene", "fp": "first-person", "aux": "auxiliary"}[k],
                                                      role_calls[k][0], role_calls[k][1])
                              for k in ("scene", "fp", "aux") if k in role_calls) or "none in a non-zero-phase sequence"
        body = "ROLES: the route counted %s; logged calls in non-zero-phase sequences: %s" % (route_text, call_text)
        # The weapon (the stage 2 experiment build): the fold-in's mode counts say how many frames drew a weapon (mode 1 or 2), inj-fp how
        # many of its calls the role test credited, and the inject line's fov= range whether the struct carries a second, tighter field of
        # view for the test to find. Flight 2: inj-fp 0 with the fold-in in mode 2 on every weapon frame, and no fov= token.
        modes = [_cmodes(route_tok(w, "fp-mode")) for w in routes]
        m1 = sum(m[1] for m in modes if m)
        m2 = sum(m[2] for m in modes if m)
        folded = m1 + m2
        fov_lo, fov_hi, fov_n = route_fov(routes)
        fp_calls, scene_calls = sums["inj-fp"][0], sums["inj-scene"][0]
        if fov_n:
            body += "; struct field of view %.4f..%.4f rad over %d window(s)" % (fov_lo, fov_hi, fov_n)
        if sums["inj-fp"][1] and fp_calls and folded:
            body += "; the weapon's fold-in ran in %d frame(s) (mode 1: %d, mode 2: %d), about %.1f first-person call(s) credited a frame" % (
                folded, m1, m2, fp_calls / float(folded))
        refused = sums["inj-refused"][0]
        write_fail, _ = total(routes, "write-fail")
        if aux_injected:
            add("v", "STOP", "%s: %d auxiliary call(s) were injected (an excluded role must never get a phase)" % (body, aux_injected))
        elif refused or write_fail:
            add("v", "WARN", "%s: %d call(s) were refused for want of a write and %d write(s) failed (a flush or a restore that failed "
                "can leave a phase in a camera)" % (body, refused, write_fail))
        elif sums["inj-scene"][1] and not sums["inj-scene"][0]:
            add("v", "WARN", "%s: no scene call was injected" % body)
        elif events.get("no-scene-call"):
            add("v", "WARN", "%s; the route logged: %s" % (body, events["no-scene-call"][0][:200]))
        elif sums["inj-fp"][1] and not fp_calls and folded:
            if fov_n and fov_lo < CENSUS_FP_FOV_RATIO * fov_hi:
                why = ("the struct carries two fields of view (%.4f and %.4f rad), so the role test should have told the weapon's calls "
                       "from the scene's: a fault in the field-of-view test" % (fov_lo, fov_hi))
            elif fov_n:
                why = ("the struct carries one field of view (%.4f rad) in every window, so the weapon's calls cannot be told from the "
                       "scene's by it" % fov_hi)
            else:
                why = "no inject line carries a fov= range (a build that predates the field-of-view test: flight 2's picture)"
            add("v", "WARN", "%s: no first-person call was credited (inj-fp 0) although the weapon's fold-in ran in %d frame(s) (mode 1: %d, "
                "mode 2: %d), so the weapon's pixels refuse their history; %s" % (body, folded, m1, m2, why))
        elif fp_calls and m2 and not m1:
            add("v", "WARN", "%s: first-person calls were credited (inj-fp %d) and yet the fold-in ran only in mode 2 (%d frame(s), mode 1 in "
                "none): the frames that credited the weapon are not the frames its map was made in" % (body, fp_calls, m2))
        elif fp_calls and scene_calls and fp_calls >= scene_calls:
            add("v", "WARN", "%s: as many first-person calls as scene calls (%d against %d; flight 2's weapon was 15 of the world's 78): the "
                "role test's scene anchor is probably a wider screen-aspect camera, read the struct field of view range" % (
                    body, fp_calls, scene_calls))
        else:
            add("v", "PASS", body)

    # ---- (vi) the injection window: nothing is injected on a frame whose window the route had shut ----
    shut, shut_n = total(routes, "inj-shut")
    unnamed, unnamed_n = total(routes, "inj-unnamed")
    states = [route_tok(w, "jitter") for w in routes]
    faults = sum(1 for s in states if s == "fault")
    nohook = sum(1 for s in states if s == "no-hook")
    # Episodes the log itself shows in which the route let go or found no source: a run of jitter=unnamed windows, a drop from jitter=on to
    # anything else, and the route's own RELEASED lines. The first map or menu frame after a world frame cannot be told from a world frame
    # before its cameras refresh (they come before any draw), so ONE injected-on-an-unnamed-frame per such change is expected.
    episodes, previous = 0, None
    for s in states:
        if s == "unnamed" and previous != "unnamed":
            episodes += 1
        elif previous == "on" and s not in ("on", "unnamed", None):
            episodes += 1
        previous = s
    episodes += len(events.get("released", []))
    parts = []
    if shut:
        parts.append("inj-shut=%d: camera calls were INJECTED on a frame whose window the route had shut (it must always be 0)" % shut)
    if events.get("shut"):
        parts.append("the route logged: %s" % events["shut"][0][:200])
    # The route's STOP lines other than the two named here: a kind-other STOP is (iii)'s, and a shut-window STOP is quoted once, above.
    stops = [l for l in events.get("stop", []) if "kind other than 3" not in l and l not in events.get("shut", [])]
    if stops:
        parts.append("the route's own STOP line: %s" % stops[0][:200])
    if faults:
        parts.append("jitter=fault in %d route window(s)" % faults)
    if parts:
        add("vi", "STOP", "INJECTION WINDOW: %s" % "; ".join(parts))
    elif not shut_n and not unnamed_n and not states.count("unnamed") and not nohook:
        add("vi", "n/a", "INJECTION WINDOW: not judged: no route line has inj-shut= or inj-unnamed= (an older route line, or none)")
    elif nohook:
        add("vi", "WARN", "INJECTION WINDOW: jitter=no-hook in %d route window(s): the route wanted to jitter and the refresh hook was not live, so "
            "nothing was injected in them; inj-shut=%d" % (nohook, shut))
    elif unnamed > episodes:
        add("vi", "WARN", "INJECTION WINDOW: inj-shut=0, but inj-unnamed=%d exceeds the %d map/menu/release episode(s) this log shows (jitter=unnamed runs, "
            "drops out of jitter=on, RELEASED lines): calls were injected on frames that named no source more often than the scene changed" % (unnamed, episodes))
    else:
        add("vi", "PASS", "INJECTION WINDOW: inj-shut=0 over %d route window(s); inj-unnamed=%d (one per world-to-map change is expected: its first frame "
            "cannot be told from a world frame before the cameras refresh; this log shows %d such episode(s)); jitter=unnamed in %d window(s)"
            % (shut_n, unnamed, episodes, states.count("unnamed")))

    # ---- the pair check and the call rates, as notes ----
    checked, n1 = total(routes, "pair-checked")
    bad, n2 = total(routes, "pair-bad")
    if n1 or n2:
        add("note", "WARN" if bad else "note",
            "the route's own pair check of the rows it believes: %d checked, %d inconsistent%s"
            % (checked, bad, " (the rows do not carry the phase the route claims)" if bad else ""))
    if events.get("excluded"):
        sigs = []
        for line in events["excluded"]:
            m = re.search(r"kind 3 (aspect=\S+ fov=\S+ near=\S+ far=\S+ caller=\S+).*?: (\d+) call\(s\) so far", line)
            sigs.append("%s (%s call(s) so far)" % (m.group(1), m.group(2)) if m else line[:120])
        add("note", "note", "the route excluded %d kind-3 call signature(s) by role and never injected them (they need a role before they get a phase): %s"
            % (len(sigs), "; ".join(sigs[:4]) + ("; ..." if len(sigs) > 4 else "")))
    full = [k for k in window_kv if _cint(k.get("frames")) and k.get("on-foot-frames") == k.get("frames")]
    frames = sum(_cint(k["frames"]) for k in full)
    if frames:
        k5 = 0
        inj = 0
        inj_seen = False
        for k in full:
            m = re.search(r"(?:^|,)5:(\d+)", k.get("kinds", ""))
            k5 += int(m.group(1)) if m else 0
            if _cint(k.get("inj-calls")) is not None:
                inj += _cint(k["inj-calls"])
                inj_seen = True
        text = "a fully-on-foot census window holds %.1f kind-5 calls a frame (design: %.1f, two eyes x three call sites)" % (
            k5 / frames, CENSUS_KIND5_PER_FRAME)
        if inj_seen:
            text += "; %.1f injected calls a frame (design: %d-%d)" % (inj / frames, CENSUS_INJECTED_PER_FRAME[0],
                                                                      CENSUS_INJECTED_PER_FRAME[1])
        add("note", "note", text + " (over %d frame(s) in %d window(s))" % (frames, len(full)))
    return out


def print_stage2_verdict(c, routes, roles=None, events=None, refusals=None):
    """Prints the refusal-census section (when `refusals`, the parsed refusal windows, is given: an empty list says there were none) and
    then the verdict section, and returns the verdict's overall word: STOP, WARN, PASS or n/a. The refusal census has its own closing
    line and never changes the verdict's word."""
    if refusals is not None:
        print_refusal_census(refusals, events)
    print("\n== stage 2 verdict (PASS leak < %.0e NDC, STOP leak > %.0e; kind-3 rows within %.0e NDC of the phase) =="
          % (CENSUS_LEAK_PASS, CENSUS_LEAK_STOP, CENSUS_PHASE_TOL))
    verdict = census_verdict(c, routes, roles, events)
    events = events or {}
    if events.get("jittered") or events.get("released"):
        print("route log: the world was JITTERED from %d episode(s) (`the world is JITTERED from frame=`), RELEASED %d time(s)%s"
              % (len(events.get("jittered", [])), len(events.get("released", [])),
                 "; `camera rows disagree with the phase` %d time(s)" % len(events["rows-disagree"]) if events.get("rows-disagree") else ""))
    if routes:
        print("route lines: %d window(s), %d with jitter=on" % (len(routes), sum(1 for w in routes if route_tok(w, "jitter") == "on")))
    else:
        print("route lines: none (no `vr world route 5s:` line in this log: the route key was off, or this build predates it)")
    for tag, status, text in verdict:
        if status == "note":
            print("      note: %s" % text)
        else:
            print("%-5s (%s) %s" % (status, tag, text))
    statuses = [s for _, s, _ in verdict if s != "note"]
    core = [s for t, s, _ in verdict if t in ("i", "ii", "iii")]
    # A PASS needs the three lines that establish the injection (the eyes did not move, the kind-3 rows carry the phase, only kind 3
    # was injected) to have been judged and passed: a log that predates stage 2 passes (iv) for having nothing off-thread and says n/a.
    overall = "STOP" if "STOP" in statuses else "WARN" if "WARN" in statuses else \
        "PASS" if core and all(s == "PASS" for s in core) else "n/a"
    print("stage 2 verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)%s"
          % (overall, statuses.count("PASS"), statuses.count("WARN"), statuses.count("STOP"), statuses.count("n/a"),
             " -- not judged: this log does not show the injection (see the n/a lines)" if overall == "n/a" else ""))
    return overall


def _phase_text(state, phase):
    if state == "on":
        return "phase (%.4f, %.4f) px" % (phase[0], phase[1])
    if state == "off":
        return "the route was not jittering (phase=-)"
    return "no phase= token (a census that predates stage 2)"


# ---- the episodes (design-world-camera-motion-2026-09-30.md section 6, Phase 0) ----------------------------------------
# One frame sampled 30 frames after a trigger, whatever the frame is (src/d3d11/vr_camera_census_core.h: the lines are written by one function,
# vrCensusPrintEpisode, which tools\vr_camera_census_test also builds tools\camera_census_fixture.log with). Per episode: the header (the trigger, the journal,
# GuiFocus, the naming, the calls by kind and caller over ALL the frame's calls), the calls that printed (up to 120, those that matched something first), the pass's
# chosen rows with the calls whose view axes equal them, and the join: the first draw of each (depth, vertex shader, pixel shader) into the 2D screen's depth or an eye's, whether
# it writes depth, its b1 size and, for the first of each depth, the b1 rows 270..273 read back and the calls that composed them. The DLL matches over every call the frame
# recorded; this reader matches again over the calls that printed (the tolerance of the eye-draw join), so a call the cap kept out is named by its ordinal and said not to be printed.
CENSUS_EPISODE_RUNS_SHOWN = 12


def census_episode_call_label(r):
    """One call as the episode report names it: its camera, kind, caller, view and place against the tone."""
    return "camera 0x%X kind %s caller %s view %s tone %s draw %s" % (
        r["camera"], "-" if r["kind"] is None else r["kind"], "+0x%X" % r["caller"] if r["caller"] is not None else "-",
        "0x%X" % r["view"] if r["view"] else "-", r["tone"], "-" if r["draw"] is None else r["draw"])


def census_episode_join(ep, tol=CENSUS_JOIN_TOL):
    """Each join signature against the episode's printed calls. Returns one dict per signature: {join, printed [call rows whose composed rows are the
    rows the draw read, within tol], listed [the ordinals the DLL listed], unprinted [listed ordinals that are not among the printed calls], more [matches
    the DLL counted past its list], disagree [printed calls the two rules judge differently, when the DLL's list is whole]}. A signature whose rows were not
    read has none of these (`read` False)."""
    by_n = {r["n"]: r for r in ep["rows"]}
    out = []
    for j in ep["joins"]:
        entry = {"join": j, "read": j["rows"] is not None, "printed": [], "listed": list(j["match"] or []), "more": j["match_more"],
                 "unprinted": [], "disagree": []}
        if entry["read"]:
            entry["printed"] = [r for r in ep["rows"] if r["rows"] and len(r["rows"]) == len(j["rows"]) and
                                all(abs(a - b) <= tol for a, b in zip(r["rows"], j["rows"]))]
            entry["unprinted"] = [n for n in entry["listed"] if n not in by_n]
            if j["match"] is not None and not entry["more"]:
                printed_n = {r["n"] for r in entry["printed"]}
                entry["disagree"] = sorted(printed_n.symmetric_difference(n for n in entry["listed"] if n in by_n))
        out.append(entry)
    return out


def census_episode_pass(ep):
    """The pass's chosen rows against the printed calls: {pass, matched [call rows the DLL listed as equal, that printed], unprinted [listed ordinals not
    printed], nearest [the nearest call's row, or None]}, or None when the pass chose nothing in the frame."""
    p = ep["pass"]
    if not p or not p["valid"]:
        return None
    by_n = {r["n"]: r for r in ep["rows"]}
    return {"pass": p, "matched": [by_n[n] for n in p["match"] if n in by_n], "unprinted": [n for n in p["match"] if n not in by_n],
            "nearest": by_n.get(p["nearest"])}


def _kinds_phrase(kinds):
    """{3: 290, 5: 10, "other": 2} -> `k3 x290, k5 x10, kother x2`, kinds in order."""
    order = sorted(kinds, key=lambda k: (not isinstance(k, int), k if isinstance(k, int) else str(k)))
    return ", ".join("k%s x%d" % (k, kinds[k]) for k in order) or "none"


def _calls_phrase(rows):
    """Printed calls grouped by camera: `camera 0x.. kind 5 caller +0x.., +0x.. (n=9/10/11) view 0x.. tone after`, one per camera."""
    by_camera = {}
    for r in rows:
        by_camera.setdefault((r["camera"], r["kind"]), []).append(r)
    parts = []
    for (ptr, kind), ms in sorted(by_camera.items(), key=lambda kv: (kv[1][0]["n"], kv[0][0])):
        parts.append("camera 0x%X (kind %s) caller %s, %d call(s) n=%s, view %s, tone %s"
                     % (ptr, "-" if kind is None else kind, ", ".join(sorted({"+0x%X" % m["caller"] if m["caller"] is not None else "-" for m in ms})),
                        len(ms), "/".join(str(m["n"]) for m in ms),
                        ", ".join(sorted({"0x%X" % m["view"] if m["view"] else "-" for m in ms})), "/".join(sorted({str(m["tone"]) for m in ms}))))
    return "; ".join(parts)


def _depth_label(j):
    return "screen" if j["depth"] == "screen" else ("eye %d" % j["eye"] if j["eye"] is not None else "eye ?")


def _trigger_text(trigger):
    """(kind, from, to) -> `key-on`, `foot no>yes`, `naming unnamed>named`, `gui 0>6`."""
    if not trigger:
        return "?"
    kind, old, new = trigger
    return kind if old is None else "%s %s>%s" % (kind, old, new)


def print_census_episodes(c):
    """The episodes section of --camera-census, then the naming runs (H2) and the detour's CPU (D). Prints nothing for a log that has none of their lines (an
    older census), so a legacy report is byte-for-byte what it was."""
    eps, counters = c["episodes"], c["episode_counters"]
    if not (eps or counters or c["runs"] or c["detour"]):
        return
    last = counters[-1] if counters else None
    head = "%d printed" % len(eps)
    if last:
        head += "; the last `episodes` line: %d taken of %s, %d trigger(s), %d skipped" % (last["taken"], last["of"] if last["of"] is not None else 10,
                                                                                        last["triggers"], last["skipped"])
    print("\n== episodes (%s) ==" % head)
    if not counters:
        print("no `vr camera census: episodes` line (the window's counters): a census that predates the episodes, or a log that ends before its first 5 s window")
    elif last["taken"] > len(eps):
        print("%d episode(s) were armed and not printed: the log ended, or the key went off, before the frame they were to sample%s"
              % (last["taken"] - len(eps), "; one is armed now (trigger %s, armed at frame %s, samples frame %s)" % (
                  _trigger_text(last["trigger"]), last["armed"], last["sample"]) if last["state"] != "idle" and last["trigger"] else ""))
    if last and last["skipped"]:
        print("%d trigger(s) arrived while an episode was armed (or after the session's ten): counted, never sampled" % last["skipped"])
    if not eps:
        print("none: no episode was sampled (the census was on for under 31 frames, or no frame of the session reached an armed trigger's sampled frame)")
    for ep in eps:
        print("episode %d/%s: frame %d, trigger %s (armed at frame %s); journal foot=%s, GuiFocus %s, naming %s, %s"
              % (ep["n"], ep["of"] if ep["of"] is not None else "?", ep["frame"], _trigger_text(ep["trigger"]), ep["armed"], ep["foot"],
                 ep["gui"] if ep["gui"] is not None else "unknown", "NAMED (a draw named the screen's source)" if ep["named"] else "unnamed",
                 _phase_text(ep["phase_state"], ep["phase"])))
        print("    %d call(s), %d recorded%s, %d printed; by kind: %s; by caller: %s%s"
              % (ep["calls"], ep["recorded"], " (%d past the buffer: counted, not recorded)" % (ep["calls"] - ep["recorded"])
                 if ep["calls"] > ep["recorded"] else "", ep["printed"], _kinds_phrase(ep["kinds"]),
                 ", ".join("+0x%X x%d" % (rva, n) for rva, n in sorted(ep["callers"], key=lambda t: (-t[1], t[0]))) or "none",
                 " and %d more caller(s)" % ep["callers_more"] if ep["callers_more"] else ""))
        if ep["printed"] < ep["recorded"]:
            print("    the call lines are capped at 120 an episode (%d of %d recorded calls printed): the calls that matched a join or the pass's rows first, then every kind-5 call, "
                  "then the first call of each run, then the rest in order; the counts above are of all %d calls" % (ep["printed"], ep["recorded"], ep["calls"]))
        if len(ep["rows"]) != ep["printed"]:
            print("    !! %d call line(s) of the %d the header says printed are in this log (the log's own line budget, or lines cut short)"
                  % (len(ep["rows"]), ep["printed"]))
        runs = census_runs(ep["rows"])
        half = CENSUS_EPISODE_RUNS_SHOWN // 2
        for i, (rkind, caller, tone, count, first, last_n) in enumerate(runs):
            if len(runs) > CENSUS_EPISODE_RUNS_SHOWN and half <= i < len(runs) - half:
                if i == half:
                    print("    ... %d more run(s) of the printed calls ..." % (len(runs) - 2 * half))
                continue
            cams = sorted({r["camera"] for r in ep["rows"] if first <= r["n"] <= last_n and (r["kind"], r["caller"], r["tone"]) == (rkind, caller, tone)})
            print("    k%s %s %-6s x%-3d (calls %d..%d) camera %s"
                  % ("-" if rkind is None else rkind, "+0x%X" % caller if caller is not None else "-", tone, count, first, last_n,
                     ", ".join("0x%X" % p for p in cams)))
        joined = census_episode_join(ep)
        jd = ep["join_draws"]
        print("    join: %d signature(s)%s%s (a signature is a depth, a vertex shader and a pixel shader; the first of each depth has its rows read back)"
              % (len(joined), "; %d more did not fit the table (%s draw(s))" % (ep["join_more"]["signatures"], ep["join_more"]["draws"]) if ep["join_more"] else "",
                 "; the per-draw hook was handed %d draw(s), %d of them into a screen- or eye-sized depth, %d depth view(s) resolved" % (jd["seen"], jd["relevant"], jd["views"])
                 if jd else "; no `join-draws` line: the hook's draw counts are not in this log"))
        if jd and not jd["seen"]:
            print("        !! the join's per-draw hook was handed NO draw in the sampled frame: the route's per-draw path never reached it (the census was on, but no draw "
                  "watch ran), so nothing could be joined; this is not 'no draw into a screen- or eye-sized depth'")
        elif not joined:
            print("        none: %s" % ("the hook saw %d draw(s) and none went into a depth of the 2D screen's size or an eye's size" % jd["seen"] if jd
                                        else "no draw of the sampled frame went into a depth of the 2D screen's size or an eye's size (no draw counts to say how many were seen)"))
        for entry in joined:
            j = entry["join"]
            print("        %s %dx%d: first draw %s (%s draw(s)), vs 0x%X ps 0x%X, depth write %s, b1 %s%s"
                  % (_depth_label(j), j["w"], j["h"], "-" if j["draw"] is None else j["draw"], "-" if j["draws"] is None else j["draws"],
                     j["vs"] or 0, j["ps"] or 0, {True: "yes", False: "no", None: "unknown"}[j["dw"]],
                     "0x%X (%s bytes, first constant %s)" % (j["b1"], j["bytes"], j["first"]) if j["b1"] else "not bound",
                     "" if entry["read"] else "; the rows were read but their `join-rows` line is not in this log" if j["read"]
                     else "; rows not read (%s)" % (j["why"] or "?")))
            if not entry["read"]:
                continue
            if entry["printed"]:
                print("            rows 270..273 equal the composed rows of: %s" % _calls_phrase(entry["printed"]))
            elif entry["listed"] or entry["more"]:
                print("            rows 270..273 equal the composed rows of call(s) n=%s, none of which is among the printed calls" % ",".join(str(n) for n in entry["listed"]))
            else:
                print("            rows 270..273 equal the composed rows of NO call of this frame: whatever composed them is not at the refresh (or came from another frame)")
            if entry["unprinted"]:
                print("            the DLL also lists call(s) n=%s%s, not among the printed lines" % (",".join(str(n) for n in entry["unprinted"]),
                                                                                                       " and %d more" % entry["more"] if entry["more"] else ""))
            if entry["disagree"]:
                print("            !! the DLL's match and this reader's disagree on call(s) n=%s" % ",".join(str(n) for n in entry["disagree"]))
        analysis = census_episode_pass(ep)
        if ep["pass"] is None:
            print("    the pass's chosen rows: no `pass-rows` line for this episode")
        elif analysis is None:
            print("    the pass's chosen rows: none (valid=0: the pass chose no camera rows this frame: it is off, or nothing was treated)")
        else:
            p = analysis["pass"]
            relation = "as they are" if p["how"] == "identity" else "transposed" if p["how"] == "transpose" else "-"
            if p["match"] or p["match_more"]:
                print("    the pass's chosen rows (bound block %s) equal the view axes (%s) of: %s%s"
                      % ({True: "yes", False: "no", None: "?"}[p["bound"]], relation, _calls_phrase(analysis["matched"]) or "call(s) n=%s, not printed" % ",".join(str(n) for n in p["match"]),
                         " (+%d more call(s))" % p["match_more"] if p["match_more"] else ""))
                kinds = {r["kind"] for r in analysis["matched"]}
                if kinds and kinds != {5} and 5 in ep["kinds"]:
                    print("    -> the chooser STRAYS: the rows equal a kind %s camera's axes, not an eye camera's (this frame has %d kind-5 call(s))"
                          % ("/".join(sorted(str(k) for k in kinds)), ep["kinds"][5]))
                elif kinds == {5}:
                    print("    -> the rows are an eye camera's (kind 5)")
            else:
                print("    the pass's chosen rows (bound block %s) equal the view axes of NO recorded call; the nearest is call n=%s at a distance of %.3e%s%s"
                      % ({True: "yes", False: "no", None: "?"}[p["bound"]], p["nearest"] if p["nearest"] is not None else "-", p["diff"],
                         " (%s, read %s)" % (census_episode_call_label(analysis["nearest"]), relation) if analysis["nearest"] else "",
                         ": the rows are not a refresh call's axes (or not in the convention compared)" if p["nearest"] is not None else ""))
    if eps:
        print("\n== reading the episodes (facts for H1, H2 and H3, no verdict) ==")
        foot_yes = [e for e in eps if e["foot"] == "yes"]
        unnamed = [e for e in foot_yes if not e["named"]]
        print("H1 (on-foot maps and menus never name a source): %d on-foot episode(s) (journal foot=yes), %d of them unnamed (no draw named the 2D screen's source)%s"
              % (len(foot_yes), len(unnamed), ": episode(s) %s" % ", ".join(str(e["n"]) for e in unnamed) if unnamed else ""))
        for e in unnamed:
            screens = [en for en in census_episode_join(e) if en["join"]["depth"] == "screen"]
            read = [en for en in screens if en["read"]]
            if not screens:
                what = "no draw went into a screen-sized depth"
            elif not read:
                what = "%d signature(s) drew into a screen-sized depth, their rows not read" % len(screens)
            else:
                kinds = sorted({str(r["kind"]) for en in read for r in en["printed"]})
                what = "%d signature(s) drew into a screen-sized depth; the first draw's rows %s" % (
                    len(screens), "equal the composed rows of kind %s call(s)" % "/".join(kinds) if kinds else "equal no printed call's composed rows")
            print("    episode %d: %s" % (e["n"], what))
        aboard = [e for e in eps if e["foot"] in ("no", "off")]
        print("H3 (the cockpit's maps are driven by kind-5 eye cameras' rows): %d aboard episode(s) (journal foot=no or off)" % len(aboard))
        for e in aboard:
            parts = []
            for en in census_episode_join(e):
                if en["join"]["depth"] == "eye" and en["read"]:
                    kinds = sorted({str(r["kind"]) for r in en["printed"]})
                    parts.append("%s depth rows equal %s" % (_depth_label(en["join"]), "kind %s call(s)" % "/".join(kinds) if kinds else "no printed call's rows"))
            analysis = census_episode_pass(e)
            if analysis:
                kinds = sorted({str(r["kind"]) for r in analysis["matched"]})
                parts.append("the pass's rows equal %s" % ("kind %s call(s)' axes" % "/".join(kinds) if kinds else "no recorded call's axes (nearest n=%s)" % analysis["pass"]["nearest"]))
            print("    episode %d (%s, GuiFocus %s): %s" % (e["n"], _trigger_text(e["trigger"]), e["gui"] if e["gui"] is not None else "unknown",
                                                            "; ".join(parts) or "no eye-depth rows read and no pass rows"))
    print_census_runs(c)
    print_census_detour(c)


def census_runs_total(c):
    """The runs lines summed: {named {bin: n}, unnamed {bin: n}, frames, longest_named, longest_unnamed, windows, lines}."""
    total = {"named": dict.fromkeys(CENSUS_RUN_BINS, 0), "unnamed": dict.fromkeys(CENSUS_RUN_BINS, 0), "frames": 0, "longest_named": 0,
             "longest_unnamed": 0, "windows": 0, "lines": len(c["runs"])}
    for r in c["runs"]:
        for way in ("named", "unnamed"):
            for b in CENSUS_RUN_BINS:
                total[way][b] += r[way][b]
        total["frames"] += r["frames"]
        total["longest_named"] = max(total["longest_named"], r["longest_named"])
        total["longest_unnamed"] = max(total["longest_unnamed"], r["longest_unnamed"])
        total["windows"] += r["windows"] or 0
    return total


def print_census_runs(c):
    if not c["runs"]:
        return
    t = census_runs_total(c)
    print("\n== on-foot naming runs (the `runs` lines summed over %d 5 s window(s); frames the journal says on foot: %d) ==" % (t["windows"], t["frames"]))
    print("a run is consecutive on-foot frames that all named the 2D screen's source (named) or none did (unnamed), counted when it ends")
    print("%-9s %s" % ("run of", "  ".join("%6s" % b for b in CENSUS_RUN_BINS)))
    for way in ("named", "unnamed"):
        print("%-9s %s" % (way, "  ".join("%6d" % t[way][b] for b in CENSUS_RUN_BINS)))
    long_unnamed = sum(t["unnamed"][b] for b in CENSUS_RUN_BINS[2:])
    print("longest named run %d frame(s), longest unnamed run %d frame(s)" % (t["longest_named"], t["longest_unnamed"]))
    print("H2 (the longest unnamed run in an on-foot world stays under 3 frames): %d unnamed run(s) of 3 frames or more, and %d of 1 or 2; a map or a menu opened on foot is "
          "one of the long ones, so this reads the world only over a stretch with none open" % (long_unnamed, t["unnamed"]["1"] + t["unnamed"]["2"]))
    last = c["runs"][-1]
    if last["open"]:
        print("the run open at the last window: %s for %d frame(s) so far" % last["open"])


def print_census_detour(c):
    if not c["detour"]:
        return
    lines = c["detour"]
    frames = sum(d["frames"] or 0 for d in lines)
    calls = sum(d["calls"] or 0 for d in lines)
    sampled = sum(d["sampled"] or 0 for d in lines)
    every = lines[0]["every"]
    print("\n== the detour's CPU (the observer's two halves, %s call in %s timed; %d `detour` line(s) over %d 5 s window(s)) =="
          % ("1" if every else "?", every if every else "?", len(lines), sum(d["windows"] or 0 for d in lines)))
    print("what is timed: %s -- the census's work inside the detour (the clock is read at the start and end of each half). The detour's own prologue, the injection's "
          "writes and the game's body are not in it." % (lines[0]["timed"] or "?"))
    print("%d frame(s), %d refresh call(s), %d timed" % (frames, calls, sampled))
    est = [(d["est_ms"], d["frames"] or 0) for d in lines if d["est_ms"] is not None]
    if est:
        weight = sum(f for _, f in est)
        mean = sum(e * f for e, f in est) / float(weight) if weight else sum(e for e, _ in est) / float(len(est))
        print("estimated ms a frame: mean %.3f over %d window line(s) (lowest %.3f, highest %.3f); calls a frame: %.1f"
              % (mean, len(est), min(e for e, _ in est), max(e for e, _ in est), calls / float(frames) if frames else 0.0))
    else:
        print("estimated ms a frame: none (no window had both calls and timed calls)")
    for name, label in (("obs", "observed calls (the detour did not inject for them)"), ("inj", "injected calls (the route's phase was written for them)")):
        n_calls = sum(d["modes"][name]["calls"] or 0 for d in lines)
        n_sampled = sum(d["modes"][name]["sampled"] or 0 for d in lines)
        parts = []
        for half in ("pre", "post"):
            weighted = [(d["modes"][name][half], d["modes"][name]["sampled"] or 0) for d in lines if d["modes"][name][half]]
            weight = sum(w for _, w in weighted)
            if weight:
                parts.append("%s half: mean %.3g us, longest %.3g us" % (half, sum(p[0] * w for p, w in weighted) / float(weight), max(p[1] for p, _ in weighted)))
        print("%s: %d call(s), %d timed%s" % (label, n_calls, n_sampled, "; " + "; ".join(parts) if parts else "; nothing timed"))


def _camera_role_label(ptr, roles):
    return "EYE" if ptr in roles["eye"] else "WORLD" if ptr == roles["world"] \
        else "world-side" if ptr in roles["world_side"] else "other"


def print_camera_census(text):
    """The --camera-census report. Returns the process exit code: 0 when the log
    has census lines, 1 when it has none."""
    c = parse_camera_census(text)
    if not c["lines"]:
        print("[edvr] camera census: no `vr camera census` line in this log. The key "
              "advanced.vr_camera_census was off, this is not a VR-profile log, or the "
              "build predates the census.")
        return 1
    routes = parse_world_route(text)
    events = route_events(text)
    refusals = parse_refusal_windows(text)
    print("[edvr] camera census: %d census line(s): %d 5 s line(s), %d camera(s), "
          "%d call sequence(s), %d eye draw(s), %d other-thread entr%s%s"
          % (c["lines"], len(c["windows"]), len(c["order"]), len(c["sequences"]),
             len(c["eyes"]), len(c["threads"]), "y" if len(c["threads"]) == 1 else "ies",
             ", %d episode(s)" % len(c["episodes"]) if c["episodes"] else ""))
    if c["unparsed"]:
        print("[edvr]   %d census line(s) could not be parsed (cut short or garbled) and were skipped" % c["unparsed"])
    for info in c["info"]:
        print("[edvr]   note: %s" % info)

    print("\n== 5 s lines ==")
    if not c["windows"]:
        print("none: the census never printed a window (a log that ends within five "
              "seconds of the key going on, or a census that did not run)")
    for stamp, line in c["windows"]:
        print("%s vr camera census 5s: %s" % (stamp, line))
    window_kv = [_ckv(line) for _, line in c["windows"]]
    off_thread = sum(int(k.get("off-thread", "0")) for k in window_kv if k.get("off-thread", "0").isdigit())
    if off_thread:
        print("!! %d refresh call(s) ran on a thread other than the render thread "
              "(off-thread): the owner-thread assumption failed; the other-thread "
              "entries below name them" % off_thread)
    elif window_kv:
        print("off-thread calls: 0 in every window -- the refresh runs on the render thread")
    if window_kv and not any(k.get("progress") == "yes" for k in window_kv):
        print("!! progress=no in every window: the world route never reported its draw progress, so every "
              "call prints draw=- tone=none and the place against the tone draw (C) cannot be read; frames "
              "were sampled by the journal alone (foot=yes), so a call sequence and eye rows exist only if "
              "the journal said on foot")
    tone_frames = sum(int(k.get("tone-frames", "0")) for k in window_kv if k.get("tone-frames", "0").isdigit())
    sampled_frames = sum(int(k.get("on-foot-frames", "0")) for k in window_kv if k.get("on-foot-frames", "0").isdigit())
    if window_kv:
        feet = sorted({k.get("foot", "?") for k in window_kv})
        print("frames with the tone drawn: %d; sampled (tone while the journal says on foot, or no journal; while the "
              "world route jitters, with a non-zero phase): %d; the journal said: %s"
              % (tone_frames, sampled_frames, ", ".join(feet)))
    if routes:
        states = {}
        for w in routes:
            states[route_tok(w, "jitter") or "(no jitter= token)"] = states.get(route_tok(w, "jitter") or "(no jitter= token)", 0) + 1
        print("world route: %d 5 s window(s); jitter: %s" % (len(routes), ", ".join("%s x%d" % (k, states[k]) for k in sorted(states))))
    if tone_frames and not sampled_frames:
        if any(route_tok(w, "jitter") == "on" for w in routes):
            print("!! the tone was drawn in %d frame(s) but no frame was sampled: the route was jittering, and the census "
                  "samples a frame then only with a non-zero phase (or the journal never said on foot: foot=no is a ship, "
                  "foot=unknown a menu). Nothing below can be joined." % tone_frames)
        else:
            print("!! the tone was drawn in %d frame(s) but no frame was sampled: the journal never said on foot "
                  "(foot=no is a ship, foot=unknown a menu). Nothing below can be joined; disembark and fly again."
                  % tone_frames)
    for t in c["threads"]:
        print("other-thread: tid %s camera %s kind %s caller %s calls %s"
              % (t["tid"], _cg(t["camera"]), t["kind"], _cg(t["caller"]), t["calls"]))

    join = census_join(c)
    roles = census_roles(c, join)
    stats = roles["stats"]
    print("\n== cameras (%d, in first-seen order; `1st` is the kind of the camera's FIRST call ever, `calls` the kinds of its "
          "logged calls) ==" % len(c["order"]))
    if c["order"]:
        print("%-14s %-4s %-9s %-10s %-8s %-7s %-7s %-24s %-14s %-14s %-6s %-5s %-16s %s"
              % ("camera", "1st", "caller", "aspect", "near", "fov", "far", "bound",
                 "viewport", "view", "tone", "frame", "calls", "role / changes"))
    for ptr in c["order"]:
        cam = c["cameras"][ptr]
        st = stats.get(ptr)
        changes = len(c["changes"].get(ptr, []))
        label = _camera_role_label(ptr, roles) if st else "not in a logged sequence"
        print("0x%-12X %-4s %-9s %-10s %-8s %-7s %-7s %-24s %-14s %-14s %-6s %-5s %-16s %s%s"
              % (ptr, cam["kind"], "+0x%X" % cam["caller"] if cam["caller"] is not None else "-",
                 _cg(cam["aspect"]), _cg(cam["near"]), _cg(cam["fov"]), _cg(cam["far"]),
                 _cg(cam["bound"]), _cg(cam["viewport"]),
                 "0x%X" % cam["view"] if cam["view"] else "-", cam["tone"], cam["frame"],
                 "/".join("k%s x%d" % (k, st["kinds"][k]) for k in sorted(st["kinds"], key=lambda k: (k is None, k or 0))) if st else "-",
                 label, "; %d 'changed:' line(s)" % changes if changes else ""))
    for ptr in c["order"]:
        for ch in c["changes"].get(ptr, [])[:2]:
            print("    0x%X frame %s moved: %s" % (ptr, ch["frame"], ", ".join(
                "%s %s" % (k, v) for k, v in sorted(ch["fields"].items()))))

    # ---- what each camera is, from the kind of each of its calls ----
    by_camera = {}
    for seq in c["sequences"]:
        for r in seq["rows"]:
            by_camera.setdefault(r["camera"], []).append(r)
    print("\n== roles from the calls (the KIND of each logged call decides; the camera object's first-seen kind does not) ==")
    if not stats:
        print("none: no call sequence was logged, so no camera can be labelled by its calls")
    shown = [p for p in c["order"] if p in stats] + sorted(p for p in stats if p not in c["cameras"])
    for ptr in shown:
        st = stats[ptr]
        nframes = len(st["frames"])
        first = c["cameras"].get(ptr, {}).get("kind")
        kinds = st["kinds"]
        line = "camera 0x%X %s: %s over %d logged frame(s) (%.1f a frame)" % (
            ptr, _camera_role_label(ptr, roles), _ckinds_text(kinds), nframes, st["calls"] / float(max(nframes, 1)))
        k3 = kinds.get(3, 0)
        if k3:
            line += "; kind-3 calls before the tone %d, after %d%s" % (
                st["before"], st["after"], ", no tone info %d" % st["none"] if st["none"] else "")
        if first is not None and first not in kinds:
            line += "; first seen as kind %s" % first
        if len([k for k in kinds if k is not None]) > 1:
            per = ", ".join("frame %d: %s" % (f, _ckinds_text(st["frames"][f])) for f in sorted(st["frames"]))
            line += "; SEVERAL KINDS (%s)" % per
        print(line)
        if k3:
            groups = census_projections(by_camera[ptr])
            for i, g in enumerate(groups[:CENSUS_PROJ_SHOWN]):
                nf = float(max(len(g["frames"]), 1))
                ratio = ""
                if i and groups[0]["ys"] > 0:
                    tighter = g["ys"] / groups[0]["ys"]
                    ratio = ", x%.3f %s than projection 1" % (tighter if tighter >= 1 else 1 / tighter,
                                                              "tighter" if tighter >= 1 else "wider")
                roles_seen = ", ".join("%s x%d" % (k, g["roles"][k]) for k in sorted(g["roles"]))
                note = ""
                if i and g["near"] > groups[0]["near"] and g["ys"] > groups[0]["ys"]:
                    note = " -- the first-person weapon camera's signature: the same object, a tighter field of view and a larger near plane"
                print("    projection %d: %.1f call(s) a frame, scale %.4f x %.4f (aspect %.4f), near %.6g%s, off-centre (%.2e, %.2e)%s%s%s"
                      % (i + 1, g["count"] / nf, g["xs"], g["ys"], g["ys"] / g["xs"], g["near"], ratio, g["shift"][0], g["shift"][1],
                         "; role= %s" % roles_seen if roles_seen else "",
                         "; injected %d of %d" % (g["injected"], g["count"]) if any(r["inj"] is not None for r in by_camera[ptr]) else "",
                         note))
            if len(groups) > CENSUS_PROJ_SHOWN:
                print("    ... %d more projection(s) ..." % (len(groups) - CENSUS_PROJ_SHOWN))

    print("\n== call sequences (%d frame(s), calls reduced to runs of kind/caller/tone) ==" % len(c["sequences"]))
    if not c["sequences"]:
        print("none: no on-foot frame (the tone was never seen) while a sequence was still wanted")
    for seq in c["sequences"]:
        print("frame %d (sequence %s, journal foot=%s, %s): %d call(s), %d recorded, %d truncated"
              % (seq["frame"], seq["index"], seq["foot"] or "?", _phase_text(seq["phase_state"], seq["phase"]),
                 seq["calls"], seq["recorded"], seq["truncated"]))
        # Per camera first: how many calls, from which callers, on which side of the tone, over which draws.
        digest = {}
        for r in seq["rows"]:
            d = digest.setdefault(r["camera"], {"kinds": {}, "n": 0, "callers": {}, "tone": {}, "draws": [], "views": set(),
                                                "inj": 0, "roles": {}})
            d["n"] += 1
            d["kinds"][r["kind"]] = d["kinds"].get(r["kind"], 0) + 1
            d["views"].add(r["view"])
            d["callers"][r["caller"]] = d["callers"].get(r["caller"], 0) + 1
            d["tone"][r["tone"]] = d["tone"].get(r["tone"], 0) + 1
            if r["inj"] == 1:
                d["inj"] += 1
            if r["role"] in ("scene", "fp", "aux"):
                d["roles"][r["role"]] = d["roles"].get(r["role"], 0) + 1
            if r["draw"] is not None:
                d["draws"].append(r["draw"])
        for ptr, d in sorted(digest.items(), key=lambda kv: min(r["n"] for r in seq["rows"] if r["camera"] == kv[0])):
            print("    camera 0x%X %s: %d call(s), callers %s, view %s, tone %s, draws %s%s"
                  % (ptr, "+".join("k%s" % ("-" if k is None else k) for k in sorted(d["kinds"], key=lambda k: (k is None, k or 0))),
                     d["n"],
                     ", ".join("+0x%X x%d" % (k, v) if k is not None else "- x%d" % v
                               for k, v in sorted(d["callers"].items(), key=lambda kv: (kv[0] is None, kv[0] or 0))),
                     ", ".join("0x%X" % v if v else "-" for v in sorted(d["views"], key=lambda v: v or 0)),
                     _cfmt_tone(d["tone"]),
                     "%d..%d" % (min(d["draws"]), max(d["draws"])) if d["draws"] else "-",
                     ("; injected %d%s" % (d["inj"], " (%s)" % ", ".join("%s %d" % (k, d["roles"][k]) for k in sorted(d["roles"]))
                                          if d["roles"] else "")) if d["inj"] or d["roles"] else ""))
        runs = census_runs(seq["rows"])
        half = CENSUS_RUNS_SHOWN // 2
        for i, (kind, caller, tone, count, first, last) in enumerate(runs):
            if len(runs) > CENSUS_RUNS_SHOWN and half <= i < len(runs) - half:
                if i == half:
                    print("    ... %d more run(s) ..." % (len(runs) - 2 * half))
                continue
            cams = sorted({r["camera"] for r in seq["rows"]
                           if first <= r["n"] <= last and (r["kind"], r["caller"], r["tone"]) == (kind, caller, tone)})
            print("    k%s %s %-6s x%-3d (calls %d..%d) camera %s"
                  % ("-" if kind is None else kind, "+0x%X" % caller if caller is not None else "-",
                     tone, count, first, last, ", ".join("0x%X" % p for p in cams)))

    print("\n== eye draws (b1 rows 270..273 read back from the GPU) ==")
    if not c["eyes"]:
        print("none: no eye composite draw in an on-foot frame reached the readback")
    for j in join:
        e = j["eye"]
        print("eye %d frame %d draw %s (journal foot=%s, %s) b1 0x%X first=%s bytes=%s" %
              (e["eye"], e["frame"], e["draw"] if e["draw"] is not None else "-", e["foot"] or "?",
               _phase_text(e["phase_state"], e["phase"]), e["b1"] or 0, e["first"], e["bytes"]))
        if e["rows"]:
            print("    rows   %s" % ", ".join("%.7g" % v for v in e["rows"]))
            print("    meas   %s (flatCameraMeasureRowShift: the off-centre terms of the rows)" % _cg(e["meas"]))
        else:
            print("    rows   unavailable (%s)" % (e["why"] or "?"))
        g = c["geometry"].get((e["eye"], e["frame"]))
        if g and g["known"]:
            print("    EDVR advertised seq %s frustum %s shift %s; expected measure %s, shifted %s; leak %s"
                  % (g["seq"], _cg(tuple(g["frustum"]) if g["frustum"] else None), _cg(g["shift"]),
                     _cg(g["expect"]), _cg(g["expect_shifted"]), _cg(g["leak"])))
            fit = census_shift_fit(e["meas"], g["frustum"], g["shift"]) if g["frustum"] and g["shift"] else None
            if fit and fit[3] is not None and fit[3] - fit[1] < CENSUS_FIT_TIE:
                # No winner to name: sorting a tie would pick a label by its spelling.
                print("    the rows cannot tell which way the shift is carried: every candidate leaves about %.2e NDC "
                      "(the advertised shift, %s, is too small to separate them from rounding)"
                      % (fit[1], _cg(g["shift"])))
            elif fit:
                print("    the rows measure as %s (residual %.2e NDC; next best: %s, %.2e)"
                      % (fit[0], fit[1], fit[2], fit[3]))
        else:
            print("    EDVR advertised geometry: unavailable")
    leaks = [max(abs(v) for v in g["leak"]) for g in c["geometry"].values()
             if g["known"] and g["leak"] and all(v == v for v in g["leak"])]
    if leaks:
        print("leak measure over %d eye draw(s): largest |leak| = %.3e NDC (with no world phase injected this is the "
              "baseline a leak detector must clear; with one, the stage 2 verdict below judges it for each eye draw whose "
              "frame has a non-zero phase; a phase of half a pixel at 5040 wide is 2e-4; `leak` assumes the game builds "
              "its eye camera from the frustum moved by +shift, which the fit line above checks: `the rows measure as`, "
              "or `cannot tell` when the shift is too small)"
              % (len(leaks), max(leaks)))

    print("\n== the offline join (B): eye rows against the rows of every logged call, tolerance %g ==" % CENSUS_JOIN_TOL)
    if not join:
        print("nothing to join: no eye draw was read back")
    for j in join:
        e = j["eye"]
        label = "eye %d frame %d draw %s" % (e["eye"], e["frame"], e["draw"] if e["draw"] is not None else "-")
        if j["scope"] == "no-rows":
            print("%s: no rows were read (%s)" % (label, e["why"] or "?"))
        elif j["scope"] == "no-sequence":
            print("%s: frame %d's call sequence was not logged (only the first %d on-foot frames are), so its rows "
                  "cannot be joined" % (label, e["frame"], 3))
        elif j["scope"] == "none":
            print("%s: NO logged call of frame %d produced these rows. The eye's camera does not reach the refresh "
                  "with them: either it is composed by another of the composer's callers, or by a path the detour "
                  "does not see. (B) does not identify it." % (label, e["frame"]))
        else:
            cams = {}
            for m in j["matches"]:
                cams.setdefault(m["camera"], []).append(m)
            for ptr, ms in sorted(cams.items()):
                print("%s: camera 0x%X (kind %s) caller %s, %d call(s) n=%s, draw %s, tone %s%s"
                      % (label, ptr, "/".join(sorted({str(m["kind"]) for m in ms})),
                         ", ".join(sorted({"+0x%X" % m["caller"] for m in ms})),
                         len(ms), "/".join(str(m["n"]) for m in ms),
                         "/".join(str(m["draw"]) for m in ms if m["draw"] is not None) or "-",
                         "/".join(sorted({m["tone"] for m in ms})),
                         "" if j["scope"] == "frame" else " (another frame's call: the same pose)"))
            if len(cams) > 1:
                print("    %d different cameras composed identical rows for this draw" % len(cams))
    # The field signature the eye rows belong to: each joined camera's first-sight line, once.
    for ptr in [p for p in c["order"] if p in roles["eye"]]:
        cam = c["cameras"][ptr]
        print("eye camera 0x%X signature: kind %s, caller %s, aspect %s, near %s, far %s, fov %s, bound %s, offcentre %s, "
              "viewport %s, tangents %s, view %s"
              % (ptr, cam["kind"], "+0x%X" % cam["caller"] if cam["caller"] is not None else "-", _cg(cam["aspect"]),
                 _cg(cam["near"]), _cg(cam["far"]), _cg(cam["fov"]), _cg(cam["bound"]), _cg(cam["offcentre"]),
                 _cg(cam["viewport"]), _cg(cam["tan"]), "0x%X" % cam["view"] if cam["view"] else "-"))

    print("\n== which signal separates the eye cameras (A)-(F) ==")
    if not roles["eye"]:
        candidates = sorted(p for p, s in stats.items() if s["kinds"].get(5))
        print("no camera was joined to an eye draw, so no eye camera is known and nothing can be separated. "
              "%s%s" % ("(The join found no matching call in a logged frame: see above.)" if join else "(No eye draw was read back.)",
                        " Cameras with kind-5 calls in the logged sequences (candidates, not joined): %s"
                        % ", ".join("0x%X" % p for p in candidates) if candidates else ""))
        print_census_episodes(c)
        print_stage2_verdict(c, routes, roles, events, refusals)
        return 0
    print("eye camera(s): %s; world camera: %s; other world-side kind-3 camera(s): %s"
          % (", ".join("0x%X" % p for p in c["order"] if p in roles["eye"]),
             "0x%X" % roles["world"] if roles["world"] else "none found",
             ", ".join("0x%X" % p for p in c["order"] if p in roles["world_side"] and p != roles["world"]) or "none"))
    if roles["world"]:
        st = stats[roles["world"]]
        nframes = max(len(st["frames"]), 1)
        print("world camera 0x%X: %d kind-3 call(s) over %d logged frame(s) = %.1f a frame, %d of them before the tone "
              "(%d projection(s); its first-seen kind was %s)"
              % (roles["world"], st["kinds"].get(3, 0), nframes, st["kinds"].get(3, 0) / float(nframes), st["before"],
                 len(census_projections(by_camera[roles["world"]])),
                 c["cameras"].get(roles["world"], {}).get("kind", "unknown")))
    separating = census_separation(c, roles)
    if separating:
        print("(A) call signature (kind, and what a call's composed rows say: aspect, fov, near, off-centre): %s separate every "
              "eye camera from EVERY world-side kind-3 camera:" % ", ".join(f for f, _, _ in separating))
        for field, ev, ov in separating:
            print("      %-9s eye %s  vs  world-side %s" % (field, ", ".join(_cg(v) for v in ev[:4]), ", ".join(_cg(v) for v in ov[:4])))
    else:
        print("(A) call signature: no single field (kind, aspect, fov, near, off-centre) separates the eye cameras' calls from the "
              "world-side kind-3 calls")
    print("(B) content join: %d of %d eye draw(s) joined to a camera; eye camera(s) %s"
          % (sum(1 for j in join if j["matches"]), len(join),
             ", ".join("0x%X" % p for p in c["order"] if p in roles["eye"])))
    order = census_order(c, roles)
    decided = [o for o in order if o["by_position"] is not None]
    ok = [o for o in decided if o["by_position"]]
    if decided:
        print("(C) place in the frame: every refresh of a world-side camera precedes every refresh of an eye camera "
              "in %d of %d logged sequence(s) (the world side being the kind-3 calls)" % (len(ok), len(decided)))
        for o in decided:
            print("      frame %d: world-side %d call(s) [tone %s; last draw %s], eye %d call(s) [tone %s; first draw %s]%s"
                  % (o["frame"], o["world"], _cfmt_tone(o["world_tone"]),
                     o["world_last_draw"] if o["world_last_draw"] is not None else "-", o["eye"],
                     _cfmt_tone(o["eye_tone"]), o["eye_first_draw"] if o["eye_first_draw"] is not None else "-",
                     "" if o["by_position"] else "  <- INTERLEAVED"))
        unknown = any("none" in o["world_tone"] or "none" in o["eye_tone"] for o in decided)
        tone_clean = all(set(o["world_tone"]) <= {"before"} and set(o["eye_tone"]) <= {"after"} for o in decided)
        print("      the tone flag %s" % (
            "is unavailable (tone=none: the route reported no draw progress for these calls)" if unknown
            else "separates the two sets" if tone_clean else "does NOT separate the two sets"))
    else:
        print("(C) place in the frame: no logged sequence holds both a world-side and an eye camera's call")
    eye_callers, world_callers = set(), set()
    for seq in c["sequences"]:
        for r in seq["rows"]:
            if r["camera"] in roles["eye"]:
                eye_callers.add(r["caller"])
            elif r["camera"] == roles["world"] and r["kind"] == 3:
                world_callers.add(r["caller"])
    fmt = lambda s: ", ".join("+0x%X" % x for x in sorted(v for v in s if v is not None)) or "none"
    shared = eye_callers & world_callers
    print("(D) caller: eye camera(s) call from %s; the world camera from %s; %s"
          % (fmt(eye_callers), fmt(world_callers),
             ("no caller is shared: the caller separates them" if eye_callers and world_callers and not shared
              else "shared: %s -- the caller alone does not separate them" % fmt(shared) if shared
              else "not enough calls logged to say")))
    eye_views, world_views = set(), set()
    for seq in c["sequences"]:
        for r in seq["rows"]:
            if r["camera"] in roles["eye"]:
                eye_views.add(r["view"])
            elif r["camera"] == roles["world"] and r["kind"] == 3:
                world_views.add(r["view"])
    vfmt = lambda s: ", ".join("0x%X" % v for v in sorted(x for x in s if x)) or "none"
    shared_views = (eye_views & world_views) - {None, 0}
    print("(F) view (the refresh's second argument, the pass object): eye camera(s) are refreshed with %s; the world "
          "camera with %s; %s"
          % (vfmt(eye_views), vfmt(world_views),
             ("no view is shared: the view separates them" if eye_views - {None, 0} and world_views - {None, 0} and not shared_views
              else "shared: %s -- the view alone does not separate them" % vfmt(shared_views) if shared_views
              else "not enough calls logged to say")))
    worst, compared = 0.0, 0
    for j in join:
        e = j["eye"]
        g = c["geometry"].get((e["eye"], e["frame"]))
        if not g or not g["known"] or not g["frustum"] or not j["matches"]:
            continue
        cam = c["cameras"].get(j["matches"][0]["camera"])
        if not cam or not cam["tan"] or len(cam["tan"]) != 4 or cam["frame"] != e["frame"]:
            continue
        diff = max(abs(a - b) for a, b in zip(cam["tan"], g["frustum"]))
        diff_flip = max(abs(a - b) for a, b in zip(cam["tan"], (g["frustum"][0], g["frustum"][1], -g["frustum"][3], -g["frustum"][2])))
        worst = max(worst, min(diff, diff_flip))
        compared += 1
    if compared:
        print("(E) tangents: the eye camera's tangents match the frustum EDVR advertised for its eye to within %.2e "
              "(compared on %d draw(s) in the camera's first frame)" % (worst, compared))
    elif all(not (c["cameras"].get(p) or {}).get("tan") for p in roles["eye"]):
        print("(E) tangents: not compared: the eye cameras' lines carry no tangents (tan=-: a kind-5 camera is the game's custom "
              "matrix, not a frustum); the eye-geometry leak= above is the comparison with what EDVR advertised")
    else:
        print("(E) tangents: no eye draw fell in the frame an eye camera's line was printed, so the camera's tangents "
              "and the advertised frustum were not compared (their difference is the eye shift, about 1e-4, per frame)")
    print_census_episodes(c)
    print_stage2_verdict(c, routes, roles, events, refusals)
    return 0


# --maps-sharp: the on-foot maps gate (experimental.on_foot_maps_sharp, docs/design-world-camera-motion-2026-09-30.md, Phase 1).
# The lines it reads are written by src/d3d11/ui_maps_math.h's formatters (their wording is the anchors below); the rig
# tools/on_foot_maps_test compares tools/maps_sharp_fixture.log to those formatters byte for byte, and this reader's self-test
# parses the same file, so a formatter that drifts fails in the build rather than in the ten minutes after a flight.
MAPS_STAMP_RE = re.compile(r"^\[(?P<ts>\d\d:\d\d:\d\d\.\d{3})\] (?P<msg>.*)$")
# The journal's reading is text the DLL substitutes into "(the journal: %s)", and one of its readings has parentheses of its own ("no
# Flags2 in Status.json (a menu, or no file yet)": every arrival, before the game writes Status.json), so the groups below take
# everything up to the fixed words after them, never up to the first ")".
MAPS_ON_RE = re.compile(r"^on foot maps sharp: ON at frame=(?P<frame>\d+) .*gate starts as .*: (?P<start>the world|not the world) "
                        r"\(the journal: (?P<journal>.*)\)\.$")
# The OFF line's reason is "(why)", and three of the DLL's reasons have parentheses of their own ("no temporal mode is on (fix.temporal_aa is
# off)", "the eye jitter is not as shipped (...)", "screen motion is not live (...)"), so it takes one level of nesting: a pattern that stopped
# at the first ")" lost the OFF line of a real flight (edvr_gfx_20261001_085519.log, fix.temporal_aa off) as a line it did not know.
MAPS_OFF_RE = re.compile(r"^on foot maps sharp: OFF at frame=(?P<frame>\d+) \((?P<why>(?:[^()]|\([^()]*\))*)\): ")
MAPS_TAKE_RE = re.compile(r"^on foot maps sharp: the layer TAKES the 2D screen at frame=(?P<frame>\d+): no world camera named its source "
                          r"for (?P<run>\d+) frames in a row \(after (?P<world>\d+) world frames, (?P<secs>[0-9.]+) s; the journal: "
                          r"(?P<journal>.*)\)\. A map or a menu is sharp from the layer")
MAPS_BACK_RE = re.compile(r"^on foot maps sharp: the layer HANDS BACK the 2D screen at frame=(?P<frame>\d+) after (?P<frames>\d+) panel "
                          r"frames \((?P<secs>[0-9.]+) s; (?P<only>\d+) eyes through the layer-only door, (?P<kept>\d+) kept the "
                          r"upscaler because the game drew something else into them\): (?P<why>.*)\.$")
MAPS_NOTEMPTY_RE = re.compile(r"^on foot maps sharp: the layer took the 2D screen for eye (?P<eye>\d) \(sequence (?P<seq>\d+)\) but the "
                              r"game drew (?P<draws>\d+) draw\(s\) into eye-sized targets this frame and the layer took (?P<taken>\d+): ")
MAPS_NOTLIVE_RE = re.compile(r"^on foot maps sharp: experimental\.on_foot_maps_sharp is on but .*: (?P<why>[^:]*)\.$")
MAPS_WINDOW_RE = re.compile(r"^on foot maps sharp 5s: key=on (?P<secs>[0-9.]+) s mode=(?P<mode>\w+) gate=(?P<gate>world|panel) "
                            r"frames=(?P<frames>\d+) named=(?P<named>\d+) unnamed=(?P<unnamed>\d+) world-frames=(?P<world>\d+) "
                            r"panel-frames=(?P<panel>\d+) holds=(?P<holds>\d+) releases=(?P<releases>\d+) "
                            r"screen-takes=(?P<takes>\d+) recognised=(?P<recognised>\d+) door-layer-only=(?P<only>\d+) "
                            r"door-not-empty=(?P<notempty>\d+) not-live-frames=(?P<notlive>\d+)(?: screen-draws=(?P<draws>\d+))?")
# `draws` is the 2D screen composites the layer's decision SAW in the window (taken or not); a log from a build before the Phase 1 fix
# (design doc 8.10) has no such token and reads None: the reader then judges by takes alone and says it cannot tell "nothing drawn"
# from "drawn and refused".
MAPS_ROUTE_RELEASED_RE = re.compile(r"vr world route: RELEASED the world at frame=(?P<frame>\d+) \((?P<why>[^)]*)\)")
MAPS_ROUTE_OWNS_RE = re.compile(r"vr world route: OWNS the world from frame=(?P<frame>\d+) ")
# A black eye is the sharpen door's own failure line ("... got NO composite from the UI layer -- the eye is BLACK", printed only on the
# failure path). The luma probe's "first black stage is X" lines are NOT black-eye evidence: the probe prints one whenever the first
# black stage CHANGES, including "is none" the moment a black arrival gives way to a world, and "is game" for as long as the layer
# holds the screen (the game's own eye image is empty by construction). Four real flights each carry 8 to 20 of them; reading them as
# black eyes made every real flight STOP. The probe's sample lines are kept per panel period instead (MAPS_LUMA_RE), as a note.
MAPS_BLACK_NEEDLES = ("native sharpen: LAYER-ONLY eye",)
MAPS_LUMA_RE = re.compile(r"^luma probe: eye=(?P<eye>\d) game=(?P<game>\S+) dlss_out=(?P<dlss>\S+) final=(?P<final>\S+)$")
MAPS_LUMA_STAGE_RE = re.compile(r"^(?P<mean>[0-9.]+)/(?P<max>[0-9.]+)/(?P<black>\d+)%$")
MAPS_SHORT_PANEL_FRAMES = 10     # a panel period shorter than this (0.11 s at 90 Hz) is a flap in the world, not a map or a menu a person opened
MAPS_DOOR_SHARE = 0.9            # eyes through the layer-only door, of the 2 x panel-frames a panel period should have had
MAPS_SAME_BOUNDARY_S = 0.05      # the route lets go on the gate's boundary: its RELEASED line is within this of the TAKES line


def _clock_s(ts):
    h, m, rest = ts.split(":")
    return int(h) * 3600 + int(m) * 60 + float(rest)


def parse_maps_sharp(text):
    """The feature's lines, in log order: {events: [{kind, ts, t, ...}], windows: [{ts, t, ...ints}], route: [{kind, ts, t, ...}],
    black: [{ts, t, line}], declined: [...], luma: [{ts, t, eye, final_black, final_mean}]}. A line that starts "on foot maps sharp"
    but is none of the kinds goes in `unparsed`. `luma` holds the probe's sample lines (the final stage's black share), which
    maps_sharp_episodes lays against the panel periods."""
    out = {"events": [], "windows": [], "route": [], "black": [], "declined": [], "unparsed": [], "luma": []}
    for raw in text.splitlines():
        m = MAPS_STAMP_RE.match(raw.rstrip("\r"))
        if not m:
            continue
        ts, msg = m.group("ts"), m.group("msg")
        t = _clock_s(ts)
        if msg.startswith("on foot maps sharp 5s:"):
            w = MAPS_WINDOW_RE.match(msg)
            if not w:
                out["unparsed"].append(raw)
                continue
            d = {k: int(v) for k, v in w.groupdict().items() if k not in ("secs", "mode", "gate", "draws")}
            d.update(ts=ts, t=t, secs=float(w.group("secs")), mode=w.group("mode"), gate=w.group("gate"),
                     draws=int(w.group("draws")) if w.group("draws") is not None else None)
            out["windows"].append(d)
        elif msg.startswith("on foot maps sharp:"):
            for kind, rx in (("on", MAPS_ON_RE), ("off", MAPS_OFF_RE), ("take", MAPS_TAKE_RE), ("back", MAPS_BACK_RE),
                             ("notempty", MAPS_NOTEMPTY_RE), ("notlive", MAPS_NOTLIVE_RE)):
                e = rx.match(msg)
                if e:
                    ev = {"kind": kind, "ts": ts, "t": t}
                    for k, v in e.groupdict().items():
                        ev[k] = float(v) if k == "secs" else int(v) if v.isdigit() else v
                    out["events"].append(ev)
                    break
            else:
                out["unparsed"].append(raw)
        elif "vr world route: RELEASED the world" in msg:
            e = MAPS_ROUTE_RELEASED_RE.search(msg)
            if e:
                out["route"].append({"kind": "released", "ts": ts, "t": t, "frame": int(e.group("frame")), "why": e.group("why")})
        elif "vr world route: OWNS the world" in msg:
            e = MAPS_ROUTE_OWNS_RE.search(msg)
            if e:
                out["route"].append({"kind": "owns", "ts": ts, "t": t, "frame": int(e.group("frame"))})
        elif msg.startswith("luma probe: eye=") and " game=" in msg:
            lm = MAPS_LUMA_RE.match(msg)
            if lm:
                fm = MAPS_LUMA_STAGE_RE.match(lm.group("final"))
                out["luma"].append({"ts": ts, "t": t, "eye": int(lm.group("eye")),
                                    "final_black": int(fm.group("black")) if fm else None,
                                    "final_mean": float(fm.group("mean")) if fm else None})
        elif any(n in msg for n in MAPS_BLACK_NEEDLES):
            out["black"].append({"ts": ts, "t": t, "line": msg[:160]})
        elif "native temporal: layer-only declined" in msg:
            out["declined"].append({"ts": ts, "t": t, "line": msg[:200]})
    return out


def maps_sharp_episodes(p):
    """Panel periods: each TAKES line paired with the next HANDS BACK or OFF line. {take, end, frames, secs, only, kept, why, route_released,
    route_owns, luma_samples, luma_black, open}. `open` is a TAKES still unanswered at the end of the log.

    A period also starts at an ON line whose gate starts as a panel (the key on, or the layer live, while the screen already shows
    nothing the world camera names: the main menu, a load, the arrival after it). Its `take` is that ON line, marked from_on, with
    no world frames behind it. Without it the arrival with the key on from launch had no TAKES line, and its HANDS BACK closed
    nothing the report could show."""
    eps = []
    cur = None
    for ev in p["events"]:
        if ev["kind"] == "on" and ev.get("start") == "not the world":
            if cur is not None:
                cur["open"] = True
                eps.append(cur)
            cur = {"take": {"kind": "take", "ts": ev["ts"], "t": ev["t"], "frame": ev["frame"], "run": 0, "world": 0, "secs": 0.0,
                            "journal": ev["journal"], "from_on": True}, "end": None, "open": False}
        elif ev["kind"] == "take":
            if cur is not None:   # a second take with no hand-back between (a switch in and out): the earlier one ended unseen
                cur["open"] = True
                eps.append(cur)
            cur = {"take": ev, "end": None, "open": False}
        elif ev["kind"] in ("back", "off") and cur is not None:
            cur["end"] = ev
            if ev["kind"] == "back":
                cur.update(frames=ev["frames"], secs=ev["secs"], only=ev["only"], kept=ev["kept"], why=ev["why"])
            else:
                cur.update(frames=None, secs=ev["t"] - cur["take"]["t"], only=None, kept=None, why="the gate stopped: " + ev["why"])
            eps.append(cur)
            cur = None
    if cur is not None:
        cur["open"] = True
        eps.append(cur)
    for e in eps:
        t0 = e["take"]["t"]
        # A period that began at the ON line had no world to let go of: no route release belongs to it.
        rel = [] if e["take"].get("from_on") else [r for r in p["route"] if r["kind"] == "released" and abs(r["t"] - t0) <= 1.0]
        e["route_released"] = min(rel, key=lambda r: abs(r["t"] - t0)) if rel else None
        # The luma probe's samples inside the period: the final stage is the texture handed to the VR half, the layer's composite
        # included. Black by content after a load (nothing is drawn); black under a map is a defect. The judge tells them apart by
        # what the journal said when the period began.
        end_t = e["end"]["t"] if e["end"] is not None else float("inf")
        samples = [l for l in p["luma"] if t0 <= l["t"] <= end_t and l["final_black"] is not None]
        e["luma_samples"] = len(samples)
        e["luma_black"] = sum(1 for l in samples if l["final_black"] >= 99)
        # What the 2D screen did in the period, from the 5 s windows that overlap it (a window is stamped when it closes). Composites are
        # only ever TAKEN in panel frames, so the taken count needs no trimming. A window with world frames in it cannot say how many of
        # the composites it saw were drawn in its panel frames, so the drawn count uses only the whole-panel windows (gate=panel at its
        # close and no world frame in it): there, 0 drawn means nothing was on the 2D screen (the cockpit after boarding, a loading
        # black) and a drawn count above the taken count means the layer left composites in the game's frame.
        wins = [w for w in p["windows"] if w["t"] > t0 and w["t"] - w["secs"] < end_t]
        e["takes"] = sum(w["takes"] for w in wins)
        pure = [w for w in wins if w["gate"] == "panel" and w["world"] == 0 and w["panel"] > 0]
        e["pure_windows"] = len(pure)
        e["pure_frames"] = sum(w["panel"] for w in pure)
        e["pure_takes"] = sum(w["takes"] for w in pure)
        e["pure_draws"] = sum(w["draws"] for w in pure) if pure and all(w["draws"] is not None for w in pure) else None
        if e["end"] is not None and e["end"]["kind"] == "back":
            owns = [r for r in p["route"] if r["kind"] == "owns" and 0 <= r["t"] - e["end"]["t"] <= 2.0]
            e["route_owns"] = min(owns, key=lambda r: r["t"]) if owns else None
        else:
            e["route_owns"] = None
    return eps


def maps_sharp_judge(p, eps):
    """(stops, warns, notes): lists of sentences. STOP is what would ruin a flight (the design's STOP list: a release in the world, flapping, a
    black eye); WARN is what says the feature did not do all it should (an eye kept the upscaler, a door that ran for too few eyes, the route
    and the gate letting go apart); notes are facts."""
    stops, warns, notes = [], [], []
    ws = p["windows"]
    for w in ws:
        if w["takes"] and not w["recognised"]:
            stops.append("%s: %d taken 2D screen composites and none recognised -- screen motion's recognition never ran for a taken screen, so "
                         "the panel could never come back to the eye route (the design's trap)" % (w["ts"], w["takes"]))
        elif w["takes"] and w["recognised"] * 10 < w["takes"] * 9:
            warns.append("%s: only %d of %d taken composites were recognised" % (w["ts"], w["recognised"], w["takes"]))
        if w["notlive"]:
            warns.append("%s: the key is on but the gate was not decided by naming for %d frame(s)" % (w["ts"], w["notlive"]))
        if w["notempty"]:
            warns.append("%s: %d eye(s) had the screen taken but the game drew something else into an eye-sized target, so the upscaler ran for "
                         "them (door-not-empty)" % (w["ts"], w["notempty"]))
        # The door should have run for every composite the layer TOOK (an eye whose 2D screen was taken with nothing else drawn into it): the
        # expectation is the taken composites, not the panel frames. A panel frame with nothing on the 2D screen (the cockpit after boarding,
        # a loading black) has nothing taken and nothing for the door to skip; the first flight read as a failed door because this rule
        # counted panel frames (design doc 8.10).
        if w["takes"] and w["only"] < MAPS_DOOR_SHARE * w["takes"] and not w["notempty"]:
            warns.append("%s: %d taken composites but only %d eyes through the layer-only door (expected about %d): the upscaler ran for the rest "
                         "without a counted reason" % (w["ts"], w["takes"], w["only"], w["takes"]))
        # A window that was panel from start to end, in which the decision saw more 2D screen composites than the layer took: composites
        # the gate gave the layer and the layer left in the game's frame. (Only the new 5 s line counts what the decision saw.)
        if w["draws"] is not None and w["gate"] == "panel" and w["world"] == 0 and w["panel"] and w["draws"] > w["takes"]:
            warns.append("%s: the gate gave the layer the panel for the whole window and the decision saw %d 2D screen composites, but only %d were "
                         "taken: the layer left %d in the game's frame" % (w["ts"], w["draws"], w["takes"], w["draws"] - w["takes"]))
    for e in eps:
        t0 = e["take"]["ts"]
        if e["takes"] == 0 and e["pure_windows"]:
            if e["pure_draws"] == 0:
                notes.append("%s: no 2D screen composite was drawn in this panel period's %d whole-panel window(s) (%d frames): the cockpit after "
                             "boarding, a load, anything that draws no screen. Nothing for the layer to take and nothing for the door to skip"
                             % (t0, e["pure_windows"], e["pure_frames"]))
            elif e["pure_draws"] is None:
                notes.append("%s: the layer took no 2D screen composite in this panel period's %d whole-panel window(s) (%d frames). This log's 5 s "
                             "line has no screen-draws, so \"nothing was drawn\" cannot be told from \"drawn and not taken\" here; the journal, the "
                             "on-foot source lines and the layer's own 30 s `2D screen draws asked` can" % (t0, e["pure_windows"], e["pure_frames"]))
        if e["luma_black"]:
            if e["take"].get("journal") == "on foot" and e["takes"] > 0:
                warns.append("%s: the luma probe read the final stage black in %d of %d sample(s) of a panel period the journal calls on foot: a "
                             "map or a menu should be on screen, so look at the headset's picture for this stretch"
                             % (t0, e["luma_black"], e["luma_samples"]))
            elif e["takes"] == 0:
                notes.append("%s: the luma probe read the final stage black in %d of %d sample(s) of this panel period, in which the layer held no "
                             "2D screen: not the layer's picture. A load's black, a fade, the game closing (the first flight's four samples were "
                             "its exit fade, seconds before the shutdown totals)" % (t0, e["luma_black"], e["luma_samples"]))
            else:
                notes.append("%s: the luma probe read the final stage black in %d of %d sample(s) of this panel period (the journal: %s): black "
                             "by content when nothing is drawn after a load, so this is the arrival and not a black eye"
                             % (t0, e["luma_black"], e["luma_samples"], e["take"].get("journal", "?")))
        if e["end"] is None or e["open"]:
            notes.append("%s: the panel period starting here was still open at the end of the log" % t0)
            continue
        if e["frames"] is not None and e["frames"] < MAPS_SHORT_PANEL_FRAMES:
            stops.append("%s: a panel period of %d frames (%.2f s) -- shorter than a person opens a map or a menu: the gate released in the world "
                         "and held again (a flap)" % (t0, e["frames"], e["secs"]))
        if e["kept"]:
            warns.append("%s: %d eye(s) of this panel period kept the upscaler because something else was drawn into them" % (t0, e["kept"]))
        if e["route_released"] is not None and abs(e["route_released"]["t"] - e["take"]["t"]) > MAPS_SAME_BOUNDARY_S:
            warns.append("%s: the VR world route let go %.0f ms from the gate (not the same boundary)"
                         % (t0, 1000 * abs(e["route_released"]["t"] - e["take"]["t"])))
    for b in p["black"]:
        stops.append("%s: a black eye was reported (%s)" % (b["ts"], b["line"]))
    for d in p["declined"]:
        warns.append("%s: the layer-only door declined an eye (%s)" % (d["ts"], d["line"]))
    if not ws:
        warns.append("no 5 s window line: the feature printed nothing while the key was on, so it never ran")
    if p["unparsed"]:
        warns.append("%d 'on foot maps sharp' line(s) the reader does not know (the formatters changed?); the first: %s"
                     % (len(p["unparsed"]), p["unparsed"][0].strip()[:200]))
    if not any(e["end"] is not None and not e["open"] and e["end"]["kind"] == "back" for e in eps):
        warns.append("no panel period closed in this log: nothing was taken and handed back (a map or a menu opened and closed is what "
                     "the flight is for)")
    return stops, warns, notes


def _maps_period_screens(e):
    """One line on what the 2D screen did in a panel period's whole-panel windows (no world frame in them), or None when it had none."""
    if not e["pure_windows"]:
        return None
    if e["pure_draws"] is None:
        return ("      in the period's %d whole-panel window(s) (%d frames) the layer took %d 2D screen composite(s); this log's 5 s line does not "
                "count the composites the decision saw" % (e["pure_windows"], e["pure_frames"], e["pure_takes"]))
    return ("      2D screen composites in the period's %d whole-panel window(s) (%d frames): drawn %d, taken %d%s"
            % (e["pure_windows"], e["pure_frames"], e["pure_draws"], e["pure_takes"],
               " -- none was drawn: nothing for the layer to take or the door to skip (a cockpit, a load)" if e["pure_draws"] == 0 else ""))


def print_maps_sharp(text):
    """--maps-sharp: the on-foot maps gate's flight in one report; exit 0 (PASS or WARN), 1 (STOP), 3 (no line of the feature in the log)."""
    p = parse_maps_sharp(text)
    if not p["events"] and not p["windows"]:
        print("[edvr] maps-sharp: no 'on foot maps sharp' line in this log. The key experimental.on_foot_maps_sharp was off, the UI layer "
              "was not live, or this build does not have the feature; with the key on and the layer live the feature prints an ON line and "
              "a 5 s line, zeros included.")
        return 3
    eps = maps_sharp_episodes(p)
    ws = p["windows"]
    tot = {k: sum(w[k] for w in ws) for k in ("frames", "named", "unnamed", "world", "panel", "holds", "releases", "takes", "recognised",
                                               "only", "notempty", "notlive")}
    print("[edvr] maps-sharp: %d event line(s), %d five-second window(s), %d panel period(s)" % (len(p["events"]), len(ws), len(eps)))
    for ev in p["events"]:
        if ev["kind"] == "on":
            print("  %s  ON   frame %d, the gate starts as %s (the journal: %s)" % (ev["ts"], ev["frame"], ev["start"], ev["journal"]))
        elif ev["kind"] == "off":
            print("  %s  OFF  frame %d (%s)" % (ev["ts"], ev["frame"], ev["why"]))
        elif ev["kind"] == "notlive":
            print("  %s  NOT LIVE: %s" % (ev["ts"], ev["why"]))
        elif ev["kind"] == "notempty":
            print("  %s  eye %d sequence %d: %d eye draws, the layer took %d" % (ev["ts"], ev["eye"], ev["seq"], ev["draws"], ev["taken"]))
    print("panel periods (a map or a menu the layer held, or a stretch with nothing for a world camera to name: a load, the arrival after it):")
    for e in eps:
        t = e["take"]
        if t.get("from_on"):
            lead = "%s  ON frame %d, the screen already a panel (the journal: %s)" % (t["ts"], t["frame"], t["journal"])
            lead_open = lead_closed = lead
        else:
            lead_open = "%s  TAKES frame %d after %d world frames (%.1f s)" % (t["ts"], t["frame"], t["world"], t["secs"])
            lead_closed = "%s  TAKES frame %d after %d world frames (%.1f s of world)" % (t["ts"], t["frame"], t["world"], t["secs"])
        if e["end"] is None or e["open"]:
            print("  %s -> still open at the end of the log" % lead_open)
            screens = _maps_period_screens(e)
            if screens:
                print(screens)
            continue
        if e["frames"] is not None:
            tail = "%d frames (%.1f s), %d eyes layer-only, %d kept the upscaler -> %s" % (e["frames"], e["secs"], e["only"], e["kept"], e["why"])
        else:
            tail = "%.1f s, then %s" % (e["secs"], e["why"])
        print("  %s  ->  %s  %s" % (lead_closed, e["end"]["ts"], tail))
        if e["route_released"] is not None:
            print("      the VR world route let go at %s (%+.0f ms from the take: %s)"
                  % (e["route_released"]["ts"], 1000 * (e["route_released"]["t"] - t["t"]), e["route_released"]["why"]))
        if e["route_owns"] is not None:
            print("      the route owned the world again at %s (%.0f ms after the hand-back)"
                  % (e["route_owns"]["ts"], 1000 * (e["route_owns"]["t"] - e["end"]["t"])))
        if e["luma_samples"]:
            print("      luma probe: %d sample(s) in this period, the final stage black in %d (black by content for a loading stretch, a "
                  "defect under a map or a menu)" % (e["luma_samples"], e["luma_black"]))
        screens = _maps_period_screens(e)
        if screens:
            print(screens)
    if ws:
        print("5 s windows, summed (%d): %d frames, named %d / unnamed %d, world %d / panel %d, %d hold(s), %d release(s); %d screen takes, %d "
              "recognised; %d eyes through the layer-only door, %d kept the upscaler; %d frame(s) not decided by naming"
              % (len(ws), tot["frames"], tot["named"], tot["unnamed"], tot["world"], tot["panel"], tot["holds"], tot["releases"], tot["takes"],
                 tot["recognised"], tot["only"], tot["notempty"], tot["notlive"]))
        if tot["panel"]:
            # Against the composites the layer TOOK, not 2 x the panel frames: a panel frame with nothing on the 2D screen (the cockpit after
            # boarding) takes nothing and has nothing for the door to skip (the first flight's "46.1%" counted those frames as failures).
            if tot["takes"]:
                print("  panel frames: %d; composites taken: %d, and the layer-only door ran for %.1f%% of them"
                      % (tot["panel"], tot["takes"], 100.0 * tot["only"] / tot["takes"]))
            else:
                print("  panel frames: %d; the layer took no 2D screen composite" % tot["panel"])
            if all(w["draws"] is not None for w in ws):
                print("  the decision saw %d 2D screen composites in these windows, %d of them taken"
                      % (sum(w["draws"] for w in ws), tot["takes"]))
    stops, warns, notes = maps_sharp_judge(p, eps)
    for n in notes:
        print("  note: %s" % n)
    for w in warns:
        print("  WARN: %s" % w)
    for s in stops:
        print("  STOP: %s" % s)
    verdict = "STOP" if stops else "WARN" if warns else "PASS"
    print("maps-sharp verdict: %s (%d STOP, %d WARN). PASS: every map and menu was taken within 3 frames (the TAKES line) and handed back within "
          "2 named frames of closing, no flap in the world, no black eye, the recognition ran for every taken composite, the door ran for the "
          "eyes. STOP: a release in the world, a flap, a black eye, a taken screen never recognised. WARN: an eye kept the upscaler, a door "
          "short of its eyes, the route and the gate letting go apart." % (verdict, len(stops), len(warns)))
    return 1 if stops else 0


# --tally periodic: the phase-0 timing of periodic work, laid against long
# frames. What each source writes, and on which clock (read out of the source
# 2026-09-29):
#
#   graphics log  Log::note (src/common/log.cpp) prefixes "[HH:MM:SS.mmm] " from
#                 GetLocalTime: LOCAL time of day, no date. The file name is local
#                 as well and carries the date.
#   runtime log   nativeTracePrintf -> NativeTrace::write (src/openxr/
#                 native_trace.h) prefixes "YYYY-MM-DD HH:MM:SS.mmm UTC pid=<pid>
#                 tid=<tid> " from GetSystemTime: UTC. Its FILE NAME is local
#                 (GetLocalTime, with millisecond and pid fields), which gives the
#                 UTC offset without asking this machine's time zone.
#   periodic work "periodic work: <op> n=<runs> total=<ms> max=<ms> at HH:MM:SS.mmm
#                 slow=<runs>[ <label>=<v>]" (a 30 s summary) and "periodic work:
#                 <op> SLOW ms=<ms> at HH:MM:SS.mmm[ <label>=<v>]" (one slow run),
#                 src/common/periodic_work.h. `at` is GetLocalTime in BOTH logs
#                 (frame_cycle_report goes to the runtime log with the same local
#                 clock, native_runtime_host.h recordFrameCycleReport), read when
#                 the run FINISHED, so the run covers [at - ms, at].
#   LONG FRAME    "monitor: LONG FRAME -- <ms> ms between Presents ..." in the
#                 graphics log, src/d3d11/perf_monitor.cpp: written by the
#                 post-Present block that measured the frame, so the line's time is
#                 the frame's END and the frame covers [time - ms, time]. The DLL
#                 rate-limits these lines: a sample, not a count.
#   long cycle    "native_long_cycle,sequence=..,cycle_ms=..,period_ms=.." in the
#                 runtime log, native_runtime_host.h noteLongCycle: written as the
#                 next WaitGetPoses returns, so again the END. Its session-close
#                 line "native_long_cycle_summary,count=..,logged=.." has the true
#                 count. The LONG FRAME line's "runtime sequence N" is the same
#                 counter as the cycle's sequence= (to within a frame), which is
#                 how the two clocks are checked against each other.
#
# Everything is put on one clock: seconds since 2000-01-01 00:00 LOCAL, as a
# float, so a subtraction is a duration whichever log a time came from.

GFX_STAMP_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\] ")
RT_STAMP_RE = re.compile(
    r"^(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3}) UTC pid=\d+ tid=\d+ ")
PERIODIC_SLOW_RE = re.compile(
    r"periodic work: (?P<op>\w+) SLOW ms=(?P<ms>[0-9.]+) "
    r"at (?P<at>\d\d:\d\d:\d\d\.\d{3})(?: (?P<label>\w+)=(?P<ctx>\d+))?(?:\s|$)")
PERIODIC_WINDOW_RE = re.compile(
    r"periodic work: (?P<op>\w+) n=(?P<n>\d+) total=(?P<total>[0-9.]+) "
    r"max=(?P<max>[0-9.]+) at (?P<at>\d\d:\d\d:\d\d\.\d{3}) slow=(?P<slow>\d+)"
    r"(?: (?P<label>\w+)=(?P<ctx>\d+))?(?:\s|$)")
# Anchored on the message start: the flip-timeline dump also says "monitor:
# LONG FRAME" but continues "-- this line is about FRAME", which is no frame.
LONG_FRAME_RE = re.compile(
    r"monitor: LONG FRAME -- (?P<ms>[0-9.]+) ms between Presents")
LONG_FRAME_SEQ_RE = re.compile(r"\bruntime sequence (?P<seq>\d+)")
LONG_FRAME_CAP_RE = re.compile(
    r"monitor: (?P<logged>\d+) dropped or long frames were logged this session "
    r"\(of (?P<cap>\d+) at most\)")
LONG_CYCLE_RE = re.compile(
    r"native_long_cycle,sequence=(?P<seq>\d+),cycle_ms=(?P<ms>[0-9.]+),"
    r"period_ms=(?P<period>[0-9.]+),")
LONG_CYCLE_SUMMARY_RE = re.compile(
    r"native_long_cycle_summary,count=(?P<count>\d+),logged=(?P<logged>\d+)")

DAY = 86400.0
EPOCH = datetime.datetime(2000, 1, 1)
PAIR_TOLERANCE_S = 900.0    # a runtime log opens within seconds of its graphics log
# The operations the source times, in the order they are listed. A new one
# still tallies when it appears; this only lets "not seen" name what is absent.
KNOWN_OPS = {"gfx": ("journal_status", "journal_tail", "journal_reglob",
                     "xinput_probe", "luma_round", "ui_layer_totals"),
             "rt": ("frame_cycle_report",)}
SRC_LABEL = {"gfx": "LONG FRAME", "rt": "native_long_cycle"}
MATCHED_ROWS = 30   # rows of section 3; a busy flight has more matches than a reader needs


def log_name_time(name):
    """(local datetime the file name says the log opened, tag, has the
    millisecond and pid fields), or None when the name is not an EDVR log's."""
    m = LOG_RE.match(os.path.basename(name))
    if not m:
        return None
    try:
        opened = datetime.datetime.strptime(m.group("stamp"), "%Y%m%d_%H%M%S")
    except ValueError:
        return None
    if m.group("ms") is not None:
        opened += datetime.timedelta(milliseconds=int(m.group("ms")))
    return opened, m.group("tag").lower(), m.group("ms") is not None


def _utc_of(m):
    """The datetime an RT_STAMP_RE match names (naive, UTC), or None."""
    try:
        return datetime.datetime(int(m.group(1)), int(m.group(2)),
                                 int(m.group(3)), int(m.group(4)),
                                 int(m.group(5)), int(m.group(6)),
                                 int(m.group(7)) * 1000)
    except ValueError:
        return None


def runtime_to_local(rt_path, rt_text):
    """(to_local, how, first line's UTC datetime): what to add to the runtime
    log's UTC prefix to get local time, and where that came from.

    The file name is local time (GetLocalTime) and the first line follows the
    file's creation by milliseconds, so first line minus name is the UTC offset
    to within a second; offsets are whole quarter hours, so it rounds. A name
    without the millisecond and pid fields (an older log) or a first line that
    disagrees with it falls back to this machine's zone, and `how` says so."""
    first_utc = None
    for raw in rt_text.splitlines():
        m = RT_STAMP_RE.match(raw)
        if m:
            first_utc = _utc_of(m)
            if first_utc is not None:
                break
    if first_utc is None:
        return (datetime.timedelta(0),
                "no timestamped line to read it from; treated as UTC", None)
    named = log_name_time(rt_path) if rt_path else None
    if named and named[1] == "openxr" and named[2]:
        raw_s = (first_utc - named[0]).total_seconds()
        quarter = round(raw_s / 900.0) * 900.0
        if abs(raw_s - quarter) <= 2.0:
            return (datetime.timedelta(seconds=-quarter),
                    "from the log's file name (local) against its first line (UTC)",
                    first_utc)
    try:
        stamp = calendar.timegm(first_utc.timetuple())
        local = datetime.datetime.fromtimestamp(stamp)
        return (local - first_utc.replace(microsecond=0),
                "from this machine's time zone; the file name could not give it",
                first_utc)
    except (OverflowError, OSError, ValueError):
        return (datetime.timedelta(0),
                "no time zone available; treated as UTC", first_utc)


def _at_time(at_text, t_line):
    """`at HH:MM:SS.mmm` is a time of day. Give it the day that puts it nearest
    the line that carries it: a run finished at most one 30 s window before the
    line that reports it, so the nearest day is the right one, midnight
    included."""
    tod = (int(at_text[0:2]) * 3600 + int(at_text[3:5]) * 60 +
           int(at_text[6:8]) + int(at_text[9:12]) / 1000.0)
    return tod + DAY * round((t_line - tod) / DAY)


def _op_stats(scan, op):
    return scan["ops"].setdefault(op, {
        "windows": 0, "runs": 0, "total_ms": 0.0, "slow_runs": 0,
        "slow_lines": 0, "max_ms": -1.0, "max_at": None, "max_ctx": ""})


def _unparsed(scan, msg):
    """A line that opens like one of ours but matches none of the formats: the
    C++ side changed its wording. Counted and shown, never dropped quietly --
    a parser that drifted from what it reads would look like a quiet flight."""
    scan["unparsed"]["count"] += 1
    if len(scan["unparsed"]["samples"]) < 2:
        scan["unparsed"]["samples"].append(msg[:120])


def _note_line(scan, msg, t):
    """File one line's message, stamped t, into the scan if it is one of ours."""
    if msg.startswith("periodic work: "):
        m = PERIODIC_SLOW_RE.match(msg)
        if m:
            at = _at_time(m.group("at"), t)
            ms = float(m.group("ms"))
            ctx = ("%s=%s" % (m.group("label"), m.group("ctx"))
                   if m.group("label") else "")
            st = _op_stats(scan, m.group("op"))
            st["slow_lines"] += 1
            if ms > st["max_ms"]:
                st["max_ms"], st["max_at"], st["max_ctx"] = ms, at, ctx
            scan["events"].append({"op": m.group("op"), "kind": "slow",
                                   "at": at, "ms": ms, "ctx": ctx,
                                   "src": scan["kind"]})
            return
        m = PERIODIC_WINDOW_RE.match(msg)
        if m:
            at = _at_time(m.group("at"), t)
            top = float(m.group("max"))
            ctx = ("%s=%s" % (m.group("label"), m.group("ctx"))
                   if m.group("label") else "")
            st = _op_stats(scan, m.group("op"))
            st["windows"] += 1
            st["runs"] += int(m.group("n"))
            st["total_ms"] += float(m.group("total"))
            st["slow_runs"] += int(m.group("slow"))
            if top > st["max_ms"]:
                st["max_ms"], st["max_at"], st["max_ctx"] = top, at, ctx
            scan["events"].append({"op": m.group("op"), "kind": "max",
                                   "at": at, "ms": top, "ctx": ctx,
                                   "src": scan["kind"]})
            # The summary is written by the run that closes the window, so its
            # line time is that run's end (infer_runs reads the cadence off it).
            scan["windows"].append({"op": m.group("op"), "close": t,
                                    "n": int(m.group("n")),
                                    "total": float(m.group("total"))})
            return
        _unparsed(scan, msg)
        return
    if msg.startswith("monitor: LONG FRAME -- "):
        m = LONG_FRAME_RE.match(msg)
        if m:
            seq = LONG_FRAME_SEQ_RE.search(msg)
            scan["frames"].append({"src": "gfx", "t": t,
                                   "ms": float(m.group("ms")),
                                   "seq": int(seq.group("seq")) if seq else 0})
        else:
            _unparsed(scan, msg)
        return
    if msg.startswith("native_long_cycle,"):
        m = LONG_CYCLE_RE.match(msg)
        if m:
            scan["frames"].append({"src": "rt", "t": t,
                                   "ms": float(m.group("ms")),
                                   "seq": int(m.group("seq"))})
        else:
            _unparsed(scan, msg)
        return
    if msg.startswith("native_long_cycle_summary,"):
        m = LONG_CYCLE_SUMMARY_RE.match(msg)
        if m:
            scan["cycle_summary"] = (int(m.group("count")),
                                     int(m.group("logged")))
        return
    if msg.startswith("monitor: ") and "dropped or long frames were logged" in msg:
        m = LONG_FRAME_CAP_RE.match(msg)
        if m:
            scan["frame_cap"] = (int(m.group("logged")), int(m.group("cap")))


def scan_flight_log(text, kind, base_days=0, start_tod=None,
                    to_local=datetime.timedelta(0)):
    """One pass over a log: its `periodic work:` lines and its long frames on
    the common clock, plus the first and last stamped line.

    kind "gfx": a local time-of-day prefix. base_days is the flight's first day
    (days since 2000-01-01) and start_tod the time of day the file name says the
    log opened; the prefix has no date, so the day rolls over whenever the clock
    goes back by more than half a day (a line stamped just before a midnight
    already crossed, which threads can write out of order, keeps the old day).
    kind "rt": a full UTC prefix, turned to local by to_local."""
    scan = {"kind": kind, "stamped": 0, "first": None, "last": None,
            "ops": {}, "events": [], "frames": [], "windows": [],
            "cycle_summary": None, "frame_cap": None,
            "unparsed": {"count": 0, "samples": []}}
    day = 0
    prev = start_tod
    for raw in text.splitlines():
        if kind == "gfx":
            m = GFX_STAMP_RE.match(raw)
            if not m:
                continue
            tod = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 +
                   int(m.group(3)) + int(m.group(4)) / 1000.0)
            line_day = day
            if prev is None:
                prev = tod
            elif tod < prev - DAY / 2:
                day += 1
                line_day = day
                prev = tod
            elif tod > prev + DAY / 2:
                line_day = day - 1
            else:
                prev = tod
            t = (base_days + line_day) * DAY + tod
        else:
            m = RT_STAMP_RE.match(raw)
            if not m:
                continue
            utc = _utc_of(m)
            if utc is None:
                continue
            t = (utc + to_local - EPOCH).total_seconds()
        scan["stamped"] += 1
        if scan["first"] is None or t < scan["first"]:
            scan["first"] = t
        if scan["last"] is None or t > scan["last"]:
            scan["last"] = t
        _note_line(scan, raw[m.end():], t)
    return scan


def scan_flight(gfx_path, gfx_text, rt_path=None, rt_text=None):
    """Both logs scanned onto the one local clock. Returns (graphics scan,
    runtime scan or None, clock), clock naming how the runtime's UTC became
    local. The graphics log's date is the one its file name carries; without a
    usable name it is the runtime log's first local day."""
    to_local, how, first_utc = datetime.timedelta(0), None, None
    if rt_text is not None:
        to_local, how, first_utc = runtime_to_local(rt_path, rt_text)
    named = log_name_time(gfx_path) if gfx_path else None
    first_day = start_tod = None
    if named:
        first_day = named[0].date()
        start_tod = (named[0].hour * 3600 + named[0].minute * 60 +
                     named[0].second + named[0].microsecond / 1e6)
    elif first_utc is not None:
        first_day = (first_utc + to_local).date()
    base_days = (first_day - EPOCH.date()).days if first_day else 0
    gscan = scan_flight_log(gfx_text, "gfx", base_days, start_tod)
    rscan = None
    if rt_text is not None:
        rscan = scan_flight_log(rt_text, "rt", to_local=to_local)
    clock = {"to_local": to_local if rt_text is not None else None,
             "how": how}
    return gscan, rscan, clock


def infer_runs(scan, window_s):
    """Runs nobody logged, put back where a fixed cadence says they ran.

    Only a window's slowest run and at most one SLOW line per 10 s carry a
    time, so an operation that runs every 4 s and is slow every time (the
    journal re-glob) has a logged time for a third of its runs. A summary is
    written by the run that closes its window and the window before it closed
    on the run just ahead of this one's first, so an operation on a timer that
    ran n times between two closes ran every (close - previous close) / n.
    That grid is believed only when at least two logged times inside the window
    (its SLOW lines, its slowest run) sit on it to within tol, a frame or so,
    and only when its spacing is at least four windows and a quarter second: a
    window around a run every 100 ms covers everything and proves nothing.

    Returns (events of kind "est", one per grid run no logged time stands for,
    each with its window's mean ms; notes {op: windows accepted, runs inferred,
    median period in s})."""
    by_op = {}
    for w in scan["windows"]:
        by_op.setdefault(w["op"], []).append(w)
    est, notes = [], {}
    for op, wins in by_op.items():
        wins.sort(key=lambda w: w["close"])
        known = sorted(e["at"] for e in scan["events"] if e["op"] == op)
        periods, added = [], 0
        for prev, w in zip(wins, wins[1:]):
            n = w["n"]
            if n < 2:
                continue
            period = (w["close"] - prev["close"]) / n
            if period < max(4.0 * window_s, 0.25):
                continue
            tol = min(0.05, period / 4.0)
            grid = [prev["close"] + i * period for i in range(1, n + 1)]
            # Distinct instants: a slowest run that also has a SLOW line is one
            # run, named twice, and confirms nothing more than once.
            inside = sorted({round(k, 3) for k in known
                             if prev["close"] < k <= w["close"] + tol})
            if len(inside) < 2 or any(min(abs(k - g) for g in grid) > tol
                                      for k in inside):
                continue
            periods.append(period)
            for g in grid:
                if all(abs(k - g) > tol for k in known):
                    est.append({"op": op, "kind": "est", "at": g,
                                "ms": w["total"] / n, "ctx": "",
                                "src": scan["kind"]})
                    added += 1
        if periods:
            notes[op] = {"windows": len(periods), "runs": added,
                         "period": statistics.median(periods)}
    return est, notes


def pair_runtime_log(gfx_path, native_dirs):
    """The runtime log that goes with a graphics log: the edvr_openxr_*.log
    that opened nearest it, within PAIR_TOLERANCE_S (both are opened by the one
    game process, seconds apart). Returns (path, None), or (None, why)."""
    named = log_name_time(gfx_path)
    if not named:
        return None, ("the graphics log's name carries no timestamp to pair "
                      "by; name the runtime log with --runtime-file")
    best = None
    for directory in native_dirs:
        for _, _, path in find_logs(directory, "openxr"):
            other = log_name_time(path)
            if not other:
                continue
            gap = abs((other[0] - named[0]).total_seconds())
            if best is None or gap < best[0]:
                best = (gap, path)
    if best is None:
        return None, "no edvr_openxr_*.log in %s" % ", ".join(native_dirs)
    if best[0] > PAIR_TOLERANCE_S:
        return None, ("the nearest edvr_openxr_*.log (%s) opened %.0f min from "
                      "the graphics log, more than %.0f min"
                      % (os.path.basename(best[1]), best[0] / 60.0,
                         PAIR_TOLERANCE_S / 60.0))
    return best[1], None


def nearest_event(frame, events):
    """The event closest to a long frame [t - ms, t]: gap 0 when the event's
    end lies inside it, else the distance to the nearer edge. offset_ms is the
    event's end minus the frame's end (negative: it finished first). None when
    there are no events."""
    start = frame["t"] - frame["ms"] / 1000.0
    end = frame["t"]
    best = None
    for ev in events:
        gap = max(0.0, start - ev["at"], ev["at"] - end)
        key = (gap, abs(ev["at"] - end))
        if best is None or key < best[0]:
            best = (key, ev)
    if best is None:
        return None
    return {"event": best[1], "gap_s": best[0][0],
            "offset_ms": (best[1]["at"] - end) * 1000.0}


def analyse_periodic(events, frames, window_s, span_s):
    """Lay the long frames against the periodic events.

    A long frame [t - ms, t] coincides with an operation when one of its
    events' end times lies within window_s of that interval. A window summary
    and a SLOW line that name the same run (same end time) are one event.
    chance is the matches independence would give: each event lands in a given
    frame's padded interval with probability (ms + 2 window) / span, so the
    expected matches are the sum over frames of min(1, events * that).

    Returns events (op -> sorted distinct events), hits and chance (both keyed
    (op, source)), counts and touched (per source: long frames, and those with
    any match), matches (each long frame with a match and, per operation that
    matched, its largest event there; the largest event first) and top (per
    source: the ten longest, each with its nearest event and whether it is
    within the window)."""
    distinct = {}
    for ev in sorted(events, key=lambda e: (e["at"], e["kind"] != "slow")):
        distinct.setdefault((ev["op"], round(ev["at"], 3)), ev)
    by_op = {}
    for (op, _), ev in distinct.items():
        by_op.setdefault(op, []).append(ev)
    for lst in by_op.values():
        lst.sort(key=lambda e: e["at"])
    times = {op: [e["at"] for e in lst] for op, lst in by_op.items()}
    hits, chance = {}, {}
    counts = {"gfx": 0, "rt": 0}
    touched = {"gfx": 0, "rt": 0}
    matches = []
    for f in frames:
        lo = f["t"] - f["ms"] / 1000.0 - window_s
        hi = f["t"] + window_s
        counts[f["src"]] += 1
        found = []
        for op, ts in times.items():
            i = bisect.bisect_left(ts, lo)
            biggest = None
            while i < len(ts) and ts[i] <= hi:
                if biggest is None or by_op[op][i]["ms"] > biggest["ms"]:
                    biggest = by_op[op][i]
                i += 1
            if biggest is not None:
                hits[(op, f["src"])] = hits.get((op, f["src"]), 0) + 1
                found.append(biggest)
            if span_s > 0:
                chance[(op, f["src"])] = chance.get((op, f["src"]), 0.0) + \
                    min(1.0, len(ts) * (hi - lo) / span_s)
        if found:
            touched[f["src"]] += 1
            matches.append({"frame": f, "events": found})
    matches.sort(key=lambda m: (-max(e["ms"] for e in m["events"]),
                                m["frame"]["t"]))
    flat = [e for lst in by_op.values() for e in lst]
    top = {"gfx": [], "rt": []}
    for src in top:
        longest = sorted((f for f in frames if f["src"] == src),
                         key=lambda f: (-f["ms"], f["t"]))[:10]
        for f in longest:
            near = nearest_event(f, flat)
            top[src].append({"frame": f, "near": near,
                             "hit": bool(near and near["gap_s"] <= window_s)})
    return {"events": by_op, "hits": hits, "chance": chance,
            "counts": counts, "touched": touched, "matches": matches,
            "top": top}


def clock_check(frames):
    """Pair each LONG FRAME line with the native_long_cycle line of the same
    runtime sequence and return (pairs, median of graphics time minus runtime
    time in ms, None without pairs). The two describe one hitch a few ms apart;
    a wrong UTC offset would show as minutes or hours."""
    cycles = [f for f in frames if f["src"] == "rt" and f["seq"]]
    deltas = []
    for f in frames:
        if f["src"] != "gfx" or not f["seq"]:
            continue
        near = [c for c in cycles if abs(c["seq"] - f["seq"]) <= 2]
        if near:
            c = min(near, key=lambda c: abs(c["t"] - f["t"]))
            deltas.append((f["t"] - c["t"]) * 1000.0)
    return len(deltas), (statistics.median(deltas) if deltas else None)


def _kind_label(ev, long=False):
    """What sort of time an event carries: a SLOW line, a window's slowest run,
    or a run placed by cadence (infer_runs)."""
    if ev["kind"] == "slow":
        return "SLOW"
    if ev["kind"] == "est":
        return "inferred run" if long else "est"
    return "window max" if long else "max"


def fmt_clock(t, day0):
    """HH:MM:SS.mmm of a common-clock time, with the day if it is not the
    flight's first (day0 = that day's index)."""
    ms = int(round(t * 1000.0))
    day, in_day = divmod(ms, 86400000)
    h, rest = divmod(in_day, 3600000)
    m, rest = divmod(rest, 60000)
    s, milli = divmod(rest, 1000)
    text = "%02d:%02d:%02d.%03d" % (h, m, s, milli)
    if day != day0:
        text += " %+dd" % (day - day0)
    return text


def _fmt_zone(delta):
    minutes = int(round(delta.total_seconds() / 60.0))
    return "UTC%s%02d:%02d" % ("+" if minutes >= 0 else "-",
                               abs(minutes) // 60, abs(minutes) % 60)


def print_periodic_report(gfx_path, gfx_text, gfx_ver, want, args, native_dirs):
    """The --tally periodic report. Returns the process exit code."""
    window_s = args.window_ms / 1000.0

    # The runtime log that goes with this graphics log, and whether it is the
    # right build: its lines are as much evidence as the graphics log's.
    rt_path = rt_why = None
    if args.runtime_file:
        rt_path = os.path.abspath(args.runtime_file)
        if not os.path.isfile(rt_path):
            print("[edvr] no such runtime log: %s" % rt_path)
            return 1
    else:
        rt_path, rt_why = pair_runtime_log(gfx_path, native_dirs)
    rt_text = None
    if rt_path:
        rt_text = read_text(rt_path)
        print("[edvr] runtime log: %s  (%d lines, %.1f KB)"
              % (rt_path, rt_text.count("\n") + 1, len(rt_text) / 1024.0))
        rt_line, rt_ver, _ = version_line(rt_text)
        if rt_line:
            print("[edvr] %s" % rt_line)
        else:
            print("[edvr] WARNING: the runtime log has no version line -- it "
                  "may be truncated, or not an EDVR log.")
        if want is not None:
            if version_matches(rt_ver, want):
                print("[edvr] runtime build matches: %s" % want)
            else:
                print("[edvr] BUILD MISMATCH (runtime log)\n"
                      "       log says   %s\n"
                      "       expected   %s\n"
                      "       This flight is not evidence about that build. "
                      "Reinstall and fly again."
                      % (rt_ver or "(nothing)", want))
                return 2
        elif gfx_ver and rt_ver and not version_matches(gfx_ver, rt_ver):
            print("[edvr] WARNING: the runtime log is from build %s and the "
                  "graphics log from %s; they are not one flight of one build."
                  % (rt_ver, gfx_ver))
    else:
        print("[edvr] runtime log: NONE FOUND -- %s.\n"
              "       native_long_cycle and frame_cycle_report lines are "
              "unavailable; only the graphics log is read." % rt_why)

    gscan, rscan, clock = scan_flight(gfx_path, gfx_text, rt_path, rt_text)
    if gscan["stamped"] == 0:
        print("[edvr] the graphics log has no [HH:MM:SS.mmm] lines; "
              "--tally periodic reads edvr_gfx_*.log (a runtime log goes to "
              "--runtime-file).")
        return 1
    if rscan is not None and rscan["stamped"] == 0:
        print("[edvr] WARNING: the runtime log has no 'YYYY-MM-DD HH:MM:SS.mmm "
              "UTC pid= tid=' lines; it is not used.")
        rscan = None

    day0 = int(gscan["first"] // DAY)

    def clk(t):
        return fmt_clock(t, day0)

    def span_text(sc):
        return "%s .. %s (%.1f s)" % (clk(sc["first"]), clk(sc["last"]),
                                      sc["last"] - sc["first"])

    print("[edvr] periodic work vs long frames; window +/-%g ms" % args.window_ms)
    print("[edvr] clocks (every time below is LOCAL):")
    print("       graphics log  local time of day, [HH:MM:SS.mmm] prefix; no "
          "date, so the date is the file name's and midnight rolls it over")
    if rscan is not None:
        print("       runtime log   UTC prefix, converted at %s (%s)"
              % (_fmt_zone(clock["to_local"]), clock["how"]))
    print("       `at` in a `periodic work:` line: local time of day, in both "
          "logs")
    print("[edvr] graphics log spans %s" % span_text(gscan))
    if rscan is not None:
        print("[edvr] runtime log spans  %s" % span_text(rscan))
        if rscan["last"] < gscan["first"] or rscan["first"] > gscan["last"]:
            print("[edvr] WARNING: the two logs' time spans do not overlap: "
                  "the clock conversion is wrong, or these are not one flight.")

    scans = [gscan] + ([rscan] if rscan is not None else [])
    for sc, where in ((gscan, "graphics"), (rscan, "runtime")):
        if sc is not None and sc["unparsed"]["count"]:
            print("[edvr] WARNING: %d line(s) in the %s log open like a "
                  "`periodic work:`, LONG FRAME or native_long_cycle line but "
                  "match none of the formats this reads; the C++ wording has "
                  "probably changed, and what follows leaves them out. First: %s"
                  % (sc["unparsed"]["count"], where,
                     " | ".join(sc["unparsed"]["samples"])))

    # 1. What the timing saw, per operation.
    print("[edvr] 1. periodic work, per operation")
    length = gscan["last"] - gscan["first"]
    if not gscan["ops"]:
        if length >= 60.0:
            print("[edvr] NO `periodic work:` lines in the graphics log (it "
                  "spans %.0f s). The timing was never wired into this build, "
                  "or the build is wrong; check the version line above and "
                  "rerun with --expect-build HEAD." % length)
        else:
            print("[edvr] no `periodic work:` lines in the graphics log, which "
                  "spans only %.0f s; the first 30 s summary may not be "
                  "written yet." % length)
    if rscan is not None and not rscan["ops"]:
        print("[edvr] no `periodic work:` lines in the runtime log (its build "
              "predates the timing, or it spans under 30 s).")
    order = {op: i for i, op in enumerate(KNOWN_OPS["gfx"] + KNOWN_OPS["rt"])}
    rows = sorted(((op, sc["kind"], st) for sc in scans
                   for op, st in sc["ops"].items()),
                  key=lambda r: (order.get(r[0], len(order)), r[0]))
    if rows:
        print("%-19s %-3s %7s %8s %10s %8s %8s %9s %10s  %-12s %s"
              % ("operation", "log", "windows", "runs", "total ms", "mean ms",
                 "max ms", "slow runs", "SLOW lines", "max at", "at the max"))
        for op, src, st in rows:
            mean = ("%.3f" % (st["total_ms"] / st["runs"])) if st["runs"] else "-"
            print("%-19s %-3s %7d %8d %10.3f %8s %8.3f %9d %10d  %-12s %s"
                  % (op, src, st["windows"], st["runs"], st["total_ms"], mean,
                     st["max_ms"], st["slow_runs"], st["slow_lines"],
                     clk(st["max_at"]), st["max_ctx"]))
    for sc, where in ((gscan, "graphics"), (rscan, "runtime")):
        if sc is None or not sc["ops"]:
            continue
        absent = [op for op in KNOWN_OPS[sc["kind"]] if op not in sc["ops"]]
        if absent:
            print("[edvr] not seen in the %s log: %s (it never ran this flight, "
                  "or this build does not time it; absence does not show it was "
                  "fast)" % (where, ", ".join(absent)))

    # 2. The long frames, and which operation each coincides with.
    frames = list(gscan["frames"]) + (list(rscan["frames"]) if rscan else [])
    print("[edvr] 2. long frames")
    cap = gscan["frame_cap"]
    if cap and cap[0] >= cap[1]:
        note = (" (the DLL's session cap of %d lines was reached, so later long "
                "frames are not in the log)" % cap[1])
    else:
        note = (" (the DLL rate-limits these lines: a sample of the long "
                "frames, not a count)")
    print("       LONG FRAME lines, graphics log: %d%s"
          % (len(gscan["frames"]), note))
    if rscan is not None:
        if rscan["cycle_summary"]:
            note = (" (the runtime counted %d over twice its predicted period "
                    "and logged %d)" % rscan["cycle_summary"])
        else:
            note = (" (no native_long_cycle_summary: the runtime did not close "
                    "its trace, so the true count is unknown)")
        print("       native_long_cycle lines, runtime log: %d%s"
              % (len(rscan["frames"]), note))
        pairs, median = clock_check(frames)
        if median is None:
            print("       clock check: no LONG FRAME line shares a runtime "
                  "sequence with a native_long_cycle line, so the two clocks "
                  "could not be checked against each other")
        elif abs(median) > 300000.0:
            print("       clock check: WARNING %d LONG FRAME line(s) share a "
                  "runtime sequence with a native_long_cycle line, %.0f s "
                  "apart at the median: the UTC offset is wrong; nothing "
                  "across the two logs can be trusted" % (pairs, median / 1000.0))
        else:
            print("       clock check: %d LONG FRAME line(s) share a runtime "
                  "sequence with a native_long_cycle line, %+.0f ms apart at "
                  "the median" % (pairs, median))
    if not frames:
        print("[edvr] no long frames in either log: nothing to lay against the "
              "periodic work.")
        return 0
    if args.infer_runs:
        notes = {}
        for sc in scans:
            more, found = infer_runs(sc, window_s)
            sc["events"] = sc["events"] + more
            notes.update(found)
        for op in sorted(notes):
            print("       inferred runs: %s, %d run(s) in %d window(s), every "
                  "%.2f s (each window's schedule has two or more logged times "
                  "on it)" % (op, notes[op]["runs"], notes[op]["windows"],
                              notes[op]["period"]))
        print("       an inferred run (est) is where the cadence says it ran, "
              "not a logged time" if notes else
              "       inferred runs: no operation had a schedule that two "
              "logged times could confirm")
    spans = [sc["last"] - sc["first"] for sc in scans]
    res = analyse_periodic([e for sc in scans for e in sc["events"]], frames,
                           window_s, max(spans))
    if not res["events"]:
        print("[edvr] no periodic events to lay the long frames against.")
        return 0
    have = [s for s in ("gfx", "rt") if res["counts"][s]]
    print("       long frames within +/-%g ms of any periodic event: %s"
          % (args.window_ms, "; ".join(
              "%s %d of %d" % (SRC_LABEL[s], res["touched"][s], res["counts"][s])
              for s in have)))
    print("       a frame ends at its line's time and lasts its ms; an event "
          "coincides with it when the event ended inside the frame or within "
          "the window of it")
    print("       chance = the matches independence alone would give (events x "
          "frame length over the flight)")
    print("       known/runs = the operation's runs that have a logged time (a "
          "SLOW line, at most one per 10 s, or a window's slowest run); a run "
          "nobody logged cannot coincide with anything here%s"
          % ("" if args.infer_runs else " (--infer-runs adds the ones a "
             "fixed cadence places)"))
    ranked = sorted(res["events"], key=lambda o: (
        -sum(res["hits"].get((o, s), 0) for s in have),
        order.get(o, len(order)), o))
    total_runs = {}
    for op, _, st in rows:
        total_runs[op] = total_runs.get(op, 0) + st["runs"]

    def runs_text(op):
        evs = res["events"][op]
        est = sum(1 for e in evs if e["kind"] == "est")
        return "%d%s/%s" % (len(evs) - est, ("+%d" % est) if est else "",
                            total_runs.get(op) or "?")

    cw = 16 if args.infer_runs else 10
    head = "%-19s %*s" % ("operation", cw,
                          "known+est/runs" if args.infer_runs else "known/runs")
    for s in have:
        head += "  %*s %7s" % (len(SRC_LABEL[s]) + 5, SRC_LABEL[s] + " hits",
                               "chance")
    print(head)
    for op in ranked:
        line = "%-19s %*s" % (op, cw, runs_text(op))
        for s in have:
            line += "  %*s %7.2f" % (
                len(SRC_LABEL[s]) + 5,
                "%d/%d" % (res["hits"].get((op, s), 0), res["counts"][s]),
                res["chance"].get((op, s), 0.0))
        print(line)

    # 3. Every long frame with an event beside it: the counts above say how
    # many, this says which, and an event's own ms says whether it could matter
    # (a 0.01 ms run beside a 190 ms frame is a coincidence, not a cause).
    matches = res["matches"]
    shown = matches[:MATCHED_ROWS]
    print("[edvr] 3. long frames with a periodic event beside them, largest "
          "event first (%d of %d long frame(s))" % (len(matches), len(frames)))
    if shown:
        print("       events: operation, SLOW run or window max, its ms, then "
              "its end minus the frame's end in ms")
        print("       %-17s %-15s %9s  %s"
              % ("source", "frame ends", "ms", "events beside it"))
    for m in shown:
        f = m["frame"]
        beside = "; ".join("%s %s %.3f (%+.0f)" % (
            e["op"], _kind_label(e), e["ms"], (e["at"] - f["t"]) * 1000.0)
            for e in sorted(m["events"], key=lambda e: -e["ms"]))
        print("       %-17s %-15s %9.1f  %s"
              % (SRC_LABEL[f["src"]], clk(f["t"]), f["ms"], beside))
    if len(matches) > len(shown):
        print("       ... and %d more, each with a smaller event beside it"
              % (len(matches) - len(shown)))

    # 4. The ten longest of each, with what was nearest.
    for s in have:
        tops = res["top"][s]
        print("[edvr] 4. the %d longest %s line(s), with the nearest periodic "
              "event" % (len(tops), SRC_LABEL[s]))
        print("       offset = the event's end minus the frame's end (negative: "
              "the event finished first); within = inside the window")
        print("       %-3s %-15s %9s  %-64s %10s  %s"
              % ("#", "frame ends", "ms", "nearest event", "offset ms",
                 "within"))
        for i, row in enumerate(tops, 1):
            f, near = row["frame"], row["near"]
            what = "-"
            offset = "-"
            if near:
                ev = near["event"]
                what = "%s %s %.3f ms, ended %s%s" % (
                    ev["op"], _kind_label(ev, True), ev["ms"], clk(ev["at"]),
                    (" " + ev["ctx"]) if ev["ctx"] else "")
                offset = "%+.1f" % near["offset_ms"]
            verdict = "no"
            if row["hit"]:
                verdict = "yes, inside the frame" if near["gap_s"] == 0.0 \
                    else "yes"
            print("       %-3d %-15s %9.1f  %-64s %10s  %s"
                  % (i, clk(f["t"]), f["ms"], what, offset, verdict))
    return 0


def _products_under(root):
    found = []
    products = os.path.join(root, "Products")
    if not os.path.isdir(products):
        return found
    for name in sorted(os.listdir(products)):
        leaf = os.path.join(products, name)
        if os.path.isfile(os.path.join(leaf, GAME_EXE)):
            found.append(os.path.normpath(leaf))
    return found


def resolve_target(spec):
    if spec not in ("steam", "frontier"):
        p = os.path.abspath(spec)
        if os.path.isdir(p):
            return p
        raise SystemExit("[edvr] no such directory: %s" % p)
    # Deliberately shares no code with install_edvr.py's fuller search:
    # this one only has to find a log, and a wrong guess here costs a
    # message rather than an overwritten DLL.
    if spec == "frontier":
        local = os.environ.get("LOCALAPPDATA", "")
        found = _products_under(os.path.join(local, "Frontier_Developments")) \
            if local else []
    else:
        found = []
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                r"Software\Valve\Steam") as k:
                steam = winreg.QueryValueEx(k, "SteamPath")[0]
        except (ImportError, OSError):
            steam = r"C:\Steam"
        found = _products_under(os.path.join(steam, "steamapps", "common",
                                             "Elite Dangerous"))
    if not found:
        raise SystemExit("[edvr] no %s install found; pass a path to "
                         "--target." % spec)
    if len(found) > 1:
        raise SystemExit("[edvr] %d installs found; name one:\n       %s"
                         % (len(found), "\n       ".join(found)))
    return found[0]


# ---------------------------------------------------------------------------------------------------------------------------------------
# --vscreen-fit: fix.vscreen_res_width = auto, fitted to what each eye shows (design doc section 82, the "vscreen auto-fit" entry).
# src/common/vscreen_fit.h writes the `vScreen resolution: auto = ...` rule line and the `vscreen footprint 30s:` line; the armed line
# is src/d3d11/vscreen_footprint.cpp's. tools\vscreen_fit_test holds the formatters to tools\vscreen_fit_fixture.log, the file this
# reader's own self-test reads. The constants below are that header's (kMultiplier, kFloorWidth, kLegacyMultiplier, the calibration
# point); the rig pins the header, and self_test_vscreen_fit pins this copy of it to the header's text.
# ---------------------------------------------------------------------------------------------------------------------------------------
VSCREEN_M = 1.0                 # kMultiplier
VSCREEN_FLOOR = 2880            # kFloorWidth
VSCREEN_LEGACY_M = 1.25         # kLegacyMultiplier
VSCREEN_SEED = (3504.0, 0.7, 4032.0)   # kSeedWidthPx, kSeedDistance, kSeedEyeWidthPx: Sean's calibration point
VSCREEN_SHAPE = 16.0 / 9.0
VSCREEN_STABLE = 0.02           # the screen's width may move this much (relative) between windows, and inside one
VSCREEN_SHAPE_WARN = 0.03
VSCREEN_SHAPE_STOP = 0.15
VSCREEN_LAW = 0.02              # footprint at distance 1 may differ this much between windows at different distances
VSCREEN_CALIBRATION = 0.03      # the measured footprint may differ this much from the calibration point before m is re-derived

VSCREEN_RULE_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vScreen resolution: auto = (?P<w>\d+) wide: (?P<rest>.*)$")
VSCREEN_APPLY_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vScreen resolution: (?P<sw>\d+)x(?P<sh>\d+) -> (?P<w>\d+)x(?P<h>\d+) at (?P<sites>\d+) site")
VSCREEN_EXPLICIT_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vScreen resolution: explicit (?P<w>\d+) wide")
VSCREEN_FOOT_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vscreen footprint 30s: (?P<rest>.*)$")
VSCREEN_ARMED_RE = re.compile(r"^(?:\[(?P<ts>[0-9:.]+)\] )?vscreen footprint: (?P<what>armed|not armed|STOOD DOWN)")


def _vround16(value):
    return int(value / 16.0 + 0.5) * 16


def _vnum(text):
    """A token's number (a leading float), else None: `3504`, `0.8690`, `-` -> 3504.0, 0.869, None."""
    m = re.match(r"^-?\d+(?:\.\d+)?", text or "")
    return float(m.group(0)) if m else None


def parse_vscreen_fit(text):
    """The log's vscreen auto-fit lines: {rule: {ts, width, kv, prose} (the launch's first auto line) or None, explicit: width or None,
    no_eye: bool (auto with no eye width on record), off: bool, applied: {ts, src, w, h, sites} or None, armed: [(ts, what)],
    windows: [{ts, kv}]}. A line cut short or garbled is skipped, never fatal."""
    f = {"rule": None, "explicit": None, "no_eye": False, "off": False, "applied": None, "armed": [], "windows": []}
    for raw in text.splitlines():
        try:
            m = VSCREEN_RULE_RE.match(raw)
            if m:
                head, _, prose = m.group("rest").partition(" -- ")
                if f["rule"] is None:
                    f["rule"] = {"ts": m.group("ts") or "", "width": int(m.group("w")), "kv": _ckv(head), "prose": prose}
                continue
            m = VSCREEN_APPLY_RE.match(raw)
            if m:
                if f["applied"] is None:
                    f["applied"] = {"ts": m.group("ts") or "", "src": (int(m.group("sw")), int(m.group("sh"))),
                                    "w": int(m.group("w")), "h": int(m.group("h")), "sites": int(m.group("sites"))}
                continue
            m = VSCREEN_EXPLICIT_RE.match(raw)
            if m:
                f["explicit"] = int(m.group("w"))
                continue
            if "vScreen resolution: auto, but no per-eye render width is on record" in raw:
                f["no_eye"] = True
                continue
            if "vScreen resolution: off (set to " in raw:
                f["off"] = True
                continue
            m = VSCREEN_ARMED_RE.match(raw)
            if m:
                f["armed"].append((m.group("ts") or "", m.group("what")))
                continue
            m = VSCREEN_FOOT_RE.match(raw)
            if m:
                f["windows"].append({"ts": m.group("ts") or "", "kv": _ckv(m.group("rest"))})
        except (ValueError, TypeError):
            continue
    return f


def vscreen_fit_windows(f):
    """Each window's tokens as numbers: [{window, samples, on_foot, other, skipped, late, draws, why, distance, applied, eye (w, h) or None,
    fp, frac, lo, hi, h, shape, other_fp, at1, frac1, session_n, session_frac1, persisted, fit, legacy}]."""
    out = []
    for w in f["windows"]:
        kv = w["kv"]
        eye = re.match(r"^(\d+)x(\d+)$", kv.get("eye", ""))
        rng = re.match(r"^(-?\d+(?:\.\d+)?)\.\.(-?\d+(?:\.\d+)?)$", kv.get("range", ""))
        out.append({
            "ts": w["ts"], "window": _cint(kv.get("window")), "samples": _cint(kv.get("samples")) or 0, "on_foot": _cint(kv.get("on-foot")) or 0,
            "other": _cint(kv.get("other")) or 0, "skipped": _cint(kv.get("skipped")) or 0, "late": _cint(kv.get("late")) or 0,
            "draws": _cint(kv.get("draws")) or 0, "why": kv.get("why", ""), "distance": _vnum(kv.get("distance")), "applied": _vnum(kv.get("applied")),
            "eye": (int(eye.group(1)), int(eye.group(2))) if eye else None, "fp": _vnum(kv.get("fp")), "frac": _vnum(kv.get("frac")),
            "lo": float(rng.group(1)) if rng else None, "hi": float(rng.group(2)) if rng else None, "h": _vnum(kv.get("h")),
            "shape": _vnum(kv.get("shape")), "other_fp": _vnum(kv.get("other-fp")), "at1": _vnum(kv.get("at1")), "frac1": _vnum(kv.get("frac1")),
            "session_n": _cint(kv.get("session-n")) or 0, "session_frac1": _vnum(kv.get("session-frac1")), "persisted": _vnum(kv.get("persisted")),
            "fit": _vnum(kv.get("fit")), "legacy": _vnum(kv.get("legacy"))})
    return out


def vscreen_fit_verdict(f):
    """The verdict on one flight: [(tag, status, text)], status PASS, WARN, STOP or n/a (what the log cannot say). The tags are the
    questions the flight plan asks: RULE (did auto choose by the rule it names, and was that what was applied), INSTRUMENT (did the
    footprint instrument run, see the composite and read its sources), ON FOOT (was the screen measured on foot), STABLE, SHAPE (the
    corner arithmetic and the eye size agree that the screen is 16:9), DISTANCE LAW (A varies as 1/d), CALIBRATION (does m = 1.0 give
    Sean's 3504), STORED (what the next launch will fit)."""
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    wins = vscreen_fit_windows(f)
    rule = f["rule"]
    applied = f["applied"]

    # ---- RULE ----
    if f["explicit"]:
        add("RULE", "n/a", "an explicit width (%d), used exactly: auto's rule is not in play" % f["explicit"])
    elif f["no_eye"]:
        add("RULE", "n/a", "auto with no per-eye width on record yet: the panel stayed at the game's own 1920x1080 (a fresh install's first launch)")
    elif rule is None:
        add("RULE", "n/a", "no `vScreen resolution: auto =` line: a log that predates the rule, or a session that never reached the panel patch")
    else:
        kv = rule["kv"]
        width = rule["width"]
        name = kv.get("rule")
        eye = _vnum(kv.get("eye"))
        text = ""
        status = "PASS"
        if name == "fitted":
            fp, m, floor, cap = _vnum(kv.get("footprint")), _vnum(kv.get("m")), _vnum(kv.get("floor")), _vnum(kv.get("cap"))
            if None in (fp, m, floor, cap):
                status, text = "WARN", "rule=fitted but its footprint, m, floor or cap token is missing: the width cannot be recomputed"
            else:
                lo = min(floor, cap)
                want = _vround16(min(max(m * fp, lo), cap))
                nudged = kv.get("nudged") == "yes"
                if width == want or (nudged and 0 < abs(width - want) <= 16 * 8):
                    text = ("FITTED to %d wide from a %s footprint of %.0f px at fix.panel_distance %s on a %d px eye (m=%.2f, floor %d, cap %d, clamp %s%s); "
                            "legacy would have been %s"
                            % (width, kv.get("source", "?"), fp, kv.get("distance", "?"), eye or 0, m, floor, cap, kv.get("clamp", "?"),
                               ", nudged off another target's size" if nudged else "", kv.get("legacy", "?")))
                else:
                    status, text = "STOP", ("the line says %d wide but its own tokens (footprint %.0f, m %.2f, floor %d, cap %d) give %d" % (width, fp, m, floor, cap, want))
        elif name == "legacy":
            want = _vround16((eye or 0) * VSCREEN_LEGACY_M) if eye else None
            why = rule["prose"].partition("because the world route will not run:")[2].partition(". Without the route")[0].strip()
            if want is not None and width != min(max(want, 640), 8192):
                status, text = "STOP", "the legacy width %d is not 125%% of the %d px eye (%d)" % (width, eye, want)
            elif not why:
                status, text = "WARN", "legacy %d wide, but the line does not say which route condition failed" % width
            else:
                text = "LEGACY %d wide (125%% of the %d px eye): the world route will not run: %s" % (width, eye or 0, why)
        else:
            status, text = "WARN", "the auto line has no rule= token (%r): an older build's line" % kv.get("rule")
        if status != "STOP" and applied is not None and applied["w"] != width:
            status, text = "STOP", "%s -- but the panel patch applied %dx%d, not %d wide" % (text, applied["w"], applied["h"], width)
        elif status == "PASS" and applied is None:
            text += " (no `vScreen resolution: ... ->` apply line in this log: the patch did not write, or the line was cut)"
        elif status == "PASS":
            text += "; applied %dx%d at %d site(s)" % (applied["w"], applied["h"], applied["sites"])
        add("RULE", status, text)

    # ---- INSTRUMENT ----
    armed = [a for a in f["armed"] if a[1] == "armed"]
    notarmed = [a for a in f["armed"] if a[1] != "armed"]
    if f["explicit"] and not wins:
        add("INSTRUMENT", "n/a", "not armed by design: an explicit width has nothing to fit%s" % (" (the log says so)" if notarmed else ""))
    elif not f["armed"] and not wins:
        add("INSTRUMENT", "STOP", "no `vscreen footprint` line at all, not even the arming line: the instrument never ran (a build that predates it, the flat "
            "profile, or a session that never reached its first frame boundary)")
    elif notarmed and not armed and not wins:
        add("INSTRUMENT", "n/a", "not armed: %s" % notarmed[0][1])
    elif not wins:
        add("INSTRUMENT", "WARN", "armed, but no 30 s window closed: the session was shorter than 30 s")
    else:
        total = sum(w["samples"] for w in wins)
        foot = sum(w["on_foot"] for w in wins)
        draws = sum(w["draws"] for w in wins)
        skipped = sum(w["skipped"] for w in wins)
        late = sum(w["late"] for w in wins)
        why = {}
        for w in wins:
            for part in w["why"].split(","):
                key, _, n = part.partition(":")
                if key and n.isdigit():
                    why[key] = why.get(key, 0) + int(n)
        base = "%d window(s): %d sample(s) (%d on foot, %d other), %d composite draw(s) seen, %d skipped%s, %d late" % (
            len(wins), total, foot, total - foot, draws, skipped, " (%s)" % ", ".join("%s x%d" % kv for kv in sorted(why.items())) if why else "", late)
        if draws == 0:
            add("INSTRUMENT", "STOP", "armed and ticking, but the 2D screen's composite was never seen (draws=0 in every window): the recognition did not match, "
                "or the screen was never on show. " + base)
        elif total == 0:
            add("INSTRUMENT", "STOP", "the composite was seen but not one sample could be read: " + base)
        elif skipped > total:
            add("INSTRUMENT", "WARN", "more samples skipped than read: " + base)
        elif late:
            add("INSTRUMENT", "WARN", "a copy was not ready in time: " + base)
        else:
            add("INSTRUMENT", "PASS", base)

    # ---- ON FOOT ----
    foot_wins = [w for w in wins if w["on_foot"] and w["fp"] is not None]
    if wins:
        if not foot_wins:
            others = [w["other_fp"] for w in wins if w["other_fp"] is not None]
            add("ON FOOT", "WARN", "no on-foot sample: the screen was measured at the menu only%s, and nothing on foot was stored for the next launch"
                % (" (%.0f px)" % statistics.median(others) if others else ""))
        else:
            fps = [w["fp"] for w in foot_wins]
            add("ON FOOT", "PASS", "%d on-foot window(s), %d sample(s); the screen spans %.0f px (median of the windows' medians %.0f..%.0f) of the eye"
                % (len(foot_wins), sum(w["on_foot"] for w in foot_wins), statistics.median(fps), min(fps), max(fps)))
            # ---- STABLE ----
            med = statistics.median(fps)
            spread = (max(fps) - min(fps)) / med if med else 0.0
            inside = max(((w["hi"] - w["lo"]) / w["fp"] for w in foot_wins if w["lo"] is not None and w["hi"] is not None and w["fp"]), default=0.0)
            if spread > VSCREEN_STABLE or inside > VSCREEN_STABLE:
                add("STABLE", "WARN", "the screen's width moved: %.1f%% between windows, up to %.1f%% inside one (over %.0f%%): the head, a menu, or the panel distance "
                    "changed during the flight" % (spread * 100.0, inside * 100.0, VSCREEN_STABLE * 100.0))
            else:
                add("STABLE", "PASS", "the screen's width held: %.1f%% between windows, up to %.1f%% inside one (under %.0f%%)" % (spread * 100.0, inside * 100.0, VSCREEN_STABLE * 100.0))
            # ---- SHAPE ----
            shapes = [w["shape"] for w in foot_wins if w["shape"] is not None]
            if not shapes:
                add("SHAPE", "n/a", "no shape= token: the eye's height was not known (the runtime had not published its sizing)")
            else:
                worst = max(abs(s / VSCREEN_SHAPE - 1.0) for s in shapes)
                status = "STOP" if worst > VSCREEN_SHAPE_STOP else ("WARN" if worst > VSCREEN_SHAPE_WARN else "PASS")
                add("SHAPE", status, "the footprint's pixel aspect reads %.3f..%.3f against 16:9 = %.3f (worst %.1f%% off)%s"
                    % (min(shapes), max(shapes), VSCREEN_SHAPE, worst * 100.0,
                       "" if status == "PASS" else ": the corner arithmetic, the eye size or the screen's stretch is not what the instrument assumes"))
            # ---- DISTANCE LAW ----
            by_d = {}
            for w in foot_wins:
                if w["applied"] and w["frac1"] is not None:
                    by_d.setdefault(round(w["applied"], 2), []).append(w["frac1"])
            if len(by_d) < 2:
                add("DISTANCE LAW", "n/a", "one panel distance in this log (%s): a restart leg or a live change at another fix.panel_distance tests the 1/d law"
                    % (", ".join("%.2f" % d for d in by_d) or "unknown"))
            else:
                meds = {d: statistics.median(v) for d, v in by_d.items()}
                lo, hi = min(meds.values()), max(meds.values())
                spread = (hi - lo) / statistics.median(meds.values())
                add("DISTANCE LAW", "PASS" if spread <= VSCREEN_LAW else "STOP",
                    "the footprint at distance 1 over %d distances (%s): %.1f%% apart (%s %.0f%%): A %s 1/d"
                    % (len(meds), ", ".join("%.2f -> %.4f" % (d, meds[d]) for d in sorted(meds)), spread * 100.0,
                       "under" if spread <= VSCREEN_LAW else "over", VSCREEN_LAW * 100.0, "varies as" if spread <= VSCREEN_LAW else "does NOT vary as"))
            # ---- CALIBRATION ----
            seed_w, seed_d, seed_e = VSCREEN_SEED
            at_seed = [w for w in foot_wins if w["eye"] and w["eye"][0] == int(seed_e) and w["applied"] and abs(w["applied"] - seed_d) < 0.011]
            if at_seed:
                got = statistics.median([w["fp"] for w in at_seed])
                ratio = got / seed_w
                if abs(ratio - 1.0) <= VSCREEN_CALIBRATION:
                    add("CALIBRATION", "PASS", "at Sean's calibration point (a %d px eye at distance %.1f) the screen spans %.0f px against the 3504 he flew (%.1f%%): m = 1.0 "
                        "reproduces his width" % (int(seed_e), seed_d, got, ratio * 100.0))
                else:
                    add("CALIBRATION", "WARN", "at Sean's calibration point the screen spans %.0f px, not the 3504 he flew (%.1f%%): m = 1.0 would fit %d wide there; "
                        "m = %.3f reproduces 3504 -- his call whether 3504 or the measurement is right (vscreen_fit.h kMultiplier, kSeed*)"
                        % (got, ratio * 100.0, _vround16(got), seed_w / got))
            else:
                add("CALIBRATION", "n/a", "not at the calibration point (a %d px eye at panel distance %.1f)" % (int(seed_e), seed_d))

    # ---- STORED ----
    if wins:
        last = wins[-1]
        persisted = [w for w in wins if w["persisted"] is not None]
        if persisted:
            p = persisted[-1]
            fit = p["fit"]
            note = ""
            if applied is not None and fit:
                note = "; the next launch fits %d wide (this launch: %d)" % (fit, applied["w"]) if abs(fit - applied["w"]) > 16 else \
                    "; the next launch fits %d wide, the width this launch ran at (%d)" % (fit, applied["w"])
            if rule is not None and rule["kv"].get("route") == "no":
                note += " -- this launch's route=no (legacy), so a launch fits only once the world route will run"
            add("STORED", "PASS", "the on-foot median is stored: %.4f of the eye at panel distance 1 (%d sample(s))%s" % (p["persisted"], p["session_n"], note))
        elif foot_wins:
            add("STORED", "WARN", "on-foot samples were taken (%d) but nothing was stored: the file needs at least 12 on-foot samples in the session"
                % sum(w["on_foot"] for w in foot_wins))
        else:
            add("STORED", "n/a", "nothing was stored: no on-foot samples")
    return out


def vscreen_fit_summary(verdict):
    counts = {"PASS": 0, "WARN": 0, "STOP": 0, "n/a": 0}
    for _, status, _ in verdict:
        counts[status] = counts.get(status, 0) + 1
    worst = "STOP" if counts["STOP"] else ("WARN" if counts["WARN"] else ("PASS" if counts["PASS"] else "n/a"))
    return "vscreen fit verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)" % (worst, counts["PASS"], counts["WARN"], counts["STOP"], counts["n/a"])


def print_vscreen_fit(text):
    """The --vscreen-fit report. Returns the process exit code: 0 when the log has any of the auto-fit lines, 1 when it has none (a log
    from a build that predates them, or no VR session); the verdict never changes the exit code (read its lines)."""
    f = parse_vscreen_fit(text)
    wins = vscreen_fit_windows(f)
    lines = (1 if f["rule"] else 0) + (1 if f["explicit"] else 0) + (1 if f["no_eye"] else 0) + len(f["armed"]) + len(wins)
    if not lines:
        print("[edvr] no vscreen auto-fit line in this log (a build that predates the fit, or a session that never reached the panel patch).")
        return 1
    print("[edvr] vscreen fit: %s, %s, %d arming line(s), %d footprint window(s)"
          % ("the rule line" if f["rule"] else ("an explicit-width line" if f["explicit"] else "no rule line"),
             "an apply line" if f["applied"] else "no apply line", len(f["armed"]), len(wins)))
    if f["rule"]:
        kv = f["rule"]["kv"]
        print("launch: auto = %d wide, rule=%s source=%s route=%s eye=%s distance=%s footprint=%s clamp=%s legacy=%s%s"
              % (f["rule"]["width"], kv.get("rule", "?"), kv.get("source", "?"), kv.get("route", "?"), kv.get("eye", "?"), kv.get("distance", "?"),
                 kv.get("footprint", "-"), kv.get("clamp", "-"), kv.get("legacy", "?"),
                 "; applied %dx%d" % (f["applied"]["w"], f["applied"]["h"]) if f["applied"] else ""))
    for w in wins:
        print("window %s: samples %d (on foot %d, other %d), draws %d, skipped %d, late %d; eye %s distance %s applied %s; on foot fp=%s frac=%s range=%s..%s "
              "shape=%s; at distance 1 %s px (%s); session n=%d median %s persisted %s; next launch fits %s (legacy %s)"
              % (w["window"], w["samples"], w["on_foot"], w["other"], w["draws"], w["skipped"], w["late"],
                 "%dx%d" % w["eye"] if w["eye"] else "-", w["distance"], w["applied"], "%.0f" % w["fp"] if w["fp"] is not None else "-",
                 w["frac"], "%.0f" % w["lo"] if w["lo"] is not None else "-", "%.0f" % w["hi"] if w["hi"] is not None else "-", w["shape"],
                 "%.0f" % w["at1"] if w["at1"] is not None else "-", w["frac1"], w["session_n"], w["session_frac1"], w["persisted"],
                 "%.0f" % w["fit"] if w["fit"] is not None else "-", "%.0f" % w["legacy"] if w["legacy"] is not None else "-"))
    verdict = vscreen_fit_verdict(f)
    for tag, status, text_ in verdict:
        print("%s (%s) %s" % (status, tag, text_))
    print(vscreen_fit_summary(verdict))
    return 0


# ---------------------------------------------------------------------------------------------------------------------------------------
# --flat-upscale: the flat profile's final copy admitted by structure (design doc section 83), and what follows from it. Below the output
# (Elite's supersampling under 1.0) the game's own copy upscales; the structure admission is what lets DLSS and FSR resolve there whatever
# bloom, depth of field and the tone variant do. The lines are written by src/d3d11/flat_copy_structure.h (`flat copy structure 5s:`, the
# first admission and the declines), flat_standdown.h, flat_elite_settings.h and flat_runtime.cpp; tools\flat_temporal_test holds the
# formatters to tools\flat_upscale_fixture.log (a good flight and three episodes), the file this reader's self-test reads.
# ---------------------------------------------------------------------------------------------------------------------------------------
FLATU_TS = r"^(?:\[(?P<ts>[0-9:.]+)\] )?"
FLATU_KEY_RE = re.compile(FLATU_TS + r"flat hdr route: experimental\.temporal_aa_before_post=(?P<key>\w+) \((?P<when>read at startup|changed)\) at frame=(?P<frame>\d+)")
FLATU_TRIGGER_RE = re.compile(FLATU_TS + r"flat hdr route: first trigger at frame=(?P<frame>\d+)")
FLATU_WINDOW_RE = re.compile(FLATU_TS + r"flat copy structure 5s: (?P<rest>.*)$")
FLATU_FIRST_RE = re.compile(
    FLATU_TS + r"flat copy structure: first admission at frame=(?P<frame>\d+) \(experimental\.temporal_aa_before_post=auto\): the game's final copy reads a "
    r"(?P<w>\d+)x(?P<h>\d+) R8G8B8A8 image written by one pass, VS=(?P<vs>[0-9A-F]+) PS=(?P<ps>[0-9A-F]+), after the scene HDR's first consumer "
    r"\(VS=(?P<tvs>[0-9A-F]+) PS=(?P<tps>[0-9A-F]+)\), .*?\(the whitelist said (?P<wl>[\w-]+)\); the scene is (?P<sw>\d+)x(?P<sh>\d+) on a "
    r"(?P<ow>\d+)x(?P<oh>\d+) output(?P<menu> \(the 3D menu\))?, route=(?P<route>[\w-]+);")
FLATU_DECLINED_RE = re.compile(
    FLATU_TS + r"flat copy structure: declined at frame=(?P<frame>\d+): (?P<why>[\w-]+) \(the whitelist said (?P<wl>[\w-]+)\); the final copy reads a "
    r"(?P<w>\d+)x(?P<h>\d+) fmt (?P<fmt>\d+) image with (?P<writers>\d+) writer\(s\) .*?, (?P<passes>\d+) R-sized image pass\(es\) between")
FLATU_ROUTE_RE = re.compile(FLATU_TS + r"flat route: (?P<name>[\w-]+) R=(?P<rw>\d+)x(?P<rh>\d+) E=(?P<ew>\d+)x(?P<eh>\d+) D=(?P<dw>\d+)x(?P<dh>\d+)(?P<rest>.*)$")
FLATU_STAND_RE = re.compile(FLATU_TS + r"flat stand-down: (?P<what>entered|resumed|ended|still stood down) (?P<rest>.*)$")
FLATU_STAND_ENTERED_RE = re.compile(r"^at frame=(?P<frame>\d+): every frame for (?P<secs>[\d.]+) s \((?P<frames>\d+) frames\) was refused for (?P<reason>[\w-]+)(?: \((?P<words>[^)]*)\))?, none treated")
FLATU_STAND_STILL_RE = re.compile(r"^at frame=(?P<frame>\d+) after (?P<secs>\d+) s: refused for (?P<reason>[\w-]+)(?: \((?P<words>[^)]*)\))?, (?P<probes>\d+) probes so far")
FLATU_WARN_RE = re.compile(FLATU_TS + r"flat settings warning: (?P<what>shown|changed|hidden)(?: \(mode=(?P<mode>\w+), frames refused for (?P<reason>[\w-]+)(?P<flags>.*?)\): (?P<words>.*))?$")
FLATU_REFUSAL_RE = re.compile(FLATU_TS + r"flat runtime refusal 5s: reason=(?P<reason>[\w-]+) count=(?P<count>\d+)")
FLATU_RUNTIME_RE = re.compile(FLATU_TS + r"flat runtime: treated=(?P<treated>\d+) refused=(?P<refused>\d+) last=(?P<last>[\w-]+)")
FLATU_TONE_REASONS = ("no-known-tone-pass", "invalid-tone-pass")
FLATU_SILENT_REASONS = ("no-3d-scene", "no-known-output-copy")
FLATU_OLD_ADVICE = ("Supersampling is below 1.0", ", supersampling below 1.0 (render ")


def parse_flat_upscale(text):
    """The log's flat-upscale lines: {keys: [{ts, key, when}], triggers: [index of the first-trigger line], windows: [{ts, kv, index}], first: {...} or None,
    declines: [{ts, why, wl, passes}], routes: [{ts, name, r, e, d}], stand: [{ts, what, reason, words, index}], warns: [{ts, what, reason, mode,
    flags, words, index}], refusals: {reason: count}, runtime: [{ts, treated, refused, last}], old_advice: bool, flat: bool}. A line cut short or
    garbled is skipped, never fatal."""
    f = {"keys": [], "triggers": [], "windows": [], "first": None, "declines": [], "routes": [], "stand": [], "warns": [], "refusals": {}, "runtime": [],
         "old_advice": False, "flat": False}
    for index, raw in enumerate(text.splitlines()):
        try:
            if any(token in raw for token in FLATU_OLD_ADVICE):
                f["old_advice"] = True
            m = FLATU_KEY_RE.match(raw)
            if m:
                f["flat"] = True
                f["keys"].append({"ts": m.group("ts") or "", "key": m.group("key"), "when": m.group("when")})
                continue
            m = FLATU_TRIGGER_RE.match(raw)
            if m:
                f["triggers"].append(index)
                continue
            m = FLATU_WINDOW_RE.match(raw)
            if m:
                f["windows"].append({"ts": m.group("ts") or "", "kv": _ckv(m.group("rest")), "index": index})
                continue
            m = FLATU_FIRST_RE.match(raw)
            if m:
                if f["first"] is None:
                    f["first"] = {"ts": m.group("ts") or "", "frame": int(m.group("frame")), "src": (int(m.group("w")), int(m.group("h"))),
                                  "scene": (int(m.group("sw")), int(m.group("sh"))), "output": (int(m.group("ow")), int(m.group("oh"))),
                                  "whitelist": m.group("wl"), "route": m.group("route"), "menu": bool(m.group("menu")), "index": index}
                continue
            m = FLATU_DECLINED_RE.match(raw)
            if m:
                f["declines"].append({"ts": m.group("ts") or "", "why": m.group("why"), "wl": m.group("wl"), "passes": int(m.group("passes")),
                                      "src": (int(m.group("w")), int(m.group("h")))})
                continue
            m = FLATU_ROUTE_RE.match(raw)
            if m:
                f["routes"].append({"ts": m.group("ts") or "", "name": m.group("name"), "r": (int(m.group("rw")), int(m.group("rh"))),
                                    "e": (int(m.group("ew")), int(m.group("eh"))), "d": (int(m.group("dw")), int(m.group("dh")))})
                continue
            m = FLATU_STAND_RE.match(raw)
            if m:
                what, rest = m.group("what"), m.group("rest")
                entry = {"ts": m.group("ts") or "", "what": what, "reason": None, "words": None, "index": index}
                sub = (FLATU_STAND_ENTERED_RE if what == "entered" else FLATU_STAND_STILL_RE if what == "still stood down" else None)
                sm = sub.match(rest) if sub else None
                if sm:
                    entry["reason"], entry["words"] = sm.group("reason"), sm.group("words")
                f["stand"].append(entry)
                continue
            m = FLATU_WARN_RE.match(raw)
            if m:
                f["warns"].append({"ts": m.group("ts") or "", "what": m.group("what"), "reason": m.group("reason"), "mode": m.group("mode"),
                                   "flags": m.group("flags") or "", "words": m.group("words") or "", "index": index})
                continue
            m = FLATU_REFUSAL_RE.match(raw)
            if m:
                f["refusals"][m.group("reason")] = f["refusals"].get(m.group("reason"), 0) + int(m.group("count"))
                continue
            m = FLATU_RUNTIME_RE.match(raw)
            if m:
                f["flat"] = True
                f["runtime"].append({"ts": m.group("ts") or "", "treated": int(m.group("treated")), "refused": int(m.group("refused")), "last": m.group("last")})
        except (ValueError, TypeError):
            continue
    return f


def flat_upscale_windows(f):
    """Each `flat copy structure 5s:` window's tokens as numbers: [{ts, key, copies, whitelist, admitted, declined, selector_refused, no_scene,
    render_size, route_serves, key_off, last, scene (w, h) or None, output (w, h) or None, source (w, h) or None, ldr_before_max, declines {why: n}}]."""
    out = []
    for w in f["windows"]:
        kv = w["kv"]

        def size(token):
            m = re.match(r"^(\d+)x(\d+)$", kv.get(token, ""))
            return (int(m.group(1)), int(m.group(2))) if m and int(m.group(1)) else None
        declines = {}
        text = kv.get("declines", "none")
        if text != "none":
            for part in text.split(","):
                key, sep, value = part.rpartition(":")
                if sep and value.isdigit():
                    declines[key] = int(value)
        out.append({"ts": w["ts"], "key": kv.get("key", "?"), "copies": _cint(kv.get("copies")) or 0, "whitelist": _cint(kv.get("whitelist")) or 0,
                    "admitted": _cint(kv.get("admitted")) or 0, "declined": _cint(kv.get("declined")) or 0,
                    "selector_refused": _cint(kv.get("selector-refused")) or 0, "no_scene": _cint(kv.get("no-scene")) or 0,
                    "render_size": _cint(kv.get("render-size")) or 0, "route_serves": _cint(kv.get("route-serves")) or 0,
                    "key_off": _cint(kv.get("key-off")) or 0, "last": kv.get("last", "?"), "scene": size("scene"), "output": size("output"),
                    "source": size("source"), "ldr_before_max": _cint(kv.get("ldr-passes-before-max")) or 0, "declines": declines})
    return out


def flat_upscale_verdict(f):
    """The verdict on one flight: [(tag, status, text)], status PASS, WARN, STOP or n/a (what the log cannot say). The tags are the questions the
    flight plan asks: KEY (is the admission on), ADMISSION (did it run, and what did it see), TREATED (did frames get treated), UPSCALE (below the
    output, by structure or by the whitelist), TONE REFUSALS (frames the whitelist refused for a tone pass and nothing admitted), STAND-DOWN (which
    reasons stood the work down), F8 WARNING (what the panel said, and the startup false warning), CHAIN (a game anti-aliasing chain the structure
    declined), ADVICE (the old supersampling advice must be gone)."""
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    wins = flat_upscale_windows(f)
    total = {k: sum(w[k] for w in wins) for k in ("copies", "whitelist", "admitted", "declined", "selector_refused", "no_scene", "render_size", "route_serves", "key_off")}
    runtime = f["runtime"]
    treated = (runtime[-1]["treated"] - runtime[0]["treated"]) if len(runtime) > 1 else (runtime[-1]["treated"] if runtime else 0)
    # The AA rule's declines: the frames the windows counted (each cause is logged as a line once a session, so the lines are not a count of frames),
    # or, with no window that counted them, the decline lines.
    chain_lines = sum(1 for d in f["declines"] if "r-sized-image-passes" in d["why"])
    chain_frames = sum(n for w in wins for why, n in w["declines"].items() if "r-sized-image-passes" in why)
    passes_declines = chain_frames or chain_lines

    # KEY
    if f["keys"]:
        last = f["keys"][-1]
        if last["key"] == "auto":
            add("KEY", "PASS", "experimental.temporal_aa_before_post=auto (%s): the game's final copy is admitted by structure where the HDR route does not serve the frame" % last["when"])
        else:
            add("KEY", "WARN", "experimental.temporal_aa_before_post=%s (%s): the copy route is the whitelist alone; nothing is admitted by structure" % (last["key"], last["when"]))
    else:
        add("KEY", "n/a", "no `flat hdr route:` key line (a build that predates the route, or a session that never reached a Present)")

    # ADMISSION
    if not wins:
        add("ADMISSION", "STOP", "no `flat copy structure 5s:` window: the admission never ran (a build that predates section 83, or no temporal mode was selected)")
    else:
        add("ADMISSION", "PASS" if total["copies"] else "WARN", "%d window(s): copies %d, whitelist %d, admitted %d, declined %d, selector-refused %d, no-3d-scene %d, render-size %d, route-serves %d, key-off %d%s%s"
            % (len(wins), total["copies"], total["whitelist"], total["admitted"], total["declined"], total["selector_refused"], total["no_scene"], total["render_size"],
               total["route_serves"], total["key_off"],
               "" if total["copies"] else "; NO final copy was ruled on in any window: the admission ran and had nothing to say (a session that never drew a 3D frame, or a copy the reducer never reached)",
               "; first admission at frame %d (%dx%d image, scene %dx%d on %dx%d, route %s%s)" % (
                   f["first"]["frame"], f["first"]["src"][0], f["first"]["src"][1], f["first"]["scene"][0], f["first"]["scene"][1], f["first"]["output"][0],
                   f["first"]["output"][1], f["first"]["route"], ", the 3D menu" if f["first"]["menu"] else "") if f["first"] else "; no frame was admitted by structure"))

    # TREATED
    if not runtime:
        add("TREATED", "n/a", "no `flat runtime:` line")
    elif treated > 0:
        add("TREATED", "PASS", "%d frame(s) treated over the log (the counter went %d -> %d)" % (treated, runtime[0]["treated"], runtime[-1]["treated"]))
    else:
        add("TREATED", "STOP", "no frame was treated (the counter stayed at %d; last verdict %s)" % (runtime[-1]["treated"], runtime[-1]["last"]))

    # UPSCALE
    below = [r for r in f["routes"] if r["r"][0] < r["d"][0] or r["r"][1] < r["d"][1]]
    if not below:
        add("UPSCALE", "n/a", "no frame rendered below the output in this log (no `flat route:` line with R under D)")
    else:
        names = sorted({"%s R=%dx%d D=%dx%d" % (r["name"], r["r"][0], r["r"][1], r["d"][0], r["d"][1]) for r in below})
        if total["admitted"] > 0 and treated > 0:
            add("UPSCALE", "PASS", "below the output (%s): %d frame(s) admitted by structure, treated" % ("; ".join(names), total["admitted"]))
        elif total["whitelist"] > 0 and treated > 0:
            add("UPSCALE", "PASS", "below the output (%s): the whitelist selected %d frame(s), treated; the structure had nothing to do (every frame had a known tone pass)" % ("; ".join(names), total["whitelist"]))
        else:
            add("UPSCALE", "STOP", "below the output (%s) and nothing was admitted or selected, or nothing treated" % "; ".join(names))

    # TONE REFUSALS
    tone = {r: n for r, n in f["refusals"].items() if r in FLATU_TONE_REASONS}
    if not tone:
        add("TONE REFUSALS", "PASS", "no frame was refused for a tone pass")
    else:
        text = ", ".join("%s %d" % (r, n) for r, n in sorted(tone.items()))
        add("TONE REFUSALS", "WARN" if treated > 0 else "STOP", "%s frame(s) refused for a tone pass: %s" % (sum(tone.values()), text))

    # STAND-DOWN
    entered = [s for s in f["stand"] if s["what"] == "entered"]
    if not entered:
        add("STAND-DOWN", "PASS", "the work never stood down")
    else:
        worst, notes = "PASS", []
        for s in entered:
            reason = s["reason"] or "?"
            if reason in FLATU_SILENT_REASONS:
                status = "PASS"
            elif reason == "render-size-does-not-fit-output":
                status = "WARN"
            elif reason in FLATU_TONE_REASONS and passes_declines:
                status = "WARN"
            else:
                status = "STOP"
            notes.append("%s%s: %s%s" % (s["ts"] or "?", "", reason, " (%s)" % s["words"] if s["words"] else ""))
            worst = "STOP" if status == "STOP" or worst == "STOP" else ("WARN" if status == "WARN" or worst == "WARN" else "PASS")
        add("STAND-DOWN", worst, "%d stand-down(s): %s%s" % (len(entered), "; ".join(notes),
            "" if worst == "PASS" else " (no-3d-scene is a startup or a loading screen, silent; render-size-does-not-fit-output is Elite's resolution not being the screen's shape; a tone refusal "
                                            "with declines for R-sized passes is a game anti-aliasing chain; anything else is a chain nothing recognised)"))

    # F8 WARNING
    shown = [w for w in f["warns"] if w["what"] in ("shown", "changed")]
    if not shown:
        add("F8 WARNING", "PASS", "the panel showed no warning")
    else:
        first_scene = min([i for i in f["triggers"]] + ([f["first"]["index"]] if f["first"] else []) or [10 ** 9])
        worst, notes = "PASS", []
        for w in shown:
            reason = w["reason"] or "?"
            if w["index"] < first_scene and reason != "render-size-does-not-fit-output":
                status, why = "STOP", "BEFORE ANY SCENE: the false startup warning"
            elif reason == "render-size-does-not-fit-output":
                status, why = "WARN", "the render size"
            elif reason in FLATU_TONE_REASONS and passes_declines:
                status, why = "WARN", "a game anti-aliasing chain, Anti-aliasing advised"
            else:
                status, why = "STOP", "an unrecognised chain"
            notes.append("%s %s (%s): %s" % (w["ts"] or "?", reason, why, w["words"][:110]))
            worst = "STOP" if status == "STOP" or worst == "STOP" else ("WARN" if status == "WARN" or worst == "WARN" else "PASS")
        add("F8 WARNING", worst, "%d warning(s): %s" % (len(shown), " | ".join(notes)))

    # CHAIN
    if not wins:
        add("CHAIN", "n/a", "no window")
    elif passes_declines:
        longest = max([d["passes"] for d in f["declines"]] + [w["ldr_before_max"] for w in wins])
        add("CHAIN", "WARN", "the structure declined %s with R-sized image passes between the scene HDR's first consumer and the copy (the longest chain: %d): "
            "the game's anti-aliasing filter, which the structure leaves refused; turn Anti-aliasing off in Elite"
            % ("%d frame(s)" % chain_frames if chain_frames else "frames (%d decline line(s); no window counted them)" % chain_lines, longest))
    else:
        add("CHAIN", "PASS", "no R-sized image pass between the scene HDR's first consumer and the copy (the longest chain seen: %d)" % max([w["ldr_before_max"] for w in wins] + [0]))

    # ADVICE
    if f["old_advice"]:
        add("ADVICE", "STOP", "the old supersampling advice is in this log (\"Supersampling is below 1.0\"): a build from before section 83, or the advice is back")
    else:
        add("ADVICE", "PASS", "the supersampling advice is gone (below 1.0 is supported)")
    return out


def flat_upscale_summary(verdict):
    counts = {"PASS": 0, "WARN": 0, "STOP": 0, "n/a": 0}
    for _, status, _ in verdict:
        counts[status] = counts.get(status, 0) + 1
    worst = "STOP" if counts["STOP"] else ("WARN" if counts["WARN"] else ("PASS" if counts["PASS"] else "n/a"))
    return "flat upscale verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)" % (worst, counts["PASS"], counts["WARN"], counts["STOP"], counts["n/a"])


def print_flat_upscale(text):
    """The --flat-upscale report. Returns the process exit code: 0 when the log has any flat line, 1 when it has none (a VR log, or no flat session);
    the verdict never changes the exit code (read its lines)."""
    f = parse_flat_upscale(text)
    wins = flat_upscale_windows(f)
    if not (f["keys"] or wins or f["runtime"] or f["stand"] or f["warns"]):
        print("[edvr] no flat-profile line in this log (a VR session, or a build that predates the flat runtime).")
        return 1
    print("[edvr] flat upscale: %d key line(s), %d copy-structure window(s), %d route line(s), %d stand-down line(s), %d warning line(s), %d decline line(s)"
          % (len(f["keys"]), len(wins), len(f["routes"]), len(f["stand"]), len(f["warns"]), len(f["declines"])))
    for k in f["keys"]:
        print("key %s: experimental.temporal_aa_before_post=%s (%s)" % (k["ts"] or "?", k["key"], k["when"]))
    for r in f["routes"]:
        print("route %s: %s R=%dx%d E=%dx%d D=%dx%d" % (r["ts"] or "?", r["name"], r["r"][0], r["r"][1], r["e"][0], r["e"][1], r["d"][0], r["d"][1]))
    if f["first"]:
        fi = f["first"]
        print("first admission %s at frame %d: a %dx%d image, scene %dx%d on a %dx%d output, route %s%s; the whitelist said %s"
              % (fi["ts"] or "?", fi["frame"], fi["src"][0], fi["src"][1], fi["scene"][0], fi["scene"][1], fi["output"][0], fi["output"][1], fi["route"],
                 " (the 3D menu)" if fi["menu"] else "", fi["whitelist"]))
    for d in f["declines"]:
        print("declined %s: %s (the whitelist said %s; a %dx%d image, %d R-sized pass(es) between)" % (d["ts"] or "?", d["why"], d["wl"], d["src"][0], d["src"][1], d["passes"]))
    for w in wins:
        print("window %s: key=%s copies %d (whitelist %d, admitted %d, declined %d, selector-refused %d, no-3d-scene %d, render-size %d, route-serves %d, key-off %d); last %s; "
              "scene %s output %s source %s; longest chain %d%s"
              % (w["ts"] or "?", w["key"], w["copies"], w["whitelist"], w["admitted"], w["declined"], w["selector_refused"], w["no_scene"], w["render_size"], w["route_serves"],
                 w["key_off"], w["last"], "%dx%d" % w["scene"] if w["scene"] else "-", "%dx%d" % w["output"] if w["output"] else "-", "%dx%d" % w["source"] if w["source"] else "-",
                 w["ldr_before_max"], "; declines " + ", ".join("%s:%d" % kv for kv in sorted(w["declines"].items())) if w["declines"] else ""))
    for s in f["stand"]:
        print("stand-down %s: %s%s%s" % (s["ts"] or "?", s["what"], " for %s" % s["reason"] if s["reason"] else "", " (%s)" % s["words"] if s["words"] else ""))
    for w in f["warns"]:
        print("F8 warning %s: %s%s%s" % (w["ts"] or "?", w["what"], " for %s" % w["reason"] if w["reason"] else "", ": %s" % w["words"][:200] if w["words"] else ""))
    if f["refusals"]:
        print("refusals (summed from `flat runtime refusal 5s:`): " + ", ".join("%s %d" % kv for kv in sorted(f["refusals"].items(), key=lambda kv: -kv[1])))
    verdict = flat_upscale_verdict(f)
    for tag, status, text_ in verdict:
        print("%s (%s) %s" % (status, tag, text_))
    print(flat_upscale_summary(verdict))
    return 0


# ---------------------------------------------------------------------------------------------------------------------------------------
# --vr-supersampling: Elite's Supersampling below 1.0 in VR, read from the render sizes (design doc section 83, the VR warning). The lines are
# src/common/vr_supersample_notice.h's log line (once a session), vscreen.cpp's own adoption line it follows, and menu.cpp's note that the
# headset notice was queued. tools\vscreen_fit_test (R13) holds the formatter and the wiring; this reader's self-test builds the lines from
# the header's own text.
# ---------------------------------------------------------------------------------------------------------------------------------------
VRSS_RE = re.compile(FLATU_TS + r"vr supersampling: Elite draws the 3D world at (?P<rw>\d+)x(?P<rh>\d+), (?P<pct>\d+)% of the (?P<ew>\d+)x(?P<eh>\d+) eye texture, and scales it up before EDVR sees it: ")
VRSS_ADOPT_RE = re.compile(FLATU_TS + r"vScreen: the world on this rig is rendered at (?P<rw>\d+)x(?P<rh>\d+) and scaled into the (?P<ew>\d+)x(?P<eh>\d+) the headset is handed -- (?P<pct>\d+)% of the width")
VRSS_QUEUED_RE = re.compile(FLATU_TS + r"vr supersampling: (?:the headset notice is queued as a toast|menu\.toasts is off, so no toast)")
VRSS_BELOW_PERCENT = 98    # kBelowPercent in src/common/vr_supersample_notice.h


def vrss_below(r, e):
    """vrss::below: the render size under kBelowPercent of the eye's width AND height, an exact compare (the percent in the log is rounded,
    so 97.6% reads 98 there and is still below)."""
    return bool(r[0] and r[1] and e[0] and e[1] and r[0] * 100 < e[0] * VRSS_BELOW_PERCENT and r[1] * 100 < e[1] * VRSS_BELOW_PERCENT)


def parse_vr_supersampling(text):
    """{notice: {ts, r, pct, e} or None, adopt: {ts, r, e, pct} or None, queued: ts or None, toast: bool, flat: bool}."""
    f = {"notice": None, "adopt": None, "queued": None, "toast": False, "flat": False}
    for raw in text.splitlines():
        try:
            m = VRSS_RE.match(raw)
            if m:
                if f["notice"] is None:
                    f["notice"] = {"ts": m.group("ts") or "", "r": (int(m.group("rw")), int(m.group("rh"))), "pct": int(m.group("pct")), "e": (int(m.group("ew")), int(m.group("eh")))}
                continue
            m = VRSS_ADOPT_RE.match(raw)
            if m:
                if f["adopt"] is None:
                    f["adopt"] = {"ts": m.group("ts") or "", "r": (int(m.group("rw")), int(m.group("rh"))), "e": (int(m.group("ew")), int(m.group("eh"))), "pct": int(m.group("pct"))}
                continue
            m = VRSS_QUEUED_RE.match(raw)
            if m:
                if f["queued"] is None:
                    f["queued"] = m.group("ts") or "?"
                    f["toast"] = "queued as a toast" in raw
                continue
            if FLATU_RUNTIME_RE.match(raw) or FLATU_KEY_RE.match(raw):
                f["flat"] = True
        except (ValueError, TypeError):
            continue
    return f


def vr_supersampling_verdict(f):
    """[(tag, status, text)]: NOTICE (the log line, from the measured sizes), CONSISTENT (it agrees with vScreen's own adoption line), HEADSET (the
    toast was queued, and the Status page's hint has it while the menu is open), FLAT (a flat log never carries it)."""
    out = []

    def add(tag, status, text):
        out.append((tag, status, text))

    n, a = f["notice"], f["adopt"]
    if f["flat"] and (n or a or f["queued"]):
        add("FLAT", "STOP", "a flat-profile log carries the VR notice: it must never")
    elif f["flat"]:
        add("FLAT", "PASS", "a flat-profile log, and no VR notice in it")
    if n:
        if vrss_below(n["r"], n["e"]):
            add("NOTICE", "PASS", "the world is drawn at %dx%d, %d%% of the %dx%d eye texture: Elite's Supersampling is below 1 (or an upscaler sits in the chain)"
                % (n["r"][0], n["r"][1], n["pct"], n["e"][0], n["e"][1]))
        else:
            add("NOTICE", "STOP", "the notice names %dx%d against a %dx%d eye (%d%%), which is not below %d%% on both axes" % (n["r"][0], n["r"][1], n["e"][0], n["e"][1], n["pct"], VRSS_BELOW_PERCENT))
    elif a and vrss_below(a["r"], a["e"]):
        add("NOTICE", "STOP", "vScreen measured the world at %d%% of the eye (%dx%d in %dx%d) and no `vr supersampling:` line followed: the detection did not run" % (a["pct"], a["r"][0], a["r"][1], a["e"][0], a["e"][1]))
    elif a:
        add("NOTICE", "n/a", "vScreen measured the world at %d%% of the eye: not below %d%% on both axes, so no notice is right" % (a["pct"], VRSS_BELOW_PERCENT))
    else:
        add("NOTICE", "n/a", "vScreen adopted no render size: the world may be drawn at the eye's own size, or vScreen's guards held the adoption back (read its `vScreen:` lines), "
                             "or no scene was drawn yet. Elite's Supersampling below 1 cannot be ruled out from this log")
    if n and a:
        if n["r"] == a["r"] and n["e"] == a["e"]:
            add("CONSISTENT", "PASS", "the notice's sizes are vScreen's own adoption line's")
        else:
            add("CONSISTENT", "STOP", "the notice says %dx%d in %dx%d, vScreen's adoption line %dx%d in %dx%d" % (n["r"] + n["e"] + a["r"] + a["e"]))
    elif n:
        add("CONSISTENT", "WARN", "no vScreen adoption line to check the notice against")
    if n:
        if f["queued"]:
            add("HEADSET", "PASS", "%s (%s)" % ("the headset notice was queued as a toast" if f["toast"] else "menu.toasts is off: no toast", f["queued"]) + "; the Status page shows the advice as its hint while the menu is open")
        else:
            add("HEADSET", "WARN", "no `vr supersampling:` menu line: the headset notice was not queued (the menu may not have ticked yet)")
    return out


def print_vr_supersampling(text):
    """The --vr-supersampling report. Returns 0 when the log has any of the lines, 1 when it has none; the verdict never changes the exit code."""
    f = parse_vr_supersampling(text)
    if not (f["notice"] or f["adopt"] or f["queued"]):
        print("[edvr] no `vr supersampling:` or vScreen render-size line in this log (the world may be drawn at the eye's own size, or vScreen's guards held the "
              "adoption back, or this is a build that predates section 83: Supersampling below 1 cannot be ruled out from it).")
        return 1
    if f["adopt"]:
        a = f["adopt"]
        print("adoption %s: the world is drawn at %dx%d and scaled into the %dx%d the headset is handed (%d%% of the width)" % (a["ts"] or "?", a["r"][0], a["r"][1], a["e"][0], a["e"][1], a["pct"]))
    if f["notice"]:
        n = f["notice"]
        print("notice %s: %dx%d, %d%% of the %dx%d eye texture" % (n["ts"] or "?", n["r"][0], n["r"][1], n["pct"], n["e"][0], n["e"][1]))
    verdict = vr_supersampling_verdict(f)
    for tag, status, text_ in verdict:
        print("%s (%s) %s" % (status, tag, text_))
    counts = {"PASS": 0, "WARN": 0, "STOP": 0, "n/a": 0}
    for _, status, _ in verdict:
        counts[status] = counts.get(status, 0) + 1
    worst = "STOP" if counts["STOP"] else ("WARN" if counts["WARN"] else ("PASS" if counts["PASS"] else "n/a"))
    print("vr supersampling verdict: %s (%d PASS, %d WARN, %d STOP, %d n/a)" % (worst, counts["PASS"], counts["WARN"], counts["STOP"], counts["n/a"]))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Locate and read EDVR flight logs.")
    ap.add_argument("--target", default="steam",
                    help="steam, frontier, or a path to the game directory")
    ap.add_argument("--dir", default=None,
                    help="read this log directory directly, ignoring --target")
    ap.add_argument("--file", default=None, help="read exactly this log file")
    ap.add_argument("--tag", default="gfx",
                    help="gfx (d3d11), vr (legacy OpenVR proxy, retired 2026-09-16), "
                         "openxr (native OpenXR), or all")
    ap.add_argument("--nth", type=int, default=0,
                    help="0 is the newest log, 1 the one before it")
    ap.add_argument("--list", action="store_true",
                    help="list the logs found and stop")
    ap.add_argument("--version", action="store_true",
                    help="print the log's version line and stop")
    ap.add_argument("--expect-build", default=None,
                    help="a git ref (HEAD) or literal version; exit 2 if the "
                         "log was not written by that build")
    ap.add_argument("--grep", default=None,
                    help="print only lines matching this regular expression")
    ap.add_argument("--tail", type=int, default=None,
                    help="print only the last N lines (after --grep)")
    ap.add_argument("--tally", choices=["vh", "periodic"], default=None,
                    help="aggregate instead of dumping: vh counts eye-texture "
                         "DC lines per vh= hash, split by r= render-target "
                         "token, with the DC frame summaries as totals; "
                         "periodic lays the `periodic work:` timing against "
                         "the flight's long frames (graphics log plus its "
                         "runtime log)")
    ap.add_argument("--frame", type=int, default=None,
                    help="with --tally vh, restrict to this census frame "
                         "ordinal; frames past the census line cap have no "
                         "per-draw lines and are reported as summaries")
    ap.add_argument("--camera-census", action="store_true",
                    help="report a VR camera census flight (advanced.vr_camera_census "
                         "= on): the 5 s lines, the cameras and their roles read "
                         "from the kind of each call, each logged frame's "
                         "call sequence reduced to runs, the eye draws' rows, the "
                         "offline join of those rows to the calls' -- which "
                         "camera is an eye's, and which of its caller, signature, "
                         "place in the frame, tangents and view tell it from the "
                         "world's -- the episodes (a sampled frame per trigger, "
                         "aboard or on foot: its calls, its join to the b1 rows, "
                         "the pass's rows), the on-foot naming runs and the "
                         "detour's CPU, and the stage 2 verdict (PASS / WARN / "
                         "STOP) on the world route's injected phase")
    ap.add_argument("--maps-sharp", action="store_true",
                    help="report an on-foot maps gate flight (experimental.on_foot_maps_sharp "
                         "= on): every map or menu the layer took and handed back "
                         "(when, how long, how many eyes skipped the upscaler), the 5 s "
                         "counters summed, whether the VR world route let go and "
                         "re-owned with the gate, and a PASS / WARN / STOP verdict")
    ap.add_argument("--vscreen-fit", action="store_true",
                    help="report a fix.vscreen_res_width = auto flight: the `vScreen "
                         "resolution:` rule line (fitted or legacy, and why) and the "
                         "width applied, the footprint instrument's arming line and "
                         "its 30 s lines (the on-foot screen's width in eye pixels, "
                         "its stability, shape and 1/d law, against Sean's 3504 "
                         "calibration point) and what is stored for the next launch; "
                         "PASS / WARN / STOP lines")
    ap.add_argument("--flat-upscale", action="store_true",
                    help="report a flat-profile flight (design doc section 83): the final copy admitted by "
                         "structure, so DLSS, FSR and TAA resolve below the output whatever the post chain; the "
                         "key, routes, first admission, declines, 5 s windows, stand-down and F8 warning lines, "
                         "and KEY / ADMISSION / TREATED / UPSCALE / TONE REFUSALS / STAND-DOWN / F8 WARNING / "
                         "CHAIN / ADVICE PASS / WARN / STOP lines")
    ap.add_argument("--vr-supersampling", action="store_true",
                    help="report a VR flight's Elite-supersampling-below-1 notice (design doc section 83): "
                         "the `vr supersampling:` line from the measured render size, vScreen's adoption line "
                         "and the headset notice; NOTICE / CONSISTENT / HEADSET / FLAT lines")
    ap.add_argument("--window-ms", type=float, default=100.0,
                    help="with --tally periodic, a long frame coincides with a "
                         "periodic event when the event's end time is inside "
                         "the frame or within this many ms of it (default 100)")
    ap.add_argument("--runtime-file", default=None,
                    help="with --tally periodic, read exactly this runtime "
                         "(edvr_openxr_*.log) log instead of pairing the one "
                         "that opened nearest the graphics log")
    ap.add_argument("--infer-runs", action="store_true",
                    help="with --tally periodic, also place the runs nobody "
                         "logged where a fixed cadence says they ran (only for "
                         "an operation whose schedule two logged times "
                         "confirm); they are marked est, and the default counts "
                         "logged times only")
    ap.add_argument("--root", default=None,
                    help="repository to resolve --expect-build against")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against itself and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.tally == "periodic":
        if not args.file and args.tag.lower() != "gfx":
            print("[edvr] --tally periodic reads the graphics log (--tag gfx) "
                  "and pairs its runtime log itself; drop --tag %s." % args.tag)
            return 1
        if args.window_ms < 0:
            print("[edvr] --window-ms cannot be negative.")
            return 1
        if args.frame is not None:
            print("[edvr] --frame belongs to --tally vh; ignored here.")

    native_dirs = None
    if args.file:
        path = os.path.abspath(args.file)
        if not os.path.isfile(path):
            print("[edvr] no such log: %s" % path)
            return 1
        logs = [("", "", path)]
        native_dirs = [os.path.dirname(path)]
    else:
        if args.dir:
            directories = [args.dir]
            native_dirs = [args.dir]
        else:
            game_dir = resolve_target(args.target)
            directories = log_dirs_for(game_dir, args.tag.lower())
            native_dirs = log_dirs_for(game_dir, "openxr")
        logs = []
        for directory in directories:
            logs.extend(find_logs(directory, None if args.tag == "all" else args.tag))
        logs.sort(reverse=True)
        if not logs:
            print("[edvr] no edvr_%s_*.log in %s"
                  % (args.tag, ", ".join(directories)))
            return 1
        if args.list:
            print("[edvr] %d log(s) in %s:" % (len(logs), ", ".join(directories)))
            for stamp, tag, p in logs:
                print("       %-4s %s  %s"
                      % (tag, stamp, os.path.basename(p)))
            return 0
        if args.nth >= len(logs):
            print("[edvr] only %d log(s); --nth %d is past the end"
                  % (len(logs), args.nth))
            return 1
        logs = [logs[args.nth]]

    path = logs[0][2]
    text = read_text(path)
    print("[edvr] %s  (%d lines, %.1f KB)"
          % (path, text.count("\n") + 1, len(text) / 1024.0))

    line, ver, stamp = version_line(text)
    if line:
        print("[edvr] %s" % line)
    else:
        print("[edvr] WARNING: this log has no version line -- it may be "
              "truncated, or not an EDVR log.")

    want = None
    if args.expect_build:
        want = expected_version(args.expect_build,
                                os.path.abspath(args.root) if args.root
                                else repo_root())
        if version_matches(ver, want):
            print("[edvr] build matches: %s" % want)
        else:
            print("[edvr] BUILD MISMATCH\n"
                  "       log says   %s\n"
                  "       expected   %s\n"
                  "       This flight is not evidence about that build. "
                  "Reinstall and fly again."
                  % (ver or "(nothing)", want))
            return 2

    if args.version:
        return 0

    if args.vscreen_fit:
        return print_vscreen_fit(text)
    if args.flat_upscale:
        return print_flat_upscale(text)
    if args.vr_supersampling:
        return print_vr_supersampling(text)
    if args.camera_census:
        return print_camera_census(text)
    if args.maps_sharp:
        return print_maps_sharp(text)
    if args.tally == "periodic":
        return print_periodic_report(path, text, ver, want, args, native_dirs)
    if args.tally:
        return print_vh_tally(text, args.frame)

    lines = text.splitlines()
    if args.grep:
        try:
            rx = re.compile(args.grep)
        except re.error as e:
            print("[edvr] bad --grep pattern: %s" % e)
            return 1
        lines = [l for l in lines if rx.search(l)]
        print("[edvr] %d line(s) match %s" % (len(lines), args.grep))
    if args.tail is not None:
        lines = lines[-args.tail:]
    if args.grep or args.tail is not None:
        for l in lines:
            print(l)
    return 0


def self_test():
    ok = True

    for name, want in (("edvr_gfx_20260910_042313.log", "gfx"),
                       ("edvr_vr_20260910_042313.log", "vr"),
                       ("edvr_openxr_20260914_042313_007_1234.log", "openxr"),
                       ("notalog.txt", None)):
        m = LOG_RE.match(name)
        got = m.group("tag") if m else None
        if got != want:
            print("LOG_RE on %s -> %r, want %r" % (name, got, want))
            ok = False

    # The exact line log.cpp writes, long form and short.
    long_form = ("[00:00:00.001] version 0.14.1-93-gf78eba4 (build 68C0A1F2) "
                 "-- this DLL was linked 2026-09-09 20:34:39 UTC")
    _, ver, stamp = version_line("first line\n" + long_form + "\nmore\n")
    if ver != "0.14.1-93-gf78eba4" or stamp != "68C0A1F2":
        print("version_line long form -> %r %r" % (ver, stamp))
        ok = False
    _, ver2, stamp2 = version_line("[00:00:00.001] version unversioned test "
                                   "build\n")
    if ver2 != "unversioned":
        print("version_line short form -> %r" % ver2)
        ok = False
    # And a sentence that merely contains the word is not the version note.
    _, ver3, _ = version_line("[00:00:01.500] the version of the shader is 3\n")
    if ver3 is not None:
        print("version_line matched prose: %r" % ver3)
        ok = False

    native = ("2026-09-13 14:01:42.659 UTC pid=1234 tid=5678 "
              "module_init,version=v0.16.2-77-gab80a6c-dirty,durable_log=1")
    native2 = native.replace("14:01:42.659", "14:01:43.001")
    _, nver, nstamp = version_line("noise version=v0.0.0\n" + native + "\n" + native2 + "\n")
    if nver != "v0.16.2-77-gab80a6c-dirty" or nstamp is not None:
        print("native version_line -> %r %r" % (nver, nstamp)); ok = False
    for label in ("v0.16.2", "ab80a6c", "ab80a6c-dirty"):
        if version_line(native.replace("v0.16.2-77-gab80a6c-dirty", label))[1] != label:
            print("native version label rejected: %r" % label); ok = False
    for bad in ("native module_init,version=v0.16.2-77-gab80a6c,durable_log=1",
                native.replace("durable_log=1", "durable_log=0"),
                native.replace("UTC", "LOCAL"),
                "[14:01:42.659] module_init,version=v0.16.2-77-gab80a6c,durable_log=1"):
        if version_line(bad)[1] is not None:
            print("native false positive: %r" % bad); ok = False

    # Matching has to tolerate the suffixes `git describe` adds, and must
    # still refuse a genuinely different commit -- the whole point.
    # HEAD asks about the working tree; a named ref must not, because
    # git refuses the combination outright.
    if "--dirty" not in describe_cmd("HEAD", "."):
        print("describe_cmd(HEAD) does not ask about a dirty tree")
        ok = False
    named = describe_cmd("v0.14.0", ".")
    if "--dirty" in named or named[-1] != "v0.14.0":
        print("describe_cmd(v0.14.0) -> %r" % named)
        ok = False

    for a, e, want in (
            ("0.14.1-93-gf78eba4", "0.14.1-93-gf78eba4", True),
            ("0.14.1-93-gf78eba4", "0.14.1-93-gf78eba4-dirty", True),
            ("0.14.1-93-gf78eba4", "0.14.1-40-gc468661", False),
            ("v0.14.0", "v0.14.0", True),
            (None, "v0.14.0", False)):
        if version_matches(a, e) != want:
            print("version_matches(%r, %r) != %s" % (a, e, want))
            ok = False

    tmp = tempfile.mkdtemp(prefix="edvr_log_test_")
    try:
        game = os.path.join(tmp, "game")
        logs = os.path.join(game, "edvr_logs")
        os.makedirs(logs)
        for stamp_s in ("20260910_040000", "20260910_050000"):
            with open(os.path.join(logs, "edvr_gfx_%s.log" % stamp_s),
                      "wb") as f:
                # With the timestamp prefix Log::note() really writes: a
                # fixture without it is what hid a regex that matched
                # nothing in the field.
                f.write(("[00:00:00.001] version 0.14.1-93-gf78eba4 "
                         "(build 68C0A1F2) -- this DLL was linked "
                         "2026-09-09 20:34:39 UTC\n"
                         "[00:00:12.400] Stats[40] ships 3\n"
                         "[00:00:12.400] Stats[41] ships 0\n"
                         "[00:00:12.401] something else\n").encode("utf-8"))
        with open(os.path.join(logs, "edvr_vr_20260910_060000.log"),
                  "wb") as f:
            f.write(b"[00:00:00.001] version 0.14.1-93-gf78eba4 "
                    b"(build 68C0A1F2)\n")
        with open(os.path.join(logs, "edvr_openxr_20260910_060001_123_77.log"),
                  "wb") as f:
            f.write(b"2026-09-13 14:01:42.659 UTC pid=1234 tid=5678 "
                    b"module_init,version=v0.16.2-77-gab80a6c,durable_log=1\n")

        # The default log directory, and the ini's override, both found.
        if log_dir_for(game) != logs:
            print("log_dir_for did not default to edvr_logs")
            ok = False
        other = os.path.join(tmp, "elsewhere")
        os.makedirs(other)
        with open(os.path.join(game, "edvr.ini"), "wb") as f:
            f.write(("[log]\ndir = %s\n" % other).encode("utf-8"))
        if log_dir_for(game) != other:
            print("log_dir_for ignored log.dir in the ini")
            ok = False
        if log_dirs_for(game, "openxr") != [logs] or log_dirs_for(game, "all") != [other, logs]:
            print("native and redirected legacy discovery did not remain independent")
            ok = False
        os.remove(os.path.join(game, "edvr.ini"))

        # Native discovery remains beside the executable even if legacy logs
        # are redirected by log.dir, and --all combines both directories.
        if len(log_dirs_for(game, "openxr")) != 1 or \
                log_dirs_for(game, "openxr")[0] != logs:
            print("native discovery did not use executable edvr_logs")
            ok = False
        if main(["--dir", logs, "--tag", "openxr", "--version"]) != 0:
            print("native --version discovery failed")
            ok = False

        # Newest first, and the tag filter separates the two halves.
        found = find_logs(logs, "gfx")
        if len(found) != 2 or not found[0][2].endswith("050000.log"):
            print("find_logs did not return the newest gfx log first: %r"
                  % [os.path.basename(p) for _, _, p in found])
            ok = False
        if len(find_logs(logs, "vr")) != 1:
            print("find_logs tag filter is wrong")
            ok = False
        if len(find_logs(logs)) != 4:
            print("find_logs with no tag did not return all four")
            ok = False
        if len(find_logs(logs, "openxr")) != 1:
            print("find_logs native OpenXR tag filter is wrong")
            ok = False
        # A legacy log opened in the same second as another: log.cpp gives it
        # the suffix, and it is a log like any other, later than the plain
        # name it lost the second to.
        same_second = os.path.join(logs, "edvr_gfx_20260910_040000_001_1.log")
        with open(same_second, "wb") as f:
            f.write(b"a second process in the same second\n")
        found = [os.path.basename(p) for _, _, p in find_logs(logs, "gfx")]
        if found != ["edvr_gfx_20260910_050000.log", "edvr_gfx_20260910_040000_001_1.log",
                     "edvr_gfx_20260910_040000.log"]:
            print("a suffixed legacy log is not found, or sorts wrong: %r" % found)
            ok = False
        os.remove(same_second)
        with open(os.path.join(logs, "edvr_openxr_20260910_060002.log"), "wb") as f:
            f.write(b"an old native log with a UTC name\n")
        if len(find_logs(logs, "openxr")) != 1:
            print("an unsuffixed native name was accepted")
            ok = False
        os.remove(os.path.join(logs, "edvr_openxr_20260910_060002.log"))
        # Distinct native Init attempts within one second sort by milliseconds.
        newer_native = os.path.join(logs, "edvr_openxr_20260910_060001_999_2.log")
        with open(newer_native, "wb") as f:
            f.write(b"native later in the same second\n")
        if find_logs(logs, "openxr")[0][2] != newer_native:
            print("native millisecond ordering is wrong")
            ok = False
        os.remove(newer_native)

        newest = os.path.join(logs, "edvr_gfx_20260910_050000.log")
        if main(["--file", newest, "--version"]) != 0:
            print("--version on a good log did not exit 0")
            ok = False
        # The exit code a caller keys off: 2, distinct from 1.
        rc = main(["--file", newest, "--expect-build",
                   "0.14.1-40-gc468661", "--version"])
        if rc != 2:
            print("a build mismatch exited %d, want 2" % rc)
            ok = False
        rc = main(["--file", newest, "--expect-build",
                   "0.14.1-93-gf78eba4", "--version"])
        if rc != 0:
            print("a matching build exited %d, want 0" % rc)
            ok = False
        if main(["--dir", logs, "--tag", "gfx", "--grep", r"Stats\[4[01]\]"]) \
                != 0:
            print("--grep exited nonzero on a readable log")
            ok = False
        if main(["--file", os.path.join(logs, "nope.log")]) != 1:
            print("a missing log did not exit 1")
            ok = False

        # --tally vh: the fixture feeds the parser the way a located log
        # does, through main() on a directory the tool discovers on its
        # own. Frame 2 carries only a DC frame summary -- the shape a
        # truncated census leaves behind.
        import contextlib
        import io

        census_dir = os.path.join(tmp, "census_logs")
        os.makedirs(census_dir)
        census_lines = [
            "[00:00:00.001] version 0.14.1-93-gf78eba4 (build 68C0A1F2)\n",
            "[00:00:01.000] DC begin census=1 frames=3 frame=100 offscreen=yes\n",
            "[00:00:01.001] DC 0 #0 X n=10 i=1 r=@10 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=AAAAAAAAAAAAAAAA vb=@2 sd=8 of=0 tp=4 ia=0,0,0 "
            "ib=@3 x=-,-,-,- q=0\n",
            "[00:00:01.002] DC 0 #1 X n=20 i=1 r=@10 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=aaaaaaaaaaaaaaaa vb=@2 ia=0,0,0 ib=@3 q=1\n",
            "[00:00:01.003] DC 0 #2 X n=60 i=1 r=@11 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=AAAAAAAAAAAAAAAA vb=@2 ia=0,0,0 ib=@3 q=2\n",
            "[00:00:01.004] DC 0 #3 X n=30 i=2 r=@11 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=BBBBBBBBBBBBBBBB vb=@2 ia=0,0,0 ib=@3 q=3\n",
            "[00:00:01.005] DC 0 #4 X n=40 i=4 r=@12 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=BBBBBBBBBBBBBBBB vb=@2 ia=0,0,0 ib=@3 q=4\n",
            # A draw whose IA tail was skipped under budget pressure has
            # no vh=: it tallies, in its own bucket.
            "[00:00:01.006] DC 0 #5 X n=50 i=1 r=- d=@50 c=- s=-,-,-,- q=5\n",
            "[00:00:01.007] DCO 0 #0 X n=5 i=1 r=- d=@51 c=- s=-,-,-,- "
            "vs=@4 vh=CCCCCCCCCCCCCCCC vb=@5 ia=0,0,0 ib=@6 q=6\n",
            "[00:00:01.008] DCO 0 #1 X n=6 i=1 r=@20 d=@51 c=- s=-,-,-,- "
            "vs=@4 vh=CCCCCCCCCCCCCCCC vb=@5 ia=0,0,0 ib=@6 q=7\n",
            "[00:00:01.009] DC frame 0 draws=8 off=2 copies=7 disp=9 "
            "clears=3 unseen=0\n",
            "[00:00:01.010] DC 1 #0 X n=8 i=1 r=@10 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=DDDDDDDDDDDDDDDD vb=@2 ia=0,0,0 ib=@3 q=8\n",
            "[00:00:01.011] DC 1 #1 X n=2 i=1 r=@30 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=DDDDDDDDDDDDDDDD vb=@2 ia=0,0,0 ib=@3 q=9\n",
            "[00:00:01.012] DC frame 1 draws=2 off=0 copies=0 disp=0 "
            "clears=0 unseen=0\n",
            "[00:00:01.013] DC frame 2 draws=9 off=1 copies=0 disp=0 "
            "clears=1 unseen=0\n",
            "[00:00:01.014] DC end census=1 draws=19 off=3 copies=7 disp=9 "
            "clears=4 unseen=0 lines=16384 interned=2048 overflow=0 "
            "truncated=12\n",
        ]
        census_log = os.path.join(census_dir, "edvr_gfx_20260910_070000.log")
        with open(census_log, "wb") as f:
            f.write("".join(census_lines).encode("utf-8"))

        # Parser-level: counting order, percent base, per-eye split by
        # r=, DCO separation, lowercase hash folding, the no-vh bucket.
        draws, summaries, cap, dropped = parse_census("".join(census_lines))
        if len(draws) != 10 or set(summaries) != {0, 1, 2} \
                or cap != 16384 or dropped != 12:
            print("parse_census -> %d draws, frames %r, cap %r, dropped %r"
                  % (len(draws), sorted(summaries), cap, dropped))
            ok = False
        rows, eye_total, off_total = tally_vh(draws)
        if eye_total != 8 or off_total != 2:
            print("tally_vh totals -> eye %d off %d, want 8/2"
                  % (eye_total, off_total))
            ok = False
        if [r["vh"] for r in rows] != ["AAAAAAAAAAAAAAAA", "BBBBBBBBBBBBBBBB",
                                       "DDDDDDDDDDDDDDDD", None]:
            print("tally_vh order/vh -> %r" % [r["vh"] for r in rows])
            ok = False
        if rows[0]["count"] != 3 or rows[0]["sub"] != {"@10": 2, "@11": 1} \
                or rows[0]["avg_n"] != 30.0 or rows[0]["avg_i"] != 1.0:
            print("tally_vh row A -> %r" % rows[0])
            ok = False
        if rows[1]["count"] != 2 or rows[1]["sub"] != {"@11": 1, "@12": 1} \
                or rows[1]["avg_n"] != 35.0 or rows[1]["avg_i"] != 3.0:
            print("tally_vh row B -> %r" % rows[1])
            ok = False
        if any(r["vh"] == "CCCCCCCCCCCCCCCC" for r in rows):
            print("an offscreen DCO hash leaked into the eye table")
            ok = False

        # Frame restriction keeps only that frame's lines.
        rows1, eye1, _ = tally_vh(draws, frame=1)
        if eye1 != 2 or len(rows1) != 1 or rows1[0]["vh"] != "DDDDDDDDDDDDDDDD" \
                or rows1[0]["sub"] != {"@10": 1, "@30": 1}:
            print("tally_vh frame=1 -> %r eye %d" % (rows1, eye1))
            ok = False
        # A frame with a summary but no lines -- the truncated-census
        # shape -- tallies empty instead of guessing.
        rows2, eye2, _ = tally_vh(draws, frame=2)
        if rows2 or eye2 != 0:
            print("tally_vh frame=2 -> %r eye %d" % (rows2, eye2))
            ok = False

        def run_census_tally(argv):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = main(argv)
            return rc, buf.getvalue()

        rc, out = run_census_tally(["--dir", census_dir, "--tally", "vh",
                                    "--frame", "0"])
        if rc != 0 or "6 eye-texture DC lines, 2 offscreen DCO lines" not in out:
            print("--tally vh --frame 0 rc=%d header missing:\n%s" % (rc, out))
            ok = False
        for want in ("50.0%", "AAAAAAAAAAAAAAAA", "BBBBBBBBBBBBBBBB",
                     "@10:2", "frame 0 summary: draws=8 off=2 copies=7 "
                     "disp=9 clears=3 unseen=0"):
            if want not in out:
                print("--tally vh --frame 0 output lacks %r:\n%s" % (want, out))
                ok = False
        rc, out = run_census_tally(["--dir", census_dir, "--tally", "vh",
                                    "--frame", "2"])
        if rc != 0 or "only its summary survived" not in out \
                or "frame 2 summary: draws=9" not in out:
            print("--tally vh --frame 2 did not report the summary-only "
                  "frame (rc=%d):\n%s" % (rc, out))
            ok = False
        rc, out = run_census_tally(["--dir", census_dir, "--tally", "vh"])
        if rc != 0 or "truncated: 12 line(s)" not in out \
                or "2 summary-only" not in out:
            print("--tally vh did not report truncation (rc=%d):\n%s"
                  % (rc, out))
            ok = False
        rc, out = run_census_tally(["--dir", census_dir, "--tally", "vh",
                                    "--frame", "9"])
        if rc != 0 or "appears in no census line or summary" not in out:
            print("--tally vh --frame 9 rc=%d:\n%s" % (rc, out))
            ok = False
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    if not self_test_periodic():
        ok = False
    if not self_test_camera_census():
        ok = False
    if not self_test_maps_sharp():
        ok = False
    if not self_test_vscreen_fit():
        ok = False
    if not self_test_flat_upscale():
        ok = False

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


FLATU_FIXTURE = "flat_upscale_fixture.log"


def self_test_flat_upscale():
    """--flat-upscale on the checked-in synthetic flight (tools\\flat_upscale_fixture.log, which tools\\flat_temporal_test holds to exactly what the DLL's
    formatters write): a good flight below the output, then each episode appended to it, then logs altered to take away each thing the report
    depends on and to break each thing the verdict judges; and --vr-supersampling on lines built from the header's own text. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("flat upscale: %s" % msg)
        ok = False

    def report(text, fn=None):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = (fn or print_flat_upscale)(text)
        return rc, buf.getvalue()

    def statuses(text, fn=None):
        """{tag: status} of the verdict lines of a log, and the whole report."""
        _, out = report(text, fn)
        found = {}
        for line in out.splitlines():
            m = re.match(r"^(PASS|WARN|STOP|n/a) \(([A-Z0-9 -]+)\) ", line)
            if m:
                found[m.group(2)] = m.group(1)
        return found, out

    def sub(text, old, new, count=-1):
        """text with `old` replaced by `new`; fails the test (and returns text) when `old` is not there: a mutation that changes nothing would pass
        every check for the wrong reason."""
        if old not in text:
            fail("the mutation %r -> %r found nothing to change" % (old, new))
            return text
        return text.replace(old, new, count)

    def want_statuses(text, want, label):
        got, out = statuses(text)
        for tag, status in want.items():
            if got.get(tag) != status:
                fail("%s: %s is %r, wanted %r:\n%s" % (label, tag, got.get(tag), status, out))

    here = os.path.dirname(os.path.abspath(__file__))
    fixture = os.path.join(here, FLATU_FIXTURE)
    if not os.path.isfile(fixture):
        fail("the fixture %s is missing beside this script" % FLATU_FIXTURE)
        return False
    whole = read_text(fixture)
    segments, current = {"base": []}, "base"
    for line in whole.splitlines():
        m = re.match(r"^# episode: (.+)$", line)
        if m:
            current = m.group(1)
            segments[current] = []
        else:
            segments[current].append(line)
    base = "\n".join(segments["base"]) + "\n"
    if sorted(segments) != ["base", "game-aa", "old-advice", "render-size"]:
        fail("the fixture's segments are %r" % sorted(segments))
        return False

    def with_episode(name):
        return base + "\n".join(segments[name]) + "\n"

    # ---- the parser, on the good flight ----
    f = parse_flat_upscale(base)
    if len(f["keys"]) != 1 or f["keys"][0]["key"] != "auto" or f["keys"][0]["when"] != "read at startup" or len(f["windows"]) != 5 or len(f["routes"]) != 1 or \
            f["routes"][0]["r"] != (2880, 1620) or f["routes"][0]["d"] != (3840, 2160) or f["routes"][0]["name"] != "trained-upscale" or len(f["runtime"]) != 5:
        fail("the fixture's key, windows, route and runtime lines parsed as %r" % ({k: (v if k in ("keys", "routes") else len(v) if isinstance(v, list) else v) for k, v in f.items()},))
    fi = f["first"]
    if not fi or (fi["frame"], fi["src"], fi["scene"], fi["output"], fi["whitelist"], fi["route"], fi["menu"]) != \
            (1919, (2880, 1620), (2880, 1620), (3840, 2160), "no-known-tone-pass", "trained-upscale", False):
        fail("the first admission parsed as %r" % (fi,))
    stand = [(s["what"], s["reason"]) for s in f["stand"]]
    if stand != [("entered", "no-3d-scene"), ("resumed", None)] or f["warns"] or f["refusals"] != {"no-3d-scene": 1482} or f["old_advice"]:
        fail("the stand-down, warning and refusal lines parsed as %r %r %r" % (stand, f["warns"], f["refusals"]))
    w = flat_upscale_windows(f)
    if len(w) != 5 or (w[0]["copies"], w[0]["no_scene"], w[0]["admitted"], w[0]["last"], w[0]["scene"], w[0]["output"]) != (741, 741, 0, "no-scene", None, (3840, 2160)) or \
            (w[2]["admitted"], w[2]["copies"], w[2]["scene"], w[2]["source"], w[2]["ldr_before_max"], w[2]["last"]) != (331, 331, (2880, 1620), (2880, 1620), 0, "admitted"):
        fail("the fixture's windows parsed as %r" % (w,))
    # A line cut short or garbled is skipped, never fatal.
    g = parse_flat_upscale("flat copy structure 5s: key=auto copies=x\nflat stand-down: entered at frame=zz\nflat route: trained-upscale R=\n"
                           "flat settings warning: shown (mode=\nflat runtime: treated=\nnothing at all\n")
    if g["first"] is not None or g["routes"] or g["runtime"] or len(g["windows"]) != 1:
        fail("a cut-short line was mis-parsed: %r" % (g,))

    # ---- the report on the good flight: every question PASSes ----
    rc, out = report(base)
    flat = re.sub(r"[ ]+", " ", out)
    for want in (
            "[edvr] flat upscale: 1 key line(s), 5 copy-structure window(s), 1 route line(s), 2 stand-down line(s), 0 warning line(s), 0 decline line(s)",
            "key 15:12:02.151: experimental.temporal_aa_before_post=auto (read at startup)",
            "route 15:12:18.913: trained-upscale R=2880x1620 E=3840x2160 D=3840x2160",
            "first admission 15:12:18.914 at frame 1919: a 2880x1620 image, scene 2880x1620 on a 3840x2160 output, route trained-upscale; the whitelist said no-known-tone-pass",
            "window 15:12:21.149: key=auto copies 331 (whitelist 0, admitted 331, declined 0, selector-refused 0, no-3d-scene 0, render-size 0, route-serves 0, key-off 0); last admitted; "
            "scene 2880x1620 output 3840x2160 source 2880x1620; longest chain 0",
            "stand-down 15:12:08.368: entered for no-3d-scene",
            "stand-down 15:12:18.903: resumed",
            "PASS (KEY) experimental.temporal_aa_before_post=auto (read at startup)",
            "PASS (ADMISSION) 5 window(s): copies 1753, whitelist 0, admitted 1009, declined 0, selector-refused 0, no-3d-scene 744, render-size 0, route-serves 0, key-off 0; first admission at frame 1919",
            "PASS (TREATED) 1009 frame(s) treated over the log (the counter went 0 -> 1009)",
            "PASS (UPSCALE) below the output (trained-upscale R=2880x1620 D=3840x2160): 1009 frame(s) admitted by structure, treated",
            "PASS (TONE REFUSALS) no frame was refused for a tone pass",
            "PASS (STAND-DOWN) 1 stand-down(s): 15:12:08.368: no-3d-scene",
            "PASS (F8 WARNING) the panel showed no warning",
            "PASS (CHAIN) no R-sized image pass between the scene HDR's first consumer and the copy (the longest chain seen: 0)",
            "PASS (ADVICE) the supersampling advice is gone (below 1.0 is supported)",
            "flat upscale verdict: PASS (9 PASS, 0 WARN, 0 STOP, 0 n/a)"):
        if want not in flat:
            fail("the good flight's report lacks %r:\n%s" % (want, out))
    if rc != 0:
        fail("the good flight reported exit %d" % rc)

    # ---- the episodes ----
    want_statuses(with_episode("render-size"), {"KEY": "PASS", "ADMISSION": "PASS", "TREATED": "PASS", "STAND-DOWN": "WARN", "F8 WARNING": "WARN", "CHAIN": "PASS", "ADVICE": "PASS"},
                  "a render size that does not fit")
    _, out = statuses(with_episode("render-size"))
    flat = re.sub(r"[ ]+", " ", out)
    for want in ("stand-down 15:14:28.335: entered for render-size-does-not-fit-output (Elite renders 2176x1224 on a 2560x1600 screen)",
                 "F8 warning 15:14:28.335: shown for render-size-does-not-fit-output: DLSS is not active: Elite renders 2176x1224 on a 2560x1600 screen. | Set Elite's resolution to your screen's",
                 "WARN (F8 WARNING) 1 warning(s): 15:14:28.335 render-size-does-not-fit-output (the render size)",
                 "flat upscale verdict: WARN"):
        if want not in flat:
            fail("the render-size episode's report lacks %r:\n%s" % (want, out))
    want_statuses(with_episode("game-aa"), {"TONE REFUSALS": "WARN", "STAND-DOWN": "WARN", "F8 WARNING": "WARN", "CHAIN": "WARN", "ADMISSION": "PASS", "ADVICE": "PASS"}, "a game AA chain")
    _, out = statuses(with_episode("game-aa"))
    flat = re.sub(r"[ ]+", " ", out)
    for want in ("declined 15:16:40.100: r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr (the whitelist said no-known-tone-pass; a 2880x1620 image, 2 R-sized pass(es) between)",
                 "WARN (CHAIN) the structure declined 600 frame(s) with R-sized image passes", "(the longest chain: 2)"):
        if want not in flat:
            fail("the game-AA episode's report lacks %r:\n%s" % (want, out))
    # The CHAIN count is the windows' frames, not the decline lines (one line a cause a session); with no window that counted them it says lines.
    no_counts = re.sub(r"(declines=)r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr:\d+", r"\1none", with_episode("game-aa"))
    _, out = statuses(no_counts)
    if "frames (1 decline line(s); no window counted them)" not in re.sub(r"[ ]+", " ", out):
        fail("a game-AA log whose windows counted no declines should name the decline lines, not call them frames:\n%s" % out)
    want_statuses(with_episode("old-advice"), {"ADVICE": "STOP", "F8 WARNING": "STOP"}, "a build from before section 83")

    # ---- take away what the report depends on, and break what the verdict judges ----
    no_windows = "\n".join(l for l in base.splitlines() if "flat copy structure 5s:" not in l) + "\n"
    rc, out = report(no_windows)
    got, _ = statuses(no_windows)
    if got.get("ADMISSION") != "STOP" or rc != 0 or got.get("UPSCALE") != "STOP":
        fail("a flight with no admission window should say ADMISSION STOP and (nothing admitted below the output) UPSCALE STOP, exit 0: %r rc=%d" % (got, rc))
    want_statuses(sub(base, "temporal_aa_before_post=auto (read at startup)", "temporal_aa_before_post=off (read at startup)"), {"KEY": "WARN"}, "the key off")
    no_key = "\n".join(l for l in base.splitlines() if "flat hdr route:" not in l) + "\n"
    want_statuses(no_key, {"KEY": "n/a", "ADMISSION": "PASS"}, "no key line")
    # Windows that never ruled on a final copy: the admission ran and had nothing to say, which is not a PASS.
    no_copies = "\n".join(re.sub(r"(copies|whitelist|admitted|no-scene)=\d+", r"\1=0", l) if "flat copy structure 5s:" in l else l for l in base.splitlines()) + "\n"
    want_statuses(no_copies, {"ADMISSION": "WARN"}, "windows with no final copy")
    _, out = statuses(no_copies)
    if "NO final copy was ruled on in any window" not in out:
        fail("windows with copies=0 should say no final copy was ruled on:\n%s" % out)
    untreated = re.sub(r"flat runtime: treated=\d+", "flat runtime: treated=0", base)
    want_statuses(untreated, {"TREATED": "STOP", "UPSCALE": "STOP"}, "nothing treated")
    no_runtime = "\n".join(l for l in base.splitlines() if "flat runtime:" not in l) + "\n"
    want_statuses(no_runtime, {"TREATED": "n/a", "UPSCALE": "STOP"}, "no runtime lines")
    no_route = "\n".join(l for l in base.splitlines() if "flat route:" not in l) + "\n"
    want_statuses(no_route, {"UPSCALE": "n/a"}, "no route line")
    # The startup false warning: a warning shown before any scene is a STOP, whatever it says.
    startup = sub(base, "[15:12:08.368] flat stand-down: entered", "[15:12:08.360] flat settings warning: shown (mode=DLSS, frames refused for no-known-tone-pass, work stood down): DLSS is not "
                  "active: Elite's post-processing is not recognised. Turn off in Elite's graphics options: Anti-aliasing, Bloom, Depth of field\n[15:12:08.368] flat stand-down: entered", 1)
    want_statuses(startup, {"F8 WARNING": "STOP"}, "the false startup warning")
    _, out = statuses(startup)
    if "BEFORE ANY SCENE" not in out:
        fail("the false startup warning is not named:\n%s" % out)
    # A stand-down for a chain nothing recognised (no declines to explain it) is a STOP.
    chain = base + "\n".join(l for l in segments["game-aa"] if "flat stand-down:" in l or "flat runtime refusal" in l) + "\n"
    want_statuses(chain, {"STAND-DOWN": "STOP", "TONE REFUSALS": "WARN"}, "an unrecognised chain")
    hdr = base + "[15:20:00.000] flat stand-down: entered at frame=9: every frame for 5.0 s (400 frames) was refused for no-hdr-consumer, none treated; paused: x\n"
    want_statuses(hdr, {"STAND-DOWN": "STOP"}, "a missing HDR consumer")
    # Tone refusals with nothing treated are a STOP rather than a WARN.
    want_statuses(untreated + "[15:20:00.000] flat runtime refusal 5s: reason=no-known-tone-pass count=500\n", {"TONE REFUSALS": "STOP"}, "refusals with nothing treated")
    # A VR log has no flat lines: exit 1.
    rc, out = report("[10:00:00.000] version v0.18.0 (build 1)\n[10:00:01.000] vScreen: something\n")
    if rc != 1 or "no flat-profile line" not in out:
        fail("a log with no flat line should exit 1 and say so: rc=%d %r" % (rc, out))
    # This reader's copy of the header's key text: the fixture's key line is the runtime's.
    runtime_cpp = os.path.join(os.path.dirname(here), "src", "d3d11", "flat_runtime.cpp")
    if os.path.isfile(runtime_cpp):
        r = read_text(runtime_cpp)
        if "flat hdr route: experimental.temporal_aa_before_post=%s%s at frame=%llu: %s" not in r:
            fail("src\\d3d11\\flat_runtime.cpp no longer writes the key line this reader parses")
    else:
        fail("src\\d3d11\\flat_runtime.cpp is not where the self-test looks for it (%s)" % runtime_cpp)

    # ---- --vr-supersampling, from the header's own text ----
    header = os.path.join(os.path.dirname(here), "src", "common", "vr_supersample_notice.h")
    vscreen = os.path.join(os.path.dirname(here), "src", "d3d11", "vscreen.cpp")
    notice_prefix = "vr supersampling: Elite draws the 3D world at %ux%u, %u%% of the %ux%u eye texture, and scales it up before EDVR sees it: "
    adopt_prefix = "vScreen: the world on this rig is rendered at %ux%u and scaled into the %ux%u the headset is handed -- %u%% of the width"
    if os.path.isfile(header):
        if notice_prefix.replace("\\", "") not in read_text(header).replace("\"\n        \"", ""):
            fail("src\\common\\vr_supersample_notice.h's log line is not the text this reader parses")
        if kBelow := re.search(r"constexpr uint32_t kBelowPercent = (\d+);", read_text(header)):
            if int(kBelow.group(1)) != VRSS_BELOW_PERCENT:
                fail("this reader's VRSS_BELOW_PERCENT (%d) is not the header's kBelowPercent (%s)" % (VRSS_BELOW_PERCENT, kBelow.group(1)))
        else:
            fail("kBelowPercent was not found in the header")
    else:
        fail("src\\common\\vr_supersample_notice.h is not where the self-test looks for it (%s)" % header)
    if os.path.isfile(vscreen):
        if adopt_prefix.replace("%ux%u", "%ux%u") not in read_text(vscreen).replace("\"\n                \"", ""):
            fail("src\\d3d11\\vscreen.cpp's adoption line is not the text this reader parses")
    notice = ("[09:30:12.100] " + (notice_prefix % (2112, 2304, 75, 2816, 3072)) + "Elite's Supersampling is below 1 (an upscaler in the chain reads the same). EDVR's DLSS then upscales an "
              "image that is already upscaled, which softens the world and the holograms. Set Elite's Supersampling to 1 and raise HMD Image Quality instead: EDVR's DLSS upscales from that. "
              "Measured from the render sizes, not read from Elite's settings file.\n")
    adopt = ("[09:30:12.098] " + (adopt_prefix % (2112, 2304, 2816, 3072, 75)) + ", which is what supersampling away from 1.0 and every upscaler in the chain do (FSR and NIS at their \"ultra quality\" are exactly this).\n")
    queued = "[09:30:12.300] vr supersampling: the headset notice is queued as a toast (\"Elite Supersampling is below 1: use HMD Image Quality\"); the Status page shows the advice as its hint while the menu is open.\n"
    vr = "[09:29:00.000] version v0.18.0-rc.5-26-g5ec0de01 (build 5EC0DE01) -- this DLL was linked 2026-10-01 20:05:44 UTC\n" + adopt + notice + queued
    p = parse_vr_supersampling(vr)
    if not p["notice"] or p["notice"]["r"] != (2112, 2304) or p["notice"]["pct"] != 75 or p["notice"]["e"] != (2816, 3072) or not p["adopt"] or p["adopt"]["pct"] != 75 or not p["queued"] or not p["toast"] or p["flat"]:
        fail("the VR lines parsed as %r" % (p,))
    want_statuses_vr = lambda text, want, label: [fail("%s: %s is %r, wanted %r" % (label, t, statuses(text, print_vr_supersampling)[0].get(t), s))
                                                  for t, s in want.items() if statuses(text, print_vr_supersampling)[0].get(t) != s]
    want_statuses_vr(vr, {"NOTICE": "PASS", "CONSISTENT": "PASS", "HEADSET": "PASS"}, "the good VR flight")
    _, out = statuses(vr, print_vr_supersampling)
    flat = re.sub(r"[ ]+", " ", out)
    for want in ("adoption 09:30:12.098: the world is drawn at 2112x2304 and scaled into the 2816x3072 the headset is handed (75% of the width)",
                 "notice 09:30:12.100: 2112x2304, 75% of the 2816x3072 eye texture",
                 "PASS (NOTICE) the world is drawn at 2112x2304, 75% of the 2816x3072 eye texture: Elite's Supersampling is below 1",
                 "PASS (CONSISTENT) the notice's sizes are vScreen's own adoption line's",
                 "PASS (HEADSET) the headset notice was queued as a toast (09:30:12.300)",
                 "vr supersampling verdict: PASS (3 PASS, 0 WARN, 0 STOP, 0 n/a)"):
        if want not in flat:
            fail("the VR report lacks %r:\n%s" % (want, out))
    want_statuses_vr(adopt + queued, {"NOTICE": "STOP"}, "the adoption line and no notice (the detection did not run)")
    want_statuses_vr(sub(sub(vr, "rendered at 2112x2304", "rendered at 2816x3072"), "75% of the width", "100% of the width").replace(notice, ""), {"NOTICE": "n/a"}, "a world at the eye's own size")
    # The DLL's compare is exact on both axes; the percent in the log is rounded. 2751x3000 in 2816x3072 is 97.7% (logged as 98) and 97.7%: below.
    near = vr.replace("2112x2304", "2751x3000").replace("75% of", "98% of")
    want_statuses_vr(near, {"NOTICE": "PASS", "CONSISTENT": "PASS"}, "97.7 percent, logged as 98: below, since the DLL's compare is exact")
    # One axis under the threshold and the other not is no notice (the DLL needs both).
    one_axis = vr.replace("2112x2304", "2112x3070")
    want_statuses_vr(one_axis, {"NOTICE": "STOP"}, "a notice for a world under the eye on one axis only")
    want_statuses_vr(sub(vr, "scaled into the 2816x3072", "scaled into the 2800x3072"), {"CONSISTENT": "STOP"}, "sizes that disagree")
    want_statuses_vr(vr.replace(queued, ""), {"HEADSET": "WARN"}, "no menu line")
    want_statuses_vr(vr + "[09:31:00.000] flat runtime: treated=1 refused=0 last=treated-jittered\n", {"FLAT": "STOP"}, "a flat log with the notice")
    rc, out = report("[09:00:00.000] version v0.18.0 (build 1)\n", print_vr_supersampling)
    if rc != 1 or "no `vr supersampling:`" not in out:
        fail("a log with no VR line should exit 1 and say so: rc=%d %r" % (rc, out))
    return ok


def self_test_maps_sharp():
    """--maps-sharp on the checked-in synthetic flight (tools\\maps_sharp_fixture.log, which tools\\on_foot_maps_test holds to exactly what the
    DLL's formatters write), then on logs altered to remove each thing the verdict judges. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("maps-sharp: %s" % msg)
        ok = False

    path = os.path.join(repo_root(), "tools", "maps_sharp_fixture.log")
    try:
        base = read_text(path)
    except OSError:
        fail("the fixture %s is missing" % path)
        return False
    p = parse_maps_sharp(base)
    kinds = [e["kind"] for e in p["events"]]
    if kinds != ["on", "take", "notempty", "back", "take", "back", "take", "back", "off"]:
        fail("the fixture's events read as %s" % kinds)
    if len(p["windows"]) != 7 or p["unparsed"]:
        fail("the fixture has %d windows (want 7) and %d unparsed line(s)" % (len(p["windows"]), len(p["unparsed"])))
    on = p["events"][0] if p["events"] else {}
    if (on.get("frame"), on.get("start"), on.get("journal")) != (900, "the world", "on foot"):
        fail("the ON line reads as %r" % on)
    take = p["events"][1] if len(p["events"]) > 1 else {}
    if (take.get("frame"), take.get("run"), take.get("world"), take.get("secs"), take.get("journal")) != (27844, 3, 26944, 149.9, "on foot"):
        fail("the first TAKES line reads as %r" % take)
    back = p["events"][3] if len(p["events"]) > 3 else {}
    if (back.get("frame"), back.get("frames"), back.get("secs"), back.get("only"), back.get("kept"), back.get("why")) != (
            29443, 1599, 17.8, 3190, 2, "a world camera named the screen's source for 2 frames in a row"):
        fail("the first HANDS BACK line reads as %r" % back)
    ne = p["events"][2] if len(p["events"]) > 2 else {}
    if (ne.get("eye"), ne.get("seq"), ne.get("draws"), ne.get("taken")) != (0, 31200, 3, 2):
        fail("the not-empty line reads as %r" % ne)
    w = p["windows"][3] if len(p["windows"]) > 3 else {}
    if (w.get("gate"), w.get("frames"), w.get("unnamed"), w.get("panel"), w.get("takes"), w.get("recognised"), w.get("only"), w.get("notempty")) != (
            "panel", 450, 450, 450, 900, 900, 898, 2):
        fail("the fourth window reads as %r" % w)
    if [r["kind"] for r in p["route"]] != ["released", "owns"]:
        fail("the fixture's route lines read as %s" % [r["kind"] for r in p["route"]])
    eps = maps_sharp_episodes(p)
    if len(eps) != 3 or [e["frames"] for e in eps] != [1599, 4, 900]:
        fail("the fixture's panel periods read as %s" % [e.get("frames") for e in eps])
    elif eps[0]["route_released"] is None or abs(eps[0]["route_released"]["t"] - eps[0]["take"]["t"]) > 1e-6 or \
            eps[0]["route_owns"] is None or abs((eps[0]["route_owns"]["t"] - eps[0]["end"]["t"]) - 0.144) > 1e-6 or eps[1]["route_released"] is not None:
        fail("the first panel period's route pairing: released %r, owns %r" % (eps[0]["route_released"], eps[0]["route_owns"]))

    def run(text):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = print_maps_sharp(text)
        return rc, buf.getvalue()

    # The fixture as it is: a 4-frame panel period is a flap (STOP), the not-empty eyes are a WARN, and the report says all of it.
    rc, out = run(base)
    if rc != 1 or "maps-sharp verdict: STOP" not in out or "a flap" not in out or "door-not-empty" not in out or "VR world route let go at" not in out:
        fail("the fixture's report (rc=%d) lacks its flap STOP, its not-empty WARN or its route pairing:\n%s" % (rc, out))
    if "the route owned the world again at 16:23:00.126 (144 ms after the hand-back)" not in out:
        fail("the report does not say the route owned again 144 ms after the first hand-back:\n%s" % out)

    # A clean flight: the flap, the not-empty eyes and their counters removed.
    clean_lines = []
    for raw in base.splitlines():
        if "16:23:20.000" in raw[:14] or "16:23:20.050" in raw[:14] or "the layer took the 2D screen for eye" in raw:
            continue
        raw = raw.replace("door-layer-only=898 door-not-empty=2", "door-layer-only=900 door-not-empty=0")
        raw = raw.replace("2 kept the upscaler", "0 kept the upscaler")
        clean_lines.append(raw)
    clean = "\n".join(clean_lines) + "\n"
    rc, out = run(clean)
    if rc != 0 or "maps-sharp verdict: PASS" not in out:
        fail("the clean flight does not PASS (rc=%d):\n%s" % (rc, out))

    def altered(what, text, want_rc, want_verdict, want_in_out):
        rc2, out2 = run(text)
        if rc2 != want_rc or ("maps-sharp verdict: " + want_verdict) not in out2 or want_in_out not in out2:
            fail("%s: rc=%d, wanted %d %s mentioning %r:\n%s" % (what, rc2, want_rc, want_verdict, want_in_out, out2))

    # The trap: taken composites that were never recognised.
    altered("recognised=0 with screen-takes>0", clean.replace("screen-takes=810 recognised=810", "screen-takes=810 recognised=0"), 1, "STOP",
            "none recognised")
    # A black eye is the sharpen door's own failure line, and only that.
    altered("a black eye", clean + "[16:23:41.000] native sharpen: LAYER-ONLY eye 0 (sequence 5) got NO composite from the UI layer -- the eye "
                                   "is BLACK for this frame (1 so far); the layer stands down\n", 1, "STOP", "a black eye was reported")
    # The luma probe's lines are not black-eye evidence. Real flights carry them by the dozen: a sample line every 2 s, and a
    # "first black stage is X" line at every CHANGE of the first black stage ("is none" when a black arrival gives way to a world, "is
    # game" for as long as the layer holds the screen). This test once demanded a STOP for "is game", and every real flight stopped.
    luma_real = ("[16:23:41.000] luma probe: eye=0 game=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100%\n"
                 "[16:23:41.001] luma probe: eye=0 first black stage is game (game 0.000 dlss_out 0.000 final 0.000).\n"
                 "[16:23:43.000] luma probe: eye=0 game=0.226/0.824/0% dlss_out=0.226/0.820/0% final=0.226/0.820/0%\n"
                 "[16:23:43.001] luma probe: eye=0 first black stage is none (game 0.226 dlss_out 0.226 final 0.226).\n")
    altered("the luma probe's lines outside any panel period", clean + luma_real, 0, "PASS", "(0 STOP, 0 WARN)")
    if parse_maps_sharp(clean + luma_real)["black"]:
        fail("the luma probe's transition lines were read as black eyes")
    # The same black sample inside a panel period the journal calls on foot: a map or a menu was on screen, so look.
    altered("a black final stage under an on-foot map",
            clean.replace("[16:22:50.100]", "[16:22:50.000] luma probe: eye=0 game=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100%\n"
                          "[16:22:50.100]", 1), 0, "WARN", "calls on foot")
    # The journal's reading with parentheses of its own (every arrival, before Status.json exists): the ON and TAKES lines must still
    # parse, with the whole reading as the journal (a regex that stopped at the first ")" lost both lines as "unknown").
    nested = base.replace("the journal: on foot)", "the journal: no Flags2 in Status.json (a menu, or no file yet))")
    pn = parse_maps_sharp(nested)
    jn = "no Flags2 in Status.json (a menu, or no file yet)"
    if [e["kind"] for e in pn["events"]] != kinds or pn["unparsed"] or len(pn["events"]) < 2 or \
            (pn["events"][0].get("journal"), pn["events"][1].get("journal"), pn["events"][1].get("world")) != (jn, jn, 26944):
        fail("with parentheses inside the journal's reading the events read as %s (%d unparsed), journals %r" % (
            [e["kind"] for e in pn["events"]], len(pn["unparsed"]), [e.get("journal") for e in pn["events"][:2]]))
    # The arrival with the key on from launch: the gate starts as a panel (the main menu, the load), so there is no TAKES line and the
    # period begins at the ON line. It must be a period, paired with its HANDS BACK and the route owning again, with the black luma
    # sample a note (the journal had no Flags2) and not a WARN or a STOP.
    base_lines = base.splitlines()

    def first_line(pred):
        return next(l for l in base_lines if pred(l))

    arrival = "\n".join([
        first_line(lambda l: "version v0.0.0-fixture" in l),
        first_line(lambda l: "ON at frame=" in l).replace("left it: the world (the journal: on foot).",
                                                         "left it: not the world (the journal: no Flags2 in Status.json (a menu, or no file yet))."),
        first_line(lambda l: "5s:" in l and "gate=panel" in l),
        "[16:22:50.000] luma probe: eye=0 game=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100%",
        first_line(lambda l: "HANDS BACK" in l).replace("2 kept the upscaler", "0 kept the upscaler"),
        first_line(lambda l: "vr world route: OWNS" in l),
    ]) + "\n"
    ea = maps_sharp_episodes(parse_maps_sharp(arrival))
    if len(ea) != 1 or not ea[0]["take"].get("from_on") or ea[0]["frames"] != 1599 or ea[0]["route_released"] is not None or \
            ea[0]["route_owns"] is None or (ea[0]["luma_samples"], ea[0]["luma_black"]) != (1, 1):
        fail("the arrival period (a gate that starts as a panel) reads as %r" % (ea,))
    rc, out = run(arrival)
    if rc != 0 or "maps-sharp verdict: PASS" not in out or "(0 STOP, 0 WARN)" not in out or \
            "ON frame 900, the screen already a panel (the journal: no Flags2 in Status.json (a menu, or no file yet))" not in out or \
            "black by content" not in out or "the route owned the world again" not in out:
        fail("the arrival report (rc=%d):\n%s" % (rc, out))
    # The route letting go apart from the gate.
    altered("the route apart", clean.replace("[16:22:41.915] vr world route: RELEASED", "[16:22:42.300] vr world route: RELEASED"), 0, "WARN",
            "not the same boundary")
    # The door short of its eyes without a counted reason.
    altered("the door short", clean.replace("panel-frames=450 holds=0 releases=0 screen-takes=900 recognised=900 door-layer-only=900",
                                            "panel-frames=450 holds=0 releases=0 screen-takes=900 recognised=900 door-layer-only=300"), 0, "WARN",
            "only 300 eyes through the layer-only door")
    # The key on but the gate not decided by naming.
    altered("not live frames", clean.replace("not-live-frames=0", "not-live-frames=12", 1), 0, "WARN", "not decided by naming")
    # A declined layer-only eye.
    altered("a declined door", clean + "[16:23:41.000] native temporal: layer-only declined for eye 0 (sequence 7): the UI layer is not live; the "
                                       "eye route serves this eye through the pass, as it does without the layer-only door.\n", 0, "WARN",
            "declined an eye")
    # No panel period closed.
    altered("nothing handed back", "\n".join(l for l in clean.splitlines() if "HANDS BACK" not in l) + "\n", 0, "WARN", "no panel period closed")
    # A log with no line of the feature says so, and exits 3.
    rc, out = run("[16:20:00.000] version v0.0.0 (build 1) -- this DLL was linked x\n[16:20:01.000] something else\n")
    if rc != 3 or "no 'on foot maps sharp' line" not in out:
        fail("a log with no feature line (rc=%d):\n%s" % (rc, out))
    # A line of the feature the reader does not know is reported, not dropped, and the first one is quoted so the next defect is not a hunt.
    altered("an unknown line", clean + "[16:23:41.000] on foot maps sharp: something the formatters never wrote\n", 0, "WARN",
            "the first: [16:23:41.000] on foot maps sharp: something the formatters never wrote")

    # Every reason the DLL can give for the gate stopping or standing aside (src\\d3d11\\ui_layer_math.h uiLayerNotLiveReasonFor; ui_layer.cpp mapsGate and
    # mapsLayerNotLive), in the OFF line's "(why)" and the not-live line's last words. Three carry parentheses of their own, and the OFF pattern stopped
    # at the first ")": a real flight (edvr_gfx_20261001_085519.log, fix.temporal_aa off) lost its OFF line as "unknown". The list is held to the DLL's
    # sources, so a reason that is reworded fails here and not in the ten minutes after a flight.
    reasons = ["the key went off", "screen motion is not live", "fix.ui_quality is off", "no temporal mode is on (fix.temporal_aa is off)",
               "the eye jitter is not as shipped (advanced.temporal_aa_jitter_sign or _lag is set)", "the layer stood down for the session",
               "screen motion is not live (fix.temporal_aa is off, or it stood down)", "the UI layer is not live"]
    reason_src = ""
    for rel in ("src/d3d11/ui_layer_math.h", "src/d3d11/ui_layer.cpp"):
        try:
            reason_src += read_text(os.path.join(repo_root(), *rel.split("/")))
        except OSError:
            fail("%s is missing: the reasons below cannot be held to the DLL's sources" % rel)
    for why in reasons:
        if reason_src and '"%s"' % why not in reason_src:
            fail("the reason %r is not in the DLL's sources any more: reword it here too" % why)
        pr = parse_maps_sharp(
            "[16:23:45.000] on foot maps sharp: OFF at frame=32500 (%s): the 2D screen is the world by the journal's reading or the screen's own "
            "depth again, as without the key.\n"
            "[16:23:46.000] on foot maps sharp: experimental.on_foot_maps_sharp is on but the 2D screen's gate stays the journal's and the screen's "
            "own depth, as without the key: %s.\n" % (why, why))
        got = [(e["kind"], e.get("why")) for e in pr["events"]]
        if got != [("off", why), ("notlive", why)] or pr["unparsed"]:
            fail("a gate-stopped line with the reason %r reads as %r (%d unparsed)" % (why, got, len(pr["unparsed"])))

    # The first flight (design doc 8.10: edvr_gfx_20261001_082459.log, Frontier, df9172db). A map was handed back with the route owning again;
    # later the player BOARDED his ship: the gate released and the route let go on one boundary (TAKES and RELEASED at one stamp), the game
    # stopped drawing the 2D screen (a cockpit has no screen composite), and every window after the take read gate=panel with screen-takes=0.
    # The reader of that build judged the door against PANEL FRAMES and called the cockpit six failed doors, and the journal's reading at the
    # take (stale by 1.6 s: still "on foot") turned the game's exit fade into a luma WARN. These are that log's lines; the TAKES, RELEASED and
    # ON lines are the fixture's (formatter) text restamped. The old format has no screen-draws; the new one does.
    def restamp(line, ts):
        return "[%s]%s\n" % (ts, line[line.index("]") + 1:])

    def win(ts, gate, frames, named, world, panel, takes, rel, draws=None):
        return ("[%s] on foot maps sharp 5s: key=on 5 s mode=naming gate=%s frames=%d named=%d unnamed=%d world-frames=%d panel-frames=%d holds=0 "
                "releases=%d screen-takes=%d recognised=%d door-layer-only=%d door-not-empty=0 not-live-frames=0%s\n"
                % (ts, gate, frames, named, frames - named, world, panel, rel, takes, takes, takes,
                   "" if draws is None else " screen-draws=%d" % draws))

    def flight(new_format, cockpit_draws=0):
        d = (lambda n: n) if new_format else (lambda n: None)
        cockpit = [("08:33:03.156", 428), ("08:33:08.155", 450), ("08:33:13.156", 428), ("08:33:18.159", 429), ("08:33:23.160", 364)]
        return "".join([
            "[08:24:59.501] version v0.18.0-rc.5-37-gdf9172db (build 6ABE6A99) -- this DLL was linked 2026-10-01 14:13:45 UTC\n",
            restamp(first_line(lambda l: "ON at frame=" in l), "08:31:23.144"),
            # period 1: a map, handed back clean, the route owning the world again 163 ms later
            restamp(first_line(lambda l: "the layer TAKES" in l), "08:31:23.166"),
            win("08:31:28.150", "panel", 450, 0, 0, 450, 900, 0, d(900)),
            win("08:31:33.150", "panel", 450, 0, 0, 450, 900, 0, d(900)),
            win("08:31:38.150", "panel", 450, 0, 0, 450, 900, 0, d(900)),
            restamp(first_line(lambda l: "HANDS BACK" in l).replace("2 kept the upscaler", "0 kept the upscaler"), "08:31:44.796"),
            restamp(first_line(lambda l: "vr world route: OWNS" in l), "08:31:44.959"),
            win("08:32:48.156", "world", 448, 448, 448, 0, 0, 0, d(896)),
            win("08:32:53.159", "world", 451, 451, 451, 0, 0, 0, d(902)),
            restamp(first_line(lambda l: "the layer TAKES" in l), "08:32:55.975"),
            restamp(first_line(lambda l: "vr world route: RELEASED" in l), "08:32:55.975"),
            win("08:32:58.157", "panel", 375, 211, 213, 162, 0, 1, d(426)),
            "".join(win(ts, "panel", n, 0, 0, n, 0, 0, d(cockpit_draws)) for ts, n in cockpit),
            "[08:33:22.312] luma probe: eye=0 game=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100%\n",
            "[08:33:22.312] luma probe: eye=0 first black stage is game (game 0.000 dlss_out 0.000 final 0.000).\n",
            "[08:33:25.110] native sharpen totals: treated=79454, off=0, refusals=0, invalidations=8, stood_down=0, layer_only=14114, layer_only_black=0.\n",
        ])

    rc, out = run(flight(False))
    if rc != 0 or "(0 STOP, 0 WARN)" not in out or "layer-only door (expected" in out or \
            "the layer took no 2D screen composite in this panel period's 5 whole-panel window(s)" not in out or "not the layer's picture" not in out:
        fail("the boarding flight, 5 s line without screen-draws (rc=%d): a cockpit with nothing taken must not read as a failed door or a black eye:\n%s"
             % (rc, out))
    rc, out = run(flight(True))
    if rc != 0 or "(0 STOP, 0 WARN)" not in out or "layer-only door (expected" in out or \
            "no 2D screen composite was drawn in this panel period's 5 whole-panel window(s)" not in out or \
            "drawn 0, taken 0 -- none was drawn" not in out or "the decision saw" not in out:
        fail("the boarding flight, 5 s line with screen-draws (rc=%d): the cockpit's windows say none was drawn:\n%s" % (rc, out))
    # Composites that WERE drawn while the gate gave the layer the whole window and that the layer did not take: that is the failure the old
    # reading imagined, and the new line can tell it from the cockpit.
    rc, out = run(flight(True, cockpit_draws=40))
    if rc != 0 or "maps-sharp verdict: WARN" not in out or "the layer left 40 in the game's frame" not in out:
        fail("composites drawn but not taken in a whole-panel window (rc=%d) must WARN, naming the count:\n%s" % (rc, out))
    return ok


VSCREEN_FIXTURE = "vscreen_fit_fixture.log"


def self_test_vscreen_fit():
    """--vscreen-fit on the checked-in synthetic flight (tools\\vscreen_fit_fixture.log, which tools\\vscreen_fit_test holds to
    exactly what the DLL's formatters write), then on logs altered to take away each thing the report depends on and to break each thing
    the verdict judges, and the census verdict's size read from the log. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("vscreen fit: %s" % msg)
        ok = False

    def report(text):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = print_vscreen_fit(text)
        return rc, buf.getvalue()

    def sub(text, old, new, count=-1):
        """text with `old` replaced by `new`; fails the test (and returns text) when `old` is not there: a mutation that changes
        nothing would pass every check for the wrong reason."""
        if old not in text:
            fail("the mutation %r -> %r found nothing to change" % (old, new))
            return text
        return text.replace(old, new, count)

    def statuses(text):
        """{tag: status} of the verdict lines of a log."""
        _, out = report(text)
        found = {}
        for line in out.splitlines():
            m = re.match(r"^(PASS|WARN|STOP|n/a) \(([A-Z ]+)\) ", line)
            if m:
                found[m.group(2)] = m.group(1)
        return found, out

    here = os.path.dirname(os.path.abspath(__file__))
    fixture = os.path.join(here, VSCREEN_FIXTURE)
    if not os.path.isfile(fixture):
        fail("the fixture %s is missing beside this script" % VSCREEN_FIXTURE)
        return False
    text = read_text(fixture)

    # ---- this copy of the rule's constants is the header's ----
    header = os.path.join(os.path.dirname(here), "src", "common", "vscreen_fit.h")
    if os.path.isfile(header):
        h = read_text(header)
        for name, value in (("kMultiplier", VSCREEN_M), ("kFloorWidth", VSCREEN_FLOOR), ("kLegacyMultiplier", VSCREEN_LEGACY_M),
                            ("kSeedWidthPx", VSCREEN_SEED[0]), ("kSeedDistance", VSCREEN_SEED[1]), ("kSeedEyeWidthPx", VSCREEN_SEED[2])):
            m = re.search(r"constexpr (?:double|uint32_t) %s = ([0-9.]+);" % name, h)
            if not m or abs(float(m.group(1)) - value) > 1e-9:
                fail("this reader's constant for %s (%r) is not src\\common\\vscreen_fit.h's (%r)" % (name, value, m.group(1) if m else None))
    else:
        fail("src\\common\\vscreen_fit.h is not where the self-test looks for it (%s)" % header)

    # ---- the parser ----
    f = parse_vscreen_fit(text)
    kv = f["rule"]["kv"] if f["rule"] else {}
    if not f["rule"] or f["rule"]["width"] != 3504 or kv.get("rule") != "fitted" or kv.get("source") != "seed" or kv.get("route") != "run" or \
            kv.get("eye") != "4032" or kv.get("distance") != "0.700" or kv.get("legacy") != "5040" or kv.get("footprint") != "3504" or \
            kv.get("m") != "1.00" or kv.get("floor") != "2880" or kv.get("cap") != "5040" or kv.get("clamp") != "none":
        fail("the fixture's rule line parsed as %r" % (f["rule"],))
    if not f["applied"] or (f["applied"]["w"], f["applied"]["h"], f["applied"]["sites"]) != (3504, 1971, 6) or len(f["armed"]) != 1 or \
            f["armed"][0][1] != "armed" or len(f["windows"]) != 4:
        fail("the fixture parsed as applied %r, %d arming line(s), %d window(s)" % (f["applied"], len(f["armed"]), len(f["windows"])))
    w = vscreen_fit_windows(f)
    if len(w) != 4 or (w[0]["samples"], w[0]["on_foot"], w[0]["other"], w[0]["draws"], w[0]["fp"], w[0]["other_fp"], w[0]["applied"]) != (44, 0, 44, 5280, None, 3503.0, None) or \
            (w[1]["fp"], w[1]["lo"], w[1]["hi"], w[1]["shape"], w[1]["at1"], w[1]["persisted"], w[1]["fit"], w[1]["legacy"], w[1]["eye"]) != \
            (3497.0, 3489.0, 3505.0, 1.778, 2448.0, 0.6071, 3504.0, 5040.0, (4032, 3898)) or w[3]["session_n"] != 177:
        fail("the fixture's windows parsed as %r" % (w,))
    # A line cut short or garbled is skipped, never fatal.
    g = parse_vscreen_fit("vscreen footprint 30s: window=x samples=y on-foot=\nvScreen resolution: auto = 12 wide\n"
                          "vScreen resolution: 1920x1080 -> 3504x1971 at\nnothing at all\nvscreen footprint: armed -- x\n")
    if g["rule"] is not None or g["applied"] is not None or len(g["armed"]) != 1 or len(g["windows"]) != 1:
        fail("a cut-short line was mis-parsed: %r" % (g,))

    # ---- the report on the fixture: every question PASSes, the one the log cannot answer says so ----
    rc, out = report(text)
    flat = re.sub(r"[ ]+", " ", out)
    for want in (
            "vscreen fit: the rule line, an apply line, 1 arming line(s), 4 footprint window(s)",
            "launch: auto = 3504 wide, rule=fitted source=seed route=run eye=4032 distance=0.700 footprint=3504 clamp=none legacy=5040; applied 3504x1971",
            "window 1: samples 44 (on foot 0, other 44), draws 5280, skipped 0, late 0; eye 4032x3898",
            "window 4: samples 59 (on foot 59, other 0), draws 5400, skipped 0, late 0; eye 4032x3898 distance 0.7 applied 0.7; on foot fp=3499",
            "PASS (RULE) FITTED to 3504 wide from a seed footprint of 3504 px at fix.panel_distance 0.700 on a 4032 px eye (m=1.00, floor 2880, cap 5040, "
            "clamp none); legacy would have been 5040; applied 3504x1971 at 6 site(s)",
            "PASS (INSTRUMENT) 4 window(s): 221 sample(s) (177 on foot, 44 other), 21480 composite draw(s) seen, 0 skipped, 0 late",
            "PASS (ON FOOT) 3 on-foot window(s), 177 sample(s); the screen spans 3499 px",
            "PASS (STABLE) the screen's width held",
            "PASS (SHAPE) the footprint's pixel aspect reads 1.778..1.778 against 16:9 = 1.778",
            "n/a (DISTANCE LAW) one panel distance in this log (0.70)",
            "PASS (CALIBRATION) at Sean's calibration point (a 4032 px eye at distance 0.7) the screen spans 3499 px against the 3504 he flew (99.9%): m = 1.0 reproduces his width",
            "PASS (STORED) the on-foot median is stored: 0.6075 of the eye at panel distance 1 (177 sample(s)); the next launch fits 3504 wide, the width this launch ran at (3504)",
            "vscreen fit verdict: PASS (7 PASS, 0 WARN, 0 STOP, 1 n/a)"):
        if want not in flat:
            fail("the fixture's report lacks %r:\n%s" % (want, out))
    if rc != 0:
        fail("the fixture reported exit %d" % rc)

    # ---- the rule line, taken apart ----
    legacy_line = ("[06:07:03.620] vScreen resolution: auto = 5040 wide: rule=legacy source=none route=no eye=4032 distance=0.700 legacy=5040 m=1.25 -- LEGACY: "
                   "125% of the 4032 px the runtime last rendered per eye, because the world route will not run: experimental.temporal_aa_on_foot_world is "
                   "not auto. Without the route nothing anti-aliases the on-foot world before it reaches the panel, and the extra "
                   "width does that job.")
    rule_line = next(l for l in text.splitlines() if "vScreen resolution: auto = " in l)
    legacy = sub(sub(text, rule_line, legacy_line), "-> 3504x1971", "-> 5040x2835")
    st, out = statuses(legacy)
    if st.get("RULE") != "PASS" or "LEGACY 5040 wide (125% of the 4032 px eye): the world route will not run: experimental.temporal_aa_on_foot_world is not auto" not in re.sub(r"[ ]+", " ", out):
        fail("a legacy launch did not say which condition failed: %r\n%s" % (st, out))
    if st.get("STORED") != "PASS" or "this launch's route=no (legacy), so a launch fits only once the world route will run" not in out:
        fail("a legacy launch's stored footprint did not say the next launch fits only when the route will run: %r\n%s" % (st, out))
    st, out = statuses(sub(legacy, "auto = 5040 wide", "auto = 4800 wide"))
    if st.get("RULE") != "STOP":
        fail("a legacy width that is not 125%% of the eye did not STOP: %r" % (st,))
    st, out = statuses(sub(text, "-> 3504x1971", "-> 5040x2835"))
    if st.get("RULE") != "STOP" or "the panel patch applied 5040x2835, not 3504 wide" not in out:
        fail("a width that was not the one applied did not STOP: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "auto = 3504 wide", "auto = 3600 wide"))
    if st.get("RULE") != "STOP" or "its own tokens" not in out:
        fail("a width that is not what its own tokens give did not STOP: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "footprint=3504 m=1.00", "footprint=3504 m=0.95"))
    if st.get("RULE") != "STOP":
        fail("a different m in the tokens did not change the recomputed width: %r" % (st,))
    st, out = statuses(sub(sub(text, "clamp=none", "clamp=floor nudged=yes"), "auto = 3504 wide", "auto = 3520 wide").replace("-> 3504x1971", "-> 3520x1980"))
    if st.get("RULE") != "PASS":
        fail("a nudged width within a few steps of the recomputed one was refused: %r\n%s" % (st, out))
    st, out = statuses("\n".join(l for l in text.splitlines() if "-> 3504x1971" not in l))
    if st.get("RULE") != "PASS" or "no `vScreen resolution: ... ->` apply line" not in out:
        fail("a log with no apply line did not say so: %r" % (st,))
    explicit = "[06:07:03.620] vScreen resolution: explicit 3504 wide (fix.vscreen_res_width), 3504x1971 at 16:9, used exactly: auto's rule is not in play."
    not_armed = ("[06:07:05.412] vscreen footprint: not armed -- fix.vscreen_res_width is \"3504\", not auto: an explicit width is used exactly and there is "
                 "nothing to fit.")
    ex = "\n".join([l for l in text.splitlines() if "version" in l] + [explicit, not_armed])
    st, out = statuses(ex)
    if st.get("RULE") != "n/a" or st.get("INSTRUMENT") != "n/a" or report(ex)[0] != 0:
        fail("an explicit width did not read n/a for the rule and the instrument: %r" % (st,))
    fresh = "[06:07:03.620] vScreen resolution: auto, but no per-eye render width is on record yet for this install -- the on-foot screen stays at the game's own 1920x1080"
    st, out = statuses(fresh)
    if st.get("RULE") != "n/a" or "fresh install" not in out:
        fail("an auto with no eye width on record did not say so: %r" % (st,))

    # ---- the instrument ----
    nothing = "\n".join(l for l in text.splitlines() if not re.match(r"^\[[0-9:.]+\] vscreen footprint", l))
    st, out = statuses(nothing)
    if st.get("INSTRUMENT") != "STOP" or "never ran" not in out:
        fail("a launch with no footprint line at all did not STOP the instrument: %r\n%s" % (st, out))
    armed_only = "\n".join(l for l in text.splitlines() if not re.match(r"^\[[0-9:.]+\] vscreen footprint 30s:", l))
    st, out = statuses(armed_only)
    if st.get("INSTRUMENT") != "WARN" or "shorter than 30 s" not in out:
        fail("an armed instrument with no closed window did not WARN: %r" % (st,))
    st, out = statuses(sub(text, "draws=5400", "draws=0").replace("draws=5280", "draws=0"))
    if st.get("INSTRUMENT") != "STOP" or "never seen" not in out:
        fail("draws=0 in every window did not STOP: %r\n%s" % (st, out))
    st, out = statuses(re.sub(r"samples=\d+ on-foot=\d+ other=\d+", "samples=0 on-foot=0 other=0", text))
    if st.get("INSTRUMENT") != "STOP" or "not one sample" not in out:
        fail("a composite seen but not one sample read did not STOP: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "window=2 samples=60 on-foot=60 other=0 skipped=0 late=0", "window=2 samples=60 on-foot=60 other=0 skipped=2 late=0 why=vb0-stride:2"))
    if st.get("INSTRUMENT") != "PASS" or "2 skipped (vb0-stride x2)" not in out:
        fail("skipped samples were not counted by reason: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "window=2 samples=60 on-foot=60 other=0 skipped=0 late=0", "window=2 samples=60 on-foot=60 other=0 skipped=0 late=3"))
    if st.get("INSTRUMENT") != "WARN" or "not ready in time" not in out:
        fail("late copies did not WARN: %r" % (st,))

    # ---- on foot, stable, shape, the distance law, the calibration point, what is stored ----
    menu_only = "\n".join(l for l in text.splitlines() if "window=2 " not in l and "window=3 " not in l and "window=4 " not in l)
    st, out = statuses(menu_only)
    if st.get("ON FOOT") != "WARN" or st.get("STORED") != "n/a" or "menu only (3503 px)" not in out:
        fail("a menu-only session did not WARN on foot and store nothing: %r\n%s" % (st, out))
    st, out = statuses(sub(text, "fp=3501 frac=0.8683 range=3493..3509", "fp=3700 frac=0.9177 range=3693..3709"))
    if st.get("STABLE") != "WARN" or "the screen's width moved" not in out:
        fail("a window that moved by 5%% did not WARN stable: %r" % (st,))
    st, out = statuses(sub(text, "fp=3501 frac=0.8683 range=3493..3509", "fp=3501 frac=0.8683 range=3300..3709"))
    if st.get("STABLE") != "WARN":
        fail("a wide range inside a window did not WARN stable: %r" % (st,))
    st, out = statuses(text.replace("shape=1.778", "shape=1.700"))
    if st.get("SHAPE") != "WARN":
        fail("a shape 4%% off 16:9 did not WARN: %r" % (st,))
    st, out = statuses(text.replace("shape=1.778", "shape=1.200"))
    if st.get("SHAPE") != "STOP" or "not what the instrument assumes" not in out:
        fail("a shape 32%% off 16:9 did not STOP: %r\n%s" % (st, out))
    st, out = statuses(text.replace(" shape=1.778", ""))
    if st.get("SHAPE") != "n/a":
        fail("windows with no shape token did not read n/a: %r" % (st,))
    # Two distances: the last window flown at 1.0 with the footprint scaled by 1/d (A at distance 1 unchanged): the law holds; if it did not
    # scale (the screen the same width at another distance) it does not.
    w4 = next(l for l in text.splitlines() if "window=4 " in l)
    w4d = (w4.replace("distance=0.700 applied=0.700", "distance=1.000 applied=1.000").replace("fp=3499 frac=0.8678 range=3491..3507 h=0.5049",
                                                                                             "fp=2449 frac=0.6074 range=2441..2457 h=0.3533"))
    st, out = statuses(text.replace(w4, w4d))
    if st.get("DISTANCE LAW") != "PASS" or "varies as 1/d" not in out:
        fail("a window flown at another panel distance with the footprint scaled by 1/d did not PASS the law: %r\n%s" % (st, out))
    w4bad = w4d.replace("at1=2449 frac1=0.6075", "at1=3499 frac1=0.8678")
    st, out = statuses(text.replace(w4, w4bad))
    if st.get("DISTANCE LAW") != "STOP" or "does NOT vary as 1/d" not in out:
        fail("a footprint that did not scale with the distance did not STOP the law: %r\n%s" % (st, out))
    off = text.replace("fp=3497", "fp=3200").replace("fp=3501", "fp=3200").replace("fp=3499", "fp=3200")
    st, out = statuses(off)
    if st.get("CALIBRATION") != "WARN" or "m = 1.095" not in out:
        fail("a measurement 9%% under Sean's 3504 did not WARN and name the m that reproduces it: %r\n%s" % (st, out))
    st, out = statuses(text.replace("eye=4032x3898", "eye=3296x3186"))
    if st.get("CALIBRATION") != "n/a":
        fail("another eye did not read n/a at the calibration point: %r" % (st,))
    st, out = statuses(re.sub(r"persisted=[0-9.]+", "persisted=no", text))
    if st.get("STORED") != "WARN" or "needs at least 12 on-foot samples" not in out:
        fail("on-foot samples with nothing stored did not WARN: %r" % (st,))
    st, out = statuses(sub(text, "persisted=0.6075 fit=3504", "persisted=0.6075 fit=3856"))
    if st.get("STORED") != "PASS" or "the next launch fits 3856 wide (this launch: 3504)" not in out:
        fail("a next launch that would change the width did not say so: %r\n%s" % (st, out))
    rc, out = report("nothing of the kind\nvr camera census: x\n")
    if rc != 1 or "no vscreen auto-fit line" not in out:
        fail("a log with none of the lines did not exit 1 and say so:\n%s" % out)

    # ---- the census verdict reads the render size from the log, never assumes it ----
    census_path = os.path.join(here, CENSUS_FIXTURE)
    if os.path.isfile(census_path):
        census = read_text(census_path)
        apply_line = "[00:00:01.000] vScreen resolution: 1920x1080 -> 3504x1971 at 6 site(s). This writes to game CODE\n"
        blind = census.replace("hdr=5040x2835", "hdr=0x0").replace(" px in 5040x2835,", " px in 0x0,")
        if blind == census:
            fail("the census fixture has no hdr= or JITTERED size to blank")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            print_camera_census(apply_line + blind)
        got = buf.getvalue()
        if "about 1.8e-04" in got or "NDC at 3504x1971" not in got:
            fail("with the route's sizes blanked the census verdict did not read the size the panel patch logged (3504x1971):\n%s" % got)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            print_camera_census(blind)
        leak = [l for l in buf.getvalue().splitlines() if "(i) LEAK" in l]
        if len(leak) != 1 or "NDC at" in leak[0] or "the render size is not in this log" not in leak[0]:
            fail("with no size anywhere in the log the census verdict's LEAK line still assumed one: %r" % (leak,))
        if route_hdr([], route_events(apply_line)) != (3504, 1971, "the panel patch's `vScreen resolution:` line"):
            fail("route_hdr did not fall back to the panel patch's line: %r" % (route_hdr([], route_events(apply_line)),))
    else:
        fail("the census fixture %s is missing beside this script" % CENSUS_FIXTURE)
    return ok


def self_test_camera_census():
    """--camera-census on the checked-in synthetic flight (tools\\camera_census_fixture.log,
    which tools\\vr_camera_census_test holds to exactly what the DLL's formatters write),
    then on logs altered to take away each thing the report depends on, and to break each
    thing the stage 2 verdict judges. Returns ok."""
    import contextlib
    import io
    ok = True

    def fail(msg):
        nonlocal ok
        print("camera census: %s" % msg)
        ok = False

    def report(text):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = print_camera_census(text)
        return rc, buf.getvalue()

    def mutate(text, prefix, fn):
        """The same log with every census line that starts (after its stamp and
        the `vr camera census: ` words) with `prefix` replaced by fn(line)."""
        out = []
        for line in text.split("\n"):
            body = line.split("] ", 1)[1] if line.startswith("[") and "] " in line else line
            body = body[len("vr camera census: "):] if body.startswith("vr camera census: ") else ""
            out.append(fn(line) if body.startswith(prefix) else line)
        return "\n".join(out)

    def sub(text, old, new, count=-1):
        """text with `old` replaced by `new`; fails the test (and returns text) when `old` is not there: a mutation
        that changes nothing would pass every check for the wrong reason."""
        if old not in text:
            fail("the mutation %r -> %r found nothing to change" % (old, new))
            return text
        return text.replace(old, new, count)

    def squash(out):
        return re.sub(r"[ ]+", " ", out)

    def verdict_line(out, tag):
        """The verdict line (status word first) for (tag), or ''."""
        for line in out.split("\n"):
            m = re.match(r"^(PASS|WARN|STOP|n/a)\s+\(%s\) " % re.escape(tag), line)
            if m:
                return line
        return ""

    def status(out, tag):
        return verdict_line(out, tag).split(" ", 1)[0]

    fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)), CENSUS_FIXTURE)
    if not os.path.isfile(fixture):
        fail("the fixture %s is missing beside this script" % CENSUS_FIXTURE)
        return False
    text = read_text(fixture)

    # ---- the parser ----
    c = parse_camera_census(text)
    if (len(c["windows"]), len(c["order"]), len(c["sequences"]), len(c["eyes"]),
            len(c["geometry"]), len(c["threads"])) != (2, 5, 3, 8, 8, 0):
        fail("the fixture parsed as %r, not 2 windows, 5 cameras, 3 sequences, 8 eye draws, 8 geometry lines, no "
             "other-thread entry" % ((len(c["windows"]), len(c["order"]), len(c["sequences"]), len(c["eyes"]),
                                      len(c["geometry"]), len(c["threads"])),))
    cam = c["cameras"].get(0x241DF6D0BB0)
    if not cam or cam["kind"] != 5 or cam["caller"] != 0x594E13 or cam["tone"] != "after" or \
            abs(cam["near"] - 0.025) > 1e-9 or cam["tan"] is not None or \
            cam["viewport"] != (0.0, 0.0) or cam["frame"] != 4 or cam["view"] != 0x241DD00E000 or \
            cam["vctx"] != 0x241DCE749F40:
        fail("the eye camera's line (kind 5: no tangents, no viewport) parsed as %r" % (cam,))
    world = c["cameras"].get(0x241DC2E2960)
    if not world or world["kind"] != 5 or world["frame"] != 1 or world["tone"] != "none":
        fail("the world camera's first-sight line (it was an eye camera in the cockpit: kind 5) parsed as %r" % (world,))
    seq = c["sequences"][0]
    if seq["frame"] != 4 or seq["calls"] != 24 or len(seq["rows"]) != 24 or seq["rows"][0]["rows"] is None or \
            len(seq["rows"][0]["rows"]) != 16 or seq["rows"][15]["kind"] != 1 or seq["rows"][15]["rows"] is not None or \
            seq["rows"][15]["post"] != 0x4 or seq["rows"][0]["pre"] != 0x1C or seq["rows"][0]["post"] != 0x0 or \
            seq["foot"] != "yes" or seq["rows"][0]["view"] != 0x241DD00A000 or seq["rows"][23]["view"] != 0x241DD00F000:
        fail("a call sequence parsed wrong: %r" % (seq["rows"][15],))
    if seq["phase_state"] != "on" or seq["phase"] != (0.252, -0.126) or \
            [(r["kind"], r["inj"], r["role"]) for r in (seq["rows"][0], seq["rows"][12], seq["rows"][15], seq["rows"][17])] != \
            [(3, 1, "scene"), (3, 1, "fp"), (1, 0, "-"), (5, 0, "-")]:
        fail("the sequence's phase, or a call's inj and role, parsed wrong: %r %r" % (seq["phase"], seq["rows"][0]))
    if [s["phase"] for s in c["sequences"]] != [(0.252, -0.126), (-0.189, 0.063), (0.126, 0.252)] or \
            [e["phase"] for e in c["eyes"]][:2] != [(0.252, -0.126), (0.252, -0.126)] or \
            any(e["phase_state"] != "on" for e in c["eyes"]):
        fail("the phases of the sequences and the eye draws parsed wrong: %r" % ([s["phase"] for s in c["sequences"]],))
    g0 = seq["rows"][0]["geo"]
    if not g0 or abs(g0["near"] - 0.025) > 1e-9 or abs(g0["shift"][0] - 2 * 0.252 / 5040) > 1e-6 or \
            abs(g0["shift"][1] - 2 * 0.126 / 2835) > 1e-6 or abs(g0["aspect"] - 5040.0 / 2835.0) > 1e-4:
        fail("a call's geometry read back from its rows is wrong: %r" % (g0,))
    if any(e["foot"] != "yes" for e in c["eyes"]):
        fail("an eye draw parsed without the journal's word: %r" % (c["eyes"][0],))
    if not any("kind" in ch["fields"] and ch["fields"]["kind"] == "5->3" for chs in c["changes"].values() for ch in chs) or \
            any(ch["fields"].keys() - {"bound", "kind", "aspect", "fov", "viewport", "near"} for chs in c["changes"].values() for ch in chs):
        fail("the 'changed:' lines parsed as %r (the world camera's kind 5->3 is one)" % (c["changes"],))
    routes = parse_world_route(text)
    if len(routes) != 2 or routes[0]["route"]["jitter"] != "idle" or routes[1]["route"]["jitter"] != "on" or \
            route_tok(routes[1], "hdr") != "5040x2835" or route_tok(routes[1], "inj-kinds") != "3:6750" or \
            _cpair(route_tok(routes[1], "phase")) != (-0.252, -0.063) or route_tok(routes[1], "fp-mode") != "0/450/0" or \
            route_tok(routes[1], "pair-checked") != "448" or route_hdr(routes) != (5040, 2835, "a `vr world route 5s:` line's hdr="):
        fail("the route's two lines parsed as %r" % (routes,))
    # A line with no stamp, one with, and a value the DLL printed as nan; and an older census's call line, which has no inj and no role.
    n = parse_camera_census("vr camera census: call frame=9 n=1 camera=0x10 kind=3 caller=+0x594E13 draw=- tone=none "
                            "fl=0x1C>- rows=[1,nan,0,0,0,0,0,0,0,0,0,0,0,0,0,0]\n"
                            "[01:02:03.004] vr camera census: call frame=9 n=2 camera=0x10 kind=- caller=+0x1 draw=7 "
                            "tone=after inj=1 role=scene fl=0x0>0x0 rows=-\nsomething else entirely\n")
    rows = n["sequences"][0]["rows"] if n["sequences"] else []
    if len(rows) != 2 or rows[0]["draw"] is not None or rows[0]["post"] is not None or rows[1]["kind"] is not None or \
            rows[1]["rows"] is not None or rows[0]["rows"][1] == rows[0]["rows"][1] or n["lines"] != 2 or \
            (rows[0]["inj"], rows[0]["role"]) != (None, None) or (rows[1]["inj"], rows[1]["role"]) != (1, "scene") or \
            rows[0]["geo"] is not None or n["sequences"][0]["phase_state"] != "absent":
        fail("the stamp, a dash, a nan, and a line with no inj/role/phase parsed wrong: %r lines=%r" % (rows, n["lines"]))
    if (_cphase(None), _cphase("-"), _cphase("0.25,-0.5"), _cphase("x,y"), _cphase("1,2,3"), _cphase("nan,1")) != \
            (("absent", None), ("off", None), ("on", (0.25, -0.5)), ("absent", None), ("absent", None), ("absent", None)):
        fail("_cphase")
    if (_ckinds("none"), _ckinds("3:54"), _ckinds("3:54,other:2"), _ckinds("3:x"), _ckinds(None), _ckinds("")) != \
            ({}, {"3": 54}, {"3": 54, "other": 2}, None, None, None):
        fail("_ckinds")
    if (_cint("12"), _cint("-1"), _cint(None), _cint("")) != (12, None, None, None):
        fail("_cint")

    # A line cut short or garbled is counted and skipped, and a call with no camera has nothing to digest.
    g = parse_camera_census("vr camera census: call frame=x n=1 camera=0x10 kind=3 caller=+0x1 draw=- tone=none fl=0x0>- rows=-\n"
                            "vr camera census: call frame=5 n=1 kind=3 caller=+0x1 draw=- tone=none fl=0x0>- rows=-\n"
                            "vr camera census: sequence frame=5 index=1/3 foot=yes calls=1 recorded=1 truncated=0\n"
                            "vr camera census: call frame=5 n=1 camera=0x20 kind=3 caller=+0x1 draw=- tone=none fl=0x0>- view=0x99 rows=-\n")
    if g["unparsed"] != 2 or len(g["sequences"]) != 1 or len(g["sequences"][0]["rows"]) != 1 or \
            g["sequences"][0]["rows"][0]["view"] != 0x99:
        fail("a garbled line and a call with no camera were not skipped: %r" % (g,))
    rc, out = report("vr camera census: call frame=x n=1 camera=0x10 kind=3\n")
    if rc != 0 or "1 census line(s) could not be parsed" not in out:
        fail("a log of only garbled lines did not say so:\n%s" % out)

    # ---- the geometry a call's rows carry, read back out of them (flight 1's real rows) ----
    scene = [-1.04384, -0.007344862, 0, 0.113728, -2.103792e-08, 1.866733, 0, 0.03455516, -0.1195614, 0.06412452, 0, -0.9929108,
             0, 0, 0.025, 0]
    first_person = [-1.285258, -0.009043574, 0, 0.113728, -2.590355e-08, 2.298469, 0, 0.03455516, -0.1472135, 0.07895518, 0,
                    -0.9929108, 0, 0, 0.0675, 0]
    eye_left = [0.9415472, -0.06691322, 0, 0.02606118, 0.07811873, 0.9655237, 0, 0.08863233, 0.1475604, -0.08419283, 0, 0.9957235,
                0, 0, 0.025, 0]
    gs, gf, ge = census_geometry(scene), census_geometry(first_person), census_geometry(eye_left)
    if not gs or abs(gs["xs"] - 1.05066) > 1e-4 or abs(gs["ys"] - 1.86785) > 1e-4 or abs(gs["aspect"] - 16.0 / 9.0) > 1e-4 or \
            abs(gs["near"] - 0.025) > 1e-9 or max(abs(v) for v in gs["shift"]) > 1e-6:
        fail("flight 1's scene rows read back as %r, not 1.0507 x 1.8678, aspect 16:9, near 0.025, no shift" % (gs,))
    if not gf or abs(gf["ys"] / gs["ys"] - 1.2313) > 1e-3 or abs(gf["near"] - 0.0675) > 1e-9:
        fail("flight 1's first-person rows read back as %r, not x1.231 tighter with near 0.0675" % (gf,))
    if not ge or abs(ge["shift"][0] - 0.178391) > 1e-5 or abs(ge["aspect"] - 1.0344) > 1e-3 or abs(ge["fov"] - 1.59971) > 1e-3:
        fail("flight 1's left eye rows read back as %r, not shift +0.1784, aspect 1.0344, fov 1.5997" % (ge,))
    if census_geometry(None) is not None or census_geometry([1.0] * 15) is not None or \
            census_geometry([float("nan")] + [0.0] * 15) is not None or census_geometry([0.0] * 16) is not None:
        fail("rows that are absent, short, not finite or degenerate produced a geometry")
    if census_phase_ndc((0.252, -0.126), 5040, 2835) != (2 * 0.252 / 5040, 2 * 0.126 / 2835) or \
            census_phase_ndc((0.0, 0.0), 5040, 2835) != (0.0, -0.0):
        fail("census_phase_ndc is not flatProjectionJitter's rule (x = 2 px / W, y = -2 py / H)")
    # One object, two projections: grouped by scale and near, whichever way the head is turned.
    def row(n, rows):
        return {"n": n, "kind": 3, "camera": 0x1, "caller": 0x594E13, "tone": "before", "view": 0x5, "frame": 7,
                "rows": rows, "geo": census_geometry(rows), "inj": None, "role": None}
    groups = census_projections([row(1, scene), row(2, first_person), row(3, scene), row(4, scene)])
    if [grp["count"] for grp in groups] != [3, 1] or groups[1]["near"] != 0.0675 or groups[0]["near"] != 0.025:
        fail("one camera's kind-3 calls were not split into its two projections: %r" % ([(g["count"], g["near"]) for g in groups],))

    # ---- the report on the fixture ----
    rc, out = report(text)
    flat = squash(out)
    want = [
        "165 census line(s): 2 5 s line(s), 5 camera(s), 3 call sequence(s), 8 eye draw(s), 0 other-thread entries, 3 episode(s)",
        "off-thread calls: 0 in every window -- the refresh runs on the render thread",
        "frames with the tone drawn: 898; sampled (tone while the journal says on foot, or no journal; while the world route "
        "jitters, with a non-zero phase): 450; the journal said: no, yes",
        "world route: 2 5 s window(s); jitter: idle x1, on x1",
        "0x241DC2E2960 5 +0x594E13 0.95 0.025 1.5708 50000 (0, 0) (0, 0) 0x241DD00E000 none 1 k3 x45 WORLD; 3 'changed:' line(s)",
        "0x241DF6D0BB0 5 +0x594E13 0.95 0.025 1.5708 50000 (0, 0) (0, 0) 0x241DD00E000 after 4 k5 x9 EYE",
        "0x241DC2E2960 frame 4 moved: aspect 0.95->1.777778, bound (0,0)->(5e-05,4.444444e-05), fov 1.570796->1.0122, kind 5->3, "
        "viewport (0,0)->(5040,2835)",
        "camera 0x241DC2E2960 WORLD: k3 x45 over 3 logged frame(s) (15.0 a frame); kind-3 calls before the tone 45, after 0; "
        "first seen as kind 5",
        "projection 1: 12.0 call(s) a frame, scale 1.0149 x 1.8042 (aspect 1.7778), near 0.025, off-centre (9.99e-05, 8.89e-05); "
        "role= scene x36; injected 36 of 36",
        "projection 2: 3.0 call(s) a frame, scale 1.2507 x 2.2234 (aspect 1.7778), near 0.0675, x1.232 tighter than projection 1",
        "role= fp x9; injected 9 of 9 -- the first-person weapon camera's signature: the same object, a tighter field of view and "
        "a larger near plane",
        "camera 0x241DE100200 world-side: k3 x2, k5 x1 over 3 logged frame(s) (1.0 a frame); kind-3 calls before the tone 2, after 0; "
        "SEVERAL KINDS (frame 4: k5 x1, frame 5: k3 x1, frame 6: k3 x1)",
        "camera 0x241DF6D0BB0 EYE: k5 x9 over 3 logged frame(s) (3.0 a frame)",
        "frame 4 (sequence 1/3, journal foot=yes, phase (0.2520, -0.1260) px): 24 call(s), 24 recorded, 0 truncated",
        "camera 0x241DC2E2960 k3: 15 call(s), callers +0x594E13 x7, +0x594EAB x4, +0x594FE1 x4, view 0x241DD00A000, 0x241DD00A800, "
        "0x241DD00B000, tone before x15, draws 350..5047; injected 15 (fp 3, scene 12)",
        "camera 0x241DE100200 k5: 1 call(s), callers +0x594FE1 x1, view 0x241DD00C800, tone after x1, draws 8209..8209",
        "camera 0x241DE100200 k3: 1 call(s), callers +0x58DE73 x1, view 0x241DD00C800, tone before x1, draws 5059..5059; injected 0 (aux 1)",
        "k5 +0x594FE1 after x1 (calls 18..18) camera 0x241DE100200",
        "eye 0 frame 4 draw 8213 (journal foot=yes, phase (0.2520, -0.1260) px) b1 0x1EB2E751E20 first=0 bytes=5376",
        "eye 0 frame 4 draw 8213: camera 0x241DF6D0BB0 (kind 5) caller +0x594E13, +0x594EAB, +0x594FE1, 3 call(s) n=19/20/21, "
        "draw 8210/8211/8212, tone after",
        "eye 1 frame 6 draw 8220: camera 0x241DF6D0FF0 (kind 5) caller +0x594E13, +0x594EAB, +0x594FE1, 3 call(s) n=22/23/24",
        "eye camera 0x241DF6D0BB0 signature: kind 5, caller +0x594E13, aspect 0.95, near 0.025, far 50000, fov 1.5708, bound (0, 0), "
        "offcentre (0.263158, -0.1), viewport (0, 0), tangents -, view 0x241DD00E000",
        "eye 1 frame 7 draw 8220: frame 7's call sequence was not logged (only the first 3 on-foot frames are)",
        "eye camera(s): 0x241DF6D0BB0, 0x241DF6D0FF0; world camera: 0x241DC2E2960; other world-side kind-3 camera(s): 0x241DE100200",
        "world camera 0x241DC2E2960: 45 kind-3 call(s) over 3 logged frame(s) = 15.0 a frame, 45 of them before the tone "
        "(2 projection(s); its first-seen kind was 5)",
        "(A) call signature (kind, and what a call's composed rows say: aspect, fov, near, off-centre): kind, aspect, shift separate "
        "every eye camera from EVERY world-side kind-3 camera",
        "(B) content join: 6 of 8 eye draw(s) joined to a camera; eye camera(s) 0x241DF6D0BB0, 0x241DF6D0FF0",
        "(C) place in the frame: every refresh of a world-side camera precedes every refresh of an eye camera in 3 of 3 logged "
        "sequence(s)",
        "frame 4: world-side 15 call(s) [tone before x15; last draw 5047], eye 6 call(s) [tone after x6; first draw 8210]",
        "the tone flag separates the two sets",
        "(D) caller: eye camera(s) call from +0x594E13, +0x594EAB, +0x594FE1; the world camera from +0x594E13, +0x594EAB, +0x594FE1; "
        "shared: +0x594E13, +0x594EAB, +0x594FE1 -- the caller alone does not separate them",
        "(F) view (the refresh's second argument, the pass object): eye camera(s) are refreshed with 0x241DD00E000, 0x241DD00F000; "
        "the world camera with 0x241DD00A000, 0x241DD00A800, 0x241DD00B000; no view is shared: the view separates them",
        "(E) tangents: not compared: the eye cameras' lines carry no tangents (tan=-",
        "leak measure over 8 eye draw(s): largest |leak| = 3.648e-08 NDC",
        "the rows cannot tell which way the shift is carried: every candidate leaves about 5.26e-09 NDC",
        "PASS (i) LEAK: 8 of 8 eye draw(s) had a non-zero world phase (up to 0.2520 px, about 1.8e-04 NDC at 5040x2835); worst |leak| "
        "3.65e-08 NDC over 8 measured, below 1e-06",
        "PASS (ii) KIND-3 ROWS CARRY THE PHASE: 45 injected kind-3 call(s) in 3 sequence(s) with a non-zero phase measure the phase "
        "they were given (scene 36 call(s) worst 9.48e-08, first-person 9 call(s) worst 1.16e-07), within 1e-06 NDC; expected: "
        "x = 2 px / W, y = -2 py / H at 5040x2835 (from a `vr world route 5s:` line's hdr=)",
        "PASS (iii) INJECTED KINDS: only kind 3 was injected (route inj-kinds=3:6750 over 2 window(s); 45 logged call(s) with inj=1, "
        "all kind 3)",
        "PASS (iv) OFF-THREAD / UNREADABLE: none: census off-thread 0 and no unreadable kind over 2 window line(s), route off-thread 0 "
        "and unreadable 0 over 2 route window(s)",
        "PASS (v) ROLES: the route counted inj-scene 5400, inj-fp 1350, inj-refused 0, warming 0, aux 450, after 0, unsupported 2700, "
        "other-kind 900; logged calls in non-zero-phase sequences: scene 36 (36 injected), first-person 9 (9 injected), auxiliary 2 "
        "(0 injected)",
        "note: the route's own pair check of the rows it believes: 448 checked, 0 inconsistent",
        "note: a fully-on-foot census window holds 6.0 kind-5 calls a frame (design: 6.0, two eyes x three call sites); 15.0 injected "
        "calls a frame (design: 54-68) (over 450 frame(s) in 1 window(s))",
        "PASS (vi) INJECTION WINDOW: inj-shut=0 over 2 route window(s); inj-unnamed=0 (one per world-to-map change is expected: its first frame "
        "cannot be told from a world frame before the cameras refresh; this log shows 0 such episode(s)); jitter=unnamed in 0 window(s)",
        "stage 2 verdict: PASS (6 PASS, 0 WARN, 0 STOP, 0 n/a)",
    ]
    if rc != 0:
        fail("the fixture reported exit %d" % rc)
    for w in want:
        if w not in flat:
            fail("the report on the fixture lacks %r:\n%s" % (w, out))
            break
    # The two things flight 1 showed: a camera object labelled by its FIRST call's kind, and (C), (D) and (F) then unanswerable.
    if "not enough calls logged to say" in out or "other kind" in out:
        fail("the report on the fixture still says 'not enough calls' or labels a camera 'other kind':\n%s" % out)

    # ---- the join, a row at a time ----
    # A float moved by 9e-6 still joins; one moved by 2e-5 does not, and that draw says so.
    def nudge(delta):
        def fn(line):
            head, _, tail = line.partition("rows=[")
            values, _, rest = tail.partition("]")
            parts = values.split(",")
            parts[0] = "%.9g" % (float(parts[0]) + delta)
            return head + "rows=[" + ",".join(parts) + "]" + rest
        return fn
    near = mutate(text, "eye=0 frame=5 ", nudge(9e-6))
    far = mutate(text, "eye=0 frame=5 ", nudge(2e-5))
    _, out = report(near)
    if "(B) content join: 6 of 8" not in out:
        fail("rows 9e-6 apart did not join (the tolerance is 1e-5):\n%s" % out)
    _, out = report(far)
    if "(B) content join: 5 of 8" not in out or \
            "eye 0 frame 5 draw 8213: NO logged call of frame 5 produced these rows" not in out:
        fail("rows 2e-5 apart joined, or the unmatched draw was not named:\n%s" % out)
    # No census lines at all: exit 1 and a sentence that says why.
    rc, out = report("[00:00:01.000] version 0.18.0 (build ABCD)\n[00:00:02.000] something else\n")
    if rc != 1 or "no `vr camera census` line in this log" not in out:
        fail("a log with no census lines reported exit %d:\n%s" % (rc, out))
    # The route never reported progress: a window with progress=no says so, and there is nothing to join.
    only = "[00:00:05.000] vr camera census 5s: frames=450 calls=1000 posts=1000 off-thread=0 stale=0 kinds=3:1000 " \
           "callers=+0x594E13:1000 cameras-seen=1 cameras-total=1 tone=0/0/1000 on-foot-frames=0 eye-draws=900/0 " \
           "progress=no hook=installed windows=1 cam-overflow=0 thread-overflow=0\n"
    rc, out = report(only)
    if rc != 0 or "progress=no in every window" not in out or "No eye draw was read back" not in out or \
            "off-thread calls: 0 in every window" not in out or "stage 2 verdict: n/a" not in out:
        fail("a progress=no window was not called out:\n%s" % out)
    # The tone was drawn all session but the journal never said on foot (a ship, a menu): named, and nothing is joined.
    ship = "[00:00:05.000] vr camera census 5s: frames=450 calls=1000 posts=1000 off-thread=0 stale=0 kinds=3:1000 " \
           "callers=+0x594E13:1000 cameras-seen=1 cameras-total=1 tone=500/500/0 tone-frames=450 on-foot-frames=0 foot=no " \
           "eye-draws=900/0 progress=yes hook=installed windows=1 cam-overflow=0 thread-overflow=0\n"
    rc, out = report(ship)
    if rc != 0 or "the tone was drawn in 450 frame(s) but no frame was sampled" not in out or \
            "the journal said: no" not in out or "No eye draw was read back" not in out:
        fail("a tone with no on-foot frame was not called out:\n%s" % out)
    # ... and when the route was jittering it says the phase is the other reason a frame is not sampled.
    jitter_ship = "[00:00:05.000] vr world route 5s: key=auto state=owned last=treated jitter=on phase=0.0000,0.0000 rows=0.0000,0.0000 " \
                  "fp-mode=0/1/0 last-trigger=VS=1 PS=2 target=2520x1417 hdr=5040x2835 selection=selected:1\n" + ship.replace("foot=no", "foot=yes")
    rc, out = report(jitter_ship)
    if "the route was jittering, and the census samples a frame then only with a non-zero phase" not in out:
        fail("a jittering route with no sampled frame did not name the phase as a reason:\n%s" % out)
    # Eye draws whose rows match nothing at all: the eye's camera is not at the refresh.
    every = mutate(text, "eye=", lambda line: line if "eye-geometry" in line else
                   line.replace("rows=[", "rows=[9").replace("[99", "[9"))
    _, out = report(every)
    if "no camera was joined to an eye draw" not in out or "NO logged call of frame 4" not in out or \
            "Cameras with kind-5 calls in the logged sequences (candidates, not joined): " not in out or \
            "== stage 2 verdict" not in out:
        fail("eye rows that join to no call did not say so (and still print the verdict):\n%s" % out)
    # Two cameras with the same rows are both named; a failed readback is named, not joined.
    twice = text + "[12:00:09.000] vr camera census: call frame=4 n=22 camera=0x241DF6D0FF0 kind=5 caller=+0x594FE1 draw=8214 tone=after fl=0x1C>0x0 rows=-\n"
    rc, out = report(twice)
    if rc != 0:
        fail("a log with an extra call line failed to report")
    failed = mutate(text, "eye=1 frame=6 ", lambda line: line.split("rows=")[0] + "rows=- meas=- why=map")
    _, out = report(failed)
    if "eye 1 frame 6 draw 8220: no rows were read (map)" not in out or "(B) content join: 5 of 8" not in out:
        fail("a failed readback was joined or not named:\n%s" % out)
    # A shift of nothing (the jitter off): every sign candidate is the same number, so no sign is named. (Found on the
    # glue rig's first real log: a tie was sorted by the candidates' spelling and named +shift.x, +shift.y.)
    zero = mutate(text, "eye-geometry ", lambda line: re.sub(r" shift=\([^)]*\)", " shift=(0,0)", line))
    _, out = report(zero)
    if "the rows cannot tell which way the shift is carried" not in out or "\n    the rows measure as " in out:
        fail("a zero shift named a sign, or did not say it could not:\n%s" % out)

    # ---- roles from the calls (flight 1's bug: a camera labelled by its first-seen kind) ----
    # The world camera's first-sight line says kind 5 and every call of it is kind 3: it is the WORLD camera, not "other kind".
    _, out = report(text)
    table = [l for l in out.split("\n") if l.startswith("0x241DC2E2960")]
    if not table or "WORLD" not in table[0] or "other kind" in table[0]:
        fail("the world camera (first seen as kind 5) was not labelled by its calls:\n%s" % table)
    # Take the world camera's kind-3 calls away and it has no call to be labelled by: no world camera is named.
    no_world = "\n".join(l for l in text.split("\n") if not (": call frame=" in l and "camera=0x241dc2e2960" in l.lower() and " kind=3 " in l))
    _, out = report(no_world)
    if "world camera: 0x241DC2E2960" in out:
        fail("a camera with no kind-3 call was named the world camera:\n%s" % out)
    # A camera that is kind 5 in one frame and kind 3 in another is labelled by BOTH, and its kind-5 call is not an eye call.
    _, out = report(text)
    if "SEVERAL KINDS (frame 4: k5 x1, frame 5: k3 x1, frame 6: k3 x1)" not in out or \
            "eye camera(s): 0x241DF6D0BB0, 0x241DF6D0FF0;" not in out:
        fail("a camera that changed kind between frames was not labelled by each frame's kind, or became an eye:\n%s" % out)
    # The eye cameras, when an eye draw's rows join a kind-3 call (an older build's eyes): still found, by the join.
    old_eyes = text.replace(" kind=5 caller", " kind=3 caller")
    _, out = report(old_eyes)
    if "eye camera(s): 0x241DF6D0BB0, 0x241DF6D0FF0;" not in out:
        fail("the eye cameras are found by the join whatever their kind:\n%s" % out)
    # The world camera is the camera with the most kind-3 calls BEFORE the tone (then the most kind-3 calls, then the first seen), not the first
    # camera of the table: move the auxiliary camera's line (2 kind-3 calls) ahead of the world camera's and the world camera is still found.
    lines = text.split("\n")
    aux_at = next(i for i, l in enumerate(lines) if "vr camera census: camera=0x241de100200 " in l)
    world_at = next(i for i, l in enumerate(lines) if "vr camera census: camera=0x241dc2e2960 " in l)
    lines.insert(world_at, lines.pop(aux_at))
    _, out = report("\n".join(lines))
    if "world camera: 0x241DC2E2960" not in out or out.index("0x241DE100200") > out.index("0x241DC2E2960 "):
        fail("the world camera was chosen by its place in the camera table, not by its calls:\n%s" % out)
    def kind3_call(n, camera, tone):
        return "vr camera census: call frame=1 n=%d camera=0x%X kind=3 caller=+0x594E13 draw=%d tone=%s fl=0x0>0x0 view=0x%X rows=-" % (
            n, camera, n, tone, camera + 0x1000)
    tie = parse_camera_census("vr camera census: sequence frame=1 index=1/3 calls=20 recorded=20 truncated=0\n" + "\n".join(
        [kind3_call(n, 0xA0, "after") for n in range(1, 11)] + [kind3_call(n, 0xB0, "before") for n in range(11, 21)]) + "\n")
    tie2 = parse_camera_census("vr camera census: sequence frame=1 index=1/3 calls=25 recorded=25 truncated=0\n" + "\n".join(
        [kind3_call(n, 0xA0, "before") for n in range(1, 11)] + [kind3_call(n, 0xB0, "before") for n in range(11, 26)]) + "\n")
    tie3 = parse_camera_census("vr camera census: sequence frame=1 index=1/3 calls=30 recorded=30 truncated=0\n" + "\n".join(
        [kind3_call(n, 0xA0, "before") for n in range(1, 11)] + [kind3_call(n, 0xB0, "before") for n in range(11, 21)] +
        [kind3_call(n, 0xB0, "after") for n in range(21, 31)]) + "\n")
    if census_roles(tie, [])["world"] != 0xB0 or census_roles(tie2, [])["world"] != 0xB0 or census_roles(tie3, [])["world"] != 0xB0:
        fail("the world camera is the one with the most kind-3 calls before the tone, then the most kind-3 calls: %r %r %r"
             % (census_roles(tie, [])["world"], census_roles(tie2, [])["world"], census_roles(tie3, [])["world"]))
    # No draw progress: the tone flag is unavailable and (C) says so instead of deciding.
    no_tone = text.replace("tone=before", "tone=none").replace("tone=after", "tone=none")
    _, out = report(no_tone)
    if "the tone flag is unavailable" not in out:
        fail("calls with tone=none were judged by a tone flag they do not have:\n%s" % out)

    # ---- the stage 2 verdict ----
    _, base = report(text)
    # (i) the leak: PASS below 1e-6, WARN to 1e-5, STOP above.
    def with_leak(value):
        return mutate(text, "eye-geometry eye=0 frame=5 ", lambda line: re.sub(r" leak=\([^)]*\)", " leak=(%s,0.000e+00)" % value, line))
    for value, want_status in (("9.0e-07", "PASS"), ("2.0e-06", "WARN"), ("9.0e-06", "WARN"), ("2.0e-05", "STOP"), ("-2.0e-04", "STOP")):
        _, out = report(with_leak(value))
        if status(out, "i") != want_status:
            fail("a leak of %s was %s, want %s:\n%s" % (value, status(out, "i"), want_status, verdict_line(out, "i")))
    _, out = report(with_leak("2.0e-04"))
    if "worst |leak| 2.00e-04 NDC at eye 0 frame 5" not in verdict_line(out, "i") or "stage 2 verdict: STOP" not in out:
        fail("a leaking eye was not named with its eye and frame, or did not make the verdict STOP:\n%s" % verdict_line(out, "i"))
    # A leak in a frame with a ZERO phase is not judged (nothing could leak): the same number is the baseline there.
    zero_frame = mutate(with_leak("2.0e-04"), "eye=0 frame=5 ", lambda line: re.sub(r" phase=\S+", " phase=0.0000,0.0000", line))
    _, out = report(zero_frame)
    if status(out, "i") != "PASS" or "7 of 8 eye draw(s) had a non-zero world phase" not in verdict_line(out, "i"):
        fail("an eye draw with a zero phase was judged for a leak:\n%s" % verdict_line(out, "i"))
    # An eye shift that is ON while the phase is non-zero: a warning, and the leak is the best fit over the shift's signs.
    shifted = mutate(text, "eye-geometry eye=0 frame=5 ", lambda line: re.sub(r" shift=\([^)]*\)", " shift=(0.0001,0.00005)", line))
    _, out = report(shifted)
    if status(out, "i") != "WARN" or "had the eye shift on while the world phase was non-zero" not in verdict_line(out, "i"):
        fail("an eye shift on during a non-zero phase was not warned about:\n%s" % verdict_line(out, "i"))
    # ... and when the shift is on, the game may carry it with the other sign than `leak=` assumes: an eye whose rows are the frustum moved by
    # MINUS the shift has a large `leak=` and a zero residual at the best fit, so it is the eye shift that is reported (WARN), not a leak (STOP).
    frustum0 = c["geometry"][(0, 5)]["frustum"]
    minus = census_expected_measure(frustum0, -0.0001, -0.00005)
    plus = census_expected_measure(frustum0, 0.0001, 0.00005)

    def carried_minus(line):
        line = re.sub(r" shift=\([^)]*\)", " shift=(0.0001,0.00005)", line)
        return re.sub(r" leak=\([^)]*\)", " leak=(%.3e,%.3e)" % (minus[0] - plus[0], minus[1] - plus[1]), line)
    signed = mutate(text, "eye-geometry eye=0 frame=5 ", carried_minus)
    signed = mutate(signed, "eye=0 frame=5 ", lambda line: re.sub(r" meas=\([^)]*\)", " meas=(%.9g,%.9g)" % minus, line))
    _, out = report(signed)
    worst_seen = re.search(r"worst \|leak\| ([0-9.e+-]+) NDC over", verdict_line(out, "i"))
    if status(out, "i") != "WARN" or "had the eye shift on while the world phase was non-zero" not in verdict_line(out, "i") or \
            not worst_seen or float(worst_seen.group(1)) > 1e-6 or abs(minus[0] - plus[0]) < 1e-4:
        fail("an eye carrying the shift with the other sign was not judged by its best fit:\n%s" % verdict_line(out, "i"))
    # No eye draw sampled with a non-zero phase while the route jitters: STOP with its reason (zero phases; phase=- earlier).
    all_zero = mutate(text, "eye=", lambda line: line if "eye-geometry" in line else re.sub(r" phase=\S+", " phase=0.0000,0.0000", line))
    _, out = report(all_zero)
    if status(out, "i") != "STOP" or "no eye draw was sampled with a non-zero phase although the route was jittering" not in verdict_line(out, "i"):
        fail("eye draws with only zero phases were not a STOP with its reason:\n%s" % verdict_line(out, "i"))
    early = mutate(text, "eye=", lambda line: line if "eye-geometry" in line else re.sub(r" phase=\S+", " phase=-", line))
    _, out = report(early)
    if status(out, "i") != "STOP" or "were read earlier with phase=-" not in verdict_line(out, "i") or "(jitter=on in 1 route window(s))" not in verdict_line(out, "i"):
        fail("eye draws read before the route jittered were not explained:\n%s" % verdict_line(out, "i"))
    no_eyes = "\n".join(l for l in text.split("\n") if "vr camera census: eye" not in l)
    _, out = report(no_eyes)
    if status(out, "i") != "STOP" or "no eye draw was read back" not in verdict_line(out, "i"):
        fail("a jittering route with no eye draw read back was not a STOP:\n%s" % verdict_line(out, "i"))
    # The route never jittered (the census on, the route key off): nothing to judge, and not a STOP.
    idle_route = mutate(text, "eye=", lambda line: line if "eye-geometry" in line else re.sub(r" phase=\S+", " phase=-", line))
    idle_route = "\n".join(l for l in idle_route.split("\n") if "vr world route" not in l)
    idle_route = re.sub(r" phase=0\.\d{4},-?0\.\d{4} calls=", " phase=- calls=", idle_route)
    idle_route = re.sub(r" phase=-0\.\d{4},-?0\.\d{4} calls=", " phase=- calls=", idle_route)
    _, out = report(idle_route)
    if status(out, "i") != "n/a" or "the route never jittered in this log" not in verdict_line(out, "i") or \
            status(out, "ii") != "n/a":
        fail("a census with the route never jittering was judged:\n%s\n%s" % (verdict_line(out, "i"), verdict_line(out, "ii")))
    # A phase that differs between a frame's sequence header and its eye line.
    mismatch = mutate(text, "sequence frame=4 ", lambda line: line.replace("phase=0.2520,-0.1260", "phase=0.2520,-0.1000"))
    _, out = report(mismatch)
    if status(out, "i") != "WARN" or "frame(s) 4 name different phases" not in verdict_line(out, "i"):
        fail("a sequence and an eye line with different phases were not warned about:\n%s" % verdict_line(out, "i"))

    # (ii) the kind-3 rows carry the phase, within 1e-6 of flatProjectionJitter's shift.
    def with_header_phase(old, new):
        return mutate(text, "sequence frame=4 ", lambda line: line.replace("phase=" + old, "phase=" + new))
    wrong = with_header_phase("0.2520,-0.1260", "0.3520,-0.1260")      # 1e-4 px further right: 2e-5 NDC
    _, out = report(wrong)
    if status(out, "ii") != "STOP" or "do not measure the phase they were given" not in verdict_line(out, "ii"):
        fail("rows that carry another phase were not a STOP:\n%s" % verdict_line(out, "ii"))
    tiny = with_header_phase("0.2520,-0.1260", "0.2524,-0.1260")      # 4e-4 px: 8e-8 NDC, inside the tolerance
    _, out = report(tiny)
    if status(out, "ii") != "PASS":
        fail("a phase within the tolerance of the rows was not a PASS:\n%s" % verdict_line(out, "ii"))
    between = with_header_phase("0.2520,-0.1260", "0.2640,-0.1260")    # 0.012 px: 4.8e-6 NDC, between 1e-6 and 1e-5
    _, out = report(between)
    if status(out, "ii") != "WARN" or "is between 1e-06 and 1e-05" not in verdict_line(out, "ii"):
        fail("rows between the tolerance and the stop were not a WARN:\n%s" % verdict_line(out, "ii"))
    flipped = with_header_phase("0.2520,-0.1260", "0.2520,0.1260")     # the y sign the other way
    _, out = report(flipped)
    if status(out, "ii") != "STOP" or "the rows that miss DO fit the phase with y flipped" not in verdict_line(out, "ii"):
        fail("rows carrying the y sign the other way were not recognised:\n%s" % verdict_line(out, "ii"))
    # The phase never reached a call: every scene and first-person call is inj=0.
    not_injected = text.replace(" inj=1 role=scene ", " inj=0 role=scene ").replace(" inj=1 role=fp ", " inj=0 role=fp ")
    _, out = report(not_injected)
    if status(out, "ii") != "STOP" or "hold no injected (inj=1) scene or first-person call" not in verdict_line(out, "ii"):
        fail("a phase that reached no call was not a STOP:\n%s" % verdict_line(out, "ii"))
    one_refused = text.replace(" inj=1 role=scene ", " inj=0 role=scene ", 1)
    _, out = report(one_refused)
    if status(out, "ii") != "WARN" or "were not injected (inj=0)" not in verdict_line(out, "ii"):
        fail("one scene call not injected was not a WARN:\n%s" % verdict_line(out, "ii"))
    after_tone = text.replace("tone=before inj=1 role=scene fl=0x1C>0x0 view=0x241dd00a000", "tone=after inj=1 role=scene fl=0x1C>0x0 view=0x241dd00a000", 1)
    _, out = report(after_tone)
    if status(out, "ii") != "WARN" or "came after the tone" not in verdict_line(out, "ii"):
        fail("an injected call after the tone was not a WARN:\n%s" % verdict_line(out, "ii"))
    no_hdr = re.sub(r" hdr=\d+x\d+ ", " hdr=0x0 ", text)
    _, out = report(no_hdr)
    if status(out, "ii") != "WARN" or "the render size is unknown" not in verdict_line(out, "ii"):
        fail("a log with no render size was not a WARN:\n%s" % verdict_line(out, "ii"))
    # The size the phase is in matters: the same rows against another render size do not carry that phase.
    small = text.replace("hdr=5040x2835", "hdr=2520x1417")
    _, out = report(small)
    if status(out, "ii") != "STOP":
        fail("rows measured against the wrong render size were not a STOP:\n%s" % verdict_line(out, "ii"))
    # An older census (no inj= on the calls) cannot be judged here.
    _, out = report(re.sub(r" inj=\d role=\S+", "", text))
    if status(out, "ii") != "n/a" or "no inj= token" not in verdict_line(out, "ii"):
        fail("calls with no inj= were judged:\n%s" % verdict_line(out, "ii"))

    # (iii) only kind 3 is injected: from the route's inj-kinds= and from the census's own call lines.
    kinds = text.replace("inj-kinds=3:6750", "inj-kinds=3:6745,other:5")
    _, out = report(kinds)
    if status(out, "iii") != "STOP" or "kind other x5" not in verdict_line(out, "iii"):
        fail("an injected kind other than 3 (route token) was not a STOP:\n%s" % verdict_line(out, "iii"))
    kind_five = mutate(text, "call frame=4 n=19 ", lambda line: line.replace("inj=0 role=-", "inj=1 role=-"))
    _, out = report(kind_five)
    if status(out, "iii") != "STOP" or "are not kind 3 (k5)" not in verdict_line(out, "iii"):
        fail("an injected kind-5 call (census line) was not a STOP:\n%s" % verdict_line(out, "iii"))
    # A kind-5 call that claims a scene role and an injection is judged as what it is: (iii) STOPs, and (ii) measures only kind-3 calls (the
    # eye call's rows carry the eye's own off-centre, not the phase: measured as a scene call they would be a second, false STOP).
    kind_five_scene = mutate(text, "call frame=4 n=19 ", lambda line: line.replace("inj=0 role=-", "inj=1 role=scene"))
    _, out = report(kind_five_scene)
    if status(out, "iii") != "STOP" or status(out, "ii") != "PASS":
        fail("an injected kind-5 call was measured as a kind-3 scene call in (ii), or was not a STOP in (iii):\n%s\n%s"
             % (verdict_line(out, "ii"), verdict_line(out, "iii")))
    nothing = text.replace("inj-kinds=3:6750", "inj-kinds=none")
    nothing = re.sub(r" inj=1 role=", " inj=0 role=", nothing)
    _, out = report(nothing)
    if status(out, "iii") != "WARN" or "nothing was injected" not in verdict_line(out, "iii"):
        fail("a flight that injected nothing was not a WARN:\n%s" % verdict_line(out, "iii"))
    # (iv) off-thread or unreadable, from the census's 5 s lines and the route's tokens.
    off_census = text.replace("off-thread=0 stale=0 inj-calls=6750", "off-thread=7 stale=0 inj-calls=6750")
    _, out = report(off_census)
    if status(out, "iv") != "STOP" or "the census counted 7 call(s) off the render thread" not in verdict_line(out, "iv"):
        fail("census off-thread calls were not a STOP:\n%s" % verdict_line(out, "iv"))
    off_route = text.replace("unreadable=0 off-thread=0 write-fail=0 inj-kinds=3:6750", "unreadable=0 off-thread=2 write-fail=0 inj-kinds=3:6750")
    _, out = report(off_route)
    if status(out, "iv") != "STOP" or "the route counted 2 off-thread call(s)" not in verdict_line(out, "iv"):
        fail("route off-thread calls were not a STOP:\n%s" % verdict_line(out, "iv"))
    unreadable = text.replace("unreadable=0 off-thread=0 write-fail=0 inj-kinds=3:6750", "unreadable=3 off-thread=0 write-fail=0 inj-kinds=3:6750")
    _, out = report(unreadable)
    if status(out, "iv") != "STOP" or "the route counted 3 call(s) whose kind could not be read" not in verdict_line(out, "iv"):
        fail("route unreadable calls were not a STOP:\n%s" % verdict_line(out, "iv"))
    unreadable_census = text.replace("kinds=1:900,3:7200,5:2700", "kinds=1:900,3:7200,5:2700,unreadable:4")
    _, out = report(unreadable_census)
    if status(out, "iv") != "STOP" or "the census counted 4 call(s) whose kind could not be read" not in verdict_line(out, "iv"):
        fail("census unreadable kinds were not a STOP:\n%s" % verdict_line(out, "iv"))
    thread_line = text + "[12:00:09.000] vr camera census: other-thread tid=4321 camera=0x241dc2e2960 kind=3 caller=+0x594E13 calls=57\n"
    rc, out = report(thread_line.replace("off-thread=0 stale=0 inj-calls=6750", "off-thread=57 stale=0 inj-calls=6750"))
    if "57 refresh call(s) ran on a thread other than the render thread" not in out or "1 other-thread entry" not in out or \
            "other-thread: tid 4321 camera 0x241DC2E2960 kind 3 caller 0x594E13 calls 57" not in out or status(out, "iv") != "STOP":
        fail("an off-thread call was not reported, listed and a STOP:\n%s" % out)
    # (v) the roles: an auxiliary call injected is a STOP; calls refused a WARN.
    aux_injected = text.replace(" inj=0 role=aux ", " inj=1 role=aux ")
    _, out = report(aux_injected)
    if status(out, "v") != "STOP" or "auxiliary call(s) were injected" not in verdict_line(out, "v"):
        fail("an injected auxiliary call was not a STOP:\n%s" % verdict_line(out, "v"))
    refused = text.replace("inj-refused=0 warming=0 aux=450", "inj-refused=4 warming=0 aux=450")
    _, out = report(refused)
    if status(out, "v") != "WARN" or "4 call(s) were refused for want of a write" not in verdict_line(out, "v"):
        fail("refused calls were not a WARN:\n%s" % verdict_line(out, "v"))
    write_failed = text.replace("write-fail=0 inj-kinds=3:6750", "write-fail=2 inj-kinds=3:6750")
    _, out = report(write_failed)
    if status(out, "v") != "WARN" or "2 write(s) failed" not in verdict_line(out, "v"):
        fail("failed writes were not a WARN:\n%s" % verdict_line(out, "v"))
    no_scene = text + "[12:00:10.000] vr world route: jitter is wanted but no scene camera call was injected (frame=4)\n"
    _, out = report(no_scene)
    if status(out, "v") != "WARN" or "the route logged: " not in verdict_line(out, "v"):
        fail("the route's own 'no scene camera call was injected' line was not a WARN:\n%s" % verdict_line(out, "v"))
    # (vi) the injection window: inj-shut is always 0; inj-unnamed is one per world-to-map change; the route's own STOP lines are quoted.
    shut = text.replace("pair-checked=448 pair-bad=0 inj-unnamed=0 inj-shut=0", "pair-checked=448 pair-bad=0 inj-unnamed=0 inj-shut=3")
    _, out = report(shut)
    if status(out, "vi") != "STOP" or "inj-shut=3: camera calls were INJECTED on a frame whose window the route had shut" not in verdict_line(out, "vi"):
        fail("inj-shut above 0 was not a STOP:\n%s" % verdict_line(out, "vi"))
    route_stop = text + "[12:00:10.000] vr world route: STOP at frame=812: camera calls were injected on a frame whose window the route had shut (decision: unnamed)\n"
    _, out = report(route_stop)
    if status(out, "vi") != "STOP" or "the route logged: " not in verdict_line(out, "vi") or \
            "the route's own STOP line: " in verdict_line(out, "vi"):
        fail("the route's shut-window STOP line was not quoted once as a STOP:\n%s" % verdict_line(out, "vi"))
    other_stop = text + "[12:00:10.000] vr world route: STOP at frame=813: some other route-side invariant failed\n"
    _, out = report(other_stop)
    if status(out, "vi") != "STOP" or "the route's own STOP line: " not in verdict_line(out, "vi"):
        fail("any other route STOP line was not quoted as a STOP:\n%s" % verdict_line(out, "vi"))
    kind_other = text + "[12:00:10.000] vr world route: STOP at frame=9: camera calls of a kind other than 3 were INJECTED (kind 5 x2)\n"
    _, out = report(kind_other)
    if status(out, "iii") != "STOP" or "the route logged: " not in verdict_line(out, "iii"):
        fail("the route's 'kind other than 3 were INJECTED' line was not a STOP in (iii):\n%s" % verdict_line(out, "iii"))
    if status(out, "vi") == "STOP":
        fail("the route's kind-other STOP line was counted against the injection window as well:\n%s" % verdict_line(out, "vi"))
    # The size the phase is in may come from the route's own JITTERED line when no 5 s line has an hdr=; hdr= wins when both are there.
    jittered = "[12:00:10.000] vr world route: the world is JITTERED from frame=13805: phase (0.2520,-0.1260) px in 5040x2835, 12 scene and 3 first-person " \
               "camera call(s) injected this frame (kind 3 only; the eye cameras, kind 5, are never written)\n"
    _, out = report(re.sub(r" hdr=\d+x\d+ ", " hdr=0x0 ", text) + jittered)
    if status(out, "ii") != "PASS" or "(from the route's `the world is JITTERED` line)" not in verdict_line(out, "ii"):
        fail("the render size was not taken from the route's JITTERED line when no 5 s line has one:\n%s" % verdict_line(out, "ii"))
    _, out = report(text + jittered.replace("5040x2835", "2520x1417"))
    if status(out, "ii") != "PASS" or "(from a `vr world route 5s:` line's hdr=)" not in verdict_line(out, "ii"):
        fail("the JITTERED line's size beat the route line's hdr=:\n%s" % verdict_line(out, "ii"))
    # The signatures the route excluded by role are quoted, so the roles that still need deciding are in the verdict.
    excluded = text + "[12:00:10.000] vr world route: camera call EXCLUDED, not a screen view: kind 3 aspect=1.0000 fov=1.5708 near=0.1000 far=50000.0 " \
                      "caller=+0x58DE73 (the screen's aspect is 1.7778; a screen view is within 4%): 6 call(s) so far, never injected\n"
    _, out = report(excluded)
    if "the route excluded 1 kind-3 call signature(s) by role and never injected them" not in out or \
            "aspect=1.0000 fov=1.5708 near=0.1000 far=50000.0 caller=+0x58DE73 (6 call(s) so far)" not in out:
        fail("the route's EXCLUDED line was not quoted as a note:\n%s" % out)
    fault = text.replace("jitter=on", "jitter=fault")
    _, out = report(fault)
    if status(out, "vi") != "STOP" or "jitter=fault in 1 route window(s)" not in verdict_line(out, "vi"):
        fail("jitter=fault was not a STOP:\n%s" % verdict_line(out, "vi"))
    no_hook = text.replace("jitter=on", "jitter=no-hook")
    _, out = report(no_hook)
    if status(out, "vi") != "WARN" or "jitter=no-hook in 1 route window(s)" not in verdict_line(out, "vi"):
        fail("jitter=no-hook was not a WARN:\n%s" % verdict_line(out, "vi"))
    # inj-unnamed: one per world-to-map change is expected, and the verdict can only compare it with the episodes the log shows.
    unnamed_no_episode = text.replace("pair-checked=448 pair-bad=0 inj-unnamed=0 inj-shut=0", "pair-checked=448 pair-bad=0 inj-unnamed=2 inj-shut=0")
    _, out = report(unnamed_no_episode)
    if status(out, "vi") != "WARN" or "inj-unnamed=2 exceeds the 0 map/menu/release episode(s) this log shows" not in verdict_line(out, "vi"):
        fail("inj-unnamed with no episode in the log was not a WARN:\n%s" % verdict_line(out, "vi"))
    with_unnamed_window = unnamed_no_episode.replace("inj-unnamed=2", "inj-unnamed=1").replace("jitter=on", "jitter=unnamed")
    _, out = report(with_unnamed_window)
    if status(out, "vi") != "PASS" or "inj-unnamed=1 (one per world-to-map change is expected" not in verdict_line(out, "vi") or \
            "this log shows 1 such episode(s)" not in verdict_line(out, "vi") or "jitter=unnamed in 1 window(s)" not in verdict_line(out, "vi"):
        fail("inj-unnamed explained by a jitter=unnamed episode was not a PASS with its note:\n%s" % verdict_line(out, "vi"))
    with_release = unnamed_no_episode + "[12:00:10.000] vr world route: RELEASED the world at frame=812 (frames-not-treated) after 13044 owned frame(s); the eye shift is back on\n" \
                   "[12:00:11.000] vr world route: RELEASED the world at frame=1812 (frames-not-treated) after 98 owned frame(s); the eye shift is back on\n"
    _, out = report(with_release)
    if status(out, "vi") != "PASS" or "this log shows 2 such episode(s)" not in verdict_line(out, "vi") or \
            "RELEASED 2 time(s)" not in out:
        fail("inj-unnamed explained by RELEASED lines was not a PASS:\n%s\n%s" % (verdict_line(out, "vi"), out))
    old_route = "\n".join(l.replace(" inj-unnamed=0 inj-shut=0", "") for l in text.split("\n"))
    _, out = report(old_route)
    if status(out, "vi") != "n/a" or "no route line has inj-shut= or inj-unnamed=" not in verdict_line(out, "vi") or \
            "stage 2 verdict: PASS (5 PASS, 0 WARN, 0 STOP, 1 n/a)" not in out:
        fail("a route line without inj-shut/inj-unnamed was not n/a (and did not leave the verdict PASS):\n%s" % out)
    # The route's jitter= has six values (on, off, idle, unnamed, no-hook, fault): all are counted, none crashes the report.
    six = text.replace("jitter=idle", "jitter=unnamed")
    rc, out = report(six)
    if rc != 0 or "world route: 2 5 s window(s); jitter: on x1, unnamed x1" not in out:
        fail("the route's jitter= states were not all counted:\n%s" % out)
    pair_bad = text.replace("pair-checked=448 pair-bad=0", "pair-checked=448 pair-bad=3")
    _, out = report(pair_bad)
    if "WARN  (note) the route's own pair check of the rows it believes: 448 checked, 3 inconsistent" not in squash(out).replace("WARN (note)", "WARN  (note)"):
        fail("the route's pair check failing was not a WARN:\n%s" % out)
    # The overall word.
    _, out = report(with_leak("2.0e-04"))
    if "stage 2 verdict: STOP (" not in out:
        fail("one STOP did not make the verdict STOP")
    _, out = report(with_leak("2.0e-06"))
    if "stage 2 verdict: WARN (" not in out:
        fail("one WARN and no STOP did not make the verdict WARN")

    # ---- the stage 2 experiment build: the refusal census, the key's state, the view, the weapon's role ----
    # The parser: the fixture's two refusal windows, each joined to the route and inject lines of its own window.
    rw = parse_refusal_windows(text)
    if len(rw) != 2 or [refusal_state(w) for w in rw] != ["idle", "measured"] or rw[1]["pixels"] != 1600300800 or rw[1]["refused"] != 58410978 or \
            rw[1]["causes"]["stale-refused"] != 40007520 or rw[1]["causes"]["masked"] != 800150 or rw[1]["causes"]["sentinel"] != 16003008 or \
            rw[1]["causes"]["range"] != 1600300 or rw[1]["kept"] != 0 or (rw[1]["check_ran"], rw[1]["check_skipped"]) != (0, 0) or \
            rw[1]["steady"] != "off" or rw[1]["view"] != "off" or \
            (rw[1]["w"], rw[1]["h"]) != (5040, 2835) or abs(rw[1]["pct"] - 3.650) > 1e-9 or rw[1]["every"] != 4 or rw[1]["dropped"] != 0 or \
            rw[1]["route"]["fp-mode"] != "0/450/0" or rw[1]["inject"]["fov"] != "0.8203..0.9831" or rw[0]["route"]["jitter"] != "idle" or \
            rw[0]["inject"]["fov"] != "-" or not rw[0]["census"]:
        fail("the fixture's refusal lines parsed as %r" % ([(w["ts"], refusal_state(w), w["pixels"], w["refused"]) for w in rw],))
    if len(parse_world_route(text)) != 2:
        fail("the refusal lines joined the route's own windows (they are a third line and a list of their own)")
    if (_cmodes("0/450/0"), _cmodes("0/0/12"), _cmodes("0/450"), _cmodes(None), _cmodes("a/b/c")) != ((0, 450, 0), (0, 0, 12), None, None, None):
        fail("_cmodes")
    if (_cfov("0.8203..0.9831"), _cfov("-"), _cfov(None), _cfov("1..2"), _cfov("0.8203.0.9831")) != ((0.8203, 0.9831), None, None, (1.0, 2.0), None):
        fail("_cfov")
    garbled = parse_refusal_windows("vr world route refusal 5s: census=on treated=x asked=0\n")
    if len(garbled) != 1 or garbled[0]["treated"] is not None or refusal_state(garbled[0]) != "unreadable":
        fail("a refusal counter that is not a number parsed as one, or its window was not 'unreadable'")
    if [e for e in route_events("[00:00:01.000] vr world route: steady-detail is ON from frame=100 (x)\n"
                                "[00:00:02.000] vr world route: the refusal view is ON from frame=200 (y)\n").keys()] != ["steady-detail", "refusal-view"]:
        fail("the route's steady-detail and refusal-view lines are not among its event markers")
    # The report on the fixture: the section, and the weapon's numbers in (v), whose old PASS text is unchanged.
    rc, out = report(text)
    flat = squash(out)
    for w in (
        "== refusal census (advanced.vr_camera_census: the route's own resolve, the prep's per-pixel classification, one sample in 4 of the "
        "resolves that ask; shares are of the pixels the samples examined) ==",
        "census=on: NOT MEASURED: the route treated no frame in this window (nothing was measured) | route state=observing jitter=idle "
        "fp-mode 0/0/0, inj-fp 0, struct fov -",
        "census=on: MEASURED 112 sample(s) of 5040x2835 (450 asked, 113 dispatched, 0 dropped), treated 450; pixels 1600300800; refused "
        "3.650% (58410978): stale-refused 2.500%, masked 0.050%, sentinel 1.000%, range 0.100%; stale-kept 0.000% | route state=owned jitter=on "
        "fp-mode 0/450/0, inj-fp 1350, struct fov 0.8203..0.9831",
        "totals, steady-detail=off: 1 measured window(s), 112 sample(s), pixels 1600300800, refused 3.650% (58410978): stale-refused 2.500%, masked "
        "0.050%, sentinel 1.000%, range 0.100%; stale-kept 0.000%; of the refused: stale-refused 68.5%, masked 1.4%, sentinel 27.4%, range 2.7%",
        "causes seen: stale-refused = the engine slot's depth was not the pixel's (a later draw overdrew it) and the steady-detail depth check, if it is on, "
        "did not confirm the camera term; masked = a rig record with no usable history",
        "refusal census: consistent (1 of 2 window(s) measured)",
        "; struct field of view 0.8203..0.9831 rad over 1 window(s); the weapon's fold-in ran in 450 frame(s) (mode 1: 450, mode 2: 0), about "
        "3.0 first-person call(s) credited a frame",
    ):
        if w not in flat:
            fail("the report on the fixture lacks %r:\n%s" % (w, out))
            break
    if "!! " in out or "refusal note:" in out:
        fail("the fixture's refusal census raised a finding:\n%s" % out)

    def refusal_line(**kw):
        """One refusal line of the fixture's measured window, with some tokens changed."""
        v = dict(census="on", every=4, treated=450, asked=450, sampled=113, read=112, dropped=0, size="5040x2835", pixels=1600300800,
                 refused=58410978, pct="3.650", stale=40007520, masked=800150, corrupt=0, sentinel=16003008, unreprojectable=0, camera=0,
                 range=1600300, depth=0, weapon=0, other=0, kept=0, ran=0, skipped=0, steady="off", view="off")
        v.update(kw)
        return ("[12:00:09.000] vr world route refusal 5s: census=%(census)s every=%(every)d treated=%(treated)d asked=%(asked)d "
                "sampled=%(sampled)d read=%(read)d dropped=%(dropped)d size=%(size)s pixels=%(pixels)d refused=%(refused)d "
                "refused-pct=%(pct)s stale-refused=%(stale)d masked=%(masked)d corrupt=%(corrupt)d sentinel=%(sentinel)d "
                "unreprojectable=%(unreprojectable)d camera=%(camera)d range=%(range)d depth=%(depth)d weapon=%(weapon)d other=%(other)d "
                "stale-kept=%(kept)d depth-check=%(ran)d/%(skipped)d steady-detail=%(steady)s view=%(view)s\n") % v

    nothing = dict(refused=0, pct="0.000", stale=0, masked=0, sentinel=0, range=0)
    # "Ran, 0 refused" (pixels > 0, refused=0) is never the same text as "never ran" (treated=0, asked=0, read=0).
    _, out = report(text + refusal_line(**nothing))
    if "refused 0.000% (0): none refused; stale-kept 0.000%" not in squash(out) or "!! " in out or \
            "totals, steady-detail=off: 2 measured window(s), 224 sample(s), pixels 3200601600, refused 1.825% (58410978)" not in squash(out):
        fail("a window that measured and found nothing refused was not reported as measured, or the totals did not add the two:\n%s" % out)
    _, out = report(text + refusal_line(treated=450, asked=0, sampled=0, read=0, pixels=0, **nothing))
    if "NOT MEASURED: the route treated 450 frame(s) and none asked for the census" not in out or \
            "!! [12:00:09.000]: the route treated 450 frame(s) and none asked the resolver for the census" not in out or \
            "refusal census: WARN (1 finding(s) above)" not in out or "0.000% (0): none refused" in out.split("NOT MEASURED")[1].split("\n")[0]:
        fail("a route that treated frames and asked for no census was not called out as never having measured:\n%s" % out)
    _, out = report(text + refusal_line(treated=3, asked=3, sampled=0, read=0, pixels=0, **nothing))
    if "NOT MEASURED: 3 ask(s), fewer than one sample's worth (every 4)" not in out or "!! " in out:
        fail("fewer asks than one sample took was a WARN (it is a short window), or was not said:\n%s" % out)
    _, out = report(text + refusal_line(treated=450, asked=450, sampled=5, read=0, pixels=0, **nothing))
    if "NOT MEASURED: 5 sample(s) dispatched, none read back" not in out or "5 sample(s) were dispatched and none was read back" not in out:
        fail("samples dispatched and never read back were not a WARN:\n%s" % out)
    _, out = report(text.replace("treated=0 asked=0 sampled=0 read=0", "treated=450 asked=450 sampled=0 read=0").replace(
        "treated=450 asked=450 sampled=113 read=112 dropped=0 size=5040x2835 pixels=1600300800", "treated=450 asked=450 sampled=113 read=0 dropped=0 size=5040x2835 pixels=0"))
    if "the census never measured: the route treated 900 frame(s) over 2 window(s) and no sample was read back" not in out:
        fail("a census that never read a sample back was not a WARN:\n%s" % out)
    _, out = report("\n".join(l for l in text.split("\n") if "vr world route refusal" not in l))
    if "none: no `vr world route refusal 5s:` line in this log" not in out or "!! " in out or "stage 2 verdict: PASS (6 PASS" not in out:
        fail("a log without refusal lines (the census key off, or an older build) did not say so, or its verdict moved:\n%s" % out)
    # The key's state: each state is totalled on its own; with it on a stale slot is kept (last frame's depth confirmed the camera term) or
    # refused (it did not), and the depth check's own frames are counted. 38007520 + 2000000 = the fixture's 40007520 stale pixels.
    on = refusal_line(steady="on", stale=2000000, kept=38007520, refused=20403458, pct="1.275", read=112, ran=448, skipped=2)
    _, out = report(text + on)
    if "!! " in out or "totals, steady-detail=on: 1 measured window(s), 112 sample(s), pixels 1600300800, refused 1.275% (20403458)" not in squash(out) or \
            "stale-kept 2.375% (95.0% of the stale pixels); depth-check 448 ran, 2 skipped" not in squash(out) or \
            "stale-refused 0.125%" not in squash(out) or "totals, steady-detail=off: 1 measured window(s)" not in squash(out):
        fail("a window with the steady-detail key on was not totalled on its own, with its stale-kept share and the depth check's frames:\n%s" % out)
    _, out = report(text + refusal_line(steady="on", stale=40007520, kept=0, ran=0, skipped=450))
    if "steady-detail=on and the depth check never ran (0 ran, 450 skipped)" not in out or "refusal census: WARN" not in out:
        fail("the key on with a depth check that never ran (every frame skipped) was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(steady="on", stale=40007520, kept=0, ran=0, skipped=0))
    if "the resolver counted no depth-check frame in a window that treated 450 frame(s)" not in out or "refusal census: WARN" not in out:
        fail("the key on with no depth-check frame counted at all was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(steady="on", stale=40007520, kept=0, ran=448, skipped=2))
    if "the depth check ran in 448 frame(s) and kept no stale pixel (40007520 refused)" not in out or "!! " in out:
        fail("the depth check that ran and kept nothing was not a note (and only a note):\n%s" % out)
    _, out = report(text + refusal_line(steady="off", kept=9))
    if "9 stale pixel(s) were kept while steady-detail=off" not in out:
        fail("pixels kept with the key off were not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(steady="off", ran=3, skipped=1))
    if "the depth check counted frames (3 ran, 1 skipped) while steady-detail=off" not in out:
        fail("depth-check frames with the key off were not a WARN:\n%s" % out)
    # The flight-3 build's spellings (`stale=`, `forgiven=`, no depth-check) still parse, as stale-refused and stale-kept, and its blanket rule is judged as it was.
    old_spelling = lambda line: line.replace("stale-refused=", "stale=").replace(" stale-kept=", " forgiven=").replace(" depth-check=0/0", "")
    flight3 = old_spelling(refusal_line(steady="on", stale=0, kept=40007520, refused=18403458, pct="1.150", read=112))
    if " depth-check=" in flight3 or " forgiven=40007520 " not in flight3 or " stale=0 " not in flight3:
        fail("the flight-3 line was not built")
    w3 = parse_refusal_windows(flight3)[0]
    if w3["kept"] != 40007520 or w3["causes"]["stale-refused"] != 0 or w3["check_ran"] is not None or w3["check_skipped"] is not None:
        fail("the flight-3 spellings did not parse as stale-kept and stale-refused with no depth-check: %r" % ((w3["kept"], w3["causes"]["stale-refused"], w3["check_ran"]),))
    _, out = report(text + flight3)
    if "!! " in out or "stale-kept 2.500%" not in squash(out) or "depth-check" in out.split("totals, steady-detail=on")[1].split("\n")[0]:
        fail("a flight-3 window with its key on was not read as stale-kept 2.500% with no depth-check, and no finding:\n%s" % out)
    _, out = report(text + old_spelling(refusal_line(steady="on", stale=5, kept=40007515)))
    if "steady-detail=on and 5 stale pixel(s) were still refused" not in out or "refusal census: WARN" not in out:
        fail("stale pixels still refused with the flight-3 build's key on were not a WARN:\n%s" % out)
    # The census key OFF with the steady-detail key on (its default): the route prints the line for the depth check's frame counts, and
    # such a window is `census-off`: not a census that failed to measure, nothing to WARN about unless the depth check's own counts are wrong.
    off_kw = dict(census="off", every=4, treated=450, asked=0, sampled=0, read=0, size="0x0", pixels=0, refused=0, pct="0.000",
                  stale=0, masked=0, sentinel=0, range=0, steady="on", kept=0, ran=448, skipped=2)
    cw = parse_refusal_windows(refusal_line(**off_kw))[0]
    if refusal_state(cw) != "census-off" or (cw["check_ran"], cw["check_skipped"]) != (448, 2) or cw["census"]:
        fail("a census=off line with nothing asked was not the census-off state: %r" % (refusal_state(cw),))
    _, out = report(text + refusal_line(**off_kw))
    if "!! " in out or "census off in 1 window(s), steady-detail=on (the line is printed for the key): the route treated 450 frame(s); " \
            "depth-check 448 ran, 2 skipped" not in squash(out) or \
            "refusal census: consistent (1 of 2 window(s) measured); 1 other window(s) had the census off" not in squash(out) or \
            "NOT MEASURED: the route treated 450" in out:
        fail("a census-off window beside a measured one was not summarised on its own line, or was called a failed census:\n%s" % out)
    _, out = report("\n".join(l for l in text.split("\n") if "vr world route refusal" not in l) + refusal_line(**off_kw) +
                    refusal_line(**dict(off_kw, ran=450, skipped=0)))
    if "!! " in out or "census off in 2 window(s), steady-detail=on" not in squash(out) or "depth-check 898 ran, 2 skipped" not in squash(out) or \
            "refusal census: the census key was off in all 2 window(s): nothing to measure" not in squash(out) or \
            "the census never measured" in out:
        fail("a log whose every refusal line is census-off was not read as 'nothing to measure' without a WARN:\n%s" % out)
    _, out = report(text + refusal_line(**dict(off_kw, ran=0, skipped=0)))
    if "steady-detail=on and the resolver counted no depth-check frame in a window that treated 450 frame(s)" not in out or \
            "refusal census: WARN" not in out:
        fail("a census-off window with the key on and no depth-check frame was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(**dict(off_kw, ran=0, skipped=450)))
    if "steady-detail=on and the depth check never ran (0 ran, 450 skipped)" not in out:
        fail("a census-off window whose depth check never ran was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(**dict(off_kw, steady="off", ran=3, skipped=1)))
    if "the depth check counted frames (3 ran, 1 skipped) while steady-detail=off" not in out:
        fail("a census-off window with depth-check frames and the key off was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(**dict(off_kw, treated=0, ran=0, skipped=0)))
    if "!! " in out:
        fail("a census-off window that treated no frame (nothing to check) was a finding:\n%s" % out)
    _, out = report(text + refusal_line(refused=1600300801))
    if "refused 1600300801 exceeds the pixels examined, 1600300800" not in out:
        fail("more pixels refused than examined was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(dropped=3, other=7))
    if "3 sample(s) were skipped because the read-back ring was full" not in out or "7 refused pixel(s) are of a class the census cannot name" not in out or \
            "refusal census: consistent" not in out:
        fail("dropped samples and unnamed causes were not notes (consistent, not WARN):\n%s" % out)
    _, out = report(text + refusal_line(view="on", steady="on", stale=0, kept=1, ran=1))
    if "the refusal view was painting: the headset showed the prep's classification, not the world" not in out:
        fail("a window with the refusal view on was not noted:\n%s" % out)
    _, out = report(text + "[00:00:10.000] vr world route: steady-detail is ON from frame=100 (experimental.temporal_aa_on_foot_world_steady_detail): x\n"
                    "[00:00:11.000] vr world route: the refusal view is ON from frame=200 (advanced.temporal_aa_debug = motion_source): y\n")
    if "route log: [00:00:10.000] vr world route: steady-detail is ON from frame=100" not in out or "route log: [00:00:11.000] vr world route: the refusal view is ON" not in out:
        fail("the route's state lines were not quoted in the refusal section:\n%s" % out)
    # The weapon's role in (v): flight 2's picture (inj-fp 0 with the fold-in in mode 2 on every weapon frame) is a WARN and says what the
    # field-of-view range makes of it; a credited weapon with a mode-1 fold-in is the fixture's PASS.
    fl2 = text.replace("inj-fp=1350", "inj-fp=0").replace("fp-mode=0/450/0", "fp-mode=0/0/450")
    _, out = report(fl2.replace("fov=0.8203..0.9831", "fov=-"))
    line = verdict_line(out, "v")
    if status(out, "v") != "WARN" or "no first-person call was credited (inj-fp 0) although the weapon's fold-in ran in 450 frame(s) (mode 1: 0, mode 2: 450)" not in line or \
            "no inject line carries a fov= range" not in line:
        fail("flight 2's weapon (inj-fp 0, mode 2, no fov=) was not a WARN naming the missing field-of-view range:\n%s" % line)
    _, out = report(fl2)
    if status(out, "v") != "WARN" or "the struct carries two fields of view (0.8203 and 0.9831 rad)" not in verdict_line(out, "v") or \
            "a fault in the field-of-view test" not in verdict_line(out, "v"):
        fail("inj-fp 0 with two fields of view in the struct was not a WARN naming a fault in the role test:\n%s" % verdict_line(out, "v"))
    _, out = report(fl2.replace("fov=0.8203..0.9831", "fov=0.9831..0.9831"))
    if status(out, "v") != "WARN" or "the struct carries one field of view (0.9831 rad)" not in verdict_line(out, "v"):
        fail("inj-fp 0 with one field of view was not a WARN saying the weapon cannot be told by it:\n%s" % verdict_line(out, "v"))
    _, out = report(text.replace("fp-mode=0/450/0", "fp-mode=0/0/450"))
    if status(out, "v") != "WARN" or "yet the fold-in ran only in mode 2 (450 frame(s), mode 1 in none)" not in verdict_line(out, "v"):
        fail("a credited weapon whose fold-in never ran in mode 1 was not a WARN:\n%s" % verdict_line(out, "v"))
    _, out = report(text.replace("inj-scene=5400", "inj-scene=1000"))
    if status(out, "v") != "WARN" or "as many first-person calls as scene calls (1350 against 1000" not in verdict_line(out, "v"):
        fail("more first-person calls than scene calls was not a WARN:\n%s" % verdict_line(out, "v"))
    _, out = report(text.replace("inj-fp=1350", "inj-fp=0").replace("fp-mode=0/450/0", "fp-mode=450/0/0"))
    if status(out, "v") != "PASS":
        fail("no weapon drawn (the fold-in never ran) with inj-fp 0 is not a fault:\n%s" % verdict_line(out, "v"))
    _, out = report(fl2 + "[12:00:10.000] vr world route: jitter is wanted but no scene camera call was injected at frame=4 (warming 4)\n")
    if status(out, "v") != "WARN" or "the route logged: " not in verdict_line(out, "v"):
        fail("the route's no-scene line did not keep its place ahead of the weapon's checks:\n%s" % verdict_line(out, "v"))

    # ---- older logs: missing tokens are said, not crashed on ----
    older = re.sub(r" inj=\d role=\S+", "", text)
    older = re.sub(r" phase=\S+ calls=", " calls=", older)
    older = re.sub(r" phase=\S+ draw=", " draw=", older)
    older = re.sub(r" inj-calls=\d+", "", older)
    older = "\n".join(l for l in older.split("\n") if "vr world route" not in l)
    rc, out = report(older)
    if rc != 0 or "stage 2 verdict: n/a" not in out or status(out, "i") != "n/a" or status(out, "ii") != "n/a" or \
            "no `vr world route 5s:` line in this log" not in out or "this log predates stage 2" not in out:
        fail("an older log (no phase, inj, role or route lines) was not reported as not judged:\n%s" % out)
    # ... and its roles still come from the calls: the world camera is found, and (C), (D), (F) are answered.
    if "world camera: 0x241DC2E2960" not in out or "(C) place in the frame: every refresh of a world-side camera precedes every" not in out \
            or "not enough calls logged to say" in out:
        fail("an older log's roles were not read from its calls:\n%s" % out)
    # The route tokens on ONE line (the first wording of the route's format), and tokens missing from a window.
    lines = text.split("\n")
    merged, i = [], 0
    while i < len(lines):
        if "vr world route 5s:" in lines[i] and i + 1 < len(lines) and "vr world route inject 5s:" in lines[i + 1]:
            inject_tokens = lines[i + 1].split("vr world route inject 5s: ", 1)[1]
            merged.append(lines[i].replace(" last-trigger=", " " + inject_tokens + " last-trigger=", 1))
            i += 2
        else:
            merged.append(lines[i])
            i += 1
    rc, out = report("\n".join(merged))
    if rc != 0 or "vr world route inject" in "\n".join(merged) or status(out, "iii") != "PASS" or \
            "stage 2 verdict: PASS (6 PASS" not in out:
        fail("the route tokens all on one line (the first wording of the format) were not read:\n%s" % out)
    partial ="\n".join(l.replace(" jitter=on", "") if "vr world route 5s:" in l else l for l in text.split("\n"))
    rc, out = report(partial)
    if rc != 0 or "0 with jitter=on" not in out:
        fail("a route line missing its jitter= token broke the report:\n%s" % out)
    inject_only = "\n".join(l for l in text.split("\n") if "vr world route 5s:" not in l)
    rc, out = report(inject_only)
    if rc != 0 or status(out, "iii") != "PASS" or "hdr" not in verdict_line(out, "ii") and status(out, "ii") != "WARN":
        fail("inject lines with no route line were not read (the size is then unknown):\n%s" % verdict_line(out, "ii"))
    route_only = "\n".join(l for l in text.split("\n") if "vr world route inject 5s:" not in l)
    rc, out = report(route_only)
    if rc != 0 or status(out, "iii") != "PASS" or status(out, "ii") != "PASS":
        fail("route lines with no inject line broke the verdict (the census's own call lines still say what was injected):\n%s" % out)
    rows_joined = parse_world_route("[00:00:01.000] vr world route inject 5s: inj-scene=1\n[00:00:02.000] vr world route inject 5s: inj-scene=2\n"
                                    "[00:00:03.000] vr world route 5s: jitter=on hdr=10x20\n[00:00:03.001] vr world route inject 5s: inj-scene=3\n")
    if [(w["route"] is not None, w["inject"] is not None) for w in rows_joined] != [(False, True), (False, True), (True, True)]:
        fail("route and inject lines were not joined by order: %r" % (rows_joined,))

    # ---- the pieces ----
    rows = [{"n": i + 1, "kind": k, "caller": cl, "tone": t} for i, (k, cl, t) in enumerate(
        [(3, 1, "before")] * 5 + [(3, 2, "before")] * 2 + [(3, 1, "before"), (3, 1, "after"), (3, 1, "after")])]
    runs = census_runs(rows)
    if runs != [(3, 1, "before", 5, 1, 5), (3, 2, "before", 2, 6, 7), (3, 1, "before", 1, 8, 8), (3, 1, "after", 2, 9, 10)]:
        fail("census_runs: %r" % (runs,))
    if not _cequal(1.0, 1.00001) or _cequal(1.0, 1.001) or _cequal((0.0, 0.0), (0.0, 0.01)) or not _cequal((0.0, 1.0), (0.0, 1.0)) \
            or _cequal(float("nan"), float("nan")) or not _cequal(3, 3) or _cequal(3, 4) or not _cequal(None, None):
        fail("_cequal")
    if _cf("nan") == _cf("nan") or _cf("-") == _cf("-") or _cf("1.5") != 1.5 or _chex("+0x594E13") != 0x594E13 or \
            _chex("0x241dc2e2960") != 0x241DC2E2960 or _chex("-") is not None or _ctuple("(1,2)") != (1.0, 2.0) or \
            _ctuple("-") is not None or _clist("[1,2,3]") != [1.0, 2.0, 3.0] or _clist("-") is not None:
        fail("the value parsers")
    # The sign of the shift in the rows: the fit names the convention the rows carry, whichever it is.
    frustum = [-1.2, 0.7, -0.9, 1.1]
    shift = (0.001, 0.0005)
    for label, sx, sy in (("(+shift.x, +shift.y)", 1, 1), ("(+shift.x, -shift.y)", 1, -1),
                          ("(-shift.x, +shift.y)", -1, 1), ("(-shift.x, -shift.y)", -1, -1)):
        meas = census_expected_measure(frustum, sx * shift[0], sy * shift[1])
        fit = census_shift_fit(meas, frustum, shift)
        if not fit or label not in fit[0] or fit[1] > 1e-12 or not fit[3] > 1e-5:
            fail("census_shift_fit did not find %s: %r" % (label, fit))
    meas = census_expected_measure(frustum, 0.0, 0.0)
    fit = census_shift_fit(meas, frustum, shift)
    if not fit or "unshifted" not in fit[0]:
        fail("census_shift_fit did not find the unshifted frustum: %r" % (fit,))
    if census_expected_measure([1, 1, 0, 1], 0, 0) is not None or census_shift_fit(None, frustum, shift) is not None:
        fail("a degenerate window or a missing measurement produced a fit")
    # A frame with more runs than the report shows names how many it left out.
    many = []
    for i in range(40):
        many.append("[00:00:00.%03d] vr camera census: call frame=8 n=%d camera=0x10 kind=3 caller=+0x%X draw=%d tone=before "
                    "fl=0x1C>0x0 rows=-" % (i, i + 1, 0x594E13 + (i & 1), 100 + i))
    many.insert(0, "[00:00:00.000] vr camera census: sequence frame=8 index=1/3 calls=40 recorded=40 truncated=0")
    rc, out = report("\n".join(many) + "\n")
    if "... 16 more run(s) ..." not in out or out.count("(calls ") != CENSUS_RUNS_SHOWN:
        fail("a 40-run frame did not show %d runs and name the 16 it left out:\n%s" % (CENSUS_RUNS_SHOWN, out))
    # No field separates, and the tone flag is unavailable: both are said, not guessed.
    same = "\n".join([
        "vr camera census: camera=0x100 kind=3 caller=+0x594E13 thread=owner aspect=1 near=0.025 far=50000 fov=1 bound=(0,0) "
        "offcentre=(0,0) viewport=(100,100) tan=(-1,1,-1,1) first-call=1 draw=1 tone=none frame=1",
        "vr camera census: camera=0x200 kind=3 caller=+0x594E13 thread=owner aspect=1 near=0.025 far=50000 fov=1 bound=(0,0) "
        "offcentre=(0,0) viewport=(100,100) tan=(-1,1,-1,1) first-call=2 draw=2 tone=none frame=1",
        "vr camera census: sequence frame=1 index=1/3 calls=2 recorded=2 truncated=0",
        "vr camera census: call frame=1 n=1 camera=0x100 kind=3 caller=+0x594E13 draw=1 tone=none fl=0x1C>0x0 rows=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0.1,0]",
        "vr camera census: call frame=1 n=2 camera=0x200 kind=3 caller=+0x594E13 draw=2 tone=none fl=0x1C>0x0 rows=[2,0,0,0,0,1,0,0,0,0,1,0,0,0,0.1,0]",
        "vr camera census: eye=0 frame=1 draw=2 b1=0x5 first=0 bytes=5376 rows=[2,0,0,0,0,1,0,0,0,0,1,0,0,0,0.1,0] meas=(0,0)",
        ""])
    rc, out = report(same)
    if "(A) call signature: no single field" not in out:
        fail("two cameras with one signature were separated by a field:\n%s" % out)
    if "eye camera(s): 0x200; world camera: 0x100" not in out:
        fail("the joined camera was not the eye and the other the world:\n%s" % out)
    if "the tone flag is unavailable" not in out:
        fail("calls with tone=none were judged by a tone flag they do not have:\n%s" % out)
    if "(F) view" not in out or "not enough calls logged to say" not in out:
        fail("calls with no view were judged by one:\n%s" % out)
    if "(D) caller: eye camera(s) call from +0x594E13; the world camera from +0x594E13; shared: +0x594E13" not in out:
        fail("two cameras calling from one site were separated by the caller:\n%s" % out)

    # ---- the episodes (Phase 0): the value parsers, the parser, the report, and logs altered to take away each thing the section depends on ----
    if (_ckindmap("0:12,3:290,5:10"), _ckindmap("-"), _ckindmap("1:2,other:3,unreadable:4"), _ckindmap("x"), _ckindmap(None)) != \
            ({0: 12, 3: 290, 5: 10}, {}, {1: 2, "other": 3, "unreadable": 4}, None, None):
        fail("_ckindmap")
    if (_ccallers("+0x58DE73:300,+0x594E13:130,+more:2"), _ccallers("-"), _ccallers("+0x1:1,"), _ccallers(None)) != \
            (([(0x58DE73, 300), (0x594E13, 130)], 2), ([], 0), None, None):
        fail("_ccallers")
    if (_ctrigger("key-on"), _ctrigger("gui:0>6"), _ctrigger("naming:unnamed>named"), _ctrigger("foot:no"), _ctrigger(""), _ctrigger("bogus:1>2"),
            _ctrigger("zzz"), _ctrigger("gui:>6"), _ctrigger(None)) != \
            (("key-on", None, None), ("gui", "0", "6"), ("naming", "unnamed", "named"), None, None, None, None, None, None):
        fail("_ctrigger")
    if (_cmatch("98,101"), _cmatch("1,2,3,4,5,6+48"), _cmatch("-"), _cmatch("1,x"), _cmatch("3+"), _cmatch(None)) != \
            (([98, 101], 0), ([1, 2, 3, 4, 5, 6], 48), ([], 0), None, None, None):
        fail("_cmatch")
    if _cbins("1:1,2:0,3:0,4-8:0,9-30:0,31-89:1,90+:1") != {"1": 1, "2": 0, "3": 0, "4-8": 0, "9-30": 0, "31-89": 1, "90+": 1} or \
            _cbins("1:1,2:0") is not None or _cbins("2:1,1:0,3:0,4-8:0,9-30:0,31-89:0,90+:0") is not None or _cbins(None) is not None:
        fail("_cbins")
    if (_cpair_us("25/30"), _cpair_us("0.31/4.2"), _cpair_us("-"), _cpair_us("a/b"), _cpair_us(None)) != ((25.0, 30.0), (0.31, 4.2), None, None, None):
        fail("_cpair_us")

    c = parse_camera_census(text)
    eps = c["episodes"]
    if len(c["windows"]) != 2 or (len(eps), len(c["episode_counters"]), len(c["runs"]), len(c["detour"])) != (3, 2, 2, 2) or c["unparsed"]:
        fail("the fixture parsed as %r episodes, counters, runs, detour lines (want 3, 2, 2, 2), %d 5 s windows (want 2: the companions are not windows), %d unparsed"
             % ((len(eps), len(c["episode_counters"]), len(c["runs"]), len(c["detour"])), len(c["windows"]), c["unparsed"]))
    e1, e2, e3 = eps
    if (e1["n"], e1["of"], e1["frame"], e1["armed"], e1["trigger"], e1["foot"], e1["gui"], e1["named"], e1["phase_state"]) != \
            (1, 10, 31, 1, ("key-on", None, None), "no", 0, False, "off") or \
            (e2["trigger"], e2["gui"], e2["armed"], e2["frame"]) != (("gui", "0", "6"), 6, 1530, 1560) or \
            (e3["trigger"], e3["foot"], e3["gui"], e3["phase"], e3["named"]) != (("naming", "named", "unnamed"), "yes", None, (0.252, -0.126), False):
        fail("the episode headers parsed wrong: %r" % ([{k: v for k, v in e.items() if k not in ("rows", "joins", "pass")} for e in eps],))
    if (e1["calls"], e1["recorded"], e1["printed"], len(e1["rows"])) != (14, 14, 14, 14) or e1["kinds"] != {1: 2, 3: 6, 5: 6} or \
            sorted(e1["callers"]) != [(0x58DE73, 2), (0x594E13, 4), (0x594EAB, 4), (0x594FE1, 4)] or e1["callers_more"] != 0 or \
            [r["kind"] for r in e1["rows"]] != [1, 1] + [3] * 6 + [5] * 6 or any(r["frame"] != 31 for r in e1["rows"]) or \
            len(c["sequences"]) != 3:
        fail("an episode's call lines did not follow its header, or leaked into the first three sequences: %r" % ([r["kind"] for r in e1["rows"]],))
    j1 = e1["joins"]
    if [(j["sig"], j["depth"], j["eye"], j["w"], j["h"], j["draw"], j["draws"], j["dw"], j["b1"], j["bytes"], j["read"], j["why"], j["match"]) for j in j1] != \
            [(1, "eye", 0, 2620, 2533, 8210, 1432, True, 0x1EB2E751E20, 5376, True, None, [9, 10, 11]),
             (2, "eye", 0, 2620, 2533, 8650, 61, False, 0x1EB2E751E20, 5376, False, "skip", None),
             (3, "eye", 1, 2620, 2533, 8215, 1432, True, 0x1EB2E751E20, 5376, True, None, [12, 13, 14])] or \
            [len(j["rows"]) if j["rows"] else None for j in j1] != [16, None, 16] or j1[0]["vs"] != 0x5C36AF051B98B9F1 or j1[0]["ps"] != 0xCFE84157BC76E921:
        fail("the join signatures of episode 1 parsed wrong: %r" % (j1,))
    j3 = e3["joins"]
    if [(j["depth"], j["eye"], j["match"], j["b1"], j["why"]) for j in j3] != \
            [("screen", None, [], 0x1EB2E751E20, None), ("screen", None, None, 0x1EB2E751E20, "skip"), ("eye", None, None, None, "no-b1")]:
        fail("the join signatures of episode 3 (a screen depth, no match, a skipped signature, no b1) parsed wrong: %r" % (j3,))
    p1, p2, p3 = e1["pass"], e2["pass"], e3["pass"]
    if (p1["valid"], p1["bound"], len(p1["rows"]), p1["match"], p1["how"], p1["nearest"], p1["diff"]) != (True, True, 12, [9, 10, 11], "identity", 9, 0.0) or \
            (p2["valid"], p2["bound"], p2["match"], p2["how"], p2["nearest"]) != (True, False, [5, 6, 7, 8, 9], "transpose", 5) or \
            (p3["valid"], p3["rows"], p3["match"], p3["how"]) != (False, None, [], None):
        fail("the pass's rows parsed wrong: %r" % ((p1, p2, p3),))
    r2 = c["runs"][1]
    if r2["frames"] != 413 or r2["named"]["31-89"] != 1 or r2["unnamed"]["4-8"] != 1 or (r2["longest_named"], r2["longest_unnamed"]) != (211, 7) or \
            r2["open"] != ("named", 211) or c["runs"][0]["open"] is not None:
        fail("a runs line parsed wrong: %r" % (r2,))
    d2 = c["detour"][1]
    if (d2["every"], d2["frames"], d2["calls"], d2["sampled"]) != (16, 450, 10800, 676) or abs(d2["est_ms"] - 0.155) > 1e-9 or \
            d2["modes"]["inj"]["calls"] != 6750 or d2["modes"]["obs"]["pre"] != (3.35, 3.6) or d2["modes"]["inj"]["post"] != (2.7, 2.8) or c["detour"][0]["modes"]["inj"]["pre"] is not None:
        fail("a detour line parsed wrong: %r" % (d2,))
    if [e["join_draws"] for e in eps] != [{"seen": 3100, "relevant": 2925, "views": 4, "signatures": 3}, {"seen": 2100, "relevant": 1800, "views": 3, "signatures": 2},
                                          {"seen": 905, "relevant": 337, "views": 5, "signatures": 3}]:
        fail("the join-draws lines parsed wrong: %r" % ([e["join_draws"] for e in eps],))
    k2 = c["episode_counters"][1]
    if (k2["taken"], k2["of"], k2["triggers"], k2["skipped"], k2["state"], k2["trigger"]) != (3, 10, 5, 2, "idle", None):
        fail("an episodes (counters) line parsed wrong: %r" % (k2,))
    armed = parse_camera_census("[00:00:01.000] vr camera census: episodes windows=1 taken=2/10 triggers=3 skipped=0 state=armed trigger=gui:0>6 armed=100 sample=130\n")
    if armed["episode_counters"][0]["trigger"] != ("gui", "0", "6") or (armed["episode_counters"][0]["armed"], armed["episode_counters"][0]["sample"]) != (100, 130):
        fail("an armed episodes line parsed wrong: %r" % (armed["episode_counters"],))
    # A line cut short or garbled is counted and skipped; a join line of an episode whose header is not there has nowhere to go.
    bad = parse_camera_census("vr camera census: episode frame=5 n=1/10 trigger=zzz armed=1 foot=no gui=- named=0 phase=- calls=1 recorded=1 printed=1 kinds=- callers=-\n"
                              "vr camera census: join ep=9 sig=1 depth=eye eye=0 size=2620x2533 draw=1 draws=1 vs=0x1 ps=0x1 dw=yes b1=- first=- bytes=- rows=- why=no-b1\n"
                              "vr camera census: pass-rows ep=9 frame=5 valid=0 bound=- rows=- axes-match=- how=- nearest=- diff=-\n"
                              "vr camera census: join-rows ep=9 sig=1 rows=[1,2]\n"
                              "vr camera census: join-draws ep=9 seen=1 relevant=1 views=1 signatures=1\n")
    if bad["unparsed"] != 5 or bad["episodes"]:
        fail("a garbled header and lines of an episode with no header were not skipped and counted: %r" % (bad["unparsed"],))

    # ---- the report ----
    rc, out = report(text)
    flat = squash(out)
    want_episodes = [
        "== episodes (3 printed; the last `episodes` line: 3 taken of 10, 5 trigger(s), 2 skipped) ==",
        "2 trigger(s) arrived while an episode was armed (or after the session's ten): counted, never sampled",
        "episode 1/10: frame 31, trigger key-on (armed at frame 1); journal foot=no, GuiFocus 0, naming unnamed, the route was not jittering (phase=-)",
        "14 call(s), 14 recorded, 14 printed; by kind: k1 x2, k3 x6, k5 x6; by caller: +0x594E13 x4, +0x594EAB x4, +0x594FE1 x4, +0x58DE73 x2",
        "k5 +0x594E13 after x1 (calls 9..9) camera 0x241DF6D0BB0",
        "join: 3 signature(s); the per-draw hook was handed 3100 draw(s), 2925 of them into a screen- or eye-sized depth, 4 depth view(s) resolved (a signature is a depth, a vertex "
        "shader and a pixel shader; the first of each depth has its rows read back)",
        "join: 3 signature(s); the per-draw hook was handed 905 draw(s), 337 of them into a screen- or eye-sized depth, 5 depth view(s) resolved",
        "eye 0 2620x2533: first draw 8210 (1432 draw(s)), vs 0x5C36AF051B98B9F1 ps 0xCFE84157BC76E921, depth write yes, b1 0x1EB2E751E20 (5376 bytes, first constant 0)",
        "rows 270..273 equal the composed rows of: camera 0x241DF6D0BB0 (kind 5) caller +0x594E13, +0x594EAB, +0x594FE1, 3 call(s) n=9/10/11, view 0x241DD00E000, tone after",
        "depth write no, b1 0x1EB2E751E20 (5376 bytes, first constant 0); rows not read (skip)",
        "the pass's chosen rows (bound block yes) equal the view axes (as they are) of: camera 0x241DF6D0BB0 (kind 5) caller +0x594E13, +0x594EAB, +0x594FE1, 3 call(s) n=9/10/11",
        "-> the rows are an eye camera's (kind 5)",
        "episode 2/10: frame 1560, trigger gui 0>6 (armed at frame 1530); journal foot=no, GuiFocus 6, naming unnamed",
        "eye 0 2620x2533: first draw 8300 (900 draw(s)), vs 0x5C36AF051B98B9F1 ps 0xCFE84157BC76E921, depth write yes, b1 0x1EB2E751E20 (5376 bytes, first constant 0); rows not read (map)",
        "the pass's chosen rows (bound block no) equal the view axes (transposed) of: camera 0x241DC2E2960 (kind 3) caller +0x594E13, +0x594EAB, +0x594FE1, 5 call(s) n=5/6/7/8/9",
        "-> the chooser STRAYS: the rows equal a kind 3 camera's axes, not an eye camera's (this frame has 6 kind-5 call(s))",
        "episode 3/10: frame 6120, trigger naming named>unnamed (armed at frame 6090); journal foot=yes, GuiFocus unknown, naming unnamed, phase (0.2520, -0.1260) px",
        "screen 5040x2835: first draw 41 (22 draw(s)), vs 0xDFED8E1C9E191BEC ps 0x143AAE0597E2F7BF, depth write yes, b1 0x1EB2E751E20 (5376 bytes, first constant 0)",
        "rows 270..273 equal the composed rows of NO call of this frame: whatever composed them is not at the refresh (or came from another frame)",
        "eye ? 2620x2533: first draw 90 (4 draw(s)), vs 0x11A2B3C4D5E6F708 ps 0xCFE84157BC76E921, depth write yes, b1 not bound; rows not read (no-b1)",
        "the pass's chosen rows: none (valid=0: the pass chose no camera rows this frame: it is off, or nothing was treated)",
        "== reading the episodes (facts for H1, H2 and H3, no verdict) ==",
        "H1 (on-foot maps and menus never name a source): 1 on-foot episode(s) (journal foot=yes), 1 of them unnamed (no draw named the 2D screen's source): episode(s) 3",
        "episode 3: 2 signature(s) drew into a screen-sized depth; the first draw's rows equal no printed call's composed rows",
        "H3 (the cockpit's maps are driven by kind-5 eye cameras' rows): 2 aboard episode(s) (journal foot=no or off)",
        "episode 1 (key-on, GuiFocus 0): eye 0 depth rows equal kind 5 call(s); eye 1 depth rows equal kind 5 call(s); the pass's rows equal kind 5 call(s)' axes",
        "episode 2 (gui 0>6, GuiFocus 6): eye 1 depth rows equal kind 5 call(s); the pass's rows equal kind 3 call(s)' axes",
        "== on-foot naming runs (the `runs` lines summed over 2 5 s window(s); frames the journal says on foot: 413) ==",
        "named 1 0 0 0 0 1 1",
        "unnamed 2 1 0 1 0 0 0",
        "longest named run 211 frame(s), longest unnamed run 7 frame(s)",
        "H2 (the longest unnamed run in an on-foot world stays under 3 frames): 1 unnamed run(s) of 3 frames or more, and 3 of 1 or 2",
        "the run open at the last window: named for 211 frame(s) so far",
        "== the detour's CPU (the observer's two halves, 1 call in 16 timed; 2 `detour` line(s) over 2 5 s window(s)) ==",
        "898 frame(s), 52588 refresh call(s), 3289 timed",
        "estimated ms a frame: mean 0.344 over 2 window line(s) (lowest 0.155, highest 0.533); calls a frame: 58.6",
        "observed calls (the detour did not inject for them): 45838 call(s), 2866 timed; pre half: mean 3.32 us, longest 52 us; post half: mean 2.41 us, longest 2.7 us",
        "injected calls (the route's phase was written for them): 6750 call(s), 423 timed; pre half: mean 4.08 us, longest 61 us; post half: mean 2.7 us, longest 2.8 us",
    ]
    for w in want_episodes:
        if w not in flat:
            fail("the report on the fixture lacks %r:\n%s" % (w, out[out.find("== episodes"):out.find("== stage 2 verdict")]))
            break
    if out.index("== episodes") > out.index("== stage 2 verdict") or "== stage 2 verdict" not in out or "stage 2 verdict: PASS (6 PASS" not in out:
        fail("the episodes come after the stage 2 verdict, or the verdict changed with them in the log:\n%s" % out[-600:])

    def drop(log, prefix):
        return mutate(log, prefix, lambda line: "")

    episode_calls = ("call frame=31 ", "call frame=1560 ", "call frame=6120 ")   # the fixture's three episodes' call lines (no sequence is made of them)

    # A log with none of the new lines is the report it was: no episodes section, no sections of runs and detour, and the summary line without a count of episodes.
    legacy = text
    for prefix in ("episode ", "join ", "join-rows ", "join-more ", "join-draws ", "pass-rows ", "episodes ", "runs ", "detour ") + episode_calls:
        legacy = drop(legacy, prefix)
    rc, out = report(legacy)
    if rc != 0 or "== episodes" in out or "== on-foot naming runs" in out or "== the detour's CPU" in out or "episode(s)" in out.split("\n")[0] or \
            "stage 2 verdict: PASS (6 PASS" not in out or "3 call sequence(s), 8 eye draw(s)" not in out:
        fail("a census log with none of the episode lines (an older census) did not report exactly as before:\n%s" % out)
    # No `episodes` counters line: the section says so (and still prints the episodes).
    _, out = report(drop(text, "episodes "))
    if "no `vr camera census: episodes` line (the window's counters)" not in out or "== episodes (3 printed)" not in out:
        fail("a log with no episode counters line did not say so:\n%s" % out)
    # An episode armed and never printed (the log ends before its frame): the counters line says it, with its trigger and the frame it samples.
    armed_log = text
    for prefix in ("episode ", "join ", "join-rows ", "join-draws ", "pass-rows ") + episode_calls:
        armed_log = drop(armed_log, prefix)
    armed_log += \
        "[12:00:09.000] vr camera census: episodes windows=1 taken=4/10 triggers=6 skipped=2 state=armed trigger=gui:6>0 armed=7000 sample=7030\n"
    _, out = report(armed_log)
    if "4 episode(s) were armed and not printed" not in out or "one is armed now (trigger gui 6>0, armed at frame 7000, samples frame 7030)" not in out or \
            "none: no episode was sampled" not in out:
        fail("an armed episode that never printed was not reported:\n%s" % out)
    # The pass's rows line is missing, or valid=0.
    _, out = report(drop(text, "pass-rows ep=1 "))
    if "episode 1/10" not in out or "the pass's chosen rows: no `pass-rows` line for this episode" not in squash(out):
        fail("an episode with no pass-rows line did not say so:\n%s" % out)
    # The join's hook was never handed a draw (seen=0) is not 'no draw joined': the first says the route's per-draw path never reached the census; the second counts what was seen.
    never = sub(drop(drop(drop(text, "join ep=3 "), "join-rows ep=3 "), "join-more ep=3 "), "join-draws ep=3 seen=905 relevant=337 views=5 signatures=3",
                "join-draws ep=3 seen=0 relevant=0 views=0 signatures=0")
    _, out = report(never)
    part = squash(out)[squash(out).find("episode 3/10"):]
    if "the per-draw hook was handed 0 draw(s), 0 of them into a screen- or eye-sized depth, 0 depth view(s) resolved" not in part or \
            "!! the join's per-draw hook was handed NO draw in the sampled frame" not in part or "none: the hook saw" in part:
        fail("an episode whose join hook never ran was not told from one with no draw joined:\n%s" % part[:1200])
    quiet = sub(drop(drop(drop(text, "join ep=3 "), "join-rows ep=3 "), "join-more ep=3 "), "join-draws ep=3 seen=905 relevant=337 views=5 signatures=3",
                "join-draws ep=3 seen=905 relevant=0 views=5 signatures=0")
    _, out = report(quiet)
    part = squash(out)[squash(out).find("episode 3/10"):]
    if "none: the hook saw 905 draw(s) and none went into a depth of the 2D screen's size or an eye's size" not in part or "NO draw in the sampled frame" in part:
        fail("an episode whose draws saw no screen- or eye-sized depth was not named with its draw count:\n%s" % part[:1200])
    _, out = report(drop(text, "join-draws ep=3 "))
    if "no `join-draws` line: the hook's draw counts are not in this log" not in squash(out):
        fail("an episode with no join-draws line did not say so:\n%s" % out[out.find("episode 3/10"):][:900])
    # A rows line missing: the join says the rows were read and the line is not here (and does not read as 'no call matched').
    _, out = report(drop(text, "join-rows ep=1 sig=1 "))
    if "the rows were read but their `join-rows` line is not in this log" not in out:
        fail("a join signature whose rows line is missing was not named:\n%s" % out)
    # The calls the join matched are not in the log (the DLL's own list names them): said, never 'no call composed them'.
    no_eyes = "\n".join(l for l in text.split("\n") if not (": call frame=31 " in l and " kind=5 " in l))
    _, out = report(no_eyes)
    if "equal the composed rows of call(s) n=9,10,11, none of which is among the printed calls" not in squash(out) or \
            "equal the composed rows of NO call of this frame" in out.split("episode 2/10")[0]:
        fail("matched calls that were not printed were not named by their ordinals, or read as 'no call':\n%s" % out[out.find("== episodes"):out.find("episode 2/10")])
    # The DLL lists an ordinal that is not printed beside printed ones; and the reader's own match disagreeing with the DLL's is flagged.
    more = sub(text, "rows=read match=9,10,11", "rows=read match=9,10,11,99+3")
    _, out = report(more)
    if "the DLL also lists call(s) n=99 and 3 more, not among the printed lines" not in squash(out):
        fail("an ordinal the DLL listed that is not printed (and a count past its list) was not said:\n%s" % out[out.find("== episodes"):out.find("episode 2/10")])
    nudged = mutate(text, "call frame=31 n=10 ", lambda line: re.sub(r"rows=\[([^,\]]+)", lambda m: "rows=[%.9g" % (float(m.group(1)) + 5e-5), line, count=1))
    _, out = report(nudged)
    if "the DLL's match and this reader's disagree on call(s) n=10" not in squash(out):
        fail("a printed call whose rows the reader does not match, though the DLL listed it, was not flagged:\n%s" % out[out.find("== episodes"):out.find("episode 2/10")])
    # The chooser: rows that equal no call's axes name the nearest and how near; rows equal to an eye camera's axes do not say STRAYS.
    _, out = report(sub(text, "axes-match=5,6,7,8,9 how=transpose nearest=5 diff=0.000e+00", "axes-match=- how=identity nearest=15 diff=2.500e-03"))
    if "equal the view axes of NO recorded call; the nearest is call n=15 at a distance of 2.500e-03" not in squash(out) or "STRAYS" in out:
        fail("pass rows that equal no call's axes were not reported with the nearest call:\n%s" % out[out.find("episode 2/10"):out.find("episode 3/10")])
    # No kind-5 call in the frame: a kind-3 match is not a stray (there is no eye camera to stray from).
    solo = sub(text, "kinds=1:4,3:5,5:6", "kinds=1:4,3:5")
    _, out = report(solo)
    if "STRAYS" in out:
        fail("a frame with no kind-5 call reported a stray chooser:\n%s" % out)
    # The call lines are capped (more calls recorded than printed): said once for the episode, with the order the cap keeps; a header that promises more lines than the log holds is flagged.
    _, out = report(sub(text, "calls=14 recorded=14 printed=14", "calls=20 recorded=18 printed=14"))
    part = squash(out).split("episode 2/10")[0]
    if "the call lines are capped at 120 an episode (14 of 18 recorded calls printed)" not in part or "the counts above are of all 20 calls" not in part or \
            "call line(s) of the" in part or "(2 past the buffer: counted, not recorded)" not in part:
        fail("an episode whose calls were capped was not said so:\n%s" % part[:900])
    _, out = report(sub(text, "calls=14 recorded=14 printed=14", "calls=14 recorded=14 printed=20"))
    if "!! 14 call line(s) of the 20 the header says printed are in this log" not in squash(out):
        fail("a header that says more call lines printed than the log holds was not flagged:\n%s" % out[out.find("== episodes"):out.find("episode 2/10")])
    # Runs: no `runs` lines, no section; a log whose longest unnamed run is under 3 says 0 long runs.
    _, out = report(drop(text, "runs "))
    if "== on-foot naming runs" in out or "== the detour's CPU" not in out:
        fail("a log with no runs lines still printed the naming runs, or dropped the detour section:\n%s" % out)
    short = sub(sub(text, "unnamed=1:2,2:1,3:0,4-8:1,9-30:0,31-89:0,90+:0", "unnamed=1:2,2:1,3:0,4-8:0,9-30:0,31-89:0,90+:0"), "longest=named:211,unnamed:7", "longest=named:211,unnamed:2")
    _, out = report(short)
    if "H2 (the longest unnamed run in an on-foot world stays under 3 frames): 0 unnamed run(s) of 3 frames or more, and 3 of 1 or 2" not in squash(out) or \
            "longest named run 211 frame(s), longest unnamed run 2 frame(s)" not in squash(out):
        fail("runs that stay under three frames were not reported as none of 3 or more:\n%s" % out[out.find("== on-foot naming runs"):])
    # A run of exactly three unnamed frames is a long one (the bin of 3 counts): the release the design waits for.
    three = sub(text, "unnamed=1:2,2:1,3:0,4-8:1,9-30:0,31-89:0,90+:0", "unnamed=1:2,2:1,3:1,4-8:0,9-30:0,31-89:0,90+:0")
    _, out = report(three)
    if "H2 (the longest unnamed run in an on-foot world stays under 3 frames): 1 unnamed run(s) of 3 frames or more, and 3 of 1 or 2" not in squash(out):
        fail("an unnamed run of exactly three frames was not counted as one of three or more:\n%s" % out[out.find("== on-foot naming runs"):])
    # Detour: no lines, no section; no timed call at all says nothing was estimated.
    _, out = report(drop(text, "detour "))
    if "== the detour's CPU" in out or "== on-foot naming runs" not in out:
        fail("a log with no detour lines still printed the CPU section, or dropped the runs:\n%s" % out)
    # The mean estimate is weighted by each window's frames: a window of 45 frames counts a tenth of one of 448.
    _, out = report(sub(text, "windows=1 every=16 timed=observer-halves frames=450 ", "windows=1 every=16 timed=observer-halves frames=45 "))
    if not re.search(r"estimated ms a frame: mean 0\.49\d over 2 window line\(s\) \(lowest 0\.155, highest 0\.533\)", squash(out)):
        fail("the detour's mean estimate was not weighted by each window's frames:\n%s" % out[out.find("== the detour's CPU"):])
    untimed = re.sub(r"(vr camera census: detour [^\n]*?) est-ms-frame=\S+", r"\1 est-ms-frame=-", text)
    _, out = report(untimed)
    if "estimated ms a frame: none (no window had both calls and timed calls)" not in out:
        fail("detour lines with no estimate were not said so:\n%s" % out[out.find("== the detour's CPU"):])
    # The episode lines are in the log of a census that never had a legacy sequence (a cockpit-only session): the report still finishes and joins eye draws as it did.
    cockpit_only = text
    for prefix in ("sequence ", "call frame=4 ", "call frame=5 ", "call frame=6 ", "eye=", "eye-geometry "):
        cockpit_only = drop(cockpit_only, prefix)
    rc, out = report(cockpit_only)
    if rc != 0 or "== episodes (3 printed" not in out or "episode 1/10" not in out or "no eye draw was read back" not in out.lower():
        fail("a session with episodes and no on-foot sequence did not report both sides:\n%s" % out[:1500])

    # ---- the command line ----
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = main(["--file", fixture, "--camera-census"])
    if rc != 0 or "== which signal separates the eye cameras (A)-(F) ==" not in buf.getvalue() or \
            "== stage 2 verdict" not in buf.getvalue() or "== episodes (3 printed" not in buf.getvalue():
        fail("--camera-census through main() returned %d:\n%s" % (rc, buf.getvalue()))
    return ok


def self_test_periodic():
    """--tally periodic on fixtures in a temp directory: the line formats
    (the exact strings the C++ rigs pin), the clocks, the matching, and every
    way the report has to say something is missing. Returns ok."""
    import contextlib
    import io
    import shutil

    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("periodic: %s" % what)
            ok = False

    base_days = (datetime.date(2026, 9, 29) - EPOCH.date()).days

    def T(h, m, s):
        """A local time on 2026-09-29, on the common clock."""
        return base_days * DAY + h * 3600 + m * 60 + s

    def long_frame(stamp, ms, seq):
        # The native-path line perf_monitor.cpp writes, tail included.
        return ("[%s] monitor: LONG FRAME -- %.1f ms between Presents (runtime "
                "predicted period 11.1 ms), no WaitGetPoses, CPU busy, "
                "compositor, reprojection, or door samples; game creations: 0 "
                "textures, 0 buffers, 0 shaders (0.0 MB); EDVR events: none. "
                "This is frame 812; the flip timeline is not armed, so there "
                "are no table changes to order against it. runtime sequence "
                "%d, game work 5.20 ms.\r\n" % (stamp, ms, seq))

    def put(path, text):
        with open(path, "wb") as f:
            f.write(text.encode("utf-8"))

    def run(argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = main(argv)
        return rc, buf.getvalue()

    version = "0.17.0-5-gabcdef1"
    version_gfx = ("[07:15:30.002] version %s (build 68C0A1F2) -- this DLL was "
                   "linked 2026-09-28 20:34:39 UTC\r\n" % version)

    # --- the exact strings src/common/periodic_work.h's rig pins ------------
    rig = ("[10:00:00.101] periodic work: journal_reglob SLOW ms=3.412 at "
           "10:00:00.100 files=1893\r\n"
           "[10:00:30.000] periodic work: journal_reglob n=5 total=11.012 "
           "max=3.412 at 10:00:00.100 slow=4 files=1893\r\n"
           "[12:34:56.800] periodic work: op_quiet n=31 total=15.500 max=0.500 "
           "at 12:34:56.789 slow=0\r\n")
    sc = scan_flight_log(rig, "gfx", base_days)
    st = sc["ops"].get("journal_reglob", {})
    check(st.get("windows") == 1 and st.get("runs") == 5 and
          st.get("slow_runs") == 4 and st.get("slow_lines") == 1 and
          abs(st.get("total_ms", 0) - 11.012) < 1e-9 and
          st.get("max_ms") == 3.412 and st.get("max_ctx") == "files=1893" and
          abs(st.get("max_at", 0) - T(10, 0, 0.1)) < 1e-6,
          "the SLOW and summary forms with a context did not parse: %r" % st)
    st = sc["ops"].get("op_quiet", {})
    check(st.get("runs") == 31 and st.get("max_ctx") == "" and
          st.get("slow_runs") == 0 and
          abs(st.get("max_at", 0) - T(12, 34, 56.789)) < 1e-6,
          "the summary form without a context did not parse: %r" % st)
    check(len(sc["events"]) == 3 and sc["unparsed"]["count"] == 0,
          "expected 3 events and no unparsed lines from the rig lines")

    tmp = tempfile.mkdtemp(prefix="edvr_log_periodic_")
    try:
        # --- one flight: graphics log (local prefix) + runtime log (UTC) ---
        # Local is UTC-7 here. (a) sits beside a summary's `at` with no SLOW
        # line for it; (b) is 30 ms after a journal_reglob SLOW line and (c)
        # 500 ms after it; (d) and the runtime's cycle (e) are beside the
        # runtime's frame_cycle_report, whose `at` is local in a UTC-prefixed
        # log. The VTableHook line says "monitor: LONG FRAME" and is no frame.
        gfx_text = (
            "[07:15:30.001] EDVR log -- unofficial VR fixes for Elite Dangerous: "
            "Odyssey\r\n" + version_gfx +
            long_frame("07:15:45.300", 33.0, 4001) +
            "[07:15:52.001] periodic work: journal_reglob SLOW ms=3.412 at "
            "07:15:52.000 files=1893\r\n"
            "[07:16:00.100] periodic work: journal_status n=30 total=6.000 "
            "max=0.500 at 07:15:45.250 slow=0\r\n"
            "[07:16:00.101] periodic work: journal_reglob n=5 total=11.012 "
            "max=3.412 at 07:15:52.000 slow=4 files=1893\r\n"
            "[07:16:02.101] periodic work: journal_reglob SLOW ms=2.500 at "
            "07:16:02.100 files=1893\r\n" +
            long_frame("07:16:02.130", 45.3, 4100) +
            "[07:16:02.131] VTableHook device: monitor: LONG FRAME -- this line "
            "is about FRAME 811 (the frame in progress is 812), 3.0 s after "
            "the timeline armed.\r\n" +
            long_frame("07:16:02.600", 30.0, 4110) +
            long_frame("07:16:29.930", 60.0, 4300) +
            "[07:16:31.000] monitor: 4 dropped or long frames were logged this "
            "session (of 60 at most).\r\n")
        rt_text = (
            "2026-09-29 14:15:31.205 UTC pid=4242 tid=1 module_init,version=%s,"
            "durable_log=1\r\n"
            "2026-09-29 14:16:30.010 UTC pid=4242 tid=9 native_long_cycle,"
            "sequence=4300,cycle_ms=58.0000,period_ms=11.1000,"
            "game_before_first_submit=3.0000,units=wall_ms\r\n"
            "2026-09-29 14:16:30.020 UTC pid=4242 tid=9 periodic work: "
            "frame_cycle_report n=2698 total=41.000 max=6.250 at 07:16:29.900 "
            "slow=1 samples=2700\r\n"
            "2026-09-29 14:16:31.000 UTC pid=4242 tid=9 native_long_cycle_summary,"
            "count=88,logged=1,threshold=2x_period\r\n" % version)
        flight = os.path.join(tmp, "flight")
        os.makedirs(flight)
        gfx_path = os.path.join(flight, "edvr_gfx_20260929_071530.log")
        rt_path = os.path.join(flight, "edvr_openxr_20260929_071531_200_4242.log")
        put(gfx_path, gfx_text)
        put(rt_path, rt_text)
        # A runtime log from another day, which must not be the one paired.
        put(os.path.join(flight, "edvr_openxr_20260928_101010_000_1111.log"),
            "2026-09-28 17:10:10.000 UTC pid=1111 tid=1 module_init,version=%s,"
            "durable_log=1\r\n" % version)

        # The clocks. Local is UTC-7, read off the runtime log's own file name.
        gscan, rscan, clock = scan_flight(gfx_path, gfx_text, rt_path, rt_text)
        check(clock["to_local"] == datetime.timedelta(hours=-7) and
              "file name" in clock["how"],
              "the UTC offset was not read off the file name: %r" % (clock,))
        check(len(gscan["frames"]) == 4 and len(rscan["frames"]) == 1,
              "long frame counts: %d LONG FRAME (want 4, the VTableHook line "
              "is no frame), %d native_long_cycle (want 1, its summary line is "
              "no cycle)" % (len(gscan["frames"]), len(rscan["frames"])))
        check(abs(rscan["frames"][0]["t"] - T(7, 16, 30.010)) < 1e-6,
              "a UTC-prefixed native_long_cycle did not land on local time")
        check(abs(rscan["ops"]["frame_cycle_report"]["max_at"] -
                  T(7, 16, 29.900)) < 1e-6,
              "the `at` of a UTC-prefixed frame_cycle_report is not the local "
              "time it names")
        check(rscan["cycle_summary"] == (88, 1) and
              gscan["frame_cap"] == (4, 60),
              "the closing count lines did not parse: %r %r"
              % (rscan["cycle_summary"], gscan["frame_cap"]))
        check(gscan["unparsed"]["count"] == 0 and rscan["unparsed"]["count"] == 0,
              "lines of the real formats were left unparsed: %r %r"
              % (gscan["unparsed"], rscan["unparsed"]))
        check(gscan["ops"]["journal_reglob"]["slow_lines"] == 2 and
              gscan["ops"]["journal_reglob"]["windows"] == 1,
              "journal_reglob: %r" % gscan["ops"]["journal_reglob"])

        events = gscan["events"] + rscan["events"]
        frames = gscan["frames"] + rscan["frames"]
        span = max(s["last"] - s["first"] for s in (gscan, rscan))
        res = analyse_periodic(events, frames, 0.100, span)
        # The summary and the SLOW line that name one run are one event.
        check([len(res["events"][o]) for o in
               ("frame_cycle_report", "journal_reglob", "journal_status")]
              == [1, 2, 1],
              "distinct events per operation: %r"
              % {o: len(v) for o, v in res["events"].items()})
        # (a) is attributed through a window's `at` alone; (b), 30 ms after a
        # journal_reglob SLOW line, to it; (c), 500 ms away, to nothing; (d)
        # and (e) to the runtime's report, across the two clocks.
        check(res["hits"] == {("journal_status", "gfx"): 1,
                              ("journal_reglob", "gfx"): 1,
                              ("frame_cycle_report", "gfx"): 1,
                              ("frame_cycle_report", "rt"): 1},
              "hits at +/-100 ms: %r" % (res["hits"],))
        check(res["counts"] == {"gfx": 4, "rt": 1} and
              res["touched"] == {"gfx": 3, "rt": 1},
              "counts %r touched %r" % (res["counts"], res["touched"]))
        check(all(abs(m["frame"]["t"] - T(7, 16, 2.6)) > 1e-3
                  for m in res["matches"]) and len(res["matches"]) == 4,
              "the frame 500 ms from the SLOW line was attributed to it")
        check([round(m["frame"]["t"] - T(7, 16, 0)) for m in res["matches"]] ==
              [30, 30, 2, -15],
              "matches are not largest event first: %r"
              % [m["frame"]["t"] - T(7, 16, 0) for m in res["matches"]])
        top = res["top"]["gfx"]
        check([round(r["frame"]["ms"], 1) for r in top] == [60.0, 45.3, 33.0, 30.0],
              "the longest frames are not in order")
        if len(top) == 4:
            far = top[3]
            check(far["near"]["event"]["op"] == "journal_reglob" and
                  abs(far["near"]["offset_ms"] + 500.0) < 1e-3 and
                  not far["hit"],
                  "the nearest event to the frame 500 ms away: %r"
                  % (far["near"],))
            check(top[1]["hit"] and top[1]["near"]["gap_s"] == 0.0 and
                  abs(top[1]["near"]["offset_ms"] + 30.0) < 1e-3,
                  "the frame 30 ms after the SLOW line: %r" % (top[1]["near"],))
        expect = sum(2 * (f["ms"] / 1000.0 + 0.2) / span
                     for f in frames if f["src"] == "gfx")
        # (Times are ~8e8 s on the common clock, so a difference carries ~1e-7 s.)
        check(abs(res["chance"][("journal_reglob", "gfx")] - expect) < 1e-6,
              "chance is not events x padded frame / span: %r vs %r"
              % (res["chance"][("journal_reglob", "gfx")], expect))
        tight = analyse_periodic(events, frames, 0.010, span)
        check(("frame_cycle_report", "rt") not in tight["hits"] and
              tight["touched"] == {"gfx": 2, "rt": 0},
              "--window-ms 10 still matched the cycle 52 ms from the run: %r"
              % (tight["hits"],))
        check(nearest_event(frames[0], []) is None, "nearest_event of nothing")
        # The window applies after a frame's end too, and a long frame owns
        # everything that ended inside it however long it ran.
        after = {"op": "x", "kind": "slow", "at": T(8, 0, 0.050), "ms": 2.0,
                 "ctx": "", "src": "gfx"}
        short = {"src": "gfx", "t": T(8, 0, 0), "ms": 30.0, "seq": 0}
        check(analyse_periodic([after], [short], 0.100, 60.0)["hits"] ==
              {("x", "gfx"): 1} and
              analyse_periodic([after], [short], 0.040, 60.0)["hits"] == {},
              "an event ending 50 ms after the frame did / did not match")
        inside = dict(after, at=T(8, 0, 9.0))
        long_one = {"src": "gfx", "t": T(8, 0, 10.0), "ms": 1117.1, "seq": 0}
        check(analyse_periodic([inside], [long_one], 0.010, 60.0)["hits"] ==
              {("x", "gfx"): 1},
              "an event that ended inside a 1.1 s frame did not match")
        pairs, median = clock_check(frames)
        check(pairs == 1 and abs(median + 80.0) < 1e-3,
              "the two clocks were not checked against each other by "
              "sequence: %r %r" % (pairs, median))

        # The report itself, through main(), on the directory it discovers.
        rc, out = run(["--dir", flight, "--tally", "periodic"])
        check(rc == 0, "periodic report exited %d" % rc)
        for want in ("UTC-07:00", "from the log's file name", "journal_reglob",
                     "frame_cycle_report", "07:16:30.010",
                     "graphics log spans 07:15:30.001 .. 07:16:31.000",
                     "LONG FRAME 3 of 4; native_long_cycle 1 of 1",
                     "LONG FRAME lines, graphics log: 4",
                     "the runtime counted 88 over twice its predicted period",
                     "1 LONG FRAME line(s) share a runtime sequence",
                     "-80 ms apart at the median",
                     "long frames with a periodic event beside them",
                     "journal_reglob SLOW 2.500 (-30)",
                     "the 4 longest LONG FRAME line(s)"):
            check(want in out, "the report lacks %r" % want)
        check("edvr_openxr_20260929_071531_200_4242.log" in out and
              "edvr_openxr_20260928" not in out,
              "the runtime log paired is not the one that opened nearest")
        rc, out = run(["--dir", flight, "--tally", "periodic", "--window-ms",
                       "10"])
        check(rc == 0 and "LONG FRAME 2 of 4; native_long_cycle 0 of 1" in out,
              "--window-ms did not narrow the match (rc=%d)" % rc)
        # --version and --expect-build keep their meaning with the new mode.
        rc, out = run(["--dir", flight, "--tally", "periodic", "--version"])
        check(rc == 0 and "periodic work vs long frames" not in out,
              "--version did not stop before the report")
        rc, out = run(["--dir", flight, "--tally", "periodic",
                       "--expect-build", version])
        check(rc == 0 and "runtime build matches" in out and
              "periodic work vs long frames" in out,
              "a matching build did not run the report (rc=%d)" % rc)
        rc, out = run(["--dir", flight, "--tally", "periodic",
                       "--expect-build", "0.17.0-9-g1234567"])
        check(rc == 2 and "BUILD MISMATCH" in out and
              "periodic work vs long frames" not in out,
              "a graphics-log build mismatch did not exit 2 (rc=%d)" % rc)

        # A runtime log from another build: refused with --expect-build,
        # named as a warning without it.
        stale = os.path.join(tmp, "stale")
        os.makedirs(stale)
        put(os.path.join(stale, "edvr_gfx_20260929_071530.log"), gfx_text)
        put(os.path.join(stale, "edvr_openxr_20260929_071531_200_4242.log"),
            rt_text.replace(version, "0.17.0-9-g1234567"))
        rc, out = run(["--dir", stale, "--tally", "periodic",
                       "--expect-build", version])
        check(rc == 2 and "BUILD MISMATCH (runtime log)" in out and
              "periodic work vs long frames" not in out,
              "a stale runtime log was not refused (rc=%d)" % rc)
        rc, out = run(["--dir", stale, "--tally", "periodic"])
        check(rc == 0 and "WARNING: the runtime log is from build "
              "0.17.0-9-g1234567" in out,
              "two builds in one flight went unremarked (rc=%d)" % rc)

        # No runtime log: said plainly, and the graphics half still reports.
        solo = os.path.join(tmp, "solo")
        os.makedirs(solo)
        put(os.path.join(solo, "edvr_gfx_20260929_071530.log"), gfx_text)
        rc, out = run(["--dir", solo, "--tally", "periodic"])
        # (d) needed the runtime's frame_cycle_report, which is not here.
        check(rc == 0 and "runtime log: NONE FOUND" in out and
              "LONG FRAME 2 of 4" in out and "native_long_cycle lines" not in out,
              "a missing runtime log was not reported (rc=%d)" % rc)
        # --infer-runs. slowop runs every 4 s and logs two SLOW lines a window;
        # the frame ends 10 ms after a run nobody logged (08:00:52.000), 4 s
        # from the nearest logged one. sparse has the same cadence but one
        # logged time (its window max), and dense runs every 0.1 s with two on
        # its grid: the first has too little to confirm a schedule and the
        # second spaces its runs too closely for a window around one to test
        # anything, so neither is inferred.
        infer_dir = os.path.join(tmp, "infer")
        os.makedirs(infer_dir)
        infer_text = (
            "[08:00:00.001] version %s (build 68C0A1F2)\r\n"
            "[08:00:10.000] periodic work: dense n=100 total=10.000 max=0.500 "
            "at 08:00:05.000 slow=0\r\n"
            "[08:00:12.001] periodic work: dense SLOW ms=2.100 at 08:00:12.000\r\n"
            "[08:00:16.001] periodic work: dense SLOW ms=2.200 at 08:00:16.000\r\n"
            "[08:00:20.000] periodic work: dense n=100 total=10.000 max=2.200 "
            "at 08:00:16.000 slow=2\r\n"
            "[08:00:36.000] periodic work: slowop n=9 total=22.500 max=2.700 "
            "at 08:00:24.003 slow=9 files=5\r\n"
            "[08:00:36.000] periodic work: sparse n=9 total=4.500 max=0.900 "
            "at 08:00:30.000 slow=0\r\n"
            "[08:00:48.001] periodic work: slowop SLOW ms=2.500 at 08:00:48.000 "
            "files=5\r\n" % version +
            long_frame("08:00:52.010", 30.0, 7000) +
            "[08:00:56.001] periodic work: sparse SLOW ms=0.700 at 08:00:56.000\r\n"
            "[08:01:00.002] periodic work: slowop SLOW ms=2.700 at 08:01:00.001 "
            "files=5\r\n"
            "[08:01:12.000] periodic work: slowop n=9 total=22.500 max=2.700 "
            "at 08:01:00.001 slow=9 files=5\r\n"
            "[08:01:12.000] periodic work: sparse n=9 total=4.500 max=0.700 "
            "at 08:00:56.000 slow=0\r\n")
        put(os.path.join(infer_dir, "edvr_gfx_20260929_080000.log"), infer_text)
        isc = scan_flight_log(infer_text, "gfx", base_days, 8 * 3600)
        more, notes = infer_runs(isc, 0.1)
        check(sorted(notes) == ["slowop"] and notes["slowop"]["runs"] == 7 and
              notes["slowop"]["windows"] == 1 and
              abs(notes["slowop"]["period"] - 4.0) < 1e-6,
              "infer_runs notes: %r" % (notes,))
        check(sorted(round(e["at"] - T(8, 0, 0)) for e in more) ==
              [40, 44, 52, 56, 64, 68, 72] and
              all(e["kind"] == "est" and abs(e["ms"] - 2.5) < 1e-9 for e in more),
              "inferred runs: %r" % [(e["op"], e["at"] - T(8, 0, 0)) for e in more])
        rc, out = run(["--dir", infer_dir, "--tally", "periodic"])
        check(rc == 0 and "LONG FRAME 0 of 1" in out and
              "inferred runs" not in out and " 3/18 " in out,
              "the default counted a run nobody logged (rc=%d):\n%s" % (rc, out))
        rc, out = run(["--dir", infer_dir, "--tally", "periodic", "--infer-runs"])
        check(rc == 0 and "LONG FRAME 1 of 1" in out and
              "inferred runs: slowop, 7 run(s) in 1 window(s), every 4.00 s" in out
              and "slowop est 2.500 (-10)" in out and " 3+7/18 " in out and
              "inferred runs: sparse" not in out and
              "inferred runs: dense" not in out,
              "--infer-runs did not place the unlogged run (rc=%d):\n%s"
              % (rc, out))
        # A zero window still infers: the spacing floor is a quarter second.
        rc, out = run(["--dir", infer_dir, "--tally", "periodic", "--infer-runs",
                       "--window-ms", "0"])
        check(rc == 0 and "inferred runs: slowop" in out and
              "inferred runs: dense" not in out,
              "--window-ms 0 changed the inference (rc=%d)" % rc)

        # The C++ side rewords a line: counted and shown, not a quiet flight.
        drift = os.path.join(tmp, "drift")
        os.makedirs(drift)
        put(os.path.join(drift, "edvr_gfx_20260929_071530.log"), gfx_text +
            "[07:16:32.000] periodic work: journal_tail n=5 total=1.000 "
            "max=0.400 at 07:16:31.900 slows=0\r\n"
            "[07:16:33.000] monitor: LONG FRAME -- about 40 ms between Presents\r\n")
        rc, out = run(["--dir", drift, "--tally", "periodic"])
        check(rc == 0 and "WARNING: 2 line(s) in the graphics log open like" in out
              and "slows=0" in out and "LONG FRAME lines, graphics log: 4" in out,
              "reworded lines went unremarked (rc=%d)" % rc)
        # And one too far from the graphics log's start to be its own.
        far_rt = os.path.join(tmp, "farrt")
        os.makedirs(far_rt)
        put(os.path.join(far_rt, "edvr_gfx_20260929_071530.log"), gfx_text)
        put(os.path.join(far_rt, "edvr_openxr_20260929_080000_000_1.log"),
            rt_text)
        rc, out = run(["--dir", far_rt, "--tally", "periodic"])
        check(rc == 0 and "runtime log: NONE FOUND" in out and
              "more than 15 min" in out,
              "a runtime log 44 min away was paired (rc=%d)" % rc)

        # No `periodic work:` lines at all: not "all was well".
        bare = os.path.join(tmp, "bare")
        os.makedirs(bare)
        put(os.path.join(bare, "edvr_gfx_20260929_070000.log"),
            "[07:00:00.002] version %s (build 68C0A1F2)\r\n" % version +
            long_frame("07:01:30.000", 40.0, 9))
        rc, out = run(["--dir", bare, "--tally", "periodic"])
        check(rc == 0 and "NO `periodic work:` lines in the graphics log" in out
              and "The timing was never wired into this build" in out and
              "no periodic events to lay the long frames against" in out,
              "a log with no periodic lines was not reported as such (rc=%d):"
              "\n%s" % (rc, out))
        put(os.path.join(bare, "edvr_gfx_20260929_070000.log"),
            "[07:00:00.002] version %s (build 68C0A1F2)\r\n" % version +
            long_frame("07:00:10.000", 40.0, 9))
        rc, out = run(["--dir", bare, "--tally", "periodic"])
        check(rc == 0 and "spans only 10 s" in out and
              "NO `periodic work:`" not in out,
              "a 10 s log was told its timing was never wired in")
        # A graphics log with no long frames, and a runtime log named by hand
        # (a quiet one, and 15 minutes after the graphics log) whose span does
        # not overlap it.
        put(os.path.join(bare, "edvr_gfx_20260929_070000.log"),
            "[07:00:00.002] version %s (build 68C0A1F2)\r\n"
            "[07:01:30.000] periodic work: journal_tail n=61 total=0.5 "
            "max=0.013 at 07:01:16.832 slow=0 bytes=0\r\n" % version)
        quiet_rt = os.path.join(bare, "edvr_openxr_20260929_071531_205_4242.log")
        put(quiet_rt, "2026-09-29 14:15:31.205 UTC pid=4242 tid=1 "
            "module_init,version=%s,durable_log=1\r\n" % version)
        rc, out = run(["--file", os.path.join(bare, "edvr_gfx_20260929_070000.log"),
                       "--tally", "periodic", "--runtime-file", quiet_rt])
        check(rc == 0 and "no long frames in either log" in out and
              "the two logs' time spans do not overlap" in out and
              "not seen in the graphics log: journal_status" in out,
              "no-long-frames / disjoint spans / not-seen (rc=%d):\n%s"
              % (rc, out))
        rc, out = run(["--file", os.path.join(bare, "edvr_gfx_20260929_070000.log"),
                       "--tally", "periodic", "--runtime-file",
                       os.path.join(tmp, "nope.log")])
        check(rc == 1 and "no such runtime log" in out,
              "a missing --runtime-file did not exit 1")
        rc, out = run(["--dir", flight, "--tally", "periodic", "--tag",
                       "openxr"])
        check(rc == 1 and "reads the graphics log" in out,
              "--tally periodic accepted a runtime --tag (rc=%d)" % rc)

        # A flight across midnight: the graphics prefix has no date. The frame
        # (00:00:00.010, a day on) ends 70 ms after a run that finished at
        # 23:59:59.940, and the summary line written after midnight names an
        # `at` from before it. The budget variant of the LONG FRAME line.
        night = os.path.join(tmp, "night")
        os.makedirs(night)
        night_text = (
            "[23:59:00.001] version %s (build 68C0A1F2)\r\n"
            "[23:59:59.950] periodic work: journal_tail SLOW ms=2.500 at "
            "23:59:59.940 bytes=512\r\n"
            "[00:00:00.010] monitor: LONG FRAME -- 40.0 ms between Presents "
            "(budget 11.1), of which the thread waited 3.0 in Present (busy "
            "37.0); the game's creations in it: 0 textures, 0 buffers (0.0 MB "
            "together), 0 shaders; EDVR this frame: boundary 0.10 ms.\r\n"
            "[00:00:00.100] periodic work: journal_status n=10 total=1.000 "
            "max=0.400 at 23:59:59.800 slow=0\r\n"
            "[00:00:05.000] a line after midnight\r\n" % version)
        night_path = os.path.join(night, "edvr_gfx_20260929_235900.log")
        put(night_path, night_text)
        ns, _, _ = scan_flight(night_path, night_text)
        frame = ns["frames"][0]
        tail = [e for e in ns["events"] if e["op"] == "journal_tail"][0]
        status = [e for e in ns["events"] if e["op"] == "journal_status"][0]
        check(abs((frame["t"] - tail["at"]) - 0.070) < 1e-6 and
              abs((frame["t"] - status["at"]) - 0.210) < 1e-6 and
              frame["t"] > T(23, 59, 59.9),
              "midnight: frame %.3f after the run, %.3f after the summary's "
              "`at`" % (frame["t"] - tail["at"], frame["t"] - status["at"]))
        check(abs(ns["last"] - (T(24, 0, 5.0))) < 1e-6,
              "the last line of a log that crossed midnight is not on the "
              "next day")
        rc, out = run(["--dir", night, "--tally", "periodic"])
        check(rc == 0 and "00:00:00.010 +1d" in out and
              "journal_tail SLOW 2.500 (-70)" in out and
              "journal_status" in out,
              "the report across midnight (rc=%d):\n%s" % (rc, out))

        # Clock conversion: a zone east of UTC, and the ways the file name
        # cannot give the offset.
        first = "2026-09-29 07:15:31.205 UTC pid=1 tid=1 module_init\r\n"
        delta, how, _ = runtime_to_local(
            "edvr_openxr_20260929_124531_200_4242.log", first)
        check(delta == datetime.timedelta(hours=5, minutes=30) and
              "file name" in how, "UTC+05:30: %r %r" % (delta, how))
        check(_fmt_zone(delta) == "UTC+05:30" and
              _fmt_zone(datetime.timedelta(hours=-7)) == "UTC-07:00",
              "zone text")
        for name in ("edvr_openxr_20260929_071531.log",           # no fields
                     "edvr_openxr_20260929_124931_200_4242.log",  # off by 4 min
                     "renamed.log"):
            delta, how, _ = runtime_to_local(name, first)
            check("time zone" in how,
                  "%s did not fall back to the machine's zone: %r" % (name, how))
        delta, how, first_utc = runtime_to_local("x.log", "no stamps here\r\n")
        check(first_utc is None and delta == datetime.timedelta(0) and
              "treated as UTC" in how, "a runtime log with no timestamps")
        check(fmt_clock(T(7, 16, 2.6) + DAY, base_days) == "07:16:02.600 +1d" and
              fmt_clock(T(23, 59, 59.9996), base_days) == "00:00:00.000 +1d",
              "fmt_clock day suffix or rounding: %r %r"
              % (fmt_clock(T(7, 16, 2.6) + DAY, base_days),
                 fmt_clock(T(23, 59, 59.9996), base_days)))
    except Exception:
        # A parser that stopped finding its lines fails here, not in a flight.
        import traceback
        traceback.print_exc()
        print("periodic: the checks stopped at an exception (above)")
        ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


if __name__ == "__main__":
    sys.exit(main())
