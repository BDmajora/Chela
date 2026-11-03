"""Set up InferSSD's native model router on the inference computer (stdlib only).

Chela sends this helper over SSH. No models are downloaded or copied. Each router
start rescans the configured folder and common model caches, using symlinks to
give split GGUFs safe, stable paths. Only one model is resident at a time.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

SERVICE = "chela-inferssd.service"
SPLIT = re.compile(r"^(.*)-(\d{5})-of-(\d{5})\.gguf$", re.I)


def locations():
    config = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "chela"
    data = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "chela/inferssd"
    return config, data


def atomic_write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temp = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(text)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def valid_gguf(path):
    try:
        with path.open("rb") as stream:
            return stream.read(4) == b"GGUF"
    except OSError:
        return False


def discover(roots):
    models, seen, skipped = [], set(), []
    for root in roots:
        root = Path(root).expanduser()
        if not root.is_dir():
            continue
        # Do not follow directory symlinks (cycles); HF snapshot file symlinks work.
        for folder, dirs, files in os.walk(root):
            dirs[:] = sorted(d for d in dirs if not d.startswith("."))
            for filename in sorted(files):
                if not filename.lower().endswith(".gguf") or "mmproj" in filename.lower():
                    continue
                path = Path(folder) / filename
                real = path.resolve()
                if real in seen:
                    continue
                split = SPLIT.match(filename)
                if split and int(split[2]) != 1:
                    continue
                parts = [path]
                label = path.stem
                if split:
                    count = int(split[3])
                    if not 1 <= count <= 99999:
                        continue
                    label = split[1]
                    parts = [path.with_name(f"{label}-{i:05d}-of-{count:05d}.gguf") for i in range(1, count + 1)]
                if not all(valid_gguf(p) for p in parts):
                    skipped.append(f"{filename}: incomplete or not a GGUF")
                    continue
                seen.add(real)
                digest = hashlib.sha256(str(real).encode()).hexdigest()[:8]
                safe = re.sub(r"[^a-zA-Z0-9_.-]+", "-", label).strip("-.")[:200] or "model"
                models.append({"id": f"{safe}-{digest}", "parts": parts})
    return sorted(models, key=lambda m: m["id"].lower()), skipped


def prepare(config, data):
    roots = [config["model_dir"], "~/.local/share/inferssd/models", "~/.cache/llama.cpp", "~/.cache/huggingface/hub"]
    models, skipped = discover(roots)
    if not models:
        raise RuntimeError(f"No complete GGUF models found in {config['model_dir']} or the model caches. Download a GGUF first, or change Model folder.")
    if any(c in str(data) for c in "\r\n#;"):
        raise RuntimeError("The home/data folder contains characters unsupported by InferSSD presets.")
    preset = ["[*]", "load-on-startup = false", ""]
    for model in models:
        folder = data / "models" / model["id"]
        folder.mkdir(parents=True, exist_ok=True, mode=0o700)
        for i, part in enumerate(model["parts"], 1):
            name = "model.gguf" if len(model["parts"]) == 1 else f"model-{i:05d}-of-{len(model['parts']):05d}.gguf"
            link = folder / name
            if link.is_symlink():
                link.unlink()
            elif link.exists():
                raise RuntimeError(f"Expected a managed symlink at {link}; no model file was overwritten.")
            link.symlink_to(part.absolute())
            if i == 1:
                preset += [f"[{model['id']}]", f"model = {link}", ""]
    atomic_write(data / "models.ini", "\n".join(preset))
    (data / "cache").mkdir(exist_ok=True, mode=0o700)
    return models, skipped


def command(config, data):
    mode = config.get("memory_mode", 0)
    context = config.get("context_size", 8192)
    headroom = config.get("gpu_headroom", 1024)
    device = config.get("device", "ROCm0")  # Preserve older installed configurations.
    if (type(mode) is not int or mode not in (0, 1, 2) or type(context) is not int or
            not 512 <= context <= 131072 or type(headroom) is not int or not 256 <= headroom <= 65536 or
            not isinstance(device, str) or not re.fullmatch(r"[A-Za-z0-9_,-]+", device)):
        raise RuntimeError("Invalid memory settings. Reconnect using Model library setup.")
    args = [config["server"], "--models-preset", str(data / "models.ini"),
            "--models-max", "1", "--models-autoload", "--device", "none" if mode == 2 else device,
            "--gpu-layers", "0" if mode == 2 else "auto", "--fit", "off" if mode == 2 else "on",
            "--fit-target", str(headroom), "--ctx-size", str(context),
            "--batch-size", "512", "--ubatch-size", "128",
            "--parallel", "1", "--jinja", "--host", "127.0.0.1", "--port", str(config["port"])]
    if mode == 1:
        args.append("--cpu-moe")
    return args


def gpu_devices(binary):
    result = subprocess.run([str(binary), "--list-devices"], capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError("Could not query the server's GPUs: " + (result.stderr + result.stdout)[-2000:])
    devices = []
    for identifier, description in re.findall(r"^\s+([A-Za-z0-9_-]+):\s*(.+)$", result.stdout, re.M):
        name = re.sub(r"\s*\(\d+ MiB(?:,.*)?\)\s*$", "", description).strip()
        devices.append({"id": identifier, "name": name, "description": description.strip()})
    return devices


def gpu_device(binary, requested, devices=None):
    devices = gpu_devices(binary) if devices is None else devices
    identifiers = [device["id"] for device in devices]
    if requested == "auto" and identifiers:
        return identifiers[0]
    if requested != "auto" and all(d in identifiers for d in requested.split(",")):
        return requested
    available = ", ".join(identifiers) or "none"
    raise RuntimeError(f"GPU {requested!r} is unavailable (server devices: {available}). "
                       "Check the ROCm build and GPU access, or choose CPU / RAM only in Advanced.")


def cpu_name():
    try:
        text = Path("/proc/cpuinfo").read_text()
        for key in ("model name", "Hardware"):
            match = re.search(r"^" + key + r"\s*:\s*(.+)$", text, re.M)
            if match:
                return match[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine() or "Unknown CPU"


HOST_BUFFERS = re.compile(r"^(CPU|CPU_Mapped|CPU_REPACK|.*_Host)$", re.I)


def allocation(child):
    """Sum the selected child's final allocation, in MiB, per device and purpose.

    Buffer summaries follow the last "offloaded N/M layers" line of a real load.
    CPU, mapped-file and pinned host buffers count as system RAM.
    """
    layers = list(re.finditer(r"offloaded\s+\d+/\d+\s+layers to GPU", child))
    if not layers:
        return None
    devices, gpu, ram = {}, 0.0, 0.0
    for identifier, purpose, size in re.findall(
            r"\b([A-Za-z0-9_-]+)\s+(model|KV|compute|output|RS) buffer size\s*=\s*([0-9.]+)\s*MiB",
            child[layers[-1].end():]):
        purpose = {"model": "weights", "KV": "context", "RS": "context"}.get(purpose, "compute")
        entry = devices.setdefault(identifier, {"weights": 0.0, "context": 0.0, "compute": 0.0})
        entry[purpose] = round(entry[purpose] + float(size), 2)
        if HOST_BUFFERS.match(identifier):
            ram += float(size)
        else:
            gpu += float(size)
    if not devices:
        return None
    return {"gpu_mib": round(gpu, 1), "ram_mib": round(ram, 1), "devices": devices}


def gpu_usage(drm=Path("/sys/class/drm"), nvidia=Path("/proc/driver/nvidia")):
    """Read whole-device VRAM and load without starting a GPU tool (AMD sysfs, then nvidia-smi)."""
    gpus = []
    for card in sorted(drm.glob("card[0-9]*")):
        device = card / "device"
        try:
            total = int((device / "mem_info_vram_total").read_text())
            used = int((device / "mem_info_vram_used").read_text())
        except (OSError, ValueError):
            continue
        entry = {"card": card.name, "vram_used_mib": round(used / 2**20), "vram_total_mib": round(total / 2**20)}
        try:
            entry["busy_percent"] = int((device / "gpu_busy_percent").read_text())
        except (OSError, ValueError):
            pass
        gpus.append(entry)
    if not gpus and nvidia.exists():
        try:
            result = subprocess.run(["nvidia-smi", "--query-gpu=name,memory.used,memory.total,utilization.gpu",
                                     "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=5)
            for line in result.stdout.splitlines() if result.returncode == 0 else []:
                name, used, total, busy = [part.strip() for part in line.split(",")]
                gpus.append({"name": name, "vram_used_mib": int(used), "vram_total_mib": int(total),
                             "busy_percent": int(busy)})
        except (OSError, ValueError, subprocess.TimeoutExpired):
            pass
    return gpus


def process_memory(server, port, proc=Path("/proc")):
    """Resident RAM of the model's own server process, found by its router-assigned port."""
    wanted = ["--port", str(port)]
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            args = (entry / "cmdline").read_bytes().split(b"\0")
            args = [arg.decode(errors="replace") for arg in args]
            if Path(args[0]).name != Path(server).name or not any(args[i:i + 2] == wanted for i in range(len(args))):
                continue
            match = re.search(r"^VmRSS:\s+(\d+)\s+kB", (entry / "status").read_text(), re.M)
            if match:
                return {"pid": int(entry.name), "rss_mib": round(int(match[1]) / 1024)}
        except (OSError, IndexError):
            continue
    return None


