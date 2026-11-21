"""profile_summary.py <file.dprof> [last N frames]: averages a Player --profile recording (CPU sections by path, GPU
passes, counters) over its last frames, slowest first."""
import json
import sys
from collections import defaultdict

frames = json.load(open(sys.argv[1]))["frames"]
frames = frames[-int(sys.argv[2]):] if len(sys.argv) > 2 else frames[len(frames) // 3:]
n = len(frames)
print(f"{n} frames, {sum(f['ms'] for f in frames) / n:.1f} ms a frame")


def average(key):
    total, calls = defaultdict(float), defaultdict(float)
    for f in frames:
        path = []
        for name, depth, ms, count in f[key]:
            path = path[:depth] + [name]
            total["/".join(path)] += ms
            calls["/".join(path)] += count
    return total, calls


for key in ("sections", "gpu"):
    total, calls = average(key)
    print(f"--- {key}")
    for name, ms in sorted(total.items(), key=lambda kv: -kv[1])[:25]:
        print(f"{ms / n:8.2f} ms {calls[name] / n:7.1f}x  {name}")
counters = defaultdict(float)
for f in frames:
    for k, v in f["counters"].items():
        counters[k] += v
print("--- counters")
for k, v in counters.items():
    print(f"{v / n:14.0f}  {k}")
