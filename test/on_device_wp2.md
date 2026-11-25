# On-device measurement (JamClock on the MPC)

Short guide for measuring the emitted MIDI clock on the MPC itself with
`test/on_device_wp2.sh` (run on the device, as root). Everything below runs on the
MPC over SSH, e.g. `ssh root@<mpc-ip>`.

## Prerequisites

- JamClock installed on the MPC (release zip + `install.sh -y` as root, see the main
  README "Quick start").
- Root SSH access to the MPC.
- `aconnect` and `aseqdump` available in the root PATH (alsa-utils). Note: MPC OS
  ships alsa-utils 1.2.x, whose `aseqdump` has **no `-t` option** - the script detects
  this and falls back to prefixing every aseqdump line with `date +%s%3N` (epoch ms).
- `date +%s%3N` must print exactly 13 digits (check: `date +%s%3N | wc -c` = 14).
  Without milliseconds all tick deltas would be 0 and the measurement invalid. The
  script aborts before recording in that case.

## Workflow

1. In the MPC app: select track 3/4 (Input 3/4), put JamClock in an insert slot, set
   the track to Thru/Monitor (so `process()` runs). Preferences -> Sync -> external
   (MIDI Clock), MIDI in = the port you connect to.
2. Find the app's MIDI port: `cat /proc/asound/seq/clients`.
3. Connect and verify:
   ```sh
   aconnect -d "JamClock:MIDI Out" "<app client:port>" 2>/dev/null || true   # re-run safe
   aconnect "JamClock:MIDI Out" "<app client:port>"
   aconnect -l | grep -i -A3 jamclock    # must show "Connecting To: <app>", else troubleshoot
   ```
4. Feed live audio into Input 3/4 (e.g. a 120 BPM click/drum pattern), the insert's
   BPM readout should go SEARCH -> LOCKED, bpm ~ 120.
5. Record and evaluate (the script does connect + record + evaluate):
   ```sh
   sh on_device_wp2.sh <ref_bpm> <duration_s> "<app port>"
   # e.g. sh on_device_wp2.sh 120 300 "128:0"
   ```
   The log lands in `/tmp/jamclock_wp2.txt` on the device. The script refuses to run
   without root, re-verifies the aconnect subscription (`Connecting To:` must be
   visible), guards against `invalid option` in the log and against 0 clock events
   (both exit 1), and ends the recording cleanly via `timeout -s INT` (SIGINT flushes
   stdio). If `stdbuf` is missing (busybox), recording runs without it and a log tail
   of up to 4 KB may be lost - the measurement stays valid.

## Evaluation

The awk parser understands both timestamp formats (epoch ms as the first field, or
`aseqdump -t` relative seconds). Metrics: counts of START/CONTINUE/STOP/SPP, drift
ppm vs. an ideal 24-ppq tempo at `<ref_bpm>`, jitter % = max|delta - mean| / mean x 100
(symmetric - early AND late ticks count). Transport gaps are segmented: a delta
> 3x mean is a STOP/CONTINUE pause mid-run (no clock runs there) -> segment boundary.
Metrics are computed per segment and reported as worst case (`segments=N
transport_gaps=K`). Run as pause-free as possible.

Exit code: 0 = PASS (worst-case drift < 50 ppm AND worst-case jitter < 5 % AND events
present), 1 = FAIL/invalid.

Acceptance criteria: drift < 50 ppm over 5 min, tick jitter < 5 % of the tick interval
(ms class), STOP after more than `silence` s of silence and CONTINUE (no second
START) on return, and the MPC sequencer audibly in time (a test pattern on the
downbeat stays on the downbeat).

## Rollback

Delete the plugin folder and its `pluginList-arm` entry, or run `uninstall.sh` from
the release zip, or re-flash the official OS image.
