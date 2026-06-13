#include "agent/diff.hpp"

#include <string>

#include "test_harness.hpp"

using namespace flagent;
using Op = DiffLine::Op;

namespace {

// Count entries of each op.
struct Counts { int ctx = 0, add = 0, del = 0; };
Counts count(const std::vector<DiffLine>& d) {
    Counts c;
    for (const auto& l : d)
        switch (l.op) {
            case Op::Context: ++c.ctx; break;
            case Op::Add: ++c.add; break;
            case Op::Del: ++c.del; break;
        }
    return c;
}

}  // namespace

TEST("diff: identical text is all context") {
    auto d = diff_lines("a\nb\nc", "a\nb\nc");
    auto c = count(d);
    CHECK_EQ(c.add, 0);
    CHECK_EQ(c.del, 0);
    CHECK_EQ(c.ctx, 3);
}

TEST("diff: pure addition appends Add lines and keeps context") {
    auto d = diff_lines("a\nb", "a\nb\nc\nd");
    auto c = count(d);
    CHECK_EQ(c.ctx, 2);
    CHECK_EQ(c.add, 2);
    CHECK_EQ(c.del, 0);
    CHECK(d[2].op == Op::Add);
    CHECK_EQ(d[2].text, std::string("c"));
    CHECK_EQ(d[3].text, std::string("d"));
}

TEST("diff: pure deletion drops Del lines and keeps context") {
    auto d = diff_lines("a\nb\nc\nd", "a\nd");
    auto c = count(d);
    CHECK_EQ(c.del, 2);
    CHECK_EQ(c.add, 0);
    CHECK_EQ(c.ctx, 2);
}

TEST("diff: modify-in-middle is del+add surrounded by context") {
    auto d = diff_lines("a\nX\nc", "a\nY\nc");
    // 'a' context, 'X' del, 'Y' add, 'c' context (order of del/add may vary but
    // both present, both endpoints context).
    auto c = count(d);
    CHECK_EQ(c.ctx, 2);
    CHECK_EQ(c.add, 1);
    CHECK_EQ(c.del, 1);
    CHECK(d.front().op == Op::Context);
    CHECK_EQ(d.front().text, std::string("a"));
    CHECK(d.back().op == Op::Context);
    CHECK_EQ(d.back().text, std::string("c"));
}

TEST("diff: new file (old empty) is all Add") {
    auto d = diff_lines("", "one\ntwo\nthree");
    auto c = count(d);
    CHECK_EQ(c.add, 3);
    CHECK_EQ(c.del, 0);
    CHECK_EQ(c.ctx, 0);
}

TEST("diff: emptied file (new empty) is all Del") {
    auto d = diff_lines("one\ntwo", "");
    auto c = count(d);
    CHECK_EQ(c.del, 2);
    CHECK_EQ(c.add, 0);
}

TEST("diff: both empty yields nothing") {
    auto d = diff_lines("", "");
    CHECK_EQ(d.size(), std::size_t{0});
}

TEST("diff: trailing newline does not create a spurious empty line") {
    auto d = diff_lines("a\n", "a\n");
    CHECK_EQ(d.size(), std::size_t{1});  // just "a", not "a" + ""
    CHECK(d[0].op == Op::Context);
    CHECK_EQ(d[0].text, std::string("a"));
}

TEST("diff: trailing-newline vs none differs by no line (content equal)") {
    // "a\nb" and "a\nb\n" both split to {a,b} after dropping the trailing empty.
    auto d = diff_lines("a\nb", "a\nb\n");
    auto c = count(d);
    CHECK_EQ(c.add, 0);
    CHECK_EQ(c.del, 0);
    CHECK_EQ(c.ctx, 2);
}

TEST("diff: unicode lines compared by bytes, preserved verbatim") {
    auto d = diff_lines("héllo\n世界", "héllo\n世界!");
    auto c = count(d);
    CHECK_EQ(c.ctx, 1);   // "héllo" unchanged
    CHECK_EQ(c.del, 1);   // "世界" removed
    CHECK_EQ(c.add, 1);   // "世界!" added
    bool found = false;
    for (const auto& l : d)
        if (l.op == Op::Add && l.text == "世界!") found = true;
    CHECK(found);
}