def slot_activity(port, opener):
    """What the model's own server is doing right now, from its /slots monitor.

    The server answers /slots between model steps, so a slot that is generating
    tokens the stream has not delivered is distinguishable from one stuck inside
    a single long step, which leaves the request unanswered.
    """
    try:
        with opener.open(f"http://127.0.0.1:{port}/slots", timeout=2) as response:
            slots = json.load(response)
    except (OSError, ValueError) as error:
        timed_out = isinstance(error, TimeoutError) or "timed out" in str(error)
        return {"available": False, "timed_out": timed_out, "error": str(error)[:200]}
    for slot in slots if isinstance(slots, list) else []:
        if isinstance(slot, dict) and slot.get("is_processing"):
            progress = slot.get("next_token")
            if isinstance(progress, list):
                progress = progress[0] if progress else {}
            decoded = progress.get("n_decoded") if isinstance(progress, dict) else None
            activity = {"available": True, "processing": True}
            if type(decoded) is int and decoded >= 0:
                activity["decoded"] = decoded
            return activity
    return {"available": True, "processing": False}


def model_device_status(config, model, state, service, logs, cpu):
    """Use a selected model's current child log, never another model's GPU log.

    This reports weight placement, not instantaneous GPU utilization. A GPU in
    --list-devices or in saved configuration is not evidence of offloading.
    """
    mode = config.get("memory_mode", 0)
    chosen = config.get("device", "ROCm0").split(",")
    devices = [d for d in config.get("devices", []) if d.get("id") in chosen]
    configured = ", ".join(f"{d['id']} — {d['name']}" for d in devices) or ", ".join(chosen)
    mode_name = {0: "Automatic GPU + RAM", 1: "GPU + experts in CPU RAM", 2: "CPU / RAM only"}.get(mode, "Unknown")
    summary = f"Configured: CPU — {cpu}; placement unverified" if mode == 2 else f"Configured: {configured}; offload unverified"
    status = {"summary": summary, "confirmed": False, "cpu_name": cpu, "model": model,
              "model_state": state, "configured_mode": mode_name, "configured_devices": devices,
              "service": service, "evidence": "Waiting for the selected model's current load log."}
    # Current invocation logs alone are insufficient after a model exits or
    # reloads. Require its current API state and most recent matching spawn.
    if service.get("ActiveState") != "active" or state not in ("loaded", "loading"):
        status["evidence"] = f"Model state: {state or 'unknown'}; service: {service.get('ActiveState', 'unknown')}."
        return status
    spawns = list(re.finditer(r"spawning server instance with name=(\S+) on port (\d+)", logs))
    matching = [spawn for spawn in spawns if spawn[1] == model]
    if not matching:
        return status
    spawn = matching[-1]
    section = logs[spawn.end():]
    # Child output is forwarded with [port]. Never use untagged discovery logs
    # or a concurrently loading child's tensor allocation.
    child = "\n".join(line for line in section.splitlines() if re.search(r"\[\s*" + spawn[2] + r"\]", line))
    if re.search(r"instance name=" + re.escape(model) + r" exited with status", section):
        status["evidence"] = "The model process exited; its previous device placement is no longer current."
        return status
    status["port"] = int(spawn[2])
    used = []
    # Newer builds follow the name with a PCI address: "(AMD Radeon RX 6800) (0000:0a:00.0)".
    for identifier, name in re.findall(r"using device\s+([A-Za-z0-9_-]+)\s*\(([^()]+)\)", child):
        if identifier not in [device["id"] for device in used]:
            used.append({"id": identifier, "name": name.strip()})
    layers = list(re.finditer(r"offloaded\s+(\d+)/(\d+)\s+layers to GPU", child))
    buffers = re.findall(r"\b([A-Za-z0-9_-]+)\s+model buffer size\s*=\s*([0-9.]+)\s*MiB", child)
    if not layers:
        status["evidence"] = "The current model log has not reported an offloaded layer count."
        return status
    offloaded, total = map(int, layers[-1].groups())
    if not total or not 0 <= offloaded <= total:
        return status
    # An allocation summary may be emitted before weights finish loading. Do
    # not claim a completed load until the router reports this child ready.
    status.update({"offloaded_layers": offloaded, "total_layers": total,
                   "evidence": layers[-1][0], "actual_devices": used, "model_buffers_mib": dict(buffers)})
    memory = allocation(child)
    if memory:
        status["memory"] = memory
    if state != "loaded":
        status["summary"] = f"Loading model · {offloaded}/{total} layers assigned to GPU; load not complete"
        return status
    status["confirmed"] = True
    if not offloaded:
        status["backend"] = "CPU"
        status["summary"] = f"CPU — {cpu} · 0/{total} layers on GPU"
        return status
    # Prefer names actually reported by this child's initialization. Saved
    # names can describe the selected device, but remain labeled as configured.
    actual = ", ".join(f"{d['name']} ({d['id']})" for d in used)
    if not actual:
        actual = f"GPU ({configured}; device configured)"
    cpu_weights = any(identifier in ("CPU", "CPU_Mapped") and float(size) > 0 for identifier, size in buffers)
    mixed = offloaded < total or mode == 1 or cpu_weights
    status["backend"] = "GPU + CPU" if mixed else "GPU"
    status["summary"] = f"{status['backend']} — {actual} · {offloaded}/{total} layers on GPU"
    if mode == 1:
        status["summary"] += "; experts in CPU RAM"
    return status


