#!/usr/bin/env python3
# Build a raw exFAT release image from a title folder and verify every file's
# content round-trips. Uses MkPFS's exfat_writer (GPL-3 tool; we invoke it).
import hashlib, sys
from pathlib import Path
from mkpfs.exfat import open_exfat
from mkpfs.exfat_writer import write_exfat_image
source, output = map(Path, sys.argv[1:])
write_exfat_image(source, output)
reader = open_exfat(str(output)); count = 0
for entry in reader.iter_files():
    want = hashlib.sha256((source / entry.rel_path).read_bytes()).digest()
    got = hashlib.sha256()
    for chunk in reader.read_file(entry):
        got.update(chunk)
    if got.digest() != want:
        raise SystemExit("exFAT payload mismatch: " + entry.rel_path)
    count += 1
print(f"exFAT verified: {count} files, {output.stat().st_size} bytes")
