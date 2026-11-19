# Differential test of the pattern translator

`pattern` and `patternProperties` are regular expressions in the dialect of ECMA-262 read with the `u` flag. The
validator translates each one to the syntax of RE2, or refuses it. A wrong translation is silent: a string that the
author's pattern rejects passes validation. This test compares the validator with a JavaScript engine (V8, through
Node) on generated patterns.

| File | What it is |
| --- | --- |
| `generate.js` | Node 20 or newer, no packages. Writes a file of cases from a seed, checks a file of cases against V8, or writes the audit of the general categories. |
| `check.cpp` | The test executable `neomcp_pattern_oracle`. Reads a file of cases and compares the validator with it. |
| `../data/pattern_cases.json` | The hand-written cases, in the same format. Read by the unit tests, by `check.cpp` and by `generate.js --check`. |

## Running it

```sh
node tests/pattern_oracle/generate.js --seed 1 --count 20000 --out cases.json
cmake --build build --target neomcp_pattern_oracle
build/tests/neomcp_pattern_oracle cases.json
```

`ctest` runs the hand-written cases on every run. Configure with `-DNEOMCP_PATTERN_ORACLE_CASES=<file>` to have it
check a generated file as well. The CI job `pattern-oracle` generates three files from fixed seeds.

## The cases

A case is a pattern and either `"syntax_error": true` or a list of `{"text", "match"}`. The generator draws patterns
from a grammar of every construct that the translator handles, every construct it refuses, and near misses of syntax
(unescaped `]` `{` `}`, stray quantifiers, identity escapes, octal escapes, incomplete escapes, reversed ranges,
POSIX-looking classes, Perl-only escapes, group syntax of other dialects), nested up to four levels. The texts of a
pattern (at most 24 code points, never a lone surrogate) are mostly made of the characters the pattern mentions and
their neighbours, of texts that the pattern matches, of those texts changed in one place, and of a fixed pool of
characters where dialects differ.

The checker fails when

- the reference says the pattern is a syntax error and the validator accepts it;
- the validator accepts the pattern and a text gives another answer than the reference;
- the validator refuses a pattern in a way that no named construct explains (an expression that the engine does not
  accept is one);
- fewer than 75 % of the patterns that the reference accepts reached a comparison (the run is then mostly refusals).

A refusal of a pattern that the reference accepts is allowed and is counted by the construct its message names. A
generated file of 50,000 patterns reaches a comparison for about 88 % of the patterns that the reference accepts.

## The reference

- **Search.** A text is searched the way ECMA-262 does it (RegExpBuiltinExec): a sticky match at each code point
  boundary, from the start of the text to its end. V8 12 with `new RegExp(p, "u")` and `.test(text)` also tries a
  match between the two halves of a surrogate pair, where `\B` holds, which the algorithm in the specification never
  does.
- **Time.** V8 backtracks. Every pattern runs in a `vm` context with a timeout of 400 ms (the `timeout` option of
  `vm.Script.runInContext`, which ends the execution of a script, matching included). A pattern whose texts are not
  all answered in time is dropped and counted in the header of the file. The grammar keeps nesting and texts short.
- **Unicode tables.** The two engines carry different versions of the Unicode data: Node 22 reports 17.0 in
  `process.versions.unicode` (it is in the header of every generated file and in the log of the CI job), and the RE2
  release that `cmake/NeomcpRe2.cmake` pins generates its tables from Unicode 15.1.0 (`_UNICODE_DIR` in
  `re2/unicode.py`). Texts that a pattern with `\p{..}` is tried on therefore use only characters assigned long ago,
  whose category is the same in every release since. `generate.js --audit-properties` writes a file of cases of every
  general category against every character of that set, so a disagreement of the tables about one of them is a
  failure of its own and not noise in the generated cases.
