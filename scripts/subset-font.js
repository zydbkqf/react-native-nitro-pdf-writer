#!/usr/bin/env node
/**
 * Subset a TTF/WOFF/WOFF2 font down to the glyphs needed for a given
 * set of UTF-8 characters, via the `subset-font` (harfbuzz) package.
 *
 * Usage:
 *   npm run subset-font -- -i input.ttf --text "中文✓"
 *   npm run subset-font -- -i input.woff2 -o out/font.ttf --unicodes U+2713,U+4E2D
 *   npm run subset-font -- -i input.ttf --text-file chars.txt
 *
 * Output is always TrueType (.ttf). When -o is omitted the output path is
 * the input path with its extension replaced by `-output.ttf`.
 */
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import subsetFont from 'subset-font';

const USAGE = `Usage: subset-font -i <input> [glyph sources] [-o <output>]

Glyph sources (at least one required, may be combined):
  --text <string>          Characters to keep, e.g. --text "中文✓"
  --text-file <path>       UTF-8 text file whose characters should be kept
  --unicodes <list>        Comma-separated code points (hex), e.g. U+2713,U+4E2D or 2713,4E2D

Options:
  -i, --input <path>       Input font (.ttf / .otf / .woff / .woff2)
  -o, --output <path>      Output .ttf path (default: <input>-output.ttf)
  -h, --help               Show this help

Output is always TrueType (.ttf).

Examples:
  npm run npw-subset-font -- -i src/NotoSans.ttf --text "你好世界"
  npm run npw-subset-font -- -i src/font.woff2 -o out/font.ttf --unicodes U+2713,U+4E2D
  npm run npw-subset-font -- -i src/font.ttf --text-file chars.txt --text "ABC"
`;

function fail(message, code = 1) {
  console.error(`[subset-font] error: ${message}`);
  process.exit(code);
}

function parseCli() {
  let values;
  let positionals;
  try {
    ({ values, positionals } = parseArgs({
      options: {
        input: { type: 'string', short: 'i' },
        output: { type: 'string', short: 'o' },
        text: { type: 'string' },
        'text-file': { type: 'string' },
        unicodes: { type: 'string' },
        help: { type: 'boolean', short: 'h' },
      },
      allowPositionals: true,
    }));
  } catch (err) {
    fail(`${err.message}\n\n${USAGE}`);
  }

  if (values.help) {
    console.log(USAGE);
    process.exit(0);
  }

  if (positionals.length > 0) {
    fail(`unexpected argument(s): ${positionals.join(' ')}\n\n${USAGE}`);
  }

  if (!values.input) fail(`missing required -i/--input\n\n${USAGE}`);
  if (!values.text && !values['text-file'] && !values.unicodes) {
    fail(`at least one of --text, --text-file, or --unicodes is required\n\n${USAGE}`);
  }

  return values;
}

/** Default output path: replace the input extension with `-output.ttf`. */
function defaultOutputPath(inputPath) {
  const ext = path.extname(inputPath);
  const base = ext ? inputPath.slice(0, -ext.length) : inputPath;
  return `${base}-output.ttf`;
}

/**
 * Parse a single code-point token.
 *
 * Accepted forms:
 *   U+2713 / u+2713   — explicit hex code point
 *   0x2713            — explicit hex code point
 *   2713 / 4E2D       — bare hex (2+ digits, fonttools / hb-subset convention)
 *   A / ✓ / 中         — a literal single character (use --text for full strings)
 */
function parseCodePoint(token) {
  const t = token.trim();
  if (!t) return null;

  let cp;
  if (/^u\+[0-9a-f]+$/i.test(t)) {
    cp = parseInt(t.slice(2), 16);
  } else if (/^0x[0-9a-f]+$/i.test(t)) {
    cp = parseInt(t.slice(2), 16);
  } else if (/^[0-9a-f]{2,6}$/i.test(t)) {
    // bare hex, e.g. "2713", "4E2D", "41"
    cp = parseInt(t, 16);
  } else {
    // a literal character (possibly multi-byte / surrogate pair)
    const cps = [...t];
    if (cps.length !== 1) {
      throw new Error(`not a code point or single character: "${token}"`);
    }
    cp = cps[0].codePointAt(0);
  }

  if (!Number.isInteger(cp) || cp < 0 || cp > 0x10ffff) {
    throw new Error(`invalid code point: "${token}"`);
  }
  return cp;
}

