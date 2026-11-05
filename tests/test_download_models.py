"""Check file selection, integrity and failures without downloading model weights."""
import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("download_models", ROOT / "scripts/download-models.py")
download = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = download
spec.loader.exec_module(download)
File = download.File


class DownloadTests(unittest.TestCase):
    def test_repository_override_is_explicit_and_scoped_to_one_alias(self):
        repo = "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF"
        catalog = download.repositories([f"qwen={repo}"])
        self.assertEqual(catalog["qwen"], repo)
        self.assertEqual(catalog["gemma"], download.MODELS["gemma"])
        self.assertNotEqual(download.MODELS["qwen"], repo)
        for invalid in (["unknown=" + repo], ["qwen=https://huggingface.co/" + repo],
                        ["qwen=../model"], ["qwen=" + repo, "qwen=" + repo]):
            with self.assertRaises(ValueError):
                download.repositories(invalid)

    def test_corrected_repository_downloads_only_selected_gguf_shards(self):
        repo = "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF"
        names = [f"IQ1_M/Model-IQ1_M-{i:05d}-of-00002.gguf" for i in (1, 2)]
        contents = {name: b"GGUF" + name.encode() for name in names}
        entries = [SimpleNamespace(rfilename=name, size=len(raw),
                                  lfs={"sha256": hashlib.sha256(raw).hexdigest()}, blob_id=None)
                   for name, raw in contents.items()]
        calls = []
        class Api:
            def model_info(self, actual_repo, **_):
                self.assert_repo = actual_repo
                calls.append(actual_repo)
                return SimpleNamespace(sha="b" * 40, siblings=entries)
        def fetch(actual_repo, name, revision, local_dir, **_):
            self.assertEqual(actual_repo, repo)
            self.assertEqual(revision, "b" * 40)
            path = Path(local_dir) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents[name])
            return str(path)
        fake_hub = SimpleNamespace(HfApi=Api, hf_hub_download=fetch)
        with tempfile.TemporaryDirectory() as temp, patch.dict(sys.modules, {"huggingface_hub": fake_hub}), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(download.main(["qwen", "--repo", f"qwen={repo}", "--format", "gguf",
                                            "--quant", "IQ1_M", "--output", temp]), 0)
            self.assertEqual(calls, [repo])
            folder = Path(temp) / repo.replace("/", "--")
            receipt = download.json.loads((folder / ".chela-download.json").read_text())
            self.assertEqual(receipt["repository"], repo)
            self.assertEqual(receipt["requested_repository"], download.MODELS["qwen"])
            self.assertEqual({f["path"] for f in receipt["files"]}, set(names))

    def test_storage_failure_has_storage_advice_without_login_advice(self):
        entry = SimpleNamespace(rfilename="model.safetensors", size=48 * 1024 ** 3,
                                lfs={"sha256": "a" * 64}, blob_id=None)
        api = SimpleNamespace(model_info=lambda *_args, **_kwargs: SimpleNamespace(sha="b" * 40, siblings=[entry]))
        fake_hub = SimpleNamespace(HfApi=lambda: api, hf_hub_download=lambda *_args, **_kwargs: self.fail("Must not download"))
        error = io.StringIO()
        with tempfile.TemporaryDirectory() as temp, patch.dict(sys.modules, {"huggingface_hub": fake_hub}), \
             patch.object(download.shutil, "disk_usage", return_value=SimpleNamespace(free=48 * 1024 ** 3)), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(error):
            self.assertEqual(download.main(["gemma", "--output", temp]), 1)
        self.assertIn("Use --output on a larger filesystem", error.getvalue())
        self.assertNotIn("hf auth login", error.getvalue())

    def test_original_weights_and_supporting_files_only(self):
        names = ["model-00001-of-00002.safetensors", "model-00002-of-00002.safetensors",
                 "config.json", "tokenizer.json", "chat_template.jinja", "model.safetensors.index.json",
                 "README.md", "LICENSE", "model-Q4_K_M.gguf", "modeling_remote.py", "optimizer.safetensors"]
        chosen = download.select_files([File(n, 10) for n in names], "original")
        self.assertEqual({f.name for f in chosen}, set(names[:8]))
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            download.select_files([File(names[0], 10)], "original")

    def test_gguf_selects_one_quant_and_all_shards(self):
        names = ["Q8_0/Model-Q8_0-00001-of-00002.gguf", "Q8_0/Model-Q8_0-00002-of-00002.gguf",
                 "Model-Q4_K_M.gguf", "mtp-Model-Q8_0.gguf", "mmproj-Model-Q8_0.gguf", "Model-imatrix.gguf"]
        files = [File(n, 10) for n in names]
        self.assertEqual([f.name for f in download.select_files(files, "gguf", vision=False)], names[:2])
        self.assertEqual({f.name for f in download.select_files(files, "gguf")}, {*names[:2], names[4]})
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            download.select_files(files[:1], "gguf")
        with self.assertRaisesRegex(ValueError, "Multiple"):
            download.select_files(files + [File("Other-Q8_0.gguf", 10)], "gguf")
        with self.assertRaisesRegex(ValueError, "No main-model"):
            download.select_files(files, "gguf", quant="Q6_K")

    def test_gguf_vision_file_is_the_nearest_widely_supported_one(self):
        # The layout of the Qwen repository this project uses: quants in folders, one vision file at the top.
        weights = ["IQ1_M/Qwen-IQ1_M-00001-of-00002.gguf", "IQ1_M/Qwen-IQ1_M-00002-of-00002.gguf"]
        files = [File(n, 10) for n in weights + ["mmproj-Qwen-BF16.gguf", "IQ2_S/Qwen-IQ2_S.gguf"]]
        chosen = {f.name for f in download.select_files(files, "gguf", quant="IQ1_M")}
        self.assertEqual(chosen, {*weights, "mmproj-Qwen-BF16.gguf"})
        # Of several precisions F16 wins, and one beside the weights beats one at the top.
        names = ["Model-Q8_0.gguf", "mmproj-Model-F32.gguf", "mmproj-Model-BF16.gguf", "mmproj-Model-F16.gguf"]
        self.assertIn("mmproj-Model-F16.gguf", [f.name for f in download.select_files([File(n, 10) for n in names], "gguf")])
        names = ["Q8_0/Model-Q8_0.gguf", "mmproj-Model-F16.gguf", "Q8_0/mmproj.gguf"]
        chosen = [f.name for f in download.select_files([File(n, 10) for n in names], "gguf")]
        self.assertEqual(chosen, ["Q8_0/Model-Q8_0.gguf", "Q8_0/mmproj.gguf"])
        # Another quant's folder is not this model's, and a vision file is never mistaken for the weights.
        names = ["Q8_0/Model-Q8_0.gguf", "Q6_K/mmproj-Model-Q6_K.gguf"]
        self.assertEqual([f.name for f in download.select_files([File(n, 10) for n in names], "gguf")], names[:1])
        names = ["Model-Q8_0.gguf", "Model-Q8_0-mmproj.gguf"]
        self.assertEqual({f.name for f in download.select_files([File(n, 10) for n in names], "gguf", gguf_glob="*Q8_0*")}, set(names))

    def test_hashes_and_storage_resume(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            raw = b"GGUF" + b"weights" * 10
            file = File("Model-Q8_0.gguf", len(raw), hashlib.sha256(raw).hexdigest())
            path = folder / file.name
            path.write_bytes(raw)
            download.verify(path, file)
            self.assertEqual(download.bytes_needed(folder, [file]), 0)
            path.write_bytes(raw[:-1] + b"!")
            with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
                download.verify(path, file)
            path.unlink()
            cache = folder / ".cache/huggingface/download"
            cache.mkdir(parents=True)
            (cache / f"{file.name}.{file.sha256}.incomplete").write_bytes(raw[:20])
            self.assertEqual(download.bytes_needed(folder, [file]), len(raw) - 20)
            text = b"configuration"
            metadata = File("config.json", len(text), blob_id=hashlib.sha1(f"blob {len(text)}\0".encode() + text).hexdigest())
            (folder / metadata.name).write_bytes(text)
            download.verify(folder / metadata.name, metadata)

    def test_unsafe_paths_and_symlinks(self):
        for name in ("/tmp/model", "../model", "x/../model", "bad\nname", "x\\y"):
            with self.assertRaisesRegex(ValueError, "Unsafe"):
                download.safe_name(name)
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / "model"
            folder.mkdir()
            (folder / "redirect").symlink_to(Path(temp), target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "outside"):
                download.output_path(folder, File("redirect/file.gguf", 1))

    def test_one_unavailable_repo_does_not_block_other_downloads(self):
        raw = b"test checkpoint"
        calls = []
        entry = SimpleNamespace(rfilename="model.safetensors", size=len(raw),
                                lfs={"sha256": hashlib.sha256(raw).hexdigest()}, blob_id=None)
        class Api:
            def model_info(self, repo, **_):
                if repo == download.MODELS["qwen"]:
                    raise RuntimeError("401 repository access error")
                return SimpleNamespace(sha="a" * 40, siblings=[entry])
        def fetch(repo, name, revision, local_dir, **_):
            calls.append((repo, name, revision))
            path = Path(local_dir) / name
            path.write_bytes(raw)
            return str(path)
        fake_hub = SimpleNamespace(HfApi=Api, hf_hub_download=fetch)
        with tempfile.TemporaryDirectory() as temp, patch.dict(sys.modules, {"huggingface_hub": fake_hub}), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(download.main(["qwen", "gemma", "--output", temp]), 1)
            self.assertEqual(calls, [(download.MODELS["gemma"], "model.safetensors", "a" * 40)])
            folder = Path(temp) / download.MODELS["gemma"].replace("/", "--")
            self.assertTrue((folder / ".chela-download.json").exists())
            calls.clear()
            self.assertEqual(download.main(["gemma", "--output", temp, "--dry-run"]), 0)
            self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
