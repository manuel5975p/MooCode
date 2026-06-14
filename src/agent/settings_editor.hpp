#ifndef FLAGENT_SETTINGS_EDITOR_HPP
#define FLAGENT_SETTINGS_EDITOR_HPP

// Screen-free settings-editor form logic: a pure data + transition layer under
// the /settings slash command. Zero FTXUI, zero mutex, zero I/O — the TUI
// handler glue in tui.cpp owns persistence, rendering, and the redraw loop.
// Mirrors the tui_slash precedent of a decision-only unit that is independently
// unit-testable.

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "agent/persist.hpp"  // Settings, Profile

namespace flagent {

// One field in the editor form.
struct FieldDef {
    enum class Type { String, Int, Double, Choice, BoolToggle };
    std::string label;                // display label, e.g. "model", "provider"
    Type type;
    std::vector<std::string> choices; // valid values for Choice and BoolToggle
    std::string unset_label = "(unset)"; // label when the field is empty/unset
};

// Outcome of closing the form modal. The TUI handler uses this to decide
// whether to persist.
enum class FormAction { None, Saved, Cancelled };

// --- Profile sub-editor ------------------------------------------------------
// Manages the list of profiles within the settings editor.  A nested state
// machine: profile list → profile detail editor → models sub-list.

struct ProfileEditor {
    bool active = false;
    std::vector<Profile> profiles;    // working copy (mirrors draft.profiles)
    int sel = 0;                      // selected profile in the list
    bool editing_profile = false;     // editing a single profile's fields
    int profile_field_sel = 0;        // which field of the profile is selected
    int edit_field_idx = -1;          // -1 = none, else which field index is being edited
    bool profile_edit_text_mode = false;
    std::string edit_buf;
    bool editing_models = false;      // editing the models sub-list
    int model_sel = 0;
    std::string model_edit_buf;
    bool dirty = false;

    // Navigation within the profile list.
    void move_up();
    void move_down();

    // Profile CRUD.
    void add_profile();               // creates empty Profile, enters edit mode
    void delete_profile(int idx);     // removes from profiles (caller confirms)
    void begin_edit_profile();        // enter the detail editor for sel
    void commit_profile_edit();       // apply edits, return to list
    void cancel_profile_edit();       // discard, return to list

    // Model list.
    void add_model();
    void delete_model(int idx);
    void begin_edit_model();
    void commit_model_edit();
    void cancel_model_edit();

    // Move within profile detail fields.
    void profile_field_up();
    void profile_field_down();
    void begin_edit_profile_field();
    void commit_profile_field_edit();
    void cancel_profile_field_edit();
};

ProfileEditor profile_editor_build(const std::vector<Profile>& profiles);

// Number of profile detail fields, for rendering loops.
constexpr int kNumProfileFields = 4;

// Label for profile detail field index i (0..kNumProfileFields-1).
std::string profile_field_label(int i);

// Field index constants for profile detail.
constexpr int kPFieldName    = 0;
constexpr int kPFieldKind    = 1;
constexpr int kPFieldBaseUrl = 2;
constexpr int kPFieldModel   = 3;

// Read a profile field value by field index (kPFieldName etc.)
std::string profile_field_value(const Profile& p, int field_idx);

// --- Validation messages -----------------------------------------------------

struct ValidationMessage {
    std::string field;   // which field (empty for global invariants)
    std::string message;
    bool is_error;       // true = block save, false = warn
};

// --- Credential sub-editor (Phase 3) -----------------------------------------

struct CredentialEditor {
    bool active = false;
    std::map<std::string, std::string> credentials; // profile_name -> key (raw)
    int sel = 0;
    bool editing_key = false;
    std::string key_edit_buf;
    bool dirty = false;
    std::set<std::string> deleted;

    void move_up();
    void move_down();
    void begin_edit_key();
    void commit_key();
    void cancel_key();
    void delete_key();  // mark for deletion (caller confirms)

    // Returns masked display string: "sk-…a1b2", "(set)", "(not set)".
    std::string display(const std::string& profile_name) const;

    // List of profile names for which keys can be edited (from the profiles list).
    std::vector<std::string> profile_names;
};

CredentialEditor credential_editor_build(
    const std::map<std::string, std::string>& creds,
    const std::vector<Profile>& profiles);

// --- The settings-editor form state machine ----------------------------------

struct SettingsForm {
    enum class Tab { General, Profiles, Credentials };

    // --- Immutable field definitions (built once by settings_form_build) ---
    std::vector<FieldDef> fields;

    // --- Tab state ---
    Tab current_tab = Tab::General;

    // --- Mutable form state ---
    Settings draft;        // working copy; mutated only by apply_value()
    int sel = 0;           // selected row index within the current tab
    bool editing = false;  // inline-edit mode active (String / Int / Double)
    std::string edit_buf;  // edit buffer (capped by the TUI handler)
    bool dirty = false;    // any field was modified since open
    bool active = false;   // modal is open

    // Phase 2+
    ProfileEditor profile_editor;
    // Phase 3
    CredentialEditor credential_editor;

    // True when settings.toml was malformed on open (show warning banner).
    bool malformed_file = false;
    std::string malformed_msg;

    // Rebuild fields + draft from current settings. Resets dirty / sel /
    // editing.  Called when the modal opens.
    void rebuild(const Settings& current);

    // Display string for field `idx`. Falls back to `live_model` /
    // `live_provider` when the corresponding draft field is empty.
    // pre: 0 <= idx < fields.size()
    std::string display_value(int idx,
                              std::string_view live_model,
                              std::string_view live_provider) const;

    // Apply a string value to field `idx`. Returns false on validation failure
    // (draft is NOT mutated). Returns true and sets dirty on success.
    bool apply_value(int idx, std::string_view val);

    // Begin editing the selected field. For Choice / BoolToggle: cycles.
    // For String / Int / Double: enters edit mode with edit_buf primed.
    void begin_edit();

    // Commit the edit buffer to the selected field. No-op for Choice / BoolToggle.
    void commit_edit();

    // Cancel editing; clears the buffer and exits edit mode.
    void cancel_edit();

    // Navigation within the current tab.
    void move_up();
    void move_down();

    // Tab switching.
    void next_tab();
    void prev_tab();

    // Open/close the profile sub-editor (copies draft.profiles on open).
    void open_profile_editor();
    void close_profile_editor();

    // Open/close the credential sub-editor.
    void open_credential_editor();
    void close_credential_editor();

    // Validate all fields. Returns errors (block save) and warnings.
    // provider_kind is "openai"/"anthropic"/"gemini"/"" for cross-field checks.
    std::vector<ValidationMessage> validate(std::string_view provider_kind) const;

    // Helper: detect malformed file on open (called by TUI glue before rebuild).
    void detect_malformed(const std::string& home);
};

// Build a fresh form from loaded settings.
SettingsForm settings_form_build(const Settings& current);

// Called when the modal closes (toggle, Esc, or /settings re-invoked).
// Returns Saved when dirty, Cancelled when clean.  Resets active / editing.
FormAction settings_form_close(SettingsForm& form);

}  // namespace flagent

#endif  // FLAGENT_SETTINGS_EDITOR_HPP
