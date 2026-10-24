#include "settings_store.h"
#include "common/config/configuration_state.h"
#include "common/config/config_ini_edit.h"
#include "common/config/config_text_encoding.h"
#include "common/logging/log_meter.h"
#include "common/logging/logging.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <utility>

namespace ce::runtime {
namespace {
using ce::config_reload::FileIdentity;
using ce::config_reload::LoadEvidence;
using ce::config_text::IniDocument;
using ce::config_text::IniEditStatus;
using ce::config_text::IniLine;
using ce::config_text::IniSection;

// The process name the validation loads under: it matches no application profile, so only the global
// settings of the candidate are checked.
constexpr const char* kValidationProcessName = "<settings-validation>";
// The same bound the loader's text reads use.
constexpr size_t kMaxConfigBytes = 16 * 1024 * 1024;
constexpr size_t kIoChunkBytes = 1024 * 1024;
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t DefaultNowMs() noexcept {
    return GetTickCount64();
}

// FNV-1a over the file's bytes: change detection for the conflict check, not a security boundary.
uint64_t ContentHash(std::string_view bytes) noexcept {
    uint64_t hash = kFnvOffsetBasis;
    for (const char raw : bytes) {
        hash ^= static_cast<unsigned char>(raw);
        hash *= kFnvPrime;
    }
    return hash;
}

// Write time and size as ConfigurationState compares them. A missing file is the default identity.
FileIdentity StatIdentity(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    FileIdentity identity;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return identity;
    identity.exists = true;
    identity.lastWriteTime =
        (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    identity.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return identity;
}

enum class ReadStatus { kOk, kMissing, kFailed };

ReadStatus ReadBytes(const std::wstring& path, std::string& bytes) {
    bytes.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? ReadStatus::kMissing
                                                                                 : ReadStatus::kFailed;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
              static_cast<uint64_t>(size.QuadPart) <= kMaxConfigBytes;
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < bytes.size()) {
            const DWORD want = static_cast<DWORD>(std::min(bytes.size() - done, kIoChunkBytes));
            DWORD got = 0;
            ok = ReadFile(file, bytes.data() + done, want, &got, nullptr) != FALSE && got != 0;
            done += got;
        }
    }
    CloseHandle(file);
    if (!ok) {
        bytes.clear();
        return ReadStatus::kFailed;
    }
    return ReadStatus::kOk;
}

// Writes and flushes a whole file (CREATE_ALWAYS: a leftover temp file of this process is replaced).
bool WriteBytes(const std::wstring& path, std::string_view text) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    size_t done = 0;
    while (ok && done < text.size()) {
        const DWORD want = static_cast<DWORD>(std::min(text.size() - done, kIoChunkBytes));
        DWORD wrote = 0;
        ok = WriteFile(file, text.data() + done, want, &wrote, nullptr) != FALSE && wrote == want;
        done += wrote;
    }
    ok = (FlushFileBuffers(file) != FALSE) && ok;
    ok = (CloseHandle(file) != FALSE) && ok;
    return ok;
}

