// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef _WIN32
    #include <unistd.h>
#endif

#include "../libcc/libcc.hh"
#include "config.hh"
#include "toolchain.hh"

struct TargetData {
    const char *name;
    TargetType type;

    const char *c_pch_filename;
    const char *cxx_pch_filename;

    Span<const char *const> include_directories;
    HeapArray<const char *> libraries;

    HeapArray<ObjectInfo> objects;
};

struct TargetSet {
    HeapArray<TargetData> targets;
    HashMap<const char *, Size> targets_map;

    BlockAllocator str_alloc;

    TargetData *CreateTarget(const Config &config, const char *target_name,
                             const char *output_directory);
};

struct BuildCommand {
    const char *dest_filename;
    const char *cmd;
};

struct BuildSet {
    HeapArray<BuildCommand> commands;
    HashSet<const char *> commands_set;

    BlockAllocator str_alloc;
};

static int64_t GetFileModificationTime(const char *filename)
{
    FileInfo file_info;
    if (!StatFile(filename, false, &file_info))
        return -1;

    return file_info.modification_time;
}

static const char *BuildOutputPath(const char *src_filename, const char *output_directory,
                                   const char *suffix, Allocator *alloc)
{
    DebugAssert(!PathIsAbsolute(src_filename));

    HeapArray<char> buf;
    buf.allocator = alloc;

    Size offset = Fmt(&buf, "%1%/objects%/", output_directory).len;
    Fmt(&buf, "%1%2", src_filename, suffix);

    // Replace '..' components with '__'
    {
        char *ptr = buf.ptr + offset;

        while ((ptr = strstr(ptr, ".."))) {
            if (IsPathSeparator(ptr[-1]) && (IsPathSeparator(ptr[2]) || !ptr[2])) {
                ptr[0] = '_';
                ptr[1] = '_';
            }

            ptr += 2;
        }
    }

    return buf.Leak().ptr;
}

// TODO: Support Make escaping
static bool ParseCompilerMakeRule(const char *filename, Allocator *alloc,
                                  HeapArray<const char *> *out_filenames)
{
    HeapArray<char> rule_buf;
    if (ReadFile(filename, Megabytes(2), &rule_buf) < 0)
        return false;
    rule_buf.Append(0);

    // Skip output path
    Span<const char> rule;
    {
        const char *ptr = strstr(rule_buf.ptr, ": ");
        if (ptr) {
            rule = Span<const char>(ptr + 2);
        } else {
            rule = {};
        }
    }

    while (rule.len) {
        Span<const char> path = TrimStr(SplitStr(rule, ' ', &rule));

        if (path.len && path != "\\") {
            const char *dep_filename = NormalizePath(path, alloc).ptr;
            out_filenames->Append(dep_filename);
        }
    }

    return true;
}

static bool IsFileUpToDate(const char *dest_filename, Span<const char *const> src_filenames)
{
    int64_t dest_time = GetFileModificationTime(dest_filename);

    for (const char *src_filename: src_filenames) {
        int64_t src_time = GetFileModificationTime(src_filename);
        if (src_time < 0 || src_time > dest_time)
            return false;
    }

    return true;
}

static bool EnsureDirectoryExists(const char *filename)
{
    Span<const char> directory;
    SplitStrReverseAny(filename, PATH_SEPARATORS, &directory);

    return MakeDirectoryRec(directory);
}

static bool CreatePrecompileHeader(const char *pch_filename, const char *dest_filename)
{
    if (!EnsureDirectoryExists(dest_filename))
        return false;

    StreamWriter writer(dest_filename);
    Print(&writer, "#include \"%1%/%2\"", GetWorkingDirectory(), pch_filename);
    return writer.Close();
}

