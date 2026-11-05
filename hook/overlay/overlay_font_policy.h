#pragma once

#include <cstddef>
#include <cwctype>

// Which font the overlay asks GDI for. Pure, so the choice is testable without a device context.
namespace ce::overlay_font {

// Tried in order when no font is configured or the configured one is not installed. Bahnschrift's DIN
// digits read fastest at overlay sizes and it needs the least row height for their size; it ships with
// Windows 10 1709 and later, Segoe UI with every supported Windows, Arial with the rest.
inline constexpr const char* kDefaultFaces[] = {"Bahnschrift SemiBold", "Segoe UI Semibold", "Segoe UI", "Arial"};

inline constexpr int kDefaultBasePixelSize = 14;
inline constexpr int kMinBasePixelSize = 8;
inline constexpr int kMaxBasePixelSize = 96;

inline constexpr int kWeightNormal = 400;
inline constexpr int kWeightBold = 700;

inline wchar_t AsciiLower(wchar_t ch) {
    return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
}

inline bool ContainsAsciiWord(const char* face, const char* word) {
    for (const char* start = face; *start; ++start) {
        const char* a = start;
        const char* b = word;
        while (*a && *b && AsciiLower(static_cast<unsigned char>(*a)) == static_cast<unsigned char>(*b)) {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

// A face that names its own weight ("Bahnschrift SemiBold") is drawn as designed: asking GDI for bold on
// top of it would smear it with synthetic emboldening. A plain family is drawn bold, for legibility at
// overlay sizes. An explicit weight always wins.
inline int ResolveWeight(const char* face, int configuredWeight) {
    if (configuredWeight > 0)
        return configuredWeight;
    if (!face)
        return kWeightBold;
    for (const char* word : {"thin", "light", "regular", "medium", "semi", "demi", "bold", "black", "heavy"}) {
        if (ContainsAsciiWord(face, word))
            return kWeightNormal;
    }
    return kWeightBold;
}

// CreateFont never fails for a font that is not installed: GDI substitutes the nearest one, so the only
// proof is the face it selected. GDI reads 31 characters of a requested name and reports the full name
// back ("Bahnschrift SemiBold SemiConden" selects "... SemiCondensed"), so a prefix match is the test.
inline bool FaceMatches(const wchar_t* requested, const wchar_t* selected) {
    if (!requested || !selected || !*requested)
        return false;
    size_t index = 0;
    for (; requested[index] && index < 31; ++index) {
        if (AsciiLower(requested[index]) != AsciiLower(selected[index]))
            return false;
    }
    return true;
}

}  // namespace ce::overlay_font
