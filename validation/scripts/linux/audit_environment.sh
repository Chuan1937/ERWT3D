#!/usr/bin/env bash
# Capture the Linux host and the actual filesystems backing formal benchmark paths.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
require_roots

out_dir="$ERWT3D_RESULT_ROOT/environment"
mkdir -p "$out_dir"
python3 - "$out_dir/server.json" "$ERWT3D_DATASET_ROOT" "$ERWT3D_SSD_ROOT" "$ERWT3D_HDD_ROOT" <<'PY'
import json, os, platform, shutil, subprocess, sys
from datetime import datetime, timezone
out, dataset_root, ssd_root, hdd_root = sys.argv[1:]
def cmd(*argv):
    try: return subprocess.check_output(argv, text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError): return None
def mount(path):
    return cmd("findmnt", "-J", "-T", path) or "unavailable"
payload = {
  "audit_timestamp_utc": datetime.now(timezone.utc).isoformat(),
  "hostname": platform.node(), "kernel": platform.platform(),
  "cpu": cmd("lscpu", "-J"), "memory": cmd("free", "-b"),
  "block_devices": cmd("lsblk", "-J", "-o", "NAME,MODEL,SERIAL,SIZE,ROTA,TYPE,FSTYPE,MOUNTPOINT"),
  "paths": {name: {"path": path, "mount": mount(path), "disk_usage": shutil.disk_usage(path)._asdict()}
            for name, path in (("dataset_root", dataset_root), ("ssd_root", ssd_root), ("hdd_root", hdd_root))},
  "compiler": {"cc": cmd(os.environ.get("CC", "gcc"), "--version"), "cxx": cmd(os.environ.get("CXX", "g++"), "--version"), "cmake": cmd("cmake", "--version")}
}
open(out, "w").write(json.dumps(payload, indent=2) + "\n")
PY
echo "[OK] $out_dir/server.json"
