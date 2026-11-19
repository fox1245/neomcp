#!/usr/bin/env node
// Generates test cases for the differential test of the regular expression translator, with V8 as the reference.
//
//   node generate.js --seed 1 --count 20000 --out cases-1.json    patterns drawn from a grammar, with V8's answers
//   node generate.js --audit-properties --out audit.json          every general category against the stable characters
//   node generate.js --check ../data/pattern_cases.json           the expectations of a hand-written file against V8
//
// A case is a pattern and either "syntax_error": true (V8 refuses `new RegExp(pattern, "u")`) or a list of texts with
// V8's answer to `.test(text)`. The same file format holds the hand-written cases of tests/data/pattern_cases.json.
//
// No packages and no Math.random: the generator is deterministic for a seed, with a small generator of its own. V8
// backtracks, so every pattern runs inside a vm context with a timeout (the `timeout` option of vm.Script
// .runInContext, which ends script execution, regular expression matching included, from a watchdog thread). A
// pattern whose texts are not all answered in time is dropped and counted. A text is searched for the way ECMA-262
// does it, a sticky match at each code point boundary (see Reference).
'use strict';

const fs = require('fs');
const vm = require('vm');

// ---- Random numbers: SFC32, seeded through SplitMix32.

function makeRandom(seed) {
  let state = seed | 0;
  const splitmix = () => {
    state = (state + 0x9e3779b9) | 0;
    let z = state;
    z = Math.imul(z ^ (z >>> 16), 0x85ebca6b);
    z = Math.imul(z ^ (z >>> 13), 0xc2b2ae35);
    return (z ^ (z >>> 16)) >>> 0;
  };
  let a = splitmix(), b = splitmix(), c = splitmix(), d = splitmix();
  const u32 = () => {
    const t = (((a + b) | 0) + d) | 0;
    d = (d + 1) | 0;
    a = b ^ (b >>> 9);
    b = (c + (c << 3)) | 0;
    c = (c << 21) | (c >>> 11);
    c = (c + t) | 0;
    return t >>> 0;
  };
  for (let i = 0; i < 16; i++) u32();
  const rng = {
    u32,
    int: (n) => u32() % n,
    range: (lo, hi) => lo + (u32() % (hi - lo + 1)),
    chance: (p) => u32() / 4294967296 < p,
    pick: (list) => list[u32() % list.length],
    // weighted([[weight, value], ...])
    weighted: (entries) => {
      let total = 0;
      for (const entry of entries) total += entry[0];
      let at = u32() % total;
      for (const entry of entries) {
        if (at < entry[0]) return entry[1];
        at -= entry[0];
      }
      return entries[entries.length - 1][1];
    },
  };
  return rng;
}

// ---- Facts about characters.

const MAX_NESTING = 4;
const MAX_TEXT_CODE_POINTS = 24;
const MAX_PATTERN_BYTES = 300;
const TEXTS_PER_PATTERN = 14;

const SYNTAX_CHARACTERS = new Set(Array.from('^$\\.*+?()[]{}|').map((c) => c.codePointAt(0)));
const hex = (n, width) => n.toString(16).padStart(width, '0');
const chr = (cp) => String.fromCodePoint(cp);
const cps = (text) => Array.from(text, (c) => c.codePointAt(0));
const str = (list) => String.fromCodePoint(...list);
const isSurrogate = (cp) => cp >= 0xd800 && cp <= 0xdfff;

// ECMA-262: \d, \w, \s and the line terminators, written from the specification and not from the translator.
const isDigit = (cp) => cp >= 0x30 && cp <= 0x39;
const isWord = (cp) => isDigit(cp) || (cp >= 0x41 && cp <= 0x5a) || (cp >= 0x61 && cp <= 0x7a) || cp === 0x5f;
const isLineTerminator = (cp) => cp === 0x0a || cp === 0x0d || cp === 0x2028 || cp === 0x2029;
const isSpace = (cp) =>
  isLineTerminator(cp) || cp === 0x09 || cp === 0x0b || cp === 0x0c || cp === 0x20 || cp === 0xa0 || cp === 0xfeff ||
  cp === 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp === 0x202f || cp === 0x205f || cp === 0x3000;
// The characters around the edges of those sets.
const SET_EDGES = {
  d: [0x2f, 0x30, 0x39, 0x3a],
  w: [0x2f, 0x30, 0x39, 0x3a, 0x40, 0x41, 0x5a, 0x5b, 0x5e, 0x5f, 0x60, 0x61, 0x7a, 0x7b],
  s: [0x08, 0x09, 0x0d, 0x0e, 0x1f, 0x20, 0x21, 0x9f, 0xa0, 0xa1, 0x167f, 0x1680, 0x1681, 0x1fff, 0x2000, 0x200a,
      0x200b, 0x2027, 0x2028, 0x2029, 0x202a, 0x202e, 0x202f, 0x2030, 0x205e, 0x205f, 0x2060, 0x2fff, 0x3000, 0x3001,
      0xfefe, 0xfeff, 0xff00],
};

