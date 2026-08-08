

def _stage_ffmpeg_corresponding_source(source_repo: str, downloads_root: str, destination_root: str) -> List[str]:
    """Stage the exact LGPL source and build inputs corresponding to the shipped DLLs."""
    git_exe = shutil.which("git")
    if not git_exe:
        raise RuntimeError("Git is required to create the FFmpeg corresponding-source archive")
    if not os.path.isdir(source_repo):
        raise RuntimeError(f"FFmpeg source checkout is missing: {source_repo}")

    head = run_command([git_exe, "-C", source_repo, "rev-parse", "HEAD"]).strip()
    if head.lower() != FFMPEG_SOURCE_REF.lower():
        raise RuntimeError(f"FFmpeg source checkout is {head or 'unknown'}, expected {FFMPEG_SOURCE_REF}")
    status = run_command([git_exe, "-C", source_repo, "status", "--porcelain"]).strip()
    if status:
        raise RuntimeError("FFmpeg source checkout is dirty; refusing to publish ambiguous corresponding source")

    ffmpeg_destination = os.path.join(destination_root, "ffmpeg")
    for current_root, directories, filenames in os.walk(source_repo):
        for name in directories + filenames:
            if name != ".git" and os.path.islink(os.path.join(current_root, name)):
                raise RuntimeError(f"Refusing to package an FFmpeg source symlink: {os.path.join(current_root, name)}")
    shutil.copytree(source_repo, ffmpeg_destination, ignore=shutil.ignore_patterns(".git"))

    patches_root = os.path.join(PROJECT_ROOT, "tools", "patches", "ffmpeg")
    patch_paths = sorted(glob.glob(os.path.join(patches_root, "*.patch")))
    normalize_custom_patch_targets(ffmpeg_destination, patch_paths)
    # A nested temporary repository prevents `git apply` from discovering the
    # CaptureEngine repository above the staging directory.
    run_command([git_exe, "init", "--quiet"], cwd=ffmpeg_destination)
    try:
        for patch_path in patch_paths:
            run_command([git_exe, "apply", "--verbose", patch_path], cwd=ffmpeg_destination)
    finally:
        temporary_git = os.path.join(ffmpeg_destination, ".git")
        if os.path.isdir(temporary_git):
            shutil.rmtree(temporary_git)

    build_inputs = os.path.join(destination_root, "captureengine-build-inputs")
    os.makedirs(build_inputs, exist_ok=True)
    shutil.copy2(os.path.join(PROJECT_ROOT, "build.py"), build_inputs)
    shutil.copytree(
        os.path.join(PROJECT_ROOT, "tools", "build"),
        os.path.join(build_inputs, "tools", "build"),
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
    )
    for relative in (
        os.path.join("tools", "dependency_build_policy.py"),
        os.path.join("tools", "dependency_pgp.py"),
        os.path.join("tools", "ffmpeg_dependencies.json"),
        os.path.join("tools", "ffmpeg_dependencies.py"),
        os.path.join("tools", "ffmpeg_patch_utils.py"),
        os.path.join("tools", "source_download.py"),
    ):
        source = os.path.join(PROJECT_ROOT, relative)
        destination = os.path.join(build_inputs, relative)
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        shutil.copy2(source, destination)
    shutil.copytree(patches_root, os.path.join(build_inputs, "tools", "patches", "ffmpeg"))
    shutil.copytree(os.path.join(PROJECT_ROOT, "tools", "pgp-keys"), os.path.join(build_inputs, "tools", "pgp-keys"))

    libiconv = next(
        (item for item in FFMPEG_DEPENDENCY_MANIFEST_DATA["dependencies"] if item.get("name") == "libiconv"), None
    )
    if not libiconv:
        raise RuntimeError("FFmpeg dependency manifest has no libiconv source entry")
    source_archives = (
        (os.path.basename(libiconv["upstream_source_url"]), libiconv["upstream_source_sha256"]),
        (libiconv["source_package"], libiconv["source_package_sha256"]),
    )
    dependency_destination = os.path.join(destination_root, "libiconv-source")
    os.makedirs(dependency_destination, exist_ok=True)
    manifest_lines = [f"FFmpeg commit: {FFMPEG_SOURCE_REF}", "Local FFmpeg patches are already applied under ffmpeg/."]
    for filename, expected_sha256 in source_archives:
        source = os.path.join(downloads_root, filename)
        if not os.path.isfile(source) or sha256_file(source).lower() != expected_sha256.lower():
            raise RuntimeError(f"Missing or invalid corresponding-source archive: {source}")
        shutil.copy2(source, os.path.join(dependency_destination, filename))
        manifest_lines.append(f"SHA256 {expected_sha256}  libiconv-source/{filename}")
    source_package_signature = os.path.join(downloads_root, libiconv["source_package"] + ".sig")
    if os.path.isfile(source_package_signature):
        shutil.copy2(
            source_package_signature,
            os.path.join(dependency_destination, os.path.basename(source_package_signature)),
        )

    manifest_lines.extend(
        [
            "",
            "captureengine-build-inputs/ contains the release's FFmpeg build driver, dependency manifest,",
            "vendored verification keys, and local patches. The tagged CaptureEngine source tree provides",
            "the remaining first-party sources. See tools/licenses/FFmpeg_NOTICE.txt in that tree.",
        ]
    )
    write_text_atomic(os.path.join(destination_root, "SOURCE_MANIFEST.txt"), "\n".join(manifest_lines) + "\n")
    return sorted(
        os.path.relpath(os.path.join(current_root, filename), destination_root).replace("\\", "/")
        for current_root, _, filenames in os.walk(destination_root)
        for filename in filenames
    )
