#!/usr/bin/env python3
"""inputs/*.txt ("<frame> <buttons>") -> C header for PCE_SCRIPT builds."""
import sys
BITS = {"I": 0x01, "II": 0x02, "SEL": 0x04, "RUN": 0x08, "U": 0x10, "R": 0x20, "D": 0x40, "L": 0x80}
rows = []
for line in open(sys.argv[1]):
    p = line.split()
    if not p or p[0].startswith("#"):
        continue
    v = 0
    for t in p[1:]:
        v |= BITS.get(t, 0)
    rows.append((int(p[0]), v))
with open(sys.argv[2], "w") as o:
    o.write("static const struct { uint32_t f; uint8_t p; } script[] = {\n")
    o.write(",\n".join(f"    {{{f}, 0x{v:02x}}}" for f, v in rows) or "    {0, 0}")
    o.write("\n};\n")
