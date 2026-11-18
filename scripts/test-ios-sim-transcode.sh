#!/usr/bin/env bash
#
# Runs the real iOS ImageIO transcode path inside an iOS Simulator.
#
# Why a simulator executable instead of an XCTest bundle: the iOS-specific work
# (HEIC/WebP decoding, EXIF baking, sRGB normalization, alpha detection) all
# lives in ios/NitroPdfWriterImageTranscoder.mm, and that file needs no UIKit.
# Compiling it into a small Mach-O binary and running it through `simctl spawn`
# exercises the same code an app binary runs, without paying for a full Xcode
# project or a React Native build. If the simulator cannot be booted on this
# machine, run it on a real device instead.
#
# Usage: scripts/test-ios-sim-transcode.sh [fixtures_dir] [simulator_udid]
#   fixtures_dir    defaults to android/src/androidTest/assets (shared corpus)
#   simulator_udid  defaults to the first booted iOS iPhone/iPad, else the first
#                   available iPhone (which this script then boots)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURES="${1:-$ROOT/android/src/androidTest/assets}"
UDID="${2:-}"
OUT_DIR="/tmp/npdf-ios-sim"
BINARY="$OUT_DIR/transcode-harness"

if [[ ! -d "$FIXTURES" ]]; then
  echo "fixtures directory not found: $FIXTURES" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "== building harness for iphonesimulator =="
# Host arch, not a hardcoded arm64: Intel Macs build x86_64 simulators.
HOST_ARCH="$(uname -m)"
xcrun -sdk iphonesimulator clang++ \
  -target "${HOST_ARCH}-apple-ios15.0-simulator" \
  -std=c++20 -fobjc-arc -O1 \
  -I"$ROOT/cpp" -I"$ROOT/ios" \
  "$ROOT/ios/NitroPdfWriterImageTranscoder.mm" \
  "$ROOT/cpp/PlatformImageTranscoder.cpp" \
  "$ROOT/scripts/ios-sim-transcode-harness/main.mm" \
  -framework Foundation -framework ImageIO -framework CoreGraphics -framework CoreImage \
  -o "$BINARY"

if [[ -z "$UDID" ]]; then
  # Only an iPhone/iPad runtime can run this harness; a booted Watch or tvOS
  # device would fail at launch, so filter by runtime name.
  UDID="$(xcrun simctl list devices booted -j | /usr/bin/python3 -c '
import json,sys
data=json.load(sys.stdin)["devices"]
for runtime, devices in data.items():
    if "iOS-" not in runtime:
        continue
    for device in devices:
        if device.get("name","").startswith(("iPhone","iPad")):
            print(device["udid"]); sys.exit(0)
print("")
')"
fi

if [[ -z "$UDID" ]]; then
  UDID="$(xcrun simctl list devices available -j | /usr/bin/python3 -c '
import json,sys
data=json.load(sys.stdin)["devices"]
for runtime, devices in data.items():
    if "iOS-" not in runtime:
        continue
    for device in devices:
        if device.get("name","").startswith("iPhone"):
            print(device["udid"]); sys.exit(0)
print("")
')"
  if [[ -z "$UDID" ]]; then
    echo "no iPhone simulator available; install a runtime or pass a UDID" >&2
    exit 1
  fi
fi

# Applies to a UDID given on the command line as well: `simctl spawn` needs a
# booted device, so make sure it is one (boot is a no-op when already up).
echo "== ensuring simulator $UDID is booted =="
xcrun simctl boot "$UDID" >/dev/null 2>&1 || true
xcrun simctl bootstatus "$UDID" -b >/dev/null

echo "== running inside $UDID =="
xcrun simctl spawn "$UDID" "$BINARY" "$FIXTURES"