def device_status(model):
    """Read-only status; this never loads a model, restarts, or changes setup."""
    config_dir, _ = locations()
    config = json.loads((config_dir / "inferssd-library.json").read_text())
    result = subprocess.run(["systemctl", "--user", "show", SERVICE,
                             "--property=ActiveState,SubState,Result,ExecMainStatus,NRestarts,InvocationID"],
                            capture_output=True, text=True, timeout=5)
    service = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    logs, log_error = "", ""
    invocation = service.get("InvocationID", "")
    # Journal invocations prevent a previous service's successful GPU load
    # from being presented as the current process's placement.
    if re.fullmatch(r"[0-9a-f]{32}", invocation):
        journal = subprocess.run(["journalctl", "--user", "-u", SERVICE,
                                  "_SYSTEMD_INVOCATION_ID=" + invocation, "-n", "2000",
                                  "--no-pager", "-o", "cat"], capture_output=True, text=True, timeout=5)
        if journal.returncode == 0:
            logs = journal.stdout[-500000:]
        else:
            log_error = journal.stderr[-2000:]
    state, api_error, opener = "", "", None
    try:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(f"http://127.0.0.1:{config['port']}/v1/models", timeout=2) as response:
            inventory = json.load(response)
        for entry in inventory.get("data", []):
            if entry.get("id") == model:
                state = entry.get("status", {}).get("value", "")
                if entry.get("status", {}).get("failed"):
                    state = "failed"
                break
    except (OSError, ValueError) as error:
        api_error = str(error)
    status = model_device_status(config, model, state, service, logs, cpu_name())
    # Live readings describe the whole GPU and this model's process right now.
    live = {"gpus": gpu_usage()}
    if status.get("port") and state in ("loaded", "loading"):
        process = process_memory(config["server"], status["port"])
        if process:
            live["process"] = process
    if opener and status.get("port") and state == "loaded":
        live["slot"] = slot_activity(status["port"], opener)
    status["live"] = live
    if result.returncode:
        status["service_error"] = result.stderr[-2000:]
    if api_error:
        status["api_error"] = api_error
    if log_error:
        status["log_error"] = log_error
    return status


