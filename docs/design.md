# automata: how it works

`automata.hh` matches terms against prefixes, globs and edit distances by walking a sorted term
dictionary, so a query visits the terms that can match and seeks past the rest. It came from
strata's `phase5/automata` (strata:219ded0d, its design revision 1 and review AUT-R1 to AUT-R14)
and serves prospect's term expansion (prospect `docs/design/automata.md`, `include/prospect/expand.hh`).
Decision numbers below are that design's.

## D1. Characters

Patterns and terms are sequences of characters, each decoded by `unicode::next_utf8`: valid UTF-8
decodes to its code point and any other byte to the code point of the same value, as `analysis`
decodes terms.

- **Aliasing.** A lone byte 0x80 to 0xFF decodes to U+0080 to U+00FF, the same character as that
  code point's two-byte form, so U+0080 to U+00FF have two spellings.
- **Literal prefix.** Every match starts with the bytes of the automaton's leading literal
  characters, up to the first one with two spellings; the walk covers only
  [literal prefix, `successor(literal prefix)`).
- **Lookahead.** `next_utf8` decides a character at byte s from at most `lookahead(s[0])` bytes
  (1, 2, 3 or 4 by the lead byte), valid or not. The matcher records that extent per character,
  so it knows which byte prefix decided each state.

## D2. Prefix

`prefix(p)` is the byte range [p, successor(p)): no decoding, no state.

## D3. Glob

`glob(pattern, flags)`: `*` (under `multi`) matches any run of characters and `?` (under `single`)
exactly one; with neither flag the pattern is literal, and there is no escape. Consecutive stars
collapse. The state is the set of live pattern positions, a bitset of m + 1 bits for m elements;
a character steps it with `next = ((s & (L[c] | Q)) << 1) | (s & S)` and one closure pass for
stars. `can_match` (any bit set) and `next` are exact. Nothing is determinized, so `*a??????????`
costs one word of state, not 2^10 states. The masks are charged to the caller's budget before they
are allocated.

## D4. Levenshtein

`levenshtein(target, k, prefix_chars, transpositions)`: keys within k insertions, deletions and
substitutions of the target, with transpositions also swaps of adjacent characters (the optimal
string alignment distance), whose first `prefix_chars` characters equal the target's. The state is
Ukkonen's band of the dynamic program, min(2k + 1, n + 1) cells clipped at k + 1, plus one swap band
with transpositions; a step costs O(min(k, n)). `can_match` is exact; `next` is conservative: it may
name a dead character, never skip a live one. Every k is accepted (it clamps at 2^62, past any key).
Lucene's and Tantivy's compiled Schulz-Mihov tables are the measured alternative; Lucene's stop at
k = 2.

## D5. The interface

```cpp
std::size_t words() const;                           // the state, in 64-bit words
void start(word* s) const;
void step(const word* s, char32_t c, word* out) const;
bool is_match(const word* s) const;
bool can_match(const word* s) const;
std::optional<char32_t> next(const word* s, char32_t c) const;  // smallest live >= c, never above it
std::string_view literal() const;                    // D1's literal prefix
static constexpr bool byte_range;                    // true for prefix: no decoding
```

A state is plain words with no pointers; an automaton is immutable once built, so one serves every
segment and thread of a query.

## D6. The matcher and its seek targets

`matcher<A>` runs an automaton over keys in any order and is fastest in increasing order:

- **Resuming.** It records, per character boundary of the previous key, the byte offset, the
  furthest byte examined so far (`seen`), and the state. A key sharing l bytes with the previous
  one resumes at the last boundary whose `seen` is at most l. Neighbors in a dictionary share most
  of their bytes, so most keys cost a few steps.
- **Dead prefixes.** When the state dies within the key, every key starting with its first `seen`
  bytes dies too, and is answered from bytes alone.
- **Seek targets.** When a key dies at boundary o and the bytes before o decided every earlier
  character, the characters that keep the state before o alive form a set A, listed by `next`.
  Every later live key starts with the bytes P before o and a spelling of some a in A, so the
  target is the smallest such P + spelling above the key, bounded by the dead prefix's successor
  and P's. Past 256 live characters it settles for the dead prefix's bound.
- **Depth.** Boundaries are recorded up to `resume_depth` characters (4,096); a longer key's tail
  is decoded on every visit. A preset: it changes no answer.

## The walk

`walk(cursor, automaton, f, options, from)` runs a matcher over any cursor of sorted keys with
`seek(t)` (forward to the first key at or above t), `next()` and `key()`. It positions the cursor
at the literal prefix, calls f on each match, and on a dead key seeks to the matcher's target
unless the target is the key's immediate successor. Every `check_every` entries (1,024) it calls
`pause()`, which may throw to cancel and returns true to stop; the walk then returns the key to
resume from, never its first key, and a new walk from it rebuilds the matcher (correct for any key
order). `intersect_sorted` is the walk over a sorted range, by binary search.

A dictionary supplies the cursor; termdict's `reader::cursor` fits as is. The automata never see
the dictionary's format, and termdict does not depend on them.

## Tests

`test/automata_test.cc` compares every automaton with brute force (`test/automata_reference.hh`:
`next_utf8`, a dynamic-programming glob and a full OSA table) over every {a, b} string to length
10, random Unicode and invalid-byte sets, every cut of a 2-, 3- and 4-byte sequence, matcher depths
0 to 4,096, every dead prefix and seek target against the terms it skips, paused and resumed walks,
and matchers on threads. `fuzz/fuzz_automata.cc` does the same from fuzzer input, and also walks a
termdict dictionary of the terms, intact and with corrupted blocks; its corpus replays as a test.