// Characters where the dialects of regular expressions differ.
const POOL = [
  0x0a, 0x0d, 0x2028, 0x2029, 0x09, 0x0b, 0x0c, 0xa0, 0xfeff, 0x1680, 0x2000, 0x3000, 0x85, 0x180e, 0x00, 0x7f,
  0x663, 0x3b1, 0x436, 0x17f, 0x212a, 0x1f600, 0x301, 0x20, 0x5f, 0x2d, 0x61, 0x62, 0x41, 0x30, 0x39,
];

// General categories: [short name, long name, alias, code points of the category that were assigned long ago and
// have the same category in every release since]. The major categories are the unions of their parts.
const MINOR = [
  ['Lu', 'Uppercase_Letter', '', [0x41, 0x5a, 0xc9, 0x3a3, 0x416]],
  ['Ll', 'Lowercase_Letter', '', [0x61, 0x7a, 0xe9, 0xdf, 0x3c3, 0x436, 0x17f]],
  ['Lt', 'Titlecase_Letter', '', [0x1c5, 0x1c8, 0x1f88]],
  ['Lm', 'Modifier_Letter', '', [0x2b0, 0x2b2, 0x3005, 0x30fc]],
  ['Lo', 'Other_Letter', '', [0xaa, 0x5d0, 0x65e5, 0x3042, 0x10000]],
  ['Mn', 'Nonspacing_Mark', '', [0x300, 0x301, 0x483]],
  ['Mc', 'Spacing_Mark', '', [0x903, 0x93e]],
  ['Me', 'Enclosing_Mark', '', [0x20dd, 0x488]],
  ['Nd', 'Decimal_Number', 'digit', [0x30, 0x39, 0x663, 0x966]],
  ['Nl', 'Letter_Number', '', [0x2160, 0x2164, 0x3007]],
  ['No', 'Other_Number', '', [0xb2, 0xbd, 0x2460]],
  ['Pc', 'Connector_Punctuation', '', [0x5f, 0x203f]],
  ['Pd', 'Dash_Punctuation', '', [0x2d, 0x2013, 0x2014]],
  ['Ps', 'Open_Punctuation', '', [0x28, 0x5b, 0x7b]],
  ['Pe', 'Close_Punctuation', '', [0x29, 0x5d, 0x7d]],
  ['Pi', 'Initial_Punctuation', '', [0xab, 0x2018, 0x201c]],
  ['Pf', 'Final_Punctuation', '', [0xbb, 0x2019, 0x201d]],
  ['Po', 'Other_Punctuation', '', [0x21, 0x23, 0x25, 0x2a, 0x2c, 0x2e, 0x2f]],
  ['Sm', 'Math_Symbol', '', [0x2b, 0x3c, 0x3d, 0x3e, 0x7c, 0x7e, 0xd7, 0x2211]],
  ['Sc', 'Currency_Symbol', '', [0x24, 0xa2, 0x20ac]],
  ['Sk', 'Modifier_Symbol', '', [0x5e, 0x60, 0xa8, 0xaf]],
  ['So', 'Other_Symbol', '', [0xa9, 0xae, 0x2603, 0x1f600]],
  ['Zs', 'Space_Separator', '', [0x20, 0xa0, 0x2003, 0x3000]],
  ['Zl', 'Line_Separator', '', [0x2028]],
  ['Zp', 'Paragraph_Separator', '', [0x2029]],
  ['Cc', 'Control', 'cntrl', [0x00, 0x09, 0x0a, 0x7f, 0x85]],
  ['Cf', 'Format', '', [0xad, 0x200b, 0x200e, 0x202e, 0xfeff]],
  ['Co', 'Private_Use', '', [0xe000, 0xf8ff, 0xf0000, 0x10fffd]],
];
const MAJOR = [
  ['L', 'Letter', '', ['Lu', 'Ll', 'Lt', 'Lm', 'Lo']],
  ['M', 'Mark', 'Combining_Mark', ['Mn', 'Mc', 'Me']],
  ['N', 'Number', '', ['Nd', 'Nl', 'No']],
  ['P', 'Punctuation', 'punct', ['Pc', 'Pd', 'Ps', 'Pe', 'Pi', 'Pf', 'Po']],
  ['S', 'Symbol', '', ['Sm', 'Sc', 'Sk', 'So']],
  ['Z', 'Separator', '', ['Zs', 'Zl', 'Zp']],
];
const CATEGORIES = MINOR.map(([short, long, alias, chars]) => ({ short, long, alias, chars: new Set(chars) }));
for (const [short, long, alias, parts] of MAJOR) {
  const chars = new Set();
  for (const part of parts) for (const cp of MINOR.find((m) => m[0] === part)[3]) chars.add(cp);
  CATEGORIES.push({ short, long, alias, chars });
}
// Code points whose category is known: both engines are expected to agree on them.
const STABLE = new Set(POOL);
for (const category of CATEGORIES) for (const cp of category.chars) STABLE.add(cp);
const isStable = (cp) => cp < 0x80 || STABLE.has(cp);

