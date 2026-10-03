"""Generate local LY1 metrics for LINE Seed JP; requires fonttools and TeX Live."""
from pathlib import Path
import re
import subprocess
import sys

try:
    from fontTools.ttLib import TTFont
    from fontTools.agl import toUnicode
    from fontTools.pens.boundsPen import BoundsPen
except ImportError:
    sys.exit('Install fonttools first: python3 -m pip install fonttools')

root = Path(__file__).resolve().parents[1]
fonts = root / 'fonts'
enc_path = subprocess.check_output(['kpsewhich', 'texnansi.enc'], text=True).strip()
if not enc_path:
    sys.exit('texnansi.enc is missing. Install the TeX Live ly1 package.')
source = re.sub(r'%[^\n]*', '', Path(enc_path).read_text())
names = re.findall(r'/([^\s/]+)', source.split('[', 1)[1].split(']', 1)[0])
assert len(names) == 256
for weight in ('Rg', 'Bd', 'Eb'):
    if not (fonts / f'LINESeedJP_OTF_{weight}.otf').is_file():
        sys.exit('Run scripts/setup-fonts.mjs first; see README.md.')

for weight in ('Rg', 'Bd', 'Eb'):
    with TTFont(fonts / f'LINESeedJP_OTF_{weight}.otf') as font:
        cmap, units = font.getBestCmap(), font['head'].unitsPerEm
        glyphs = font.getGlyphSet()
        space = font['hmtx'][cmap[32]][0] / units
        lines = ['(DESIGNSIZE R 10.0)', '(CODINGSCHEME TeXnANSI)',
                 f'(FONTDIMEN (SPACE R {space}) (STRETCH R {space/2}) '
                 f'(SHRINK R {space/3}) (XHEIGHT R {font["OS/2"].sxHeight/units}) '
                 '(QUAD R 1.0))']
        cidmap = []
        for code, name in enumerate(names):
            char = toUnicode(name)
            if len(char) != 1 or ord(char) not in cmap:
                continue
            glyph = cmap[ord(char)]
            cidmap.append(f'<{code:02X}> {int(glyph.removeprefix("cid"))}')
            pen = BoundsPen(glyphs)
            glyphs[glyph].draw(pen)
            bounds = pen.bounds or (0, 0, 0, 0)
            lines.append(f'(CHARACTER D {code} (CHARWD R {font["hmtx"][glyph][0]/units}) '
                         f'(CHARHT R {max(0,bounds[3])/units}) '
                         f'(CHARDP R {max(0,-bounds[1])/units}))')
        base = fonts / f'seed-{weight.lower()}'
        pl = base.with_suffix('.pl')
        pl.write_text('\n'.join(lines) + '\n')
        subprocess.run(['pltotf', str(pl), str(base.with_suffix('.tfm'))], check=True)
        registry, ordering, supplement = font['CFF '].cff.topDictIndex[0].ROS
        header = f'''%!PS-Adobe-3.0 Resource-CMap
/CIDInit /ProcSet findresource begin
12 dict begin begincmap
/CIDSystemInfo << /Registry ({registry}) /Ordering ({ordering}) /Supplement {supplement} >> def
/CMapName /Seed{weight} def /CMapType 1 def
1 begincodespacerange <00> <FF> endcodespacerange
'''
        for offset in range(0, len(cidmap), 100):
            chunk = cidmap[offset:offset+100]
            header += f'{len(chunk)} begincidchar\n' + '\n'.join(chunk) + '\nendcidchar\n'
        header += 'endcmap CMapName currentdict /CMap defineresource pop end end\n'
        (fonts / f'seed-{weight.lower()}-cmap').write_text(header)
        pl.unlink()
print('Generated LINE Seed JP metrics in fonts/. Build: cd beamer && latexmk slides.tex')
