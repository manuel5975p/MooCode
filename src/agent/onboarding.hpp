#ifndef MOOCODE_ONBOARDING_HPP
#define MOOCODE_ONBOARDING_HPP

// First-run setup wizard. When no settings.toml exists yet and the user has not
// configured an endpoint/key through a flag or LLM_* env var, walk them through
// picking a provider (or a custom endpoint), entering an API key, probing the
// endpoint's model list, and adapting the endpoint until the probe succeeds.
// The result is persisted as settings.toml + credentials.toml under ~/.moo, so
// main()'s normal config-resolution layer transparently picks it up afterwards.
//
// The pure pieces (onboarding_needed, onboarding_settings) are screen- and
// network-free so they unit-test directly; the interactive driver injects its
// input/output streams and the model-listing call so the whole flow is testable
// offline against a fake lister.

#include <expected>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "agent/persist.hpp"           // Settings, Profile
#include "agent/provider_factory.hpp"  // ProviderConnection
#include "agent/types.hpp"             // Error

namespace moocode {

// Probe a candidate endpoint: build a throwaway provider for `conn` and return
// the model ids it advertises, or an Error when the listing call fails. Injected
// into the driver so tests can substitute a deterministic fake.
using ModelLister =
    std::function<std::expected<std::vector<std::string>, Error>(
        const ProviderConnection&)>;

// The default ModelLister: make_provider(conn) then list_models(). The live wire.
std::expected<std::vector<std::string>, Error> default_model_lister(
    const ProviderConnection& conn);

// Whether the first-run wizard should run: interactive terminal, a usable home
// directory to persist into, no settings.toml yet, and no endpoint/key already
// supplied via flag/env (`has_explicit_config`). Pure but for the file probe.
bool onboarding_needed(const std::string& home, bool interactive,
                       bool has_explicit_config);

// Build the Settings to persist from the wizard's resolved answers: a single
// [profiles.<name>] table (kind/base_url/model + discovered model ids) selected
// as the active profile, plus the mirrored top-level model/provider. The API key
// is NOT part of this — it is returned separately by the driver and written to
// credentials.toml so secrets never enter settings.toml. Pure, no I/O.
Settings onboarding_settings(const std::string& profile_name,
                             const std::string& kind,
                             const std::string& base_url,
                             const std::string& model,
                             const std::vector<std::string>& models);

// Outcome of the wizard. `completed` => settings.toml + credentials.toml were
// written under the home dir. `cancelled` => the user skipped or hit EOF and
// nothing was written; main() then proceeds with its built-in defaults.
struct OnboardingOutcome {
    bool completed = false;
    bool cancelled = false;
    std::string profile;  // active profile name written (when completed)
    std::string model;    // chosen model (when completed)
};

// Run the interactive wizard, prompting on `out` and reading from `in`, probing
// endpoints through `lister`. On success persists settings.toml + credentials.toml
// under `home`. Total: never throws; EOF or an explicit skip yields a cancelled
// outcome. pre: home non-empty (callers gate via onboarding_needed).
OnboardingOutcome run_onboarding(const std::string& home, std::istream& in,
                                 std::ostream& out, const ModelLister& lister);

}  // namespace moocode

#endif  // MOOCODE_ONBOARDING_HPP
