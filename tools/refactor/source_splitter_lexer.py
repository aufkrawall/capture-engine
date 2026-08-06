# part of source_splitter.py (semantic-unit facade parts; see source_splitter.py)
class Token:
    kind: str
    text: str
    line: int


def lex(text: str) -> List[Token]:
    """Tokenize C++ text; comments and string contents become opaque."""
    tokens: List[Token] = []
    i = 0
    n = len(text)
    line = 0
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise RuntimeError("unterminated block comment")
            line += text.count("\n", i, j + 2)
            i = j + 2
            continue
        if text.startswith('R"', i) or text.startswith('LR"', i):
            j = text.find("(", i)
            if 0 <= j < i + 32 and "\\" not in text[i : j + 1] and text[i : j + 1].count('"') == 1:
                delim = text[i + 2 : j] if text.startswith('R"', i) else text[i + 3 : j]
                end = text.find(")" + delim + '"', j)
                if end < 0:
                    raise RuntimeError("unterminated raw string")
                line += text.count("\n", i, end + 1)
                i = end + len(delim) + 2
                continue
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            if j >= n:
                raise RuntimeError("unterminated string literal")
            line += text.count("\n", i, j + 1)
            tokens.append(Token("OTHER", text[i : j + 1], line))
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    break
                j += 1
            if j >= n:
                raise RuntimeError("unterminated char literal")
            line += text.count("\n", i, j + 1)
            i = j + 1
            continue
        if c == "#":
            j = i
            while j < n and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n and text[j + 1] == "\n":
                    j += 2
                    continue
                j += 1
            tokens.append(Token("DIR", text[i:j].strip(), line))
            line += text.count("\n", i, j)
            i = j
            continue
        if c in "{}();":
            tokens.append(Token(c, c, line))
            i += 1
            continue
        if c.isalpha() or c == "_":
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            tokens.append(Token("IDENT", text[i:j], line))
            i = j
            continue
        tokens.append(Token("OTHER", c, line))
        i += 1
    return tokens


@dataclass
class Chunk:
    kind: str  # func | var | class | namespace | enum | extern | pp_region | directive | other
    name: str
    start: int  # 0-based line
    end: int  # exclusive
    text: str
    ns_path: Tuple[str, ...] = ()
    anon_region: Optional[int] = None
    template: bool = False
    static: bool = False
    signature: str = ""


def _skip_template(tokens: Sequence[Token], idx: int) -> Tuple[int, bool]:
    if idx < len(tokens) and tokens[idx].text == "template":
        depth = 0
        i = idx
        while i < len(tokens):
            if tokens[i].text == "<":
                depth += 1
            elif tokens[i].text == ">":
                depth -= 1
                if depth == 0:
                    return i + 1, True
            i += 1
    return idx, False


def _last_ident(tokens: Sequence[Token]) -> str:
    for t in reversed(tokens):
        if t.kind == "IDENT":
            return t.text
    return ""


TYPE_KEYWORDS = {
    "static",
    "const",
    "constexpr",
    "consteval",
    "volatile",
    "inline",
    "thread_local",
    "extern",
    "unsigned",
    "signed",
    "long",
    "short",
    "struct",
    "class",
    "enum",
    "union",
    "typename",
    "auto",
    "void",
    "bool",
    "char",
    "wchar_t",
    "int",
    "float",
    "double",
    "nullptr",
    "true",
    "false",
    "NULL",
    "nullptr_t",
}


def _var_name(tokens: Sequence[Token]) -> str:
    """Name of a top-level variable statement: last identifier before the initializer."""
    cutoff = len(tokens)
    for i, t in enumerate(tokens):
        if t.text in ("=", "{", "["):
            cutoff = i
            break
    for t in reversed(tokens[:cutoff]):
        if t.kind == "IDENT" and t.text not in TYPE_KEYWORDS:
            return t.text
    return ""


def _func_name(pending: Sequence[Token]) -> str:
    for i, t in enumerate(pending):
        if t.text == "(" and i > 0:
            prev = pending[i - 1]
            if prev.kind == "IDENT" and prev.text != "operator":
                if i >= 2 and pending[i - 2].text == "~":
                    return "~" + prev.text
                return prev.text
            if prev.text == "operator":
                parts = ["operator"]
                for op in pending[i:]:
                    if op.text == "(":
                        break
                    parts.append(op.text)
                return "".join(parts)
    return _last_ident(pending)


def _looks_like_block_start(pending: Sequence[Token]) -> bool:
    """True when '{' starts a parameter-list block (function, lambda, ctor)."""
    last_close = -1
    for i, t in enumerate(pending):
        if t.text == ")":
            last_close = i
    if last_close < 0:
        return False
    depth = 0
    open_idx = -1
    for i in range(last_close, -1, -1):
        if pending[i].text == ")":
            depth += 1
        elif pending[i].text == "(":
            depth -= 1
            if depth == 0:
                open_idx = i
                break
    if open_idx < 0:
        return False
    prefix = pending[:open_idx]
    if any(t.text in ("=", "{") for t in prefix):
        return False
    if not any(t.kind == "IDENT" for t in prefix):
        return False
    tail = pending[last_close + 1 :]
    if not tail:
        return True
    for t in tail:
        if t.text in ("const", "noexcept", "override", "final", "volatile", "&", "*", "->", "<", ">", "::", "[", "]"):
            continue
        if t.kind == "IDENT":
            continue
        return False
    return True


