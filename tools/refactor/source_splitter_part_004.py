# part of source_splitter.py (semantic-unit facade parts; see source_splitter.py)
def split_source(facade: Path, grouping: Dict, dry_run: bool = False) -> None:
    """Split a logical source into .cpp units plus a generated internal header.

    The generated header receives: top-level non-conditional directives,
    using/typedef/extern statements, enums, template functions, classes (unless
    overridden), shared file-scope statics and shared static functions, both
    converted to `inline` and renamed with a module prefix so they cannot
    collide with same-named statics of other modules, and prototypes of every
    non-static top-level function. Chunks inside one #if/#endif region or one
    anonymous namespace are atomic: they must all land in the same unit.
    """
    text = reassemble(facade)
    chunks = scan(text)
    module = grouping["module"]
    header_name = grouping["header"]
    units: Dict[str, Dict] = grouping["units"]
    unit_names = list(units)

    # Chunks inside anonymous namespaces keep internal linkage per unit, so
    # they cannot be shared across units. unscope_anon promotes whole anon
    # regions to file scope (prototypes/externs in the header, definitions in
    # their units); unstatic does the same for individual file-scope statics.
    unscope_anon = set(grouping.get("unscope_anon", []))
    unstatic = set(grouping.get("unstatic", []))
    keep_static = set(grouping.get("keep_static", []))
    unscoped_idx: set = set()
    for idx, c in enumerate(chunks):
        is_const_var = c.kind == "var" and re.search(r"\b(constexpr|const)\b", c.text)
        if grouping.get("statics_in_units") and c.static and not is_const_var and idx not in keep_static:
            unscoped_idx.add(idx)
            c.static = False
        if c.anon_region is not None and c.anon_region in unscope_anon and not is_const_var:
            unscoped_idx.add(idx)
            c.ns_path = tuple(part for part in c.ns_path if part != "")
            c.anon_region = None
            c.static = False
        elif idx in unstatic:
            unscoped_idx.add(idx)
            c.static = False

    assignment: Dict[int, str] = {}
    for unit_name, spec in units.items():
        for idx in spec.get("chunks", []):
            if idx in assignment:
                raise RuntimeError(f"chunk {idx} assigned twice")
            assignment[idx] = unit_name
    rest_unit = next((n for n, s in units.items() if s.get("rest")), unit_names[0])
    for idx in range(len(chunks)):
        if idx not in assignment:
            assignment[idx] = rest_unit

    # Anonymous-namespace integrity: all chunks of one anon region share a unit.
    allow_anon_split = set(grouping.get("allow_anon_split", []))
    by_anon: Dict[int, str] = {}
    for idx, c in enumerate(chunks):
        if c.anon_region is None or c.anon_region in allow_anon_split:
            continue
        prev = by_anon.get(c.anon_region)
        if prev is not None and prev != assignment[idx]:
            raise RuntimeError(f"anonymous namespace chunk {idx} split across units")
        by_anon[c.anon_region] = assignment[idx]

    unit_chunks: Dict[str, List[Chunk]] = {name: [] for name in unit_names}
    for idx, c in enumerate(chunks):
        unit_chunks[assignment[idx]].append(c)
    idx_by_id = {id(c): idx for idx, c in enumerate(chunks)}

    keep_in_units = set(grouping.get("keep_in_units", []))
    classes_in_units = set(grouping.get("classes_in_units", []))
    extern_in_units = set(grouping.get("extern_in_units", []))
    hoist_regions = set(grouping.get("hoist_regions", []))
    destatic = set(grouping.get("destatic", []))

    def hoisted_by_rule(idx: int) -> bool:
        c = chunks[idx]
        if idx in keep_in_units:
            return False
        if c.kind == "directive" and not c.ns_path:
            return True
        anon_scope = c.ns_path == ("",)
        if c.kind == "other" and (not c.ns_path or anon_scope):
            code = _code_text(c.text)
            if (
                USING_OR_TYPEDEF.match(code)
                or EXTERN_DECL.match(code)
                or FWD_DECL.match(code)
                or NS_ALIAS.match(code)
            ):
                return True
        if c.kind == "enum":
            return True
        if c.kind == "func" and c.template:
            return True
        if c.kind == "class" and idx not in classes_in_units:
            return True
        if c.kind == "extern" and idx not in extern_in_units:
            return True
        return False

    shared: set = set()
    changed = True
    while changed:
        changed = False
        for idx, c in enumerate(chunks):
            if (
                idx in shared
                or not c.name
                or c.kind not in ("var", "func")
                or c.name in TYPE_KEYWORDS
                or not re.fullmatch(r"[A-Za-z_]\w*", c.name)
            ):
                continue
            if (
                c.kind == "func"
                and not c.static
                and c.anon_region is None
                and not re.search(r"^\s*(inline|constexpr)\b", c.text)
            ):
                continue  # non-static functions get header prototypes, not renames
            if c.kind == "func" and idx in destatic:
                continue  # destatic functions stay in their unit as prototypes
            if (
                c.kind == "var"
                and not c.static
                and c.anon_region is None
                and not re.search(r"\b(constexpr|const)\b", c.text)
                and idx not in unscoped_idx
            ):
                continue  # mutable external globals stay in their unit
            if any(
                j != idx
                and (hoisted_by_rule(j) or j in shared or assignment[j] != assignment[idx])
                and _count_ident(chunks[j].text, c.name) > 0
                for j in range(len(chunks))
            ):
                shared.add(idx)
                changed = True

    # A non-static definition whose name matches a shared static declaration
    # had internal linkage in the original single-TU source (the earlier
    # `static` declaration governs). Hoist the definition as inline too so
    # every unit sees the same entity.
    static_decl_names = {
        chunks[i].name
        for i in shared
        if chunks[i].kind == "func" and "{" not in chunks[i].text and i not in destatic
    }
    if static_decl_names:
        for idx, c in enumerate(chunks):
            if (
                c.kind == "func"
                and c.name in static_decl_names
                and idx not in shared
                and any(
                    j != idx
                    and (hoisted_by_rule(j) or j in shared or assignment[j] != assignment[idx])
                    and _count_ident(chunks[j].text, c.name) > 0
                    for j in range(len(chunks))
                )
            ):
                shared.add(idx)

    shared_statics = [(idx, chunks[idx]) for idx in shared if chunks[idx].kind == "var"]
    shared_funcs = [(idx, chunks[idx]) for idx in shared if chunks[idx].kind == "func"]
    shared_idx = {i for i, _ in shared_statics} | {i for i, _ in shared_funcs}
    # Shared variables are renamed with a module prefix; shared functions keep
    # their names (as inline) so overload sets declared in other headers keep
    # resolving exactly as before.
    renames = {
        c.name: f"{module}_{c.name}"
        for _, c in shared_statics
        if c.name and not c.name.startswith(module + "_")
    }
    define_rewrites = grouping.get("define_prefix_rewrites", [])

    def rename_text(text: str) -> str:
        for old, new in renames.items():
            text = _rename_ident(text, old, new)
        if define_rewrites:
            out_lines = []
            for line in text.split("\n"):
                for old_prefix, new_prefix in define_rewrites:
                    if old_prefix in line:
                        line = line.replace(old_prefix, new_prefix)
                out_lines.append(line)
            text = "\n".join(out_lines)
        return text

    def emit_shared(idx: int, c: Chunk) -> None:
        text = rename_text(c.text)
        comments, body = _split_comments(text)
        body = re.sub(r"^\s*static\s+", "", body, count=1)
        body = re.sub(r"^\s*inline\s+", "", body, count=1)
        ns = () if c.ns_path == ("",) else c.ns_path
        if c.kind == "var":
            header_parts.append(_wrap_ns(comments + "inline " + body, ns))
        elif body.lstrip().startswith("template"):
            header_parts.append(_wrap_ns(comments + body, ns))
        else:
            header_parts.append(_wrap_ns(comments + "inline " + body, ns))

    def unit_body_text(c: Chunk) -> str:
        text = rename_text(c.text)
        if idx_by_id[id(c)] in destatic or idx_by_id[id(c)] in unscoped_idx:
            comments, body = _split_comments(text)
            body = re.sub(r"^\s*static\s+", "", body, count=1)
            body = re.sub(r"^\s*inline\s+", "", body, count=1)
            text = comments + body
        if (
            c.kind == "func"
            and c.anon_region is None
            and (not c.static or idx_by_id[id(c)] in destatic)
        ):
            text = _strip_definition_defaults(text)
        return text

    header_parts: List[str] = ["#pragma once"]
    header_idx: set = set()

    # Pass 0: forward declarations of hoisted classes so earlier typedefs and
    # prototypes can reference them as incomplete types.
    for idx, c in enumerate(chunks):
        if idx in keep_in_units or idx in header_idx:
            continue
        if c.kind == "class" and idx not in classes_in_units:
            ns = c.ns_path
            prefix = "class" if _code_text(c.text).lstrip().startswith("class") else "struct"
            header_parts.append(_wrap_ns(f"{prefix} {c.name};", ns))

    # Pass 1: types and declarations in original order.
    for idx, c in enumerate(chunks):
        if idx in keep_in_units:
            continue
        if idx in header_idx:
            continue
        if c.kind == "directive" and not c.ns_path:
            if c.text.strip() != "#pragma once":
                header_parts.append(c.text)
            header_idx.add(idx)
        elif c.kind == "other":
            code = _code_text(c.text)
            if (
                USING_OR_TYPEDEF.match(code)
                or EXTERN_DECL.match(code)
                or FWD_DECL.match(code)
                or NS_ALIAS.match(code)
            ):
                header_parts.append(rename_text(c.text))
                header_idx.add(idx)
        elif c.kind == "enum":
            header_parts.append(_wrap_ns(rename_text(c.text), c.ns_path))
            header_idx.add(idx)
        elif c.kind == "pp_region" and (
            idx in hoist_regions or _is_pure_directive_region(c.text)
        ):
            header_parts.append(rename_text(c.text))
            header_idx.add(idx)

    # Pass 2: prototypes.
    for idx, c in enumerate(chunks):
        if idx in header_idx:
            continue
        if (
            c.kind != "func"
            or c.template
            or (c.static and idx not in destatic)
            or "" in c.ns_path
            or re.search(r"[A-Za-z_]\w*::[~A-Za-z_]\w*\s*\(", c.signature)
        ):
            continue
        if not c.signature:
            continue
        proto = rename_text(c.signature.rstrip().rstrip(";"))
        if idx in destatic or idx in unscoped_idx:
            proto = re.sub(r"^\s*static\s+", "", proto, count=1)
            proto = re.sub(r"^\s*inline\s+", "", proto, count=1)
        header_parts.append(_wrap_ns(proto + ";", c.ns_path))
        if "{" not in c.text:
            header_idx.add(idx)  # declaration-only: prototype replaces it in units

    # Pass 3: definitions in original order (templates, classes, extern blocks,
    # shared statics/functions at their original positions). Shared static
    # variables are emitted first so extern declarations (Pass 4) and inline
    # functions/classes that reference renamed constants or extern globals see
    # them in order.
    def pass3(kinds: set) -> None:
        for idx, c in enumerate(chunks):
            if idx in keep_in_units or idx in header_idx or c.kind not in kinds:
                continue
            if idx in shared_idx:
                if idx in unscoped_idx:
                    # Definition stays in its unit (renamed); Pass 4 emits the
                    # extern declaration for cross-unit references.
                    pass
                else:
                    emit_shared(idx, c)
                    header_idx.add(idx)
            elif c.kind == "func" and c.template:
                header_parts.append(_wrap_ns(rename_text(c.text), c.ns_path))
                header_idx.add(idx)
            elif c.kind == "class" and idx not in classes_in_units:
                header_parts.append(_wrap_ns(rename_text(c.text), c.ns_path))
                header_idx.add(idx)
            elif c.kind == "extern" and idx not in extern_in_units:
                if not re.search(r"\)\s*\{", c.text):
                    # Declaration-only extern block: safe to share via the header.
                    header_parts.append(_wrap_ns(rename_text(c.text), c.ns_path))
                    header_idx.add(idx)
                # Definition blocks (function bodies inside extern "C") stay in
                # their unit; hoisting them would duplicate the definitions.

    pass3({"var"})

    # Pass 4: extern declarations for mutable globals used from other units.
    # Emitted after shared statics so declarations that reference renamed
    # inline constants (e.g. array bounds) see their definitions, and before
    # inline functions/classes that reference the globals themselves.
    for idx, c in enumerate(chunks):
        if idx in header_idx or c.kind not in ("var", "other") or not c.name:
            continue
        if c.static or c.anon_region is not None or re.search(r"\b(constexpr|const)\b", c.text):
            continue
        if not any(
            j != idx
            and (hoisted_by_rule(j) or j in shared or assignment[j] != assignment[idx])
            and _count_ident(chunks[j].text, c.name) > 0
            for j in range(len(chunks))
        ):
            continue
        extern = _extern_decl(rename_text(c.text), renames.get(c.name, c.name))
        if extern:
            header_parts.append(_wrap_ns(extern, c.ns_path))

    pass3({"func", "class", "extern"})

    if dry_run:
        for name in unit_names:
            print(f"=== {name}: {len(unit_chunks[name])} chunks ===")
            for c in unit_chunks[name]:
                print(f"  {c.kind:<10} {c.name:<38} lines {c.start + 1}-{c.end}")
        print(f"=== header: {len(header_parts)} parts; renames: {sorted(renames)} ===")
        return

    out_dir = facade.parent
    (out_dir / header_name).write_text("\n\n".join(header_parts) + "\n", encoding="utf-8", newline="\n")
    for name in unit_names:
        body = "\n\n".join(
            _wrap_ns(unit_body_text(c), c.ns_path)
            for c in unit_chunks[name]
            if idx_by_id[id(c)] not in header_idx
        )
        unit_text = f'#include "{header_name}"\n\n{body}\n'
        (out_dir / name).write_text(unit_text, encoding="utf-8", newline="\n")
    for frag in grouping.get("delete", []):
        (out_dir / frag).unlink(missing_ok=True)
    print(f"split {module}: {len(chunks)} chunks -> {unit_names}")


def main(argv: List[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd = argv[1]
    facade = Path(argv[2])
    if cmd == "reassemble":
        out: Optional[Path] = None
        if len(argv) > 4 and argv[3] == "--out":
            out = Path(argv[4])
        text = reassemble(facade)
        if out:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(text, encoding="utf-8", newline="\n")
            print(f"wrote {out} ({len(text.splitlines())} lines)")
        else:
            sys.stdout.write(text)
        return 0
    if cmd == "map":
        for i, c in enumerate(scan(reassemble(facade))):
            print(
                f"{i:4d} {c.kind:<10} {c.name:<38} lines {c.start + 1:6d}-{c.end:6d} "
                f"ns={'.'.join(c.ns_path) or '-'} anon={c.anon_region} tmpl={int(c.template)} "
                f"static={int(c.static)}"
            )
        return 0
    if cmd == "split":
        grouping = json.loads(Path(argv[3]).read_text(encoding="utf-8"))
        split_source(facade, grouping, dry_run=len(argv) > 4 and argv[4] == "--dry-run")
        return 0
    print(f"unknown command: {cmd}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
