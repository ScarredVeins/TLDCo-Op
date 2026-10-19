"""Minimal PE32+ reader: normal imports, delay-load imports, and exports. No dependencies.

Usage:
  python -I pe_info.py imports <file>            # DLLs imported normally and delay-loaded
  python -I pe_info.py exports <file> [--json]   # export names/ordinals (for proxy generation)
"""
import json
import struct
import sys


class PE:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        if magic != 0x20B:
            raise ValueError("only PE32+ (x64) supported")
        self.dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(16)]
        sec = opt + opt_size
        self.sections = []
        for i in range(nsec):
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, sec + 40 * i + 8)
            self.sections.append((va, max(vsize, rsize), raw))

    def off(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f"rva {rva:#x} not in any section")

    def cstr(self, rva):
        o = self.off(rva)
        return self.data[o:self.data.index(b"\0", o)].decode("ascii", "replace")

    def imports(self):
        rva, size = self.dirs[1]
        out = []
        if rva:
            o = self.off(rva)
            while True:
                ilt, _, _, name, iat = struct.unpack_from("<IIIII", self.data, o)
                if name == 0:
                    break
                out.append(self.cstr(name))
                o += 20
        return out

    def delay_imports(self):
        rva, size = self.dirs[13]
        out = []
        if rva:
            o = self.off(rva)
            while True:
                attrs, name = struct.unpack_from("<II", self.data, o)
                if name == 0:
                    break
                out.append(self.cstr(name))
                o += 32
        return out

    def exports(self):
        rva, size = self.dirs[0]
        if not rva:
            return []
        d = self.data
        o = self.off(rva)
        base, nfuncs, nnames, afuncs, anames, aords = struct.unpack_from("<IIIIII", d, o + 16)
        names = {}
        for i in range(nnames):
            nrva = struct.unpack_from("<I", d, self.off(anames) + 4 * i)[0]
            idx = struct.unpack_from("<H", d, self.off(aords) + 2 * i)[0]
            names[idx] = self.cstr(nrva)
        out = []
        for idx in range(nfuncs):
            frva = struct.unpack_from("<I", d, self.off(afuncs) + 4 * idx)[0]
            if frva == 0:
                continue
            forwarder = self.cstr(frva) if rva <= frva < rva + size else None
            out.append({"ordinal": base + idx, "name": names.get(idx), "forwarder": forwarder})
        return out


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    pe = PE(path)
    if cmd == "imports":
        print("imports:", ", ".join(pe.imports()))
        print("delay-load:", ", ".join(pe.delay_imports()) or "(none)")
    elif cmd == "exports":
        ex = pe.exports()
        if "--json" in sys.argv:
            print(json.dumps(ex, indent=1))
        else:
            for e in ex:
                print(e["ordinal"], e["name"] or "<ordinal-only>", e["forwarder"] or "")
            print(f"total {len(ex)}, named {sum(1 for e in ex if e['name'])}")


if __name__ == "__main__":
    main()
