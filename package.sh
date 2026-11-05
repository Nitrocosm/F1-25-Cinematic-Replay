#!/usr/bin/env bash
# Builds the addon and packages it as dist/xsoul.cinematic-replay.swoftyaddon: a zip with swofty.json at its root
# and only what players need (the DLL, the settings template, the docs and the licenses).
set -euo pipefail
cd "$(dirname "$0")"
bash addon/build.sh
bash addon/tests/run.sh
stage=dist/stage
rm -rf "$stage"
mkdir -p "$stage/native" "$stage/icons"
cp addon/swofty.json addon/cinecam.cfg "$stage/"
cp addon/native/cinecam.dll "$stage/native/"
cp addon/icons/cinematic-replay.png "$stage/icons/"
cp README.md CONTROLS.md LICENSE THIRD_PARTY_NOTICES.md "$stage/"
out=dist/xsoul.cinematic-replay.swoftyaddon
rm -f "$out"
python - "$stage" "$out" <<'EOF'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(stage):
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, stage).replace(os.sep, "/"))
EOF
rm -rf "$stage"
version=$(python -c "import json; print(json.load(open('addon/swofty.json'))['version'])")
echo "packaged $out (version $version)"
