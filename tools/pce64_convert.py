#!/usr/bin/env python3
"""PCE64 ROM converter.

Combines the PCE64 emulator template ROM with a PC Engine / TurboGrafx-16
HuCard image you own, producing one .z64 that boots on an N64 (flashcart,
Analogue 3D) or in an emulator.

    python3 pce64_convert.py PCE64_template.z64 "Bonk's Adventure (USA).pce" Bonk.z64

The game file can be a .pce/.bin or a .zip that contains one.
No dependencies beyond the Python 3 standard library.
"""
import argparse
import os
import re
import struct
import sys
import zipfile
import zlib

MAGIC = b"PCE64GM1"
HDR_SIZE = 64
VERSION = 1
MAPPER_NORMAL, MAPPER_SF2 = 0, 1
GF_REGION_US = 0x01
GF_NO_SPRLIMIT = 0x02
GF_NO_MULTITAP = 0x04
BLOB_ALIGN = 0x10000
BLOB_MIN_OFFSET = 0x110000      # keep clear of the 1 MB the N64 boot code checksums
MAX_N64_ROM = 64 * 1024 * 1024
TEMPLATE_TITLE = b"PCE64"

_REV = bytes(int(f"{i:08b}"[::-1], 2) for i in range(256))


class ConvertError(Exception):
    pass


def read_game(path):
    """Return (name, bytes) for a .pce/.bin or a .zip holding one."""
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            cands = [n for n in z.namelist() if n.lower().endswith((".pce", ".bin", ".sgx")) and not n.endswith("/")]
            if not cands:
                raise ConvertError(f"{path}: no .pce file inside the zip")
            if len(cands) > 1:
                raise ConvertError(f"{path}: more than one game inside the zip: {', '.join(cands)}")
            return os.path.basename(cands[0]), z.read(cands[0])
    with open(path, "rb") as f:
        return os.path.basename(path), f.read()


def reset_vector(rom):
    return rom[0x1FFE] | (rom[0x1FFF] << 8)


def normalize(name, data):
    """Strip copier headers, undo bit reversal, pick mapper. Returns (rom, mapper, notes)."""
    notes = []
    if len(data) % 0x2000 == 512:
        data = data[512:]
        notes.append("removed 512-byte copier header")
    if len(data) < 0x2000 or len(data) % 0x2000:
        raise ConvertError(f"{name}: size {len(data)} is not a HuCard image (must be a multiple of 8 KB)")
    if len(data) > 0x280000:
        raise ConvertError(f"{name}: {len(data)} bytes is larger than any HuCard (CD images are not supported yet)")
    if name.lower().endswith(".sgx"):
        raise ConvertError(f"{name}: SuperGrafx games are not supported yet")
    if reset_vector(data) < 0xE000:
        flipped = data.translate(_REV)
        if reset_vector(flipped) >= 0xE000:
            data = flipped
            notes.append("undid US bit-reversed dump")
        else:
            raise ConvertError(f"{name}: no valid reset vector; this does not look like a PC Engine HuCard image")
    mapper = MAPPER_SF2 if len(data) > 0x100000 else MAPPER_NORMAL
    if mapper == MAPPER_SF2:
        notes.append("Street Fighter II mapper")
    return data, mapper, notes


def guess_region(name):
    n = name.lower()
    if re.search(r"\((usa|u|us|world)[),]", n) or "turbografx" in n:
        return "us"
    return "jp"


def clean_title(name):
    t = os.path.splitext(name)[0]
    t = re.sub(r"\s*[\(\[].*?[\)\]]", "", t).strip()
    return t or "PC Engine game"


def make_blob(name, data, region="auto", sprite_limit=True, title=None, multitap=True):
    rom, mapper, notes = normalize(name, data)
    if region == "auto":
        region = guess_region(name)
    flags = GF_REGION_US if region == "us" else 0
    if not sprite_limit:
        flags |= GF_NO_SPRLIMIT
    # the multitap lets N64 pads 2-4 play; Youkai Douchuuki hangs with one plugged in (as on hardware)
    if not multitap or re.search(r"y(o|ou)kai\s*d(o|ou)ch", name, re.I):
        flags |= GF_NO_MULTITAP
    title = (title or clean_title(name))
    tb = title.encode("ascii", "replace")[:31]
    hdr = MAGIC + struct.pack(">IIIIII", VERSION, len(rom), flags, mapper, zlib.crc32(rom) & 0xFFFFFFFF, 0)
    hdr += tb.ljust(32, b"\0")
    assert len(hdr) == HDR_SIZE
    info = dict(title=title, size=len(rom), mapper=mapper, region=region, notes=notes)
    return hdr + rom, info


def build_rom(template, blob, title):
    if template[:4] != b"\x80\x37\x12\x40":
        raise ConvertError("template is not a big-endian .z64 N64 ROM")
    if not template[0x20:0x40].startswith(TEMPLATE_TITLE):
        raise ConvertError("template does not look like a PCE64 template ROM")
    if MAGIC in template[BLOB_MIN_OFFSET:]:
        raise ConvertError("template already contains a game; use the empty PCE64 template")
    off = max(len(template), BLOB_MIN_OFFSET)
    off = (off + BLOB_ALIGN - 1) // BLOB_ALIGN * BLOB_ALIGN
    out = bytearray(template)
    out += b"\xFF" * (off - len(out))
    out += blob
    # round the ROM up to a multiple of 64 KB, which some flashcarts expect
    pad = (-len(out)) % BLOB_ALIGN
    out += b"\xFF" * pad
    if len(out) > MAX_N64_ROM:
        raise ConvertError("result is larger than 64 MB")
    # N64 header title (20 bytes, space padded) for flashcart menus
    t = ("PCE64 " + title).encode("ascii", "replace")[:20].ljust(20, b" ")
    out[0x20:0x34] = t
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description="PCE64 v0.1: combine the PCE64 template ROM with your own PC Engine HuCard image.")
    ap.add_argument("template", nargs="?", help="PCE64_template.z64 (omit with --blob)")
    ap.add_argument("game", help="your .pce (or a .zip containing one)")
    ap.add_argument("output", help="output .z64 (or blob with --blob)")
    ap.add_argument("--region", choices=["auto", "us", "jp"], default="auto",
                    help="console region the game sees (auto: from the file name; default Japan)")
    ap.add_argument("--no-sprite-limit", action="store_true",
                    help="draw all sprites on a line (less flicker, not what the hardware does)")
    ap.add_argument("--no-multitap", action="store_true",
                    help="one controller only, no TurboTap (for a game that misbehaves with one)")
    ap.add_argument("--title", help="title shown in menus")
    ap.add_argument("--blob", action="store_true", help="write only the game blob (for the PC test harness)")
    a = ap.parse_args(argv)
    try:
        name, data = read_game(a.game)
        blob, info = make_blob(name, data, a.region, not a.no_sprite_limit, a.title, not a.no_multitap)
        if a.blob:
            out = blob
        else:
            if not a.template:
                raise ConvertError("missing template ROM")
            with open(a.template, "rb") as f:
                out = build_rom(f.read(), blob, info["title"])
        with open(a.output, "wb") as f:
            f.write(out)
    except ConvertError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    extra = ("; " + "; ".join(info["notes"])) if info["notes"] else ""
    print(f"{info['title']}: {info['size'] // 1024} KB, region {info['region'].upper()}{extra} -> {a.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
