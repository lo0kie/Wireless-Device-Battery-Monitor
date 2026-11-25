#!/usr/bin/env python
"""Cuts the embedded Material Icons font down to the glyphs the app actually uses.

The upstream font carries ~2100 glyphs; BatteryHub draws 36 of them. Embedding the whole file costs
about 330 KB in the executable, so the font that goes into the resource is a subset of it.

The codepoints are read from the kMaterial table in src/icon_font.cpp, which is the single place that
says which glyphs exist — adding an icon there and re-running this script is all it takes.

    python scripts/subset-icon-font.py

Source: res/fonts/MaterialIconsOutlined-Regular.otf (upstream, Apache-2.0)
Output: res/MaterialIconsOutlined-Regular.otf (subset, what app.rc embeds)
"""
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, 'src', 'icon_font.cpp')
SOURCE = os.path.join(ROOT, 'res', 'fonts', 'MaterialIconsOutlined-Regular.otf')
TARGET = os.path.join(ROOT, 'res', 'MaterialIconsOutlined-Regular.otf')


def material_codepoints():
    text = io.open(TABLE, encoding='utf-8').read()
    block = text[text.index('kMaterial = {'):]
    block = block[:block.index('};')]
    return sorted({int(m, 16) for m in re.findall(r'0x([0-9A-Fa-f]{4})', block)})


def main():
    if not os.path.exists(SOURCE):
        sys.exit('missing upstream font: %s' % SOURCE)
    points = material_codepoints()
    if not points:
        sys.exit('no codepoints found in %s' % TABLE)
    print('%d glyphs in use' % len(points))
    cmd = [
        sys.executable, '-m', 'fontTools.subset', SOURCE,
        '--unicodes=' + ','.join('U+%04X' % c for c in points),
        '--output-file=' + TARGET,
        # GDI rasterises the outlines itself; hinting, layout features and the DSIG are dead weight.
        '--layout-features=', '--no-hinting', '--drop-tables+=DSIG',
        '--name-IDs=*', '--notdef-glyph', '--notdef-outline', '--recommended-glyphs',
        '--glyph-names', '--legacy-cmap', '--symbol-cmap',
    ]
    subprocess.check_call(cmd)
    before, after = os.path.getsize(SOURCE), os.path.getsize(TARGET)
    print('%s: %d -> %d bytes (%.1f%%)' % (os.path.basename(TARGET), before, after, 100.0 * after / before))


if __name__ == '__main__':
    main()