TargetData *TargetSet::CreateTarget(const Config &config, const char *target_name,
                                    const char *output_directory)
{
    BlockAllocator temp_alloc;

    const TargetConfig *target_config = config.targets_map.FindValue(target_name, nullptr);
    if (!target_config) {
        LogError("Could not find target '%1'", target_name);
        return nullptr;
    }

    // Gather target objects
    HeapArray<ObjectInfo> objects;
    {
        HeapArray<const char *> src_filenames;
        for (const char *src_directory: target_config->src_directories) {
            if (!EnumerateDirectoryFiles(src_directory, nullptr, 1024, &temp_alloc, &src_filenames))
                return nullptr;
        }
        src_filenames.Append(target_config->src_filenames);

        for (const char *src_filename: src_filenames) {
            const char *name = SplitStrReverseAny(src_filename, PATH_SEPARATORS).ptr;
            bool ignore = std::any_of(target_config->exclusions.begin(), target_config->exclusions.end(),
                                      [&](const char *excl) { return MatchPathName(name, excl); });

            if (!ignore) {
                ObjectInfo obj = {};

                Span<const char> extension = GetPathExtension(src_filename);
                if (extension == ".c") {
                    obj.src_type = SourceType::C_Source;
                } else if (extension == ".cc" || extension == ".cpp") {
                    obj.src_type = SourceType::CXX_Source;
                } else {
                    continue;
                }

                obj.src_filename = DuplicateString(src_filename, &str_alloc).ptr;
                obj.dest_filename = BuildOutputPath(src_filename, output_directory, ".o", &str_alloc);

                objects.Append(obj);
            }
        }
    }

    // Target libraries
    HeapArray<const char *> libraries;
    libraries.Append(target_config->libraries);

    // Resolve imported objects and libraries
    for (const char *import_name: target_config->imports) {
        const TargetData *import;
        {
            Size import_idx = targets_map.FindValue(import_name, -1);
            if (import_idx >= 0) {
                import = &targets[import_idx];
            } else {
                import = CreateTarget(config, import_name, output_directory);
                if (!import)
                    return nullptr;
            }
        }

        objects.Append(import->objects);
        libraries.Append(import->libraries);
    }

    // Big type, so create it directly in HeapArray
    // Introduce out_guard if things can start to fail after here
    TargetData *target = targets.AppendDefault();

    target->name = target_config->name;
    target->type = target_config->type;
    target->c_pch_filename = target_config->c_pch_filename;
    target->cxx_pch_filename = target_config->cxx_pch_filename;
    target->include_directories = target_config->include_directories;
    std::swap(target->libraries, libraries);
    std::swap(target->objects, objects);

    bool appended = targets_map.Append(target_config->name, targets.len - 1).second;
    DebugAssert(appended);

    return target;
}

static bool AppendPrecompileCommands(const TargetData &target, const char *pch_filename,
                                     SourceType src_type, const char *output_directory,
                                     const Toolchain &toolchain, BuildMode build_mode,
                                     BuildSet *out_set)
{
    const Size start_len = out_set->commands.len;
    DEFER_N(out_guard) { out_set->commands.RemoveFrom(start_len); };

    BlockAllocator temp_alloc;

    const char *dest_filename = BuildOutputPath(pch_filename, output_directory, "_pch.h",
                                                &out_set->str_alloc);
    const char *deps_filename = Fmt(&temp_alloc, "%1.d", pch_filename).ptr;

    bool build;
    if (TestFile(deps_filename, FileType::File)) {
        HeapArray<const char *> src_filenames;
        if (!ParseCompilerMakeRule(deps_filename, &temp_alloc, &src_filenames))
            return false;

        build = !IsFileUpToDate(dest_filename, src_filenames);
    } else {
        build = true;
    }
    build &= !out_set->commands_set.Find(dest_filename);

    if (build) {
        BuildCommand cmd = {};

        cmd.dest_filename = dest_filename;
        if (!CreatePrecompileHeader(pch_filename, dest_filename))
            return false;
        cmd.cmd = toolchain.BuildObjectCommand(pch_filename, src_type, build_mode, nullptr,
                                               target.include_directories, nullptr,
                                               deps_filename, &out_set->str_alloc);

        out_set->commands.Append(cmd);
    }

    // Do this at the end because it's much harder to roll back changes in out_guard
    for (Size i = start_len; i < out_set->commands.len; i++) {
        out_set->commands_set.Append(out_set->commands[i].dest_filename);
    }

    out_guard.disable();
    return true;
}

