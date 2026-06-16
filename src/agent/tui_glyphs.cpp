#include "agent/tui_glyphs.hpp"

#include <atomic>
#include <cstdlib>

#include "agent/strutil.hpp"  // to_lower

namespace moocode {

namespace {

// Set-once; reads are unsynchronised but the value stabilises on the main
// thread at TUI startup before worker threads render, so an atomic suffices.
std::atomic<GlyphMode> g_mode{GlyphMode::Unknown};

}  // namespace

GlyphMode detect_glyph_mode() {
    if (const char* g = std::getenv("MOOCODE_GLYPHS"); g && *g) {
        std::string v = to_lower(g);
        if (v == "ascii") return GlyphMode::Ascii;
        if (v == "unicode" || v == "utf8" || v == "utf-8") return GlyphMode::Unicode;
    }
    if (const char* a = std::getenv("MOOCODE_ASCII"); a && *a && a[0] != '0')
        return GlyphMode::Ascii;
#ifdef _WIN32
    // Windows Terminal sets WT_SESSION and renders full Unicode; its absence
    // means the legacy conhost, where emoji/box-drawing are unreliable.
    if (const char* wt = std::getenv("WT_SESSION"); wt && *wt) return GlyphMode::Unicode;
    return GlyphMode::Ascii;
#else
    return GlyphMode::Unicode;
#endif
}

GlyphMode glyph_mode() {
    GlyphMode m = g_mode.load(std::memory_order_relaxed);
    if (m == GlyphMode::Unknown) {
        m = detect_glyph_mode();
        g_mode.store(m, std::memory_order_relaxed);
    }
    return m;
}

void set_glyph_mode(GlyphMode mode) { g_mode.store(mode, std::memory_order_relaxed); }

const char* ascii_fold(std::uint32_t cp) {
    switch (cp) {
        case 0x00B7: return ".";        // · middle dot (separator)
        case 0x2014: return "-";        // — em dash
        case 0x2500: return "-";        // ─ box horizontal (rule)
        case 0x2026: return "...";      // … ellipsis
        case 0x2190: return "<-";       // ← left arrow
        case 0x2191: return "^";        // ↑ up arrow
        case 0x2192: return "->";       // → right arrow
        case 0x2193: return "v";        // ↓ down arrow
        case 0x2194: return "<>";       // ↔ left-right arrow
        case 0x21B3: return ">";        // ↳ down-right arrow (sub-item)
        case 0x23F3: return "[~]";      // ⏳ hourglass (buffered)
        case 0x258C: return "|";        // ▌ left half block (speaker bar)
        case 0x25A3: return "#";        // ▣ filled square (token meter)
        case 0x25B6: return ">";        // ▶ collapsed caret
        case 0x25BC: return "v";        // ▼ expanded caret
        case 0x2699: return "*";        // ⚙ gear (controls)
        case 0x26A0: return "!";        // ⚠ warning
        case 0x2713: return "+";        // ✓ check (ok)
        case 0x2717: return "x";        // ✗ cross (fail)
        case 0x276F: return ">";        // ❯ prompt caret
        case 0x1F4CE: return "[file]";  // 📎 paperclip (attachment)
        case 0x1F5BC: return "[image]"; // 🖼 framed picture
        default: return nullptr;
    }
}

std::string glyphify(std::string_view s) {
    if (glyph_mode() != GlyphMode::Ascii) return std::string(s);

    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto lead = static_cast<unsigned char>(s[i]);
        if (lead < 0x80) {  // fast path: ASCII byte
            out += static_cast<char>(lead);
            ++i;
            continue;
        }
        // Decode one UTF-8 sequence (lenient: a malformed lead is copied as-is).
        std::size_t len = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
        if (i + len > s.size()) len = 1;
        std::uint32_t cp = 0;
        if (len == 1) {
            cp = lead;
        } else {
            cp = lead & (0x7Fu >> len);
            for (std::size_t k = 1; k < len; ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        }
        if (const char* rep = ascii_fold(cp))
            out += rep;
        else
            out.append(s, i, len);
        i += len;
    }
    return out;
}

}  // namespace moocode
