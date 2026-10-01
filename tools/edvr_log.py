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
(`vr world route refusal 5s:`, the stage 2 experiment build): the report gets a section
with the share of the treated pixels whose history the resolver's prep refused, per
window and by cause (stale slot, masked record, ...), the state of the steady-detail
key and of the refusal view in each window, and totals by key state; (v) also checks
the weapon's role (inj-fp against the fold-in's mode counts and the struct's fov range).
A log with no census lines exits 1; the verdict never changes the exit code (read
its lines).

Exit 0 when a log was read, 1 when none was found (or --camera-census found no
census line), 2 when --expect-build did not match (--tally periodic checks the
runtime log against it too).
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
# two values when the first-person camera's tighter one is in it) or `fov=-` (no frustum was read), and, with the census key on,
# a third line follows the two (src/d3d11/vr_world_route_math.h vrWorldFormatRefusalWindow):
#   vr world route refusal 5s: census=on|off every=N treated=N asked=N sampled=N read=N dropped=N size=WxH pixels=N refused=N
#       refused-pct=X stale=N masked=N corrupt=N sentinel=N unreprojectable=N camera=N range=N depth=N weapon=N other=N
#       forgiven=N steady-detail=on|off view=on|off
# (`pixels` is what the read-back samples examined, `refused` the pixels whose history the prep refused, by cause; `forgiven` the stale
# pixels the steady-detail key sent to the camera term instead; "ran, 0 refused" is pixels > 0 and refused=0, "never ran" is
# treated=0, asked=0 or read=0.) The route also logs a line when the steady-detail key or the refusal view changes
# (`steady-detail is ON|OFF from frame=`, `the refusal view is ON|OFF from frame=`), see ROUTE_EVENT_MARKERS.
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
    ("stale", "the engine slot's depth was not the pixel's: a later draw overdrew it"),
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
    printed (inj, role, phase) is None or `absent`."""
    c = {"lines": 0, "unparsed": 0, "windows": [], "cameras": {}, "order": [], "changes": {},
         "sequences": [], "eyes": [], "geometry": {}, "threads": [], "info": []}
    current = None
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
    """{key: [line, ...]} for every line of the log that carries one of ROUTE_EVENT_MARKERS' phrases."""
    events = {}
    for raw in text.splitlines():
        for key, needle in ROUTE_EVENT_MARKERS:
            if needle in raw:
                events.setdefault(key, []).append(raw.strip())
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
    engaged, and while samples of a window that has just ended are still draining), each joined to the route's own line and inject line
    of the same window (the route prints route, inject, refusal, in that order; a missing one is None). A list of dicts: ts, kv (every
    token), census (census=on), steady and view (the tokens' text), w and h (size=), pct (refused-pct), route and inject (the other
    two lines' tokens), the counters as ints (None when a token is absent or not a number: treated, asked, sampled, read, dropped,
    every, pixels, refused, forgiven) and causes {name: int or None} for every name of REFUSAL_CAUSES."""
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
        for key in ("every", "treated", "asked", "sampled", "read", "dropped", "pixels", "refused", "forgiven"):
            w[key] = _cint(kv.get(key))
        w["causes"] = {name: _cint(kv.get(name)) for name, _ in REFUSAL_CAUSES}
        out.append(w)
        route = inject = None
    return out


def refusal_state(w):
    """What one refusal line says happened in its window. `measured`: samples were read back and the shares are real (including a window
    that measured and found nothing refused: pixels > 0, refused=0). The rest are windows that measured nothing, each for its own
    reason: `idle` (the route treated no frame), `no-ask` (it treated frames and none asked for the census), `no-sample` (fewer asks than
    one sample takes), `no-read` (samples were dispatched and none came back), `unreadable` (a counter did not parse)."""
    if any(w[k] is None for k in ("treated", "asked", "sampled", "read", "pixels", "refused")):
        return "unreadable"
    if w["read"] and w["pixels"]:
        return "measured"
    if not w["treated"]:
        return "idle"
    if not w["asked"]:
        return "no-ask"
    if not w["sampled"]:
        return "no-sample"
    return "no-read"


def _pct(n, pixels):
    return "%.3f%%" % (100.0 * n / pixels) if pixels else "-"


def refusal_findings(windows):
    """[(level, text)] about what the refusal lines do not support believing (WARN: a number that contradicts another, a key that did not
    take effect, a census that never measured) and what is worth knowing (note)."""
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
        elif s == "measured":
            causes = w["causes"]
            if w["refused"] > w["pixels"]:
                out.append(("WARN", "%s: refused %d exceeds the pixels examined, %d: the census counts a pixel once, so one of the two is wrong"
                            % (stamp, w["refused"], w["pixels"])))
            if w["steady"] == "on" and (causes["stale"] or 0) > 0:
                out.append(("WARN", "%s: steady-detail=on and %d stale pixel(s) were still refused: the key's rule did not reach the resolver "
                            "(with it on, a stale slot takes the camera term and is counted as forgiven)" % (stamp, causes["stale"])))
            if w["steady"] == "off" and (w["forgiven"] or 0) > 0:
                out.append(("WARN", "%s: %d stale pixel(s) were forgiven while steady-detail=off: the line's key state and the resolver's disagree"
                            % (stamp, w["forgiven"])))
            if (w["dropped"] or 0) > 0:
                out.append(("note", "%s: %d sample(s) were skipped because the read-back ring was full (the shares are unaffected, the sample "
                            "count is lower)" % (stamp, w["dropped"])))
            if (causes["other"] or 0) > 0:
                out.append(("note", "%s: %d refused pixel(s) are of a class the census cannot name (`other`)" % (stamp, causes["other"])))
        if w["view"] == "on":
            out.append(("note", "%s: the refusal view was painting: the headset showed the prep's classification, not the world" % stamp))
    measured = [w for w, s in zip(windows, states) if s == "measured"]
    treated = sum(w["treated"] or 0 for w in windows)
    if windows and not measured:
        if treated:
            out.append(("WARN", "the census never measured: the route treated %d frame(s) over %d window(s) and no sample was read back"
                        % (treated, len(windows))))
        else:
            out.append(("note", "the census was on for %d window(s) and the route treated no frame in any of them: nothing was measured "
                        "(not owning the world: a ship, a menu, or the route key off)" % len(windows)))
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
        print("none: no `vr world route refusal 5s:` line in this log (advanced.vr_camera_census was off, the route never engaged, or this "
              "build predates the census)")
        return []
    states = [refusal_state(w) for w in windows]
    for w, s in zip(windows, states):
        route = w["route"] or {}
        inject = w["inject"] or {}
        context = "route state=%s jitter=%s fp-mode %s, inj-fp %s, struct fov %s" % (
            route.get("state", "?"), route.get("jitter", "?"), route.get("fp-mode", "?"), inject.get("inj-fp", "?"), inject.get("fov", "?"))
        head = "%s steady-detail=%s view=%s census=%s" % (w["ts"] or "(no stamp)", w["steady"], w["view"], "on" if w["census"] else "off")
        if s == "measured":
            named = [(name, w["causes"][name]) for name, _ in REFUSAL_CAUSES if w["causes"][name]]
            mix = ", ".join("%s %s" % (name, _pct(n, w["pixels"])) for name, n in named) if named else "none refused"
            print("%s: MEASURED %d sample(s) of %dx%d (%d asked, %d dispatched, %d dropped), treated %d; pixels %d; refused %s (%d): %s; "
                  "forgiven %s | %s"
                  % (head, w["read"], w["w"] or 0, w["h"] or 0, w["asked"], w["sampled"], w["dropped"] or 0, w["treated"], w["pixels"],
                     _pct(w["refused"], w["pixels"]), w["refused"], mix, _pct(w["forgiven"] or 0, w["pixels"]), context))
        else:
            reason = {
                "idle": "the route treated no frame in this window (nothing was measured)",
                "no-ask": "the route treated %s frame(s) and none asked for the census" % w["treated"],
                "no-sample": "%s ask(s), fewer than one sample's worth (every %s)" % (w["asked"], w["every"]),
                "no-read": "%s sample(s) dispatched, none read back" % w["sampled"],
                "unreadable": "a counter did not parse",
            }[s]
            print("%s: NOT MEASURED: %s | %s" % (head, reason, context))
    measured = [(w, s) for w, s in zip(windows, states) if s == "measured"]
    keys = []
    for w, _ in measured:
        if w["steady"] not in keys:
            keys.append(w["steady"])
    for key in keys:
        group = [w for w, _ in measured if w["steady"] == key]
        pixels = sum(w["pixels"] for w in group)
        refused = sum(w["refused"] for w in group)
        forgiven = sum(w["forgiven"] or 0 for w in group)
        totals = {name: sum(w["causes"][name] or 0 for w in group) for name, _ in REFUSAL_CAUSES}
        mix = ", ".join("%s %s" % (name, _pct(n, pixels)) for name, n in totals.items() if n)
        of_refused = ", ".join("%s %.1f%%" % (name, 100.0 * n / refused) for name, n in totals.items() if n) if refused else ""
        print("totals, steady-detail=%s: %d measured window(s), %d sample(s), pixels %d, refused %s (%d)%s; forgiven %s%s"
              % (key, len(group), sum(w["read"] for w in group), pixels, _pct(refused, pixels), refused,
                 ": %s" % mix if mix else " (none refused)", _pct(forgiven, pixels),
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
    if warns:
        print("refusal census: WARN (%d finding(s) above)" % warns)
    elif measured:
        print("refusal census: consistent (%d of %d window(s) measured)" % (len(measured), len(windows)))
    else:
        print("refusal census: nothing measured (%d window(s))" % len(windows))
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
        ndc = max(max(abs(census_phase_ndc(e["phase"], 5040, 2835)[0]), abs(census_phase_ndc(e["phase"], 5040, 2835)[1]))
                  for e in nonzero)
        base = ("%d of %d eye draw(s) had a non-zero world phase (up to %.4f px, about %.1e NDC at 5040x2835)"
                % (len(nonzero), len(eyes), max(max(abs(e["phase"][0]), abs(e["phase"][1])) for e in nonzero), ndc))
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
    size = route_hdr(routes, events)
    sampled = [s for s in seqs if s["phase_state"] == "on" and _phase_nonzero(s["phase"])]
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
          "%d call sequence(s), %d eye draw(s), %d other-thread entr%s"
          % (c["lines"], len(c["windows"]), len(c["order"]), len(c["sequences"]),
             len(c["eyes"]), len(c["threads"]), "y" if len(c["threads"]) == 1 else "ies"))
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
    print_stage2_verdict(c, routes, roles, events, refusals)
    return 0


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
                         "world's -- and the stage 2 verdict (PASS / WARN / STOP) "
                         "on the world route's injected phase")
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

    if args.camera_census:
        return print_camera_census(text)
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

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


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
        "103 census line(s): 2 5 s line(s), 5 camera(s), 3 call sequence(s), 8 eye draw(s), 0 other-thread entries",
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
            rw[1]["causes"]["stale"] != 40007520 or rw[1]["causes"]["masked"] != 800150 or rw[1]["causes"]["sentinel"] != 16003008 or \
            rw[1]["causes"]["range"] != 1600300 or rw[1]["forgiven"] != 0 or rw[1]["steady"] != "off" or rw[1]["view"] != "off" or \
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
        "3.650% (58410978): stale 2.500%, masked 0.050%, sentinel 1.000%, range 0.100%; forgiven 0.000% | route state=owned jitter=on "
        "fp-mode 0/450/0, inj-fp 1350, struct fov 0.8203..0.9831",
        "totals, steady-detail=off: 1 measured window(s), 112 sample(s), pixels 1600300800, refused 3.650% (58410978): stale 2.500%, masked "
        "0.050%, sentinel 1.000%, range 0.100%; forgiven 0.000%; of the refused: stale 68.5%, masked 1.4%, sentinel 27.4%, range 2.7%",
        "causes seen: stale = the engine slot's depth was not the pixel's: a later draw overdrew it; masked = a rig record with no usable history",
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
                 range=1600300, depth=0, weapon=0, other=0, forgiven=0, steady="off", view="off")
        v.update(kw)
        return ("[12:00:09.000] vr world route refusal 5s: census=%(census)s every=%(every)d treated=%(treated)d asked=%(asked)d "
                "sampled=%(sampled)d read=%(read)d dropped=%(dropped)d size=%(size)s pixels=%(pixels)d refused=%(refused)d "
                "refused-pct=%(pct)s stale=%(stale)d masked=%(masked)d corrupt=%(corrupt)d sentinel=%(sentinel)d "
                "unreprojectable=%(unreprojectable)d camera=%(camera)d range=%(range)d depth=%(depth)d weapon=%(weapon)d other=%(other)d "
                "forgiven=%(forgiven)d steady-detail=%(steady)s view=%(view)s\n") % v

    nothing = dict(refused=0, pct="0.000", stale=0, masked=0, sentinel=0, range=0)
    # "Ran, 0 refused" (pixels > 0, refused=0) is never the same text as "never ran" (treated=0, asked=0, read=0).
    _, out = report(text + refusal_line(**nothing))
    if "refused 0.000% (0): none refused; forgiven 0.000%" not in squash(out) or "!! " in out or \
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
    # The key's state: each state is totalled on its own; with it on a stale slot is "forgiven", never refused.
    on = refusal_line(steady="on", stale=0, forgiven=40007520, refused=18403458, pct="1.150", read=112)
    _, out = report(text + on)
    if "!! " in out or "totals, steady-detail=on: 1 measured window(s), 112 sample(s), pixels 1600300800, refused 1.150% (18403458)" not in squash(out) or \
            "forgiven 2.500%" not in squash(out) or "totals, steady-detail=off: 1 measured window(s)" not in squash(out):
        fail("a window with the steady-detail key on was not totalled on its own, with its forgiven share:\n%s" % out)
    _, out = report(text + refusal_line(steady="on", stale=5, forgiven=40007515))
    if "steady-detail=on and 5 stale pixel(s) were still refused" not in out or "refusal census: WARN" not in out:
        fail("stale pixels still refused with the key on were not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(steady="off", forgiven=9))
    if "9 stale pixel(s) were forgiven while steady-detail=off" not in out:
        fail("pixels forgiven with the key off were not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(refused=1600300801))
    if "refused 1600300801 exceeds the pixels examined, 1600300800" not in out:
        fail("more pixels refused than examined was not a WARN:\n%s" % out)
    _, out = report(text + refusal_line(dropped=3, other=7))
    if "3 sample(s) were skipped because the read-back ring was full" not in out or "7 refused pixel(s) are of a class the census cannot name" not in out or \
            "refusal census: consistent" not in out:
        fail("dropped samples and unnamed causes were not notes (consistent, not WARN):\n%s" % out)
    _, out = report(text + refusal_line(view="on", steady="on", stale=0, forgiven=1))
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

    # ---- the command line ----
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = main(["--file", fixture, "--camera-census"])
    if rc != 0 or "== which signal separates the eye cameras (A)-(F) ==" not in buf.getvalue() or \
            "== stage 2 verdict" not in buf.getvalue():
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
