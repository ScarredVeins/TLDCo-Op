"""Hard-link the Episode 5 bundles that hold the story characters and NPC animations into a staging folder, so
AssetRipper reads only what we need (plus their dependencies) instead of the whole 2.9 GB build.

Usage: python -I stage_bundles.py <ep5 aa_ep5 dir> <staging dir>
Prints the assets found and writes staging/assets.txt (the internal ids we want from the rip).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from catalog_deps import Catalog  # noqa: E402

WANTED = re.compile(
    r"^Assets/ArtAssets/Characters/("
    r"[A-Za-z0-9_]+_Ep5/[^/]+\.fbx"                       # every story character model
    r"|NPC/NPC_[MF]/(Locomotion|Idles)/[^/]+\.fbx"        # human idle / walk / run animations
    r")$")


def main():
    aa, staging = sys.argv[1], sys.argv[2]
    cat = Catalog(os.path.join(aa, "catalog.json"))
    os.makedirs(staging, exist_ok=True)
    wanted, bundles = [], set()
    seen = set()
    for e in cat.entries:
        path = cat.ids[e[0]]
        if path in seen or not WANTED.match(path):
            continue
        seen.add(path)
        wanted.append(path)
        for d in cat.deps(e):
            bundles.add(cat.ids[d[0]].replace("\\", "/").split("/")[-1])
    src_dir = os.path.join(aa, "StandaloneWindows64")
    total = 0
    for b in sorted(bundles):
        src, dst = os.path.join(src_dir, b), os.path.join(staging, b)
        total += os.path.getsize(src)
        if not os.path.exists(dst):
            os.link(src, dst)  # same volume: no copy, no extra disk space
    with open(os.path.join(staging, "assets.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(wanted) + "\n")
    models = [w for w in wanted if "_Ep5/" in w]
    anims = [w for w in wanted if "/NPC_" in w]
    print(f"{len(models)} character models, {len(anims)} NPC animations, {len(bundles)} bundles "
          f"({total / 1048576:.0f} MB) staged in {staging}")
    for m in models:
        print("  ", m.split("Characters/")[1])


if __name__ == "__main__":
    main()
