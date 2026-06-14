#include "agent/diff.hpp"

#include <cstddef>

namespace moocode {

namespace {

// Beyond this many lines on either side the LCS DP table (O(n*m) cells) gets
// expensive, so we fall back to a whole-file replacement instead. The table is
// one contiguous row-major buffer (no per-row allocations); at the cap this is
// 2001² * 8B ≈ 32 MB.
constexpr std::size_t kMaxLcsLines = 2000;

// Split `text` into lines, dropping the final empty element that a trailing
// newline would otherwise produce (so "a\n" is one line, not two).
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> out;
    if (text.empty()) return out;  // empty text is zero lines, not one blank line
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    // A trailing '\n' leaves a spurious empty final element: drop it.
    if (!out.empty() && out.back().empty() && text.back() == '\n')
        out.pop_back();
    return out;
}

// All of `old` deleted then all of `new` added — the fallback for pathological
// inputs and the natural shape of a brand-new or fully-emptied file.
std::vector<DiffLine> whole_replace(const std::vector<std::string_view>& a,
                                    const std::vector<std::string_view>& b) {
    std::vector<DiffLine> out;
    out.reserve(a.size() + b.size());
    for (const auto& l : a) out.push_back({DiffLine::Op::Del, std::string(l)});
    for (const auto& l : b) out.push_back({DiffLine::Op::Add, std::string(l)});
    return out;
}

}  // namespace

std::vector<DiffLine> diff_lines(std::string_view old_text,
                                 std::string_view new_text) {
    const std::vector<std::string_view> a = split_lines(old_text);
    const std::vector<std::string_view> b = split_lines(new_text);
    const std::size_t n = a.size();
    const std::size_t m = b.size();

    if (n > kMaxLcsLines || m > kMaxLcsLines) return whole_replace(a, b);

    // LCS length DP: at(i,j) = LCS length of a[i..] and b[j..]. A single
    // contiguous (n+1)*(m+1) row-major buffer (one allocation, no pointer
    // chase), then a single backtrack to emit the edit script in forward order.
    const std::size_t stride = m + 1;
    std::vector<std::size_t> dp((n + 1) * stride, 0);
    auto at = [&](std::size_t i, std::size_t j) -> std::size_t& {
        return dp[i * stride + j];
    };
    for (std::size_t i = n; i-- > 0;)
        for (std::size_t j = m; j-- > 0;)
            at(i, j) = (a[i] == b[j]) ? at(i + 1, j + 1) + 1
                                      : std::max(at(i + 1, j), at(i, j + 1));

    std::vector<DiffLine> out;
    std::size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            out.push_back({DiffLine::Op::Context, std::string(a[i])});
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            out.push_back({DiffLine::Op::Del, std::string(a[i])});
            ++i;
        } else {
            out.push_back({DiffLine::Op::Add, std::string(b[j])});
            ++j;
        }
    }
    for (; i < n; ++i) out.push_back({DiffLine::Op::Del, std::string(a[i])});
    for (; j < m; ++j) out.push_back({DiffLine::Op::Add, std::string(b[j])});
    return out;
}

std::vector<DiffLine> elide_context(const std::vector<DiffLine>& diff,
                                    std::size_t context) {
    const std::size_t n = diff.size();
    // Mark every change plus the `context` lines flanking it as worth keeping;
    // the unmarked remainder is far-from-change context that gets collapsed.
    std::vector<bool> keep(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        if (diff[i].op == DiffLine::Op::Context) continue;
        keep[i] = true;
        for (std::size_t d = 1; d <= context; ++d) {
            if (i >= d) keep[i - d] = true;
            if (i + d < n) keep[i + d] = true;
        }
    }
    std::vector<DiffLine> out;
    for (std::size_t i = 0; i < n;) {
        if (keep[i]) {
            out.push_back(diff[i]);
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && !keep[j]) ++j;  // span of collapsible context
        const std::size_t count = j - i;
        if (count <= 1)
            out.push_back(diff[i]);  // not worth a marker
        else
            out.push_back({DiffLine::Op::Context,
                           "⋯ " + std::to_string(count) + " unchanged ⋯"});
        i = j;
    }
    return out;
}

std::string render_ansi_diff(const std::vector<DiffLine>& lines) {
    std::string out;
    for (const auto& l : lines) {
        switch (l.op) {
            case DiffLine::Op::Add: out += "\033[32m+ "; break;   // green
            case DiffLine::Op::Del: out += "\033[31m- "; break;   // red
            case DiffLine::Op::Context: out += "\033[2m  "; break;  // dim
        }
        out += l.text;
        out += "\033[0m\n";
    }
    return out;
}

}  // namespace moocode
