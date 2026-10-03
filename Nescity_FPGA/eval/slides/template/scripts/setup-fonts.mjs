import { access, copyFile, mkdir } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const help = 'Usage: node scripts/setup-fonts.mjs /path/to/LINESeedJP_20241105';
const sourceArg = process.argv[2];
if (!sourceArg || process.argv.length !== 3) {
  console.error(help);
  process.exit(1);
}
const source = resolve(sourceArg);
const destination = fileURLToPath(new URL('../fonts/', import.meta.url));
const files = ['Rg', 'Bd', 'Eb'].flatMap(weight => [
  [`Desktop/OTF/LINESeedJP_OTF_${weight}.otf`, `LINESeedJP_OTF_${weight}.otf`],
  [`Web/WOFF2/LINESeedJP_OTF_${weight}.woff2`, `LINESeedJP_OTF_${weight}.woff2`],
]);
files.push(['OFL.txt', 'OFL.txt']);

// Check every source before changing the destination. No downloads are made.
const missing = [];
for (const [relative] of files) {
  try { await access(join(source, relative)); }
  catch { missing.push(relative); }
}
if (missing.length) {
  console.error(`Font files not found in ${source}:\n${missing.join('\n')}\n\nExtract the official LINE Seed JP download and select the folder containing Desktop, Web, and OFL.txt.\n${help}`);
  process.exit(1);
}
await mkdir(destination, { recursive: true });
for (const [from, to] of files) await copyFile(join(source, from), join(destination, to));
console.log('Copied 6 font files and OFL.txt into fonts/. Font files are ignored by Git.');
console.log('Next: npm --prefix marp ci && npm --prefix marp run build');
console.log('Beamer: generate metrics with scripts/setup-uptex-fonts.py (see README), then cd beamer && latexmk slides.tex');
console.log('Typst: node scripts/build-typst.mjs');
