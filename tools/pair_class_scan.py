#!/usr/bin/env python3
"""Run the shared pair's rule over the draw census in EDVR graphics logs.

VS 94D5C556DFD6D705 / PS 912477AEF6958379 is one shader pair that Elite uses for the radar's contact
markers, the landing-pad display's rings and the sun's glare train (src/d3d11/shared_pair.h has the reasoning,
the evidence and the rule). This is that rule, in Python, over the `DC` lines a draw census (F10) writes, so a
capture can be judged the day it is flown:

    RADAR  a radar-only family drew within 250 draws before it on the same target
    PAD    else a console draw did
    WORLD  else

    python tools\\pair_class_scan.py LOG [LOG ...]
    python tools\\pair_class_scan.py --target frontier --latest 3
    python tools\\pair_class_scan.py --target frontier --all --summary
    python tools\\pair_class_scan.py LOG --window 400
    python tools\\pair_class_scan.py --emit-fixture LOG --census 2 --frame 0 [--noise 12]
    python tools\\pair_class_scan.py --self-test

Every draw of the pair is printed by census and ordinal with its class, how far back the mark that made the
call was, and the draw's own state (depth-stencil state and stencil reference). WORLD is the call to read
with care: it is the default when no mark is near, and no capture yet holds a glare train in a cockpit frame
with the radar up. A census that hit its line cap (its `DC end` line says how many lines were dropped) is
reported as such: draws late in the census, or in its later frames, may be missing rather than absent.

Ordinals: the census's #N is the draw's index in its frame across both eyes, which is what vscreen's
eyeDrawsThisFrame counts, so the distances here are the ones the draw path measures. The target is the census's
r= token; when the census's interned table overflowed the token is a description (`tex2620x2532f26`) that both
eyes share, and marks of one eye can then reach the other's draws here (the draw path compares the bound view
itself and cannot). The scan says so when it sees such a token on a pair draw.

--emit-fixture writes (to stdout) the reduction of one census frame that tools\\shared_pair_test reads: the
marks, the pair's draws, a few other draws, ordinals counted from 1 as the draw path counts them. The classes
it writes are this tool's own; a fixture is committed only after each class is checked against the eye dump.

Read-only: this tool opens logs for reading and writes nothing.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

PAIR_VS = "94D5C556DFD6D705"
PAIR_PS = "912477AEF6958379"
RADAR_VS = {
    "F8D8A92E96419901": "icon core", "DF3503CD07F9B10C": "stalk A", "5453D19B6D362364": "stalk B",
    "A2C2D5510BF1926D": "contact A", "9B34C331902DC1ED": "contact B", "9611A454527F7FEB": "contact C",
    "B932058F26B76691": "contact D",
}
CONSOLE_VS = {"41E245D488BFE83E": "console A", "68DDDEF04D9894AF": "console B"}
WINDOW = 250

DC_RE = re.compile(
    r"^\[(?P<t>\d\d:\d\d:\d\d\.\d+)\] DC (?P<frame>\d+) #(?P<ord>\d+) (?P<kind>\w) n=(?P<n>\d+) "
    r"i=(?P<i>\d+) r=(?P<r>\S+) ")
FIELD_RE = re.compile(r"\b(vh|ph|st|ds)=(\S+)")
BEGIN_RE = re.compile(r"\] DC begin census=(?P<c>\d+) frames=(?P<f>\d+) frame=(?P<g>\d+)")
END_RE = re.compile(r"\] DC end census=(?P<c>\d+) .*?lines=(?P<lines>\d+)(?: .*?overflow=(?P<over>\d+))?")
FRAME_RE = re.compile(r"\] DC frame (?P<f>\d+) draws=(?P<d>\d+)")


class Draw(object):
    __slots__ = ("ordinal", "t", "kind", "n", "i", "r", "vs", "ps", "st", "ds", "line")

    def __init__(self, ordinal, t, kind, n, i, r, vs, ps, st, ds, line):
        self.ordinal, self.t, self.kind, self.n, self.i = ordinal, t, kind, n, i
        self.r, self.vs, self.ps, self.st, self.ds, self.line = r, vs, ps, st, ds, line


class Census(object):
    def __init__(self, number, t, game_frame):
        self.number, self.t, self.game_frame = number, t, game_frame
        self.frames = {}          # census frame -> [Draw] in log order
        self.lines_cap = None     # `DC end`'s lines= (the cap it hit, when it hit it)
        self.dropped = 0          # `DC end`'s overflow=
        self.frame_draws = {}     # census frame -> the eye draws the census counted (`DC frame N draws=`)
        self.frame_draws = {}     # census frame -> the eye draws the census counted (`DC frame N draws=`)


class Call(object):
    def __init__(self, draw, cls, radar_back, console_back, ps_known):
        self.draw, self.cls = draw, cls
        self.radar_back, self.console_back, self.ps_known = radar_back, console_back, ps_known


def is_pair(draw):
    """The pair's vertex shader with its pixel shader (an unread pixel shader, `ph=-`, is the pair's too)."""
    return draw.vs == PAIR_VS and draw.ps in (PAIR_PS, None)


def classify_draws(draws, window=WINDOW):
    """The rule over one frame's draws in order: a list of Call, one for each draw of the pair. Marks are
    per target token and only draws before the pair's count; ordinals are the census's #N."""
    last_radar, last_console = {}, {}
    calls = []
    for d in draws:
        if d.vs in RADAR_VS:
            last_radar[d.r] = d.ordinal
        elif d.vs in CONSOLE_VS:
            last_console[d.r] = d.ordinal
        elif is_pair(d):
            radar = last_radar.get(d.r)
            console = last_console.get(d.r)
            rb = d.ordinal - radar if radar is not None and radar < d.ordinal else None
            cb = d.ordinal - console if console is not None and console < d.ordinal else None
            if rb is not None and rb <= window:
                cls = "RADAR"
            elif cb is not None and cb <= window:
                cls = "PAD"
            else:
                cls = "WORLD"
            calls.append(Call(d, cls, rb, cb, d.ps is not None))
    return calls