/** Build the set of characters to keep from all provided sources. */
function collectText(values) {
  const codePoints = new Set();

  const addString = (str, source) => {
    for (const ch of str) {
      codePoints.add(ch.codePointAt(0));
    }
    if (str.length === 0) {
      console.warn(`[subset-font] warning: ${source} contributed no characters`);
    }
  };

  if (values.text != null && values.text !== '') {
    addString(values.text, '--text');
  }

  if (values['text-file']) {
    const file = values['text-file'];
    let content;
    try {
      content = fs.readFileSync(file, 'utf8');
    } catch (err) {
      fail(`cannot read --text-file ${file}: ${err.message}`);
    }
    addString(content, `--text-file ${file}`);
  }

  if (values.unicodes) {
    for (const raw of values.unicodes.split(',')) {
      const token = raw.trim();
      if (!token) continue;
      try {
        codePoints.add(parseCodePoint(token));
      } catch (err) {
        fail(`--unicodes: ${err.message}`);
      }
    }
  }

  if (codePoints.size === 0) {
    fail('no characters selected; provide non-empty --text / --text-file / --unicodes');
  }

  return [...codePoints].map((cp) => String.fromCodePoint(cp)).join('');
}

const OTTO = 0x4f54544f; // 'OTTO' — CFF-flavoured OpenType
const TRUE = 0x74727565; // 'true' — legacy Mac TrueType
const TTCF = 0x74746366; // 'ttcf'
const WOFF = 0x774f4646; // 'wOFF'
const WOFF2 = 0x774f4632; // 'wOF2'
const TYPED = 0x00010000; // standard TrueType sfnt version

/** WOFF2 known-tag table (spec §5.1), index 0–62. Index 63 = custom tag follows. */
const WOFF2_KNOWN_TAGS = [
  'cmap', 'head', 'hhea', 'hmtx', 'maxp', 'name', 'OS/2', 'post',
  'cvt ', 'fpgm', 'glyf', 'loca', 'prep', 'CFF ', 'VORG', 'EBDT',
  'EBLC', 'gasp', 'hdmx', 'kern', 'LTSH', 'PCLT', 'VDMX', 'vhea',
  'vmtx', 'BASE', 'GDEF', 'GPOS', 'GSUB', 'EBSC', 'JSTF', 'MATH',
  'CBDT', 'CBLC', 'COLR', 'CPAL', 'SVG ', 'sbix', 'acnt', 'avar',
  'bdat', 'bloc', 'bsln', 'cvar', 'fdsc', 'feat', 'fmtx', 'fvar',
  'gvar', 'hsty', 'just', 'lcar', 'mort', 'morx', 'opbd', 'prop',
  'trak', 'Zapf', 'Silf', 'Glat', 'Gloc', 'Feat', 'Sill',
];

function readU32(buf, offset) {
  return buf.readUInt32BE(offset);
}

function readU16(buf, offset) {
  return buf.readUInt16BE(offset);
}

function tagToString(tag) {
  return String.fromCharCode((tag >>> 24) & 0xff, (tag >>> 16) & 0xff, (tag >>> 8) & 0xff, tag & 0xff);
}

/** Decode a WOFF2 UIntBase128 (spec §5.2). Returns [value, nextOffset]. */
function readUIntBase128(buf, offset) {
  let value = 0;
  for (let i = 0; i < 5; i += 1) {
    if (offset >= buf.length) throw new Error('truncated UIntBase128');
    const byte = buf[offset];
    offset += 1;
    // leading zero is invalid
    if (i === 0 && byte === 0x80) throw new Error('invalid UIntBase128 leading zero');
    value = (value << 7) | (byte & 0x7f);
    if ((byte & 0x80) === 0) {
      if (value > 0xffffffff) throw new Error('UIntBase128 overflow');
      return [value, offset];
    }
  }
  throw new Error('UIntBase128 too long');
}

/** Return the set of SFNT table tags present in the buffer. */
function readTableTags(buf) {
  if (buf.length < 12) throw new Error('font too small');

  const signature = readU32(buf, 0);

  // --- plain SFNT / TTC ---
  if (signature === TYPED || signature === TRUE || signature === OTTO) {
    return readSfntTableTags(buf, 0);
  }
  if (signature === TTCF) {
    // TrueType Collection: take the tags of the first face.
    if (buf.length < 16) throw new Error('truncated TTC header');
    const firstOffset = readU32(buf, 12);
    return readSfntTableTags(buf, firstOffset);
  }

  // --- WOFF ---
  if (signature === WOFF) {
    return readWoffTableTags(buf);
  }

  // --- WOFF2 ---
  if (signature === WOFF2) {
    return readWoff2TableTags(buf);
  }

  throw new Error(`unrecognised font format (signature 0x${signature.toString(16)})`);
}

function readSfntTableTags(buf, dirOffset) {
  if (dirOffset + 12 > buf.length) throw new Error('truncated SFNT table directory');
  const numTables = readU16(buf, dirOffset + 4);
  const tags = new Set();
  for (let i = 0; i < numTables; i += 1) {
    const rec = dirOffset + 12 + i * 16;
    if (rec + 4 > buf.length) throw new Error('truncated SFNT table record');
    tags.add(tagToString(readU32(buf, rec)));
  }
  return tags;
}

