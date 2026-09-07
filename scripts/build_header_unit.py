"""Build a Clang header unit using the consumer's exact CMake compiler flags."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def header_unit_command(command, header, output):
    result = []
    i = 0
    while i < len(command):
        arg = command[i]
        if arg in ("-o", "-c", "-MF", "-MT", "-MQ"):
            i += 2
            continue
        if arg in ("-MD", "-MMD") or arg.startswith("-fmodule-file="):
            i += 1
            continue
        result.append(arg)
        i += 1
    return result + ["-fmodule-header=user", "-x", "c++-header", "--precompile",
                     str(header), "-o", str(output), "-MD", "-MF", str(output) + ".d",
                     "-MT", str(output)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    entries = json.loads(args.database.read_text())
    entry, = [e for e in entries if Path(e["file"]).name == "plan_header_unit.cpp"]
    command = entry.get("arguments") or shlex.split(entry["command"])
    subprocess.run(header_unit_command(command, args.header, args.output),
                   cwd=entry["directory"], check=True)


if __name__ == "__main__":
    main()