// Names of properties that the translator refuses, or that no engine accepts.
const OTHER_PROPERTIES = [
  'C', 'Other', 'Cn', 'Unassigned', 'LC', 'Cased_Letter', 'Script=Greek', 'sc=Latin', 'Script_Extensions=Greek',
  'scx=Grek', 'Alphabetic', 'Any', 'ASCII', 'Assigned', 'Emoji', 'White_Space', 'ID_Start', 'Lowercase', 'Greek',
  'Foo', 'lu', 'L ', '', 'gc=', 'gc=Foo', 'General_Category', 'Lu=Lu', 'gc=C', 'General_Category=Unassigned', 'Cs',
  'Surrogate', 'Zs=', 'ASCII_Hex_Digit', 'Latn',
];

// ---- Literal characters of patterns.

const LITERALS = [
  [30, () => 'abcde'], [8, () => '0123456789'], [3, () => 'ABXZ'], [2, () => ' _-,:=/<>!@#%&~"\''],
  [4, () => [0xe9, 0xdf, 0x3b1, 0x436, 0x65e5, 0x17f, 0x212a, 0x663, 0x301].map(chr).join('')],
  [3, () => [0x1f600, 0x10000, 0x10ffff, 0x1f64f, 0x1d11e].map(chr).join('')],
  [3, () => [0xfeff, 0x2028, 0x2029, 0x0a, 0x0d, 0x09, 0x0b, 0x0c, 0xa0, 0x3000, 0x1680, 0x180e, 0x85, 0x7f].map(chr).join('')],
  [2, () => '^$\\.*+?()[]{}|'],
  [1, () => '\u0000'],
];

function pickLiteral(rng) {
  const set = rng.weighted(LITERALS)();
  return rng.pick(cps(set));
}

function hexCase(rng, text) {
  return rng.chance(0.5) ? text.toUpperCase() : text;
}

// How a character is written in a pattern: raw, or with an escape of any of the forms that ECMA-262 has.
function renderCharacter(rng, cp, inClass) {
  const forms = [];
  if (SYNTAX_CHARACTERS.has(cp)) {
    forms.push([8, () => '\\' + chr(cp)]);
    if (inClass && cp !== 0x5d && cp !== 0x5c && cp !== 0x5e) forms.push([6, () => chr(cp)]);
  } else if (cp === 0x2d) {
    forms.push(inClass ? [12, () => '\\-'] : [12, () => '-']);
    if (inClass) forms.push([1, () => '-']);
  } else {
    forms.push([14, () => chr(cp)]);
    if (cp === 0x2f) forms.push([4, () => '\\/']);
  }
  if (cp === 0x09) forms.push([5, () => '\\t']);
  if (cp === 0x0a) forms.push([5, () => '\\n']);
  if (cp === 0x0b) forms.push([5, () => '\\v']);
  if (cp === 0x0c) forms.push([5, () => '\\f']);
  if (cp === 0x0d) forms.push([5, () => '\\r']);
  if (cp === 0x08 && inClass) forms.push([8, () => '\\b']);
  if (cp >= 1 && cp <= 26) forms.push([4, () => '\\c' + (rng.chance(0.5) ? chr(cp + 64) : chr(cp + 96))]);
  if (cp === 0) forms.push([8, () => '\\0']);
  if (cp < 0x100) forms.push([4, () => '\\x' + hexCase(rng, hex(cp, 2))]);
  if (cp < 0x10000 && !isSurrogate(cp)) forms.push([4, () => '\\u' + hexCase(rng, hex(cp, 4))]);
  forms.push([3, () => '\\u{' + '0'.repeat(rng.chance(0.2) ? rng.range(1, 4) : 0) + hexCase(rng, cp.toString(16)) + '}']);
  if (cp >= 0x10000) {
    const high = 0xd800 + ((cp - 0x10000) >> 10);
    const low = 0xdc00 + ((cp - 0x10000) & 0x3ff);
    forms.push([6, () => '\\u' + hexCase(rng, hex(high, 4)) + '\\u' + hexCase(rng, hex(low, 4))]);
  }
  return rng.weighted(forms)();
}

// ---- Nodes. Each has render() (the pattern text) and sample(rng) (code points of a text that it matches, best effort).
// A node that stands for one character also has has(cp).

function literalNode(G, cp) {
  G.mention(cp);
  return {
    render: () => renderCharacter(G.rng, cp, false),
    sample: () => [cp],
    has: (c) => c === cp,
  };
}

function classEscapeNode(G, letter) {
  const upper = letter === letter.toUpperCase();
  const base = { d: isDigit, w: isWord, s: isSpace }[letter.toLowerCase()];
  const has = upper ? (cp) => !base(cp) : base;
  for (const cp of SET_EDGES[letter.toLowerCase()]) G.mention(cp);
  return { render: () => '\\' + letter, sample: (rng) => G.sampleChar(rng, has), has, escape: true };
}

function categoryOf(rng, category) {
  const forms = [category.short, category.long];
  if (category.alias) forms.push(category.alias);
  const name = rng.pick(forms);
  return rng.weighted([[5, name], [2, 'gc=' + name], [1, 'General_Category=' + name]]);
}

