# Scalar Load Speedup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the GPU cost of guest scalar loads (`S_LOAD_*` / `S_BUFFER_LOAD_*`) in translated shaders, so a Driveclub race runs measurably faster than the 14.7 fps baseline with identical output.

**Architecture:** Every scalar load today runs per pixel/vertex/thread through `dc_ptr_n` (page-table walk: map header reads, page entry read, bounds checks) although its address is uniform for the whole draw. Three independent, stacked reductions, each gated by a benchmark: (1) read the page-table header once per invocation; (2) translate a scalar base register pair once and reuse the pointer for later loads from the same base while they stay in its page; (3) for user-data pointers that the renderer already redirected to a CE-dump snapshot (`PatchUserData`), bind the snapshot as a `constant` buffer and load from it directly, no translation at all.

**Tech Stack:** C++20 / Objective-C++, Metal Shading Language generated in `src/gpu/shader/msl.cpp` + prelude `src/gpu/shader/msl_prelude.h`, Metal renderer `src/gpu/metal/mtl_renderer.mm`, CMake + Ninja, run under Rosetta (x86_64) on Apple Silicon.

**Spec:** this plan (no separate spec). Origin: perf findings of 2026-10-17 — race ~14.7 fps, GPU busy ~77 % (~51 ms GPU/frame), top pixel shader `ps 0x7002fda900` does ~20 `dc_ptr_n` scalar loads per pixel, 8 of them straight from user-data pair s12:s13.

## Global Constraints

- Output must not change: generated code may only skip work whose result is provably the same (same address → same dword).
- Every new behavior is on by default and has an env switch to turn it off for A/B: `DC_NO_MAP_HOIST`, `DC_NO_SBASE_CACHE`, `DC_NO_UD_CONST` (read once in the renderer, passed through `MslOptions`).
- Every option that changes generated MSL must be hashed into the pipeline/shader key (`vs_key`, `ps_key`, the compute key) — otherwise cached pipelines mix variants.
- New buffer bindings use indices not taken in `Shader::Binding` (0–5), `kDepthBoundsBuffer` (6), `kProbeBinding` (7): use 8 and 9.
- Commits on branch `macos-rosetta-fixes`, message style as existing commits, ending with the `Co-Authored-By` line. No push.
- Before any run: `ps -axo stat,command | grep build/driveclub` must show no `E`-state process (else reboot first).

## Review Focus

- **Base register rewritten between two loads** (`s_mov`, another `s_load` into s12, `s_add_u32` on the low half): the cached pointer must be invalidated, else loads read the old table. Task 2 case `sbase_written_between_loads`.
- **Load crossing a 16 KB page boundary** from a cached base (base near page end + offset): must fall back to `dc_ptr_n`. Task 2 case `offset_crosses_page`.
- **User-data pair not snapshot-backed** (plain guest memory, maybe GPU-written): must keep the `dc_ptr_n` path; only pairs `PatchUserData` redirected may use the constant buffer. Task 3 case `ud_not_snapshot`.
- **Snapshot shorter than the load** (`off + bytes > snapshot size`): fall back, never read past the bound buffer. Task 3 case `load_past_snapshot`.
- **Jumps and loops**: the base cache must not survive a loop back edge or a block start reached by a jump (value may differ per path). Task 2 case `cache_reset_at_block_start`.

---

## File Structure

- `src/gpu/shader/msl_prelude.h` — prelude MSL: local page-table header struct and `dc_ptr` reading it (Task 1).
- `src/gpu/shader/msl.cpp` — `Emitter::EmitSmrd`, `EmitStructured`, `EmitSwitch`, entry-point emission: hoisted header, base-pointer cache, constant-buffer loads (Tasks 1–3).
- `src/gpu/shader/msl.h` — `MslOptions` fields `map_hoist`, `sbase_cache`, `ud_const_mask`, `ud_const_words`; constants `kUdConstVs = 8`, `kUdConstPs = 9` (compute uses 8).
- `src/gpu/metal/mtl_renderer.mm` — fill the options, hash them into keys, bind the snapshot buffers (Task 3).
- `src/tools/msl_gen.cpp` (new, CMake target `msl_gen`, `EXCLUDE_FROM_ALL` like `gcn_dis`) — generates MSL for a shader binary + options, for fixture checks.
- `tools/bench_race.sh` (new) — benchmark + correctness harness.
- `tools/msl_check.py` (new) — static checks over generated `.metal` files.

---

### Task 0: Benchmark and check harness

**Files:**
- Create: `tools/bench_race.sh`, `tools/msl_check.py`, `src/tools/msl_gen.cpp`
- Modify: `CMakeLists.txt` (target `msl_gen`, next to `gcn_dis`)

