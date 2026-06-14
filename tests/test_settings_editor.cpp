#include "agent/settings_editor.hpp"

#include <map>
#include <string>
#include <vector>

#include "test_harness.hpp"

using namespace moocode;

// --- field count and rebuild -------------------------------------------------

TEST("settings_form_build creates 12 fields") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK_EQ(form.fields.size(), 12u);
}

TEST("rebuild copies all Settings fields into draft") {
    Settings s;
    s.model = "claude-sonnet-4-6";
    s.base_url = "https://api.anthropic.com/v1";
    s.provider = "anthropic";
    s.context_window = 200000;
    s.max_iterations = 50;
    s.max_tokens = 8192;
    s.effort = "high";
    s.temperature = 0.7;
    s.thinking = 1;
    s.rtk = 0;
    s.theme = "mono";
    s.profile = "work";

    auto form = settings_form_build(s);
    CHECK_EQ(form.draft.model, s.model);
    CHECK_EQ(form.draft.base_url, s.base_url);
    CHECK_EQ(form.draft.provider, s.provider);
    CHECK_EQ(form.draft.context_window, s.context_window);
    CHECK_EQ(form.draft.max_iterations, s.max_iterations);
    CHECK_EQ(form.draft.max_tokens, s.max_tokens);
    CHECK_EQ(form.draft.effort, s.effort);
    CHECK(form.draft.temperature == 0.7);  // double comparison
    CHECK_EQ(form.draft.thinking, s.thinking);
    CHECK_EQ(form.draft.rtk, s.rtk);
    CHECK_EQ(form.draft.theme, s.theme);
    CHECK_EQ(form.draft.profile, s.profile);
}

TEST("rebuild resets state") {
    Settings s;
    auto form = settings_form_build(s);
    // Manually mess it up.
    form.sel = 5;
    form.dirty = true;
    form.editing = true;
    form.edit_buf = "junk";
    form.rebuild(s);
    CHECK_EQ(form.sel, 0);
    CHECK(!form.dirty);
    CHECK(!form.editing);
    CHECK(form.edit_buf.empty());
}

// --- display_value -----------------------------------------------------------

TEST("display_value for unset model falls back to live_model") {
    Settings s;
    auto form = settings_form_build(s);
    // model is empty -> should show live_model
    std::string val = form.display_value(1, "gpt-5", "openai");
    CHECK_EQ(val, std::string{"gpt-5"});
}

TEST("display_value for set model shows draft value") {
    Settings s;
    s.model = "claude-opus";
    auto form = settings_form_build(s);
    std::string val = form.display_value(1, "gpt-5", "openai");
    CHECK_EQ(val, std::string{"claude-opus"});
}

TEST("display_value for unset provider shows auto") {
    Settings s;
    auto form = settings_form_build(s);
    // provider is at idx 0 and is empty -> should show "auto"
    std::string val = form.display_value(0, "", "");
    CHECK_EQ(val, std::string{"auto"});
}

TEST("display_value for set provider shows value") {
    Settings s;
    s.provider = "anthropic";
    auto form = settings_form_build(s);
    std::string val = form.display_value(0, "", "");
    CHECK_EQ(val, std::string{"anthropic"});
}

TEST("display_value for context_window unset shows unset_label") {
    Settings s;
    s.context_window = 0;
    auto form = settings_form_build(s);
    std::string val = form.display_value(3, "", "");
    CHECK_EQ(val, std::string{"(unset)"});
}

TEST("display_value for context_window set shows number") {
    Settings s;
    s.context_window = 200000;
    auto form = settings_form_build(s);
    std::string val = form.display_value(3, "", "");
    CHECK_EQ(val, std::string{"200000"});
}

TEST("display_value for temperature unset shows unset_label") {
    Settings s;
    s.temperature = -1;
    auto form = settings_form_build(s);
    std::string val = form.display_value(7, "", "");
    CHECK_EQ(val, std::string{"(unset)"});
}

