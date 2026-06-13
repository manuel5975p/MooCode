#include "agent/persist.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "temp_dir.hpp"
#include "test_harness.hpp"

using namespace flagent;

namespace {

Message user(std::string c) { return Message::user(std::move(c)); }

Message assistant_with_call(std::string text, std::string id, std::string name,
                            std::string args) {
    std::vector<ToolCall> calls;
    calls.push_back(ToolCall{.id = std::move(id), .name = std::move(name), .arguments_json = std::move(args)});
    return Message::assistant(std::move(text), std::move(calls));
}

Message tool_result(std::string id, std::string content) {
    return Message::tool(std::move(id), std::move(content));
}

}  // namespace

TEST("persist: conversation round-trips with tool calls and results") {
    test::TempDir td;
    std::string path = (td.path() / "conv.toml").string();

    Conversation conv;
    conv.push_back(Message::system("be terse"));
    conv.push_back(user("write a file"));
    conv.push_back(assistant_with_call("sure", "call_1", "write_file",
                                       R"({"path":"a.txt","content":"hi"})"));
    conv.push_back(tool_result("call_1", "wrote 2 bytes to a.txt"));
    conv.push_back(Message::assistant("done"));

    ConvMeta meta{"/tmp/proj", "MiniMax-M3", "2026-06-06T14:30:12Z",
                  "2026-06-06T14:41:55Z", "write a file"};

    auto saved = save_conversation(path, conv, meta);
    CHECK(saved.has_value());

    auto loaded = load_conversation_with_meta(path);
    CHECK(loaded.has_value());
    if (!loaded) return;

    const Conversation& got = loaded->first;
    CHECK_EQ(got.size(), conv.size());
    if (got.size() == conv.size()) {
        CHECK(got[0].role() == Role::System);
        CHECK_EQ(got[0].content(), std::string("be terse"));
        CHECK(got[1].role() == Role::User);
        CHECK(got[2].role() == Role::Assistant);
        CHECK_EQ(got[2].tool_calls().size(), std::size_t{1});
        CHECK_EQ(got[2].tool_calls()[0].id, std::string("call_1"));
        CHECK_EQ(got[2].tool_calls()[0].name, std::string("write_file"));
        CHECK_EQ(got[2].tool_calls()[0].arguments_json,
                 std::string(R"({"path":"a.txt","content":"hi"})"));
        CHECK(got[3].role() == Role::Tool);
        CHECK_EQ(got[3].tool_call_id(), std::string("call_1"));
        CHECK_EQ(got[3].content(), std::string("wrote 2 bytes to a.txt"));
        CHECK(got[4].role() == Role::Assistant);
        CHECK(got[4].tool_calls().empty());
    }

    const ConvMeta& gm = loaded->second;
    CHECK_EQ(gm.cwd, std::string("/tmp/proj"));
    CHECK_EQ(gm.model, std::string("MiniMax-M3"));
    CHECK_EQ(gm.created, std::string("2026-06-06T14:30:12Z"));
    CHECK_EQ(gm.updated, std::string("2026-06-06T14:41:55Z"));
    CHECK_EQ(gm.title, std::string("write a file"));
}

TEST("persist: a tool_failed=true Tool message survives a round-trip") {
    test::TempDir td;
    std::string path = (td.path() / "conv.toml").string();
    Conversation conv;
    Message failed = Message::tool("c1", "ERROR: kaboom", true);
    Message ok = Message::tool("c2", "ERROR: this is fine", false);  // succeeded
    conv.push_back(assistant_with_call("x", "c1", "boom", "{}"));
    conv.push_back(failed);
    conv.push_back(ok);
    CHECK(save_conversation(path, conv, ConvMeta{}).has_value());
    auto loaded = load_conversation(path);
    CHECK(loaded.has_value());
    if (loaded && loaded->size() == 3) {
        CHECK((*loaded)[1].tool_failed());    // failed bit preserved
        CHECK(!(*loaded)[2].tool_failed());   // success preserved despite "ERROR:"
    }
}

