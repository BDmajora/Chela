"""Exercise the remote helper without a GPU, downloaded weights, or system services."""
import importlib.util
import io
import json
import os
from pathlib import Path
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
        self.assertEqual(args[args.index("--fit-target") + 1], "2048")
        self.assertEqual(args[args.index("--ctx-size") + 1], "4096")
        self.assertNotIn("--cpu-moe", args)
        args = library.command({**config, "memory_mode": 1}, self.root)
        self.assertIn("--cpu-moe", args)
        args = library.command({**config, "memory_mode": 2}, self.root)
        self.assertEqual(args[args.index("--device") + 1], "none")
        self.assertEqual(args[args.index("--gpu-layers") + 1], "0")
        self.assertEqual(args[args.index("--fit") + 1], "off")
        self.assertNotIn("--cpu-moe", args)
        for invalid in ({"context_size": 0}, {"memory_mode": 3}, {"gpu_headroom": -1}, {"device": "ROCm0\n--model"}):
            with self.assertRaisesRegex(RuntimeError, "Invalid memory settings"):
                library.command({**config, **invalid}, self.root)

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
