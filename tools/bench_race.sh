#!/bin/bash
# Race benchmark: tools/bench_race.sh <label> [VAR=value ...]
# Starts an event (auto Cross), holds the throttle, runs 150 s, then prints
# "<label> <fps median> <gpu busy median %> <errors>" over 90-140 s.
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
while kill -0 $pid 2>/dev/null && [ $(( $(date +%s) - start )) -lt 150 ]; do
    sleep 1
done
kill -9 $pid 2>/dev/null
wait $pid 2>/dev/null
python3 - "$out/run.log" "$label" <<'PY'
import re, statistics, sys
log, label = sys.argv[1], sys.argv[2]
fps, busy, errors, t = [], [], 0, 0
for line in open(log, errors='replace'):
    if line.startswith('[error]'):
        errors += 1
    m = re.search(r'gpu stats: ([\d.]+) fps, gpu busy (\d+)%', line)
    if m:
        t += 1  # one stats line per second
        if 90 <= t <= 140:
            fps.append(float(m.group(1)))
            busy.append(int(m.group(2)))
med = lambda v: statistics.median(v) if v else 0
print(f'{label} {med(fps):.1f} {med(busy):.0f} {errors}')
PY
