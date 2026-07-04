#!/usr/bin/env bash
set -euo pipefail

#
# Build script for the WiVRn Pico Neo 2 APK.
#
# Usage:
#   ./scripts/build-apk.sh [--stage <alpha|debug|rc|release>]
#
# Defaults to --stage alpha if not specified.
#
# Environment variables:
#   PVR_SDK_URL      URL to download the PvrSDK archive
#                    Defaults to the Pico Neo 2 SDK collection on archive.org
#   KEYSTORE_BASE64  Base64-encoded keystore file (for signed release builds)
#   KEYSTORE_PASSWORD  Password for the keystore
#   KEY_ALIAS        Key alias inside the keystore (default: default_key)
#   KEY_PASSWORD     Password for the key (default: same as KEYSTORE_PASSWORD)
#
# The default PVR_SDK_URL points to a 7z archive containing multiple Pico SDKs.
# The script extracts pvrSDK-release.aar from the Unity Platform SDK inside it,
# renames it to PvrSDK-Native-release.aar, and extracts the jni/ libraries.
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CLIENT_DIR="${ROOT_DIR}/client/pico_native"
EXTERNAL_DIR="${ROOT_DIR}/external/pvrsdk-native"

STAGE="alpha"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stage)
            STAGE="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

case "${STAGE}" in
    alpha|debug|rc|release) ;;
    *)
        echo "Invalid stage '${STAGE}'. Use one of: alpha, debug, rc, release" >&2
        exit 1
        ;;
esac

echo "=== Building WiVRn Pico Neo 2 APK ==="
echo "Stage: ${STAGE}"

# --- Version string: year-day-month-hour ---
VERSION_YEAR=$(date -u +%Y)
VERSION_DAY=$(date -u +%d)
VERSION_MONTH=$(date -u +%m)
VERSION_HOUR=$(date -u +%H)

VERSION_NAME="${VERSION_YEAR}.${VERSION_DAY}.${VERSION_MONTH}.${VERSION_HOUR}"
VERSION_CODE="${VERSION_YEAR}${VERSION_MONTH}${VERSION_DAY}${VERSION_HOUR}"

echo "Version name: ${VERSION_NAME}"
echo "Version code: ${VERSION_CODE}"

# --- Map stage to Gradle build type ---
case "${STAGE}" in
    alpha)
        GRADLE_BUILD_TYPE="debug"
        APK_SUFFIX="-alpha"
        ;;
    debug)
        GRADLE_BUILD_TYPE="releaseDebuggable"
        APK_SUFFIX="-debug"
        ;;
    rc)
        GRADLE_BUILD_TYPE="release"
        APK_SUFFIX="-rc"
        APP_ID_SUFFIX=".rc"
        ;;
    release)
        GRADLE_BUILD_TYPE="release"
        APK_SUFFIX=""
        APP_ID_SUFFIX=".release"
        ;;
esac

# --- Set up PvrSDK ---
DEFAULT_PVR_SDK_URL="https://archive.org/download/pico-neo-2-sdks-exes.-7z/PicoNeo2-SDKs-EXEs.7z"
PVR_SDK_URL="${PVR_SDK_URL:-${DEFAULT_PVR_SDK_URL}}"

