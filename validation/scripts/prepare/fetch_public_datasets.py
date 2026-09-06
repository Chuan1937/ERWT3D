#!/usr/bin/env python3
"""Download the public F3 SEG-Y inputs without committing binary data."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import urllib.request
from pathlib import Path

VALIDATION = Path(__file__).resolve().parents[2]
SOURCES = VALIDATION / "datasets" / "sources.json"
CACHE = VALIDATION / "datasets" / "cache"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url: str, destination: Path) -> None:
    temporary = destination.with_suffix(destination.suffix + ".partial")
    with urllib.request.urlopen(url) as response, temporary.open("wb") as output:
        shutil.copyfileobj(response, output, length=1024 * 1024)
    temporary.replace(destination)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", action="append", choices=("f3_amplitude", "f3_similarity"))
    parser.add_argument("--output-dir", type=Path, default=CACHE)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    sources = json.loads(SOURCES.read_text())
    selected = args.dataset or list(sources)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    manifest = []
    for dataset_id in selected:
        source = sources[dataset_id]
        target = args.output_dir / f"{dataset_id}.sgy"
        if target.exists() and not args.force:
            print(f"[KEEP] {target}")
        else:
            print(f"[FETCH] {dataset_id}: {source['raw_url']}")
            download(source["raw_url"], target)
        manifest.append({
            "dataset_id": dataset_id,
            "file": target.name,
            "bytes": target.stat().st_size,
            "sha256": sha256(target),
            "source": source["raw_url"],
            "repository": source["repository"],
            "license": source["license"],
        })
    (args.output_dir / "download_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
