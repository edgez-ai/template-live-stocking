#!/usr/bin/env bash
set -euo pipefail
umask 077

sdk_root="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [[ -z "$sdk_root" || ! -d "$sdk_root/build-tools" ]]; then
  echo "Android SDK build tools are unavailable" >&2
  exit 1
fi

build_tools="$(find "$sdk_root/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
zipalign="$build_tools/zipalign"
apksigner="$build_tools/apksigner"
if [[ ! -x "$zipalign" || ! -x "$apksigner" ]]; then
  echo "zipalign or apksigner is unavailable in $build_tools" >&2
  exit 1
fi

apk_dir="app/android/app/build/outputs/apk/release"
mapfile -t apks < <(find "$apk_dir" -maxdepth 1 -type f -name '*.apk')
if [[ "${#apks[@]}" -ne 1 ]]; then
  echo "Expected one unsigned release APK in $apk_dir" >&2
  exit 1
fi

mkdir -p dist/mobile
unsigned="dist/mobile/live-stocking-unsigned.apk"
signed="dist/mobile/live-stocking-signed.apk"
aligned="$RUNNER_TEMP/live-stocking-aligned.apk"
keystore="$RUNNER_TEMP/live-stocking-upload.jks"
trap 'rm -f "$aligned" "$keystore"' EXIT

cp "${apks[0]}" "$unsigned"
if "$apksigner" verify "$unsigned" >/dev/null 2>&1; then
  echo "The unsigned release APK was unexpectedly signed" >&2
  exit 1
fi

printf '%s' "$ANDROID_KEYSTORE_BASE64" | base64 --decode > "$keystore"
"$zipalign" -P 16 -f 4 "$unsigned" "$aligned"
"$apksigner" sign \
  --ks "$keystore" \
  --ks-key-alias "$ANDROID_KEY_ALIAS" \
  --ks-pass env:ANDROID_KEYSTORE_PASSWORD \
  --key-pass env:ANDROID_KEYSTORE_PASSWORD \
  --out "$signed" \
  "$aligned"
"$apksigner" verify --verbose --print-certs "$signed"