def parse_log(path):
    """The census blocks of one log: a list of Census."""
    censuses = []
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, raw in enumerate(f, 1):
            if "] DC " not in raw:
                continue
            m = BEGIN_RE.search(raw)
            if m:
                cur = Census(int(m.group("c")), raw[1:13], int(m.group("g")))
                censuses.append(cur)
                continue
            if cur is None:
                continue
            m = FRAME_RE.search(raw)
            if m:
                cur.frame_draws[int(m.group("f"))] = int(m.group("d"))
                continue
            m = END_RE.search(raw)
            if m:
                cur.lines_cap = int(m.group("lines"))
                cur.dropped = int(m.group("over") or 0)
                continue
            m = DC_RE.match(raw)
            if not m:
                continue
            fields = dict(FIELD_RE.findall(raw))
            ph = fields.get("ph")
            cur.frames.setdefault(int(m.group("frame")), []).append(Draw(
                int(m.group("ord")), m.group("t"), m.group("kind"), int(m.group("n")), int(m.group("i")),
                m.group("r"), fields.get("vh", "?").upper(), None if ph in (None, "-") else ph.upper(),
                fields.get("st"), fields.get("ds"), lineno))
    return censuses


def explain(c):
    if c.cls == "RADAR":
        return "a radar family %d draws back" % c.radar_back
    if c.cls == "PAD":
        return "a console draw %d draws back, no radar family in the window" % c.console_back
    if c.radar_back or c.console_back:
        return "the nearest mark is %d draws back, past the window" % (c.radar_back or c.console_back)
    return "no radar family or console draw before it on its target"


def scan_log(path, window=WINDOW, out=None, summary_only=False):
    """Print the pair's calls in one log; returns {"RADAR": n, "PAD": n, "WORLD": n}."""
    out = out or sys.stdout
    totals = {"RADAR": 0, "PAD": 0, "WORLD": 0}
    censuses = parse_log(path)
    per_census = []
    for cen in censuses:
        rows = []
        for fr in sorted(cen.frames):
            for call in classify_draws(cen.frames[fr], window):
                rows.append((fr, call))
                totals[call.cls] += 1
        per_census.append((cen, rows))
    pairs = sum(len(r) for _, r in per_census)
    out.write("%s: %d census(es), %d draw(s) of the shared pair: radar %d, pad %d, world %d\n"
              % (os.path.basename(path), len(censuses), pairs, totals["RADAR"], totals["PAD"], totals["WORLD"]))
    if summary_only:
        return totals
    for cen, rows in per_census:
        out.write("  census %d (%s, frame %d): %d draw(s) of the pair\n"
                  % (cen.number, cen.t, cen.game_frame, len(rows)))
        # A census that hit its line cap keeps its first frames whole and cuts the later ones: name the
        # frames that are incomplete, because a draw missing from them is not a draw that was never made.
        for fr in sorted(cen.frame_draws):
            kept = len(cen.frames.get(fr, []))
            if kept < cen.frame_draws[fr]:
                out.write("    frame %d: %d of the %d eye draws are in the log (the census's line cap, %d line(s) "
                          "dropped): the draws after #%d are missing\n"
                          % (fr, kept, cen.frame_draws[fr], cen.dropped, kept))
        for fr, call in rows:
            d = call.draw
            note = ""
            if d.r and not d.r.startswith("@"):
                note = "  [target token is a description both eyes share]"
            if d.ps is None:
                note += "  [pixel shader not read]"
            out.write("    frame %d #%d %s i=%d r=%s ds=%s st=%s -> %s (%s)%s\n"
                      % (fr, d.ordinal, d.t, d.i, d.r, d.ds or "-", d.st or "-", call.cls, explain(call), note))
    return totals


