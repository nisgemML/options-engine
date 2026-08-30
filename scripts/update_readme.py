#!/usr/bin/env python3
"""Regenerate the benchmark block in README.md from BENCHMARK_RESULTS.md.

The README must never carry a number that is not in the committed results
file. This script rewrites everything between <!-- BENCH:START --> and
<!-- BENCH:END --> from the first "Submit latency" block it finds, and
exits non-zero if README and results disagree (use it as a CI check with
--check).
"""
import re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
readme = root / "README.md"
results = root / "BENCHMARK_RESULTS.md"

txt = results.read_text()
m = re.search(r"Submit latency.*?p50\s*:\s*(\d+)\s*ns.*?p99\s*:\s*(\d+)\s*ns.*?p99\.9:\s*(\d+)\s*ns", txt, re.S)
if not m:
    sys.exit("could not find submit latency block in BENCHMARK_RESULTS.md")
p50, p99, p999 = m.groups()
env = re.search(r"\*\*Environment:\*\*\s*(.*?)\n", txt)
env = env.group(1).strip().rstrip(",(").rstrip() if env else "see BENCHMARK_RESULTS.md"

block = f"""<!-- BENCH:START -->
### Order flow replay — 500K synthetic events ({env})

| Metric | Value |
|--------|-------|
| Submit latency p50 | **{p50} ns** |
| Submit latency p99 | {p99} ns |
| Submit latency p99.9 | {p999} ns |
<!-- BENCH:END -->"""

r = readme.read_text()
new = re.sub(r"<!-- BENCH:START -->.*?<!-- BENCH:END -->", block, r, flags=re.S)
if "--check" in sys.argv:
    sys.exit(0 if new == r else "README benchmark block is out of date; run scripts/update_readme.py")
readme.write_text(new)
print(f"README updated: p50={p50} p99={p99} p99.9={p999}")
