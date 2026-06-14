#include "agent/provider_factory.hpp"

#include <utility>

#include "agent/gemini_provider.hpp"
#include "agent/openai_provider.hpp"

namespace moocode {

std::unique_ptr<Provider> make_provider(const ProviderConnection& c,
                                        const GenerationParams& gp) {
    std::unique_ptr<Provider> p;
    if (c.kind == ProviderKind::Anthropic) {
        AnthropicConfig ac(c);
        if (c.max_tokens <= 0) ac.max_tokens = 8192;  // Anthropic requires max_tokens
        p = std::make_unique<AnthropicProvider>(std::move(ac));
    } else if (c.kind == ProviderKind::Gemini) {
        GeminiConfig gc(c);
        if (c.max_tokens <= 0) gc.max_tokens = 0;  // Gemini default
        p = std::make_unique<GeminiProvider>(std::move(gc));
    } else {
        OpenAiConfig oc(c);
        if (c.max_tokens <= 0) oc.max_tokens = 0;  // OpenAI default
        p = std::make_unique<OpenAiProvider>(std::move(oc));
    }
    p->set_params(gp);
    return p;
}

}  // namespace moocode
