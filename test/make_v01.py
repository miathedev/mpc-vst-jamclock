#!/usr/bin/env python3
"""Extract the v0.1 engine (git HEAD:src/jamclock_engine.c) into build/engine_v01.c and apply
the JAMCLOCK_HARNESS hooks, so the A/B harness can measure the v0.1 baseline with the same
event log as v0.2. Test-only copy - the shipped source is never touched. Fails loudly if
the extracted file does not look like v0.1 (e.g. after a git history reset, where HEAD is
already v0.2) or if an expected anchor string is not found (i.e. v0.1 changed and this
needs updating)."""
import subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT = os.path.join(HERE, "build", "engine_v01.c")

src = subprocess.check_output(["git", "-C", REPO, "show", "HEAD:src/jamclock_engine.c"]).decode()

# v0.1 sanity check: after a git history reset HEAD may already be the v0.2 engine, which
# would silently produce a wrong "v0.1" baseline. v0.1's alsa_send() is void; v0.2 made it
# return int and carries a "JamClock v0.2" banner. Refuse loudly in either case.
V01_MARKER = "static void alsa_send(Inst *s, int ev_type) {"
V02_BANNER = "JamClock v0.2"
if V02_BANNER in src or V01_MARKER not in src:
    sys.stderr.write(
        "make_v01.py: git HEAD:src/jamclock_engine.c is not the v0.1 engine "
        f"(v0.2 banner present: {V02_BANNER in src}, v0.1 marker missing: {V01_MARKER not in src}). "
        "After a git history reset HEAD no longer points at v0.1 - refusing to build a wrong "
        "v0.1 baseline. Restore the v0.1 commit as HEAD or drop the v0.1 A/B comparison.\n")
    sys.exit(1)

def rep(old, new):
    global src
    if src.count(old) != 1:
        sys.exit(f"anchor not found exactly once ({src.count(old)}x): {old[:70]!r}")
    src = src.replace(old, new)

# 1. harness hook declaration after the includes
rep('#include "alsa_seq_min.h"\n',
    '#include "alsa_seq_min.h"\n'
    '#ifdef JAMCLOCK_HARNESS\n'
    'extern void jclk_log_event(int ev_type, uint64_t now_ns);\n'
    '#endif\n')

# 2. alsa_out_init: harness short-circuit (no dlopen/ALSA)
rep('static void alsa_out_init(Inst *s) {\n    if (s->alsa) return;',
    'static void alsa_out_init(Inst *s) {\n'
    '#ifdef JAMCLOCK_HARNESS\n'
    '    s->alsa = 1;\n'
    '    snprintf(s->port_text, sizeof s->port_text, "harness");\n'
    '    return;\n'
    '#endif\n'
    '    if (s->alsa) return;')

# 3. alsa_send: log instead of ALSA
rep('static void alsa_send(Inst *s, int ev_type) {\n'
    '    if (s->alsa != 1) return;\n'
    '    alsa_out_send(&s->A, s->seq, s->seq_port, ev_type);\n'
    '}',
    'static void alsa_send(Inst *s, int ev_type) {\n'
    '#ifdef JAMCLOCK_HARNESS\n'
    '    jclk_log_event(ev_type, now_ns());\n'
    '    return;\n'
    '#else\n'
    '    if (s->alsa != 1) return;\n'
    '    alsa_out_send(&s->A, s->seq, s->seq_port, ev_type);\n'
    '#endif\n'
    '}')

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as f:
    f.write(src)
print(f"wrote {OUT} ({len(src)} bytes, v0.1 + harness hooks)")