function propertyNode(G, category, negated) {
  G.hasProperty = true;
  for (const cp of category.chars) G.mention(cp);
  const known = new Set();
  for (const c of CATEGORIES) for (const cp of c.chars) known.add(cp);
  const has = negated ? (cp) => known.has(cp) && !category.chars.has(cp) : (cp) => category.chars.has(cp);
  const name = categoryOf(G.rng, category);
  return {
    render: () => (negated ? '\\P{' : '\\p{') + name + '}',
    sample: (rng) => (negated ? G.sampleChar(rng, has) : [rng.pick([...category.chars])]),
    has,
    property: true,
  };
}

function rawNode(text, sampleCodePoints) {
  return { render: () => text, sample: () => sampleCodePoints || [] };
}

function classItem(G, rng) {
  const kind = rng.weighted([[40, 'single'], [26, 'range'], [14, 'escape'], [8, 'property'], [3, 'backspace'], [3, 'dash'], [2, 'syntax']]);
  switch (kind) {
    case 'single': {
      const cp = pickLiteral(rng);
      G.mention(cp);
      return { render: () => renderCharacter(rng, cp, true), has: (c) => c === cp, sample: () => [cp] };
    }
    case 'range': {
      let lo = pickLiteral(rng);
      let hi = rng.chance(0.5) ? lo + rng.range(0, 40) : pickLiteral(rng);
      if (isSurrogate(lo) || isSurrogate(hi) || hi > 0x10ffff) hi = lo;
      if (hi < lo && !rng.chance(0.03)) [lo, hi] = [hi, lo];
      G.mention(lo);
      G.mention(hi);
      G.mention(lo - 1);
      G.mention(hi + 1);
      return {
        render: () => renderCharacter(rng, lo, true) + '-' + renderCharacter(rng, hi, true),
        has: (c) => c >= lo && c <= hi,
        sample: () => [rng.chance(0.4) ? lo : rng.chance(0.5) ? hi : rng.range(lo, Math.max(lo, hi))].filter((c) => !isSurrogate(c)),
        range: true,
      };
    }
    case 'escape':
      return classEscapeNode(G, rng.pick(['d', 'D', 'w', 'W', 's', 'S']));
    case 'property':
      return propertyNode(G, rng.pick(CATEGORIES), rng.chance(0.3));
    case 'backspace':
      G.mention(8);
      return { render: () => '\\b', has: (c) => c === 8, sample: () => [8] };
    case 'dash':
      G.mention(0x2d);
      return { render: () => rng.pick(['-', '\\-']), has: (c) => c === 0x2d, sample: () => [0x2d] };
    default: {
      const cp = rng.pick([...SYNTAX_CHARACTERS].filter((c) => c !== 0x5d && c !== 0x5c && c !== 0x5e));
      G.mention(cp);
      return { render: () => renderCharacter(rng, cp, true), has: (c) => c === cp, sample: () => [cp] };
    }
  }
}

function classNode(G) {
  const rng = G.rng;
  const negated = rng.chance(0.3);
  const empty = rng.chance(0.06);
  const items = [];
  if (!empty) for (let i = rng.weighted([[35, 1], [30, 2], [20, 3], [10, 4], [5, 6]]); i > 0; i--) items.push(classItem(G, rng));
  const has = (cp) => items.some((item) => item.has(cp)) !== negated;
  return {
    render: () => '[' + (negated ? '^' : '') + items.map((item) => item.render()).join('') + ']',
    has,
    sample: (r) => G.sampleChar(r, has),
  };
}

function quantifierText(rng) {
  const lazy = rng.chance(0.25) ? '?' : '';
  const count = () => rng.weighted([[40, rng.range(0, 3)], [8, rng.range(4, 6)], [2, rng.range(7, 40)]]);
  const written = (n) => (rng.chance(0.05) ? '0'.repeat(rng.range(1, 3)) + n : String(n));
  const form = rng.weighted([[18, '*'], [18, '+'], [20, '?'], [10, 'n'], [8, 'n,'], [14, 'n,m'], [4, '0'], [2, '0,0'], [2, '0,'], [2, '1,1']]);
  let min = 0;
  let max = Infinity;
  let text;
  switch (form) {
    case '*': text = '*'; break;
    case '+': text = '+'; min = 1; break;
    case '?': text = '?'; max = 1; break;
    case 'n': min = max = count(); text = '{' + written(min) + '}'; break;
    case 'n,': min = count(); text = '{' + written(min) + ',}'; break;
    case 'n,m': min = count(); max = min + rng.range(0, 4); text = '{' + written(min) + ',' + written(max) + '}'; break;
    case '0': min = max = 0; text = '{0}'; break;
    case '0,0': min = max = 0; text = '{0,0}'; break;
    case '0,': text = '{0,}'; break;
    default: min = max = 1; text = '{1,1}';
  }
  return { text: text + lazy, min, max };
}

function quantifiedNode(G, atom) {
  const q = quantifierText(G.rng);
  return {
    render: () => atom.render() + q.text,
    sample: (rng) => {
      const times = rng.range(q.min, Math.min(q.max, q.min + 3));
      let out = [];
      for (let i = 0; i < times && out.length <= MAX_TEXT_CODE_POINTS; i++) out = out.concat(atom.sample(rng));
      return out;
    },
  };
}