TEST("diff: single empty line in the middle is handled") {
    auto d = diff_lines("a\n\nb", "a\nb");
    auto c = count(d);
    CHECK_EQ(c.del, 1);  // the blank middle line removed
    CHECK_EQ(c.ctx, 2);
}

TEST("diff: >2000 lines falls back to whole-file replacement") {
    std::string big_old, big_new;
    for (int i = 0; i < 3000; ++i) {
        big_old += "old" + std::to_string(i) + "\n";
        big_new += "new" + std::to_string(i) + "\n";
    }
    auto d = diff_lines(big_old, big_new);
    auto c = count(d);
    CHECK_EQ(c.ctx, 0);          // no LCS run; everything replaced
    CHECK_EQ(c.del, 3000);
    CHECK_EQ(c.add, 3000);
    // The fallback emits all deletions first, then all additions.
    CHECK(d.front().op == Op::Del);
    CHECK(d.back().op == Op::Add);
}

// --- P2: flatten + cap=2000 ------------------------------------------------
// The perf win (one contiguous DP buffer, no per-row allocations) is not
// unit-testable; these tests instead pin behaviour-equality and the new cap.

namespace {
// Rebuild the deterministic golden fixture (200 old lines, ~every 4th replaced,
// every 13th deleted, 5 trailing additions) used to capture the golden output.
void build_golden_fixture(std::string& a, std::string& b) {
    for (int i = 0; i < 200; ++i) {
        std::string line = "line " + std::to_string(i) + " content";
        a += line + "\n";
        if (i % 4 == 0) b += "EDITED " + std::to_string(i) + " xyz\n";
        else if (i % 13 == 0) { /* delete */ }
        else b += line + "\n";
    }
    for (int i = 0; i < 5; ++i) b += "appended " + std::to_string(i) + "\n";
}

// Serialize a diff as one "<op>\t<text>\n" line per entry, matching the golden
// capture format.
std::string serialize(const std::vector<DiffLine>& d) {
    std::string out;
    for (const auto& l : d) {
        out += (l.op == Op::Context ? 'C' : l.op == Op::Add ? 'A' : 'D');
        out += '\t';
        out += l.text;
        out += '\n';
    }
    return out;
}

// Golden DiffLine sequence captured from the diff_lines implementation BEFORE
// the flatten refactor (md5 04b83ddd0d818e61e06b21fae66ac2ce). Any drift here
// means the flatten/cap change altered output — a regression.
const char* kGolden = R"GOLDEN(D	line 0 content
A	EDITED 0 xyz
C	line 1 content
C	line 2 content
C	line 3 content
D	line 4 content
A	EDITED 4 xyz
C	line 5 content
C	line 6 content
C	line 7 content
D	line 8 content
A	EDITED 8 xyz
C	line 9 content
C	line 10 content
C	line 11 content
D	line 12 content
D	line 13 content
A	EDITED 12 xyz
C	line 14 content
C	line 15 content
D	line 16 content
A	EDITED 16 xyz
C	line 17 content
C	line 18 content
C	line 19 content
D	line 20 content
A	EDITED 20 xyz
C	line 21 content
C	line 22 content
C	line 23 content
D	line 24 content
A	EDITED 24 xyz
C	line 25 content
D	line 26 content
C	line 27 content
D	line 28 content
A	EDITED 28 xyz
C	line 29 content
C	line 30 content
C	line 31 content
D	line 32 content
A	EDITED 32 xyz
C	line 33 content
C	line 34 content
C	line 35 content
D	line 36 content
A	EDITED 36 xyz
C	line 37 content
C	line 38 content
D	line 39 content
D	line 40 content
A	EDITED 40 xyz
C	line 41 content
C	line 42 content
C	line 43 content
D	line 44 content
A	EDITED 44 xyz
C	line 45 content
C	line 46 content
C	line 47 content
D	line 48 content
A	EDITED 48 xyz
C	line 49 content
C	line 50 content
C	line 51 content
D	line 52 content
A	EDITED 52 xyz
C	line 53 content
C	line 54 content
C	line 55 content
D	line 56 content
A	EDITED 56 xyz
C	line 57 content
C	line 58 content
C	line 59 content
D	line 60 content
A	EDITED 60 xyz
C	line 61 content
C	line 62 content
C	line 63 content
D	line 64 content
D	line 65 content
A	EDITED 64 xyz
C	line 66 content
C	line 67 content
D	line 68 content
A	EDITED 68 xyz
C	line 69 content
C	line 70 content
C	line 71 content
D	line 72 content
A	EDITED 72 xyz
C	line 73 content
C	line 74 content
C	line 75 content
D	line 76 content
A	EDITED 76 xyz
C	line 77 content
D	line 78 content
C	line 79 content
D	line 80 content
A	EDITED 80 xyz
C	line 81 content
C	line 82 content
C	line 83 content
D	line 84 content
A	EDITED 84 xyz
C	line 85 content
C	line 86 content
C	line 87 content
D	line 88 content
A	EDITED 88 xyz
C	line 89 content
C	line 90 content
D	line 91 content
D	line 92 content
A	EDITED 92 xyz
C	line 93 content
C	line 94 content
C	line 95 content
D	line 96 content
A	EDITED 96 xyz
C	line 97 content
C	line 98 content
C	line 99 content
D	line 100 content
A	EDITED 100 xyz
C	line 101 content
C	line 102 content
C	line 103 content
D	line 104 content
A	EDITED 104 xyz
C	line 105 content
C	line 106 content
C	line 107 content
D	line 108 content
A	EDITED 108 xyz
C	line 109 content
C	line 110 content
C	line 111 content
D	line 112 content
A	EDITED 112 xyz
C	line 113 content
C	line 114 content
C	line 115 content
D	line 116 content
D	line 117 content
A	EDITED 116 xyz
C	line 118 content
C	line 119 content
D	line 120 content
A	EDITED 120 xyz
C	line 121 content
C	line 122 content
C	line 123 content
D	line 124 content
A	EDITED 124 xyz
C	line 125 content
C	line 126 content
C	line 127 content
D	line 128 content
A	EDITED 128 xyz
C	line 129 content
D	line 130 content
C	line 131 content
D	line 132 content
A	EDITED 132 xyz
C	line 133 content
C	line 134 content
C	line 135 content
D	line 136 content
A	EDITED 136 xyz
C	line 137 content
C	line 138 content
C	line 139 content
D	line 140 content
A	EDITED 140 xyz
C	line 141 content
C	line 142 content
D	line 143 content
D	line 144 content
A	EDITED 144 xyz
C	line 145 content
C	line 146 content
C	line 147 content
D	line 148 content
A	EDITED 148 xyz
C	line 149 content
C	line 150 content
C	line 151 content
D	line 152 content
A	EDITED 152 xyz
C	line 153 content
C	line 154 content
C	line 155 content
D	line 156 content
A	EDITED 156 xyz
C	line 157 content
C	line 158 content
C	line 159 content
D	line 160 content
A	EDITED 160 xyz
C	line 161 content
C	line 162 content
C	line 163 content
D	line 164 content
A	EDITED 164 xyz
C	line 165 content
C	line 166 content
C	line 167 content
D	line 168 content
D	line 169 content
A	EDITED 168 xyz
C	line 170 content
C	line 171 content
D	line 172 content
A	EDITED 172 xyz
C	line 173 content
C	line 174 content
C	line 175 content
D	line 176 content
A	EDITED 176 xyz
C	line 177 content
C	line 178 content
C	line 179 content
D	line 180 content
A	EDITED 180 xyz
C	line 181 content
D	line 182 content
C	line 183 content
D	line 184 content
A	EDITED 184 xyz
C	line 185 content
C	line 186 content
C	line 187 content
D	line 188 content
A	EDITED 188 xyz
C	line 189 content
C	line 190 content
C	line 191 content
D	line 192 content
A	EDITED 192 xyz
C	line 193 content
C	line 194 content
D	line 195 content
D	line 196 content
A	EDITED 196 xyz
C	line 197 content
C	line 198 content
C	line 199 content
A	appended 0
A	appended 1
A	appended 2
A	appended 3
A	appended 4
)GOLDEN";
}  // namespace

