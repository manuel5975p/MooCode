#include "agent/skills.hpp"

#include <algorithm>
#include <system_error>
#include <utility>
#include <vector>

#include "agent/fsutil.hpp"   // slurp
#include "agent/json_util.hpp"
#include "agent/strutil.hpp"  // trim_sv

namespace moocode {

void SkillRegistry::add(Skill skill) {
    std::lock_guard lk(mtx_);
    auto it = std::ranges::find_if(
        entries_, [&](const Entry& e) { return e.skill.name == skill.name; });
    if (it != entries_.end())
        it->skill = std::move(skill);  // last registration wins, keep loaded flag
    else
        entries_.push_back(Entry{.skill = std::move(skill), .loaded = false});
}

bool SkillRegistry::known(const std::string& name) const {
    std::lock_guard lk(mtx_);
    return std::ranges::any_of(
        entries_, [&](const Entry& e) { return e.skill.name == name; });
}

bool SkillRegistry::is_loaded(const std::string& name) const {
    std::lock_guard lk(mtx_);
    auto it = std::ranges::find_if(
        entries_, [&](const Entry& e) { return e.skill.name == name; });
    return it != entries_.end() && it->loaded;
}

SkillEffect SkillRegistry::effect_of(const std::string& name) const {
    std::lock_guard lk(mtx_);
    auto it = std::ranges::find_if(
        entries_, [&](const Entry& e) { return e.skill.name == name; });
    return it == entries_.end() ? SkillEffect::Tools : it->skill.effect;
}

bool SkillRegistry::injects_prompt(const std::string& name) const {
    return effect_of(name) == SkillEffect::InjectMessage;
}

std::vector<SkillRegistry::Listing> SkillRegistry::list() const {
    std::lock_guard lk(mtx_);
    std::vector<Listing> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_)
        out.push_back(Listing{.name = e.skill.name,
                              .description = e.skill.description,
                              .loaded = e.loaded});
    return out;
}

std::expected<std::string, Error> SkillRegistry::load(const std::string& name,
                                                      ToolRegistry& reg) {
    // Find the entry and snapshot its loader under the lock; run the loader
    // outside the lock so a loader that itself touches the registry can't
    // deadlock, then re-lock to flip the loaded flag.
    std::function<std::expected<std::string, Error>(ToolRegistry&)> loader;
    {
        std::lock_guard lk(mtx_);
        auto it = std::ranges::find_if(
            entries_, [&](const Entry& e) { return e.skill.name == name; });
        if (it == entries_.end())
            return std::unexpected(Error{.msg = "unknown skill: " + name, .code = 0});
        if (it->loaded)
            return "skill '" + name + "' is already loaded";
        loader = it->skill.load;
    }

    if (!loader)
        return std::unexpected(
            Error{.msg = "skill '" + name + "' has no loader", .code = 0});

    auto result = loader(reg);
    if (!result) return result;  // leave unloaded on failure

    {
        std::lock_guard lk(mtx_);
        auto it = std::ranges::find_if(
            entries_, [&](const Entry& e) { return e.skill.name == name; });
        if (it != entries_.end()) it->loaded = true;
    }
    return result;
}