static bool AppendTargetCommands(const TargetData &target, const char *output_directory,
                                 const Toolchain &toolchain, BuildMode build_mode,
                                 BuildSet *out_obj_set, BuildSet *out_link_set)
{
    const Size start_obj_len = out_obj_set->commands.len;
    const Size start_link_len = out_link_set->commands.len;
    DEFER_N(out_guard) {
        out_obj_set->commands.RemoveFrom(start_obj_len);
        out_link_set->commands.RemoveFrom(start_link_len);
    };

    BlockAllocator temp_alloc;

    // Reuse for performance
    HeapArray<const char *> src_filenames;

    bool relink = false;
    for (const ObjectInfo &obj: target.objects) {
        const char *deps_filename = Fmt(&temp_alloc, "%1.d", obj.dest_filename).ptr;

        src_filenames.RemoveFrom(0);
        src_filenames.Append(obj.src_filename);

        // Parse Make rule dependencies
        bool build = false;
        if (TestFile(deps_filename, FileType::File)) {
            if (!ParseCompilerMakeRule(deps_filename, &temp_alloc, &src_filenames))
                return false;

            build = !IsFileUpToDate(obj.dest_filename, src_filenames);
        } else {
            build = true;
        }
        build &= !out_obj_set->commands_set.Find(obj.dest_filename);
        relink |= build;

        if (build) {
            BuildCommand cmd = {};

            const char *pch_filename = nullptr;
            switch (obj.src_type) {
                case SourceType::C_Source: { pch_filename = target.c_pch_filename; } break;
                case SourceType::CXX_Source: { pch_filename = target.cxx_pch_filename; } break;

                case SourceType::C_Header:
                case SourceType::CXX_Header: { DebugAssert(false); } break;
            }

            cmd.dest_filename = DuplicateString(obj.dest_filename, &out_obj_set->str_alloc).ptr;
            if (!EnsureDirectoryExists(obj.dest_filename))
                return false;
            cmd.cmd = toolchain.BuildObjectCommand(obj.src_filename, obj.src_type, build_mode,
                                                   pch_filename, target.include_directories,
                                                   obj.dest_filename, deps_filename, &out_obj_set->str_alloc);

            out_obj_set->commands.Append(cmd);
        }
    }

    if (target.type == TargetType::Executable) {
#ifdef _WIN32
        const char *link_filename = Fmt(&out_link_set->str_alloc, "%1%/%2.exe",
                                        output_directory, target.name).ptr;
#else
        const char *link_filename = Fmt(&out_link_set->str_alloc, "%1%/%2",
                                        output_directory, target_config.name).ptr;
#endif

        relink &= !out_link_set->commands_set.Find(link_filename);

        if (relink || !TestFile(link_filename, FileType::File)) {
            BuildCommand cmd = {};

            cmd.dest_filename = link_filename;
            cmd.cmd = toolchain.BuildLinkCommand(target.objects, target.libraries, link_filename,
                                                &out_link_set->str_alloc);

            out_link_set->commands.Append(cmd);
        }
    }

    // Do this at the end because it's much harder to roll back changes in out_guard
    for (Size i = start_obj_len; i < out_obj_set->commands.len; i++) {
        out_obj_set->commands_set.Append(out_obj_set->commands[i].dest_filename);
    }
    for (Size i = start_obj_len; i < out_link_set->commands.len; i++) {
        out_link_set->commands_set.Append(out_link_set->commands[i].dest_filename);
    }

    out_guard.disable();
    return true;
}

static bool RunBuildCommands(Span<const BuildCommand> commands, bool verbose)
{
    Async async;

    static std::atomic_int progress_counter;
    progress_counter = 0;

    for (const BuildCommand &cmd: commands) {
        async.AddTask([&, cmd]() {
            if (verbose) {
                LogInfo("[%1/%2] %3", progress_counter += 1, commands.len, cmd.cmd);
            } else {
                const char *name = SplitStrReverseAny(cmd.dest_filename, PATH_SEPARATORS).ptr;
                LogInfo("[%1/%2] Build %3", progress_counter += 1, commands.len, name);
            }

            // Run command
            errno = 0;
            if (system(cmd.cmd) || errno) {
                LogError("Command '%1' failed", cmd.cmd);
#ifdef _WIN32
                _unlink(cmd.dest_filename);
#else
                unlink(cmd.dest_filename);
#endif
                return false;
            }

            return true;
        });
    }

    return async.Sync();
}

