"""Show which AssetBundles an Addressables asset lives in and depends on.

Usage: python -I catalog_deps.py <catalog.json> <internal-id regex> [--other <catalog.json>]

Decodes ContentCatalogData:
  m_EntryDataString  base64: int32 count, then per entry 7 x int32:
                     internalId, provider, dependencyKey, depHash, extraData, primaryKey, resourceType
  m_BucketDataString base64: int32 count, then per key: int32 keyDataOffset, int32 n, n x int32 entry index
  m_ExtraDataString  base64: serialized objects; for bundles, AssetBundleRequestOptions JSON (m_BundleName, ...)
With --other, also reports whether each bundle's m_BundleName is already used by another build's catalog, which
would clash if both were loaded at once.
"""
import base64
import json
import re
import struct
import sys


class Catalog:
    def __init__(self, path):
        self.c = json.load(open(path, encoding="utf-8"))
        self.ids = self.c["m_InternalIds"]
        self.providers = self.c["m_ProviderIds"]
        self.types = [t.get("m_ClassName", "?") for t in self.c.get("m_resourceTypes", [])]
        e = base64.b64decode(self.c["m_EntryDataString"])
        n = struct.unpack_from("<i", e, 0)[0]
        self.entries = [struct.unpack_from("<7i", e, 4 + 28 * i) for i in range(n)]
        b = base64.b64decode(self.c["m_BucketDataString"])
        count = struct.unpack_from("<i", b, 0)[0]
        pos = 4
        self.buckets = []
        for _ in range(count):
            _offset, k = struct.unpack_from("<ii", b, pos)
            pos += 8
            self.buckets.append(list(struct.unpack_from(f"<{k}i", b, pos)))
            pos += 4 * k
        self.extra = base64.b64decode(self.c.get("m_ExtraDataString", ""))

    def bundle_name(self, entry):
        """m_BundleName from the entry's AssetBundleRequestOptions (JSON inside the extra data blob)."""
        offset = entry[4]
        if offset < 0:
            return None
        window = self.extra[offset:offset + 2048].decode("utf-8", "replace")
        m = re.search(r'"m_BundleName"\s*:\s*"([^"]*)"', window)
        return m.group(1) if m else None

    def deps(self, entry):
        key = entry[2]
        return [self.entries[i] for i in self.buckets[key]] if key >= 0 else []


def main():
    cat = Catalog(sys.argv[1])
    rx = re.compile(sys.argv[2], re.I)
    other_names = set()
    if "--other" in sys.argv:
        other = Catalog(sys.argv[sys.argv.index("--other") + 1])
        other_names = {other.bundle_name(e) for e in other.entries if other.ids[e[0]].endswith(".bundle")}
        other_names.discard(None)
    seen = set()
    for e in cat.entries:
        path = cat.ids[e[0]]
        if path.endswith(".bundle") or not rx.search(path) or path in seen:
            continue
        seen.add(path)
        rtype = cat.types[e[6]] if 0 <= e[6] < len(cat.types) else "?"
        print(f"{path}  [{rtype}]")
        for d in cat.deps(e):
            name = cat.bundle_name(d)
            clash = "  CLASH: already in the other build" if name in other_names else ""
            print(f"    {cat.ids[d[0]].split(chr(92))[-1]}  name={name}{clash}")


if __name__ == "__main__":
    main()
