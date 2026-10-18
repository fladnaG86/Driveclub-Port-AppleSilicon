#!/bin/bash
# tools/agx/agxstat.sh <file.metal>...: compiles each kernel `main0` and prints
# the AGX registers it allocates (max_r + 1 32-bit registers; Xcode shows twice
# that in 16-bit units) and its machine-code instruction count.
# Needs dougallj/applegpu cloned next to this script as `applegpu`
# (git clone https://github.com/dougallj/applegpu tools/agx/applegpu).
D=$(cd "$(dirname "$0")" && pwd)
[ -x "$D/agxbin" ] || swiftc -O "$D/agxbin.swift" -o "$D/agxbin" || exit 1
for f in "$@"; do
  "$D/agxbin" "$f" "$D/.tmp.agx" > /dev/null 2>&1 || { echo "$f compile error"; continue; }
  APPLEGPU="$D/applegpu" python3 -I "$D/regs.py" "$D/.tmp.agx" | sed "s|^.*\.tmp\.agx|$(basename "$f")|"
done
