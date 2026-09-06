#!/usr/bin/env python3
"""Create a transferable public-data bundle and an SHA256 sidecar."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import tarfile
from pathlib import Path

VALIDATION = Path(__file__).resolve().parents[2]


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepared-dir", type=Path, default=VALIDATION / "datasets" / "prepared")
    parser.add_argument("--output-dir", type=Path, default=VALIDATION / "datasets" / "bundles")
    parser.add_argument("--name", default="erwt3d_paper_public_datasets.tar.gz")
    args = parser.parse_args()
    inputs = sorted(args.prepared_dir.glob("*.raw")) + sorted(args.prepared_dir.glob("*.metadata.json"))
    if not inputs:
        raise SystemExit(f"no prepared datasets in {args.prepared_dir}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = args.output_dir / args.name
    if output.suffix != ".gz" or not output.name.endswith(".tar.gz"):
        raise SystemExit("only .tar.gz is supported for portable stdlib-only bundles")
    with tarfile.open(output, "w:gz") as archive:
        for item in inputs:
            archive.add(item, arcname=f"prepared/{item.name}")
        archive.add(VALIDATION / "datasets" / "sources.json", arcname="sources.json")
    checksum = output.with_name(output.name + ".sha256")
    checksum.write_text(f"{digest(output)}  {output.name}\n")
    print(f"[OK] {output}")
    print(f"[OK] {checksum}")


if __name__ == "__main__":
    main()
