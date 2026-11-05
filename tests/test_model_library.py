"""Exercise the remote helper without a GPU, downloaded weights, or system services."""
import configparser
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("library", ROOT / "scripts/inferssd-library.py")
library = importlib.util.module_from_spec(spec)
spec.loader.exec_module(library)


class LibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="chela-library-")
        self.root = Path(self.temp.name)
        self.models = self.root / "models"
        self.models.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def gguf(self, name):
        path = self.models / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"GGUF" + bytes(20))
        return path

    def test_inventory_and_split_validation(self):
        first = self.gguf("qwen/Qwen-Q8_0.gguf")
        self.gguf("qwen/Qwen-Q6_K.gguf")  # Both quants in one directory must appear.
        self.gguf("split/Big-00001-of-00002.gguf")
        self.gguf("split/Big-00002-of-00002.gguf")
        self.gguf("partial/Incomplete-00001-of-00002.gguf")
        self.gguf("qwen/mmproj-f16.gguf")
        (self.models / "not-downloaded.gguf").write_text("version https://git-lfs.github.com/spec/v1")
        (self.models / "alias.gguf").symlink_to(first)
        (self.models / "loop").symlink_to(self.models, target_is_directory=True)
        found, skipped = library.discover([self.models, self.models])
        self.assertEqual(len(found), 3)
        self.assertEqual(sorted(len(m["parts"]) for m in found), [1, 1, 2])
        self.assertEqual(len(skipped), 4)  # Two skipped entries from each root.
        self.assertTrue(any(m["id"].startswith("Big-") for m in found))

    def test_safe_paths_and_stable_ids(self):
        original = self.gguf("special name #;[x]/model #;[x]-00001-of-00002.gguf")
        self.gguf("special name #;[x]/model #;[x]-00002-of-00002.gguf")
        data = self.root / "data"
        config = {"model_dir": str(self.models)}
        with patch.object(Path, "home", return_value=self.root):
            first, _ = library.prepare(config, data)
            second, _ = library.prepare(config, data)
        self.assertEqual(first[0]["id"], second[0]["id"])
        preset = (data / "models.ini").read_text()
        self.assertIn("load-on-startup = false", preset)
        self.assertIn("model-00001-of-00002.gguf", preset)
        self.assertNotIn("#;", preset)
        links = list((data / "models").rglob("*.gguf"))
        self.assertEqual(len(links), 2)
        self.assertTrue(all(p.is_symlink() and library.valid_gguf(p) for p in links))
        self.assertTrue(original.exists())

    def test_serve_uses_router_and_clears_single_model_environment(self):
        self.gguf("Qwen-Q8_0.gguf")
        config_dir, data = self.root / "config/chela", self.root / "data/chela/inferssd"
        config_dir.mkdir(parents=True)
        data.mkdir(parents=True)
        server = self.root / "fake-inferssd"
        server.write_text(f"#!{sys.executable}\nimport json,os,sys\nprint(json.dumps({{'args':sys.argv[1:], 'model':os.environ.get('LLAMA_ARG_MODEL'), 'cache':os.environ.get('LLAMA_CACHE')}}))\n")
        server.chmod(0o700)
        (config_dir / "inferssd-library.json").write_text(json.dumps({"model_dir": str(self.models), "server": str(server), "port": 8088}))
        env = {**os.environ, "HOME": str(self.root), "XDG_CONFIG_HOME": str(config_dir.parent), "XDG_DATA_HOME": str(self.root / "data"), "LLAMA_ARG_MODEL": "wrong-model", "LLAMA_ARG_HOST": "0.0.0.0"}
        result = subprocess.run([sys.executable, str(ROOT / "scripts/inferssd-library.py"), "--serve"], env=env, capture_output=True, text=True, check=True)
        output = json.loads(result.stdout)
        args = output["args"]
        self.assertNotIn("--model", args)
        self.assertEqual(args[args.index("--models-max") + 1], "1")
        self.assertEqual(args[args.index("--host") + 1], "127.0.0.1")
        self.assertIsNone(output["model"])
        self.assertEqual(output["cache"], str(data / "cache"))
        self.assertIn("Qwen-Q8_0", (data / "models.ini").read_text())

    def test_empty_library_and_conflicting_file(self):
        with patch.object(Path, "home", return_value=self.root):
            with self.assertRaisesRegex(RuntimeError, "No complete GGUF"):
                library.prepare({"model_dir": str(self.models)}, self.root / "data")
            self.gguf("Qwen.gguf")
            found, _ = library.discover([self.models])
            folder = self.root / "data/models" / found[0]["id"]
            folder.mkdir(parents=True)
            protected = folder / "model.gguf"
            protected.write_text("do not overwrite")
            with self.assertRaisesRegex(RuntimeError, "no model file was overwritten"):
                library.prepare({"model_dir": str(self.models)}, self.root / "data")
            self.assertEqual(protected.read_text(), "do not overwrite")

    def test_memory_modes_and_context(self):
        config = {"server": "/server", "port": 8088, "device": "Vulkan0",
                  "context_size": 4096, "gpu_headroom": 2048}
        args = library.command(config, self.root)
        self.assertEqual(args[args.index("--device") + 1], "Vulkan0")
        self.assertEqual(args[args.index("--gpu-layers") + 1], "auto")
        self.assertEqual(args[args.index("--ctx-size") + 1], "4096")
        self.assertNotIn("--cpu-moe", args)
        # The target differs by model (see vision below), and a command-line value would override each section's.
        self.assertNotIn("--fit-target", args)
        self.assertNotIn("--no-mmproj-offload", args)
        args = library.command({**config, "memory_mode": 1}, self.root)
        self.assertIn("--cpu-moe", args)
        args = library.command({**config, "memory_mode": 2}, self.root)
        self.assertEqual(args[args.index("--device") + 1], "none")
        self.assertEqual(args[args.index("--gpu-layers") + 1], "0")
        self.assertEqual(args[args.index("--fit") + 1], "off")
        self.assertNotIn("--cpu-moe", args)
        self.assertIn("--no-mmproj-offload", args)  # CPU only means the vision encoder too
        # Mode 3 places each model in its preset section; command-line placement would override it there.
        args = library.command({**config, "memory_mode": 3}, self.root)
        for flag in ("--gpu-layers", "--fit", "--fit-target", "--cpu-moe"):
            self.assertNotIn(flag, args)
        self.assertEqual(args[args.index("--device") + 1], "Vulkan0")
        for invalid in ({"context_size": 0}, {"memory_mode": 4}, {"gpu_headroom": -1}, {"device": "ROCm0\n--model"}):
            with self.assertRaisesRegex(RuntimeError, "Invalid memory settings"):
                library.command({**config, **invalid}, self.root)

    def gguf_with(self, name, architecture, before=(), tensors=()):
        """A GGUF v3 header with optional metadata ahead of general.architecture, and tensors (name, shape, type, offset)."""
        import struct
        def string(text):
            data = text.encode()
            return struct.pack("<Q", len(data)) + data
        entries = list(before) + [("general.architecture", 8, string(architecture))]
        body = struct.pack("<4sIQQ", b"GGUF", 3, len(tensors), len(entries))
        for key, kind, value in entries:
            body += string(key) + struct.pack("<I", kind) + value
        for tensor, shape, kind, offset in tensors:
            body += string(tensor) + struct.pack("<I", len(shape)) + struct.pack(f"<{len(shape)}Q", *shape)
            body += struct.pack("<IQ", kind, offset)
        path = self.models / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)
        return path

    def test_gguf_architecture(self):
        import struct
        array = struct.pack("<IQ", 8, 2) + struct.pack("<Q", 1) + b"a" + struct.pack("<Q", 2) + b"bc"
        plain = self.gguf_with("plain.gguf", "qwen4exp")
        later = self.gguf_with("later.gguf", "deepseek41", [("general.name", 8, struct.pack("<Q", 3) + b"abc"),
                                                            ("x.count", 4, struct.pack("<I", 7)), ("x.list", 9, array),
                                                            ("x.floats", 9, struct.pack("<IQ", 6, 3) + bytes(12))])
        self.assertEqual(library.gguf_architecture(plain), "qwen4exp")
        self.assertEqual(library.gguf_architecture(later), "deepseek41")
        self.assertEqual(library.gguf_architecture(self.gguf("bare.gguf")), "")
        (self.models / "text.gguf").write_text("not a model")
        self.assertEqual(library.gguf_architecture(self.models / "text.gguf"), "")
        self.assertEqual(library.gguf_architecture(self.models / "missing.gguf"), "")

    def test_gguf_experts(self):
        import struct
        # IQ2_S up, IQ4_NL and Q2_0 down, at an alignment of 64
        path = self.gguf_with("experts.gguf", "qwen4exp", [("general.alignment", 4, struct.pack("<I", 64))], [
            ("blk.0.ffn_up_exps.weight", (2560, 640, 256), 22, 0),
            ("blk.0.ffn_down_exps.weight", (640, 2560, 256), 20, 1000),
            ("blk.1.ffn_down_exps.weight", (640, 2560, 256), 42, 2000),
            ("blk.0.attn_q.weight", (2560, 2560), 20, 3000)])
        experts = library.gguf_experts(path)
        size = path.stat().st_size
        data = -(-size // 64) * 64
        self.assertEqual(experts, [("blk.0.ffn_up_exps.weight", 256, 524800, data),
                                   ("blk.0.ffn_down_exps.weight", 256, 921600, data + 1000),
                                   ("blk.1.ffn_down_exps.weight", 256, 460800, data + 2000)])
        unknown = self.gguf_with("unknown.gguf", "qwen4exp", (), [("blk.0.ffn_up_exps.weight", (2560, 640, 256), 99, 0)])
        with self.assertRaisesRegex(ValueError, "unsupported expert tensor"):
            library.gguf_experts(unknown)

    def test_expert_cache_mode_places_each_model(self):
        roles = [(f"blk.0.ffn_{role}_exps.weight", (256, 256, 2), 8, offset) for role, offset in (("up", 0), ("gate", 100000))]
        self.gguf_with("qwen/Flash-Next-IQ1_M-00001-of-00002.gguf", "qwen4exp", (), roles)
        self.gguf_with("qwen/Flash-Next-IQ1_M-00002-of-00002.gguf", "qwen4exp", (),
                       [("blk.0.ffn_down_exps.weight", (256, 256, 2), 8, 0)])
        self.gguf_with("gemma/Gemma-Q4_K.gguf", "gemma3")
        self.gguf_with("bare/Flash-Next-Dense.gguf", "qwen4exp")
        data = self.root / "data"
        config = {"model_dir": str(self.models), "memory_mode": 3, "gpu_headroom": 1536}
        with patch.object(Path, "home", return_value=self.root):
            models, _ = library.prepare(config, data)
        sections = {}
        for block in (data / "models.ini").read_text().split("\n\n"):
            lines = block.strip().splitlines()
            if lines:
                sections[lines[0]] = dict(line.split(" = ", 1) for line in lines[1:])
        flash = next(v for k, v in sections.items() if "IQ1_M" in k)
        gemma = next(v for k, v in sections.items() if "Gemma" in k)
        dense = next(v for k, v in sections.items() if "Dense" in k)
        sources = json.loads(flash["ssd-expert-source"])
        self.assertEqual(len(sources), 1)
        self.assertEqual(sources[0], str(Path(flash["model"]).parent / "experts.tsv"))
        rows = [line.split("\t") for line in Path(sources[0]).read_text().splitlines()]
        self.assertEqual(rows[0], ["tensor", "expert_idx", "nbytes", "source_path", "offset"])
        # two experts of each of three roles: 8 blocks of 34 bytes a row, 256 rows
        self.assertEqual(len(rows), 7)
        self.assertEqual({row[2] for row in rows[1:]}, {str(8 * 34 * 256)})
        self.assertTrue(all(Path(row[3]).is_symlink() for row in rows[1:]))
        up = [row for row in rows[1:] if "_up_" in row[0]]
        self.assertEqual(int(up[1][4]) - int(up[0][4]), 8 * 34 * 256)
        self.assertEqual({row[3] for row in rows[1:] if "_down_" in row[0]},
                         {str(Path(flash["model"]).with_name("model-00002-of-00002.gguf"))})
        self.assertEqual(flash["ssd-vram-budget"], "auto")
        self.assertEqual(flash["ssd-vram-reserve-mib"], "1536")
        self.assertEqual((flash["gpu-layers"], flash["fit"], flash["mmap"], flash["repack"]), ("99", "off", "true", "false"))
        self.assertEqual(flash["defer-experts"], "true")
        trace = json.loads(flash["ssd-trace-file"])
        self.assertEqual(len(trace), 1)
        self.assertTrue(trace[0].startswith(str(data / "traces")) and trace[0].endswith(".trace"))
        self.assertTrue((data / "traces").is_dir())
        self.assertNotIn("ssd-expert-source", gemma)
        self.assertEqual((gemma["gpu-layers"], gemma["fit"], gemma["fit-target"]), ("auto", "on", "1536"))
        # a supported architecture without routed experts has nothing for the caches to read
        self.assertNotIn("ssd-expert-source", dense)
        self.assertEqual((dense["gpu-layers"], dense["fit"]), ("auto", "on"))
        # the other modes keep their placement on the command line: sections hold only the model's own GPU reserve
        with patch.object(Path, "home", return_value=self.root):
            library.prepare({**config, "memory_mode": 0}, data)
        ini = (data / "models.ini").read_text()
        self.assertNotIn("ssd-", ini)
        self.assertEqual(set(re.findall(r"^fit.*$", ini, re.M)), {"fit-target = 1536"})
        with patch.object(Path, "home", return_value=self.root):
            library.prepare({**config, "memory_mode": 2}, data)
        self.assertNotIn("fit", (data / "models.ini").read_text())

    def gguf_meta(self, name, values, size=0):
        """A GGUF header holding the given metadata (strings, booleans, integers), padded to size bytes."""
        def text(value):
            raw = value.encode()
            return struct.pack("<Q", len(raw)) + raw
        body = b""
        for key, value in values.items():
            body += text(key)
            if isinstance(value, bool):
                body += struct.pack("<I?", 7, value)
            elif isinstance(value, int):
                body += struct.pack("<II", 4, value)
            else:
                body += struct.pack("<I", 8) + text(value)
        path = self.models / name
        path.parent.mkdir(parents=True, exist_ok=True)
        data = struct.pack("<4sIQQ", b"GGUF", 3, 0, len(values)) + body
        path.write_bytes(data.ljust(size, b"\0"))
        return path

    def text_model(self, name, embedding=2560, architecture="qwen4exp"):
        return self.gguf_meta(name, {"general.architecture": architecture, f"{architecture}.embedding_length": embedding})

    def vision_file(self, name, projection=2560, encoder=True, size=0):
        return self.gguf_meta(name, {"general.architecture": "clip", "clip.has_vision_encoder": encoder,
                                     "clip.vision.projection_dim": projection}, size)

    def projector_of(self, label):
        found, _ = library.discover([self.models])
        return next(m["projector"] for m in found if m["id"].startswith(label))

    def test_vision_file_beside_the_weights_or_one_folder_up(self):
        self.text_model("gemma/Gemma-Q.gguf", 2816, "gemma4")
        beside = self.vision_file("gemma/mmproj.gguf", 2816)
        # The published Qwen layout: each quant has a folder, with one vision file at the top of the repository.
        self.text_model("repo/IQ1_M/Qwen-IQ1_M-00001-of-00002.gguf")
        self.text_model("repo/IQ1_M/Qwen-IQ1_M-00002-of-00002.gguf")
        above = self.vision_file("repo/mmproj-Qwen-BF16.gguf")
        self.assertEqual(self.projector_of("Gemma-Q"), beside)
        self.assertEqual(self.projector_of("Qwen-IQ1_M"), above)
        found, _ = library.discover([self.models])
        self.assertEqual(len(found), 2, "a vision file is never a model of its own")

    def test_vision_file_that_cannot_work_is_ignored(self):
        """A model that loads without images beats one the server refuses to start."""
        self.text_model("a/Model.gguf", 2560)
        self.vision_file("a/mmproj-other.gguf", 4096)  # would stop the model loading
        self.vision_file("a/mmproj-audio.gguf", 2560, encoder=False)
        self.gguf("a/mmproj-truncated.gguf")
        self.assertIsNone(self.projector_of("Model"))
        # Nor does a search climb out of the model's own repository into the library's top level.
        self.text_model("b/Other.gguf", 2560)
        self.vision_file("mmproj-unrelated.gguf", 2560)
        self.assertIsNone(self.projector_of("Other"))

    def test_vision_file_choice_among_several(self):
        self.text_model("mix/Alpha-Q8.gguf")
        self.text_model("mix/Beta-Q8.gguf")
        alpha = self.vision_file("mix/mmproj-Alpha-Q8.gguf")
        beta = self.vision_file("mix/mmproj-Beta-Q8.gguf")
        self.assertEqual(self.projector_of("Alpha-Q8"), alpha)
        self.assertEqual(self.projector_of("Beta-Q8"), beta)
        self.text_model("prec/Gamma.gguf")
        self.vision_file("prec/mmproj-Gamma-F32.gguf")
        self.vision_file("prec/mmproj-Gamma-BF16.gguf")
        preferred = self.vision_file("prec/mmproj-Gamma-F16.gguf")
        self.assertEqual(self.projector_of("Gamma"), preferred)
        self.text_model("bare/Delta.gguf")
        self.vision_file("bare/mmproj-BF16.gguf")
        preferred = self.vision_file("bare/mmproj-F16.gguf")
        self.assertEqual(self.projector_of("Delta"), preferred)

    def test_preset_starts_a_model_with_its_vision_file_and_room_for_it(self):
        self.text_model("repo/IQ1_M/Qwen.gguf")
        self.text_model("plain/Text.gguf")
        vision = self.vision_file("repo/mmproj-Qwen.gguf", size=1024 * 1024 * 100)
        data = self.root / "data"
        config = {"model_dir": str(self.models), "gpu_headroom": 1024}

        def sections(mode):
            with patch.object(Path, "home", return_value=self.root):
                library.prepare({**config, "memory_mode": mode}, data)
            parsed = configparser.ConfigParser(interpolation=None, delimiters=("=",))
            parsed.read(data / "models.ini")
            return {name.split("-")[0]: dict(parsed[name]) for name in parsed.sections() if name != "*"}

        qwen, text = sections(0)["Qwen"], sections(0)["Text"]
        link = Path(qwen["mmproj"])
        self.assertTrue(link.is_symlink() and link.resolve() == vision.resolve())
        self.assertEqual(qwen["fit-target"], "1174")  # the headroom, and the encoder's 100 MiB and working memory
        self.assertEqual(text["fit-target"], "1024")
        self.assertNotIn("mmproj", text)
        self.assertEqual(sections(2)["Qwen"]["mmproj"], str(link))  # CPU only still reads images
        # The file going away takes its link and its reserve with it, so the model still starts.
        vision.unlink()
        qwen = sections(0)["Qwen"]
        self.assertNotIn("mmproj", qwen)
        self.assertEqual(qwen["fit-target"], "1024")
        self.assertFalse(link.is_symlink())

    def test_vision_link_never_replaces_a_real_file(self):
        self.text_model("a/Model.gguf")
        self.vision_file("a/mmproj.gguf")
        found, _ = library.discover([self.models])
        protected = self.root / "data/models" / found[0]["id"] / "mmproj.gguf"
        protected.parent.mkdir(parents=True)
        protected.write_text("do not overwrite")
        with patch.object(Path, "home", return_value=self.root), self.assertRaisesRegex(RuntimeError, "no model file was overwritten"):
            library.prepare({"model_dir": str(self.models)}, self.root / "data")
        self.assertEqual(protected.read_text(), "do not overwrite")

    def test_expert_cache_memory_limit(self):
        meminfo = self.root / "meminfo"
        meminfo.write_text("MemTotal:       65536000 kB\nMemFree:        1000 kB\n")
        limits = library.memory_limits(meminfo)
        total = 65536000 * 1024
        self.assertIn(f"MemoryMax={total - total // 16}\n", limits)
        self.assertIn("MemorySwapMax=0\n", limits)
        meminfo.write_text("MemTotal:       4096000 kB\n")
        with self.assertRaisesRegex(RuntimeError, "more than 6 GiB"):
            library.memory_limits(meminfo)
        meminfo.write_text("nothing here\n")
        with self.assertRaisesRegex(RuntimeError, "MemTotal"):
            library.memory_limits(meminfo)

    def test_gpu_detection_and_missing_gpu(self):
        result = subprocess.CompletedProcess([], 0, "Available devices:\n  ROCm0: AMD Radeon RX 6800 (16368 MiB, 15000 MiB free)\n  Vulkan0: AMD GPU (16368 MiB)\n", "")
        with patch.object(library.subprocess, "run", return_value=result):
            self.assertEqual(library.gpu_devices("/server")[0], {
                "id": "ROCm0", "name": "AMD Radeon RX 6800",
                "description": "AMD Radeon RX 6800 (16368 MiB, 15000 MiB free)"})
            self.assertEqual(library.gpu_device("/server", "auto"), "ROCm0")
            self.assertEqual(library.gpu_device("/server", "Vulkan0"), "Vulkan0")
            with self.assertRaisesRegex(RuntimeError, "GPU.*unavailable"):
                library.gpu_device("/server", "CUDA0")
        with patch.object(library.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "Available devices:\n", "")):
            with self.assertRaisesRegex(RuntimeError, "CPU / RAM only"):
                library.gpu_device("/server", "auto")

    def device_config(self, mode=0):
        return {"device": "ROCm0", "memory_mode": mode, "server": "/server",
                "devices": [{"id": "ROCm0", "name": "AMD Radeon RX 6800"}]}

    def selected_log(self, layers="24/33", model="gemma-test", port=41000):
        return (f"srv load: spawning server instance with name={model} on port {port}\n"
                f"[{port}] llama_model_load: using device ROCm0 (AMD Radeon RX 6800) - 15000 MiB free\n"
                f"[{port}] load_tensors: offloaded {layers} layers to GPU\n"
                f"[{port}] load_tensors: ROCm0 model buffer size = 15000.00 MiB\n"
                f"[{port}] load_tensors: CPU_Mapped model buffer size = 5000.00 MiB\n")

    def test_device_status_distinguishes_configuration_from_active_placement(self):
        config = self.device_config()
        # Merely detecting/configuring a GPU must not claim this model uses it.
        status = library.model_device_status(config, "gemma-test", "loaded", {"ActiveState": "active"}, "", "AMD Ryzen 9")
        self.assertFalse(status["confirmed"])
        self.assertIn("Configured: ROCm0 — AMD Radeon RX 6800", status["summary"])
        self.assertIn("unverified", status["summary"])
        status = library.model_device_status(config, "gemma-test", "loaded", {"ActiveState": "active"}, self.selected_log(), "AMD Ryzen 9")
        self.assertTrue(status["confirmed"])
        self.assertEqual(status["backend"], "GPU + CPU")
        self.assertEqual(status["cpu_name"], "AMD Ryzen 9")
        self.assertEqual(status["actual_devices"], [{"id": "ROCm0", "name": "AMD Radeon RX 6800"}])
        self.assertIn("24/33 layers", status["summary"])
        # Newer builds append the PCI address in its own parentheses.
        logs = self.selected_log().replace("(AMD Radeon RX 6800)", "(AMD Radeon RX 6800) (0000:0a:00.0)")
        status = library.model_device_status(config, "gemma-test", "loaded", {"ActiveState": "active"}, logs, "AMD Ryzen 9")
        self.assertEqual(status["actual_devices"], [{"id": "ROCm0", "name": "AMD Radeon RX 6800"}])

    def test_device_evidence_is_scoped_to_selected_model_current_child(self):
        config, active = self.device_config(), {"ActiveState": "active"}
        foreign = self.selected_log(model="other-model")
        status = library.model_device_status(config, "gemma-test", "loaded", active, foreign, "CPU")
        self.assertFalse(status["confirmed"])
        # Other child ports cannot replace the selected model's layer count.
        logs = self.selected_log() + self.selected_log("33/33", model="other-model", port=42000)
        status = library.model_device_status(config, "gemma-test", "loaded", active, logs, "CPU")
        self.assertTrue(status["confirmed"])
        self.assertEqual(status["offloaded_layers"], 24)
        # A reload invalidates the previous child even if the old load succeeded.
        logs += "srv load: spawning server instance with name=gemma-test on port 43000\n"
        status = library.model_device_status(config, "gemma-test", "loaded", active, logs, "CPU")
        self.assertFalse(status["confirmed"])

    def test_failed_stopped_and_loading_models_do_not_claim_active_gpu(self):
        config, logs = self.device_config(), self.selected_log()
        for state, active in (("unloaded", "active"), ("failed", "active"), ("loaded", "failed")):
            with self.subTest(state=state, active=active):
                status = library.model_device_status(config, "gemma-test", state, {"ActiveState": active}, logs, "CPU")
                self.assertFalse(status["confirmed"])
        status = library.model_device_status(config, "gemma-test", "loading", {"ActiveState": "active"}, logs, "CPU")
        self.assertFalse(status["confirmed"])
        self.assertIn("load not complete", status["summary"])
        status = library.model_device_status(config, "gemma-test", "loaded", {"ActiveState": "active"},
                                             logs + "srv instance name=gemma-test exited with status 1\n", "CPU")
        self.assertFalse(status["confirmed"])

    def test_cpu_and_expert_memory_placement(self):
        active = {"ActiveState": "active"}
        status = library.model_device_status(self.device_config(2), "gemma-test", "loaded", active,
                                             self.selected_log("0/33"), "AMD Ryzen 7 5800X")
        self.assertTrue(status["confirmed"])
        self.assertEqual(status["backend"], "CPU")
        self.assertIn("AMD Ryzen 7 5800X", status["summary"])
        status = library.model_device_status(self.device_config(1), "gemma-test", "loaded", active,
                                             self.selected_log("33/33"), "CPU")
        self.assertEqual(status["backend"], "GPU + CPU")
        self.assertIn("experts in CPU RAM", status["summary"])
        status = library.model_device_status(self.device_config(), "gemma-test", "loaded", active,
                                             self.selected_log("33/33"), "CPU")
        self.assertEqual(status["backend"], "GPU + CPU")  # CPU weight buffers count too.

    def test_memory_counts_final_allocation_by_device(self):
        port = 41000
        logs = self.selected_log("24/33") + "".join(f"[{port}] {line}\n" for line in (
            "llama_kv_cache:      ROCm0 KV buffer size =   384.00 MiB",
            "llama_kv_cache:      ROCm0 KV buffer size =   128.00 MiB",  # Sliding-window cache.
            "llama_kv_cache:        CPU KV buffer size =    64.00 MiB",
            "llama_context:      ROCm0 compute buffer size =   300.50 MiB",
            "llama_context:  ROCm_Host compute buffer size =    20.00 MiB",
            "llama_context:  ROCm_Host  output buffer size =     0.50 MiB"))
        # A trial allocation before the final load is not memory in use.
        trial = (f"srv load: spawning server instance with name=gemma-test on port {port}\n"
                 f"[{port}] load_tensors: offloaded 33/33 layers to GPU\n"
                 f"[{port}] load_tensors: ROCm0 model buffer size = 99999.00 MiB\n")
        logs = trial + logs.split("\n", 1)[1]
        status = library.model_device_status(self.device_config(), "gemma-test", "loaded",
                                             {"ActiveState": "active"}, logs, "CPU")
        memory = status["memory"]
        self.assertEqual(memory["devices"]["ROCm0"], {"weights": 15000.0, "context": 512.0, "compute": 300.5})
        self.assertEqual(memory["devices"]["CPU"], {"weights": 0.0, "context": 64.0, "compute": 0.0})
        self.assertEqual(memory["gpu_mib"], 15812.5)
        self.assertEqual(memory["ram_mib"], 5084.5)
        self.assertEqual(status["port"], port)
        status = library.model_device_status(self.device_config(), "gemma-test", "loaded",
                                             {"ActiveState": "active"}, logs.replace("buffer size", "size"), "CPU")
        self.assertNotIn("memory", status)

    def test_live_gpu_and_process_readings(self):
        card = self.root / "drm/card1/device"
        card.mkdir(parents=True)
        (card / "mem_info_vram_total").write_text(str(16368 * 2**20))
        (card / "mem_info_vram_used").write_text(str(14000 * 2**20))
        (card / "gpu_busy_percent").write_text("87\n")
        (self.root / "drm/card0-HDMI-A-1").mkdir()
        self.assertEqual(library.gpu_usage(self.root / "drm", self.root / "missing"),
                         [{"card": "card1", "vram_used_mib": 14000, "vram_total_mib": 16368, "busy_percent": 87}])
        proc = self.root / "proc"
        for pid, args, rss in (("12", ["/srv/inferssd-server", "--port", "41000"], 6000),
                               ("13", ["/srv/inferssd-server", "--port", "8088"], 300),
                               ("14", ["/bin/other", "--port", "41000"], 1)):
            (proc / pid).mkdir(parents=True)
            (proc / pid / "cmdline").write_bytes(b"\0".join(a.encode() for a in args) + b"\0")
            (proc / pid / "status").write_text(f"Name:\tx\nVmRSS:\t {rss * 1024} kB\n")
        (proc / "self").mkdir()
        self.assertEqual(library.process_memory("/srv/inferssd-server", 41000, proc), {"pid": 12, "rss_mib": 6000})
        self.assertIsNone(library.process_memory("/srv/inferssd-server", 9, proc))

    def test_load_stage_follows_the_engines_log_order(self):
        stages = [("", "starting"), ("build: 1\n", "starting"),
                  ("load_tensors: loading model tensors, this can take a while... (mmap = true)\n", "reading"),
                  ("load_tensors: offloaded 24/25 layers to GPU\n", "reading"),
                  ("load_tensors:        ROCm0 model buffer size = 15000.00 MiB\n", "uploading"),
                  ("llama_context: constructing llama_context\n", "context"),
                  ("common_init_from_params: warming up the model with an empty run\n", "warming")]
        log = ""
        for line, stage in stages:
            log += line
            self.assertEqual(library.load_stage(log), stage, log)

    def test_child_output_is_this_models_latest_process_without_its_port_tag(self):
        logs = ("srv spawning server instance with name=a on port 41000\n[41000] old load\n"
                "srv instance name=a exited with status 0\n"
                "srv spawning server instance with name=b on port 41001\n[41001] other model\n"
                "srv spawning server instance with name=a on port 41002\n"
                "[41002] first\nnot tagged\n[41001] other again\n[41002]   indented\n")
        self.assertEqual(library.child_output(logs, "a"), (41002, "first\n  indented"))
        self.assertEqual(library.child_output(logs, "missing"), (None, ""))
        self.assertEqual(library.child_output(logs + "instance name=a exited with status 1\n", "a"), (None, ""))

    def test_upload_percent_counts_dots_the_log_has_not_received(self):
        child = "line one\nline two é\n"
        logged = len(child.encode())
        self.assertEqual(library.upload_percent(child, f"rchar: 9\nwchar: {logged + 42}\n"), 42)
        self.assertEqual(library.upload_percent(child, f"wchar: {logged}\n"), 0)
        self.assertIsNone(library.upload_percent(child, f"wchar: {logged - 1}\n"))  # another writer: do not guess
        self.assertIsNone(library.upload_percent(child, f"wchar: {logged + 500}\n"))
        self.assertIsNone(library.upload_percent(child, "no counters here"))

    def test_load_progress_reports_bytes_and_skips_lazily_mapped_shards(self):
        folder = self.root / "data/models/qwen"
        folder.mkdir(parents=True)
        def shard(name, tensors, padding):
            header = struct.pack("<4sIQQ", b"GGUF", 3, len(tensors), 0)
            for tensor in tensors:
                encoded = tensor.encode()
                header += struct.pack("<Q", len(encoded)) + encoded + struct.pack("<IQIQ", 1, 8, 0, 0)
            (folder / name).write_bytes(header + bytes(padding))
        shard("model-00001-of-00002.gguf", ["blk.0.ffn_down.weight", "blk.0.attn.weight"], 3 * 2**20)
        shard("model-00002-of-00002.gguf", ["blk.0.per_layer_token_embd.weight", "blk.1.per_layer_token_embd.weight"], 5 * 2**20)
        proc = self.root / "proc"
        (proc / "77").mkdir(parents=True)
        child = "load_tensors: loading model tensors, this can take a while...\n"
        process = {"pid": 77, "rss_mib": 2100, "file_mib": 1800, "anon_mib": 300}
        report = library.load_progress(folder, child, process, proc)
        self.assertEqual(report["phase"], "reading")
        self.assertEqual(report["file_mib"], 8)
        self.assertEqual(report["target_mib"], 3)  # the token tables are mapped, never read
        self.assertEqual(report["mapped_mib"], 1800)
        self.assertNotIn("upload_percent", report)
        child += ("load_tensors:        ROCm0 model buffer size = 15000.00 MiB\n"
                  "load_tensors:   CPU_Mapped model buffer size =  5000.00 MiB\n")
        (proc / "77/io").write_text(f"wchar: {len(child.encode()) + 30}\n")
        report = library.load_progress(folder, child, process, proc)
        self.assertEqual(report["phase"], "uploading")
        self.assertEqual(report["gpu_mib"], 15000)
        self.assertEqual(report["upload_percent"], 30)
        # A model in one file, an unreadable process and an empty folder all still report.
        (folder / "model-00002-of-00002.gguf").unlink()
        report = library.load_progress(folder, child, {"pid": 99}, proc)
        self.assertEqual(report["target_mib"], report["file_mib"])
        self.assertNotIn("upload_percent", report)
        self.assertEqual(library.load_progress(self.root / "gone", "", None, proc),
                         {"phase": "starting", "file_mib": 0, "target_mib": 0, "gpu_mib": 0})

    def test_live_status_reads_only_current_invocation_without_loading_model(self):
        config_dir = self.root / "config/chela"
        config_dir.mkdir(parents=True)
        (config_dir / "inferssd-library.json").write_text(json.dumps({**self.device_config(), "port": 8088}))
        invocation = "a" * 32
        service = subprocess.CompletedProcess([], 0, "ActiveState=active\nInvocationID=" + invocation + "\n", "")
        journal = subprocess.CompletedProcess([], 0, self.selected_log(), "")
        class Opener:
            urls = []
            def open(self, url, timeout):
                self.urls.append(url)
                if url.endswith("/slots"):
                    return io.StringIO(json.dumps([{"id": 0, "is_processing": True, "next_token": [{"n_decoded": 12}]}]))
                return io.StringIO(json.dumps({"data": [{"id": "gemma-test", "status": {"value": "loaded"}}]}))
        opener = Opener()
        with patch.object(library, "locations", return_value=(config_dir, self.root)), \
             patch.object(library, "cpu_name", return_value="AMD Ryzen 7"), \
             patch.object(library.subprocess, "run", side_effect=[service, journal]) as run, \
             patch.object(library, "gpu_usage", return_value=[{"card": "card1", "vram_used_mib": 15800}]), \
             patch.object(library, "process_memory", return_value={"pid": 7, "rss_mib": 5200}) as process, \
             patch.object(library.urllib.request, "build_opener", return_value=opener):
            status = library.device_status("gemma-test")
        self.assertTrue(status["confirmed"])
        # The router's inventory, then the selected model's own server for slot activity.
        self.assertEqual(opener.urls, ["http://127.0.0.1:8088/v1/models", "http://127.0.0.1:41000/slots"])
        journal_args = run.call_args_list[1].args[0]
        self.assertIn("_SYSTEMD_INVOCATION_ID=" + invocation, journal_args)
        self.assertEqual(run.call_count, 2)
        process.assert_called_once_with("/server", 41000)
        self.assertEqual(status["live"], {"gpus": [{"card": "card1", "vram_used_mib": 15800}],
                                          "process": {"pid": 7, "rss_mib": 5200},
                                          "slot": {"available": True, "processing": True, "decoded": 12}})

    def test_slot_activity_separates_withheld_tokens_from_a_stuck_step(self):
        class Opener:
            def __init__(self, reply):
                self.reply = reply
            def open(self, url, timeout):
                if isinstance(self.reply, Exception):
                    raise self.reply
                return io.StringIO(json.dumps(self.reply))
        cases = (
            ([{"is_processing": True, "next_token": {"n_decoded": 300}}], {"available": True, "processing": True, "decoded": 300}),
            ([{"is_processing": False}], {"available": True, "processing": False}),
            ([{"is_processing": True}], {"available": True, "processing": True}),
            ([{"is_processing": True, "next_token": {"n_decoded": "300"}}], {"available": True, "processing": True}),
            ({"error": "not a list"}, {"available": True, "processing": False}),
        )
        for reply, expected in cases:
            with self.subTest(reply=reply):
                self.assertEqual(library.slot_activity(41000, Opener(reply)), expected)
        # The server answers /slots between steps; no answer means one step is still running.
        stuck = library.slot_activity(41000, Opener(TimeoutError("timed out")))
        self.assertEqual((stuck["available"], stuck["timed_out"]), (False, True))
        disabled = library.slot_activity(41000, Opener(OSError("HTTP Error 501: Not Implemented")))
        self.assertEqual((disabled["available"], disabled["timed_out"]), (False, False))

    def test_missing_current_invocation_never_uses_old_gpu_log(self):
        config_dir = self.root / "config/chela"
        config_dir.mkdir(parents=True)
        (config_dir / "inferssd-library.json").write_text(json.dumps({**self.device_config(), "port": 8088}))
        service = subprocess.CompletedProcess([], 0, "ActiveState=active\nInvocationID=\n", "")
        with patch.object(library, "locations", return_value=(config_dir, self.root)), \
             patch.object(library, "cpu_name", return_value="CPU"), \
             patch.object(library.subprocess, "run", return_value=service) as run, \
             patch.object(library, "gpu_usage", return_value=[]), \
             patch.object(library, "process_memory") as process, \
             patch.object(library.urllib.request, "build_opener", side_effect=OSError("unreachable")):
            status = library.device_status("gemma-test")
        self.assertFalse(status["confirmed"])
        self.assertEqual(status["api_error"], "unreachable")
        self.assertEqual(run.call_count, 1)
        process.assert_not_called()

    def test_install_creates_private_user_service_and_returns_inventory(self):
        self.gguf("Qwen-Q8.gguf")
        binary = self.root / "inferssd-server"
        binary.write_text("fixture")
        binary.chmod(0o700)
        args = type("Args", (), {"server": str(binary), "model_dir": str(self.models), "port": 8088,
                                "memory_mode": 0, "context_size": 8192, "gpu_headroom": 1024, "device": "auto"})()
        help_result = subprocess.CompletedProcess([], 0,
            "--models-preset --models-max --models-autoload --fit-target --fit --device --gpu-layers "
            "--ctx-size --batch-size --ubatch-size --parallel --jinja --host --port --list-devices --reasoning-budget", "")
        device_result = subprocess.CompletedProcess([], 0, "Available devices:\n  ROCm0: AMD Radeon RX 6800 (16368 MiB)\n", "")
        inventory = {"data": [{"id": "fixture-qwen", "status": {"value": "unloaded"}}]}
        class Opener:
            def open(self, url, timeout):
                self.url = url
                return io.StringIO(json.dumps(inventory))
        opener = Opener()
        def manager(*action):
            return subprocess.CompletedProcess([], 3 if action[0] == "is-active" else 0, "", "")
        env = {"XDG_CONFIG_HOME": str(self.root / "config"), "XDG_DATA_HOME": str(self.root / "data")}
        with patch.dict(os.environ, env), patch.object(Path, "home", return_value=self.root), \
             patch.object(library.subprocess, "run", side_effect=[help_result, device_result]), \
             patch.object(library, "systemctl", side_effect=manager) as ctl, \
             patch.object(library.socket, "socket"), \
             patch.object(library.urllib.request, "build_opener", return_value=opener), \
             patch("sys.stdout", new_callable=io.StringIO) as output:
            library.install(args)
        self.assertEqual(json.loads(output.getvalue())["data"], inventory["data"])
        self.assertEqual(opener.url, "http://127.0.0.1:8088/v1/models")
        unit = self.root / "config/systemd/user/chela-inferssd.service"
        self.assertIn("--serve", unit.read_text())
        self.assertIn("KillMode=control-group", unit.read_text())
        self.assertEqual(unit.stat().st_mode & 0o777, 0o600)
        config = self.root / "config/chela/inferssd-library.json"
        self.assertEqual(json.loads(config.read_text())["server"], str(binary))
        self.assertEqual(json.loads(config.read_text())["device"], "ROCm0")
        self.assertEqual(json.loads(config.read_text())["devices"][0]["name"], "AMD Radeon RX 6800")
        self.assertTrue((self.root / "data/chela/inferssd/inferssd-library.py").is_file())
        self.assertIn(unittest.mock.call("restart", library.SERVICE), ctl.call_args_list)


if __name__ == "__main__":
    unittest.main()