# ------------------------------------------------------------------------------------ fixtures --

def emit_fixture(path, census_no, frame, noise, out=None, title=None):
    """The reduction of one census frame: every mark, every draw of the pair, and `noise` other draws spread
    over the frame. Ordinals from 1. The classes are this tool's own; check them before committing."""
    out = out or sys.stdout
    for cen in parse_log(path):
        if cen.number != census_no:
            continue
        draws = cen.frames.get(frame)
        if not draws:
            raise SystemExit("census %d has no frame %d in %s" % (census_no, frame, path))
        calls = {id(c.draw): c for c in classify_draws(draws)}
        others = [d for d in draws if d.vs not in RADAR_VS and d.vs not in CONSOLE_VS and not is_pair(d)]
        keep_others = set()
        if noise > 0 and others:
            step = max(1, len(others) // noise)
            keep_others = set(id(d) for d in others[::step][:noise])
        out.write("# %s\n" % (title or "reduced by tools\\pair_class_scan.py --emit-fixture"))
        out.write("# source: %s census %d (%s) frame %d; ordinals are the census's #N plus one\n"
                  % (os.path.basename(path), cen.number, cen.t, frame))
        out.write("frame %d\n" % (frame + 1))
        for d in draws:
            if d.vs in RADAR_VS or d.vs in CONSOLE_VS or id(d) in keep_others:
                out.write("d %d %s %s\n" % (d.ordinal + 1, d.vs, d.r))
            elif id(d) in calls:
                out.write("p %d %d %s %s\n" % (d.ordinal + 1, d.i, d.r, calls[id(d)].cls.lower()))
        return
    raise SystemExit("no census %d in %s" % (census_no, path))


def parse_fixture(text):
    """[(frame_id, [entry])] with entry ('d', ordinal, vs, target) or ('p', ordinal, instances, target, expected)."""
    frames = []
    cur = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if parts[0] == "frame":
            cur = (int(parts[1]), [])
            frames.append(cur)
        elif parts[0] == "d" and cur is not None:
            cur[1].append(("d", int(parts[1]), parts[2].upper(), parts[3]))
        elif parts[0] == "p" and cur is not None:
            cur[1].append(("p", int(parts[1]), int(parts[2]), parts[3], parts[4].lower()))
        else:
            raise ValueError("bad fixture line: %r" % raw)
    return frames


def fixture_calls(entries, window=WINDOW):
    """The rule over a fixture frame: [(ordinal, class name in lower case)] for its 'p' entries."""
    last_radar, last_console = {}, {}
    out = []
    for e in entries:
        if e[0] == "d":
            _, ordinal, vs, target = e
            if vs in RADAR_VS:
                last_radar[target] = ordinal
            elif vs in CONSOLE_VS:
                last_console[target] = ordinal
        else:
            _, ordinal, _inst, target, _expected = e
            r, c = last_radar.get(target), last_console.get(target)
            if r is not None and r < ordinal and ordinal - r <= window:
                out.append((ordinal, "radar"))
            elif c is not None and c < ordinal and ordinal - c <= window:
                out.append((ordinal, "pad"))
            else:
                out.append((ordinal, "world"))
    return out


# ------------------------------------------------------------------------------------ self-test --

def _dc(frame, ordinal, vs, r="@1", n=6, i=1, ph="-", st="04", ds="02wA", kind="N", t="14:40:46.087"):
    return ("[%s] DC %d #%d %s n=%d i=%d r=%s d=@2 c=@3 s=@4,@5,-,- vs=? vh=%s vb=buf48 sd=8 of=0 tp=4 ia=0,0,0 "
            "ib=- x=-,-,-,- ph=%s vp=0,0+2620x2532 z=0.000-1.000 sc=0,0-0,0 ds=%s st=%s bm=F pr=- bl=15,2,1/5,2,1 "
            "sm=FFFFFFFF q=%d\n" % (t, frame, ordinal, kind, n, i, r, vs, ph, ds, st, ordinal * 3))


def self_test():
    import io
    import tempfile
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    radar_vs = "9611A454527F7FEB"
    console_vs = "68DDDEF04D9894AF"
    # 1. the rule on synthetic census lines, with the negative controls
    def run(lines, window=WINDOW):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "edvr_gfx_test.log")
            with open(p, "w", encoding="utf-8", newline="") as f:
                f.write("[14:40:45.959] DC begin census=1 frames=1 frame=100 offscreen=yes\n" + "".join(lines))
            cen = parse_log(p)[0]
            return classify_draws(cen.frames[0], window), cen

    pair = lambda o, r="@1", i=39, ph=PAIR_PS: _dc(0, o, PAIR_VS, r=r, i=i, ph=ph)
    calls, cen = run([_dc(0, 10, radar_vs), _dc(0, 12, PAIR_VS, i=39, ph=PAIR_PS)])
    check(len(calls) == 1 and calls[0].cls == "RADAR" and calls[0].radar_back == 2, "a radar family two draws back: RADAR")
    calls, _ = run([_dc(0, 10, console_vs), pair(200)])
    check(calls[0].cls == "PAD" and calls[0].console_back == 190, "a console draw, no radar family: PAD")
    calls, _ = run([_dc(0, 10, console_vs), _dc(0, 190, radar_vs), pair(192)])
    check(calls[0].cls == "RADAR", "a radar family wins over a console draw")
    calls, _ = run([pair(50)])
    check(calls[0].cls == "WORLD", "no marks: WORLD")
    # negative controls: each thing the rule reads, changed
    calls, _ = run([_dc(0, 10, radar_vs), pair(260)])          # 250 back
    check(calls[0].cls == "RADAR", "250 draws back is inside the window")
    calls, _ = run([_dc(0, 10, radar_vs), pair(261)])          # 251 back
    check(calls[0].cls == "WORLD", "251 draws back is outside it")
    calls, _ = run([_dc(0, 10, radar_vs, r="@2"), pair(12, r="@1")])
    check(calls[0].cls == "WORLD", "a mark on another target does not count")
    calls, _ = run([pair(12), _dc(0, 14, radar_vs)])
    check(calls[0].cls == "WORLD", "a mark AFTER the pair's draw does not count")
    calls, _ = run([_dc(0, 10, radar_vs), _dc(0, 12, "1234567890ABCDEF", i=39, ph=PAIR_PS)])
    check(not calls, "another vertex shader is not the pair")
    calls, _ = run([_dc(0, 10, radar_vs), _dc(0, 12, PAIR_VS, i=39, ph="0123456789ABCDEF")])
    check(not calls, "the pair's vertex shader with another pixel shader is not the pair")
    calls, _ = run([_dc(0, 10, radar_vs), _dc(0, 12, PAIR_VS, i=39, ph="-")])
    check(len(calls) == 1 and not calls[0].ps_known and calls[0].cls == "RADAR", "an unread pixel shader still counts as the pair's")
    calls, _ = run([_dc(0, 10, radar_vs), pair(14)], window=3)
    check(calls[0].cls == "WORLD", "--window narrows the rule")

    # 2. the scan's report and its line-cap warning
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "edvr_gfx_test.log")
        with open(p, "w", encoding="utf-8", newline="") as f:
            f.write("[14:40:45.959] DC begin census=1 frames=1 frame=100 offscreen=yes\n")
            f.write(_dc(0, 5, console_vs))
            f.write(_dc(0, 9, PAIR_VS, i=3392, ph=PAIR_PS))
            f.write("[14:40:46.234] DC frame 0 draws=5 off=0 copies=0 disp=0 clears=0 unseen=0\n")
            f.write("[14:40:46.350] DC end census=1 draws=5 off=0 copies=0 disp=0 unseen=0 lines=16384 interned=2048 "
                    "overflow=99 truncated=0\n")
        buf = io.StringIO()
        totals = scan_log(p, out=buf)
        text = buf.getvalue()
        check(totals == {"RADAR": 0, "PAD": 1, "WORLD": 0}, "the scan totals")
        check("-> PAD" in text and "i=3392" in text, "the report names the call and the instances")
        check("frame 0: 2 of the 5 eye draws are in the log" in text and "99 line(s) dropped" in text
              and "draws after #2 are missing" in text, "a frame the line cap cut is named, with what is missing")
        check("census 1 (14:40:45.959, frame 100)" in text, "the report names the census")
        # ... and a frame the log kept whole is not
        with open(p, "w", encoding="utf-8", newline="") as f:
            f.write("[14:40:45.959] DC begin census=1 frames=1 frame=100 offscreen=yes\n")
            f.write(_dc(0, 5, console_vs))
            f.write(_dc(0, 9, PAIR_VS, i=3392, ph=PAIR_PS))
            f.write("[14:40:46.234] DC frame 0 draws=2 off=0 copies=0 disp=0 clears=0 unseen=0\n")
            f.write("[14:40:46.350] DC end census=1 draws=2 off=0 copies=0 disp=0 unseen=0 lines=4979 interned=20 "
                    "overflow=774 truncated=0\n")
        buf = io.StringIO()
        scan_log(p, out=buf)
        check("are missing" not in buf.getvalue(), "a frame whose every eye draw is in the log carries no warning")
        # the fixture round trip
        buf = io.StringIO()
        emit_fixture(p, 1, 0, 0, out=buf)
        fx = parse_fixture(buf.getvalue())
        check(len(fx) == 1 and fixture_calls(fx[0][1]) == [(10, "pad")]
              and fx[0][1][-1][4] == "pad", "a fixture emitted from a census reads back to the same class")

    # 3. the committed fixtures: the rule, against recorded sequences and the class each was judged to be
    fixture_dir = os.path.join(HERE, "shared_pair_test", "fixtures")
    names = sorted(n for n in os.listdir(fixture_dir) if n.endswith(".txt")) if os.path.isdir(fixture_dir) else []
    check(len(names) >= 5, "the shared_pair_test fixtures are there (%d found)" % len(names))
    classes_seen = set()
    for name in names:
        with open(os.path.join(fixture_dir, name), "r", encoding="utf-8") as f:
            frames = parse_fixture(f.read())
        check(frames, "%s has frames" % name)
        for frame_id, entries in frames:
            got = fixture_calls(entries)
            want = [(e[1], e[4]) for e in entries if e[0] == "p"]
            check(got == want, "%s frame %d: the rule gives %s, the fixture says %s" % (name, frame_id, got, want))
            classes_seen.update(c for _, c in want)
    check({"radar", "pad", "world"} <= classes_seen, "the fixtures cover all three classes (%s)" % sorted(classes_seen))

    # 4. read-only: no file is written beside the logs, and --dry-run is accepted
    check(main(["--dry-run"]) == 0, "--dry-run is accepted")

    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("pair_class_scan self-test OK")
    return 0