// The active-code-page form of a path, for the loader (which takes narrow paths). `usable` is false when a
// character of the path has no code-page form, so the loader would read a different file.
std::string ToAnsiPath(const std::wstring& path, bool& usable) {
    usable = false;
    if (path.empty())
        return {};
    const int length = WideCharToMultiByte(CP_ACP, 0, path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (length <= 0)
        return {};
    std::string narrow(static_cast<size_t>(length), '\0');
    BOOL usedDefault = FALSE;
    WideCharToMultiByte(CP_ACP, 0, path.data(), static_cast<int>(path.size()), narrow.data(), length, nullptr,
                        &usedDefault);
    usable = usedDefault == FALSE;
    return narrow;
}

// The loader reports a value it had to reject (and replaced with the documented default) as kInvalidValue. A
// settings commit must not accept such a value silently, so those become errors. Out-of-range values are
// reported the same way by the loader and are errors here too.
ConfigDiagnostics PromoteRejectedValues(ConfigDiagnostics diagnostics) {
    for (ConfigDiagnostic& entry : diagnostics.entries) {
        if (entry.kind == ConfigDiagnosticKind::kInvalidValue)
            entry.severity = ConfigDiagnosticSeverity::kError;
    }
    return diagnostics;
}

bool HasErrors(const ConfigDiagnostics& diagnostics) {
    return std::any_of(diagnostics.entries.begin(), diagnostics.entries.end(), [](const ConfigDiagnostic& entry) {
        return entry.severity == ConfigDiagnosticSeverity::kError;
    });
}

size_t CountErrors(const ConfigDiagnostics& diagnostics) {
    return static_cast<size_t>(std::count_if(diagnostics.entries.begin(), diagnostics.entries.end(),
                                             [](const ConfigDiagnostic& entry) {
                                                 return entry.severity == ConfigDiagnosticSeverity::kError;
                                             }));
}

void AddError(ConfigDiagnostics& diagnostics, ConfigDiagnosticKind kind, std::string section, std::string key,
              std::string message) {
    ConfigDiagnostic entry;
    entry.severity = ConfigDiagnosticSeverity::kError;
    entry.kind = kind;
    entry.section = std::move(section);
    entry.key = std::move(key);
    entry.message = std::move(message);
    diagnostics.Add(std::move(entry));
}

// Lower-cased ASCII with the blanks around it removed: the comparison names of section and key lines.
std::string NormalizedName(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    const size_t last = text.find_last_not_of(" \t");
    std::string name(text.substr(first, last - first + 1));
    for (char& ch : name)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return name;
}

// Keys the loader reads as a multi-line list: their continuation lines are not part of one value. The loader
// matches them by name in more than one section, so the name decides here.
bool IsMultiLineListKey(std::string_view key) {
    const std::string name = NormalizedName(key);
    return name == "whitelist" || name == "overlay_whitelist" || name == "overlay-whitelist" ||
           name == "wgc_window_detection" || name == "wgc-window-detection" || name == "process_list";
}

// True when the section's last key line is a multi-line list and entries (or a closing ')') follow it: a new key
// inserted after that line would end the list and orphan its entries.
bool ListEndsOpen(const std::vector<IniLine>& lines) {
    bool listOpen = false;
    bool entriesFollow = false;
    for (const IniLine& line : lines) {
        if (line.hasEquals) {
            listOpen = IsMultiLineListKey(ce::config_text::WideToCodePage(line.key, CP_UTF8));
            entriesFollow = false;
        } else if (listOpen && !line.key.empty() && line.key.front() != L';') {
            entriesFollow = true;
        }
    }
    return listOpen && entriesFollow;
}

// SetIniValue inserts a missing key after the last key line of its section. Checks whether that insertion
// point lies inside a multi-line list (the first section of that name is the one an edit addresses).
bool InsertionWouldSplitList(const IniDocument& document, std::string_view section) {
    const std::string wanted = NormalizedName(section);
    for (const IniSection& candidate : document.sections) {
        if (NormalizedName(ce::config_text::WideToCodePage(candidate.name, CP_UTF8)) != wanted)
            continue;
        return ListEndsOpen(candidate.lines);
    }
    return false;
}

const char* StatusName(CommitStatus status) {
    switch (status) {
        case CommitStatus::Committed:
            return "Committed";
        case CommitStatus::Invalid:
            return "Invalid";
        case CommitStatus::Conflict:
            return "Conflict";
        case CommitStatus::IoError:
            break;
    }
    return "IoError";
}

// The loader over a candidate document, in diagnostic mode. Warnings still log, the informational narrative does not.
ConfigDiagnostics LoadValidated(const IniDocument& candidate, AppConfig& config) {
    ConfigDiagnostics diagnostics;
    ScopedQuietConfigLog quiet;
    LoadConfigFromDocument(candidate, config, kValidationProcessName, &diagnostics);
    return PromoteRejectedValues(std::move(diagnostics));
}

// What one read of config.ini produced: the identity and bytes it was read with, the parsed document and what the
// loader made of it.
struct Snapshot {
    FileIdentity identity;  // the default identity when the snapshot is not known to match the file
    uint64_t hash = 0;
    std::shared_ptr<const IniDocument> document = std::make_shared<const IniDocument>();
    ConfigDiagnostics diagnostics;
};

// The file side of the store: coherent reads, the atomic replacement, and the hooks ConfigurationState calls.
class FileBackedSettings final : public detail::ConfigurationFiles {
public:
    FileBackedSettings(std::wstring path, uint64_t (*nowMs)() noexcept)
        : path_(std::move(path)), nowMs_(nowMs ? nowMs : DefaultNowMs), ansiPath_(ToAnsiPath(path_, ansiUsable_)) {}

    uint32_t NowMs() const noexcept override {
        return static_cast<uint32_t>(nowMs_());
    }
    FileIdentity Identity() const override {
        return StatIdentity(path_);
    }
    LoadEvidence Load(AppConfig& candidate, FileIdentity) override {
        return Read(candidate, loaded_);
    }
    // A candidate whose text validates with errors keeps the published settings.
    bool Accept(const AppConfig&) override {
        if (!HasErrors(loaded_.diagnostics))
            return true;
        const ConfigDiagnostic* first = nullptr;
        for (const ConfigDiagnostic& entry : loaded_.diagnostics.entries) {
            if (entry.severity == ConfigDiagnosticSeverity::kError) {
                first = &entry;
                break;
            }
        }
        LogWarn("[Settings] external edit refused (errors=%zu, first=[%s] %s): keeping the published settings",
                CountErrors(loaded_.diagnostics), first ? first->section.c_str() : "",
                first ? first->message.substr(0, 120).c_str() : "");
        refusal_ = first ? "config.ini edit refused: [" + first->section + "] " + first->message.substr(0, 200)
                         : std::string("config.ini edit refused");
        return false;
    }
    // The refusal of the last refused candidate, handed out once (Accept runs under the store's write mutex).
    std::optional<std::string> TakeRefusal() {
        std::optional<std::string> refusal = std::move(refusal_);
        refusal_.reset();
        return refusal;
    }
    void Pending(FileIdentity identity) override {
        const auto verdict =
            pending_.Observe(ce::log_meter::FieldKey(identity.exists, identity.lastWriteTime, identity.size));
        if (verdict)
            LogDebug("[Settings] file change pending (exists=%d size=%llu repeats=%llu)", identity.exists,
                     static_cast<unsigned long long>(identity.size),
                     static_cast<unsigned long long>(verdict.suppressed));
    }
    void Deferred(const LoadEvidence& evidence) override {
        const bool changed = evidence.identityBeforeLoad != evidence.identityAfterLoad;
        const auto verdict = deferred_.Observe(ce::log_meter::FieldKey(
            evidence.fileReadBeforeLoad, evidence.readFailuresDuringLoad, changed, evidence.identityBeforeLoad.size,
            evidence.identityAfterLoad.size));
        if (verdict)
            LogWarn("[Settings] keeping settings; reload deferred (readable=%d readFailures=%llu changedDuringLoad=%d "
                    "repeats=%llu)",
                    evidence.fileReadBeforeLoad, static_cast<unsigned long long>(evidence.readFailuresDuringLoad),
                    changed, static_cast<unsigned long long>(verdict.suppressed));
    }

    // The first read, at construction. A missing file gets the default template first. An incoherent read (no file,
    // an empty one, or one changing under the read) publishes the template's settings and an unknown identity, so the
    // first Poll loads the file once it is readable. The bare AppConfig defaults are no configuration: they bind no
    // hotkey at all.
    void Initialize(AppConfig& config, FileIdentity& applied) {
        if (!StatIdentity(path_).exists && ansiUsable_) {
            const DWORD created = CreateDefaultConfig(ansiPath_);
            if (created != ERROR_SUCCESS && created != ERROR_FILE_EXISTS)
                creationError_ = created;
        }
        const LoadEvidence evidence = Read(config, applied_);
        if (!ce::config_reload::IsCoherentLoad(evidence)) {
            config = AppConfig{};
            templateApplied_ = LoadDefaultConfigTemplate(config);
            applied_ = Snapshot{};
        }
        applied = applied_.identity;
    }

    // The current file for a commit: bytes with the identity around the read.
    ReadStatus ReadText(std::string& bytes, FileIdentity& before, FileIdentity& after) const {
        before = StatIdentity(path_);
        const ReadStatus status = ReadBytes(path_, bytes);
        after = StatIdentity(path_);
        return status;
    }
    std::wstring TempPath() const {
        return path_ + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    }
    const std::wstring& Path() const {
        return path_;
    }

    // The external edit ConfigurationState just published becomes the applied text.
    void Adopt() {
        applied_ = std::move(loaded_);
        loaded_ = Snapshot{};
    }
    const Snapshot& Applied() const {
        return applied_;
    }
    void SetApplied(Snapshot snapshot) {
        applied_ = std::move(snapshot);
    }

    // Construction runs before the runtime's log exists, so a first-run file that could not be created is reported by
    // the first Poll, once. Typical cause: the folder is administrator-only (Program Files) and the user deleted the
    // file; nothing unelevated can put it back there.
    void ReportCreationFailure() {
        if (!creationError_)
            return;
        LogWarn("[Settings] config.ini is missing and could not be created (error=%lu, path=%s): %s until it exists; "
                "running setup again restores it",
                static_cast<unsigned long>(*creationError_), ansiPath_.c_str(),
                templateApplied_ ? "the built-in first-run template applies"
                                 : "no embedded template, the bare defaults (no hotkeys) apply");
        creationError_.reset();
    }

private:
    // One read of the file and the loader's view of it. The file is coherent when its identity did not change across
    // the read and the load (ConfigurationState checks the same).
    LoadEvidence Read(AppConfig& candidate, Snapshot& snapshot) {
        LoadEvidence evidence;
        evidence.identityBeforeLoad = StatIdentity(path_);
        std::string bytes;
        evidence.fileReadBeforeLoad = ReadBytes(path_, bytes) == ReadStatus::kOk;
        snapshot.identity = evidence.identityBeforeLoad;
        snapshot.hash = ContentHash(bytes);
        snapshot.document = std::make_shared<const IniDocument>(ce::config_text::ParseUtf8Ini(bytes));
        snapshot.diagnostics = {};
        const uint64_t failures = ce::config_text::ConfigReadFailureCount();
        if (evidence.fileReadBeforeLoad) {
            ConfigDiagnostics diagnostics;
            if (ansiUsable_)
                LoadConfig(ansiPath_, candidate, "", &diagnostics);
            else
                LoadConfigFromDocument(*snapshot.document, candidate, "", &diagnostics);
            snapshot.diagnostics = PromoteRejectedValues(std::move(diagnostics));
        }
        evidence.readFailuresDuringLoad = ce::config_text::ConfigReadFailureCount() - failures;
        evidence.identityAfterLoad = StatIdentity(path_);
        return evidence;
    }

    std::wstring path_;
    uint64_t (*nowMs_)() noexcept;
    bool ansiUsable_ = false;
    std::string ansiPath_;
    Snapshot applied_;  // the text behind the published document
    Snapshot loaded_;   // the last candidate Load read, until Accept and Adopt decide it
    std::optional<std::string> refusal_;  // the last refused candidate, until TakeRefusal
    std::optional<DWORD> creationError_;  // the first-run file's creation failure, until ReportCreationFailure
    bool templateApplied_ = false;        // the first read was incoherent and the embedded template took its place
    ce::log_meter::ChangeGate pending_;
    ce::log_meter::ChangeGate deferred_;
};

// The state ConfigurationState starts from. Called once, before the store's Impl is complete.
detail::ConfigurationState InitialState(FileBackedSettings& files) {
    AppConfig config;
    FileIdentity applied;
    files.Initialize(config, applied);
    return detail::ConfigurationState(std::move(config), applied, files.NowMs());
}

// Why one edit cannot be written, or nullptr when the edit was applied.
const char* EditRejection(IniEditStatus status) {
    switch (status) {
        case IniEditStatus::kInvalidSection:
            return "the section name cannot be written to config.ini";
        case IniEditStatus::kInvalidKey:
            return "the key name cannot be written to config.ini";
        case IniEditStatus::kInvalidValue:
            return "the value cannot be written to config.ini (one line of UTF-8 text)";
        case IniEditStatus::kChanged:
        case IniEditStatus::kUnchanged:
        case IniEditStatus::kNotFound:
            break;
    }
    return nullptr;
}

}  // namespace

