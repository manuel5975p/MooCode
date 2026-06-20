#include "agent/settings_editor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "agent/strutil.hpp"  // to_lower

#include "agent/persist.hpp"  // toml_check_syntax

namespace moocode {

namespace {

// Format a double to one decimal place, e.g. 0.7 -> "0.7".
std::string float_str(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

// Layout: the 12 General-tab fields in display order.
constexpr int kFieldProvider       = 0;
constexpr int kFieldModel          = 1;
constexpr int kFieldBaseUrl        = 2;
constexpr int kFieldContextWindow  = 3;
constexpr int kFieldMaxIterations  = 4;
constexpr int kFieldMaxTokens      = 5;
constexpr int kFieldEffort         = 6;
constexpr int kFieldTemperature    = 7;
constexpr int kFieldThinking       = 8;
constexpr int kFieldRtk            = 9;
constexpr int kFieldTheme          = 10;
constexpr int kFieldProfile        = 11;

// Profile detail fields (inline labels).
struct ProfileField {
    std::string label;
    enum { SName, SKind, SBaseUrl, SModel, SThinking, SDropThinking, SThinkingType } kind;
};
const ProfileField kProfileFields[] = {
    {"name",          ProfileField::SName},
    {"kind",          ProfileField::SKind},
    {"base_url",      ProfileField::SBaseUrl},
    {"model",         ProfileField::SModel},
    {"thinking",      ProfileField::SThinking},
    {"drop_think",    ProfileField::SDropThinking},
    {"thinking_type", ProfileField::SThinkingType},
};

// Valid provider kind values.
bool valid_provider_kind(std::string_view s) {
    if (s.empty()) return true;
    std::string ls(s);
    // to_lower is our normaliser
    // allow "openai", "anthropic", "gemini"
    auto low = to_lower(s);
    return low == "openai" || low == "anthropic" || low == "gemini";
}

// Check if a string contains control characters.
bool has_control_chars(std::string_view s) {
    for (char c : s)
        if (static_cast<unsigned char>(c) < 0x20) return true;
    return false;
}

}  // namespace

// --- ProfileEditor ----------------------------------------------------------

void ProfileEditor::move_up() {
    if (sel > 0) --sel;
}

void ProfileEditor::move_down() {
    if (sel + 1 < static_cast<int>(profiles.size())) ++sel;
}

void ProfileEditor::add_profile() {
    Profile p;
    p.name = "new";
    profiles.push_back(std::move(p));
    sel = static_cast<int>(profiles.size()) - 1;
    begin_edit_profile();
}

void ProfileEditor::delete_profile(int idx) {
    if (idx < 0 || idx >= static_cast<int>(profiles.size())) return;
    profiles.erase(profiles.begin() + idx);
    dirty = true;
    if (sel >= static_cast<int>(profiles.size()) && sel > 0)
        --sel;
}

void ProfileEditor::begin_edit_profile() {
    if (sel < 0 || sel >= static_cast<int>(profiles.size())) return;
    editing_profile = true;
    profile_field_sel = 0;
    editing_models = false;
    profile_edit_text_mode = false;
}

void ProfileEditor::commit_profile_edit() {
    editing_profile = false;
    profile_edit_text_mode = false;
    dirty = true;
}

void ProfileEditor::cancel_profile_edit() {
    editing_profile = false;
    profile_edit_text_mode = false;
    edit_buf.clear();
}

void ProfileEditor::add_model() {
    if (sel < 0 || sel >= static_cast<int>(profiles.size())) return;
    profiles[sel].models.push_back("");
    model_sel = static_cast<int>(profiles[sel].models.size()) - 1;
    dirty = true;
}

void ProfileEditor::delete_model(int idx) {
    if (sel < 0 || sel >= static_cast<int>(profiles.size())) return;
    auto& models = profiles[sel].models;
    if (idx < 0 || idx >= static_cast<int>(models.size())) return;
    models.erase(models.begin() + idx);
    if (model_sel >= static_cast<int>(models.size()) && model_sel > 0)
        --model_sel;
    dirty = true;
}

void ProfileEditor::begin_edit_model() {
    if (sel < 0 || sel >= static_cast<int>(profiles.size())) return;
    const auto& models = profiles[sel].models;
    if (model_sel < 0 || model_sel >= static_cast<int>(models.size())) return;
    model_edit_buf = models[model_sel];
}

void ProfileEditor::commit_model_edit() {
    if (sel < 0 || sel >= static_cast<int>(profiles.size())) return;
    auto& models = profiles[sel].models;
    if (model_sel < 0 || model_sel >= static_cast<int>(models.size())) return;
    models[model_sel] = model_edit_buf;
    model_edit_buf.clear();
    dirty = true;
}

void ProfileEditor::cancel_model_edit() {
    model_edit_buf.clear();
}

void ProfileEditor::profile_field_up() {
    if (profile_field_sel > 0) --profile_field_sel;
}

void ProfileEditor::profile_field_down() {
    if (profile_field_sel + 1 < kNumProfileFields + 1)
        ++profile_field_sel;
}

void ProfileEditor::begin_edit_profile_field() {
    if (sel < 0 || sel >= static_cast<int>(profiles.size())) return;
    Profile& p = profiles[sel];
    if (profile_field_sel < 0) return;
    if (profile_field_sel < kNumProfileFields) {
        // Edit a profile field.
        switch (kProfileFields[profile_field_sel].kind) {
        case ProfileField::SName:     edit_buf = p.name; break;
        case ProfileField::SKind:     edit_buf = p.kind; break;
        case ProfileField::SBaseUrl:  edit_buf = p.base_url; break;
        case ProfileField::SModel:    edit_buf = p.model; break;
        case ProfileField::SThinking:    edit_buf = (p.thinking < 0) ? "" : (p.thinking > 0 ? "on" : "off"); break;
        case ProfileField::SDropThinking: edit_buf = p.drop_thinking_tag ? "yes" : "no"; break;
        case ProfileField::SThinkingType: edit_buf = p.thinking_type.empty() ? "enabled" : p.thinking_type; break;
        }
        edit_field_idx = profile_field_sel;
        profile_edit_text_mode = true;
    } else {
        // "models" row: enter models sub-editor.
        editing_models = true;
        model_sel = 0;
    }
}

void ProfileEditor::commit_profile_field_edit() {
    if (sel < 0 || sel >= static_cast<int>(profiles.size())) return;
    if (edit_field_idx < 0 || edit_field_idx >= kNumProfileFields) return;
    Profile& p = profiles[sel];
    switch (kProfileFields[edit_field_idx].kind) {
    case ProfileField::SName:     p.name = edit_buf; break;
    case ProfileField::SKind:     p.kind = edit_buf; break;
    case ProfileField::SBaseUrl:  p.base_url = edit_buf; break;
    case ProfileField::SModel:    p.model = edit_buf; break;
    case ProfileField::SThinking:
        if (edit_buf == "on") p.thinking = 1;
        else if (edit_buf == "off") p.thinking = 0;
        else p.thinking = -1;
        break;
    case ProfileField::SDropThinking:
        p.drop_thinking_tag = (edit_buf == "yes" || edit_buf == "true" || edit_buf == "on" || edit_buf == "1");
        break;
    case ProfileField::SThinkingType:
        p.thinking_type = edit_buf;
        break;
    }
    profile_edit_text_mode = false;
    edit_field_idx = -1;
    edit_buf.clear();
    dirty = true;
}

void ProfileEditor::cancel_profile_field_edit() {
    profile_edit_text_mode = false;
    edit_field_idx = -1;
    edit_buf.clear();
}

ProfileEditor profile_editor_build(const std::vector<Profile>& profiles) {
    ProfileEditor pe;
    pe.profiles = profiles;
    return pe;
}

std::string profile_field_label(int i) {
    if (i < 0 || i >= kNumProfileFields) return {};
    return kProfileFields[i].label;
}

std::string profile_field_value(const Profile& p, int field_idx) {
    switch (field_idx) {
    case kPFieldName:     return p.name;
    case kPFieldKind:     return p.kind.empty() ? "(auto)" : p.kind;
    case kPFieldBaseUrl:  return p.base_url;
    case kPFieldModel:    return p.model;
    case kPFieldThinking:
        if (p.thinking < 0) return "(unset)";
        return p.thinking > 0 ? "on" : "off";
    case kPFieldDropThinking: return p.drop_thinking_tag ? "yes" : "no";
    case kPFieldThinkingType: return p.thinking_type.empty() ? "enabled" : p.thinking_type;
    default:              return {};
    }
}

// --- CredentialEditor --------------------------------------------------------

void CredentialEditor::move_up() {
    if (sel > 0) --sel;
}

void CredentialEditor::move_down() {
    if (sel + 1 < static_cast<int>(profile_names.size())) ++sel;
}

void CredentialEditor::begin_edit_key() {
    if (sel < 0 || sel >= static_cast<int>(profile_names.size())) return;
    const std::string& name = profile_names[sel];
    auto it = credentials.find(name);
    key_edit_buf = (it != credentials.end()) ? it->second : "";
    editing_key = true;
}

void CredentialEditor::commit_key() {
    if (sel < 0 || sel >= static_cast<int>(profile_names.size())) return;
    credentials[profile_names[sel]] = key_edit_buf;
    deleted.erase(profile_names[sel]);
    key_edit_buf.clear();
    editing_key = false;
    dirty = true;
}

void CredentialEditor::cancel_key() {
    key_edit_buf.clear();
    editing_key = false;
}

void CredentialEditor::delete_key() {
    if (sel < 0 || sel >= static_cast<int>(profile_names.size())) return;
    const std::string& name = profile_names[sel];
    credentials.erase(name);
    deleted.insert(name);
    dirty = true;
}

std::string CredentialEditor::display(const std::string& profile_name) const {
    auto it = credentials.find(profile_name);
    if (it == credentials.end() || it->second.empty())
        return "(not set)";
    const std::string& key = it->second;
    if (key.size() <= 4)
        return "(set)";  // never reveal short keys
    // "sk-…XXXX": first 3, …, last 4.
    return key.substr(0, 3) + "\xe2\x80\xa6" + key.substr(key.size() - 4);
}

CredentialEditor credential_editor_build(
    const std::map<std::string, std::string>& creds,
    const std::vector<Profile>& profiles) {
    CredentialEditor ce;
    ce.credentials = creds;
    for (const Profile& p : profiles) ce.profile_names.push_back(p.name);
    return ce;
}

// --- SettingsForm -----------------------------------------------------------

void SettingsForm::rebuild(const Settings& current) {
    draft = current;
    editing = false;
    edit_buf.clear();
    dirty = false;
    sel = 0;
    current_tab = Tab::General;
    profile_editor = ProfileEditor{};
    credential_editor = CredentialEditor{};
    fields.clear();
    fields.push_back({"provider", FieldDef::Type::Choice,
                      {"openai", "anthropic", "auto"}});
    fields.push_back({"model", FieldDef::Type::String, {}});
    fields.push_back({"base_url", FieldDef::Type::String, {}});
    fields.push_back({"context_window", FieldDef::Type::Int, {}, "(unset)"});
    fields.push_back({"max_iterations", FieldDef::Type::Int, {}, "(unset)"});
    fields.push_back({"max_tokens", FieldDef::Type::Int, {}, "(unset)"});
    fields.push_back({"effort", FieldDef::Type::Choice,
                      {"(unset)", "none", "low", "medium", "high"}});
    fields.push_back({"temperature", FieldDef::Type::Double, {}, "(unset)"});
    fields.push_back({"thinking", FieldDef::Type::BoolToggle,
                      {"unset", "on", "off"}});
    fields.push_back({"rtk", FieldDef::Type::BoolToggle,
                      {"unset", "on", "off"}});
    fields.push_back({"theme", FieldDef::Type::Choice,
                      {"(unset)", "default", "mono", "vivid", "none"}});
    fields.push_back({"profile", FieldDef::Type::String, {}, "(none)"});
}

void SettingsForm::detect_malformed(const std::string& home) {
    malformed_file = false;
    malformed_msg.clear();
    if (home.empty()) return;
    std::string path = home + "/settings.toml";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string body(std::istreambuf_iterator<char>(f), {});
    if (body.empty()) return;
    if (auto err = toml_check_syntax(body)) {
        malformed_file = true;
        malformed_msg = std::move(*err);
    }
}

std::string SettingsForm::display_value(int idx,
                                        std::string_view live_model,
                                        std::string_view live_provider) const {
    (void)live_provider;
    if (idx < 0 || idx >= static_cast<int>(fields.size())) return {};
    switch (idx) {
    case kFieldProvider:
        if (draft.provider.empty()) return "auto";
        return draft.provider;
    case kFieldModel:
        if (draft.model.empty()) return std::string(live_model);
        return draft.model;
    case kFieldBaseUrl:
        return draft.base_url;
    case kFieldContextWindow:
        return draft.context_window > 0
                   ? std::to_string(draft.context_window)
                   : fields[idx].unset_label;
    case kFieldMaxIterations:
        return draft.max_iterations > 0
                   ? std::to_string(draft.max_iterations)
                   : fields[idx].unset_label;
    case kFieldMaxTokens:
        return draft.max_tokens > 0
                   ? std::to_string(draft.max_tokens)
                   : fields[idx].unset_label;
    case kFieldEffort:
        return draft.effort.empty() ? fields[idx].unset_label : draft.effort;
    case kFieldTemperature:
        return draft.temperature < 0
                   ? fields[idx].unset_label
                   : float_str(draft.temperature);
    case kFieldThinking:
        if (draft.thinking < 0) return "unset";
        return draft.thinking > 0 ? "on" : "off";
    case kFieldRtk:
        if (draft.rtk < 0) return "unset";
        return draft.rtk > 0 ? "on" : "off";
    case kFieldTheme:
        return draft.theme.empty() ? fields[idx].unset_label : draft.theme;
    case kFieldProfile:
        return draft.profile.empty() ? fields[idx].unset_label : draft.profile;
    default:
        return {};
    }
}

bool SettingsForm::apply_value(int idx, std::string_view val) {
    if (idx < 0 || idx >= static_cast<int>(fields.size())) return false;

    const auto& f = fields[idx];
    if (f.type == FieldDef::Type::Int) {
        std::string vs(val);
        char* end = nullptr;
        std::strtol(vs.c_str(), &end, 10);
        if (end != vs.c_str() + vs.size()) return false;
    } else if (f.type == FieldDef::Type::Double) {
        std::string vs(val);
        if (vs == f.unset_label || vs.empty()) {
            // Allow reset via unset label.
        } else {
            char* end = nullptr;
            std::strtod(vs.c_str(), &end);
            if (end != vs.c_str() + vs.size()) return false;
        }
    } else if (f.type == FieldDef::Type::Choice || f.type == FieldDef::Type::BoolToggle) {
        if (std::ranges::find(f.choices, val) == f.choices.end()) return false;
    }

    dirty = true;

    switch (idx) {
    case kFieldProvider:
        draft.provider = (val == "auto") ? "" : std::string(val);
        break;
    case kFieldModel:
        draft.model = val;
        break;
    case kFieldBaseUrl:
        draft.base_url = val;
        break;
    case kFieldContextWindow:
        draft.context_window = std::max(0, static_cast<int>(std::atoi(std::string(val).c_str())));
        break;
    case kFieldMaxIterations:
        draft.max_iterations = std::max(0, static_cast<int>(std::atoi(std::string(val).c_str())));
        break;
    case kFieldMaxTokens:
        draft.max_tokens = std::max(0, static_cast<int>(std::atoi(std::string(val).c_str())));
        break;
    case kFieldEffort:
        draft.effort = (val == fields[idx].unset_label) ? "" : std::string(val);
        break;
    case kFieldTemperature:
        if (val == fields[idx].unset_label || val.empty())
            draft.temperature = -1;
        else
            draft.temperature = std::strtod(std::string(val).c_str(), nullptr);
        break;
    case kFieldThinking:
        if (val == "on") draft.thinking = 1;
        else if (val == "off") draft.thinking = 0;
        else draft.thinking = -1;
        break;
    case kFieldRtk:
        if (val == "on") draft.rtk = 1;
        else if (val == "off") draft.rtk = 0;
        else draft.rtk = -1;
        break;
    case kFieldTheme:
        draft.theme = (val == fields[idx].unset_label) ? "" : std::string(val);
        break;
    case kFieldProfile:
        draft.profile = (val == fields[idx].unset_label) ? "" : std::string(val);
        break;
    default:
        return false;
    }
    return true;
}

void SettingsForm::begin_edit() {
    if (sel < 0 || sel >= static_cast<int>(fields.size())) return;
    const auto& f = fields[sel];
    if (f.type == FieldDef::Type::Choice || f.type == FieldDef::Type::BoolToggle) {
        const std::string cur = display_value(sel, "", "");
        auto it = std::ranges::find(f.choices, cur);
        std::size_t idx = (it == f.choices.end()) ? 0
            : static_cast<std::size_t>(it - f.choices.begin());
        idx = (idx + 1) % f.choices.size();
        apply_value(sel, f.choices[idx]);
    } else {
        editing = true;
        edit_buf = display_value(sel, "", "");
        if (edit_buf == f.unset_label) edit_buf.clear();
    }
}

void SettingsForm::commit_edit() {
    if (sel < 0 || sel >= static_cast<int>(fields.size())) return;
    const auto& f = fields[sel];
    if (f.type == FieldDef::Type::Choice || f.type == FieldDef::Type::BoolToggle)
        return;
    apply_value(sel, edit_buf.empty() ? f.unset_label : edit_buf);
    editing = false;
    edit_buf.clear();
}

void SettingsForm::cancel_edit() {
    editing = false;
    edit_buf.clear();
}

void SettingsForm::move_up() {
    if (sel > 0) --sel;
}

void SettingsForm::move_down() {
    if (sel + 1 < static_cast<int>(fields.size())) ++sel;
}

void SettingsForm::next_tab() {
    switch (current_tab) {
    case Tab::General:     current_tab = Tab::Profiles; break;
    case Tab::Profiles:    current_tab = Tab::Credentials; break;
    case Tab::Credentials: current_tab = Tab::General; break;
    }
    sel = 0;
    editing = false;
    edit_buf.clear();
}

void SettingsForm::prev_tab() {
    switch (current_tab) {
    case Tab::General:     current_tab = Tab::Credentials; break;
    case Tab::Profiles:    current_tab = Tab::General; break;
    case Tab::Credentials: current_tab = Tab::Profiles; break;
    }
    sel = 0;
    editing = false;
    edit_buf.clear();
}

void SettingsForm::open_profile_editor() {
    profile_editor = profile_editor_build(draft.profiles);
    profile_editor.active = true;
}

void SettingsForm::close_profile_editor() {
    if (profile_editor.dirty) {
        draft.profiles = std::move(profile_editor.profiles);
        dirty = true;
        // Auto-unset profile if the active one was deleted.
        if (!draft.profile.empty()) {
            bool found = false;
            for (const auto& p : draft.profiles)
                if (p.name == draft.profile) { found = true; break; }
            if (!found) draft.profile.clear();
        }
    }
    profile_editor = ProfileEditor{};
}

void SettingsForm::open_credential_editor() {
    // credentials are loaded by the TUI glue and passed in; the empty-map
    // default is for when the TUI hasn't populated them yet.
    // The TUI handler must call open_credential_editor() after loading creds.
    credential_editor = credential_editor_build(
        credential_editor.credentials, draft.profiles);
    credential_editor.active = true;
}

void SettingsForm::close_credential_editor() {
    credential_editor = CredentialEditor{};
}

std::vector<ValidationMessage> SettingsForm::validate(
    std::string_view provider_kind) const {
    std::vector<ValidationMessage> msgs;

    auto push_err = [&](std::string f, std::string m) {
        msgs.push_back({std::move(f), std::move(m), true});
    };
    auto push_warn = [&](std::string f, std::string m) {
        msgs.push_back({std::move(f), std::move(m), false});
    };

    // --- General fields ---
    if (!draft.provider.empty()) {
        if (!valid_provider_kind(draft.provider))
            push_err("provider", "invalid provider; use openai, anthropic, gemini, or auto");
    }

    if (has_control_chars(draft.model))
        push_err("model", "model name contains control characters");

    if (!draft.base_url.empty()) {
        if (!draft.base_url.starts_with("http://") &&
            !draft.base_url.starts_with("https://"))
            push_err("base_url", "URL must start with http:// or https://");
        else if (draft.base_url.starts_with("http://"))
            push_warn("base_url", "using http:// (unencrypted)");
    }

    if (draft.context_window < 0)
        push_err("context_window", "must be >= 0");
    else if (draft.context_window > 10000000)
        push_err("context_window", "implausibly large (> 10 million)");

    if (draft.max_iterations < 0)
        push_err("max_iterations", "must be >= 0");

    if (draft.max_tokens < 0)
        push_err("max_tokens", "must be >= 0");

    if (!draft.effort.empty()) {
        std::string el = to_lower(draft.effort);
        if (el != "low" && el != "medium" && el != "high" &&
            el != "minimal" && el != "none" && el != "off")
            push_err("effort", "must be low, medium, high, minimal, none, or off");
    }

    if (draft.temperature >= 0 && draft.temperature > 2.0)
        push_warn("temperature", "temperature > 2.0 is unusual");

    // Cross-field: Anthropic thinking=1 => temperature must be unset.
    std::string pk = to_lower(provider_kind);
    if (pk == "anthropic" && draft.thinking == 1 && draft.temperature >= 0)
        push_err("thinking", "Anthropic requires temperature unset when thinking is enabled");

    // Cross-field: effort set + thinking off.
    if (!draft.effort.empty() && draft.thinking == 0)
        push_err("thinking", "thinking must be enabled when effort is set");

    // Cross-field: effort "none" + thinking on.
    std::string el = to_lower(draft.effort);
    if (el == "none" && draft.thinking == 1)
        push_err("effort", "effort 'none' conflicts with thinking enabled");

    if (!draft.profile.empty()) {
        bool found = false;
        for (const auto& p : draft.profiles)
            if (p.name == draft.profile) { found = true; break; }
        if (!found)
            push_err("profile", "profile references unknown profile '" + draft.profile + "'");
    }

    // --- Profile validation ---
    for (const auto& p : draft.profiles) {
        if (p.name.empty()) {
            push_err("profiles", "profile name must not be empty");
        } else if (p.name.find(' ') != std::string::npos ||
                   p.name.find('\t') != std::string::npos) {
            push_err("profiles", "profile name '" + p.name + "' contains whitespace");
        } else if (p.name.size() > 64) {
            push_err("profiles", "profile name '" + p.name + "' exceeds 64 characters");
        }
        // Check for duplicates
        int count = 0;
        for (const auto& q : draft.profiles)
            if (q.name == p.name) ++count;
        if (count > 1)
            push_err("profiles", "duplicate profile name '" + p.name + "'");

        if (!p.kind.empty() && !valid_provider_kind(p.kind))
            push_err("profiles", "invalid kind '" + p.kind + "' for profile '" + p.name + "'");

        if (!p.base_url.empty()) {
            if (!p.base_url.starts_with("http://") &&
                !p.base_url.starts_with("https://"))
                push_err("profiles", "base_url must start with http:// or https:// for profile '" + p.name + "'");
            else if (p.base_url.starts_with("http://"))
                push_warn("profiles", "using http:// for profile '" + p.name + "'");
        }

        // Model list caps and duplicates
        if (p.models.size() > 200)
            push_err("profiles", "profile '" + p.name + "' has > 200 models (limit is 200)");
        for (std::size_t i = 0; i < p.models.size(); ++i)
            if (p.models[i].size() > 256)
                push_err("profiles", "model name exceeds 256 characters in profile '" + p.name + "'");
        for (std::size_t i = 0; i < p.models.size(); ++i)
            for (std::size_t j = i + 1; j < p.models.size(); ++j)
                if (to_lower(p.models[i]) == to_lower(p.models[j]))
                    push_warn("profiles", "duplicate model '" + p.models[i] + "' in profile '" + p.name + "'");
    }

    // Max profiles.
    if (draft.profiles.size() > 100)
        push_err("profiles", "too many profiles (limit is 100)");

    return msgs;
}

// --- Free functions ---------------------------------------------------------

SettingsForm settings_form_build(const Settings& current) {
    SettingsForm form;
    form.rebuild(current);
    return form;
}

FormAction settings_form_close(SettingsForm& form) {
    form.active = false;
    form.editing = false;
    form.profile_editor = ProfileEditor{};
    form.credential_editor = CredentialEditor{};
    return form.dirty ? FormAction::Saved : FormAction::Cancelled;
}

}  // namespace moocode
