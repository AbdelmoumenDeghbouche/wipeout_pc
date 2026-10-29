"""Menu changes the game reads in its own archives, written for the runtime (GOW3_FILE_PATCHES,
src/runtime_file.c); the game files are not changed.

WipEout's Eliminator target slider (Racebox) stops at 600 points (#89), a limit of the menu file
only: data09.psarc's racebox_definition.xml, stored uncompressed, says maxSlide="600". It becomes
--limit (2000 by default; the launcher's Game page) in the same number of bytes: each extra digit
takes one space of the line's indentation.
Output: out/file_patches.txt, "guest-path offset original new" per line (hex); empty when no
archive matches.
"""
import argparse
import hashlib
from pathlib import Path
import struct

ARCHIVE = 'data09.psarc'
MENU = 'data/plugins/frontend/gui/racebox_definition.xml'
SLIDER = b'<Slider name="Eliminations"'
LIMIT = b'maxSlide="600"'


def stored_entry(path, name):
    """(offset, bytes) of an uncompressed archive entry, or None."""
    with open(path, 'rb') as f:
        magic, _major, _minor, _comp, toc_len, size, count, block, _flags = struct.unpack('>4sHH4sIIIII', f.read(32))
        if magic != b'PSAR':
            return None
        toc = f.read(toc_len - 32)
        digest = hashlib.md5(name.upper().encode()).digest()
        entry = next((toc[i * size:(i + 1) * size] for i in range(count) if toc[i * size:i * size + 16] == digest), None)
        if not entry:
            return None
        first = struct.unpack('>I', entry[16:20])[0]
        length, offset = int.from_bytes(entry[20:25], 'big'), int.from_bytes(entry[25:30], 'big')
        sizes = toc[count * size:]
        for i in range((length + block - 1) // block):  # 2-byte block sizes; 0 = a full block
            stored = int.from_bytes(sizes[2 * (first + i):2 * (first + i) + 2], 'big') or block
            if stored != min(block, length - i * block):
                return None  # compressed
        f.seek(offset)
        return offset, f.read(length)


def eliminator_patches(game, limit=2000):
    archive = next((p for p in Path(game).iterdir() if p.name.lower() == ARCHIVE), None)
    found = archive and stored_entry(archive, MENU)
    if not found:
        return []
    offset, data = found
    new_limit = f'maxSlide="{limit}"'.encode()
    extra = len(new_limit) - len(LIMIT)
    if limit == 600 or extra < 0:
        return []
    patches = []
    at = data.find(SLIDER)
    while at >= 0:
        indent = data.rfind(b'\n', 0, at) + 1
        end = data.find(LIMIT, at)
        if indent + extra <= at and not data[indent:indent + extra].strip(b' \t') and 0 < end - at < 400:
            end += len(LIMIT)
            original = data[indent:end]
            new = original[extra:].replace(LIMIT, new_limit)
            patches.append((f'/app0/{archive.name}', offset + indent, original, new))
        at = data.find(SLIDER, at + 1)
    return patches


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--limit', type=int, default=2000, help='Eliminator target slider maximum (600 and up)')
    a = p.parse_args()
    patches = eliminator_patches(a.game, a.limit)
    (a.out / 'file_patches.txt').write_text(''.join(f'{g} {o:x} {old.hex()} {new.hex()}\n' for g, o, old, new in patches))
    if patches:
        print(f'Menu: Eliminator target up to {a.limit} ({len(patches)} slider{"s" if len(patches) > 1 else ""})', flush=True)


if __name__ == '__main__':
    main()
