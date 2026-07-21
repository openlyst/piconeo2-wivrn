#!/usr/bin/env bash
# Fetch and extract the Pico Neo 2 SDK from the archive.org bundle.
#
# The SDK is not bundled in the repo due to licensing. This script downloads
# the 7z archive, extracts the Unity SDK, pulls out pvrSDK-release.aar from
# the unitypackage, and places the .so files and classes.jar where the Gradle
# build expects them.
#
# Usage:
#   ./tools/fetch_pico_sdk.sh [DOWNLOAD_URL]
#
# If no URL is given, falls back to $PICO_SDK_DOWNLOAD_URL, then to the
# default archive.org URL.
set -euo pipefail

DEFAULT_URL="https://archive.org/download/pico-neo-2-sdks-exes.-7z/PicoNeo2-SDKs-EXEs.7z"
URL="${1:-${PICO_SDK_DOWNLOAD_URL:-$DEFAULT_URL}}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JNILIBS="$REPO_ROOT/src/app/src/main/jniLibs/armeabi-v7a"
LIBS_DIR="$REPO_ROOT/src/app/libs"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Downloading Pico Neo 2 SDK from: $URL"
curl -fSL -o "$TMP/sdk.7z" "$URL"

echo "==> Extracting 7z archive"
7z x -y -o"$TMP/sdk" "$TMP/sdk.7z" >/dev/null

SDK_DIR="$TMP/sdk/PicoNeo2-SDKs-EXEs"
if [ ! -d "$SDK_DIR" ]; then
    echo "ERROR: expected $SDK_DIR to exist after extraction" >&2
    exit 1
fi

echo "==> Extracting Unity SDK zip"
UNITY_ZIP="$(find "$SDK_DIR/Unity+Plugin" -name 'PicoVR_Unity_SDK_*.zip' | head -1)"
if [ -z "$UNITY_ZIP" ]; then
    echo "ERROR: PicoVR Unity SDK zip not found in $SDK_DIR/Unity+Plugin" >&2
    exit 1
fi
unzip -q -o "$UNITY_ZIP" -d "$TMP/unity"

# The unitypackage is a tar.gz. Each asset is in a hash-named folder with
# a pathname text file and an asset binary file. Find the pvrSDK-release.aar.
UPKG_DIR="$(find "$TMP/unity" -type d -name 'PicoVR_Unity_SDK_*' | head -1)"
UPKG_64="$(find "$UPKG_DIR" -name '*64bit*.unitypackage' | head -1)"
if [ -z "$UPKG_64" ]; then
    # Fall back to any unitypackage in the dir.
    UPKG_64="$(find "$UPKG_DIR" -name '*.unitypackage' | head -1)"
fi
if [ -z "$UPKG_64" ]; then
    echo "ERROR: no .unitypackage found in $UPKG_DIR" >&2
    exit 1
fi

echo "==> Extracting unitypackage: $(basename "$UPKG_64")"
mkdir -p "$TMP/upkg"
tar xzf "$UPKG_64" -C "$TMP/upkg"

# Locate the pvrSDK-release.aar asset by reading pathname files.
AAR_ASSET=""
for d in "$TMP/upkg"/*/; do
    if [ -f "${d}pathname" ]; then
        pn="$(cat "${d}pathname" | tr -d '\0')"
        if [[ "$pn" == *"pvrSDK-release.aar" ]]; then
            AAR_ASSET="${d}asset"
            break
        fi
    fi
done
if [ -z "$AAR_ASSET" ] || [ ! -f "$AAR_ASSET" ]; then
    echo "ERROR: pvrSDK-release.aar not found in unitypackage" >&2
    exit 1
fi

echo "==> Extracting pvrSDK-release.aar"
mkdir -p "$TMP/aar"
unzip -q -o "$AAR_ASSET" -d "$TMP/aar"

echo "==> Placing SDK files"
mkdir -p "$JNILIBS" "$LIBS_DIR"

for so in libPvr_UnitySDK.so libnative.so libtracking_module.so; do
    src="$TMP/aar/jni/armeabi-v7a/$so"
    if [ ! -f "$src" ]; then
        echo "ERROR: $so not found in aar jni/armeabi-v7a/" >&2
        exit 1
    fi
    cp -f "$src" "$JNILIBS/$so"
    echo "    $JNILIBS/$so"
done

cp -f "$TMP/aar/classes.jar" "$LIBS_DIR/pvr_classes.jar"
echo "    $LIBS_DIR/pvr_classes.jar"

echo "==> Done. Pico Neo 2 SDK placed for build."