TEST("display_value for temperature set formats float") {
    Settings s;
    s.temperature = 0.7;
    auto form = settings_form_build(s);
    std::string val = form.display_value(7, "", "");
    CHECK_EQ(val, std::string{"0.7"});
}

TEST("display_value for thinking -1 shows unset") {
    Settings s;
    s.thinking = -1;
    auto form = settings_form_build(s);
    std::string val = form.display_value(8, "", "");
    CHECK_EQ(val, std::string{"unset"});
}

TEST("display_value for thinking 1 shows on") {
    Settings s;
    s.thinking = 1;
    auto form = settings_form_build(s);
    std::string val = form.display_value(8, "", "");
    CHECK_EQ(val, std::string{"on"});
}

TEST("display_value for thinking 0 shows off") {
    Settings s;
    s.thinking = 0;
    auto form = settings_form_build(s);
    std::string val = form.display_value(8, "", "");
    CHECK_EQ(val, std::string{"off"});
}

TEST("display_value for effort unset shows unset_label") {
    Settings s;
    s.effort = "";
    auto form = settings_form_build(s);
    std::string val = form.display_value(6, "", "");
    CHECK_EQ(val, std::string{"(unset)"});
}

TEST("display_value for rtk -1 shows unset") {
    Settings s;
    s.rtk = -1;
    auto form = settings_form_build(s);
    std::string val = form.display_value(9, "", "");
    CHECK_EQ(val, std::string{"unset"});
}

TEST("display_value for rtk 1 shows on") {
    Settings s;
    s.rtk = 1;
    auto form = settings_form_build(s);
    std::string val = form.display_value(9, "", "");
    CHECK_EQ(val, std::string{"on"});
}

TEST("display_value for theme unset shows unset_label") {
    Settings s;
    auto form = settings_form_build(s);
    std::string val = form.display_value(10, "", "");
    CHECK_EQ(val, std::string{"(unset)"});
}

TEST("display_value for theme set shows value") {
    Settings s;
    s.theme = "mono";
    auto form = settings_form_build(s);
    std::string val = form.display_value(10, "", "");
    CHECK_EQ(val, std::string{"mono"});
}

TEST("display_value for profile unset shows unset_label") {
    Settings s;
    auto form = settings_form_build(s);
    std::string val = form.display_value(11, "", "");
    CHECK_EQ(val, std::string{"(none)"});
}

TEST("display_value for profile set shows value") {
    Settings s;
    s.profile = "work";
    auto form = settings_form_build(s);
    std::string val = form.display_value(11, "", "");
    CHECK_EQ(val, std::string{"work"});
}

TEST("display_value returns empty for out-of-range index") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.display_value(-1, "", "").empty());
    CHECK(form.display_value(100, "", "").empty());
}

// --- apply_value: String fields -----------------------------------------------

TEST("apply_value String field sets draft and returns true") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.apply_value(1, "claude-opus"));  // model (idx 1)
    CHECK_EQ(form.draft.model, std::string{"claude-opus"});
}

TEST("apply_value String field base_url") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.apply_value(2, "https://api.anthropic.com/v1"));  // base_url (idx 2)
    CHECK_EQ(form.draft.base_url, std::string{"https://api.anthropic.com/v1"});
}

TEST("apply_value String field profile") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.apply_value(11, "work"));  // profile (idx 11)
    CHECK_EQ(form.draft.profile, std::string{"work"});
}

TEST("apply_value profile with (none) resets to empty") {
    Settings s;
    s.profile = "work";
    auto form = settings_form_build(s);
    CHECK(form.apply_value(11, "(none)"));
    CHECK(form.draft.profile.empty());
}

// --- apply_value: Int fields --------------------------------------------------

TEST("apply_value Int field parses correctly") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.apply_value(3, "128000"));  // context_window (idx 3)
    CHECK_EQ(form.draft.context_window, 128000);
}

TEST("apply_value Int field rejects non-numeric string") {
    Settings s;
    s.context_window = 100000;
    auto form = settings_form_build(s);
    CHECK(!form.apply_value(3, "abc"));
    // Draft must be unchanged on failure.
    CHECK_EQ(form.draft.context_window, 100000);
}