TEST("persist: assistant reasoning survives a round-trip") {
    test::TempDir td;
    std::string path = (td.path() / "conv.toml").string();
    Conversation conv;
    conv.push_back(Message::assistant("the answer", {}, "let me think: 2+2=4"));
    CHECK(save_conversation(path, conv, ConvMeta{}).has_value());
    auto loaded = load_conversation(path);
    CHECK(loaded.has_value());
    if (loaded && loaded->size() == 1) {
        CHECK_EQ((*loaded)[0].content(), std::string("the answer"));
        CHECK_EQ((*loaded)[0].reasoning(), std::string("let me think: 2+2=4"));
    }
}

// A legacy file without the reasoning key loads with empty reasoning, no crash.
TEST("persist: missing reasoning key loads as empty") {
    test::TempDir td;
    std::string path = (td.path() / "conv.toml").string();
    std::ofstream(path) << "[[message]]\nrole = \"assistant\"\ncontent = \"hi\"\n";
    auto loaded = load_conversation(path);
    CHECK(loaded.has_value());
    if (loaded && loaded->size() == 1)
        CHECK(loaded->front().reasoning().empty());
}

// Pre-migration files carry no tool_failed key; the loader infers it from the
// legacy "ERROR: " content prefix so old conversations keep their Failed glyph.
TEST("persist: legacy Tool message without the key infers tool_failed") {
    test::TempDir td;
    std::string path = (td.path() / "legacy.toml").string();
    {
        std::ofstream out(path, std::ios::binary);
        out << "[meta]\n\n"
               "[[message]]\n"
               "role = \"tool\"\n"
               "content = \"ERROR: legacy failure\"\n"
               "tool_call_id = \"c1\"\n\n"
               "[[message]]\n"
               "role = \"tool\"\n"
               "content = \"plain success\"\n"
               "tool_call_id = \"c2\"\n";
    }
    auto loaded = load_conversation(path);
    CHECK(loaded.has_value());
    if (loaded && loaded->size() == 2) {
        CHECK((*loaded)[0].tool_failed());   // inferred from "ERROR: " prefix
        CHECK(!(*loaded)[1].tool_failed());  // plain content stays Ok
    }
}

TEST("persist: multiline, unicode, and quote-heavy content survives") {
    test::TempDir td;
    std::string path = (td.path() / "conv.toml").string();
    std::string tricky =
        "line1\nline2 with \"quotes\" and 'apostrophes'\n世界 héllo\n\ttab\n";
    Conversation conv;
    conv.push_back(user(tricky));
    ConvMeta meta;
    meta.cwd = "/x";
    CHECK(save_conversation(path, conv, meta).has_value());
    auto got = load_conversation(path);
    CHECK(got.has_value());
    if (got && !got->empty()) CHECK_EQ((*got)[0].content(), tricky);
}

TEST("persist: empty optionals are omitted and tolerated on read") {
    test::TempDir td;
    std::string path = (td.path() / "conv.toml").string();
    Conversation conv;
    conv.push_back(user("hi"));                                 // no tool_call_id
    conv.push_back(Message::assistant(""));                     // empty content, no calls
    ConvMeta meta;  // entirely empty meta
    CHECK(save_conversation(path, conv, meta).has_value());
    auto loaded = load_conversation_with_meta(path);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK_EQ(loaded->first.size(), std::size_t{2});
        CHECK(loaded->first[1].content().empty());
        CHECK(loaded->first[1].tool_calls().empty());
        CHECK(loaded->second.cwd.empty());
        CHECK(loaded->second.title.empty());
    }
}

TEST("persist: load on a missing file is an Error") {
    test::TempDir td;
    auto r = load_conversation((td.path() / "nope.toml").string());
    CHECK(!r.has_value());
}

TEST("persist: load on garbage content is an Error, no crash") {
    test::TempDir td;
    std::string path = (td.path() / "junk.toml").string();
    td.write("junk.toml", "this is = = not [valid toml ][[");
    auto r = load_conversation(path);
    CHECK(!r.has_value());
}

