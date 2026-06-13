#ifndef FLAGENT_PROVIDER_FACTORY_HPP
#define FLAGENT_PROVIDER_FACTORY_HPP

// Single source of truth for building a concrete Provider from a resolved
// connection. main() uses it at startup and the TUI uses it when /model or
// /provider switches wire format at runtime (a wire-format change needs a fresh
// provider object, not just a base_url tweak). Lives in agent_provider because
// it needs both backends' configs.

#include <memory>
#include <string>

#include "agent/anthropic_provider.hpp"  // ProviderKind
#include "agent/provider.hpp"            // Provider, GenerationParams

namespace flagent {

// A fully-resolved connection: everything needed to build a Provider of either
// wire format. base_url must already be normalized (no trailing slash).
struct ProviderConnection {
    ProviderKind kind = ProviderKind::OpenAI;
    std::string base_url;
    std::string api_key;
    std::string model;
    int max_tokens = 0;  // 0 => provider default
};

// Build the concrete Provider for `c.kind`, applying `gp` (set_params) before
// returning. pre: c.base_url normalized. post: non-null owning pointer.
std::unique_ptr<Provider> make_provider(const ProviderConnection& c,
                                        const GenerationParams& gp);

}  // namespace flagent

#endif  // FLAGENT_PROVIDER_FACTORY_HPP