def systemctl(*args):
    return subprocess.run(["systemctl", "--user", *args], capture_output=True, text=True, timeout=35)


def unit_quote(value):
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%") + '"'


def install(args):
    config_dir, data = locations()
    binary = Path(args.server).expanduser().resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError(f"InferSSD executable not found at {binary}. Build it first or change Server executable.")
    result = subprocess.run([str(binary), "--help"], capture_output=True, text=True, timeout=20)
    help_text = result.stdout + result.stderr
    required = ["--models-preset", "--models-max", "--models-autoload", "--fit-target", "--fit",
                "--device", "--gpu-layers", "--ctx-size", "--batch-size", "--ubatch-size", "--parallel",
                "--jinja", "--host", "--port", "--reasoning-budget"]
    if args.memory_mode != 2:
        required.append("--list-devices")
    if args.memory_mode == 1:
        required.append("--cpu-moe")
    if result.returncode or not all(flag in help_text for flag in required):
        missing = ", ".join(flag for flag in required if flag not in help_text)
        raise RuntimeError("This InferSSD binary lacks required router, memory, or thinking options "
                           f"({missing or 'help command failed'}). Rebuild your server before setup.")
    config = {"server": str(binary), "model_dir": str(Path(args.model_dir).expanduser().absolute()),
              "port": args.port, "memory_mode": args.memory_mode, "context_size": args.context_size,
              "gpu_headroom": args.gpu_headroom, "device": "none" if args.memory_mode == 2 else args.device}
    command(config, data)  # Validate before touching a working service.
    config["cpu_name"] = cpu_name()
    if args.memory_mode != 2:
        config["devices"] = gpu_devices(binary)
        config["device"] = gpu_device(binary, args.device, config["devices"])
    if systemctl("show-environment").returncode:
        raise RuntimeError("A systemd user session is required on the inference computer. Log in normally and retry.")
    # Check discovery before modifying a working service.
    models, skipped = discover([config["model_dir"], "~/.local/share/inferssd/models", "~/.cache/llama.cpp", "~/.cache/huggingface/hub"])
    if not models:
        raise RuntimeError(f"No complete GGUF models found. Check Model folder ({config['model_dir']}) or download a model first.")
    active = systemctl("is-active", SERVICE).returncode == 0
    saved_config = config_dir / "inferssd-library.json"
    previous_port = None
    if saved_config.exists():
        try:
            previous_port = json.loads(saved_config.read_text()).get("port")
        except (OSError, ValueError):
            pass
    if not active or previous_port != args.port:
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.bind(("127.0.0.1", args.port))
            except OSError:
                raise RuntimeError(f"Port {args.port} is already in use. Choose another Server port in Advanced.") from None
    data.mkdir(parents=True, exist_ok=True, mode=0o700)
    helper = data / "inferssd-library.py"
    if Path(__file__).resolve() != helper.resolve():
        atomic_write(helper, Path(__file__).read_text())
    atomic_write(config_dir / "inferssd-library.json", json.dumps(config))
    unit = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "systemd/user" / SERVICE
    # Keep XDG paths explicit: the user manager need not inherit the SSH environment.
    service = ("[Unit]\nDescription=Chela InferSSD model library\nAfter=network.target\n\n[Service]\n"
               f"Environment={unit_quote('XDG_CONFIG_HOME=' + str(config_dir.parent))}\n"
               f"Environment={unit_quote('XDG_DATA_HOME=' + str(data.parent.parent))}\n"
               f"ExecStart={unit_quote(sys.executable)} {unit_quote(helper)} --serve\n"
               "Restart=on-failure\nRestartSec=5\nTimeoutStopSec=60\nKillMode=control-group\n\n"
               "[Install]\nWantedBy=default.target\n")
    atomic_write(unit, service)
    for action in [("daemon-reload",), ("enable", SERVICE), ("restart", SERVICE)]:
        result = systemctl(*action)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or "Could not start the model library service.")
    # No model loads during setup: this checks the router and inventory only.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        try:
            with opener.open(f"http://127.0.0.1:{args.port}/v1/models", timeout=2) as response:
                inventory = json.load(response)
            if inventory.get("data"):
                inventory["skipped"] = skipped
                print(json.dumps(inventory))
                return
        except (OSError, ValueError):
            pass
        time.sleep(.25)
    logs = subprocess.run(["journalctl", "--user", "-u", SERVICE, "-n", "12", "--no-pager"], capture_output=True, text=True, timeout=5)
    raise RuntimeError("Model library did not start. " + logs.stdout[-3000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--device-status", action="store_true")
    parser.add_argument("--model", default="")
    parser.add_argument("--server", default="~/inferssd/build-rocm/bin/inferssd-server")
    parser.add_argument("--model-dir", default="~/models")
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument("--memory-mode", type=int, choices=(0, 1, 2), default=0,
                        help="0: automatic GPU + RAM, 1: MoE experts in RAM, 2: CPU only")
    parser.add_argument("--context-size", type=int, default=8192)
    parser.add_argument("--gpu-headroom", type=int, default=1024, help="GPU memory reserve in MiB")
    parser.add_argument("--device", default="auto")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    if args.install:
        install(args)
    elif args.device_status:
        print(json.dumps(device_status(args.model)))
    elif args.serve:
        config_dir, data = locations()
        config = json.loads((config_dir / "inferssd-library.json").read_text())
        prepare(config, data)
        # An inherited single-model setting would silently disable router mode.
        env = {k: v for k, v in os.environ.items() if not k.startswith("LLAMA_ARG_")}
        env["LLAMA_CACHE"] = str(data / "cache")
        os.execve(config["server"], command(config, data), env)
    else:
        parser.error("choose --install or --serve")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