class SettingsStore::Impl {
public:
    Impl(std::wstring path, uint64_t (*nowMs)() noexcept)
        : files_(std::move(path), nowMs), state_(InitialState(files_)) {
        config_ = std::make_shared<const AppConfig>(state_.Current());
        document_ = files_.Applied().document;
    }

    std::shared_ptr<const AppConfig> Current() const {
        std::lock_guard<std::mutex> lock(publishMutex_);
        return config_;
    }
    uint64_t Revision() const {
        std::lock_guard<std::mutex> lock(publishMutex_);
        return revision_;
    }
    std::optional<std::string> Value(std::string_view section, std::string_view key) const {
        std::shared_ptr<const IniDocument> document;
        {
            std::lock_guard<std::mutex> lock(publishMutex_);
            document = document_;
        }
        std::string value;
        if (!ce::config_text::LookupIniValue(*document, section, key, CP_UTF8, &value))
            return std::nullopt;
        return value;
    }

    bool Poll() {
        std::lock_guard<std::mutex> lock(writeMutex_);
        files_.ReportCreationFailure();
        if (!state_.Poll(files_).has_value())
            return false;
        files_.Adopt();
        Publish(std::make_shared<const AppConfig>(state_.Current()), files_.Applied().document);
        LogInfo("[Settings] external edit published revision=%llu", static_cast<unsigned long long>(Revision()));
        return true;
    }