def _classify_block(pending: Sequence[Token]) -> Tuple[str, str, bool]:
    """Classify a top-level '{...}' block from its leading tokens."""
    idx, is_tmpl = _skip_template(pending, 0)
    while idx < len(pending) and pending[idx].kind == "DIR":
        idx += 1
    is_extern_c = (
        idx < len(pending)
        and pending[idx].text == "extern"
        and idx + 1 < len(pending)
        and pending[idx + 1].kind == "OTHER"
        and pending[idx + 1].text.startswith('"')
    )
    if is_extern_c:
        idx += 2
    while (
        idx < len(pending)
        and pending[idx].text == "__declspec"
        and idx + 1 < len(pending)
        and pending[idx + 1].text == "("
    ):
        depth = 0
        i = idx + 1
        while i < len(pending):
            if pending[i].text == "(":
                depth += 1
            elif pending[i].text == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        idx = i + 1
    if idx < len(pending) and pending[idx].text in ("class", "struct"):
        name = pending[idx + 1].text if idx + 1 < len(pending) and pending[idx + 1].kind == "IDENT" else ""
        return "class", name, is_tmpl
    if idx < len(pending) and pending[idx].text == "namespace":
        if idx + 1 < len(pending) and pending[idx + 1].kind == "IDENT":
            parts = [pending[idx + 1].text]
            i = idx + 2
            while (
                i + 2 < len(pending)
                and pending[i].text == ":"
                and pending[i + 1].text == ":"
                and pending[i + 2].kind == "IDENT"
            ):
                parts.append(pending[i + 2].text)
                i += 3
            return "namespace", "::".join(parts), False
        return "namespace", "", False
    if idx < len(pending) and pending[idx].text == "enum":
        for t in pending[idx + 1 :]:
            if t.kind == "IDENT":
                return "enum", t.text, False
        return "enum", "", False
    if is_extern_c and idx >= len(pending):
        return "extern", "", False
    for i in range(idx, len(pending)):
        t = pending[i]
        if t.text == "(" and i > 0 and (
            pending[i - 1].kind == "IDENT" or pending[i - 1].text in (">", "~", ")", "=")
        ):
            return "func", _func_name(pending[idx:]), is_tmpl
    if is_extern_c:
        return "extern", "", False
    if any(t.text == "=" for t in pending):
        return "var", _last_ident(pending), False
    return "other", "", False


def _signature_of(chunk_text: str) -> str:
    depth = 0
    for idx, ch in enumerate(chunk_text):
        if ch == "{":
            if depth == 0:
                sig = re.sub(r"\s+", " ", chunk_text[:idx]).strip()
                sig = re.sub(r"\s*,\s*", ", ", sig)
                sig = re.sub(r"\s*;\s*", "; ", sig)
                return sig
            depth += 1
        elif ch == "}":
            depth -= 1
    return re.sub(r"\s+", " ", chunk_text).strip()


def _strip_defaults(sig: str) -> str:
    """Remove '= default' argument initializers from a function signature."""
    out: List[str] = []
    depth = 0
    skip = False
    i = 0
    n = len(sig)
    while i < n:
        ch = sig[i]
        if skip:
            if depth == 1 and ch in ",)":
                skip = False
                out.append(ch)
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            i += 1
            continue
        if ch == "(":
            depth += 1
            out.append(ch)
        elif ch == ")":
            depth -= 1
            out.append(ch)
        elif ch == "=" and depth == 1:
            skip = True
            if out and out[-1] == " ":
                out.pop()
            out.append(" ")
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def _strip_definition_defaults(chunk_text: str) -> str:
    """Remove default argument initializers from a definition's signature."""
    comments, body = _split_comments(chunk_text)
    depth = 0
    for idx, ch in enumerate(body):
        if ch == "{":
            if depth == 0:
                sig = body[:idx]
                stripped = _strip_defaults(sig)
                return comments + stripped + body[idx:]
            depth += 1
        elif ch == "}":
            depth -= 1
    return chunk_text


def _find_matching_endif(tokens: Sequence[Token], start: int) -> int:
    depth = 0
    for i in range(start, len(tokens)):
        t = tokens[i]
        if t.kind != "DIR":
            continue
        m = COND_DIRECTIVE.match(t.text)
        if not m:
            continue
        d = m.group(1)
        if d in ("if", "ifdef", "ifndef"):
            depth += 1
        elif d == "endif":
            depth -= 1
            if depth == 0:
                return i
    raise RuntimeError("unterminated #if region")


