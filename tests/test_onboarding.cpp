#include "agent/onboarding.hpp"

#include <sstream>
#include <string>
#include <vector>

#include "agent/persist.hpp"
#include "temp_dir.hpp"
#include "test_harness.hpp"

using namespace moocode;

namespace {

// A fake ModelLister that records the connections it was probed with and replies
// from a scripted sequence of results (so we can drive retry/adapt paths).
struct FakeLister {
    std::vector<ProviderConnection> seen;
    std::vector<std::expected<std::vector<std::string>, Error>> replies;
    std::size_t idx = 0;

    ModelLister fn() {
        return [this](const ProviderConnection& c)
                   -> std::expected<std::vector<std::string>, Error> {
            seen.push_back(c);
            if (idx < replies.size()) return replies[idx++];
            return std::unexpected(Error{.msg = "no more replies", .code = 0});
        };
    }
};

}  // namespace

TEST("onboarding_needed: gated on interactive, home, explicit config, file") {
    test::TempDir td;
    const std::string home = td.path().string();

    // Clean slate: needed when interactive, home present, no explicit config.
    CHECK(onboarding_needed(home, /*interactive=*/true, /*explicit=*/false));
    // Not interactive => never.
    CHECK(!onboarding_needed(home, false, false));
    // Explicit config (flag/env) => never.
    CHECK(!onboarding_needed(home, true, true));
    // No home => never (persistence disabled).
    CHECK(!onboarding_needed("", true, false));

    // Once settings.toml exists, it is no longer needed.
    td.write("settings.toml", "model = \"x\"\n");
    CHECK(!onboarding_needed(home, true, false));
}

TEST("onboarding_settings: builds one active profile mirroring top-level") {
    Settings s = onboarding_settings("anthropic", "anthropic",
                                     "https://api.anthropic.com/v1",
                                     "claude-sonnet-4-6",
                                     {"claude-sonnet-4-6", "claude-opus-4-5"});
    CHECK_EQ(s.profile, std::string("anthropic"));
    CHECK_EQ(s.model, std::string("claude-sonnet-4-6"));
    CHECK_EQ(s.provider, std::string("anthropic"));
    CHECK_EQ(s.profiles.size(), std::size_t{1});
    if (s.profiles.size() == 1) {
        const Profile& p = s.profiles.front();
        CHECK_EQ(p.name, std::string("anthropic"));
        CHECK_EQ(p.kind, std::string("anthropic"));
        CHECK_EQ(p.base_url, std::string("https://api.anthropic.com/v1"));
        CHECK_EQ(p.model, std::string("claude-sonnet-4-6"));
        CHECK_EQ(p.models.size(), std::size_t{2});
    }
}

TEST("run_onboarding: preset happy path persists settings + credentials") {
    test::TempDir td;
    const std::string home = td.path().string();

    FakeLister lister;
    lister.replies.push_back(std::vector<std::string>{"MiniMax-M3", "MiniMax-M1"});

    // Input: choose preset 1, accept default endpoint, enter key, default model.
    // builtin_profiles()[0] is "minimax" (openai, api.minimax.io).
    std::istringstream in("1\n\nsk-test-123\n\n");
    std::ostringstream out;

    OnboardingOutcome o =
        run_onboarding(home, in, out, lister.fn());

    CHECK(o.completed);
    CHECK(!o.cancelled);

    // The lister was given the minimax endpoint + the typed key.
    CHECK_EQ(lister.seen.size(), std::size_t{1});
    if (!lister.seen.empty()) {
        CHECK_EQ(lister.seen[0].api_key, std::string("sk-test-123"));
        CHECK(lister.seen[0].base_url.find("minimax") != std::string::npos);
    }

    // settings.toml + credentials.toml were written and round-trip.
    Settings s = load_settings(home);
    CHECK_EQ(s.profile, std::string("minimax"));
    CHECK_EQ(s.profiles.size(), std::size_t{1});
    if (!s.profiles.empty())
        CHECK_EQ(s.profiles.front().model, std::string("MiniMax-M3"));

    auto creds = load_credentials(home);
    CHECK_EQ(creds["minimax"], std::string("sk-test-123"));
    CHECK_EQ(o.model, std::string("MiniMax-M3"));
}