namespace {

// True for a Markdown ATX heading line ("#", "##", ... followed by space/EOL).
bool is_heading(std::string_view line) {
    std::size_t h = 0;
    while (h < line.size() && line[h] == '#') ++h;
    return h > 0 && (h == line.size() || line[h] == ' ' || line[h] == '\t');
}

// Strip a leading run of '#' and the following blanks from a heading line.
std::string_view strip_heading_marks(std::string_view line) {
    std::size_t h = 0;
    while (h < line.size() && line[h] == '#') ++h;
    line.remove_prefix(h);
    return trim_sv(line);
}

// Pop one line (without its trailing '\n') from the front of `rest`, advancing
// `rest` past the newline. Handles a trailing CR via trim at the use site.
std::string_view next_line(std::string_view& rest) {
    const std::size_t nl = rest.find('\n');
    std::string_view line =
        (nl == std::string_view::npos) ? rest : rest.substr(0, nl);
    rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
    return line;
}

// Consume an optional leading `---`…`---` frontmatter block from the front of
// `content`, returning the parsed effect and advancing `content` past the
// closing fence. Frontmatter must be the first non-blank line; only the
// `effect:` key is recognized (value containing "system" => SystemPrompt). An
// absent or unterminated block leaves `content` untouched and yields the
// InjectMessage default.
SkillEffect consume_frontmatter(std::string_view& content) {
    // Peek: the first non-blank line must be exactly "---" to open a block.
    std::string_view probe = content;
    std::string_view first;
    bool any = false;
    while (!probe.empty()) {
        std::string_view l = trim_sv(next_line(probe));
        if (!l.empty()) {
            first = l;
            any = true;
            break;
        }
    }
    if (!any || first != "---") return SkillEffect::InjectMessage;

    // Commit: skip blanks to the opening fence, then read key:value lines until
    // a closing "---". Only consume the block if it is properly terminated.
    std::string_view scan = content;
    std::string_view l;
    do {
        l = trim_sv(next_line(scan));
    } while (l.empty());  // l == "---" (opening fence)

    SkillEffect effect = SkillEffect::InjectMessage;
    bool closed = false;
    while (!scan.empty()) {
        std::string_view kv = trim_sv(next_line(scan));
        if (kv == "---") {
            closed = true;
            break;
        }
        const std::size_t colon = kv.find(':');
        if (colon == std::string_view::npos) continue;
        std::string_view key = trim_sv(kv.substr(0, colon));
        std::string_view val = trim_sv(kv.substr(colon + 1));
        if (key == "effect")
            effect = (val.find("system") != std::string_view::npos)
                         ? SkillEffect::SystemPrompt
                         : SkillEffect::InjectMessage;
    }
    if (closed) content = scan;  // only consume a well-formed block
    return closed ? effect : SkillEffect::InjectMessage;
}

}  // namespace

std::expected<MarkdownSkill, Error> parse_markdown_skill(
    std::string name, std::string_view content) {
    std::string_view rest = content;
    const SkillEffect effect = consume_frontmatter(rest);

    // Skip leading blank lines to reach the description block.
    std::string_view line;
    bool have_line = false;
    while (!rest.empty()) {
        line = trim_sv(next_line(rest));
        if (!line.empty()) {
            have_line = true;
            break;
        }
    }
    if (!have_line)
        return std::unexpected(
            Error{.msg = "skill '" + name + "' is empty", .code = 0});

    // The first non-blank block is the description: the heading (with its '#'
    // marks stripped) plus any contiguous non-blank lines below it, collapsed to
    // one whitespace-separated line. The block ends at the first blank line.
    std::string desc;
    auto append_desc = [&desc](std::string_view piece) {
        if (piece.empty()) return;
        if (!desc.empty()) desc += ' ';
        desc.append(piece);
    };
    append_desc(is_heading(line) ? strip_heading_marks(line) : line);
    while (!rest.empty()) {
        std::string_view l = trim_sv(next_line(rest));
        if (l.empty()) break;  // blank line ends the description block
        append_desc(l);
    }
    if (desc.empty())
        return std::unexpected(Error{
            .msg = "skill '" + name + "' has no description heading", .code = 0});

    // The remainder (after the blank separator) is the prompt body. Trim leading
    // blank lines and trailing whitespace so the injected text is clean.
    while (!rest.empty()) {
        const std::size_t nl = rest.find('\n');
        std::string_view first = (nl == std::string_view::npos) ? rest : rest.substr(0, nl);
        if (!trim_sv(first).empty()) break;
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
    }
    std::string prompt(rest);
    while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r' ||
                              prompt.back() == ' ' || prompt.back() == '\t'))
        prompt.pop_back();
    if (prompt.empty())
        return std::unexpected(Error{
            .msg = "skill '" + name + "' has no prompt body", .code = 0});

    return MarkdownSkill{.name = std::move(name),
                         .description = std::move(desc),
                         .prompt = std::move(prompt),
                         .effect = effect};
}

