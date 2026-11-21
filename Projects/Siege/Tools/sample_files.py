"""The files the installed Siege sample needs: its scenes and everything they reference, followed through materials
(.mat), models (.gltf: their buffers and images) and the assets' .meta files, plus the project file, the scripts and
CREDITS.md. Writes Projects/Siege/Sample.files (paths relative to the project, one a line) for cmake/Packaging.cmake:
the installers ship only these, not the source assets nothing uses, nor the tools.

Run: python sample_files.py [--report]"""
import json
import os
import re
import sys

import siege as S

PROJECT = os.path.dirname(S.ASSETS)
SCENES = ["Scenes/Siege.scene", "Scenes/Lineup.scene"]  # the ones a user opens (AssetPreview is for picking assets)


def metas():
    """Every asset's GUID and the file it belongs to."""
    out = {}
    for root, _, files in os.walk(S.ASSETS):
        for f in files:
            if f.endswith(".meta"):
                path = os.path.join(root, f)
                try:
                    guid = json.load(open(path, encoding="utf-8")).get("guid")
                except (ValueError, OSError):
                    continue
                if guid:
                    out[int(guid)] = path[:-5]
    return out


def main():
    by_guid = metas()
    need = set()
    todo = [os.path.join(S.ASSETS, s) for s in SCENES]
    numbers = re.compile(r"(?<![\w.])(\d{6,20})(?![\w.])")
    while todo:
        path = os.path.normpath(todo.pop())
        if path in need or not os.path.exists(path):
            continue
        need.add(path)
        if os.path.exists(path + ".meta"):
            need.add(path + ".meta")
        ext = os.path.splitext(path)[1].lower()
        if ext in (".scene", ".mat", ".prefab", ".dproj", ".anim", ".controller", ".vfx", ".timeline", ".meta"):
            text = open(path, encoding="utf-8", errors="ignore").read()
            for m in numbers.findall(text):
                g = int(m)
                if g in by_guid:
                    todo.append(by_guid[g])
        if ext == ".gltf":
            doc = json.load(open(path, encoding="utf-8"))
            base = os.path.dirname(path)
            for item in doc.get("buffers", []) + doc.get("images", []):
                uri = item.get("uri", "")
                if uri and not uri.startswith("data:"):
                    todo.append(os.path.join(base, uri.replace("%20", " ")))
    # The project itself: its file, scripts (not their build output) and credits.
    for f in ("Project.dproj", "CREDITS.md"):
        need.add(os.path.join(PROJECT, f))
    for root, dirs, files in os.walk(os.path.join(PROJECT, "Scripts")):
        dirs[:] = [d for d in dirs if d not in ("bin", "obj")]
        for f in files:
            if f != "Directory.Build.props":
                need.add(os.path.join(root, f))
    rel = sorted(os.path.relpath(p, PROJECT).replace("\\", "/") for p in need if os.path.isfile(p))
    with open(os.path.join(PROJECT, "Sample.files"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# The files the installed sample needs (Tools/sample_files.py; cmake/Packaging.cmake ships these).\n")
        f.write("\n".join(rel) + "\n")
    size = sum(os.path.getsize(os.path.join(PROJECT, r)) for r in rel)
    print(f"Sample.files: {len(rel)} files, {size / 1e6:.1f} MB")
    if "--report" in sys.argv:
        shipped = set(rel)
        left = []
        for root, _, files in os.walk(S.ASSETS):
            for f in files:
                r = os.path.relpath(os.path.join(root, f), PROJECT).replace("\\", "/")
                if r not in shipped:
                    left.append((os.path.getsize(os.path.join(root, f)), r))
        for s, r in sorted(left, reverse=True)[:25]:
            print(f"  not shipped {s / 1e6:7.1f} MB  {r}")


if __name__ == "__main__":
    main()
