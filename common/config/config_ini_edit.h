#pragma once

// In-place edits of an IniDocument.
//
// A settings change must touch only the line it is about: comments, blank lines, the
// order of sections and keys, lines this build does not understand, the line endings
// (CRLF, LF, lone CR, or a mix) and the BOM stay byte for byte as they were. The edits
// therefore work on IniDocument::sourceLines and re-derive the parsed sections from them
// (RebuildIniSections), so SerializeIni(document) is the original text plus exactly the
// edited lines.
//
// Names and values are UTF-8 (the settings API's encoding). Section and key names match
// the way a lookup does: blanks around them are ignored, the comparison ignores case, the
// FIRST section of a name and the first key in it are the ones that count. Section ""
// addresses the lines before the first section header (which the loader never reads).
//
// What an edit changes:
//   - Set on an existing key rewrites that one line to "<key as written>=<blanks as
//     written><value>", dropping an inline remainder (the value of a line runs to its end).
//     A value that is already there (equal after trimming blanks) leaves the line untouched.
//   - Set on a missing key inserts "key=value" after the last key line of its section (or
//     right after the header of an empty section), with the line ending of the line before it.
//   - Set in a missing section appends a blank separator line (unless the text already ends
//     in one), the "[section]" header and the key at the end of the text. The trailing-newline
//     state of the file is preserved: a text that ended without a terminator still does.
//   - When an insertion follows a final line that has no terminator, that line gains the
//     terminator it needs (the one used by the first terminated line of the text, CRLF in an
//     empty or single-line text): the only existing line an edit can alter besides the edited one.
//   - Remove deletes every "key=value" line of that key in the first matching section, entire
//     line including its terminator. Comments above it, the section header and an emptied
//     section stay.
//
// A key's value is one line. The loader also accepts multi-line lists for a few keys (a
// parenthesized block of lines without '=' after "whitelist=", "overlay_whitelist=",
// "wgc_window_detection=" and the desktop overlay's "process_list="); those continuation
// lines are not part of the value an edit sees and are left where they are. A caller that
// replaces such a key must handle the continuation lines itself.
//
// The text stays valid UTF-8 only if the inputs are: names and values that are not valid
// UTF-8, or that contain NUL, CR or LF, are rejected. A UTF-8 value in a document whose
// existing bytes are another encoding (an ANSI-saved file) mixes encodings; check
// IsUtf8ConfigText(SerializeIni(document)) before writing such a document back.

#include <string_view>

#include "config_ini_reader.h"

namespace ce::config_text {

enum class IniEditStatus {
    kChanged,         // the document now differs
    kUnchanged,       // Set: the value was already there
    kNotFound,        // Remove: no such key
    kInvalidSection,  // a name that cannot be written as "[name]" (contains ']', CR, LF, NUL or invalid UTF-8)
    kInvalidKey,      // empty, contains '=', CR, LF, NUL or invalid UTF-8, or starts with ';' or '['
    kInvalidValue,    // contains CR, LF, NUL or invalid UTF-8
};

IniEditStatus SetIniValue(IniDocument* document, std::string_view section, std::string_view key,
                          std::string_view value);
IniEditStatus RemoveIniValue(IniDocument* document, std::string_view section, std::string_view key);

}  // namespace ce::config_text