**Interfaces:**
- Produces: `tools/bench_race.sh <label>` → `../bench/<label>/{run.log,frames/,shaders/}` and one stdout line `label fps_median gpu_busy_median errors`. `tools/msl_check.py <dir> [check...]` → exit 0 when the named checks pass (all registered checks when none named). `msl_gen <shader.bin> <vs|ps|cs> [key=value ...]` → MSL on stdout; keys are the `MslOptions` fields added by this plan.

- [ ] **Step 1: Write `tools/bench_race.sh`**: runs `./build/driveclub "$HOME/driveclub porting/extracted/Image0"` with `DC_GPU_STATS=1 DC_AUTOPRESS=40,45,55 DC_AUTO_THROTTLE=75,60000 DC_DUMP_FRAMES=<dir>/frames DC_DUMP_EVERY=300 DC_DUMP_SHADERS=<dir>/shaders`, kills after 150 s, refuses to start if an `E`-state `build/driveclub` exists. Median over `gpu stats:` fps / busy lines from 90–140 s; `errors` = count of `[error]` lines.
- [ ] **Step 2: Write `src/tools/msl_gen.cpp`**: decode the `.bin` with the existing `Program` builder, `GenerateMsl(program, options, &unsupported)`, print. Build: `cmake --build build --target msl_gen`. Expected: builds; `msl_gen <any dumped ps_*.bin> ps` prints MSL containing `fragment PsOut main0`.
- [ ] **Step 3: Write `tools/msl_check.py`** with checks `no_unsupported` and `balanced_braces`; later tasks register more in the `CHECKS` dict.
- [ ] **Step 4: Record baseline**: `tools/bench_race.sh baseline`. Expected: ~14–15 fps, `errors 0`. Write the line into Results.
- [ ] **Step 5: Commit** ("Tools: race benchmark, MSL generator and checks").

### Task 1: Hoist the page-table header

**Files:**
- Modify: `src/gpu/shader/msl_prelude.h` (`DcMem`, `dc_ptr`, `dc_ptr_n`)
- Modify: `src/gpu/shader/msl.cpp` (the three `DcMem mem = {gm0, gm1, &gmap};` entry lines)
- Modify: `src/gpu/shader/msl.h` (`bool map_hoist = true;`)
- Modify: `src/gpu/metal/mtl_renderer.mm` (set `map_hoist = !getenv("DC_NO_MAP_HOIST")`, hash into keys)

**Interfaces:**
- Produces: MSL `struct DcMapL { ulong base, pages, split, limit; uint shift; device const int* entries; };`, member `DcMapL mapl` of `DcMem`; `dc_ptr` reads `m.mapl` when built with `map_hoist`. `MslOptions::map_hoist`.

- [ ] **Step 1: Add check `map_header_read_once`**: a file with `DcMapL` has no `m.map->` inside `dc_ptr`/`dc_ptr_n`.
- [ ] **Step 2: Run** `msl_gen <top ps bin> ps map_hoist=1 | msl_check.py - map_header_read_once` — Expected: FAIL.
- [ ] **Step 3: Implement**: entry emits `DcMem mem = {gm0, gm1, &gmap, {gmap.base, gmap.pages, gmap.split, gmap.limit, gmap.shift, (device const int*)(&gmap + 1)}};`; `dc_ptr` uses `m.mapl`; `map_hoist=0` keeps today's prelude.
- [ ] **Step 4: Verify**: check PASS; `tools/bench_race.sh map_hoist` → `errors 0`, frame sheet visually equal to baseline (cockpit, HUD, scenery), fps ≥ baseline − 0.3.
- [ ] **Step 5: Commit** ("MSL: read the page-table header once per invocation"); fps into Results.

### Task 2: Cache translated scalar base pointers

**Files:**
- Modify: `src/gpu/shader/msl.cpp` (`EmitSmrd`; block starts in `EmitStructured`; case labels in `EmitSwitch`; every SGPR write path: `SetS`, `SetS64`, SMRD destinations, movrel writes, `S_SETPC`/`S_SWAPPC`)
- Modify: `src/gpu/shader/msl.h` (`bool sbase_cache = true;`), `mtl_renderer.mm` (`DC_NO_SBASE_CACHE`, key hashing)

**Interfaces:**
- Consumes: Task 1 `dc_ptr` / `mem.mapl.shift`.
- Produces: `Emitter` members `std::set<u16> sbase_cached;` (even SGPR of the pair) and `void InvalidateSbase(u16 sgpr)`, called by every SGPR write; MSL locals `ulong sbvaN_` and `device uint* sbN_` per cached pair N.