Skill make_markdown_skill(MarkdownSkill md,
                          std::function<void(std::string)> system_sink) {
    std::string prompt = std::move(md.prompt);

    if (md.effect == SkillEffect::SystemPrompt) {
        return Skill{
            .name = std::move(md.name),
            .description = std::move(md.description),
            // Loading appends the body to the system prompt via the sink (wired
            // to Agent::append_system_prompt) and returns only a status note. If
            // no sink was supplied, degrade to returning the body so the text is
            // never silently dropped.
            .load = [prompt = std::move(prompt),
                     sink = std::move(system_sink)](ToolRegistry&)
                        -> std::expected<std::string, Error> {
                if (!sink) return prompt;
                sink(prompt);
                return std::string("loaded — appended to the system prompt");
            },
            .effect = SkillEffect::SystemPrompt};
    }

    return Skill{
        .name = std::move(md.name),
        .description = std::move(md.description),
        // A message-injecting skill registers no tools; loading it just surfaces
        // the body, which the load_skill tool returns as its result (so the
        // model sees it next turn) and the /skill command feeds into the
        // conversation.
        .load = [prompt = std::move(prompt)](ToolRegistry&)
                    -> std::expected<std::string, Error> { return prompt; },
        .effect = SkillEffect::InjectMessage};
}

std::vector<std::string> load_markdown_skills(
    SkillRegistry& skills, const std::filesystem::path& dir,
    std::function<void(std::string)> system_sink) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};

    // Collect *.md files first so registration order is deterministic (sorted by
    // filename) regardless of directory-iteration order.
    std::vector<fs::path> files;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() == ".md") files.push_back(e.path());
    }
    std::ranges::sort(files);

    std::vector<std::string> registered;
    for (const fs::path& f : files) {
        auto md = parse_markdown_skill(f.stem().string(), slurp(f));
        if (!md) continue;  // skip unparseable files
        registered.push_back(md->name);
        skills.add(make_markdown_skill(std::move(*md), system_sink));
    }
    return registered;
}

namespace {

// "  - name: description [loaded]" lines for the tool description. Deterministic
// (registration order). Trailing newline dropped.
std::string format_skill_list(const std::vector<SkillRegistry::Listing>& l) {
    std::string out;
    for (const auto& s : l) {
        out += "  - " + s.name;
        if (!s.description.empty()) out += ": " + s.description;
        if (s.loaded) out += " [loaded]";
        out += '\n';
    }
    if (!out.empty()) out.pop_back();
    return out;
}

}  // namespace

Tool load_skill_tool(SkillRegistry& skills, ToolRegistry& reg) {
    std::string desc =
        "Load an optional skill, registering its extra tools for use on the "
        "next turn. Skills are not active by default. Available skills:\n" +
        format_skill_list(skills.list());

    nlohmann::json params = json::parse_or(R"({
        "type":"object",
        "properties":{
          "name":{"type":"string","description":"Name of the skill to load."}},
        "required":["name"]})");

    ToolSpec spec{"load_skill", std::move(desc), std::move(params)};

    return Tool{
        .spec = std::move(spec),
        .run = [&skills, &reg](const nlohmann::json& args)
                   -> std::expected<std::string, Error> {
            if (!args.is_object() || !args.contains("name") ||
                !args["name"].is_string())
                return std::unexpected(
                    Error{.msg = "missing or non-string argument: name", .code = 0});
            std::string name = args["name"].get<std::string>();
            const auto first = name.find_first_not_of(" \t\r\n");
            const auto last = name.find_last_not_of(" \t\r\n");
            name = (first == std::string::npos)
                       ? std::string{}
                       : name.substr(first, last - first + 1);
            if (name.empty())
                return std::unexpected(Error{.msg = "skill name must not be empty", .code = 0});
            return skills.load(name, reg);
        }};
}

}  // namespace moocode