    uint32_t NextPollDelayMs() const {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return state_.WaitMs(files_.NowMs());
    }

    std::optional<std::string> TakeRejection() {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return files_.TakeRefusal();
    }

    CommitResult Commit(const std::vector<SettingsEdit>& edits, uint64_t baseRevision) {
        std::lock_guard<std::mutex> lock(writeMutex_);
        CommitResult result;
        result.revision = Revision();
        result.status = Transact(edits, baseRevision, result);
        if (result.status == CommitStatus::Committed)
            LogInfo("[Settings] commit revision=%llu edits=%zu result=%s",
                    static_cast<unsigned long long>(result.revision), edits.size(), StatusName(result.status));
        else
            LogWarn("[Settings] commit revision=%llu edits=%zu result=%s",
                    static_cast<unsigned long long>(result.revision), edits.size(), StatusName(result.status));
        return result;
    }

    // What Commit would return for the edits, without writing or publishing anything.
    CommitResult Check(const std::vector<SettingsEdit>& edits, uint64_t baseRevision) const {
        std::lock_guard<std::mutex> lock(writeMutex_);
        CommitResult result;
        result.revision = Revision();
        Candidate candidate;
        result.status = Prepare(edits, baseRevision, result, candidate);
        return result;
    }

private:
    void Publish(std::shared_ptr<const AppConfig> config, std::shared_ptr<const IniDocument> document) {
        std::lock_guard<std::mutex> lock(publishMutex_);
        config_ = std::move(config);
        document_ = std::move(document);
        ++revision_;
    }