- [ ] **Step 1: Add checks** (fixtures in `tools/msl_fixtures/`, hex → `.bin` via `xxd -r -p`, generated with `msl_gen`): `sbase_cached` (two `s_buffer_load_dwordx4 s[20:23], s[12:15], 0x0/0x4` → one `dc_ptr(` for pair 12), `sbase_written_between_loads` (`s_mov_b32 s12, s0` between them → two translations), `offset_crosses_page` (generated code keeps the `dc_ptr_n` fallback branch), `cache_reset_at_block_start` (load, `s_cbranch_scc0` to a later block that loads again → translation repeated in that block).
- [ ] **Step 2: Run checks** — Expected: FAIL (`sbase_cached`).
- [ ] **Step 3: Implement**: cache miss emits `ulong sbvaN_ = <base>; device uint* sbN_ = dc_ptr(mem, sbvaN_ & ~3ul);`; a hit emits `device uint* p_ = (sbN_ && ((sbvaN_ & PM) + off + bytes) <= PS) ? sbN_ + ((off) >> 2) : dc_ptr_n(mem, ..., bytes);` with `PS = 1ul << mem.mapl.shift`, `PM = PS - 1`. No caching inside a `while (true)` loop region; `sbase_cached.clear()` at every block start and case label.
- [ ] **Step 4: Verify**: checks PASS; `tools/bench_race.sh sbase_cache` → `errors 0`, frames equal, fps > Task 1.
- [ ] **Step 5: Commit** ("MSL: translate a scalar base once and reuse it").

### Task 3: Load snapshot-backed user-data tables from a constant buffer

**Files:**
- Modify: `src/gpu/metal/mtl_renderer.mm` (`PatchUserData`, draw path at `PatchUserData(vud)`/`PatchUserData(pud)`, compute path at `PatchUserData(cud)`, key hashing, buffer binding)
- Modify: `src/gpu/shader/msl.h` (`u32 ud_const_mask = 0;` bit k = pair (k, k+1) points into a snapshot; `std::array<u16, 16> ud_const_words{};` dwords readable from that pointer; `kUdConstVs = 8`, `kUdConstPs = 9`)
- Modify: `src/gpu/shader/msl.cpp` (`EmitSmrd`; entry parameter lists)

**Interfaces:**
- Consumes: Task 2 `InvalidateSbase` bookkeeping (tells whether a pair still holds its user-data value).
- Produces: `u32 PatchUserData(u32* ud, std::array<u16, 16>* words)` returning the redirected-pair mask; MSL parameter `constant uint* udt [[buffer(8|9)]]` holding the snapshot dwords of flagged pairs back to back, pair k starting at the sum of `ud_const_words` of lower flagged pairs.

- [ ] **Step 1: Add checks**: `ud_const_used` (fixture `s_buffer_load_dwordx4 s[20:23], s[12:15], 0x8` with `ud_const_mask=0x1000 ud_const_words[12]=64` → reads `udt[`, no `dc_ptr`), `ud_not_snapshot` (mask 0 → `dc_ptr_n`), `load_past_snapshot` (`words[12]=2`, load 16 bytes at 0x8 → `dc_ptr_n`).
- [ ] **Step 2: Run checks** — Expected: FAIL.
- [ ] **Step 3: Implement**: per stage per draw, one `Upload()` of the flagged snapshots (a pair over 1024 dwords is not flagged); SMRD with an unwritten flagged user-data base and immediate `off + bytes <= words*4` emits `udt[start + off/4 + i]`; else the Task 2 path. `ud_const_words` rounded up to a power of two before hashing to bound pipeline variants. `DC_NO_UD_CONST` → mask 0.
- [ ] **Step 4: Verify**: checks PASS; `tools/bench_race.sh ud_const` → `errors 0`, frames equal, `pipelines compiling` back to 0 after warm-up, fps > Task 2.
- [ ] **Step 5: Commit** ("Metal: snapshot user-data tables as constant buffers").

### Task 4: Decide and record

- [ ] **Step 1:** Run `tools/bench_race.sh` four times back to back: all of `DC_NO_MAP_HOIST DC_NO_SBASE_CACHE DC_NO_UD_CONST` set (baseline), then enabling one more step each run. Fill Results.
- [ ] **Step 2:** A step gaining < 0.3 fps → `git revert` its commit, note why in Results.
- [ ] **Step 3: Commit** the plan with Results ("Plans: scalar load speedup results").

## Results

| label | fps (median 90–140 s) | GPU busy | errors |
|---|---|---|---|
| baseline | 15.0 | 76 % | 0 |
| map_hoist | | | |
| sbase_cache | | | |
| ud_const | | | |