TEST("persist: unknown role maps to User defensively") {
    test::TempDir td;
    std::string path = (td.path() / "weird.toml").string();
    td.write("weird.toml",
             "[meta]\n[[message]]\nrole = \"wizard\"\ncontent = \"hocus\"\n");
    auto r = load_conversation(path);
    CHECK(r.has_value());
    if (r && !r->empty()) {
        CHECK((*r)[0].role() == Role::User);
        CHECK_EQ((*r)[0].content(), std::string("hocus"));
    }
}

TEST("persist: settings round-trip") {
    test::TempDir td;
    std::string home = td.path().string();
    Settings s;
    s.base_url = "https://example.com/v1";
    s.model = "test-model";
    s.provider = "anthropic";
    s.context_window = 32000;
    s.max_iterations = 40;
    s.max_tokens = 12000;
    s.effort = "high";
    s.temperature = 0.7;
    s.thinking = 1;
    s.theme = "vivid";
    save_settings(home, s);
    Settings got = load_settings(home);
    CHECK_EQ(got.base_url, s.base_url);
    CHECK_EQ(got.model, s.model);
    CHECK_EQ(got.provider, std::string("anthropic"));
    CHECK_EQ(got.context_window, 32000);
    CHECK_EQ(got.max_iterations, 40);
    CHECK_EQ(got.max_tokens, 12000);
    CHECK_EQ(got.effort, std::string("high"));
    CHECK(got.temperature == 0.7);
    CHECK_EQ(got.thinking, 1);
    CHECK_EQ(got.theme, std::string("vivid"));
}

TEST("persist: settings thinking=off round-trips as 0 (not unset)") {
    test::TempDir td;
    std::string home = td.path().string();
    Settings s;
    s.thinking = 0;        // explicit off
    s.temperature = 0.0;   // explicit zero (valid, not "unset")
    save_settings(home, s);
    Settings got = load_settings(home);
    CHECK_EQ(got.thinking, 0);
    CHECK(got.temperature == 0.0);
}

TEST("settings_rtk_roundtrip") {
    test::TempDir td;
    std::string home = td.path().string();
    Settings s;
    s.rtk = 1;
    save_settings(home, s);
    CHECK_EQ(load_settings(home).rtk, 1);

    s.rtk = 0;
    save_settings(home, s);
    CHECK_EQ(load_settings(home).rtk, 0);

    s.rtk = -1;                       // unset => key omitted => stays -1 on reload
    save_settings(home, s);
    CHECK_EQ(load_settings(home).rtk, -1);
}

TEST("persist: settings missing file gives defaults") {
    test::TempDir td;
    Settings got = load_settings(td.path().string());
    CHECK(got.base_url.empty());
    CHECK(got.model.empty());
    CHECK_EQ(got.context_window, 0);
    CHECK_EQ(got.max_iterations, 0);
}

TEST("persist: malformed settings gives defaults, no crash") {
    test::TempDir td;
    td.write("settings.toml", "= = nonsense [[[");
    Settings got = load_settings(td.path().string());
    CHECK(got.model.empty());
    CHECK_EQ(got.context_window, 0);
}

TEST("persist: empty home disables settings (defaults, no write)") {
    Settings got = load_settings("");
    CHECK(got.model.empty());
    save_settings("", Settings{});  // must not crash
}