    // The file as the edits leave it, validated: what a commit writes and publishes.
    struct Candidate {
        std::string bytes;  // the file as it was read
        std::string text;   // the candidate's serialized text
        IniDocument document;
        AppConfig config;
    };

    // Validates the edits against the file, under writeMutex_ and without writing. A commit and a check return the same
    // status for the same file and revision. Every failure leaves the file untouched.
    CommitStatus Prepare(const std::vector<SettingsEdit>& edits, uint64_t baseRevision, CommitResult& result,
                         Candidate& out) const {
        for (const SettingsEdit& edit : edits) {
            if (IsMultiLineListKey(edit.key)) {
                AddError(result.diagnostics, ConfigDiagnosticKind::kInvalidValue, edit.section, edit.key,
                         "is a multi-line list (its continuation lines are not one value); edit config.ini directly");
                return CommitStatus::Invalid;
            }
        }
        if (baseRevision != result.revision) {
            AddError(result.diagnostics, ConfigDiagnosticKind::kNotice, "", "",
                     "the settings changed after revision " + std::to_string(baseRevision) +
                         "; reload and retry the edit");
            return CommitStatus::Conflict;
        }

        std::string bytes;
        FileIdentity before;
        FileIdentity after;
        if (files_.ReadText(bytes, before, after) == ReadStatus::kFailed) {
            AddError(result.diagnostics, ConfigDiagnosticKind::kNotice, "", "", "config.ini could not be read");
            return CommitStatus::IoError;
        }
        const Snapshot& applied = files_.Applied();
        if (before != after || before != applied.identity || ContentHash(bytes) != applied.hash) {
            AddError(result.diagnostics, ConfigDiagnosticKind::kNotice, "", "",
                     "config.ini changed outside the settings API; the published settings are stale");
            return CommitStatus::Conflict;
        }
        if (!ce::config_text::IsValidUtf8(bytes)) {
            AddError(result.diagnostics, ConfigDiagnosticKind::kInvalidValue, "", "",
                     "config.ini is not UTF-8 text; saving through the settings API would mix encodings "
                     "(save the file as UTF-8 first)");
            return CommitStatus::Invalid;
        }

        IniDocument candidate = ce::config_text::ParseUtf8Ini(bytes);
        for (const SettingsEdit& edit : edits) {
            if (!ApplyEdit(candidate, edit, result.diagnostics))
                return CommitStatus::Invalid;
        }
        const std::string text = ce::config_text::SerializeIni(candidate);
        AppConfig next;
        result.diagnostics = LoadValidated(candidate, next);
        if (HasErrors(result.diagnostics))
            return CommitStatus::Invalid;
        out = Candidate{std::move(bytes), text, std::move(candidate), std::move(next)};
        return CommitStatus::Committed;
    }

