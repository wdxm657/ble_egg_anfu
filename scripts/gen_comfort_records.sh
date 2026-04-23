#!/bin/sh
set -eu

# Generate comfortRecordList into BARK_RESOURCE_FILE for testing.
# Target file in firmware: /userdata/device_initial_resource.json

FILE="${1:-/userdata/device_initial_resource.json}"

# Total records to generate (<=16)
N="${2:-8}"

# How many last records are "running" (endTs = 0xFFFFFFFF)
RUNNING="${3:-1}"

if [ "$N" -lt 1 ] || [ "$N" -gt 16 ]; then
  echo "N must be 1..16" >&2
  exit 1
fi
if [ "$RUNNING" -lt 0 ] || [ "$RUNNING" -gt "$N" ]; then
  echo "RUNNING must be 0..N" >&2
  exit 1
fi

now="$(date +%s)"

# Build JSON using python3 (avoid jq dependency)
python3 - "$FILE" "$N" "$RUNNING" "$now" <<'PY'
import json, sys, os

path = sys.argv[1]
n = int(sys.argv[2])
running = int(sys.argv[3])
now = int(sys.argv[4])

try:
    with open(path, "r", encoding="utf-8") as f:
        obj = json.load(f)
except Exception:
    obj = {}

lst = []
# Oldest first. Each record lasts 60s, spaced by 120s.
base = now - n * 120
for i in range(n):
    start = base + i * 120
    if i >= n - running:
        end = 0xFFFFFFFF
    else:
        end = start + 60
    lst.append({"startTs": start, "endTs": end})

obj["comfortRecordList"] = lst

tmp = path + ".tmp"
os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
with open(tmp, "w", encoding="utf-8") as f:
    json.dump(obj, f, ensure_ascii=False, indent=2)
    f.write("\n")
os.replace(tmp, path)

print(f"Wrote {n} records to {path}. running={running}")
PY

echo "OK"

