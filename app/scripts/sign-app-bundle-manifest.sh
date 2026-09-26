#!/usr/bin/env bash
set -euo pipefail
umask 077

payload="dist/mobile/live-stocking-update.payload"
signature="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/live-stocking-update.signature"
manifest="dist/mobile/live-stocking-update.json"
keystore="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/live-stocking-bundle-upload.jks"
trap 'rm -f "$signature" "$keystore" "$payload"' EXIT

if [[ ! -s "$payload" ]]; then
  echo "React Native bundle update payload is missing" >&2
  exit 1
fi
python3 -c 'import base64, os, sys; sys.stdout.buffer.write(base64.b64decode(os.environ["ANDROID_KEYSTORE_BASE64"]))' > "$keystore"
java app/scripts/SignAppBundleManifest.java \
  "$keystore" \
  "$ANDROID_KEY_ALIAS" \
  "$payload" \
  "$signature"
node app/scripts/wrap-app-bundle-manifest.mjs \
  "$payload" \
  "$signature" \
  "$manifest"
