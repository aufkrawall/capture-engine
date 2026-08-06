# part of source_splitter.py (semantic-unit facade parts; see source_splitter.py)
class Scanner:
    def __init__(self, text: str) -> None:
        self.text = text
        self.lines = text.split("\n")
        self.chunks: List[Chunk] = []
        self.next_anon = 1

    def scan(self) -> List[Chunk]:
        self._scan_region(0, len(self.lines), ())
        return self.chunks

    def _slice(self, start_line: int, end_line: int) -> str:
        return "\n".join(self.lines[start_line:end_line])

    def _scan_region(self, start_line: int, end_line: int, ns_path: Tuple[str, ...],
                     anon: Optional[int] = None) -> None:
        tokens = lex(self._slice(start_line, end_line))
        pending: List[Token] = []
        pending_line = 0
        pending_is_dir = False
        anon_region = anon
        i = 0
        n = len(tokens)

        def emit(kind: str, name: str, p_line: int, e_line: int, sig: str = "", is_tmpl: bool = False,
                 is_static: bool = False, anon: Optional[int] = None) -> None:
            abs_start = start_line + p_line
            while abs_start > 0:
                stripped = self.lines[abs_start - 1].strip()
                if not (
                    stripped.startswith("//")
                    or stripped.startswith("/*")
                    or stripped.startswith("*")
                    or stripped.endswith("*/")
                ):
                    break
                abs_start -= 1
            self.chunks.append(
                Chunk(
                    kind=kind,
                    name=name,
                    start=abs_start,
                    end=start_line + e_line,
                    text=self._slice(abs_start, start_line + e_line),
                    ns_path=ns_path,
                    anon_region=anon_region,
                    template=is_tmpl,
                    static=is_static,
                    signature=sig,
                )
            )

        def flush_statement(stmt_line: Optional[int] = None) -> None:
            nonlocal pending, pending_is_dir
            if not pending:
                return
            start = pending_line
            end = stmt_line if stmt_line is not None else pending[-1].line
            if pending_is_dir:
                emit("directive", "", start, end + 1)
            else:
                is_static = any(t.text == "static" for t in pending)
                is_tmpl = pending[0].text == "template"
                is_decl_stmt = any(t.text in ("typedef", "using", "static_assert") for t in pending[:3])
                is_ns_alias = (
                    len(pending) >= 3
                    and pending[0].text == "namespace"
                    and pending[1].kind == "IDENT"
                    and pending[2].text == "="
                )
                first_paren = next((i for i, t in enumerate(pending) if t.text == "("), None)
                first_eq = next((i for i, t in enumerate(pending) if t.text == "="), None)
                first_param_literal = (
                    first_paren is not None
                    and first_paren + 1 < len(pending)
                    and (
                        pending[first_paren + 1].kind == "OTHER"
                        and (
                            pending[first_paren + 1].text[:1].isdigit()
                            or pending[first_paren + 1].text[:1] in ("'", '"')
                        )
                    )
                )
                is_func_decl = (
                    not is_decl_stmt
                    and not is_ns_alias
                    and not first_param_literal
                    and first_paren is not None
                    and first_paren > 0
                    and pending[first_paren - 1].kind == "IDENT"
                    and pending[first_paren - 1].text != "operator"
                    and (first_eq is None or first_eq > first_paren)
                )
            if (
                (any(t.text == "=" for t in pending) or is_static or anon_region is not None)
                and not is_decl_stmt
                and not is_ns_alias
                and not is_func_decl
            ):
                emit("var", _var_name(pending), start, end + 1, is_tmpl=is_tmpl, is_static=is_static)
            elif is_func_decl:
                sig = re.sub(r"\s+", " ", self._slice(start_line + start, start_line + end + 1)).strip()
                emit("func", _func_name(pending), start, end + 1, sig=sig, is_tmpl=is_tmpl,
                     is_static=is_static)
            else:
                name = (
                    _var_name(pending)
                    if any(t.text in ("{", "=", "[") for t in pending)
                    else _last_ident(pending)
                )
                emit("other", name, start, end + 1, is_tmpl=is_tmpl, is_static=is_static)
            pending = []
            pending_is_dir = False

        while i < n:
            t = tokens[i]
            if t.kind == "DIR":
                m = COND_DIRECTIVE.match(t.text)
                if m and not pending and m.group(1) in ("if", "ifdef", "ifndef"):
                    end_tok = _find_matching_endif(tokens, i)
                    end_line = tokens[end_tok].line
                    emit("pp_region", "", t.line, end_line + 1)
                    i = end_tok + 1
                    pending = []
                    pending_is_dir = False
                    continue
                if not pending and not m:
                    end_line = t.line + 1
                    while (
                        end_line - 1 < len(self.lines)
                        and self.lines[end_line - 1].rstrip().endswith("\\")
                    ):
                        end_line += 1
                    emit("directive", "", t.line, end_line)
                    i += 1
                    continue
                if pending:
                    pending.append(t)
                else:
                    pending = [t]
                    pending_line = t.line
                    pending_is_dir = True
                i += 1
                continue
            if t.text == ";":
                if pending:
                    flush_statement(t.line)
                i += 1
                continue
            if t.text == "{":
                if pending:
                    is_decl_block = any(
                        t2.text in ("class", "struct", "namespace", "enum", "union") for t2 in pending
                    ) or (
                        pending[0].text == "extern"
                        and any(t2.kind == "OTHER" and t2.text.startswith('"') for t2 in pending[1:3])
                    )
                    is_type_block = any(
                        t2.text in ("class", "struct", "enum", "union") for t2 in pending
                    )
                    if not _looks_like_block_start(pending) and not is_decl_block:
                        # braced-initializer statement: keep it in pending and flush at ';'
                        pending.append(t)
                        i += 1
                        continue
                    depth = 0
                    j = i
                    while j < n:
                        if tokens[j].kind == "DIR":
                            dm = COND_DIRECTIVE.match(tokens[j].text)
                            if dm and dm.group(1) in ("if", "ifdef", "ifndef"):
                                j = _find_matching_endif(tokens, j)
                            j += 1
                            continue
                        if tokens[j].text == "{":
                            depth += 1
                        elif tokens[j].text == "}":
                            depth -= 1
                            if depth == 0:
                                break
                        j += 1
                    if j >= n:
                        raise RuntimeError("unbalanced braces")
                    start = pending_line
                    end = tokens[j].line
                    trailing_ident = ""
                    if is_type_block:
                        k = j + 1
                        while k < n and tokens[k].text != ";":
                            k += 1
                        if k < n:
                            for t2 in tokens[j + 1 : k]:
                                if t2.kind == "IDENT" and t2.text not in TYPE_KEYWORDS:
                                    trailing_ident = t2.text
                            end = tokens[k].line
                            j = k
                    elif j + 1 < n and tokens[j + 1].text == ";":
                        end = tokens[j + 1].line
                        j += 1
                    kind, name, is_tmpl = _classify_block(pending)
                    is_static = any(t2.text == "static" for t2 in pending)
                    if is_type_block and trailing_ident:
                        # `static thread_local struct X {...} var;` defines a
                        # variable, not just a type; track it like one.
                        kind, name = "var", trailing_ident
                    if kind == "namespace":
                        open_abs = start_line + start
                        close_abs = start_line + end
                        inner_start = None
                        for ln in range(open_abs, close_abs):
                            if "{" in self.lines[ln]:
                                inner_start = ln + 1
                                break
                        if inner_start is None:
                            raise RuntimeError("namespace without brace")
                        child_ns = ns_path + (name,) if name else ns_path + ("",)
                        if name:
                            self._scan_region(inner_start, close_abs, child_ns, anon_region)
                        else:
                            anon = self.next_anon
                            self.next_anon += 1
                            self._scan_region(inner_start, close_abs, child_ns, anon)
                    else:
                        chunk_slice = self._slice(start_line + start, start_line + end + 1)
                        sig = _signature_of(chunk_slice) if kind == "func" else ""
                        emit(kind, name, start, end + 1, sig=sig, is_tmpl=is_tmpl, is_static=is_static)
                    i = j + 1
                    pending = []
                    pending_is_dir = False
                    continue
            if not pending:
                pending_line = t.line
            pending.append(t)
            pending_is_dir = pending_is_dir and t.kind == "DIR"
            i += 1
        if pending:
            flush_statement()


def scan(text: str) -> List[Chunk]:
    return Scanner(text).scan()


def _count_ident(text: str, name: str) -> int:
    return len(re.findall(r"\b" + re.escape(name) + r"\b", text))


def _wrap_ns(text: str, ns_path: Tuple[str, ...]) -> str:
    if not ns_path:
        return text
    open_braces = "\n".join("namespace {" if p == "" else f"namespace {p} {{" for p in ns_path)
    close_braces = "\n".join("}" for _ in ns_path)
    return f"{open_braces}\n{text}\n{close_braces}"


