// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/api/runtime_desc.h"

#include <cstring>
#include <new>
#include <utility>

namespace ce::capi {
namespace {

bool InRange(unsigned char value, unsigned char low, unsigned char high) {
    return value >= low && value <= high;
}

enum class TextProblem { None, Empty, TooLong, NotUtf8 };

// Bounded read of a client string: never scans more than `maxBytes + 1` bytes.
TextProblem InspectText(const char* text, size_t maxBytes, std::string_view& view) {
    size_t length = 0;
    while (length <= maxBytes && text[length] != '\0')
        ++length;
    if (length > maxBytes)
        return TextProblem::TooLong;
    view = std::string_view(text, length);
    if (length == 0)
        return TextProblem::Empty;
    return IsValidUtf8(view) ? TextProblem::None : TextProblem::NotUtf8;
}

}  // namespace

ce_status_t CheckApiVersion(uint32_t clientVersion, uint32_t runtimeVersion) {
    const uint32_t clientMajor = clientVersion >> 16;
    const uint32_t clientMinor = clientVersion & 0xFFFFu;
    if (clientMajor != (runtimeVersion >> 16) || clientMinor > (runtimeVersion & 0xFFFFu))
        return CE_E_VERSION;
    return CE_OK;
}

bool IsValidUtf8(std::string_view text) {
    size_t i = 0;
    const size_t size = text.size();
    auto at = [&](size_t index) { return static_cast<unsigned char>(text[index]); };
    while (i < size) {
        const unsigned char lead = at(i);
        size_t extra = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead <= 0x7F) {
            ++i;
            continue;
        } else if (InRange(lead, 0xC2, 0xDF)) {
            extra = 1;
        } else if (lead == 0xE0) {
            extra = 2;
            low = 0xA0;
        } else if (lead == 0xED) {
            extra = 2;
            high = 0x9F;  // U+D800..U+DFFF are surrogates
        } else if (InRange(lead, 0xE1, 0xEF)) {
            extra = 2;
        } else if (lead == 0xF0) {
            extra = 3;
            low = 0x90;
        } else if (lead == 0xF4) {
            extra = 3;
            high = 0x8F;  // above U+10FFFF
        } else if (InRange(lead, 0xF1, 0xF3)) {
            extra = 3;
        } else {
            return false;  // continuation byte as lead, C0/C1 overlong lead, F5..FF
        }
        if (i + extra >= size || !InRange(at(i + 1), low, high))
            return false;
        for (size_t k = 2; k <= extra; ++k) {
            if (!InRange(at(i + k), 0x80, 0xBF))
                return false;
        }
        i += extra + 1;
    }
    return true;
}

ce_status_t ValidateRuntimeDesc(const ce_runtime_desc_t* desc, RuntimeDescCopy* out, const char** reason) {
    const char* ignored = nullptr;
    const char*& why = reason ? *reason : ignored;
    why = nullptr;
    if (!desc) {
        why = "desc is NULL";
        return CE_E_INVALID_ARGUMENT;
    }
    // struct_size is the client's promise of how much of *desc is readable; check it before anything else.
    if (desc->struct_size < kRuntimeDescBaseSize) {
        why = "desc struct_size is smaller than the API 2.0 descriptor";
        return CE_E_INVALID_ARGUMENT;
    }
    if (CheckApiVersion(desc->api_version) != CE_OK) {
        why = "desc api_version is incompatible with this runtime";
        return CE_E_VERSION;
    }
    if (desc->reserved != 0) {
        why = "desc reserved field is not zero";
        return CE_E_INVALID_ARGUMENT;
    }
    if ((desc->features & ~kKnownFeatures) != 0) {
        why = "desc features contain unknown bits";
        return CE_E_INVALID_ARGUMENT;
    }

    RuntimeDescCopy copy;
    copy.apiVersion = desc->api_version;
    copy.features = desc->features;
    struct Field {
        const char* text;
        size_t maxBytes;
        std::optional<std::string>* path;  // null = client_name (may be empty)
        const char* emptyReason;
        const char* tooLongReason;
        const char* notUtf8Reason;
    };
    const Field fields[] = {
        {desc->package_dir, kMaxPathBytes, &copy.packageDir, "desc package_dir is empty",
         "desc package_dir is too long", "desc package_dir is not valid UTF-8"},
        {desc->data_dir, kMaxPathBytes, &copy.dataDir, "desc data_dir is empty", "desc data_dir is too long",
         "desc data_dir is not valid UTF-8"},
        {desc->client_name, kMaxClientNameBytes, nullptr, nullptr, "desc client_name is too long",
         "desc client_name is not valid UTF-8"},
    };
    try {
        for (const Field& field : fields) {
            if (!field.text)
                continue;
            std::string_view view;
            switch (InspectText(field.text, field.maxBytes, view)) {
                case TextProblem::Empty:
                    if (field.path) {
                        why = field.emptyReason;
                        return CE_E_INVALID_ARGUMENT;
                    }
                    break;
                case TextProblem::TooLong:
                    why = field.tooLongReason;
                    return CE_E_INVALID_ARGUMENT;
                case TextProblem::NotUtf8:
                    why = field.notUtf8Reason;
                    return CE_E_INVALID_ARGUMENT;
                case TextProblem::None:
                    break;
            }
            if (field.path)
                field.path->emplace(view);
            else
                copy.clientName.assign(view);
        }
    } catch (const std::bad_alloc&) {
        why = "out of memory copying the desc strings";
        return CE_E_NO_MEMORY;
    }
    if (out)
        *out = std::move(copy);
    return CE_OK;
}

}  // namespace ce::capi

extern "C" {

// Initializes exactly the extent the caller declared: the bytes [0, struct_size) are zeroed, then
// struct_size and api_version are written. A runtime of a later minor therefore never touches fields
// an older client's struct does not have. On CE_E_VERSION the extent is still initialized (so a client
// that ignores the status hands create() a descriptor it will refuse with the same error).
ce_status_t ce_runtime_desc_init(ce_runtime_desc_t* desc, uint32_t struct_size, uint32_t api_version) {
    if (!desc || struct_size < ce::capi::kRuntimeDescBaseSize)
        return CE_E_INVALID_ARGUMENT;
    std::memset(desc, 0, struct_size);
    desc->struct_size = struct_size;
    desc->api_version = api_version;
    return ce::capi::CheckApiVersion(api_version);
}

}  // extern "C"
