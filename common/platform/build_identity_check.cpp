#include "build_identity_check.h"

#include <cstring>

#include "build_identity.h"

namespace ce::build_identity {

namespace {

void CopyText(char (&field)[kTextChars], const char* text) noexcept {
    std::memset(field, 0, sizeof(field));
    if (!text)
        return;
    std::memcpy(field, text, strnlen(text, kTextChars - 1));
}

// The terminated length of a field, or kTextChars when it has no NUL inside the field.
size_t TextLength(const char (&field)[kTextChars]) noexcept {
    return strnlen(field, kTextChars);
}

bool SameText(const char (&lhs)[kTextChars], const char (&rhs)[kTextChars]) noexcept {
    const size_t length = TextLength(lhs);
    return length < kTextChars && length == TextLength(rhs) && std::memcmp(lhs, rhs, length) == 0;
}

}  // namespace

Record Current() noexcept {
    Record record = Request();
    record.buildNumber = GetCurrentBuildNumber();
    CopyText(record.version, GetCaptureVersion());
    CopyText(record.timestamp, GetBuildTimestamp());
    return record;
}

bool Publish(Record* out) noexcept {
    if (!out || out->size != sizeof(Record))
        return false;
    *out = Current();
    return true;
}

Verdict Compare(const Record& host, const Record& module) noexcept {
    if (module.size != sizeof(Record))
        return Verdict::SizeMismatch;
    if (module.buildNumber != host.buildNumber)
        return Verdict::BuildNumberMismatch;
    if (!SameText(host.version, module.version))
        return Verdict::VersionMismatch;
    if (!SameText(host.timestamp, module.timestamp))
        return Verdict::TimestampMismatch;
    return Verdict::Match;
}

Verdict CheckExport(ExportFn exportFn, const Record& host, Record* module) noexcept {
    if (!exportFn)
        return Verdict::ExportMissing;
    Record answer = Request();
    if (!exportFn(&answer))
        return Verdict::ExportRejected;
    if (module)
        *module = answer;
    return Compare(host, answer);
}

const char* VerdictName(Verdict verdict) noexcept {
    switch (verdict) {
        case Verdict::Match:
            return "match";
        case Verdict::ExportMissing:
            return "identity export missing";
        case Verdict::ExportRejected:
            return "identity export rejected the request";
        case Verdict::SizeMismatch:
            return "record size mismatch";
        case Verdict::BuildNumberMismatch:
            return "build number mismatch";
        case Verdict::VersionMismatch:
            return "version mismatch";
        case Verdict::TimestampMismatch:
            return "build timestamp mismatch";
    }
    return "unknown";
}

std::string Describe(const Record& record) {
    const std::string version(record.version, strnlen(record.version, kTextChars));
    const std::string timestamp(record.timestamp, strnlen(record.timestamp, kTextChars));
    return version + " (build " + std::to_string(record.buildNumber) + ", built " + timestamp + ")";
}

}  // namespace ce::build_identity
