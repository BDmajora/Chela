#!/usr/bin/env python3
"""Download one complete APEX profile from the exact requested Kimi K3 repo."""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import re
import shutil
import sys

REPO = "mudler/Kimi-K3-heretic-APEX-GGUF"
SPLIT = re.compile(r"^(.*)-(\d{5})-of-(\d{5})\.gguf$", re.IGNORECASE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="I-Compact", help="Exact profile label; default I-Compact")
    parser.add_argument("--list", action="store_true", help="List GGUF files and sizes without downloading")
    parser.add_argument("--dry-run", action="store_true", help="Check access, files and storage only")
    parser.add_argument("--output", type=Path, default=Path.home() / "models/Kimi-K3-heretic-APEX")
    args = parser.parse_args()
    try:
        from huggingface_hub import HfApi, hf_hub_download
    except ImportError:
        parser.error("Activate your InferSSD venv and run: python -m pip install huggingface_hub")

    print(f"Repository: {REPO}", flush=True)
    info = HfApi().model_info(REPO, files_metadata=True)
    if not re.fullmatch(r"[a-fA-F0-9]{40}", info.sha or ""):
        raise ValueError("Could not pin a repository revision")
    files = [f for f in info.siblings if f.rfilename.lower().endswith(".gguf")]
    if args.list:
        for f in sorted(files, key=lambda entry: entry.rfilename):
            size = f"{f.size / 1024 ** 3:.2f} GiB" if f.size is not None else "size unavailable"
            print(f"{size:>18}  {f.rfilename}")
        if not files:
            raise ValueError("This repository has no GGUF files")
        return

    pattern = re.compile(r"(?:^|[/_.-])" + re.escape(args.profile) + r"(?:[/_.-]|$)", re.IGNORECASE)
    selected = [f for f in files if pattern.search(f.rfilename)
                and not re.search(r"(?:^|[/_.-])(?:mmproj|mtp|draft|imatrix)(?:[/_.-]|$)", f.rfilename, re.IGNORECASE)]
    if not selected:
        raise ValueError(f"No main-model GGUF files match {args.profile!r}. Run with --list to see available files.")
    groups = {}
    for f in selected:
        path = PurePosixPath(f.rfilename)
        if path.is_absolute() or ".." in path.parts or "\\" in f.rfilename:
            raise ValueError("Unsafe repository filename")
        if f.size is None or f.size < 0:
            raise ValueError(f"Missing size metadata: {f.rfilename}")
        match = SPLIT.match(path.name)
        key = str(path.parent / match[1]) if match else f.rfilename
        groups.setdefault(key, []).append(f)
    if len(groups) != 1:
        raise ValueError("Multiple model sets match; use a more specific --profile or inspect --list")
    first = SPLIT.match(PurePosixPath(selected[0].rfilename).name)
    if first:
        count = int(first[3])
        parts = [SPLIT.match(PurePosixPath(f.rfilename).name) for f in selected]
        if (not 1 <= count <= 99999 or any(not p or int(p[3]) != count for p in parts)
                or sorted(int(p[2]) for p in parts) != list(range(1, count + 1))):
            raise ValueError("Incomplete model shard set; refusing to download")

    folder = args.output.expanduser().absolute()
    if folder.is_symlink():
        raise ValueError("The output directory must not be a symlink")
    remaining = 0
    for f in selected:
        path = folder / f.rfilename
        if path.is_symlink() or not path.resolve().is_relative_to(folder.resolve()):
            raise ValueError("A model path redirects outside the output directory")
        if not path.is_file() or path.stat().st_size != f.size:
            remaining += f.size
    probe = folder
    while not probe.exists():
        probe = probe.parent
    free = shutil.disk_usage(probe).free
    print(f"Revision: {info.sha}\nDestination: {folder}")
    for f in selected:
        print(f"{f.size / 1024 ** 3:.2f} GiB  {f.rfilename}")
    print(f"Total: {sum(f.size for f in selected) / 1024 ** 3:.2f} GiB; free: {free / 1024 ** 3:.2f} GiB", flush=True)
    if remaining and free < remaining + 1024 ** 3:
        raise ValueError("Not enough free disk space plus 1 GiB headroom. Partial-cache bytes are not credited in this conservative estimate.")
    if args.dry_run:
        print("Check complete; no weights downloaded.")
        return

    folder.mkdir(parents=True, exist_ok=True)
    for f in sorted(selected, key=lambda entry: entry.rfilename):
        lfs = getattr(f, "lfs", None)
        checksum = lfs.get("sha256") if isinstance(lfs, dict) else getattr(lfs, "sha256", None)
        if not checksum:
            raise ValueError(f"No SHA-256 available for {f.rfilename}; refusing an unverified download")
        path = Path(hf_hub_download(REPO, f.rfilename, revision=info.sha, local_dir=folder))
        if path.resolve() != (folder / f.rfilename).resolve() or path.stat().st_size != f.size:
            raise ValueError(f"Unexpected path or size for {f.rfilename}")
        print(f"Verifying {f.rfilename}", flush=True)
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            if stream.read(4) != b"GGUF":
                raise ValueError(f"Not a GGUF file: {f.rfilename}")
            stream.seek(0)
            for chunk in iter(lambda: stream.read(8 * 1024 ** 2), b""):
                digest.update(chunk)
        if digest.hexdigest().lower() != checksum.lower():
            raise ValueError(f"Checksum mismatch: {f.rfilename}; remove that file and rerun")
    print(f"Downloaded and verified: {folder}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("Stopped. Rerun the same command to resume.")
    except Exception as exc:
        status = getattr(getattr(exc, "response", None), "status_code", None)
        if status in (401, 403, 404):
            sys.exit(f"Cannot access {REPO} (HTTP {status}). Check the exact repository name. "
                     "If it is private or gated, use 'hf auth login' with an account that has access.")
        sys.exit(str(exc))
