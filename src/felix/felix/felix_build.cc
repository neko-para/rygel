// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "../../libcc/libcc.hh"
#include "build_command.hh"
#include "build_compiler.hh"
#include "build_target.hh"

static int RunTarget(const Target &target, Span<const char *const> arguments, bool verbose)
{
    if (target.type != TargetType::Executable) {
        LogError("Cannot run non-executable target '%1'", target.name);
        return 1;
    }

    HeapArray<char> cmd_buf;

    // FIXME: Just like the code in compiler.cc, command-line escaping is
    // either wrong or not done. Make something to deal with that uniformely.
    Fmt(&cmd_buf, "\"%1\"", target.dest_filename);
    for (const char *arg: arguments) {
        bool escape = strchr(arg, ' ');
        Fmt(&cmd_buf, escape ? " \"%1\"" : " %1", arg);
    }

    if (verbose) {
        LogInfo("Run '%1'", cmd_buf);
    } else {
        LogInfo("Run target '%1'", target.name);
    }
    PrintLn(stderr);

    return system(cmd_buf.ptr);
}

int RunBuild(Span<const char *> arguments)
{
    BlockAllocator temp_alloc;

    static const auto PrintUsage = [](FILE *fp) {
        PrintLn(fp,
R"(Usage: felix build [options] [target...]
       felix build [options] target --run [arguments...]

Options:
    -C, --config <filename>      Set configuration filename
                                 (default: FelixBuild.ini)
    -O, --output <directory>     Set output directory
                                 (default: bin/<compiler>_<mode>)

    -c, --compiler <compiler>    Set compiler, see below
                                 (default: %1)
    -m, --mode <mode>            Set build mode, see below
                                 (default: %2)
       --disable_pch             Disable header precompilation (PCH)

    -j, --jobs <count>           Set maximum number of parallel jobs
                                 (default: number of cores + 1)

    -v, --verbose                Show detailed build commands

        --run                    Run target after successful build
                                 (all remaining arguments are passed as-is)

Available compilers:)", Compilers[0]->name, BuildModeNames[0]);
        for (const Compiler *compiler: Compilers) {
            PrintLn(fp, "    %1", compiler->name);
        }
        PrintLn(fp, R"(
Available build modes:)");
        for (const char *mode_name: BuildModeNames) {
            PrintLn(fp, "    %1", mode_name);
        }
    };

    // NOTE: This overrules LIBCC_THREADS if it exists
    Async::SetThreadCount(Async::GetThreadCount() + 1);

    HeapArray<const char *> target_names;
    const char *config_filename = "FelixBuild.ini";
    const char *output_directory = nullptr;
    const Compiler *compiler = Compilers[0];
    BuildMode build_mode = (BuildMode)0;
    bool disable_pch = false;
    bool verbose = false;
    bool run = false;
    Span<const char *> run_arguments = {};
    {
        OptionParser opt(arguments);

        for (;;) {
            // We need to consume values (target names) as we go because
            // the --run option will break the loop and all remaining
            // arguments will be passed as-is to the target.
            opt.ConsumeNonOptions(&target_names);
            if (!opt.Next())
                break;

            if (opt.Test("--help")) {
                PrintUsage(stdout);
                return 0;
            } else if (opt.Test("-C", "--config", OptionType::Value)) {
                config_filename = opt.current_value;
            } else if (opt.Test("-O", "--output", OptionType::Value)) {
                output_directory = opt.current_value;
            } else if (opt.Test("-c", "--compiler", OptionType::Value)) {
                const Compiler *const *ptr = FindIf(Compilers,
                                                     [&](const Compiler *compiler) { return TestStr(compiler->name, opt.current_value); });
                if (!ptr) {
                    LogError("Unknown compiler '%1'", opt.current_value);
                    return 1;
                }

                compiler = *ptr;
            } else if (opt.Test("-m", "--mode", OptionType::Value)) {
                const char *const *name = FindIf(BuildModeNames,
                                                 [&](const char *name) { return TestStr(name, opt.current_value); });
                if (!name) {
                    LogError("Unknown build mode '%1'", opt.current_value);
                    return 1;
                }

                build_mode = (BuildMode)(name - BuildModeNames);
            } else if (opt.Test("--disable_pch")) {
                disable_pch = true;
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
            } else if (opt.Test("--run")) {
                run = true;
                break;
            } else {
                LogError("Cannot handle option '%1'", opt.current_option);
                return 1;
            }
        }

        if (run) {
            if (target_names.len != 1) {
                LogError("Exactly one target name must be specified with --run");
                return 1;
            }

            run_arguments = opt.GetRemainingArguments();
        }
    }

    // Change to root directory
    const char *start_directory = DuplicateString(GetWorkingDirectory(), &temp_alloc).ptr;
    {
        Span<const char> root_directory;
        config_filename = SplitStrReverseAny(config_filename, PATH_SEPARATORS, &root_directory).ptr;

        if (root_directory.len) {
            const char *root_directory0 = DuplicateString(root_directory, &temp_alloc).ptr;
            if (!SetWorkingDirectory(root_directory0))
                return 1;
        }
    }
    if (output_directory) {
        output_directory = NormalizePath(output_directory, start_directory, &temp_alloc).ptr;
    } else {
        output_directory = Fmt(&temp_alloc, "%1%/bin%/%2_%3", start_directory,
                               compiler->name, BuildModeNames[(int)build_mode]).ptr;
    }

    // Load configuration file
    TargetSet target_set;
    if (!LoadTargetSet(config_filename, output_directory, &target_set))
        return 1;

    // Default targets
    if (!target_names.len) {
        for (const Target &target: target_set.targets) {
            if (target.type == TargetType::Executable) {
                target_names.Append(target.name);
            }
        }
    }
    if (!target_names.len) {
        LogError("There are no targets");
        return 1;
    }

    // Select targets and their dependencies (imports)
    HeapArray<const Target *> enabled_targets;
    const Target *first_target = nullptr;
    {
        HashSet<const char *> handled_set;

        bool valid = true;
        for (const char *target_name: target_names) {
            if (handled_set.Append(target_name).second) {
                const Target *target = target_set.targets_map.FindValue(target_name, nullptr);
                if (!target) {
                    LogError("Target '%1' does not exist", target_name);
                    valid = false;
                    continue;
                }

                for (const char *import_name: target->imports) {
                    if (handled_set.Append(import_name).second) {
                        const Target *import = target_set.targets_map.FindValue(import_name, nullptr);
                        enabled_targets.Append(import);
                    }
                }

                enabled_targets.Append(target);
                if (!first_target) {
                    first_target = target;
                }
            }
        }
        if (!valid)
            return 1;
    }

    // We're ready to output stuff
    if (!MakeDirectoryRec(output_directory))
        return 1;
    LogInfo("Output directory: '%1'", output_directory);

    // Disable PCH?
    if (!disable_pch && !compiler->Supports(CompilerFlag::PCH)) {
        bool using_pch = std::any_of(enabled_targets.begin(), enabled_targets.end(),
                                   [](const Target *target) {
            return target->c_pch_filename || target->cxx_pch_filename;
        });

        if (using_pch) {
            LogError("PCH does not work correctly with %1 compiler (ignoring)", compiler->name);
            disable_pch = true;
        }
    }
    if (disable_pch) {
        for (Target &target: target_set.targets) {
            target.pch_objects.Clear();
            target.c_pch_filename = nullptr;
            target.cxx_pch_filename = nullptr;
        }
    }

    // LTO?
    if (build_mode == BuildMode::LTO && !compiler->Supports(CompilerFlag::LTO)) {
        LogError("LTO does not work correctly with %1 compiler", compiler->name);
        return 1;
    }

    // Create build commands
    BuildSet build_set;
    {
        BuildSetBuilder build_set_builder(compiler, build_mode);

        for (const Target *target: enabled_targets) {
            if (!build_set_builder.AppendTargetCommands(*target))
                return 1;
        }

        build_set_builder.Finish(&build_set);
    }

    // Build
    if (!RunBuildCommands(build_set.commands, verbose))
        return 1;

    // Run?
    if (run) {
        return RunTarget(*first_target, run_arguments, verbose);
    } else {
        return 0;
    }
}