TEST("run_onboarding: failed probe then adapted endpoint succeeds") {
    test::TempDir td;
    const std::string home = td.path().string();

    FakeLister lister;
    lister.replies.push_back(
        std::unexpected(Error{.msg = "HTTP 401: bad key", .code = 401}));
    lister.replies.push_back(std::vector<std::string>{"gpt-5", "gpt-5-mini"});

    // Choose preset 4 (openai), bad key first; probe fails; retry yes; supply a
    // new endpoint + key; probe succeeds; pick model #2.
    // Sequence after "4":
    //   endpoint [default] -> Enter
    //   wire format? (preset carries kind, so NOT asked)
    //   API key -> "badkey"
    //   retry? -> "y"
    //   endpoint -> "https://api.openai.com/v1" (re-typed, same)
    //   API key [keep current] -> "goodkey"
    //   model -> "2"
    std::istringstream in(
        "4\n"
        "\n"
        "badkey\n"
        "y\n"
        "https://api.openai.com/v1\n"
        "goodkey\n"
        "2\n");
    std::ostringstream out;

    OnboardingOutcome o = run_onboarding(home, in, out, lister.fn());

    CHECK(o.completed);
    CHECK_EQ(lister.seen.size(), std::size_t{2});
    if (lister.seen.size() == 2) {
        CHECK_EQ(lister.seen[0].api_key, std::string("badkey"));
        CHECK_EQ(lister.seen[1].api_key, std::string("goodkey"));
    }
    CHECK_EQ(o.model, std::string("gpt-5-mini"));

    auto creds = load_credentials(home);
    CHECK_EQ(creds["openai"], std::string("goodkey"));
}

TEST("run_onboarding: skip at first prompt writes nothing") {
    test::TempDir td;
    const std::string home = td.path().string();

    FakeLister lister;  // never consulted
    std::istringstream in("s\n");
    std::ostringstream out;

    OnboardingOutcome o = run_onboarding(home, in, out, lister.fn());

    CHECK(o.cancelled);
    CHECK(!o.completed);
    CHECK(lister.seen.empty());
    // No settings.toml written.
    CHECK(!std::filesystem::exists(td.path() / "settings.toml"));
}

TEST("run_onboarding: EOF mid-flow cancels cleanly without writing") {
    test::TempDir td;
    const std::string home = td.path().string();

    FakeLister lister;
    // Choose preset 1, then stream ends (EOF) at the endpoint prompt.
    std::istringstream in("1\n");
    std::ostringstream out;

    OnboardingOutcome o = run_onboarding(home, in, out, lister.fn());

    CHECK(o.cancelled);
    CHECK(!o.completed);
    CHECK(!std::filesystem::exists(td.path() / "settings.toml"));
}

TEST("run_onboarding: custom endpoint asks for wire format") {
    test::TempDir td;
    const std::string home = td.path().string();

    FakeLister lister;
    lister.replies.push_back(std::vector<std::string>{"local-model"});

    // 'c' custom; endpoint; wire format (accept detected default for an
    // anthropic host); key; model (type a name not in the list).
    std::istringstream in(
        "c\n"
        "https://api.anthropic.com/v1\n"
        "\n"            // accept detected wire format
        "key\n"
        "my-model\n");
    std::ostringstream out;

    OnboardingOutcome o = run_onboarding(home, in, out, lister.fn());

    CHECK(o.completed);
    CHECK_EQ(o.profile, std::string("custom"));
    CHECK_EQ(o.model, std::string("my-model"));
    if (!lister.seen.empty())
        CHECK(lister.seen[0].kind == ProviderKind::Anthropic);

    Settings s = load_settings(home);
    CHECK_EQ(s.provider, std::string("anthropic"));
}
