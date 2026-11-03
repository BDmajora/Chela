"""Embed the remote setup helper; the installed desktop needs no local Python."""
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes() + b"\0"
Path(sys.argv[2]).write_text(
    "/* Generated from scripts/inferssd-library.py. */\n"
    "static const char inferssd_library[] = {\n"
    + "\n".join(",".join(str(b) for b in data[i:i + 24]) + "," for i in range(0, len(data), 24))
    + "\n};\n"
)