TEST("persist: profiles round-trip with the active selector and models") {
    test::TempDir td;
    std::string home = td.path().string();
    Settings s;
    s.base_url = "https://example.com/v1";  // scalars survive alongside profiles
    s.model = "scalar-model";
    s.profile = "work";
    s.profiles.push_back(Profile{.name = "work",
        .kind = "anthropic",
        .base_url = "https://api.anthropic.com/v1",
        .model = "claude-sonnet-4-6",
        .models = {"claude-sonnet-4-6", "claude-opus-4-5"}});
    s.profiles.push_back(Profile{.name = "home",
        .kind = "openai",
        .base_url = "https://api.openai.com/v1",
        .model = "gpt-5",
        .models = {"gpt-5", "gpt-5-mini"}});
    save_settings(home, s);

    Settings got = load_settings(home);
    CHECK_EQ(got.base_url, std::string("https://example.com/v1"));
    CHECK_EQ(got.model, std::string("scalar-model"));
    CHECK_EQ(got.profile, std::string("work"));
    CHECK_EQ(got.profiles.size(), std::size_t{2});
    if (got.profiles.size() == 2) {
        // Sorted by name: "home" before "work".
        CHECK_EQ(got.profiles[0].name, std::string("home"));
        CHECK_EQ(got.profiles[0].kind, std::string("openai"));
        CHECK_EQ(got.profiles[0].base_url, std::string("https://api.openai.com/v1"));
        CHECK_EQ(got.profiles[0].model, std::string("gpt-5"));
        CHECK_EQ(got.profiles[0].models.size(), std::size_t{2});
        CHECK_EQ(got.profiles[1].name, std::string("work"));
        CHECK_EQ(got.profiles[1].kind, std::string("anthropic"));
        CHECK_EQ(got.profiles[1].models.size(), std::size_t{2});
        if (got.profiles[1].models.size() == 2)
            CHECK_EQ(got.profiles[1].models[0], std::string("claude-sonnet-4-6"));
    }
}

TEST("persist: settings.toml never contains an api_key even with a profile") {
    test::TempDir td;
    std::string home = td.path().string();
    Settings s;
    s.profile = "work";
    s.profiles.push_back(Profile{.name = "work",
        .kind = "openai",
        .base_url = "https://api.openai.com/v1",
        .model = "gpt-5",
        .models = {"gpt-5"}});
    save_settings(home, s);
    save_credential(home, "work", "sk-supersecret-1234");

    std::string body = td.read("settings.toml");
    CHECK(body.find("api_key") == std::string::npos);
    CHECK(body.find("sk-supersecret-1234") == std::string::npos);
    // The key lives only in credentials.toml.
    std::string creds = td.read("credentials.toml");
    CHECK(creds.find("sk-supersecret-1234") != std::string::npos);
}

TEST("persist: credentials round-trip and upsert preserves other keys") {
    test::TempDir td;
    std::string home = td.path().string();
    save_credential(home, "work", "sk-work-aaaa");
    save_credential(home, "home", "sk-home-bbbb");
    auto got = load_credentials(home);
    CHECK_EQ(got.size(), std::size_t{2});
    CHECK_EQ(got["work"], std::string("sk-work-aaaa"));
    CHECK_EQ(got["home"], std::string("sk-home-bbbb"));

    // Upsert "work" without dropping "home".
    save_credential(home, "work", "sk-work-cccc");
    auto got2 = load_credentials(home);
    CHECK_EQ(got2.size(), std::size_t{2});
    CHECK_EQ(got2["work"], std::string("sk-work-cccc"));
    CHECK_EQ(got2["home"], std::string("sk-home-bbbb"));
}

TEST("persist: credentials missing file is an empty map") {
    test::TempDir td;
    auto got = load_credentials(td.path().string());
    CHECK(got.empty());
}

TEST("persist: empty home / empty name disables credentials") {
    auto got = load_credentials("");
    CHECK(got.empty());
    test::TempDir td;
    save_credential(td.path().string(), "", "sk-ignored");  // empty name => no-op
    CHECK(load_credentials(td.path().string()).empty());
    save_credential("", "work", "sk-ignored");  // empty home => no-op, no crash
}

TEST("persist: credentials.toml is written with 0600 permissions") {
    test::TempDir td;
    std::string home = td.path().string();
    save_credential(home, "work", "sk-work-1234");
    namespace fs = std::filesystem;
    std::error_code ec;
    auto perms = fs::status(home + "/credentials.toml", ec).permissions();
    CHECK(!ec);
    // Only owner read+write; no group/other bits.
    CHECK((perms & fs::perms::owner_read) != fs::perms::none);
    CHECK((perms & fs::perms::owner_write) != fs::perms::none);
    CHECK((perms & fs::perms::group_all) == fs::perms::none);
    CHECK((perms & fs::perms::others_all) == fs::perms::none);
    CHECK((perms & fs::perms::owner_exec) == fs::perms::none);
}

