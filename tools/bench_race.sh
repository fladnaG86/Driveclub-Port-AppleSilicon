#!/bin/bash
# Race benchmark: tools/bench_race.sh <label> [VAR=value ...]
# Starts an event (auto Cross), holds the throttle, runs 150 s, then prints
# "<label> <fps median> <gpu busy median %> <errors> <GPU ms/frame median>
# <CP thread CPU ms/frame median> <process CPU % over 90-140 s>"
# over 90-140 s. fps snaps to vblank multiples (15, 20, 30), so GPU ms per
# frame (busy / fps) is what shows a change in GPU work.
# Output in ../bench/<label>/: run.log, frames/, shaders/.
set -u
cd "$(dirname "$0")/.."
label=$1
shift
if ps -axo stat,command | grep '[b]uild/driveclub' | grep -q '^E'; then
    echo "a driveclub process is stuck in state E: reboot first" >&2
    exit 1
fi
out="../bench/$label"
rm -rf "$out"
mkdir -p "$out/frames" "$out/shaders"
env "$@" DC_GPU_STATS=1 DC_AUTOPRESS=40,45,55 DC_AUTO_THROTTLE=75,60000 \
    DC_DUMP_FRAMES="$out/frames" DC_DUMP_EVERY=300 DC_DUMP_SHADERS="$out/shaders" \
    ./build/driveclub "$HOME/driveclub porting/extracted/Image0" > "$out/run.log" 2>&1 &
pid=$!
start=$(date +%s)
cpu90= cpu140=
while kill -0 $pid 2>/dev/null && [ $(( $(date +%s) - start )) -lt 150 ]; do
    sleep 1
    el=$(( $(date +%s) - start ))
    [ $el -ge 90 ] && [ -z "$cpu90" ] && cpu90=$(ps -o cputime= -p $pid)
    [ $el -ge 140 ] && [ -z "$cpu140" ] && cpu140=$(ps -o cputime= -p $pid)
done
kill -9 $pid 2>/dev/null
wait $pid 2>/dev/null
python3 - "$out/run.log" "$label" "$cpu90" "$cpu140" <<'PY'
import re, statistics, sys
log, label = sys.argv[1], sys.argv[2]
def secs(v):  # ps cputime: [[H:]M:]S.ss
    out = 0.0
    for part in v.strip().split(':'):
        out = out * 60 + float(part)
    return out
pcpu = (secs(sys.argv[4]) - secs(sys.argv[3])) / 50 * 100 if sys.argv[3] and sys.argv[4] else 0
fps, busy, gms, cpu, errors, t = [], [], [], [], 0, 0
for line in open(log, errors='replace'):
    if line.startswith('[error]'):
        errors += 1
    c = re.search(r'gpu stats: CP thread ([\d.]+) ms CPU', line)
    if c and 90 <= t + 1 <= 140:
        cpu.append(float(c.group(1)))
    m = re.search(r'gpu stats: ([\d.]+) fps, gpu busy (\d+)%', line)
    if m:
        t += 1  # one stats line per second
        if 90 <= t <= 140:
            fps.append(float(m.group(1)))
            busy.append(int(m.group(2)))
            if float(m.group(1)) > 0:
                gms.append(int(m.group(2)) * 10.0 / float(m.group(1)))
med = lambda v: statistics.median(v) if v else 0
print(f'{label} {med(fps):.1f} {med(busy):.0f} {errors} {med(gms):.1f} {med(cpu):.1f} {pcpu:.0f}')
PY
