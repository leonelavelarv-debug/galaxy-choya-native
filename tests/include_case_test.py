#!/usr/bin/env python3
"""Falla si queda algún #include que solo resuelve sin distinguir mayúsculas (se rompería en Linux)."""
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(os.path.dirname(here), "scripts", "fix_include_case.py")
out = subprocess.run([sys.executable, tool, sys.argv[1]], capture_output=True, text=True, check=True).stdout
last = out.strip().splitlines()[-1]
print(last)
sys.exit(0 if last.startswith("0 ") else 1)