TEST("persist: permissions set round-trips") {
    test::TempDir td;
    std::string home = td.path().string();
    std::set<std::string> always{"read_file", "list_dir", "edit_file"};
    save_permissions(home, always);
    auto got = load_permissions(home);
    CHECK(got == always);
}

TEST("persist: permissions missing file is an empty set") {
    test::TempDir td;
    auto got = load_permissions(td.path().string());
    CHECK(got.empty());
}

TEST("persist: list_conversations orders most-recent-first") {
    test::TempDir td;
    std::string dir = (td.path() / "conversations").string();
    auto write_conv = [&](const std::string& name, const std::string& updated,
                          const std::string& cwd) {
        Conversation conv;
        conv.push_back(user("hi"));
        ConvMeta meta;
        meta.cwd = cwd;
        meta.updated = updated;
        meta.title = name;
        CHECK(save_conversation(dir + "/" + name + ".toml", conv, meta).has_value());
    };
    write_conv("old", "2026-06-06T10:00:00Z", "/proj");
    write_conv("mid", "2026-06-06T12:00:00Z", "/proj");
    write_conv("new", "2026-06-06T14:00:00Z", "/proj");

    auto sums = list_conversations(dir, "");
    CHECK_EQ(sums.size(), std::size_t{3});
    if (sums.size() == 3) {
        CHECK_EQ(sums[0].title, std::string("new"));
        CHECK_EQ(sums[1].title, std::string("mid"));
        CHECK_EQ(sums[2].title, std::string("old"));
        CHECK_EQ(sums[0].count, std::size_t{1});
    }
}

TEST("persist: list_conversations filters by cwd") {
    test::TempDir td;
    std::string dir = (td.path() / "conversations").string();
    auto write_conv = [&](const std::string& name, const std::string& cwd) {
        Conversation conv;
        conv.push_back(user("hi"));
        ConvMeta meta;
        meta.cwd = cwd;
        meta.updated = "2026-06-06T10:00:00Z";
        meta.title = name;
        CHECK(save_conversation(dir + "/" + name + ".toml", conv, meta).has_value());
    };
    write_conv("here", "/proj/a");
    write_conv("there", "/proj/b");

    auto sums = list_conversations(dir, "/proj/a");
    CHECK_EQ(sums.size(), std::size_t{1});
    if (sums.size() == 1) CHECK_EQ(sums[0].title, std::string("here"));
}

TEST("persist: list_conversations on a missing dir is empty") {
    test::TempDir td;
    auto sums = list_conversations((td.path() / "nope").string(), "");
    CHECK(sums.empty());
}

TEST("persist: list_conversations skips garbage files") {
    test::TempDir td;
    std::string dir = (td.path() / "conversations").string();
    Conversation conv;
    conv.push_back(user("hi"));
    ConvMeta meta;
    meta.cwd = "/p";
    meta.updated = "2026-06-06T10:00:00Z";
    meta.title = "good";
    CHECK(save_conversation(dir + "/good.toml", conv, meta).has_value());
    std::ofstream(dir + "/bad.toml", std::ios::trunc) << "][ not toml = =";

    auto sums = list_conversations(dir, "");
    CHECK_EQ(sums.size(), std::size_t{1});
    if (sums.size() == 1) CHECK_EQ(sums[0].title, std::string("good"));
}

TEST("persist: new_conversation_id is sortable and stable per cwd") {
    std::string a = new_conversation_id("/home/x");
    std::string b = new_conversation_id("/home/x");
    // Same second + same cwd => identical id (date-time prefix + hash).
    CHECK_EQ(a.size(), b.size());
    // Hash suffix is deterministic for the same cwd.
    CHECK_EQ(a.substr(a.size() - 8), b.substr(b.size() - 8));
    // Different cwd changes the hash suffix.
    std::string c = new_conversation_id("/home/y");
    CHECK(c.substr(c.size() - 8) != a.substr(a.size() - 8));
}

TEST("persist: conversations_dir and flagent_home honor empty home") {
    CHECK(conversations_dir("").empty());
    CHECK(!conversations_dir("/x").empty());
}
