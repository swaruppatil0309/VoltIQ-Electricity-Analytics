#!/usr/bin/env python3
"""Fetch the UCI "Individual household electric power consumption" dataset.

The file is ~127 MiB compressed / 133 MB raw, so it is deliberately NOT stored in
git. Run this once (locally, or at Docker build time) to populate data/:

    python scripts/fetch_data.py                # -> data/household_power_consumption.txt
    python scripts/fetch_data.py --keep-zip     # keep the .zip next to the .txt
    python scripts/fetch_data.py --out-dir /data

Sources are tried in order until one works. Exit code 0 means the dataset is in
place and parseable.
"""
from __future__ import annotations

import argparse
import io
import sys
import urllib.request
import zipfile
from pathlib import Path

EXPECTED_ROWS = 2_075_259       # rows in the official UCI file (header included)
MIN_ROWS = 2_000_000            # sanity floor: never accept a truncated download

SOURCES = [
    # Official UCI archive (may be blocked by some corporate networks)
    "https://archive.ics.uci.edu/static/public/235/household+power+consumption.zip",
    # Public mirrors of the same official file
    "https://codeload.github.com/khalilhanna/household_power_consumption/zip/refs/heads/master",
    "https://codeload.github.com/Ripunjoy93/individual_household_electric_power_consumption/zip/refs/heads/master",
]
MIRROR_MEMBER = "power_consumption/data/raw/household_power_consumption.zip"
TARGET_NAME = "household_power_consumption.txt"


def log(msg: str) -> None:
    print(f"[fetch_data] {msg}", flush=True)


def download(url: str, timeout: int = 120) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "VoltIQ/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return response.read()


def extract_txt(payload: bytes) -> bytes:
    """Return the raw .txt bytes from a downloaded archive (or pass text through)."""
    if payload[:1] not in (b"P",) or b"Date;Time" in payload[:200]:
        return payload  # already the plain text file
    with zipfile.ZipFile(io.BytesIO(payload)) as outer:
        names = outer.namelist()
        direct = [n for n in names if n.endswith(TARGET_NAME)]
        if direct:
            return outer.read(direct[0])
        inner_zip = [n for n in names if n.endswith("household_power_consumption.zip")]
        if inner_zip:
            with zipfile.ZipFile(io.BytesIO(outer.read(inner_zip[0]))) as inner:
                nested = [n for n in inner.namelist() if n.endswith(TARGET_NAME)]
                if nested:
                    return inner.read(nested[0])
    raise ValueError("archive did not contain household_power_consumption.txt")


def validate(path: Path) -> int:
    with path.open("rb") as handle:
        rows = sum(1 for _ in handle)
    if rows < MIN_ROWS:
        raise ValueError(f"{path.name} has only {rows:,} rows (expected ~{EXPECTED_ROWS:,})")
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", default="data", help="destination directory")
    parser.add_argument("--force", action="store_true", help="re-download even if present")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / TARGET_NAME

    if target.exists() and not args.force:
        rows = validate(target)
        log(f"already present: {target} ({rows:,} rows)")
        return 0

    errors = []
    for url in SOURCES:
        try:
            log(f"downloading {url}")
            payload = download(url)
            log(f"  {len(payload) / 2**20:.1f} MiB received")
            text = extract_txt(payload)
            tmp = target.with_suffix(".part")
            tmp.write_bytes(text)
            rows = validate(tmp)
            tmp.replace(target)
            log(f"saved {target} ({rows:,} rows, {target.stat().st_size / 2**20:.0f} MiB)")
            return 0
        except Exception as exc:  # noqa: BLE001 - try the next mirror
            errors.append(f"{url} -> {exc}")
            log(f"  failed: {exc}")

    log("all sources failed:")
    for err in errors:
        log(f"  - {err}")
    log(
        "Download the file manually from "
        "https://archive.ics.uci.edu/dataset/235/individual+household+electric+power+consumption "
        f"and save it as {target}"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
