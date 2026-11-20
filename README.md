# automata

Header-only C++20 automata for term matching: prefix, glob and Levenshtein matchers
that walk a sorted term dictionary by seeking, so fuzzy and wildcard queries visit a
small fraction of the terms. Depends only on [unicode](https://github.com/Kronuz/unicode).

```cpp
#include "automata.hh"

const automata::levenshtein fuzzy("obama", 2);       // within two edits, transpositions on
termdict::reader::cursor c(dictionary);              // any cursor with seek, next and key
automata::walk(c, fuzzy, [](std::string_view term) { /* a match */ return true; });
```

How it works, and why: [docs/design.md](docs/design.md). Tests compare every automaton
with brute force; `fuzz/fuzz_automata.cc` is the libFuzzer target (`-DAUTOMATA_FUZZ=ON`
with Clang), and its corpus replays in every build.

MIT licensed; see `LICENSE`.
