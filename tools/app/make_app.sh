#!/bin/bash
# Packages build/driveclub as Driveclub.app inside a drag-to-install .dmg.
# Usage: tools/app/make_app.sh <version> [SDL3 dylib]
#   <version>    e.g. 0.2.0
#   SDL3 dylib   x86_64 libSDL3.0.dylib to bundle; build it with
#                -ffile-prefix-map so it carries no local paths.
# Output: dist/Driveclub.app and dist/Driveclub-Native-<version>.dmg (ad-hoc signed).
set -euo pipefail

version=${1:?usage: make_app.sh <version> [SDL3 dylib]}
repo=$(cd "$(dirname "$0")/../.." && pwd)
sdl=${2:-$(otool -l "$repo/build/driveclub" | sed -n '/LC_RPATH/{n;n;s/^ *path \(.*\) (offset [0-9]*)$/\1/p;}' | head -1)/libSDL3.0.dylib}
dist="$repo/dist"
app="$dist/Driveclub.app"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

[ -f "$sdl" ] || { echo "SDL3 dylib not found: $sdl" >&2; exit 1; }
if strings "$sdl" | grep -q "$HOME"; then
    echo "warning: $sdl embeds paths under $HOME" >&2
fi

# Both binaries must run on the app's minimum macOS (LSMinimumSystemVersion
# below), not just on the build machine's.
min_macos=15.0
for bin in "$repo/build/driveclub" "$sdl"; do
    minos=$(otool -l "$bin" | awk '/LC_BUILD_VERSION/{f=1} f && $1=="minos"{print $2; exit}')
    if [ "$(printf '%s\n%s\n' "$minos" "$min_macos" | sort -V | tail -1)" != "$min_macos" ]; then
        echo "$bin requires macOS $minos (> $min_macos): build with CMAKE_OSX_DEPLOYMENT_TARGET=$min_macos" >&2
        exit 1
    fi
done

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"

cp "$repo/build/driveclub" "$app/Contents/MacOS/driveclub"
cp "$sdl" "$app/Contents/Frameworks/libSDL3.0.dylib"
otool -l "$app/Contents/MacOS/driveclub" | sed -n '/LC_RPATH/{n;n;s/^ *path \(.*\) (offset [0-9]*)$/\1/p;}' |
    while IFS= read -r rpath; do
        install_name_tool -delete_rpath "$rpath" "$app/Contents/MacOS/driveclub"
    done
install_name_tool -add_rpath @executable_path/../Frameworks "$app/Contents/MacOS/driveclub"

# Icon.
swift "$repo/tools/app/make_icon.swift" "$scratch/icon.png"
iconset="$scratch/AppIcon.iconset"
mkdir "$iconset"
for s in 16 32 128 256 512; do
    sips -z $s $s "$scratch/icon.png" --out "$iconset/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$scratch/icon.png" --out "$iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns"

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Driveclub</string>
    <key>CFBundleDisplayName</key><string>Driveclub Native</string>
    <key>CFBundleIdentifier</key><string>io.github.fladnag86.driveclub-native</string>
    <key>CFBundleExecutable</key><string>driveclub</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>$min_macos</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.racing-games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSHumanReadableCopyright</key><string>Not affiliated with or endorsed by Sony Interactive Entertainment.</string>
</dict>
</plist>
EOF

cp "$repo/LICENSE" "$app/Contents/Resources/LICENSE.txt"
cp "$repo/tools/app/SDL3-LICENSE.txt" "$app/Contents/Resources/SDL3-LICENSE.txt"
cp "$repo/third_party/libatrac9/LICENSE" "$app/Contents/Resources/LibAtrac9-LICENSE.txt"
codesign --force --sign - "$app/Contents/Frameworks/libSDL3.0.dylib"
codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"

# Disk image: the app, a link to /Applications and the read-me.
stage="$scratch/dmg"
mkdir "$stage"
cp -R "$app" "$stage/"
ln -s /Applications "$stage/Applications"
cp "$repo/tools/app/READ ME FIRST.txt" "$stage/"
dmg="$dist/Driveclub-Native-$version.dmg"
rm -f "$dmg"
hdiutil create -quiet -volname "Driveclub Native $version" -srcfolder "$stage" -format UDZO "$dmg"
echo "$dmg"
