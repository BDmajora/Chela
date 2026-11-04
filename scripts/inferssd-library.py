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
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

SERVICE = "chela-inferssd.service"
SPLIT = re.compile(r"^(.*)-(\d{5})-of-(\d{5})\.gguf$", re.I)
# Architectures InferSSD's expert executor runs (engine/src/ssd/model-adapter.cpp). In memory mode 3 these models
# load through its RAM and GPU expert caches; every other model loads as in mode 0.
SSD_ARCHITECTURES = ("qwen4exp", "deepseek41", "muse-spark")
# GGUF metadata value sizes by type: u8 i8 u16 i16 u32 i32 f32 bool _ _ u64 i64 f64 (8 string, 9 array).
GGUF_SIZES = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}
# The routed experts' tensors, and ggml's tensor types: values and bytes of a block (ggml_type_traits).
EXPERT_TENSOR = re.compile(r"blk\.\d+\.ffn_(up|gate|down)_exps\.weight")
GGML_BLOCKS = {0: (1, 4), 1: (1, 2), 2: (32, 18), 3: (32, 20), 6: (32, 22), 7: (32, 24), 8: (32, 34), 9: (32, 40),
               10: (256, 84), 11: (256, 110), 12: (256, 144), 13: (256, 176), 14: (256, 210), 15: (256, 292),
               16: (256, 66), 17: (256, 74), 18: (256, 98), 19: (256, 50), 20: (32, 18), 21: (256, 110),
               22: (256, 82), 23: (256, 136), 29: (256, 56), 30: (1, 2), 34: (256, 54), 35: (256, 66),
               39: (32, 17), 40: (64, 36), 41: (128, 18), 42: (64, 18)}


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


def gguf_skip(stream, kind, depth=0):
    if kind in GGUF_SIZES:
        stream.seek(GGUF_SIZES[kind], os.SEEK_CUR)
    elif kind == 8:
        stream.seek(struct.unpack("<Q", stream.read(8))[0], os.SEEK_CUR)
    elif kind == 9 and depth == 0:
        inner, count = struct.unpack("<IQ", stream.read(12))
        if inner in GGUF_SIZES:
            stream.seek(GGUF_SIZES[inner] * count, os.SEEK_CUR)
        else:
            for _ in range(min(count, 1 << 20)):
                gguf_skip(stream, inner, 1)
    else:
        raise ValueError("unsupported GGUF metadata type")


def gguf_architecture(path):
    """general.architecture of a GGUF file, or "" when it cannot be read; only the metadata before it is read."""
    try:
        with open(path, "rb") as stream:
            magic, version, _tensors, count = struct.unpack("<4sIQQ", stream.read(24))
            if magic != b"GGUF" or version not in (2, 3):
                return ""
            for _ in range(min(count, 65536)):
                size = struct.unpack("<Q", stream.read(8))[0]
                if size > 65536:
                    return ""
                key = stream.read(size)
                kind = struct.unpack("<I", stream.read(4))[0]
                if key == b"general.architecture" and kind == 8:
                    size = struct.unpack("<Q", stream.read(8))[0]
                    return stream.read(size).decode("utf-8") if size <= 256 else ""
                gguf_skip(stream, kind)
    except (OSError, ValueError, struct.error, UnicodeDecodeError):
        pass
    return ""