if [[ ! -f "${EXTERNAL_DIR}/PvrSDK-Native-release.aar" ]]; then
    echo "Downloading PvrSDK from ${PVR_SDK_URL}..."
    mkdir -p "${EXTERNAL_DIR}"
    TMP_WORK="/tmp/pvrsdk-extract"
    rm -rf "${TMP_WORK}"
    mkdir -p "${TMP_WORK}"

    case "${PVR_SDK_URL}" in
        *.7z)
            curl -fSL -o "${TMP_WORK}/archive.7z" "${PVR_SDK_URL}"

            # Extract the Unity Platform SDK zip from the 7z
            7z x -y "${TMP_WORK}/archive.7z" \
                -o"${TMP_WORK}" \
                "PicoNeo2-SDKs-EXEs/Unity+Plugin/PicoXR_Platform_SDK-1.2.5_B81.zip" \
                >/dev/null 2>&1

            # Extract pvrSDK-release.aar from the Platform SDK zip
            unzip -o "${TMP_WORK}/PicoNeo2-SDKs-EXEs/Unity+Plugin/PicoXR_Platform_SDK-1.2.5_B81.zip" \
                "Runtime/Android/pvrSDK-release.aar" \
                -d "${TMP_WORK}" >/dev/null 2>&1

            # Place the AAR with the expected name
            cp "${TMP_WORK}/Runtime/Android/pvrSDK-release.aar" \
                "${EXTERNAL_DIR}/PvrSDK-Native-release.aar"

            # Extract jni/ libraries from the AAR
            cd "${EXTERNAL_DIR}"
            unzip -o "${TMP_WORK}/Runtime/Android/pvrSDK-release.aar" \
                "jni/arm64-v8a/*" >/dev/null 2>&1
            cd - > /dev/null

            # Rename libPvr_UnitySDK.so to libPvr_NativeSDK.so
            if [[ -f "${EXTERNAL_DIR}/jni/arm64-v8a/libPvr_UnitySDK.so" ]]; then
                mv "${EXTERNAL_DIR}/jni/arm64-v8a/libPvr_UnitySDK.so" \
                   "${EXTERNAL_DIR}/jni/arm64-v8a/libPvr_NativeSDK.so"
            fi
            ;;
        *.zip)
            curl -fSL -o "${TMP_WORK}/archive.zip" "${PVR_SDK_URL}"
            unzip -o "${TMP_WORK}/archive.zip" -d "${EXTERNAL_DIR}"
            ;;
        *.tar.gz|*.tgz)
            curl -fSL -o "${TMP_WORK}/archive.tar.gz" "${PVR_SDK_URL}"
            tar xzf "${TMP_WORK}/archive.tar.gz" -C "${EXTERNAL_DIR}"
            ;;
        *)
            echo "Unknown archive format for PVR_SDK_URL" >&2
            exit 1
            ;;
    esac

    rm -rf "${TMP_WORK}"
fi

if [[ ! -f "${EXTERNAL_DIR}/PvrSDK-Native-release.aar" ]]; then
    echo "PvrSDK-Native-release.aar not found at ${EXTERNAL_DIR}/" >&2
    exit 1
fi

# --- Set up signing (for release/rc builds) ---
GRADLE_PROPERTIES=()
GRADLE_PROPERTIES+=("-Pwivrn_version=${VERSION_NAME}")
GRADLE_PROPERTIES+=("-Pwivrn_versionCode=${VERSION_CODE}")

if [[ "${STAGE}" == "rc" || "${STAGE}" == "release" ]]; then
    if [[ -n "${KEYSTORE_BASE64:-}" && -n "${KEYSTORE_PASSWORD:-}" ]]; then
        echo "Setting up signing keystore..."
        echo "${KEYSTORE_BASE64}" | base64 -d > "${CLIENT_DIR}/ks.keystore"
        KEY_ALIAS="${KEY_ALIAS:-default_key}"
        KEY_PASSWORD="${KEY_PASSWORD:-${KEYSTORE_PASSWORD}}"
        GRADLE_PROPERTIES+=("-PsigningKeyPassword=${KEYSTORE_PASSWORD}")
        GRADLE_PROPERTIES+=("-PkeyAlias=${KEY_ALIAS}")
        GRADLE_PROPERTIES+=("-PkeyPassword=${KEY_PASSWORD}")
    else
        echo "WARNING: No keystore provided. ${STAGE} build will be unsigned." >&2
    fi
fi

if [[ -n "${APP_ID_SUFFIX:-}" ]]; then
    GRADLE_PROPERTIES+=("-Psuffix=${APP_ID_SUFFIX}")
fi

# --- Run the build ---
cd "${CLIENT_DIR}"
chmod +x gradlew

echo "Running Gradle build (type: ${GRADLE_BUILD_TYPE})..."
./gradlew "assemble${GRADLE_BUILD_TYPE^}" \
    --no-daemon \
    --stacktrace \
    "${GRADLE_PROPERTIES[@]}"

# --- Find and rename the output APK ---
APK_DIR="${CLIENT_DIR}/build/outputs/apk/${GRADLE_BUILD_TYPE}"
if [[ ! -d "${APK_DIR}" ]]; then
    echo "APK output directory not found: ${APK_DIR}" >&2
    exit 1
fi

SOURCE_APK=$(find "${APK_DIR}" -name "*.apk" | head -1)
if [[ -z "${SOURCE_APK}" ]]; then
    echo "No APK found in ${APK_DIR}" >&2
    exit 1
fi

OUTPUT_DIR="${ROOT_DIR}/build-output"
mkdir -p "${OUTPUT_DIR}"
OUTPUT_APK="${OUTPUT_DIR}/wivrn-neo2-${VERSION_NAME}${APK_SUFFIX}.apk"

cp "${SOURCE_APK}" "${OUTPUT_APK}"
echo ""
echo "=== Build complete ==="
echo "APK: ${OUTPUT_APK}"
echo "Version: ${VERSION_NAME}${APK_SUFFIX}"
echo "Build type: ${GRADLE_BUILD_TYPE}"