function groupNode(G, depth) {
  const rng = G.rng;
  const kind = rng.weighted([[40, 'plain'], [45, 'noncapturing'], [15, 'named']]);
  const inner = regexNode(G, depth + 1);
  let open = '(';
  if (kind === 'noncapturing') open = '(?:';
  if (kind === 'named') {
    const name = rng.weighted([[6, 'n' + G.names++], [1, '$' + G.names++], [1, '_' + G.names++], [1, 'a1' + G.names++]]);
    open = '(?<' + name + '>';
  }
  return { render: () => open + inner.render() + ')', sample: (r) => inner.sample(r) };
}

function assertionNode(G) {
  const text = G.rng.pick(['^', '$', '\\b', '\\B']);
  return { render: () => text, sample: () => [], assertion: true };
}

function atomNode(G, depth) {
  const rng = G.rng;
  const kind = rng.weighted([
    [30, 'literal'], [7, 'dot'], [9, 'escape'], [14, 'class'], [5, 'property'], [depth < MAX_NESTING ? 15 : 0, 'group'], [9, 'assertion'],
  ]);
  switch (kind) {
    case 'literal': return literalNode(G, pickLiteral(rng));
    case 'dot': {
      const has = (cp) => !isLineTerminator(cp);
      return { render: () => '.', has, sample: (r) => G.sampleChar(r, has) };
    }
    case 'escape': return classEscapeNode(G, rng.pick(['d', 'D', 'w', 'W', 's', 'S']));
    case 'class': return classNode(G);
    case 'property': return propertyNode(G, rng.pick(CATEGORIES), rng.chance(0.3));
    case 'group': return groupNode(G, depth);
    default: return assertionNode(G);
  }
}

function termNode(G, depth) {
  const atom = atomNode(G, depth);
  if (atom.assertion || !G.rng.chance(0.4)) return atom;
  return quantifiedNode(G, atom);
}

function sequenceNode(G, depth) {
  const rng = G.rng;
  const terms = [];
  const count = rng.weighted([[8, 0], [20, 1], [28, 2], [24, 3], [14, 4], [6, 5]]);
  for (let i = 0; i < count && G.size < 220; i++) {
    terms.push(termNode(G, depth));
    G.size += 4;
  }
  const node = {
    terms,
    render: () => terms.map((term) => term.render()).join(''),
    sample: (r) => terms.reduce((out, term) => out.concat(term.sample(r)), []),
  };
  G.sequences.push(node);
  return node;
}

function regexNode(G, depth) {
  const rng = G.rng;
  const branches = [];
  for (let i = rng.weighted([[60, 1], [28, 2], [12, 3]]); i > 0; i--) branches.push(sequenceNode(G, depth));
  return {
    render: () => branches.map((branch) => branch.render()).join('|'),
    sample: (r) => r.pick(branches).sample(r),
  };
}

// ---- Constructs the translator refuses, and near misses of syntax.

const REFUSED = [
  '(?=a)', '(?!a)', '(?<=a)', '(?<!a)', '(?=a)*', 'a(?=b)', '(?<=a)b', '(a)\\1', '(a)(b)\\2', '(?<n>a)\\k<n>', '\\k<n>(?<n>a)',
  '(a)\\1*', '(?i)', '(?i:a)', '(?s:a)', '(?-i:a)', '(?<n>a)|(?<n>b)', '(?<n>a)(?<n>b)', '(?<\u00e9>a)', '(?<a-b>x)', '(?<1a>x)',
  '(?<>a)', '(?<$a>x)', '(?<_a>x)', '\\p{Script=Greek}', '\\p{sc=Latin}', '\\p{Alphabetic}', '\\p{Any}', '\\p{ASCII}',
  '\\p{Emoji}', '\\p{C}', '\\p{Cn}', '\\p{LC}', '\\p{Cased_Letter}', '\\p{Unassigned}', '\\P{C}', '\\p{gc=C}', '\\p{Other}',
  '[\\p{Alphabetic}]', '[^\\p{Cn}]', 'a{1001}', 'a{1000}', 'a{2000}', 'a{0,1001}', 'a{1001,}', 'a{99999999999999999999}',
  '(?:a{50}){50}', '(?:a{40}){40}', '(?:a{100}){11}', '(?:a{500}){2}', '(?:a{500}){3}', '(?:a{2}){500}', '(?:a{2}){501}',
  '(?:a{10}){100}', '(?:a{10}){101}', '(?:a{1000}){1}', '(?:a{1000}){2}', 'a{1000}b{1000}', '[a-z]{1000}', '\\p{L}{1000}', '\\w{1000}', '\\u{D800}', '\\uD800', '\\uDC00', '\\uD800\\u0041', '[\\uD800]',
  '\\u{DFFF}', '\\uDC00\\uD800',
];