def gguf_experts(path):
    """The routed expert tensors of one GGUF file, from its header: (name, experts, bytes of an expert, file offset)."""
    with open(path, "rb") as stream:
        magic, version, tensors, count = struct.unpack("<4sIQQ", stream.read(24))
        if magic != b"GGUF" or version not in (2, 3):
            raise ValueError("not a GGUF file")
        alignment = 32
        for _ in range(count):
            key = stream.read(struct.unpack("<Q", stream.read(8))[0])
            kind = struct.unpack("<I", stream.read(4))[0]
            if key == b"general.alignment" and kind == 4:
                alignment = struct.unpack("<I", stream.read(4))[0]
            else:
                gguf_skip(stream, kind)
        found = []
        for _ in range(tensors):
            name = stream.read(struct.unpack("<Q", stream.read(8))[0]).decode("utf-8")
            dimensions = struct.unpack("<I", stream.read(4))[0]
            shape = struct.unpack(f"<{dimensions}Q", stream.read(8 * dimensions))
            kind, offset = struct.unpack("<IQ", stream.read(12))
            if EXPERT_TENSOR.fullmatch(name):
                if dimensions != 3 or kind not in GGML_BLOCKS or shape[0] % GGML_BLOCKS[kind][0]:
                    raise ValueError(f"unsupported expert tensor {name}")
                values, block = GGML_BLOCKS[kind]
                found.append((name, shape[2], shape[0] // values * block * shape[1], offset))
        data = -(-stream.tell() // alignment) * alignment
    return [(name, experts, bytes_, data + offset) for name, experts, bytes_, offset in found]


def write_expert_index(path, links):
    """The alias index InferSSD's expert caches read a GGUF's experts by: each expert role's file range, with no weight
    copied. True when the model has routed experts."""
    rows = ["tensor\texpert_idx\tnbytes\tsource_path\toffset"]
    for link in links:
        for name, experts, expert_bytes, offset in gguf_experts(link):
            rows += [f"{name}\t{i}\t{expert_bytes}\t{link}\t{offset + i * expert_bytes}" for i in range(experts)]
    if len(rows) == 1:
        return False
    atomic_write(path, "\n".join(rows) + "\n")
    return True


def model_options(config, data, model, links):
    """Per-model preset options. Router mode gives command-line options precedence over a model's section, so in mode 3
    the placement options live here: InferSSD's expert caches for a supported architecture, fit for the rest."""
    if config.get("memory_mode", 0) != 3:
        return []
    index = links[0].parent / "experts.tsv"
    try:
        routed = gguf_architecture(links[0]) in SSD_ARCHITECTURES and write_expert_index(index, links)
    except (OSError, ValueError, struct.error, UnicodeDecodeError):
        routed = False
    if not routed:
        return ["gpu-layers = auto", "fit = on", f"fit-target = {config.get('gpu_headroom', 1024)}"]
    traces = data / "traces"
    traces.mkdir(exist_ok=True, mode=0o700)
    return [
        "ssd-expert-source = " + json.dumps([str(index)]),
        "ssd-vram-budget = auto",
        # The tier grows until only this is left, and a long prompt's scratch comes out of it.
        f"ssd-vram-reserve-mib = {config.get('gpu_headroom', 1024)}",
        # The expert predictor's traces and usage profile, kept for this model across restarts.
        "ssd-trace-file = " + json.dumps([str(traces / (model["id"] + ".trace"))]),
        # The sink reads the experts: mapped, their pages stay unread.
        "gpu-layers = 99", "fit = off", "mmap = true", "repack = false", "defer-experts = true",
    ]


def prepare(config, data):
    roots = [config["model_dir"], "~/.local/share/inferssd/models", "~/.cache/llama.cpp", "~/.cache/huggingface/hub"]
    models, skipped = discover(roots)
    if not models:
        raise RuntimeError(f"No complete GGUF models found in {config['model_dir']} or the model caches. Download a GGUF first, or change Model folder.")
    if any(c in str(data) for c in "\r\n\t#;"):
        raise RuntimeError("The home/data folder contains characters unsupported by InferSSD presets.")
    preset = ["[*]", "load-on-startup = false", ""]
    for model in models:
        folder = data / "models" / model["id"]
        folder.mkdir(parents=True, exist_ok=True, mode=0o700)
        links = []
        for i, part in enumerate(model["parts"], 1):
            name = "model.gguf" if len(model["parts"]) == 1 else f"model-{i:05d}-of-{len(model['parts']):05d}.gguf"
            link = folder / name
            if link.is_symlink():
                link.unlink()
            elif link.exists():
                raise RuntimeError(f"Expected a managed symlink at {link}; no model file was overwritten.")
            link.symlink_to(part.absolute())
            links.append(link)
        preset += [f"[{model['id']}]", f"model = {links[0]}", *model_options(config, data, model, links), ""]
    atomic_write(data / "models.ini", "\n".join(preset))
    (data / "cache").mkdir(exist_ok=True, mode=0o700)
    return models, skipped


def command(config, data):
    mode = config.get("memory_mode", 0)
    context = config.get("context_size", 8192)
    headroom = config.get("gpu_headroom", 1024)
    device = config.get("device", "ROCm0")  # Preserve older installed configurations.
    if (type(mode) is not int or mode not in (0, 1, 2, 3) or type(context) is not int or
            not 512 <= context <= 131072 or type(headroom) is not int or not 256 <= headroom <= 65536 or
            not isinstance(device, str) or not re.fullmatch(r"[A-Za-z0-9_,-]+", device)):
        raise RuntimeError("Invalid memory settings. Reconnect using Model library setup.")
    # Mode 3 places each model in its preset section (model_options): options given here would override it.
    placement = [] if mode == 3 else ["--gpu-layers", "0" if mode == 2 else "auto", "--fit", "off" if mode == 2 else "on",
                                      "--fit-target", str(headroom)]
    args = [config["server"], "--models-preset", str(data / "models.ini"),
            "--models-max", "1", "--models-autoload", "--device", "none" if mode == 2 else device,
            *placement, "--ctx-size", str(context),
            # Experts kept in RAM are copied to the GPU once per micro-batch during prompt processing,
            # so 1024-token micro-batches prefill about 5x faster than 128 at a small VRAM cost.
            "--batch-size", "2048", "--ubatch-size", "1024",
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


def memory_limits(meminfo=Path("/proc/meminfo")):
    """The service's memory limit for InferSSD's expert caches: they size themselves within a finite cgroup and
    require swap off for it. The limit leaves the desktop 1/16 of RAM, at least 2 GiB."""
    match = re.search(r"^MemTotal:\s+(\d+) kB", meminfo.read_text(), re.M)
    if not match:
        raise RuntimeError("Cannot read MemTotal for the expert cache memory limit.")
    total = int(match[1]) * 1024
    limit = total - max(2 << 30, total // 16)
    if limit < 4 << 30:
        raise RuntimeError("The InferSSD expert cache needs more than 6 GiB of RAM on the inference computer.")
    return f"MemoryMax={limit}\nMemorySwapMax=0\n"


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
    if args.memory_mode == 3:
        required += ["--ssd-expert-source", "--ssd-vram-budget", "--ssd-trace-file", "--defer-experts"]
    if result.returncode or not all(flag in help_text for flag in required):
        missing = ", ".join(flag for flag in required if flag not in help_text)
        raise RuntimeError("This InferSSD binary lacks required router, memory, or thinking options "
                           f"({missing or 'help command failed'}). Rebuild your server before setup"
                           f"{' with the expert tier (cmake preset linux-rocm or linux-cuda)' if args.memory_mode == 3 else ''}.")
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
               f"{memory_limits() if args.memory_mode == 3 else ''}"
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
    parser.add_argument("--memory-mode", type=int, choices=(0, 1, 2, 3), default=0,
                        help="0: automatic GPU + RAM, 1: MoE experts in RAM, 2: CPU only, "
                             "3: InferSSD expert caches for supported MoE models (others as 0)")
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