TEST("apply_value Int field clamps negative to 0") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.apply_value(3, "-50"));
    CHECK_EQ(form.draft.context_window, 0);
}

// --- apply_value: Double field ------------------------------------------------

TEST("apply_value Double field parses correctly") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.apply_value(7, "0.7"));  // temperature (idx 7)
    CHECK(form.draft.temperature >= 0.69 && form.draft.temperature <= 0.71);
}

TEST("apply_value Double field rejects non-numeric") {
    Settings s;
    s.temperature = 0.5;
    auto form = settings_form_build(s);
    CHECK(!form.apply_value(7, "abc"));
    // Draft unchanged on failure.
    CHECK(form.draft.temperature == 0.5);
}

TEST("apply_value Double field accepts unset_label to reset") {
    Settings s;
    s.temperature = 0.7;
    auto form = settings_form_build(s);
    CHECK(form.apply_value(7, "(unset)"));
    CHECK_EQ(form.draft.temperature, -1);
}

// --- apply_value: Choice fields -----------------------------------------------

TEST("apply_value Choice field rejects value not in choices") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(!form.apply_value(0, "gemini"));  // provider only accepts openai/anthropic/auto
    CHECK(form.draft.provider.empty());  // unchanged
}

TEST("apply_value Choice field accepts value in choices") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.apply_value(0, "openai"));  // provider
    CHECK_EQ(form.draft.provider, std::string{"openai"});
}

TEST("apply_value provider 'auto' sets empty string") {
    Settings s;
    s.provider = "openai";
    auto form = settings_form_build(s);
    CHECK(form.apply_value(0, "auto"));
    CHECK(form.draft.provider.empty());
}

// --- apply_value: BoolToggle --------------------------------------------------

TEST("apply_value BoolToggle cycling") {
    Settings s;
    s.thinking = -1;  // unset
    auto form = settings_form_build(s);
    // thinking (idx 8) is a BoolToggle with choices {"unset", "on", "off"}
    CHECK(form.apply_value(8, "on"));
    CHECK_EQ(form.draft.thinking, 1);
    CHECK(form.apply_value(8, "off"));
    CHECK_EQ(form.draft.thinking, 0);
    CHECK(form.apply_value(8, "unset"));
    CHECK_EQ(form.draft.thinking, -1);
}

// --- apply_value: dirty flag --------------------------------------------------

TEST("apply_value sets dirty on success") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(!form.dirty);
    CHECK(form.apply_value(1, "claude"));
    CHECK(form.dirty);
}

TEST("apply_value does not set dirty on failure") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(!form.apply_value(3, "abc"));  // non-numeric for Int
    CHECK(!form.dirty);
}

// --- begin_edit / commit_edit / cancel_edit -----------------------------------

TEST("begin_edit for Choice cycles to next choice") {
    Settings s;
    s.effort = "(unset)";  // choices: (unset), none, low, medium, high
    auto form = settings_form_build(s);
    form.sel = 6;  // effort
    form.begin_edit();
    // After cycling, should be "none" (next after "(unset)").
    CHECK_EQ(form.draft.effort, std::string{"none"});
}

TEST("begin_edit for String enters edit mode with correct buffer") {
    Settings s;
    s.model = "gpt-5";
    auto form = settings_form_build(s);
    form.sel = 1;  // model
    form.begin_edit();
    CHECK(form.editing);
    CHECK_EQ(form.edit_buf, std::string{"gpt-5"});
}

TEST("begin_edit for String clears buffer when value is unset") {
    Settings s;
    s.context_window = 0;  // unset => "(unset)" display
    auto form = settings_form_build(s);
    form.sel = 3;  // context_window
    form.begin_edit();
    CHECK(form.editing);
    CHECK(form.edit_buf.empty());
}

TEST("commit_edit applies edit_buf to draft") {
    Settings s;
    auto form = settings_form_build(s);
    form.sel = 1;  // model
    form.begin_edit();
    form.edit_buf = "claude-opus";
    form.commit_edit();
    CHECK_EQ(form.draft.model, std::string{"claude-opus"});
    CHECK(!form.editing);
    CHECK(form.edit_buf.empty());
}

