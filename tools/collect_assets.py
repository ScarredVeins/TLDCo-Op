"""Copy the story-character prefabs and animation clips listed in bodies.json, plus everything they reference
(meshes, avatars, materials, textures), from AssetRipper's export into the BodyPack Unity project.

Usage: python -I collect_assets.py <export Assets dir> <bodies.json> <project Assets dir>

References are followed through "guid: <32 hex>" in YAML assets; each file is copied with its .meta so GUIDs
(and therefore references) survive. Shaders are skipped: the build swaps in TLDCoop's own shaders.
"""
import json
import os
import re
import shutil
import sys

GUID = re.compile(rb"guid: ([0-9a-f]{32})")
TEXT_EXT = {".prefab", ".mat", ".asset", ".anim", ".controller", ".overrideController", ".mask"}


def index_guids(assets_dir):
    index = {}
    for root, _, files in os.walk(assets_dir):
        for name in files:
            if not name.endswith(".meta"):
                continue
            meta = os.path.join(root, name)
            with open(meta, "rb") as f:
                m = GUID.search(f.read(512))
            if m:
                index[m.group(1).decode()] = meta[:-5]
    return index


def main():
    export_assets, bodies_json, project_assets = sys.argv[1], sys.argv[2], sys.argv[3]
    spec = json.load(open(bodies_json, encoding="utf-8"))
    roots = [b["prefab"] for b in spec["bodies"]] + spec["clipsM"] + spec["clipsF"] + [spec["clipDown"]]

    export_root = os.path.dirname(export_assets)  # paths in bodies.json start with "Assets/"
    print("indexing GUIDs...", flush=True)
    index = index_guids(export_assets)
    print(f"  {len(index)} assets indexed")

    todo = [os.path.normpath(os.path.join(export_root, r)) for r in roots]
    missing = [t for t in todo if not os.path.exists(t)]
    for m in missing:
        print("MISSING root:", m)
    done, copied_bytes, skipped_shaders = set(), 0, 0
    shader_of_material = {}  # "Assets/..mat" -> original shader name, so the build can pick a matching fallback
    while todo:
        path = todo.pop()
        if path in done or not os.path.exists(path):
            continue
        done.add(path)
        if path.endswith(".shader"):
            skipped_shaders += 1
            continue
        if path.endswith(".mat"):
            with open(path, "rb") as f:
                m = re.search(rb"m_Shader: \{[^}]*guid: ([0-9a-f]{32})", f.read())
            shader_path = index.get(m.group(1).decode()) if m else None
            name = None
            if shader_path and os.path.exists(shader_path):
                with open(shader_path, "rb") as f:
                    n = re.search(rb'Shader "([^"]+)"', f.read(4096))
                name = n.group(1).decode() if n else None
            rel_mat = "Assets/" + os.path.relpath(path, export_assets).replace(os.sep, "/")
            shader_of_material[rel_mat] = name or "unknown"
        rel = os.path.relpath(path, export_assets)
        dest = os.path.join(project_assets, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        for src, dst in ((path, dest), (path + ".meta", dest + ".meta")):
            if os.path.exists(src) and (not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src)):
                shutil.copyfile(src, dst)
        copied_bytes += os.path.getsize(path)
        if os.path.splitext(path)[1] in TEXT_EXT:
            with open(path, "rb") as f:
                for g in GUID.findall(f.read()):
                    target = index.get(g.decode())
                    if target and target not in done:
                        todo.append(target)
    shader_map = os.path.join(os.path.dirname(project_assets), "shader_map.json")
    with open(shader_map, "w", encoding="utf-8") as f:
        json.dump({"entries": [{"material": k, "shader": v} for k, v in sorted(shader_of_material.items())]}, f,
                  indent=1)
    print(f"copied {len(done) - skipped_shaders} assets ({copied_bytes / 1048576:.0f} MB) into {project_assets}; "
          f"skipped {skipped_shaders} shader(s); {len(missing)} missing root(s); "
          f"{len(shader_of_material)} materials mapped in {shader_map}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
