#!/data/data/com.termux/files/usr/bin/sh
# Patch the APK with a new libmpclient.so (Termux).
#   sh patch.sh                      -> takes the newest libmpclient*.so from /storage/emulated/0/Download
#   sh patch.sh path/libmpclient.so  -> or a given file
# Result: /storage/emulated/0/Download/XHYNMP_mp5.apk  (package com.xhynph.xhynv.mpmod5, same key = installs over the old one)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DL=/storage/emulated/0/Download
SO="${1:-$(ls -t "$DL"/libmpclient*.so 2>/dev/null | head -1)}"
BASE="${BASE:-$HERE/XHYNMP_base.apk}"
OUT="${OUT:-$DL/XHYNMP_mp5.apk}"
[ -n "$SO" ] && [ -f "$SO" ] || { echo "No libmpclient.so found. Put it in $DL or: sh patch.sh /path/libmpclient.so"; exit 1; }
[ -f "$BASE" ] || { echo "Missing $BASE (the original APK, keep it next to this script)"; exit 1; }
python3 -c "import cryptography" 2>/dev/null || { echo "installing python + cryptography (once)..."; pkg install -y python python-cryptography; }
echo "so:   $SO"; echo "base: $BASE"
python3 "$HERE/patchapk.py" "$BASE" "$SO" "$OUT" "$HERE/mpmod.key.pem" com.xhynph.xhynv.mpmod4 com.xhynph.xhynv.mpmod5
echo "DONE -> $OUT   (open it from the Files app to install)"