TEST("cancel_edit clears editing state without changing draft") {
    Settings s;
    s.model = "original";
    auto form = settings_form_build(s);
    form.sel = 1;  // model
    form.begin_edit();
    form.edit_buf = "changed";
    form.cancel_edit();
    CHECK(!form.editing);
    CHECK(form.edit_buf.empty());
    CHECK_EQ(form.draft.model, std::string{"original"});
}

// --- Navigation ----------------------------------------------------------------

TEST("move_up at 0 stays at 0") {
    Settings s;
    auto form = settings_form_build(s);
    form.sel = 0;
    form.move_up();
    CHECK_EQ(form.sel, 0);
}

TEST("move_down at last stays at last") {
    Settings s;
    auto form = settings_form_build(s);
    int last = static_cast<int>(form.fields.size()) - 1;
    form.sel = last;
    form.move_down();
    CHECK_EQ(form.sel, last);
}

TEST("move_up moves selection") {
    Settings s;
    auto form = settings_form_build(s);
    form.sel = 3;
    form.move_up();
    CHECK_EQ(form.sel, 2);
}

TEST("move_down moves selection") {
    Settings s;
    auto form = settings_form_build(s);
    form.sel = 3;
    form.move_down();
    CHECK_EQ(form.sel, 4);
}

// --- settings_form_close ------------------------------------------------------

TEST("settings_form_close returns Saved when dirty") {
    Settings s;
    auto form = settings_form_build(s);
    form.active = true;
    form.apply_value(1, "claude");
    FormAction act = settings_form_close(form);
    CHECK(act == FormAction::Saved);
    CHECK(!form.active);
    CHECK(!form.editing);
}

TEST("settings_form_close returns Cancelled when clean") {
    Settings s;
    auto form = settings_form_build(s);
    form.active = true;
    FormAction act = settings_form_close(form);
    CHECK(act == FormAction::Cancelled);
    CHECK(!form.active);
    CHECK(!form.editing);
}

// --- Field types ---------------------------------------------------------------

TEST("field types match expected layout") {
    Settings s;
    auto form = settings_form_build(s);
    // provider (Choice), model (String), base_url (String), context_window (Int),
    // max_iterations (Int), max_tokens (Int), effort (Choice), temperature (Double),
    // thinking (BoolToggle), rtk (BoolToggle), theme (Choice), profile (String)
    CHECK_EQ(form.fields[0].type, FieldDef::Type::Choice);
    CHECK_EQ(form.fields[1].type, FieldDef::Type::String);
    CHECK_EQ(form.fields[2].type, FieldDef::Type::String);
    CHECK_EQ(form.fields[3].type, FieldDef::Type::Int);
    CHECK_EQ(form.fields[4].type, FieldDef::Type::Int);
    CHECK_EQ(form.fields[5].type, FieldDef::Type::Int);
    CHECK_EQ(form.fields[6].type, FieldDef::Type::Choice);
    CHECK_EQ(form.fields[7].type, FieldDef::Type::Double);
    CHECK_EQ(form.fields[8].type, FieldDef::Type::BoolToggle);
    CHECK_EQ(form.fields[9].type, FieldDef::Type::BoolToggle);
    CHECK_EQ(form.fields[10].type, FieldDef::Type::Choice);
    CHECK_EQ(form.fields[11].type, FieldDef::Type::String);
}

// --- apply_value: out-of-range ------------------------------------------------

TEST("apply_value out-of-range returns false") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(!form.apply_value(-1, "x"));
    CHECK(!form.apply_value(100, "x"));
}

TEST("display_value out-of-range returns empty") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.display_value(-1, "", "").empty());
    CHECK(form.display_value(100, "", "").empty());
}

TEST("begin_edit out-of-range no-ops") {
    Settings s;
    auto form = settings_form_build(s);
    form.sel = 100;
    form.begin_edit();  // should not crash
    CHECK(!form.editing);
}