const NEAR_MISSES = [
  ']', '}', '{', 'a{', 'a{1', 'a{1,', 'a{,2}', 'a{2,1}', 'a{ 1}', 'a{1 }', 'a{1}{2}', '{1}', '*', '+', '?', '**', 'a**', 'a+*', 'a?+',
  'a*??', 'a+?+', '\\b*', '\\B+', '^*', '$+', '^{2}', '\\a', '\\e', '\\_', '\\ ', '\\"', "\\'", '\\@', '\\-', '\\=', '\\<', '\\1',
  '\\01', '\\07', '\\00', '\\8', '\\9', '\\u', '\\u12', '\\u{', '\\u{}', '\\u{110000}', '\\u{FFFFFFFFFF}', '\\u{41', '\\x', '\\x1',
  '\\xZZ', '\\c', '\\c1', '\\c_', '\\c\u00e9', '[z-a]', '[\\d-a]', '[a-\\d]', '[\\w-\\s]', '[\\B]', '[\\k<a>]', '[[:alpha:]]',
  '[[:^digit:]]', '[[.a.]]', '[[=a=]]', '[a-\\p{L}]', '[\\p{L}-z]', '[a-]', '[-a]', '[a-z-9]', '[--a]', '[a--]', '[---]', '[^-]',
  '[\\-]', '[\\]]', '[\\^]', '[^]', '[]', '[^^]', '[^\\]]', '[\\b]', '[\\0]', '[\\00]', '[\\1]', '[a\\', '[', '[^', '[a', '\\A', '\\z',
  '\\Z', '\\Q.\\E', '\\h', '\\H', '\\R', '\\X', '\\G', '\\K', '\\N', '\\O', '\\pL', '\\PL', '\\p', '\\p{', '\\p{}', '\\p{L', '\\p{^L}',
  '\\p{IsLu}', '\\p{ Lu}', '\\p{Lu }', '\\P{', '(?#c)', '(?>a)', '(?|a)', '(?P<n>a)', '(?P=n)', '(?P>n)', '(?', '(?:', '(?<', '(?<n',
  '(?<n>', '\\k', '\\k<', '\\k<n', '\\k<n>', '(?<n>a)\\k<m>', '(', ')', '()', '(|)', '(?:)', '(?:|)', '|', '||', '\\', 'a\\',
  '\\/', '/', '-', ',', '\\u{0}', '\\u{00000000000041}', '\\uD83D\\uDE00', '\\uD83D\\u{DE00}', '\\u{D83D}\\u{DE00}', '\\ud83d\\ude00',
];

// ---- A pattern with its texts.

function neighbours(cp) {
  const out = [];
  for (const near of [cp - 1, cp + 1]) {
    if (near < 0 || near > 0x10ffff) continue;
    out.push(isSurrogate(near) ? (near < 0xe000 && near > cp ? 0xe000 : 0xd7ff) : near);
  }
  return out;
}

function makeContext(rng) {
  const interest = new Set();
  const G = {
    rng,
    interest,
    sequences: [],
    names: 1,
    size: 0,
    hasProperty: false,
    mention(cp) {
      if (cp >= 0 && cp <= 0x10ffff && !isSurrogate(cp)) interest.add(cp);
    },
    // A character of the candidates for which `has` is true, as a list of one code point (or none).
    sampleChar(r, has) {
      const candidates = [...interest, ...POOL, 0x61, 0x62, 0x30, 0x20].filter((cp) => has(cp) && !(G.hasProperty && !isStable(cp)));
      return candidates.length ? [r.pick(candidates)] : [];
    },
  };
  return G;
}

function buildPattern(rng) {
  const G = makeContext(rng);
  const root = regexNode(G, 0);
  const roll = rng.u32() / 4294967296;
  // About a third of the patterns carry one construct that the translator refuses or one near miss of syntax.
  if (roll < 0.12) {
    const sequence = rng.pick(G.sequences);
    sequence.terms.splice(rng.int(sequence.terms.length + 1), 0, rawNode(rng.pick(REFUSED)));
  } else if (roll < 0.30) {
    const sequence = rng.pick(G.sequences);
    sequence.terms.splice(rng.int(sequence.terms.length + 1), 0, rawNode(rng.pick(NEAR_MISSES)));
  }
  let pattern = root.render();
  // A change of a single character or two.
  if (rng.chance(0.04)) {
    const list = cps(pattern);
    const where = rng.int(list.length + 1);
    switch (rng.int(4)) {
      case 0: list.splice(where, 1); break;
      case 1: list.splice(where, 0, rng.pick([...SYNTAX_CHARACTERS])); break;
      case 2: list.splice(where, 0, list[rng.int(list.length || 1)] || 0x61); break;
      default: if (where + 1 < list.length) [list[where], list[where + 1]] = [list[where + 1], list[where]];
    }
    pattern = str(list);
  }
  return { pattern, root, G };
}