    // Writes and publishes a validated candidate, under writeMutex_. Every path that does not reach the rename leaves
    // the file untouched.
    CommitStatus Transact(const std::vector<SettingsEdit>& edits, uint64_t baseRevision, CommitResult& result) {
        Candidate candidate;
        const CommitStatus status = Prepare(edits, baseRevision, result, candidate);
        if (status != CommitStatus::Committed || candidate.text == candidate.bytes)
            return status;

        const std::string& text = candidate.text;
        const std::wstring temp = files_.TempPath();
        bool replaced = WriteBytes(temp, text);
        DWORD error = replaced ? ERROR_SUCCESS : GetLastError();
        if (replaced &&
            !MoveFileExW(temp.c_str(), files_.Path().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            replaced = false;
            error = GetLastError();
        }
        if (!replaced) {
            DeleteFileW(temp.c_str());
            AddError(result.diagnostics, ConfigDiagnosticKind::kNotice, "", "",
                     "config.ini could not be replaced (Windows error " + std::to_string(error) + ")");
            return CommitStatus::IoError;
        }
        PublishCommit(candidate.config, candidate.document, text, result);
        return CommitStatus::Committed;
    }

    // Set or remove one key of the candidate. False, with the reason in `diagnostics`, when the edit cannot be written.
    static bool ApplyEdit(IniDocument& candidate, const SettingsEdit& edit, ConfigDiagnostics& diagnostics) {
        if (edit.value && !ce::config_text::LookupIniValue(candidate, edit.section, edit.key, CP_UTF8, nullptr) &&
            InsertionWouldSplitList(candidate, edit.section)) {
            AddError(diagnostics, ConfigDiagnosticKind::kInvalidValue, edit.section, edit.key,
                     "the section ends with a multi-line list; a new key there would split it (add the key in the "
                     "file)");
            return false;
        }
        const IniEditStatus status =
            edit.value ? ce::config_text::SetIniValue(&candidate, edit.section, edit.key, *edit.value)
                       : ce::config_text::RemoveIniValue(&candidate, edit.section, edit.key);
        if (const char* reason = EditRejection(status)) {
            AddError(diagnostics, ConfigDiagnosticKind::kInvalidValue, edit.section, edit.key, reason);
            return false;
        }
        return true;
    }

    // Publishes the candidate that was just written. The write is verified by reading the file back; a file that
    // changed right after the rename stays unknown to the store, so the next Poll loads it.
    void PublishCommit(const AppConfig& next, IniDocument& candidate, const std::string& text, CommitResult& result) {
        FileIdentity written;
        std::string readBack;
        const bool intact = ReadBytes(files_.Path(), readBack) == ReadStatus::kOk && readBack == text;
        if (intact)
            written = files_.Identity();
        else
            LogWarn("[Settings] config.ini changed again right after the commit; the next poll reloads it");

        Snapshot snapshot;
        snapshot.identity = written;
        snapshot.hash = intact ? ContentHash(text) : 0;
        snapshot.document = std::make_shared<const IniDocument>(std::move(candidate));
        snapshot.diagnostics = result.diagnostics;
        files_.SetApplied(snapshot);

        state_.Replace(next, written);
        Publish(std::make_shared<const AppConfig>(state_.Current()), snapshot.document);
        result.revision = Revision();
    }

    mutable std::mutex writeMutex_;  // Poll, NextPollDelayMs and Commit
    FileBackedSettings files_;
    detail::ConfigurationState state_;
    mutable std::mutex publishMutex_;  // the published snapshot, read by any thread
    std::shared_ptr<const AppConfig> config_;
    std::shared_ptr<const IniDocument> document_;
    uint64_t revision_ = 1;
};

SettingsStore::SettingsStore(std::wstring path, uint64_t (*nowMs)() noexcept)
    : impl_(std::make_unique<Impl>(std::move(path), nowMs)) {}

SettingsStore::~SettingsStore() = default;

std::shared_ptr<const AppConfig> SettingsStore::Current() const {
    return impl_->Current();
}

uint64_t SettingsStore::Revision() const {
    return impl_->Revision();
}

std::optional<std::string> SettingsStore::Value(std::string_view section, std::string_view key) const {
    return impl_->Value(section, key);
}

bool SettingsStore::Poll() {
    return impl_->Poll();
}

uint32_t SettingsStore::NextPollDelayMs() const {
    return impl_->NextPollDelayMs();
}

std::optional<std::string> SettingsStore::TakeRejection() {
    return impl_->TakeRejection();
}

ConfigDiagnostics SettingsStore::Validate(const IniDocument& candidate) {
    AppConfig scratch;
    return LoadValidated(candidate, scratch);
}

CommitResult SettingsStore::Commit(const std::vector<SettingsEdit>& edits, uint64_t baseRevision) {
    return impl_->Commit(edits, baseRevision);
}

CommitResult SettingsStore::Check(const std::vector<SettingsEdit>& edits, uint64_t baseRevision) const {
    return impl_->Check(edits, baseRevision);
}

}  // namespace ce::runtime