TEST("commit_edit out-of-range no-ops") {
    Settings s;
    auto form = settings_form_build(s);
    form.sel = 100;
    form.commit_edit();  // should not crash
}

// === Phase 2: ProfileEditor ==================================================

TEST("ProfileEditor add_profile creates empty profile and enters edit mode") {
    ProfileEditor pe;
    pe.add_profile();
    CHECK_EQ(pe.profiles.size(), 1u);
    CHECK_EQ(pe.profiles[0].name, std::string{"new"});
    CHECK(pe.editing_profile);
}

TEST("ProfileEditor delete_profile removes from vector") {
    ProfileEditor pe;
    pe.add_profile();
    pe.add_profile();
    pe.profiles[1].name = "second";
    pe.delete_profile(0);
    CHECK_EQ(pe.profiles.size(), 1u);
    CHECK_EQ(pe.profiles[0].name, std::string{"second"});
    CHECK(pe.dirty);
}

TEST("ProfileEditor delete_profile clamps selection") {
    ProfileEditor pe;
    pe.add_profile();
    pe.add_profile();
    pe.sel = 1;
    pe.delete_profile(1);
    CHECK_EQ(pe.sel, 0);
}

TEST("ProfileEditor begin_edit_profile enters detail editor") {
    ProfileEditor pe;
    pe.add_profile();
    pe.cancel_profile_edit(); // exit edit mode from add_profile
    pe.sel = 0;
    pe.begin_edit_profile();
    CHECK(pe.editing_profile);
    CHECK_EQ(pe.profile_field_sel, 0);
}

TEST("ProfileEditor commit_profile_edit sets dirty") {
    ProfileEditor pe;
    pe.add_profile();
    pe.commit_profile_edit();
    CHECK(pe.dirty);
    CHECK(!pe.editing_profile);
}

TEST("ProfileEditor add_model adds to models list") {
    ProfileEditor pe;
    Profile p;
    p.name = "test";
    pe.profiles.push_back(p);
    pe.sel = 0;
    pe.add_model();
    CHECK_EQ(pe.profiles[0].models.size(), 1u);
    CHECK(pe.dirty);
}

TEST("ProfileEditor delete_model removes from list") {
    ProfileEditor pe;
    Profile p;
    p.models = {"m1", "m2", "m3"};
    pe.profiles.push_back(p);
    pe.sel = 0;
    pe.delete_model(1);
    CHECK_EQ(pe.profiles[0].models.size(), 2u);
    CHECK_EQ(pe.profiles[0].models[0], std::string{"m1"});
    CHECK_EQ(pe.profiles[0].models[1], std::string{"m3"});
}

TEST("ProfileEditor begin/edit/commit model cycle") {
    ProfileEditor pe;
    Profile p;
    p.models = {"gpt-5"};
    pe.profiles.push_back(p);
    pe.sel = 0;
    pe.model_sel = 0;
    pe.begin_edit_model();
    CHECK_EQ(pe.model_edit_buf, std::string{"gpt-5"});
    pe.model_edit_buf = "gpt-6";
    pe.commit_model_edit();
    CHECK_EQ(pe.profiles[0].models[0], std::string{"gpt-6"});
}

TEST("ProfileEditor cancel_model_edit clears buffer") {
    ProfileEditor pe;
    Profile p;
    p.models = {"gpt-5"};
    pe.profiles.push_back(p);
    pe.sel = 0;
    pe.model_sel = 0;
    pe.begin_edit_model();
    pe.model_edit_buf = "changed";
    pe.cancel_model_edit();
    CHECK(pe.model_edit_buf.empty());
}

TEST("ProfileEditor commit_profile_field_edit") {
    ProfileEditor pe;
    Profile p;
    p.name = "old";
    pe.profiles.push_back(p);
    pe.sel = 0;
    pe.begin_edit_profile();
    pe.profile_field_sel = 0; // name
    pe.begin_edit_profile_field();
    pe.edit_buf = "new_name";
    pe.commit_profile_field_edit();
    CHECK_EQ(pe.profiles[0].name, std::string{"new_name"});
    CHECK(pe.dirty);
}