# ---------------------------------------------------------------------------------------- main --

def main(argv=None):
    ap = argparse.ArgumentParser(description="the shared pair's rule over the draw census in EDVR graphics logs")
    ap.add_argument("logs", nargs="*", help="graphics log paths")
    ap.add_argument("--target", help="steam, frontier or a game directory: read its newest graphics logs")
    ap.add_argument("--latest", type=int, default=1, help="with --target: how many of the newest logs (default 1)")
    ap.add_argument("--all", action="store_true", help="with --target: every graphics log")
    ap.add_argument("--summary", action="store_true", help="one line a log")
    ap.add_argument("--window", type=int, default=WINDOW, help="draws a mark reaches (default %d)" % WINDOW)
    ap.add_argument("--emit-fixture", metavar="LOG", help="write one census frame's reduction to stdout")
    ap.add_argument("--census", type=int, help="with --emit-fixture: the census number")
    ap.add_argument("--frame", type=int, default=0, help="with --emit-fixture: the census frame (default 0)")
    ap.add_argument("--noise", type=int, default=12, help="with --emit-fixture: other draws to keep (default 12)")
    ap.add_argument("--title", help="with --emit-fixture: the fixture's first comment line (what the frame shows)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="accepted for convention; this tool writes no file")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.dry_run:
        return 0
    if args.emit_fixture:
        if args.census is None:
            ap.error("--emit-fixture needs --census")
        emit_fixture(args.emit_fixture, args.census, args.frame, args.noise, title=args.title)
        return 0
    paths = list(args.logs)
    if args.target:
        import edvr_log
        game = edvr_log.resolve_target(args.target)
        found = []
        for directory in edvr_log.log_dirs_for(game, "gfx"):
            found.extend(edvr_log.find_logs(directory, "gfx"))
        found.sort(reverse=True)
        chosen = found if args.all else found[:max(1, args.latest)]
        paths += [p for _s, _t, p in chosen]
    if not paths:
        ap.error("name a log, or --target")
    total = {"RADAR": 0, "PAD": 0, "WORLD": 0}
    for p in paths:
        if not os.path.isfile(p):
            print("no such log: %s" % p, file=sys.stderr)
            return 2
        t = scan_log(p, args.window, summary_only=args.summary)
        for k in total:
            total[k] += t[k]
    if len(paths) > 1:
        print("all %d logs: radar %d, pad %d, world %d" % (len(paths), total["RADAR"], total["PAD"], total["WORLD"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
