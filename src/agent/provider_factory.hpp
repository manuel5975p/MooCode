#ifndef MOOCODE_PROVIDER_FACTORY_HPP
#define MOOCODE_PROVIDER_FACTORY_HPP

// Single source of truth for building a concrete Provider from a resolved
// connection. main() uses it at startup and the TUI uses it when /model or
// /provider switches wire format at runtime (a wire-format change needs a fresh
// provider object, not just a base_url tweak). Lives in agent_provider because
// it needs both backends' configs.

#include <concepts>
#include <memory>
#include <ranges>
#include <string>

#include "agent/anthropic_provider.hpp"
#include "agent/provider.hpp"
#include "agent/strutil.hpp"

namespace moocode {

// The duck-typed contract the templated profile helpers below require: string
// .kind/.base_url/.model members and a .models range. Stated as a concept so
// this layer documents the contract and yields readable diagnostics without a
// dependency on agent_persist's Profile (the layering keeps persist
// provider-agnostic). The real Profile satisfies it (verified at build time).
// Lives in provider_factory.hpp (not anthropic_provider.hpp) because only
// main.cpp and tui.cpp call these helpers — putting them here avoids pulling
// <concepts>+<ranges> into every provider backend's translation unit.
template <class P>
concept ProfileLike = requires(const P& p) {
    { p.kind }     -> std::convertible_to<std::string_view>;
    { p.base_url } -> std::convertible_to<std::string_view>;
    { p.model }    -> std::convertible_to<std::string_view>;
    { p.models }   -> std::ranges::range;
};

template <ProfileLike ProfileT>
ProviderKind profile_kind(const ProfileT& p) {
    ProviderChoice c = parse_provider_choice(p.kind);
    if (c == ProviderChoice::Anthropic) return ProviderKind::Anthropic;
    if (c == ProviderChoice::OpenAI) return ProviderKind::OpenAI;
    if (c == ProviderChoice::Gemini) return ProviderKind::Gemini;
    return detect_provider_kind(p.base_url, p.model);
}

template <ProfileLike ProfileT>
const ProfileT* find_model_profile(std::string_view name,
                                   const std::vector<ProfileT>& profiles) {
    const std::string want = to_lower(name);
    for (const ProfileT& p : profiles)
        for (const auto& m : p.models)
            if (to_lower(m) == want) return &p;
    return nullptr;
}

// A fully-resolved connection: everything needed to build a Provider of either
// wire format. base_url must already be normalized (no trailing slash).
struct ProviderConnection {
    ProviderKind kind = ProviderKind::OpenAI;
    std::string base_url;
    std::string api_key;
    std::string model;
    int max_tokens = 0;  // 0 => provider default
    // thinking.type value the OpenAI backend emits when reasoning is ON.
    // "enabled" (DeepSeek convention) by default; "adaptive" for MiniMax, which
    // rejects "enabled". Empty => provider default ("enabled").
    std::string thinking_type;
};

// Build the concrete Provider for `c.kind`, applying `gp` (set_params) before
// returning. pre: c.base_url normalized. post: non-null owning pointer.
std::unique_ptr<Provider> make_provider(const ProviderConnection& c,
                                        const GenerationParams& gp);

}  // namespace moocode

#endif  // MOOCODE_PROVIDER_FACTORY_HPP