// === Phase 2: validate() ======================================================

TEST("validate empty profile name is error") {
    SettingsForm form;
    form.draft.profiles.push_back({"", "", "", "", {}});
    auto msgs = form.validate("");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "profiles" && m.is_error && m.message.find("must not be empty") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST("validate duplicate profile names is error") {
    SettingsForm form;
    form.draft.profiles.push_back({"dup", "", "", "", {}});
    form.draft.profiles.push_back({"dup", "", "", "", {}});
    auto msgs = form.validate("");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "profiles" && m.is_error && m.message.find("duplicate") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST("validate base_url without http is error") {
    SettingsForm form;
    form.draft.base_url = "file:///etc/passwd";
    auto msgs = form.validate("");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "base_url" && m.is_error)
            found = true;
    CHECK(found);
}

TEST("validate http URL warns") {
    SettingsForm form;
    form.draft.base_url = "http://example.com/v1";
    auto msgs = form.validate("");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "base_url" && !m.is_error && m.message.find("http://") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST("validate unknown profile in profile field is error") {
    SettingsForm form;
    form.draft.profile = "nonexistent";
    auto msgs = form.validate("");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "profile" && m.is_error)
            found = true;
    CHECK(found);
}

TEST("validate profile name with whitespace is error") {
    SettingsForm form;
    form.draft.profiles.push_back({"bad name", "", "", "", {}});
    auto msgs = form.validate("");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "profiles" && m.is_error && m.message.find("whitespace") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST("validate Anthropic thinking with temperature is error") {
    SettingsForm form;
    form.draft.thinking = 1;
    form.draft.temperature = 0.7;
    auto msgs = form.validate("anthropic");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "thinking" && m.is_error && m.message.find("temperature") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST("validate effort set with thinking off is error") {
    SettingsForm form;
    form.draft.effort = "high";
    form.draft.thinking = 0;
    auto msgs = form.validate("");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "thinking" && m.is_error)
            found = true;
    CHECK(found);
}

TEST("validate effort 'none' with thinking on is error") {
    SettingsForm form;
    form.draft.effort = "none";
    form.draft.thinking = 1;
    auto msgs = form.validate("");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "effort" && m.is_error)
            found = true;
    CHECK(found);
}

TEST("validate too many profiles is error") {
    SettingsForm form;
    for (int i = 0; i < 101; ++i)
        form.draft.profiles.push_back({"p" + std::to_string(i), "", "", "", {}});
    auto msgs = form.validate("");
    bool found = false;
    for (auto& m : msgs)
        if (m.field == "profiles" && m.is_error && m.message.find("100") != std::string::npos)
            found = true;
    CHECK(found);
}

// === Phase 2: tab switching ===================================================

TEST("next_tab cycles General -> Profiles -> Credentials -> General") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.current_tab == SettingsForm::Tab::General);
    form.next_tab();
    CHECK(form.current_tab == SettingsForm::Tab::Profiles);
    form.next_tab();
    CHECK(form.current_tab == SettingsForm::Tab::Credentials);
    form.next_tab();
    CHECK(form.current_tab == SettingsForm::Tab::General);
}

TEST("prev_tab cycles General -> Credentials -> Profiles -> General") {
    Settings s;
    auto form = settings_form_build(s);
    CHECK(form.current_tab == SettingsForm::Tab::General);
    form.prev_tab();
    CHECK(form.current_tab == SettingsForm::Tab::Credentials);
    form.prev_tab();
    CHECK(form.current_tab == SettingsForm::Tab::Profiles);
    form.prev_tab();
    CHECK(form.current_tab == SettingsForm::Tab::General);
}

TEST("tab switch resets selection and editing") {
    Settings s;
    auto form = settings_form_build(s);
    form.sel = 5;
    form.editing = true;
    form.edit_buf = "junk";
    form.next_tab();
    CHECK_EQ(form.sel, 0);
    CHECK(!form.editing);
    CHECK(form.edit_buf.empty());
}

