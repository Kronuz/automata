// libFuzzer target for the automata (docs/design.md, T3): an automaton and a term set from the
// input, matched through intersect_sorted, the matcher key by key, and the walk over a termdict
// dictionary of the terms, must equal brute force, and no seek target may skip a match.
// data[0] picks the automaton (prefix, glob, Levenshtein) and data[1] its parameters; then a
// length byte and the pattern; then terms, each a length byte and its bytes. Any mismatch
// traps. With bit 0x40 of data[1], the dictionary's blocks are also corrupted at input-chosen
// bytes and walked again, which must answer or throw termdict::corrupt.
#include "automata.hh"
#include "termdict.hh"

#include "../test/automata_reference.hh"

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#define FUZZ_CHECK(cond) \
	do { \
		if (!(cond)) { __builtin_trap(); } \
	} while (0)

template <class A, class Ref>
void check(const A& a, const std::vector<std::string>& terms, const Ref& ref, bool dictionary, std::uint64_t flip) {
	std::vector<std::string> want, got;
	for (const auto& t : terms) {
		if (ref(t)) { want.push_back(t); }
	}
	automata::intersect_sorted(a, terms.begin(), terms.end(), [&](std::string_view k) {
		got.emplace_back(k);
		return true;
	});
	FUZZ_CHECK(got == want);
	if constexpr (!A::byte_range) {
		automata::matcher<A> m(a);
		for (std::size_t i = 0; i < terms.size(); ++i) {
			const auto& t = terms[i];
			const auto r = m(t);
			FUZZ_CHECK(r.match == ref(t));
			FUZZ_CHECK(!r.dead || (!r.match && r.dead <= t.size()));
			if (r.died) {  // the seek target skips no match
				const auto target = m.seek();
				FUZZ_CHECK(!target || *target > t);
				for (std::size_t j = i + 1; j < terms.size() && (!target || terms[j] < *target); ++j) { FUZZ_CHECK(!ref(terms[j])); }
			}
		}
	}
	if (!dictionary || terms.empty()) { return; }
	termdict::builder b;
	for (const auto& t : terms) { b.add(t, t.substr(0, t.size() % 3)); }
	const termdict::tables tables = b.finish();
	{
		const termdict::reader r(tables.blocks, tables.index);
		termdict::reader::cursor c(r);
		got.clear();
		automata::walk(c, a, [&](std::string_view k) {
			got.emplace_back(k);
			FUZZ_CHECK(c.value() == k.substr(0, k.size() % 3));  // the cursor stands on the match
			return true;
		});
		FUZZ_CHECK(got == want);
	}
	if (flip) {  // a corrupted copy answers or throws termdict::corrupt, never crashes
		std::string bad = tables.blocks;
		for (int i = 0; i < 3 && !bad.empty(); ++i) {
			flip = flip * 6364136223846793005u + 1442695040888963407u;
			bad[(flip >> 33) % bad.size()] ^= char((flip >> 24) | 1);
		}
		try {
			const termdict::reader r(bad, tables.index);
			termdict::reader::cursor c(r);
			std::size_t visits = 0;  // unsorted keys may lead seeks back: bound the entries
			automata::walk_options o;
			o.check_every = 1;
			o.pause = [&] { return ++visits > (1u << 16); };
			automata::walk(c, a, [](std::string_view) { return true; }, o);
		} catch (const termdict::corrupt&) {
		}
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 3) { return 0; }
	const unsigned kind = data[0] % 3, params = data[1];
	std::size_t at = 2;
	const std::size_t plen = std::min<std::size_t>(data[at], size - at - 1);
	++at;
	const std::string pattern(reinterpret_cast<const char*>(data + at), plen);
	at += plen;
	std::set<std::string> set;
	while (at < size) {
		const std::size_t n = std::min<std::size_t>(data[at], size - at - 1);
		++at;
		set.emplace(reinterpret_cast<const char*>(data + at), n);
		at += n;
	}
	const bool dictionary = params & 0x80;
	std::uint64_t flip = 0;  // nonzero: also walk a corrupted copy of the dictionary
	if (params & 0x40) {
		for (std::size_t i = 0; i < size; ++i) { flip = flip * 131 + data[i]; }
		flip |= 1;
	}
	const std::vector<std::string> terms(set.begin(), set.end());
	if (kind == 0) {
		check(automata::prefix(pattern), terms, [&](std::string_view k) { return reference::prefix(pattern, k); }, dictionary, flip);
	} else if (kind == 1) {
		const unsigned flags = params & 3;
		check(automata::glob(pattern, flags), terms, [&](std::string_view k) { return reference::glob(pattern, flags, k); }, dictionary, flip);
	} else {
		const std::uint64_t k = params & 3;
		const std::size_t fixed = (params >> 2) & 7;
		const bool swaps = params & 0x20;
		check(automata::levenshtein(pattern, k, fixed, swaps), terms, [&](std::string_view key) { return reference::levenshtein(pattern, k, fixed, swaps, key); }, dictionary, flip);
	}
	return 0;
}
