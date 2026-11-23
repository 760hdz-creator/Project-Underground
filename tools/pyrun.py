"""Run a kit script with the kit's bundled packages (pylib/, capstone on Linux) on sys.path.

usage: python3 -I tools/pyrun.py <script.py> [args...]   (ug_build.py uses it on Linux)
"""
import os
import runpy
import sys

kit = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(kit, "pylib"))
script = sys.argv[1]
sys.argv = sys.argv[1:]
sys.path.insert(0, os.path.dirname(os.path.abspath(script)))
runpy.run_path(script, run_name="__main__")
