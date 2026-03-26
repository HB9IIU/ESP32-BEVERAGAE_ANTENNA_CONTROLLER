#!/usr/bin/env python3
"""
decode.py

Reads elop.cpp from the current folder, extracts the first brace-enclosed
byte array (uint8_t style), assumes it is gzip data, and writes the
decompressed HTML to elegant.html in the same folder.

Usage:
    python decode.py
"""

import re
import gzip


INPUT_CPP = "elop.cpp"
OUTPUT_HTML = "elegant.html"


def extract_bytes_from_file(path):
    """Extract comma-separated integer bytes from the first {...} block."""
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        text = f.read()

    # Find the first {...} block
    match = re.search(r"\{([^}]*)\}", text, re.DOTALL)
    if not match:
        raise RuntimeError("No brace-enclosed data '{ ... }' found in file.")

    body = match.group(1)

    # Extract all integer literals (0–255)
    nums = re.findall(r"\d+", body)
    if not nums:
        raise RuntimeError("No numeric byte values found inside '{ ... }' block.")

    return bytes(int(n) & 0xFF for n in nums)


def main():
    print(f"[INFO] Reading C array from: {INPUT_CPP}")

    compressed = extract_bytes_from_file(INPUT_CPP)

    print(f"[INFO] Extracted {len(compressed)} bytes of compressed data")
    html = gzip.decompress(compressed)

    with open(OUTPUT_HTML, "wb") as f:
        f.write(html)

    print(f"[OK] Wrote {len(html)} bytes of HTML to: {OUTPUT_HTML}")


if __name__ == "__main__":
    main()
