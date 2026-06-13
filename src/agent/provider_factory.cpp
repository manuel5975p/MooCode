#include "agent/provider_factory.hpp"

#include <utility>

#include "agent/gemini_provider.hpp"
#include "agent/openai_provider.hpp"

namespace flagent {

std::unique_ptr<Provider> make_provider(const ProviderConnection& c,
                                        const GenerationParams& gp) {
    std::unique_ptr<Provider> p;
    if (c.kind == ProviderKind::Anthropic) {
        AnthropicConfig ac;
        ac.base_url = c.base_url;
        ac.api_key = c.api_key;
        ac.model = c.model;
        if (c.max_tokens > 0) ac.max_tokens = c.max_tokens;
        p = std::make_unique<AnthropicProvider>(std::move(ac));
    } else if (c.kind == ProviderKind::Gemini) {
        GeminiConfig gc;
        gc.base_url = c.base_url;
        gc.api_key = c.api_key;
        gc.model = c.model;
        if (c.max_tokens > 0) gc.max_tokens = c.max_tokens;
        p = std::make_unique<GeminiProvider>(std::move(gc));
    } else {
        OpenAiConfig oc;
        oc.base_url = c.base_url;
        oc.api_key = c.api_key;
        oc.model = c.model;
        if (c.max_tokens > 0) oc.max_tokens = c.max_tokens;
        p = std::make_unique<OpenAiProvider>(std::move(oc));
    }
    p->set_params(gp);
    return p;
}

}  // namespace flagent
