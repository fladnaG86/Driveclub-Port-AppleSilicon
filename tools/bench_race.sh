#!/bin/bash
# Race benchmark: tools/bench_race.sh <label> [VAR=value ...]
# The game's own Lua autotest (DC_BENCHMARK, src/core/benchmark.cpp) starts the
# event, the AI drives, frame times are measured over `seconds` after `warmup`
# and the game exits. Prints
# "<label> <avg fps> <gpu busy median %> <errors> <GPU ms/frame median>
# <CP thread CPU ms/frame median> <process CPU %> <1% low fps> <0.1% low fps>
# <seconds under 29 fps>" over the measured window. Avg fps and the lows come
# from flip intervals; the medians from the once-per-second gpu stats lines,
# which snap to vblank multiples, so GPU ms per frame (busy / fps) is what
# shows a change in GPU work.
# Event: DC_BENCH=<DC_BENCHMARK settings> (default below: heavy rain, the
# heaviest case; keys in src/core/benchmark.cpp). Game folder: DC_GAME_DIR.
# Output in ../bench/<label>/: run.log, frames/, shaders/, driveclub (the binary).
set -u
cd "$(dirname "$0")/.."
label=$1
shift
if ps -axo stat,command | grep '[b]uild/driveclub' | grep -q '^E'; then
    echo "a driveclub process is stuck in state E: reboot first" >&2
    exit 1
fi
bench=${DC_BENCH:-track=uk_road_circuit_02_n,car=fe_458,tod=12,rain=Heavy,warmup=10,seconds=60}
game=${DC_GAME_DIR:-../extracted/Image0}
out="../bench/$label"
rm -rf "$out"
mkdir -p "$out/frames" "$out/shaders"
cp build/driveclub "$out/driveclub" # symbols for `sample` profiles of this run
env "$@" DC_BENCHMARK="$bench" DC_GPU_STATS=1 \
    DC_DUMP_FRAMES="$out/frames" DC_DUMP_EVERY=300 DC_DUMP_SHADERS="$out/shaders" \
    ./build/driveclub "$game" > "$out/run.log" 2>&1 &
pid=$!
# The game exits by itself (benchmark timeout 240 s + warmup + seconds); this
# is the backstop for a hang.
start=$(date +%s)
while kill -0 $pid 2>/dev/null && [ $(( $(date +%s) - start )) -lt ${DC_BENCH_LIMIT:-600} ]; do
    sleep 1
done
kill -9 $pid 2>/dev/null
wait $pid 2>/dev/null
python3 - "$out/run.log" "$label" <<'PY'
import re, statistics, sys
log, label = sys.argv[1], sys.argv[2]
fps, busy, gms, cpu, errors, result, window = [], [], [], [], 0, None, False
for line in open(log, errors='replace'):
    if line.startswith('[error]'):
        errors += 1
    if 'benchmark: measuring' in line:
        window = True
    if 'benchmark: result' in line:
        result = dict(kv.split('=', 1) for kv in line.split('benchmark: result ')[1].split())
        window = False
    if not window:
        continue
    c = re.search(r'gpu stats: CP thread ([\d.]+) ms CPU', line)
    if c:
        cpu.append(float(c.group(1)))
    m = re.search(r'gpu stats: ([\d.]+) fps, gpu busy (\d+)%', line)
    if m:
        fps.append(float(m.group(1)))
        busy.append(int(m.group(2)))
        if float(m.group(1)) > 0:
            gms.append(int(m.group(2)) * 10.0 / float(m.group(1)))
if not result:
    print(f'{label} FAILED: no benchmark result (see {log}, "benchmark:" lines)')
    sys.exit(1)
med = lambda v: statistics.median(v) if v else 0
under = sum(1 for f in fps if f < 29)
print(f'{label} {float(result["avg_fps"]):.1f} {med(busy):.0f} {errors} {med(gms):.1f} {med(cpu):.1f} '
      f'{result["cpu_pct"]} {float(result["low1_fps"]):.1f} {float(result["low01_fps"]):.1f} {under}')
PY