TEST("diff: flatten preserves the golden DiffLine sequence byte-for-byte") {
    std::string a, b;
    build_golden_fixture(a, b);
    std::string got = serialize(diff_lines(a, b));
    CHECK_EQ(got, std::string(kGolden));
}

TEST("diff: exactly 2000 lines/side still runs a real LCS diff") {
    // 2000 == kMaxLcsLines, which is NOT > the cap, so the DP path runs.
    std::string old_t, new_t;
    for (int i = 0; i < 2000; ++i) {
        old_t += "x" + std::to_string(i) + "\n";
        new_t += "x" + std::to_string(i) + "\n";  // identical => all context
    }
    auto d = diff_lines(old_t, new_t);
    auto c = count(d);
    CHECK_EQ(c.ctx, 2000);   // real LCS, every line matched
    CHECK_EQ(c.add, 0);
    CHECK_EQ(c.del, 0);
}

TEST("diff: 2001 lines/side trips the whole-file fallback") {
    std::string old_t, new_t;
    for (int i = 0; i < 2001; ++i) {
        old_t += "o" + std::to_string(i) + "\n";
        new_t += "n" + std::to_string(i) + "\n";
    }
    auto d = diff_lines(old_t, new_t);
    auto c = count(d);
    CHECK_EQ(c.ctx, 0);          // fallback: no context lines at all
    CHECK_EQ(c.del, 2001);
    CHECK_EQ(c.add, 2001);
    CHECK(d.front().op == Op::Del);
    CHECK(d.back().op == Op::Add);
}

