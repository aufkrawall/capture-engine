# Minimal privileged sensor/ETW broker. Never link the media or FFmpeg runtime.


def compile_elevation_service(env, clang_exe, cflags):
    log("Compiling elevation service x64...")
    source_names = [
        "captureengine/sensors/sensor_plugin.cpp",
        "captureengine/sensors/sensor_bridge_host.cpp",
        "captureengine/sensors/sensor_bridge_lhm.cpp",
        "captureengine/sensors/clr_interop.cpp",
    ]
    sources = [os.path.join(PROJECT_ROOT, name) for name in source_names]
    sources += sorted(glob.glob(os.path.join(PROJECT_ROOT, "elevationservice", "*.cpp")))
    pairs = []
    for source in sources:
        relative = os.path.relpath(source, PROJECT_ROOT)
        obj = os.path.join(OBJ_DIR, "elevation", os.path.splitext(relative)[0] + ".o").replace("\\", "/")
        pairs.append((source, obj))
    parallel_compile(env, clang_exe, cflags + ["-DCE_ELEVATION_SERVICE=1"], pairs)
    common_objects = [
        os.path.join(OBJ_DIR, "x64", os.path.relpath(source, PROJECT_ROOT).replace(".cpp", ".o"))
        for source in common_sources()
    ]
    resource = os.path.join(OBJ_DIR, "elevation", "service.res.o")
    run_command([get_windres_exe("x64"), os.path.join(PROJECT_ROOT, "elevationservice", "service.rc"),
                 "-o", resource], env=env, cwd=os.path.join(PROJECT_ROOT, "elevationservice"))
    common_objects.append(resource)
    output = os.path.join(BIN_DIR, "captureengine_elevation_service.exe")
    flags = ["-mwindows", "-municode", "-static", "-static-libgcc", "-static-libstdc++"]
    flags += LD_OPT_FLAGS + get_x64_linker_flags(clang_exe)
    flags += [
        "-ladvapi32", "-lbcrypt", "-lole32", "-loleaut32", "-lshell32", "-luser32",
        "-luuid", "-ltdh", "-lwinmm", "-ldbghelp", "-lshlwapi", "-lpsapi", "-lversion",
        "-lwintrust", "-lws2_32", "-lruntimeobject",
    ]
    if env.get("CE_DISABLE_LTO") != "1":
        flags.append("-flto")
    if any(flag.startswith("-fsanitize=") for flag in cflags):
        flags.append("-fsanitize=address,undefined")
    append_windows_pdb_linker_flag(flags, output)
    temporary = os.path.join(OBJ_DIR, "elevation", "service.tmp.exe")
    safe_delete_file(temporary)
    run_command([clang_exe] + [obj for _, obj in pairs] + common_objects + flags + ["-o", temporary], env=env)
    if not safe_copy_file(temporary, output):
        raise RuntimeError("Cannot place elevation service executable (destination may be locked)")
    safe_delete_file(temporary)
    record_verification_artifact("elevation_service_exe", output)