function buildTexts(rng, root, G) {
  const ok = (cp) => !G.hasProperty || isStable(cp);
  const interest = [];
  for (const cp of G.interest) {
    interest.push(cp);
    for (const near of neighbours(cp)) interest.push(near);
  }
  const pool = POOL.filter(ok);
  const randomCharacter = () => {
    const pick = rng.int(100);
    const from = pick < 55 && interest.length ? interest : pick < 85 ? pool : [0x61, 0x62, 0x63, 0x30, 0x20, 0x41, 0x5f, 0x2d];
    const cp = rng.pick(from);
    return ok(cp) ? cp : 0x61;
  };
  const texts = new Map();
  const add = (list) => {
    if (list.length > MAX_TEXT_CODE_POINTS || !list.every(ok)) return;
    texts.set(str(list), true);
  };
  add([]);
  const witnesses = rng.weighted([[20, 1], [40, 2], [30, 3], [10, 4]]);
  for (let i = 0; i < witnesses; i++) {
    const witness = root.sample(rng);
    add(witness);
    if (rng.chance(0.4)) {
      const prefix = Array.from({ length: rng.range(0, 2) }, randomCharacter);
      const suffix = Array.from({ length: rng.range(0, 2) }, randomCharacter);
      add(prefix.concat(witness, suffix));
    }
    // One change to a text that matches.
    const changed = witness.slice();
    const where = rng.int(changed.length + 1);
    switch (rng.int(3)) {
      case 0: changed.splice(where, 1); break;
      case 1: changed.splice(where, 0, randomCharacter()); break;
      default: if (where < changed.length) changed[where] = randomCharacter();
    }
    add(changed);
  }
  const randoms = rng.weighted([[30, 3], [40, 4], [30, 5]]);
  for (let i = 0; i < randoms; i++) {
    const length = rng.weighted([[30, rng.range(1, 2)], [50, rng.range(3, 8)], [20, rng.range(9, MAX_TEXT_CODE_POINTS)]]);
    add(Array.from({ length }, randomCharacter));
  }
  add([randomCharacter()]);
  const list = [...texts.keys()];
  // Keep the empty text and the first witnesses; drop random ones when there are too many.
  while (list.length > TEXTS_PER_PATTERN) list.splice(1 + rng.int(list.length - 1), 1);
  return list;
}

// ---- The reference.

class Reference {
  constructor(timeoutMs) {
    this.timeoutMs = timeoutMs;
    this.context = vm.createContext({ pattern: '', texts: [] });
    // The search of ECMA-262 (RegExpBuiltinExec): try a match at each code point boundary of the text, from the start,
    // and at the end. A match that must start at a given place is the sticky flag. V8 12 with `new RegExp(p, "u")`
    // and `.test(text)` also tries the place between the two halves of a surrogate pair, where "\\B" holds, which the
    // algorithm in the specification never does; the loop here is the algorithm.
    new vm.Script(`
      globalThis.run = function () {
        let re;
        try { re = new RegExp(pattern, 'uy'); } catch (e) {
          const m = String(e && e.message);
          if (e instanceof SyntaxError && !m.includes('too large') && !m.includes('Stack overflow')) return { syntax: true };
          return { error: m };
        }
        const search = (text) => {
          for (let at = 0; ; at += text.codePointAt(at) > 0xffff ? 2 : 1) {
            re.lastIndex = at;
            if (re.test(text)) return true;
            if (at >= text.length) return false;
          }
        };
        try { return { answers: texts.map(search) }; } catch (e) { return { error: String(e && e.message) }; }
      };`).runInContext(this.context);
    this.call = new vm.Script('JSON.stringify(run())');
  }

  // {syntax: true}, {answers: [...]}, {timeout: true} or {error: message}
  ask(pattern, texts) {
    this.context.pattern = pattern;
    this.context.texts = texts;
    try {
      return JSON.parse(this.call.runInContext(this.context, { timeout: this.timeoutMs }));
    } catch (e) {
      if (e && e.code === 'ERR_SCRIPT_EXECUTION_TIMEOUT') return { timeout: true };
      return { error: String(e && e.message) };
    }
  }
}

// ---- Output.

function jsonString(text) {
  let out = '"';
  for (const ch of text) {
    const cp = ch.codePointAt(0);
    if (ch === '"') out += '\\"';
    else if (ch === '\\') out += '\\\\';
    else if (cp >= 0x20 && cp < 0x7f) out += ch;
    else if (cp < 0x10000) out += '\\u' + hex(cp, 4);
    else {
      const high = 0xd800 + ((cp - 0x10000) >> 10);
      const low = 0xdc00 + ((cp - 0x10000) & 0x3ff);
      out += '\\u' + hex(high, 4) + '\\u' + hex(low, 4);
    }
  }
  return out + '"';
}

function caseLine(pattern, answer) {
  if (answer.syntax) return '{"pattern":' + jsonString(pattern) + ',"syntax_error":true}';
  const texts = answer.texts.map((t, i) => '{"text":' + jsonString(t) + ',"match":' + answer.answers[i] + '}');
  return '{"pattern":' + jsonString(pattern) + ',"texts":[' + texts.join(',') + ']}';
}

// ---- Commands.

