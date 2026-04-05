#!/usr/bin/env python3

import os
import subprocess
import sys
import time

CACHE_FILE = "tmenu_path"

if not os.path.exists(CACHE_FILE):
    binaries = set()
    path_dirs = os.environ.get("PATH", "").split(os.pathsep)
    temp_dir = os.environ.get("XDG_CACHE_HOME", "/tmp")
    cache_path = os.path.join(temp_dir, CACHE_FILE)

    for directory in path_dirs:
        if os.path.isdir(directory):
            try:
                for entry in os.listdir(directory):
                    full_path = os.path.join(directory, entry)
                    if (os.path.isfile(full_path) or os.path.islink(full_path)) and os.access(full_path, os.X_OK):
                        # binaries.add(full_path)
                        binaries.add(entry)
            except PermissionError:
                continue

    with open(cache_path, "w") as f:
        for b in sorted(binaries):
            f.write(b + "\n")

try:
    with open(cache_path, "r") as f:
        result = subprocess.run(
            ["tmenu"],
            stdin=f,
            capture_output=True,
            text=True,
            check=True
        )
        prog = result.stdout.strip()
except subprocess.CalledProcessError:
    sys.exit(1)
except FileNotFoundError:
    print("Error: tmenu was not found in path envvar.", file=sys.stderr)
    sys.exit(1)

if not prog:
    sys.exit(0)

app_name = os.path.basename(prog)

cli_whitelist = {"vim", "nvim", "nano", "ranger", "htop", "top", "alsamixer", "ncdu", "btop", "pipemixer"}

if app_name in cli_whitelist:
    subprocess.run([prog])
else:
    subprocess.Popen(
        [prog],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
        start_new_session=True
    )
    time.sleep(0.1)
