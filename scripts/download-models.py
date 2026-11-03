#!/usr/bin/env python3
"""Download Brody's six requested Hugging Face checkpoints into ~/models.

Uses the exact repository IDs supplied by the user. Defaults to original
safetensors weights and their supporting files; GGUF mode requires a selected
quantization. Use --repo ALIAS=OWNER/NAME to explicitly change a repository.
Downloads resume through huggingface_hub. No model code is run.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import glob
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tempfile
import os


MODELS = {
    "qwen": "JonathanColetti/Qwen3.8-2.4T-A95B-Uncensored",
    "gemma": "llmfan46/gemma-4-26B-A4B-it-ultra-uncensored-heretic",
    "deepseek": "DavidAU/DeepSeek-V4.1-Flash-Abliterated",
    "kimi3": "llmfan46/Kimi-K3-it-ultra-uncensored-heretic",
    "kimi25": "HauhauCS/Kimi-K2.5-1T-A32B-Uncensored-Balanced",
    "glm": "llmfan46/GLM-5.3-Flash-ultra-uncensored-heretic",
}
SPLIT = re.compile(r"^(.*)-(\d{5})-of-(\d{5})\.gguf$", re.IGNORECASE)
SAFETENSORS_SPLIT = re.compile(r"^(.*)-(\d+)-of-(\d+)\.safetensors$", re.IGNORECASE)


@dataclass(frozen=True)
class File:
    name: str
    size: int
    sha256: str | None = None
    blob_id: str | None = None


def repositories(overrides):
    catalog = dict(MODELS)
    seen = set()
    for override in overrides:
        alias, separator, repo = override.partition("=")
        if not separator or alias not in MODELS:
            raise ValueError("--repo requires ALIAS=OWNER/NAME, using an alias from --list")
        if alias in seen:
            raise ValueError(f"More than one --repo supplied for {alias}")
        if (not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*/[A-Za-z0-9_][A-Za-z0-9_.-]*", repo)
                or ".." in repo or "--" in repo or repo.endswith((".", "-", ".git"))):
            raise ValueError("Use a Hugging Face repository ID (OWNER/NAME), not a URL or local path")
        catalog[alias] = repo
        seen.add(alias)
    return catalog


def field(value, name, default=None):
    return value.get(name, default) if isinstance(value, dict) else getattr(value, name, default)


def safe_name(name):
    path = PurePosixPath(name)
    if (not name or path.is_absolute() or ".." in path.parts or "\\" in name
            or any(ord(c) < 32 or ord(c) == 127 for c in name)):
        raise ValueError(f"Unsafe repository filename: {name!r}")
    return path


def inventory(info):
    files = []
    for entry in info.siblings or []:
        name = entry.rfilename
        safe_name(name)
        size = field(entry, "size")
        if size is None or size < 0:
            raise ValueError(f"Missing file-size metadata for {name}; cannot estimate storage.")
        lfs = field(entry, "lfs")
        checksum = field(lfs, "sha256")
        if lfs and not checksum:
            raise ValueError(f"Missing LFS checksum for {name}")
        if checksum and not re.fullmatch(r"[a-fA-F0-9]{64}", checksum):
            raise ValueError(f"Invalid SHA-256 metadata for {name}")
        files.append(File(name, size, checksum, field(entry, "blob_id")))
    return files


def check_shards(files, expression):
    groups = {}
    for file in files:
        path = PurePosixPath(file.name)
        match = expression.match(path.name)
        key = str(path.parent / match[1]) if match else file.name
        groups.setdefault(key, []).append(file)
    for parts in groups.values():
        match = expression.match(PurePosixPath(parts[0].name).name)
        if match:
            count = int(match[3])
            matches = [expression.match(PurePosixPath(f.name).name) for f in parts]
            if (not 1 <= count <= 99999 or any(int(m[3]) != count for m in matches)
                    or sorted(int(m[2]) for m in matches) != list(range(1, count + 1))):
                raise ValueError("Incomplete model shard set. Select all numbered shards, not just the first file.")
    return groups


def support_file(file, folders):
    path = PurePosixPath(file.name)
    leaf = path.name.lower()
    if str(path.parent) not in folders:
        return False
    return (leaf.startswith(("config", "generation_config", "tokenizer", "processor", "preprocessor",
                             "chat_template", "special_tokens", "added_tokens", "vocab", "merges", "license"))
            or leaf == "readme.md" or leaf.endswith(".safetensors.index.json")) and path.suffix.lower() not in {".py", ".pyc"}


def select_files(files, format_, quant="Q8_0", gguf_glob=None, original_subdir=None):
    if format_ == "original":
        weights = [f for f in files if f.name.endswith(".safetensors")
                   and not PurePosixPath(f.name).name.lower().startswith(("optimizer", "adapter_"))]
        folders = {str(PurePosixPath(f.name).parent) for f in weights}
        if original_subdir is not None:
            folder = str(safe_name(original_subdir)) if original_subdir != "." else "."
        elif "." in folders:
            folder = "."
        elif len(folders) == 1:
            folder = next(iter(folders))
        elif not folders:
            raise ValueError("No original safetensors weights found. If this is a GGUF repository, use --format gguf.")
        else:
            raise ValueError("Multiple weight folders found; select one with --original-subdir: " + ", ".join(sorted(folders)))
        weights = [f for f in weights if str(PurePosixPath(f.name).parent) == folder]
        if not weights:
            raise ValueError(f"No safetensors weights in folder {folder}")
        check_shards(weights, SAFETENSORS_SPLIT)
        return sorted(weights + [f for f in files if support_file(f, {".", folder})], key=lambda f: f.name)

    import fnmatch
    pattern = re.compile(r"(?:^|[-_.])" + re.escape(quant) + r"(?:[-_.]|$)", re.IGNORECASE)
    weights = []
    for file in files:
        leaf = PurePosixPath(file.name).name.lower()
        if not leaf.endswith(".gguf") or leaf.startswith(("mmproj", "mtp", "imatrix")) or "imatrix" in leaf:
            continue
        matches = fnmatch.fnmatchcase(file.name, gguf_glob) if gguf_glob else bool(pattern.search(file.name))
        if matches:
            weights.append(file)
    if not weights:
        raise ValueError(f"No main-model GGUF files matched {gguf_glob or quant}. This exact repository may publish only original weights.")
    groups = check_shards(weights, SPLIT)
    if len(groups) != 1:
        raise ValueError("Multiple GGUF model sets matched; choose one with --gguf-glob: " + ", ".join(sorted(groups)))
    return sorted(weights + [f for f in files if PurePosixPath(f.name).name.lower().startswith(("readme", "license"))], key=lambda f: f.name)


def output_path(folder, file):
    relative = safe_name(file.name)
    path = folder.joinpath(*relative.parts)
    root = folder.resolve()
    if path.is_symlink() or not path.resolve().is_relative_to(root):
        raise ValueError(f"Refusing a symlink or path outside the model folder: {file.name}")
    return path


def bytes_needed(folder, files, force=False):
    total = 0
    for file in files:
        path = output_path(folder, file)
        if not force and path.is_file() and path.stat().st_size == file.size:
            continue
        etag = file.sha256 or file.blob_id
        cached = folder / ".cache/huggingface/download" / PurePosixPath(file.name).parent
        pattern = glob.escape(PurePosixPath(file.name).name + "." + (etag or "")) + ".incomplete"
        partial = max((p.stat().st_size for p in cached.glob(pattern) if p.is_file()), default=0) if etag else 0
        total += max(0, file.size - partial)
    return total


def verify(path, file):
    if path.stat().st_size != file.size:
        raise ValueError(f"Size mismatch: {file.name}")
    if not file.sha256 and not file.blob_id:
        raise ValueError(f"No checksum available for {file.name}")
    digest = hashlib.sha256() if file.sha256 else hashlib.sha1()
    if not file.sha256:
        digest.update(f"blob {file.size}\0".encode())
    with path.open("rb") as stream:
        if file.name.lower().endswith(".gguf") and stream.read(4) != b"GGUF":
            raise ValueError(f"Not a GGUF file: {file.name}")
        stream.seek(0)
        for chunk in iter(lambda: stream.read(8 * 1024 ** 2), b""):
            digest.update(chunk)
    if digest.hexdigest().lower() != (file.sha256 or file.blob_id).lower():
        raise ValueError(f"Checksum mismatch: {file.name}. Retry with --force-download.")


def write_receipt(folder, repo, revision, files, format_, requested_repo=None):
    receipt = {"repository": repo, "revision": revision, "format": format_, "verified": True,
               "files": [{"path": f.name, "bytes": f.size, "sha256": f.sha256, "git_blob_sha1": f.blob_id} for f in files]}
    if requested_repo and requested_repo != repo:
        receipt["requested_repository"] = requested_repo
    fd, temporary = tempfile.mkstemp(dir=folder, prefix=".chela-download-")
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(receipt, stream, indent=2)
            stream.write("\n")
        os.replace(temporary, folder / ".chela-download.json")
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("models", nargs="*", help="Aliases from --list; defaults to all six")
    parser.add_argument("--all", action="store_true", help="Download all six requested repositories")
    parser.add_argument("--list", action="store_true", help="Show the exact repository catalog, then exit")
    parser.add_argument("--repo", action="append", default=[], metavar="ALIAS=OWNER/NAME",
                        help="Explicitly override one alias; repeat for multiple repositories")
    parser.add_argument("--output", type=Path, default=Path.home() / "models")
    parser.add_argument("--dry-run", action="store_true", help="Check access, selected files, and storage without downloading weights")
    parser.add_argument("--format", choices=("original", "gguf"), default="original")
    parser.add_argument("--quant", default="Q8_0", help="GGUF quantization, default Q8_0; never falls back to a smaller quant")
    parser.add_argument("--gguf-glob", help="Explicit GGUF model-set pattern; overrides --quant")
    parser.add_argument("--original-subdir", help="Select a checkpoint folder if a repo contains several")
    parser.add_argument("--revision", default="main")
    parser.add_argument("--workers", type=int, choices=range(1, 9), default=2, metavar="1..8")
    parser.add_argument("--force-download", action="store_true")
    args = parser.parse_args(argv)
    try:
        catalog = repositories(args.repo)
    except ValueError as exc:
        parser.error(str(exc))
    if args.list:
        for alias, repo in catalog.items():
            print(f"{alias:9} {repo}")
            if repo != MODELS[alias]:
                print(f"          Explicit override of {MODELS[alias]}")
        return 0
    if args.all and args.models:
        parser.error("use aliases or --all, not both")
    requested = list(MODELS) if args.all or not args.models else list(dict.fromkeys(args.models))
    if any(alias not in MODELS for alias in requested):
        parser.error("unknown model alias; use --list")
    try:
        from huggingface_hub import HfApi, hf_hub_download
    except ImportError:
        parser.error("Install the downloader in your venv: python -m pip install huggingface_hub")
    api = HfApi()  # Uses an existing hf auth login or HF_TOKEN; no credentials are printed or saved here.
    base = args.output.expanduser().absolute()
    failures = []
    print(f"Destination: {base}\nFormat: {args.format}\n", flush=True)
    if args.format == "original":
        print("Original safetensors checkpoints need conversion and compatibility checks before InferSSD can use them.\n", flush=True)
    for alias in requested:
        repo = catalog[alias]
        try:
            print(f"[{alias}] Checking {repo}", flush=True)
            if repo != MODELS[alias]:
                print(f"  Explicit override of {MODELS[alias]}", flush=True)
            info = api.model_info(repo, revision=args.revision, files_metadata=True)
            revision = info.sha
            if not revision or not re.fullmatch(r"[a-fA-F0-9]{40}", revision):
                raise ValueError("Could not pin the repository revision")
            files = select_files(inventory(info), args.format, args.quant, args.gguf_glob, args.original_subdir)
            folder = base / repo.replace("/", "--")
            if folder.is_symlink():
                raise ValueError("The model output folder is a symlink")
            needed = bytes_needed(folder, files, args.force_download)
            probe = base
            while not probe.exists():
                probe = probe.parent
            free = shutil.disk_usage(probe).free
            print(f"  Revision: {revision}\n  {len(files)} files; total {sum(f.size for f in files) / 1024 ** 3:.2f} GiB; "
                  f"remaining {needed / 1024 ** 3:.2f} GiB; free {free / 1024 ** 3:.2f} GiB", flush=True)
            for file in files:
                print(f"  {file.size / 1024 ** 3:8.3f} GiB  {file.name}", flush=True)
            if needed and free < needed + 1024 ** 3:
                raise ValueError(f"Insufficient storage on the filesystem containing {base}: "
                                 f"need {needed / 1024 ** 3:.2f} GiB plus 1 GiB headroom. "
                                 "Use --output on a larger filesystem or free space there.")
            if args.dry_run:
                print("  Plan only; no weights downloaded.\n", flush=True)
                continue
            folder.mkdir(parents=True, exist_ok=True)
            def download(file):
                expected = output_path(folder, file)
                path = Path(hf_hub_download(repo, file.name, revision=revision, local_dir=folder, force_download=args.force_download))
                if path.resolve() != expected.resolve():
                    raise ValueError(f"Unexpected output path for {file.name}")
                print(f"  Verifying {file.name}", flush=True)
                verify(path, file)
                return file.name
            with ThreadPoolExecutor(max_workers=args.workers) as pool:
                for future in as_completed([pool.submit(download, f) for f in files]):
                    print("  Verified: " + future.result(), flush=True)
            write_receipt(folder, repo, revision, files, args.format, MODELS[alias])
            print(f"  Complete: {folder}\n", flush=True)
        except KeyboardInterrupt:
            print("\nStopped. Rerun the same command to resume.", file=sys.stderr)
            return 130
        except Exception as exc:
            failures.append(alias)
            status = getattr(getattr(exc, "response", None), "status_code", None)
            if status in (401, 403, 404):
                message = (f"Hugging Face returned HTTP {status} for {repo}. "
                           "Check the exact repository ID. If it is private or gated, "
                           "use 'hf auth login' with an account that has access. "
                           "Signing in cannot fix an incorrect repository ID.")
            else:
                message = str(exc)
            print(f"  FAILED [{alias}]: {message}\n  Other models will still be checked.\n", file=sys.stderr, flush=True)
    if failures:
        print("Unavailable or unfinished: " + ", ".join(failures), file=sys.stderr)
        return 1
    print("All selected plans checked." if args.dry_run else "All selected downloads completed and verified.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