// === Phase 2: close_profile_editor ============================================

TEST("close_profile_editor copies profiles to draft and sets dirty") {
    Settings s;
    auto form = settings_form_build(s);
    form.open_profile_editor();
    form.profile_editor.profiles.push_back({"work", "openai", "https://x.com/v1", "m", {"m"}});
    form.profile_editor.dirty = true;
    form.close_profile_editor();
    CHECK_EQ(form.draft.profiles.size(), 1u);
    CHECK_EQ(form.draft.profiles[0].name, std::string{"work"});
    CHECK(form.dirty);
}

TEST("close_profile_editor auto-unsets profile when active one deleted") {
    Settings s;
    s.profile = "work";
    auto form = settings_form_build(s);
    form.open_profile_editor();
    // Only add a different profile, not "work"
    form.profile_editor.profiles = {{"other", "", "", "", {}}};
    form.profile_editor.dirty = true;
    form.close_profile_editor();
    CHECK(form.draft.profile.empty());
}

// === Phase 3: CredentialEditor ================================================

TEST("CredentialEditor display for non-empty key returns masked pattern") {
    CredentialEditor ce;
    ce.credentials["openai"] = "sk-test12345678";
    std::string d = ce.display("openai");
    // "sk-" + … (UTF-8 ellipsis 3 bytes) + last 4 chars
    CHECK(d.starts_with("sk-"));
    // Contains last 4 chars "5678"
    CHECK(d.find("5678") != std::string::npos);
}

TEST("CredentialEditor display for empty key returns (not set)") {
    CredentialEditor ce;
    CHECK_EQ(ce.display("nonexistent"), std::string{"(not set)"});
}

TEST("CredentialEditor display for short key returns (set)") {
    CredentialEditor ce;
    ce.credentials["mini"] = "ab12";
    CHECK_EQ(ce.display("mini"), std::string{"(set)"});
}

TEST("CredentialEditor delete_key marks as deleted") {
    CredentialEditor ce;
    ce.credentials["openai"] = "sk-key";
    ce.profile_names = {"openai"};
    ce.sel = 0;
    ce.delete_key();
    CHECK(ce.credentials.find("openai") == ce.credentials.end());
    CHECK(ce.deleted.count("openai") == 1u);
    CHECK(ce.dirty);
}

TEST("credential_editor_build populates from credentials and profiles") {
    std::map<std::string, std::string> creds = {{"a", "k1"}, {"b", "k2"}};
    std::vector<Profile> profs = {{"a", "", "", "", {}}, {"b", "", "", "", {}}, {"c", "", "", "", {}}};
    auto ce = credential_editor_build(creds, profs);
    CHECK_EQ(ce.profile_names.size(), 3u);
    CHECK_EQ(ce.display("a"), std::string{"(set)"}); // key "k1" is <=4 chars
}

TEST("CredentialEditor begin_edit_key primes buffer") {
    CredentialEditor ce;
    ce.credentials["openai"] = "sk-abc123";
    ce.profile_names = {"openai"};
    ce.sel = 0;
    ce.begin_edit_key();
    CHECK(ce.editing_key);
    CHECK_EQ(ce.key_edit_buf, std::string{"sk-abc123"});
}

TEST("CredentialEditor commit_key stores key") {
    CredentialEditor ce;
    ce.profile_names = {"openai"};
    ce.sel = 0;
    ce.begin_edit_key();
    ce.key_edit_buf = "sk-new-key";
    ce.commit_key();
    CHECK(!ce.editing_key);
    CHECK(ce.key_edit_buf.empty());
    CHECK_EQ(ce.credentials["openai"], std::string{"sk-new-key"});
    CHECK(ce.dirty);
}

TEST("CredentialEditor cancel_key clears buffer") {
    CredentialEditor ce;
    ce.profile_names = {"openai"};
    ce.sel = 0;
    ce.begin_edit_key();
    ce.key_edit_buf = "sk-secret";
    ce.cancel_key();
    CHECK(!ce.editing_key);
    CHECK(ce.key_edit_buf.empty());
}