int main(int argc, char **argv)
{
    BlockAllocator temp_alloc;

    static const auto PrintUsage = [](FILE *fp) {
        PrintLn(fp,
R"(Usage: felix [options] [target]

Options:
    -C, --config <filename>      Set configuration filename
                                 (default: FelixBuild.ini)
    -O, --output <directory>     Set output directory
                                 (default: working directory)

    -t, --toolchain <toolchain>  Set toolchain, see below
                                 (default: %1)
    -m, --mode     <mode>        Set build mode, see below
                                 (default: %2)

    -j, --jobs <count>           Set maximum number of parallel jobs
                                 (default: number of cores)

    -v, --verbose                Show detailed build commands

Available toolchains:)", Toolchains[0]->name, BuildModeNames[0]);
        for (const Toolchain *toolchain: Toolchains) {
            PrintLn(fp, "    %1", toolchain->name);
        }
        PrintLn(fp, R"(
Available build modes:)");
        for (const char *mode_name: BuildModeNames) {
            PrintLn(fp, "    %1", mode_name);
        }
    };

    HeapArray<const char *> target_names;
    const char *config_filename = "FelixBuild.ini";
    const char *output_directory = nullptr;
    const Toolchain *toolchain = Toolchains[0];
    BuildMode build_mode = (BuildMode)0;
    bool verbose = false;
    {
        OptionParser opt(argc, argv);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                PrintUsage(stdout);
                return 0;
            } else if (opt.Test("-C", "--config", OptionType::Value)) {
                config_filename = opt.current_value;
            } else if (opt.Test("-O", "--output", OptionType::Value)) {
                output_directory = opt.current_value;
            } else if (opt.Test("-t", "--toolchain", OptionType::Value)) {
                const Toolchain *const *ptr = FindIf(Toolchains,
                                                     [&](const Toolchain *toolchain) { return TestStr(toolchain->name, opt.current_value); });
                if (!ptr) {
                    LogError("Unknown toolchain '%1'", opt.current_value);
                    return 1;
                }

                toolchain = *ptr;
            } else if (opt.Test("-m", "--mode", OptionType::Value)) {
                const char *const *name = FindIf(BuildModeNames,
                                                 [&](const char *name) { return TestStr(name, opt.current_value); });
                if (!name) {
                    LogError("Unknown build mode '%1'", opt.current_value);
                    return 1;
                }

                build_mode = (BuildMode)(name - BuildModeNames);
            } else if (opt.Test("-j", "--jobs", OptionType::Value)) {
                int max_threads;
                if (!ParseDec(opt.current_value, &max_threads))
                    return 1;
                if (max_threads < 1) {
                    LogError("Jobs count cannot be < 1");
                    return 1;
                }

                Async::SetThreadCount(max_threads);
            } else if (opt.Test("-v", "--verbose")) {
                verbose = true;
            } else {
                LogError("Cannot handle option '%1'", opt.current_option);
                return 1;
            }
        }

        opt.ConsumeNonOptions(&target_names);
    }

    Config config;
    if (!LoadConfig(config_filename, &config))
        return 1;

    // Default targets
    if (!target_names.len) {
        for (const TargetConfig &target_config: config.targets) {
            if (target_config.type == TargetType::Executable) {
                target_names.Append(target_config.name);
            }
        }
    }

#ifdef _WIN32
    if (toolchain == &GnuToolchain) {
        for (TargetConfig &target_config: config.targets) {
            static bool warned = false;
            if (!warned && (target_config.c_pch_filename || target_config.cxx_pch_filename)) {
                LogError("PCH does not work correctly with MinGW (ignoring)");
                warned = true;
            }

            target_config.c_pch_filename = nullptr;
            target_config.cxx_pch_filename = nullptr;
        }
    }
#endif

    // Directories
    const char *start_directory = DuplicateString(GetWorkingDirectory(), &temp_alloc).ptr;
    if (output_directory) {
        output_directory = NormalizePath(output_directory, start_directory, &temp_alloc).ptr;
    } else {
        output_directory = start_directory;
    }

    // Change to root directory
    {
        Span<const char> root_directory;
        SplitStrReverseAny(config_filename, PATH_SEPARATORS, &root_directory);

        if (root_directory.len) {
            const char *root_directory0 = DuplicateString(root_directory, &temp_alloc).ptr;
            if (!SetWorkingDirectory(root_directory0))
                return 1;
        }
    }

    // Gather targets
    TargetSet target_set;
    {
        HashSet<const char *> handled_set;

        for (const char *target_name: target_names) {
            if (!handled_set.Append(target_name).second) {
                LogError("Target '%1' is specified multiple times", target_name);
                return 1;
            }

            if (!target_set.targets_map.Find(target_name) &&
                    !target_set.CreateTarget(config, target_name, output_directory))
                return 1;
        }
    }

    // We need to build PCH files first (for dependency issues)
    BuildSet pch_command_set;
    for (const TargetData &target: target_set.targets) {
        if (target.c_pch_filename &&
                !AppendPrecompileCommands(target, target.c_pch_filename, SourceType::C_Header,
                                          output_directory, *toolchain, build_mode, &pch_command_set))
            return 1;
        if (target.cxx_pch_filename &&
                !AppendPrecompileCommands(target, target.cxx_pch_filename, SourceType::CXX_Header,
                                          output_directory, *toolchain, build_mode, &pch_command_set))
            return 1;
    }

    if (pch_command_set.commands.len) {
        LogInfo("Precompile headers");
        if (!RunBuildCommands(pch_command_set.commands, verbose))
            return 1;
    }

    BuildSet obj_command_set;
    BuildSet link_command_set;
    for (const TargetData &target: target_set.targets) {
        if (!AppendTargetCommands(target, output_directory, *toolchain, build_mode,
                                  &obj_command_set, &link_command_set))
            return 1;
    }

    if (obj_command_set.commands.len) {
        LogInfo("Compile source files");
        if (!RunBuildCommands(obj_command_set.commands, verbose))
            return 1;
    }
    if (link_command_set.commands.len) {
        LogInfo("Link targets");
        if (!RunBuildCommands(link_command_set.commands, verbose))
            return 1;
    }

    LogInfo("Done!");
    return 0;
}