function generate(options) {
  const rng = makeRandom(options.seed);
  const reference = new Reference(options.timeoutMs);
  const fd = fs.openSync(options.out, 'w');
  const stats = { requested: options.count, written: 0, syntax: 0, dropped_timeout: 0, dropped_error: 0, duplicates: 0, texts: 0 };
  const seen = new Set();
  const lines = [];
  for (let made = 0; made < options.count; ) {
    const { pattern, root, G } = buildPattern(rng);
    if (!pattern.isWellFormed() || Buffer.byteLength(pattern, 'utf8') > MAX_PATTERN_BYTES) continue;
    if (seen.has(pattern)) { stats.duplicates++; continue; }
    seen.add(pattern);
    made++;
    const texts = buildTexts(rng, root, G);
    const answer = reference.ask(pattern, texts);
    if (answer.timeout) { stats.dropped_timeout++; continue; }
    if (answer.error !== undefined) { stats.dropped_error++; continue; }
    if (answer.syntax) stats.syntax++;
    else stats.texts += texts.length;
    lines.push(caseLine(pattern, { syntax: answer.syntax, texts, answers: answer.answers }));
    stats.written++;
  }
  const header = {
    format: 1, seed: options.seed, requested: stats.requested, written: stats.written, syntax_errors: stats.syntax,
    dropped_timeout: stats.dropped_timeout, dropped_error: stats.dropped_error, node: process.version,
    v8: process.versions.v8, unicode: process.versions.unicode, timeout_ms: options.timeoutMs,
  };
  fs.writeSync(fd, JSON.stringify(header).slice(0, -1) + ',"cases":[\n');
  fs.writeSync(fd, lines.join(',\n'));
  fs.writeSync(fd, '\n]}\n');
  fs.closeSync(fd);
  process.stderr.write(JSON.stringify(header) + ' texts=' + stats.texts + ' duplicates=' + stats.duplicates + '\n');
}

// Every general category against every code point of the stable set, so that a table of either engine that disagrees
// with the other about one of those characters shows up as a difference of its own. The texts about \p{..} in the
// generated cases are drawn from this set.
function auditProperties(options) {
  const texts = [...STABLE].filter((cp) => !isSurrogate(cp)).sort((a, b) => a - b).map(chr);
  const reference = new Reference(options.timeoutMs);
  const lines = [];
  for (const category of CATEGORIES) {
    for (const negation of ['p', 'P']) {
      const pattern = '^\\' + negation + '{' + category.short + '}$';
      const answer = reference.ask(pattern, texts);
      if (answer.answers === undefined) throw new Error('no answer for ' + pattern);
      lines.push(caseLine(pattern, { texts, answers: answer.answers }));
    }
  }
  const header = { format: 1, audit: 'properties', node: process.version, unicode: process.versions.unicode, texts: texts.length };
  fs.writeFileSync(options.out, JSON.stringify(header).slice(0, -1) + ',"cases":[\n' + lines.join(',\n') + '\n]}\n');
  process.stderr.write(JSON.stringify(header) + '\n');
}

// Checks the expectations of a file of cases against V8: every syntax_error must be one, and every text must give the
// stated answer.
function check(file, timeoutMs) {
  const document = JSON.parse(fs.readFileSync(file, 'utf8'));
  const reference = new Reference(timeoutMs);
  let differences = 0;
  let texts = 0;
  for (const entry of document.cases) {
    const wanted = entry.syntax_error ? null : entry.texts.map((t) => t.text);
    const answer = reference.ask(entry.pattern, wanted || []);
    if (entry.syntax_error) {
      if (!answer.syntax) { differences++; console.log('V8 ACCEPTS a pattern stated as a syntax error: ' + jsonString(entry.pattern)); }
      continue;
    }
    if (answer.syntax || answer.answers === undefined) {
      differences++;
      console.log('V8 cannot answer for ' + jsonString(entry.pattern) + ': ' + JSON.stringify(answer));
      continue;
    }
    entry.texts.forEach((t, i) => {
      texts++;
      if (answer.answers[i] !== t.match) {
        differences++;
        console.log('DIFFERENCE pattern ' + jsonString(entry.pattern) + ' text ' + jsonString(t.text) + ': file says ' + t.match + ', V8 says ' + answer.answers[i]);
      }
    });
  }
  console.log(document.cases.length + ' cases, ' + texts + ' texts, ' + differences + ' differences from V8 (node ' + process.version + ', unicode ' + process.versions.unicode + ')');
  return differences === 0 ? 0 : 1;
}

function main(argv) {
  const options = { seed: 1, count: 1000, out: '', timeoutMs: 400, check: '', audit: false };
  for (let i = 0; i < argv.length; i++) {
    const take = () => {
      if (i + 1 >= argv.length) throw new Error('missing value for ' + argv[i]);
      return argv[++i];
    };
    switch (argv[i]) {
      case '--seed': options.seed = Number(take()); break;
      case '--count': options.count = Number(take()); break;
      case '--out': options.out = take(); break;
      case '--timeout-ms': options.timeoutMs = Number(take()); break;
      case '--check': options.check = take(); break;
      case '--audit-properties': options.audit = true; break;
      default: throw new Error('unknown argument ' + argv[i]);
    }
  }
  if (options.check) return check(options.check, options.timeoutMs);
  if (!options.out) throw new Error('--out is required');
  if (options.audit) auditProperties(options);
  else generate(options);
  return 0;
}

try {
  process.exitCode = main(process.argv.slice(2));
} catch (e) {
  process.stderr.write(String(e && e.message) + '\n');
  process.exitCode = 2;
}
