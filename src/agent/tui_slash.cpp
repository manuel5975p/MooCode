#include "agent/tui_slash.hpp"

#include <cstdlib>  // strtod
#include <string>

#include "agent/strutil.hpp"  // to_lower

namespace moocode {

GenControlResult parse_effort(std::string_view arg) {
    GenControlResult r;
    const std::string a = to_lower(arg);
    if (a == "none" || a == "off") {
        r.params.thinking = false;
        r.info = "effort → off (thinking disabled)";
    } else if (a == "low" || a == "medium" || a == "high" || a == "minimal") {
        r.params.effort = a;
        r.info = "effort → " + a;
        r.reasoning_hint = true;
    } else {
        r.error = "usage: /effort low|medium|high|none";
    }
    return r;
}

GenControlResult parse_thinking(std::string_view arg) {
    GenControlResult r;
    const std::string a = to_lower(arg);
    if (a == "on" || a == "true" || a == "1") {
        r.params.thinking = true;
        r.info = "thinking → on";
        r.reasoning_hint = true;
    } else if (a == "off" || a == "false" || a == "0") {
        r.params.thinking = false;
        r.info = "thinking → off";
    } else {
        r.error = "usage: /thinking on|off";
    }
    return r;
}

GenControlResult parse_temp(std::string_view arg) {
    GenControlResult r;
    const std::string s(arg);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (arg.empty() || end == s.c_str() || *end != '\0') {
        r.error = "usage: /temp <number, e.g. 0.7>";
    } else {
        r.params.temperature = v;
        r.info = "temperature → " + s;
    }
    return r;
}

ThemeResult parse_theme(std::string_view arg) {
    ThemeResult r;
    if (arg.empty()) {
        r.list = true;
        return r;
    }
    auto t = syntax_theme_from_name(arg);
    if (!t) {
        r.error = "unknown theme '" + std::string(arg) +
                  "' — try: default, mono, vivid, none";
        return r;
    }
    r.theme = *t;
    return r;
}

}  // namespace moocode