function readWoffTableTags(buf) {
  // WOFF header: signature(4) flavor(4) length(4) numTables(2) reserved(2) ...
  const numTables = readU16(buf, 12);
  const tags = new Set();
  // Table directory starts at offset 44; each entry is 20 bytes.
  for (let i = 0; i < numTables; i += 1) {
    const rec = 44 + i * 20;
    if (rec + 4 > buf.length) throw new Error('truncated WOFF table record');
    tags.add(tagToString(readU32(buf, rec)));
  }
  return tags;
}

function readWoff2TableTags(buf) {
  // WOFF2 header: signature(4) flavor(4) length(4) numTables(2) reserved(2)
  //               totalSfntSize(4) totalCompressedSize(4) major/minor(4)
  //               metaOffset(4) metaLength(4) metaOrigLength(4)
  //               privOffset(4) privLength(4)  → 48 bytes total.
  const numTables = readU16(buf, 12);
  let offset = 48;
  const tags = new Set();
  for (let i = 0; i < numTables; i += 1) {
    if (offset >= buf.length) throw new Error('truncated WOFF2 table directory');
    const flags = buf[offset];
    offset += 1;
    const tagIndex = flags & 0x3f;
    let tag;
    if (tagIndex === 0x3f) {
      if (offset + 4 > buf.length) throw new Error('truncated WOFF2 custom tag');
      tag = tagToString(readU32(buf, offset));
      offset += 4;
    } else {
      tag = WOFF2_KNOWN_TAGS[tagIndex];
      if (!tag) throw new Error(`unknown WOFF2 tag index ${tagIndex}`);
    }
    tags.add(tag);

    // origLength (UIntBase128)
    let value;
    [value, offset] = readUIntBase128(buf, offset);
    // transformLength is present iff the table is transformed (WOFF2 §5.1):
    //   glyf/loca: version 0 = transformed, version 3 = null transform
    //   all others: version 0 or 3 = null transform, 1/2 = transformed
    const transformVersion = (flags >> 6) & 0x03;
    const isGlyfOrLoca = tagIndex === 10 || tagIndex === 11;
    const isTransformed = isGlyfOrLoca ? transformVersion !== 3 : transformVersion !== 0 && transformVersion !== 3;
    if (isTransformed) {
      [value, offset] = readUIntBase128(buf, offset);
    }
  }
  return tags;
}

/**
 * Ensure the font uses TrueType (glyf) outlines, not CFF.
 * libharu — which this package wraps — cannot embed CFF-flavoured fonts.
 */
function assertGlyfFlavor(buf) {
  let tags;
  try {
    tags = readTableTags(buf);
  } catch (err) {
    fail(`cannot inspect font tables: ${err.message}`);
  }

  const hasGlyf = tags.has('glyf');
  const hasCff = tags.has('CFF ') || tags.has('CFF2');

  if (hasCff && !hasGlyf) {
    fail(
      'input font uses CFF outlines (glyf-flavor check failed).\n' +
        '  This tool only produces TrueType (glyf) subsets, which is also the\n' +
        '  only outline format libharu / react-native-nitro-pdf-writer can embed.\n' +
        '  Convert the font to TrueType outlines first (e.g. fonttools:\n' +
        '  `fonttools ttLib.woff2 decompress` + `fonttools cu2qu` / otf2ttf),\n' +
        '  or use a TTF/WOFF/WOFF2 face that already has glyf outlines.',
    );
  }

  if (!hasGlyf && !hasCff) {
    fail('font has neither glyf nor CFF outlines — cannot determine glyf-flavor');
  }

  if (hasGlyf && hasCff) {
    console.warn('[subset-font] warning: font contains both glyf and CFF tables; treating as TrueType');
  }
}

async function main() {
  const values = parseCli();

  const inputPath = values.input;
  const outputPath = values.output || defaultOutputPath(inputPath);
  let inputBuf;
  try {
    inputBuf = fs.readFileSync(inputPath);
  } catch (err) {
    fail(`cannot read input ${inputPath}: ${err.message}`);
  }

  assertGlyfFlavor(inputBuf);

  const text = collectText(values);

  console.log(
    `[subset-font] ${inputPath} → ${outputPath} ` +
      `(ttf, ${[...text].length} character(s))`,
  );

  let subsetBuf;
  try {
    subsetBuf = await subsetFont(inputBuf, text, { targetFormat: 'sfnt' });
  } catch (err) {
    fail(`subsetting failed: ${err.message}`);
  }

  try {
    fs.mkdirSync(path.dirname(path.resolve(outputPath)), { recursive: true });
    fs.writeFileSync(outputPath, subsetBuf);
  } catch (err) {
    fail(`cannot write output ${outputPath}: ${err.message}`);
  }

  const before = inputBuf.length;
  const after = subsetBuf.length;
  const pct = before > 0 ? (Math.abs(1 - after / before) * 100).toFixed(1) : '0.0';
  const direction = after <= before ? 'smaller' : 'larger';
  console.log(`[subset-font] done: ${before} → ${after} bytes (${pct}% ${direction})`);
}

main().catch((err) => {
  fail(err?.stack || String(err));
});