TEST("elide_context: collapses far context into a marker, keeps flanks") {
    // 10 context lines, then one change. With context=3, only the 3 lines just
    // above the change survive; the other 7 collapse to a single marker.
    std::string old_t, new_t;
    for (int i = 0; i < 10; ++i) old_t += "c" + std::to_string(i) + "\n";
    new_t = old_t;
    old_t += "gone\n";
    new_t += "added\n";
    auto e = elide_context(diff_lines(old_t, new_t), 3);
    auto c = count(e);
    CHECK_EQ(c.add, 1);
    CHECK_EQ(c.del, 1);
    CHECK_EQ(c.ctx, 4);  // 3 kept flanking lines + 1 collapse marker
    bool has_marker = false;
    for (const auto& l : e)
        if (l.op == Op::Context && l.text.find("unchanged") != std::string::npos)
            has_marker = true;
    CHECK(has_marker);
}

TEST("elide_context: a single skipped line is kept rather than markered") {
    // context=0 leaves lone context lines between changes; a run of one must not
    // become a "1 unchanged" marker (a marker only pays off for runs of 2+).
    auto e = elide_context(diff_lines("a\nKEEP\nb", "X\nKEEP\nY"), 0);
    for (const auto& l : e)
        CHECK(l.text.find("unchanged") == std::string::npos);
    auto c = count(e);
    CHECK_EQ(c.ctx, 1);  // the lone KEEP survives verbatim
}

TEST("elide_context: an all-context diff collapses to one marker") {
    // No changes => nothing is "kept", so the whole run collapses to a single
    // marker. Degenerate but must not crash or duplicate.
    std::vector<DiffLine> all{{Op::Context, "a"}, {Op::Context, "b"},
                              {Op::Context, "c"}};
    auto e = elide_context(all, 3);
    CHECK_EQ(e.size(), std::size_t{1});
    CHECK(e.front().op == Op::Context);
}

TEST("render_ansi_diff: colors each op and ends lines with a reset") {
    std::vector<DiffLine> d{{Op::Add, "x"}, {Op::Del, "y"}, {Op::Context, "z"}};
    std::string s = render_ansi_diff(d);
    CHECK(s.find("\033[32m+ x") != std::string::npos);  // green add
    CHECK(s.find("\033[31m- y") != std::string::npos);  // red del
    CHECK(s.find("\033[2m  z") != std::string::npos);   // dim context
    CHECK(s.find("\033[0m\n") != std::string::npos);    // reset + newline
}
