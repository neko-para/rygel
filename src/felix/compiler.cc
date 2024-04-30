// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#include "src/core/base/base.hh"
#include "compiler.hh"
#include "locate.hh"

namespace RG {

static bool SplitPrefixSuffix(const char *binary, const char *needle,
                              Span<const char> *out_prefix, Span<const char> *out_suffix,
                              Span<const char> *out_version)
{
    const char *ptr = strstr(binary, needle);
    if (!ptr) {
        LogError("Compiler binary path must contain '%1'", needle);
        return false;
    }

    *out_prefix = MakeSpan(binary, ptr - binary);
    *out_suffix = ptr + strlen(needle);

    if (out_suffix->ptr[0] == '-' && std::all_of(out_suffix->ptr + 1, out_suffix->end(), IsAsciiDigit)) {
        *out_version = *out_suffix;
    } else {
        *out_version = {};
    }

    return true;
}

static void AddEnvironmentFlags(Span<const char *const> names, HeapArray<char> *out_buf)
{
    for (const char *name: names) {
        const char *flags = GetEnv(name);

        if (flags && flags[0]) {
            Fmt(out_buf, " %1", flags);
        }
    }
}

static void MakePackCommand(Span<const char *const> pack_filenames, bool use_arrays,
                            const char *pack_options, const char *dest_filename,
                            Allocator *alloc, Command *out_cmd)
{
    RG_ASSERT(alloc);

    HeapArray<char> buf(alloc);

    Fmt(&buf, "\"%1\" pack -O \"%2\"", GetApplicationExecutable(), dest_filename);

    Fmt(&buf, use_arrays ? "" : " -fUseLiterals");
    if (pack_options) {
        Fmt(&buf, " %1", pack_options);
    }

    for (const char *pack_filename: pack_filenames) {
        Fmt(&buf, " \"%1\"", pack_filename);
    }

    out_cmd->cache_len = buf.len;
    out_cmd->cmd_line = buf.TrimAndLeak(1);
}

static int ParseVersion(const char *cmd, Span<const char> output, const char *marker)
{
    Span<const char> remain = output;

    while (remain.len) {
        Span<const char> token = SplitStr(remain, ' ', &remain);

        if (token == marker) {
            int major = 0;
            int minor = 0;

            if (!ParseInt(remain, &major, 0, &remain)) {
                LogError("Unexpected version format returned by '%1'", cmd);
                return -1;
            }
            if(remain[0] == '.') {
                remain = remain.Take(1, remain.len - 1);

                if (!ParseInt(remain, &minor, 0, &remain)) {
                    LogError("Unexpected version format returned by '%1'", cmd);
                    return -1;
                }
            }

            int version = major * 100 + minor;
            return version;
        }
    }

    return -1;
}

static bool IdentifyCompiler(const char *bin, const char *needle)
{
    bin = SplitStrReverseAny(bin, RG_PATH_SEPARATORS).ptr;

    const char *ptr = strstr(bin, needle);
    Size len = (Size)strlen(needle);

    if (!ptr)
        return false;
    if (ptr != bin && !strchr("_-.", ptr[-1]))
        return false;
    if (ptr[len] && !strchr("_-.", ptr[len]))
        return false;

    return true;
}

static bool DetectCcache() {
    static bool detected = FindExecutableInPath("ccache");
    return detected;
}

static bool DetectDistCC() {
    static bool detected = FindExecutableInPath("distcc") &&
                           (GetEnv("DISTCC_HOSTS") || GetEnv("DISTCC_POTENTIAL_HOSTS"));
    return detected;
}

class ClangCompiler final: public Compiler {
    const char *cc;
    const char *cxx;
    const char *rc;
    const char *ld;

    int clang_ver = 0;
    int lld_ver = 0;

    BlockAllocator str_alloc;

public:
    ClangCompiler(HostPlatform platform, HostArchitecture architecture) : Compiler(platform, architecture, "Clang") {}

    static std::unique_ptr<const Compiler> Create(HostPlatform platform, HostArchitecture architecture, const char *cc, const char *ld)
    {
        std::unique_ptr<ClangCompiler> compiler = std::make_unique<ClangCompiler>(platform, architecture);

        // Prefer LLD
        if (!ld && FindExecutableInPath("ld.lld")) {
            ld = "lld";
        }

        // Find executables
        {
            Span<const char> prefix;
            Span<const char> suffix;
            Span<const char> version;
            if (!SplitPrefixSuffix(cc, "clang", &prefix, &suffix, &version))
                return nullptr;

            compiler->cc = DuplicateString(cc, &compiler->str_alloc).ptr;
            compiler->cxx = Fmt(&compiler->str_alloc, "%1clang++%2", prefix, suffix).ptr;
            compiler->rc = Fmt(&compiler->str_alloc, "%1llvm-rc%2", prefix, version).ptr;
            if (ld) {
                compiler->ld = ld;
            } else if (suffix.len) {
                compiler->ld = Fmt(&compiler->str_alloc, "%1lld%2", prefix, suffix).ptr;
            } else {
                compiler->ld = nullptr;
            }
        }

        Async async;

        // Determine Clang version
        async.Run([&]() {
            char cmd[2048];
            Fmt(cmd, "\"%1\" --version", compiler->cc);

            HeapArray<char> output;
            if (ReadCommandOutput(cmd, &output)) {
                compiler->clang_ver = ParseVersion(cmd, output, "version");
            }

            return true;
        });

        // Determine LLD version
        async.Run([&]() {
            if (compiler->ld && IdentifyCompiler(compiler->ld, "lld")) {
                char cmd[4096];
                if (PathIsAbsolute(compiler->ld)) {
                    Fmt(cmd, "\"%1\" --version", compiler->ld);
                } else {
#ifdef _WIN32
                    Fmt(cmd, "\"%1-link\" --version", compiler->ld);
#else
                    Fmt(cmd, "\"ld.%1\" --version", compiler->ld);
#endif
                }

                HeapArray<char> output;
                if (ReadCommandOutput(cmd, &output)) {
                    compiler->lld_ver = ParseVersion(cmd, output, "LLD");
                }
            }

            return true;
        });

        async.Sync();

        return compiler;
    }

    bool CheckFeatures(uint32_t features, uint32_t maybe_features, uint32_t *out_features) const override
    {
        uint32_t supported = 0;

        supported |= (int)CompileFeature::OptimizeSpeed;
        supported |= (int)CompileFeature::OptimizeSize;
        if (DetectCcache()) {
            supported |= (int)CompileFeature::Ccache;
        }
        if (DetectDistCC()) {
            supported |= (int)CompileFeature::DistCC;
        }
        supported |= (int)CompileFeature::HotAssets;
        supported |= (int)CompileFeature::PCH;
        supported |= (int)CompileFeature::Warnings;
        supported |= (int)CompileFeature::DebugInfo;
        supported |= (int)CompileFeature::ASan;
        supported |= (int)CompileFeature::UBSan;
        supported |= (int)CompileFeature::LTO;
        supported |= (int)CompileFeature::ZeroInit;
        if (platform != HostPlatform::OpenBSD) {
            supported |= (int)CompileFeature::CFI; // LTO only
        }
        if (platform != HostPlatform::Windows) {
            supported |= (int)CompileFeature::TSan;
            supported |= (int)CompileFeature::ShuffleCode; // Requires lld version >= 11
        }
        if (platform == HostPlatform::Linux) {
            if (architecture == HostArchitecture::x86_64) {
                supported |= (int)CompileFeature::SafeStack;
            } else if (architecture == HostArchitecture::ARM64) {
                supported |= (int)CompileFeature::SafeStack;
            }
        }
        supported |= (int)CompileFeature::StaticRuntime;
        supported |= (int)CompileFeature::LinkLibrary;
        if (platform == HostPlatform::Windows) {
            supported |= (int)CompileFeature::NoConsole;
        }
        supported |= (int)CompileFeature::AESNI;
        supported |= (int)CompileFeature::AVX2;
        supported |= (int)CompileFeature::AVX512;

        uint32_t unsupported = features & ~supported;
        if (unsupported) {
            LogError("Some features are not supported by %1: %2",
                     name, FmtFlags(unsupported, CompileFeatureOptions));
            return false;
        }

        features |= (supported & maybe_features);

        if ((features & (int)CompileFeature::OptimizeSpeed) && (features & (int)CompileFeature::OptimizeSize)) {
            LogError("Cannot use OptimizeSpeed and OptimizeSize at the same time");
            return false;
        }
        if ((features & (int)CompileFeature::ASan) && (features & (int)CompileFeature::TSan)) {
            LogError("Cannot use ASan and TSan at the same time");
            return false;
        }
        if (!(features & (int)CompileFeature::LTO) && (features & (int)CompileFeature::CFI)) {
            LogError("Clang CFI feature requires LTO compilation");
            return false;
        }
        if (lld_ver < 1100 && (features & (int)CompileFeature::ShuffleCode)) {
            LogError("ShuffleCode requires LLD >= 11, try --host option (e.g. --host=:clang-11:lld-11)");
            return false;
        }

        *out_features = features;
        return true;
    }

    const char *GetObjectExtension() const override
    {
        const char *ext = (platform == HostPlatform::Windows) ? ".obj" : ".o";
        return ext;
    }
    const char *GetLinkExtension(TargetType type) const override
    {
        switch (type) {
            case TargetType::Executable: {
                const char *ext = (platform == HostPlatform::Windows) ? ".exe" : "";
                return ext;
            } break;
            case TargetType::Library: {
                const char *ext = (platform == HostPlatform::Windows) ? ".dll" : ".so";
                return ext;
            } break;
        }

        RG_UNREACHABLE();
    }
    const char *GetPostExtension(TargetType) const override { return nullptr; }

    void MakePackCommand(Span<const char *const> pack_filenames,
                         const char *pack_options, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        RG::MakePackCommand(pack_filenames, false, pack_options, dest_filename, alloc, out_cmd);
    }

    void MakePchCommand(const char *pch_filename, SourceType src_type,
                        Span<const char *const> definitions, Span<const char *const> include_directories,
                        Span<const char *const> include_files, uint32_t features, bool env_flags,
                        Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        MakeObjectCommand(pch_filename, src_type, nullptr, definitions, include_directories, {},
                          include_files, features, env_flags, nullptr, alloc, out_cmd);
    }

    const char *GetPchCache(const char *pch_filename, Allocator *alloc) const override
    {
        RG_ASSERT(alloc);

        const char *cache_filename = Fmt(alloc, "%1.gch", pch_filename).ptr;
        return cache_filename;
    }
    const char *GetPchObject(const char *, Allocator *) const override { return nullptr; }

    void MakeObjectCommand(const char *src_filename, SourceType src_type,
                           const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, Span<const char *const> system_directories,
                           Span<const char *const> include_files, uint32_t features, bool env_flags,
                           const char *dest_filename, Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        if (features & (int)CompileFeature::Ccache) {
            Fmt(&buf, "ccache ");

            if (dest_filename && (features & (int)CompileFeature::DistCC)) {
                static const ExecuteInfo::KeyValue variables[] = {
                    { "CCACHE_DEPEND", "1" },
                    { "CCACHE_SLOPPINESS", "pch_defines,time_macros,include_file_ctime,include_file_mtime" },
                    { "CCACHE_PREFIX", "distcc" }
                };

                out_cmd->env_variables = variables;
            } else {
                static const ExecuteInfo::KeyValue variables[] = {
                    { "CCACHE_DEPEND", "1" },
                    { "CCACHE_SLOPPINESS", "pch_defines,time_macros,include_file_ctime,include_file_mtime" }
                };

                out_cmd->env_variables = variables;
            }
        } else if (dest_filename && (features & (int)CompileFeature::DistCC)) {
            Fmt(&buf, "distcc ");
        }

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "\"%1\" -std=gnu11", cc); } break;
            case SourceType::Cxx: { Fmt(&buf, "\"%1\" -std=gnu++2a", cxx); } break;
            case SourceType::Esbuild:
            case SourceType::QtUi:
            case SourceType::QtResources: { RG_UNREACHABLE(); } break;
        }
        if (dest_filename) {
            Fmt(&buf, " -o \"%1\"", dest_filename);
        } else {
            switch (src_type) {
                case SourceType::C: { Fmt(&buf, " -x c-header -Xclang -fno-pch-timestamp"); } break;
                case SourceType::Cxx: { Fmt(&buf, " -x c++-header -Xclang -fno-pch-timestamp"); } break;
                case SourceType::Esbuild:
                case SourceType::QtUi:
                case SourceType::QtResources: { RG_UNREACHABLE(); } break;
            }
        }
        Fmt(&buf, " -MD -MF \"%1.d\"", dest_filename ? dest_filename : src_filename);
        out_cmd->rsp_offset = buf.len;

        // Cross-compilation (Linux only for now)
        AddClangTarget(&buf);

        // Build options
        Fmt(&buf, " -I. -fvisibility=hidden -fno-strict-aliasing -fno-delete-null-pointer-checks -fno-omit-frame-pointer");
        if (clang_ver >= 1300) {
            Fmt(&buf, " -fno-finite-loops");
        }
        if (features & (int)CompileFeature::OptimizeSpeed) {
            Fmt(&buf, " -O2 -fwrapv -DNDEBUG");
        } else if (features & (int)CompileFeature::OptimizeSize) {
            Fmt(&buf, " -Os -fwrapv -DNDEBUG -ffunction-sections -fdata-sections");
        } else {
            Fmt(&buf, " -O0 -ftrapv");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto");
        }
        if (features & (int)CompileFeature::Warnings) {
            Fmt(&buf, " -Wall -Wextra -Wuninitialized -Wno-unknown-warning-option");
            if (src_type == SourceType::Cxx) {
                Fmt(&buf, " -Wzero-as-null-pointer-constant");
            }
            Fmt(&buf, " -Wreturn-type -Werror=return-type");
        } else {
            Fmt(&buf, " -Wno-everything");
        }
        if (features & (int)CompileFeature::HotAssets) {
            Fmt(&buf, " -DFELIX_HOT_ASSETS");
        }

        // Include build directory (for generated files)
        Span<const char> dest_directory = GetPathDirectory(dest_filename);
        Fmt(&buf, " \"-I%1\"", dest_directory);

        if (architecture == HostArchitecture::x86_64) {
            Fmt(&buf, " -mpopcnt -msse4.1 -msse4.2 -mssse3 -mcx16");

            if (features & (int)CompileFeature::AESNI) {
                Fmt(&buf, " -maes -mpclmul");
            }
            if (features & (int)CompileFeature::AVX2) {
                Fmt(&buf, " -mavx2");
            }
            if (features & (int)CompileFeature::AVX512) {
                Fmt(&buf, " -mavx512f -mavx512vl");
            }
        } else if (architecture == HostArchitecture::x86) {
            Fmt(&buf, " -msse2");
        }

        // Platform flags
        switch (platform) {
            case HostPlatform::Windows: {
                Fmt(&buf, " -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE"
                          " -D_MT -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_VC_NODEFAULTLIB"
                          " -Wno-unknown-pragmas -Wno-deprecated-declarations");
            } break;

            case HostPlatform::macOS: {
                Fmt(&buf, " -pthread -fPIC");
            } break;

            case HostPlatform::Linux: {
                Fmt(&buf, " -pthread -fPIC -D_FILE_OFFSET_BITS=64 -D_GLIBCXX_ASSERTIONS");

                if (clang_ver >= 1100) {
                    Fmt(&buf, " -fno-semantic-interposition");
                }

                if (features & ((int)CompileFeature::OptimizeSpeed | (int)CompileFeature::OptimizeSize)) {
                    Fmt(&buf, " -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=%1", clang_ver >= 1701 ? 3 : 2);
                } else {
                    Fmt(&buf, " -D_GLIBCXX_DEBUG -D_GLIBCXX_SANITIZE_VECTOR");
                }
            } break;

            default: {
                Fmt(&buf, " -pthread -fPIC -D_FILE_OFFSET_BITS=64");

                if (clang_ver >= 1100) {
                    Fmt(&buf, " -fno-semantic-interposition");
                }

                if (features & ((int)CompileFeature::OptimizeSpeed | (int)CompileFeature::OptimizeSize)) {
                    Fmt(&buf, " -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2");
                }
            } break;
        }

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " -g");
        }
        if (platform == HostPlatform::Windows) {
            if (features & (int)CompileFeature::StaticRuntime) {
                if (src_type == SourceType::Cxx) {
                    Fmt(&buf, " -Xclang -flto-visibility-public-std -D_SILENCE_CLANG_CONCEPTS_MESSAGE");
                }
            } else {
                Fmt(&buf, " -D_DLL");
            }
        }
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " -fsanitize=address");
        }
        if (features & (int)CompileFeature::TSan) {
            Fmt(&buf, " -fsanitize=thread");
        }
        if (features & (int)CompileFeature::UBSan) {
            Fmt(&buf, " -fsanitize=undefined");
        }
        Fmt(&buf, " -fstack-protector-strong --param ssp-buffer-size=4");
        if (platform == HostPlatform::Linux && clang_ver >= 1100) {
            Fmt(&buf, " -fstack-clash-protection");
        }
        if (features & (int)CompileFeature::SafeStack) {
            if (architecture == HostArchitecture::x86_64) {
                Fmt(&buf, " -fsanitize=safe-stack");
            } else if (architecture == HostArchitecture::ARM64) {
                Fmt(&buf, " -fsanitize=shadow-call-stack -ffixed-x18");
            }
        }
        if (features & (int)CompileFeature::ZeroInit) {
            Fmt(&buf, " -ftrivial-auto-var-init=zero");

            if (clang_ver < 1600) {
                Fmt(&buf, " -enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang");
            }
        }
        if (features & (int)CompileFeature::CFI) {
            RG_ASSERT(features & (int)CompileFeature::LTO);

            Fmt(&buf, " -fsanitize=cfi");
            if (src_type == SourceType::C) {
                // There's too much pointer type fuckery going on in C
                // to not take this precaution. Without it, SQLite3 crashes.
                Fmt(&buf, " -fsanitize-cfi-icall-generalize-pointers");
            }
        }
        if (features & (int)CompileFeature::ShuffleCode) {
            Fmt(&buf, " -ffunction-sections -fdata-sections");
        }

        // Sources and definitions
        Fmt(&buf, " -DFELIX -c \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " -include \"%1\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " \"-%1%2\"", definition[0] != '-' ? 'D' : 'U', definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " \"-I%1\"", include_directory);
        }
        for (const char *system_directory: system_directories) {
            Fmt(&buf, " -isystem \"%1\"", system_directory);
        }
        for (const char *include_file: include_files) {
            Fmt(&buf, " -include \"%1\"", include_file);
        }

        if (env_flags) {
            switch (src_type) {
                case SourceType::C: { AddEnvironmentFlags({"CPPFLAGS", "CFLAGS"}, &buf); } break;
                case SourceType::Cxx: { AddEnvironmentFlags({"CPPFLAGS", "CXXFLAGS"}, &buf); } break;
                case SourceType::Esbuild:
                case SourceType::QtUi:
                case SourceType::QtResources: { RG_UNREACHABLE(); } break;
            }
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(STDOUT_FILENO)) {
            Fmt(&buf, " -fcolor-diagnostics -fansi-escape-codes");
        } else {
            Fmt(&buf, " -fno-color-diagnostics");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);

        // Dependencies
        out_cmd->deps_mode = Command::DependencyMode::MakeLike;
        out_cmd->deps_filename = Fmt(alloc, "%1.d", dest_filename ? dest_filename : src_filename).ptr;
    }

    void MakeResourceCommand(const char *rc_filename, const char *dest_filename,
                             Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        out_cmd->cmd_line = Fmt(alloc, "\"%1\" /FO\"%2\" \"%3\"", rc, dest_filename, rc_filename);
        out_cmd->cache_len = out_cmd->cmd_line.len;
    }

    void MakeLinkCommand(Span<const char *const> obj_filenames,
                         Span<const char *const> libraries, TargetType link_type,
                         uint32_t features, bool env_flags, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Linker
        switch (link_type) {
            case TargetType::Executable: {
                bool link_static = (features & (int)CompileFeature::StaticRuntime);
                Fmt(&buf, "\"%1\"%2", cxx, link_static ? " -static" : "");
            } break;
            case TargetType::Library: { Fmt(&buf, "\"%1\" -shared", cxx); } break;
        }
        Fmt(&buf, " -o \"%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Cross-compilation (Linux only for now)
        AddClangTarget(&buf);

        // Build mode
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto");

            if (platform != HostPlatform::Windows) {
                Fmt(&buf, " -Wl,-O1");
            }
        }

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        if (libraries.len) {
            HashSet<Span<const char>> framework_paths;

            if (platform != HostPlatform::Windows && platform != HostPlatform::macOS) {
                Fmt(&buf, " -Wl,--start-group");
            }
            for (const char *lib: libraries) {
                if (platform == HostPlatform::macOS && lib[0] == '!') {
                    Span<const char> directory = {};
                    Span<const char> basename = SplitStrReverse(lib + 1, '/', &directory);

                    if (EndsWith(basename, ".framework")) {
                        basename = basename.Take(0, basename.len - 10);
                    }

                    if (directory.len) {
                        bool inserted = false;
                        framework_paths.TrySet(directory, &inserted);

                        if (inserted) {
                            Fmt(&buf, " -F \"%1\"", directory);
                        }
                    }
                    Fmt(&buf, " -framework %1", basename);
                } else if (strpbrk(lib, RG_PATH_SEPARATORS)) {
                    Fmt(&buf, " %1", lib);
                } else {
                    Fmt(&buf, " -l%1", lib);
                }
            }
            if (platform != HostPlatform::Windows && platform != HostPlatform::macOS) {
                Fmt(&buf, " -Wl,--end-group");
            }
        }

        // Platform flags
        switch (platform) {
            case HostPlatform::Windows: {
                const char *suffix = (features & ((int)CompileFeature::OptimizeSpeed | (int)CompileFeature::OptimizeSize)) ? "" : "d";

                Fmt(&buf, " -Wl,/NODEFAULTLIB:libcmt -Wl,/NODEFAULTLIB:msvcrt -Wl,setargv.obj -Wl,oldnames.lib");
                Fmt(&buf, " -Wl,/OPT:ref");

                if (features & (int)CompileFeature::StaticRuntime) {
                    Fmt(&buf, " -Wl,libcmt%1.lib", suffix);
                } else {
                    Fmt(&buf, " -Wl,msvcrt%1.lib", suffix);
                }

                if (features & (int)CompileFeature::DebugInfo) {
                    Fmt(&buf, " -g");
                }
            } break;

            case HostPlatform::macOS: {
                Fmt(&buf, " -ldl -pthread -framework CoreFoundation -framework SystemConfiguration ");
                Fmt(&buf, " -Wl,-dead_strip -rpath \"@executable_path/../Frameworks\"");
            } break;

            default: {
                Fmt(&buf, " -pthread -Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code,-z,stack-size=1048576");

                if (lld_ver) {
                    if (lld_ver >= 1300) {
                        // The second flag is needed to fix undefined __start_/__stop_ symbols related to --gc-sections
                        Fmt(&buf, "  -Wl,--gc-sections -z nostart-stop-gc");
                    }
                } else {
                    Fmt(&buf, " -Wl,--gc-sections");
                }

                if (platform == HostPlatform::Linux) {
                    Fmt(&buf, "  -static-libgcc -static-libstdc++ -ldl -lrt");
                }
                if (link_type == TargetType::Executable) {
                    Fmt(&buf, " -pie");
                }
                if (architecture == HostArchitecture::ARM32) {
                    Fmt(&buf, " -latomic");
                }
            } break;
        }

        // Features
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " -fsanitize=address");
            if (platform == HostPlatform::Windows && !(features & (int)CompileFeature::StaticRuntime)) {
                Fmt(&buf, " -shared-libasan");
            }
        }
        if (features & (int)CompileFeature::TSan) {
            Fmt(&buf, " -fsanitize=thread");
        }
        if (features & (int)CompileFeature::UBSan) {
            Fmt(&buf, " -fsanitize=undefined");
        }
        if (features & (int)CompileFeature::SafeStack) {
            if (architecture == HostArchitecture::x86_64) {
                Fmt(&buf, " -fsanitize=safe-stack");
            } else if (architecture == HostArchitecture::ARM64) {
                Fmt(&buf, " -fsanitize=shadow-call-stack -ffixed-x18");
            }
        }
        if (features & (int)CompileFeature::CFI) {
            RG_ASSERT(features & (int)CompileFeature::LTO);
            Fmt(&buf, " -fsanitize=cfi");
        }
        if (features & (int)CompileFeature::ShuffleCode) {
            if (lld_ver >= 1300) {
                Fmt(&buf, " -Wl,--shuffle-sections=*=0");
            } else {
                Fmt(&buf, " -Wl,--shuffle-sections=0");
            }
        }
        if (features & (int)CompileFeature::NoConsole) {
            Fmt(&buf, " -Wl,/subsystem:windows, -Wl,/entry:mainCRTStartup");
        }

        if (ld) {
            Fmt(&buf, " -fuse-ld=%1", ld);
        }
        if (env_flags) {
            AddEnvironmentFlags("LDFLAGS", &buf);
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(STDOUT_FILENO)) {
            Fmt(&buf, " -fcolor-diagnostics -fansi-escape-codes");
        } else {
            Fmt(&buf, " -fno-color-diagnostics");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);
    }

    void MakePostCommand(const char *, const char *, Allocator *, Command *) const override { RG_UNREACHABLE(); }

private:
    void AddClangTarget([[maybe_unused]] HeapArray<char> *out_buf) const
    {
        // Only for Linux (for now)

#ifdef __linux__
        if (architecture != NativeArchitecture) {
            RG_ASSERT(platform == HostPlatform::Linux);

            switch (architecture)  {
                case HostArchitecture::x86: { Fmt(out_buf, " --target=x86-pc-linux-gnu"); } break;
                case HostArchitecture::x86_64: { Fmt(out_buf, " --target=x86_64-pc-linux-gnu"); } break;
                case HostArchitecture::ARM64: { Fmt(out_buf, " --target=aarch64-pc-linux-gnu"); } break;
                case HostArchitecture::RISCV64: { Fmt(out_buf, " --target=riscv64-pc-linux-gnu"); } break;

                case HostArchitecture::ARM32:
                case HostArchitecture::AVR:
                case HostArchitecture::Web: { RG_UNREACHABLE(); } break;
            }
        }
#else
        RG_ASSERT(architecture == NativeArchitecture);
#endif
    }
};

class GnuCompiler final: public Compiler {
    const char *cc;
    const char *cxx;
    const char *windres;
    const char *ld;

    int gcc_ver = 0;
    bool i686 = false;

    BlockAllocator str_alloc;

public:
    GnuCompiler(HostPlatform platform, HostArchitecture architecture) : Compiler(platform, architecture, "GCC") {}

    static std::unique_ptr<const Compiler> Create(HostPlatform platform, HostArchitecture architecture, const char *cc, const char *ld)
    {
        std::unique_ptr<GnuCompiler> compiler = std::make_unique<GnuCompiler>(platform, architecture);

        // Find executables
        {
            Span<const char> prefix;
            Span<const char> suffix;
            Span<const char> version;
            if (!SplitPrefixSuffix(cc, "gcc", &prefix, &suffix, &version))
                return nullptr;

            compiler->cc = DuplicateString(cc, &compiler->str_alloc).ptr;
            compiler->cxx = Fmt(&compiler->str_alloc, "%1g++%2", prefix, suffix).ptr;
            compiler->windres = Fmt(&compiler->str_alloc, "%1windres%2", prefix, version).ptr;
            compiler->ld = ld ? DuplicateString(ld, &compiler->str_alloc).ptr : nullptr;
        }

        // Determine GCC version
        {
            char cmd[2048];
            Fmt(cmd, "\"%1\" -v", compiler->cc);

            HeapArray<char> output;
            if (ReadCommandOutput(cmd, &output)) {
                compiler->gcc_ver = ParseVersion(cmd, output, "version");
                compiler->i686 = FindStr(output, "i686") >= 0;
            }
        };

        return compiler;
    }

    bool CheckFeatures(uint32_t features, uint32_t maybe_features, uint32_t *out_features) const override
    {
        uint32_t supported = 0;

        supported |= (int)CompileFeature::OptimizeSpeed;
        supported |= (int)CompileFeature::OptimizeSize;
        if (DetectCcache()) {
            supported |= (int)CompileFeature::Ccache;
        }
        if (DetectDistCC()) {
            supported |= (int)CompileFeature::DistCC;
        }
        supported |= (int)CompileFeature::HotAssets;
        supported |= (int)CompileFeature::Warnings;
        supported |= (int)CompileFeature::DebugInfo;
        if (platform != HostPlatform::Windows) {
            // Sometimes it works, somestimes not and the object files are
            // corrupt... just avoid PCH on MinGW
            supported |= (int)CompileFeature::PCH;
            supported |= (int)CompileFeature::ASan;
            supported |= (int)CompileFeature::TSan;
            supported |= (int)CompileFeature::UBSan;
            supported |= (int)CompileFeature::LTO;
        }
        supported |= (int)CompileFeature::ZeroInit;
        if (platform == HostPlatform::Linux && architecture == HostArchitecture::x86_64) {
            supported |= (int)CompileFeature::CFI;
        }
        supported |= (int)CompileFeature::StaticRuntime;
        supported |= (int)CompileFeature::LinkLibrary;
        if (platform == HostPlatform::Windows) {
            supported |= (int)CompileFeature::NoConsole;
        }
        supported |= (int)CompileFeature::AESNI;
        supported |= (int)CompileFeature::AVX2;
        supported |= (int)CompileFeature::AVX512;

        uint32_t unsupported = features & ~supported;
        if (unsupported) {
            LogError("Some features are not supported by %1: %2",
                     name, FmtFlags(unsupported, CompileFeatureOptions));
            return false;
        }

        features |= (supported & maybe_features);

        if ((features & (int)CompileFeature::OptimizeSpeed) && (features & (int)CompileFeature::OptimizeSize)) {
            LogError("Cannot use OptimizeSpeed and OptimizeSize at the same time");
            return false;
        }
        if ((features & (int)CompileFeature::ASan) && (features & (int)CompileFeature::TSan)) {
            LogError("Cannot use ASan and TSan at the same time");
            return false;
        }
        if (gcc_ver < 1201 && (features & (int)CompileFeature::ZeroInit)) {
            LogError("ZeroInit requires GCC >= 12.1, try --host option (e.g. --host=:gcc-12)");
            return false;
        }

        *out_features = features;
        return true;
    }

    const char *GetObjectExtension() const override { return ".o"; }
    const char *GetLinkExtension(TargetType type) const override
    {
        switch (type) {
            case TargetType::Executable: {
                const char *ext = (platform == HostPlatform::Windows) ? ".exe" : "";
                return ext;
            } break;
            case TargetType::Library: {
                const char *ext = (platform == HostPlatform::Windows) ? ".dll" : ".so";
                return ext;
            } break;
        }

        RG_UNREACHABLE();
    }
    const char *GetPostExtension(TargetType) const override { return nullptr; }

    void MakePackCommand(Span<const char *const> pack_filenames,
                         const char *pack_options, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        RG::MakePackCommand(pack_filenames, false, pack_options, dest_filename, alloc, out_cmd);
    }

    void MakePchCommand(const char *pch_filename, SourceType src_type,
                        Span<const char *const> definitions, Span<const char *const> include_directories,
                        Span<const char *const> include_files, uint32_t features, bool env_flags,
                        Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        MakeObjectCommand(pch_filename, src_type, nullptr, definitions, include_directories, {},
                          include_files, features, env_flags, nullptr, alloc, out_cmd);
    }

    const char *GetPchCache(const char *pch_filename, Allocator *alloc) const override
    {
        RG_ASSERT(alloc);

        const char *cache_filename = Fmt(alloc, "%1.gch", pch_filename).ptr;
        return cache_filename;
    }
    const char *GetPchObject(const char *, Allocator *) const override { return nullptr; }

    void MakeObjectCommand(const char *src_filename, SourceType src_type,
                           const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, Span<const char *const> system_directories,
                           Span<const char *const> include_files, uint32_t features, bool env_flags,
                           const char *dest_filename, Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        if (features & (int)CompileFeature::Ccache) {
            Fmt(&buf, "ccache ");

            if (dest_filename && (features & (int)CompileFeature::DistCC)) {
                static const ExecuteInfo::KeyValue variables[] = {
                    { "CCACHE_DEPEND", "1" },
                    { "CCACHE_SLOPPINESS", "pch_defines,time_macros,include_file_ctime,include_file_mtime" },
                    { "CCACHE_PREFIX", "distcc" }
                };

                out_cmd->env_variables = variables;
            } else {
                static const ExecuteInfo::KeyValue variables[] = {
                    { "CCACHE_DEPEND", "1" },
                    { "CCACHE_SLOPPINESS", "pch_defines,time_macros,include_file_ctime,include_file_mtime" }
                };

                out_cmd->env_variables = variables;
            }
        } else if (dest_filename && (features & (int)CompileFeature::DistCC)) {
            Fmt(&buf, "distcc ");
        }

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "\"%1\" -std=gnu11", cc); } break;
            case SourceType::Cxx: { Fmt(&buf, "\"%1\" -std=gnu++2a", cxx); } break;
            case SourceType::Esbuild:
            case SourceType::QtUi:
            case SourceType::QtResources: { RG_UNREACHABLE(); } break;
        }
        if (dest_filename) {
            Fmt(&buf, " -o \"%1\"", dest_filename);
        } else {
            switch (src_type) {
                case SourceType::C: { Fmt(&buf, " -x c-header"); } break;
                case SourceType::Cxx: { Fmt(&buf, " -x c++-header"); } break;
                case SourceType::Esbuild:
                case SourceType::QtUi:
                case SourceType::QtResources: { RG_UNREACHABLE(); } break;
            }
        }
        Fmt(&buf, " -I. -MD -MF \"%1.d\"", dest_filename ? dest_filename : src_filename);
        out_cmd->rsp_offset = buf.len;

        // Build options
        Fmt(&buf, " -fvisibility=hidden -fno-strict-aliasing -fno-delete-null-pointer-checks -fno-omit-frame-pointer");
        if (gcc_ver >= 1000) {
            Fmt(&buf, " -fno-finite-loops");
        }
        if (features & (int)CompileFeature::OptimizeSpeed) {
            Fmt(&buf, " -O2 -fwrapv -DNDEBUG");
        } else if (features & (int)CompileFeature::OptimizeSize) {
            Fmt(&buf, " -Os -fwrapv -DNDEBUG -ffunction-sections -fdata-sections");
        } else {
            Fmt(&buf, " -O0 -ftrapv -fsanitize-undefined-trap-on-error");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto");
        }
        if (features & (int)CompileFeature::Warnings) {
            Fmt(&buf, " -Wall -Wextra -Wuninitialized -Wno-cast-function-type");
            if (src_type == SourceType::Cxx) {
                Fmt(&buf, " -Wno-init-list-lifetime -Wzero-as-null-pointer-constant");
            }
            Fmt(&buf, " -Wreturn-type -Werror=return-type");
        } else {
            Fmt(&buf, " -w");
        }
        if (features & (int)CompileFeature::HotAssets) {
            Fmt(&buf, " -DFELIX_HOT_ASSETS");
        }

        // Include build directory (for generated files)
        Span<const char> dest_directory = GetPathDirectory(dest_filename);
        Fmt(&buf, " \"-I%1\"", dest_directory);

        if (architecture == HostArchitecture::x86_64) {
            Fmt(&buf, " -mpopcnt -msse4.1 -msse4.2 -mssse3 -mcx16");

            if (features & (int)CompileFeature::AESNI) {
                Fmt(&buf, " -maes -mpclmul");
            }
            if (features & (int)CompileFeature::AVX2) {
                Fmt(&buf, " -mavx2");
            }
            if (features & (int)CompileFeature::AVX512) {
                Fmt(&buf, " -mavx512f -mavx512vl");
            }
        } else if (architecture == HostArchitecture::x86) {
            Fmt(&buf, " -msse2");
        }

        // Platform flags
        switch (platform) {
            case HostPlatform::Windows: {
                Fmt(&buf, " -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE"
                          " -D__USE_MINGW_ANSI_STDIO=1");
            } break;

            case HostPlatform::macOS: {
                Fmt(&buf, " -pthread -fPIC");
            } break;

            case HostPlatform::Linux: {
                Fmt(&buf, " -pthread -fPIC -fno-semantic-interposition -D_FILE_OFFSET_BITS=64 -D_GLIBCXX_ASSERTIONS");

                if (features & ((int)CompileFeature::OptimizeSpeed | (int)CompileFeature::OptimizeSize)) {
                    Fmt(&buf, " -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=%1", gcc_ver >= 1200 ? 3 : 2);
                } else {
                    Fmt(&buf, " -D_GLIBCXX_DEBUG -D_GLIBCXX_SANITIZE_VECTOR");
                }

                if (architecture == HostArchitecture::ARM32) {
                    Fmt(&buf, " -Wno-psabi");
                }
            } break;

            default: {
                Fmt(&buf, " -pthread -fPIC -fno-semantic-interposition -D_FILE_OFFSET_BITS=64");

                if (features & ((int)CompileFeature::OptimizeSpeed | (int)CompileFeature::OptimizeSize)) {
                    Fmt(&buf, " -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2");
                }
            } break;
        }

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " -g");
        }
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " -fsanitize=address");
        }
        if (features & (int)CompileFeature::TSan) {
            Fmt(&buf, " -fsanitize=thread");
        }
        if (features & (int)CompileFeature::UBSan) {
            Fmt(&buf, " -fsanitize=undefined");
        }
        Fmt(&buf, " -fstack-protector-strong --param ssp-buffer-size=4");
        if (platform != HostPlatform::Windows) {
            Fmt(&buf, " -fstack-clash-protection");
        }
        if (features & (int)CompileFeature::ZeroInit) {
            Fmt(&buf, " -ftrivial-auto-var-init=zero");
        }
        if (features & (int)CompileFeature::CFI) {
            Fmt(&buf, " -fcf-protection=full");
        }

        // Sources and definitions
        Fmt(&buf, " -DFELIX -c \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " -include \"%1\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " \"-%1%2\"", definition[0] != '-' ? 'D' : 'U', definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " \"-I%1\"", include_directory);
        }
        for (const char *system_directory: system_directories) {
            Fmt(&buf, " -isystem \"%1\"", system_directory);
        }
        for (const char *include_file: include_files) {
            Fmt(&buf, " -include \"%1\"", include_file);
        }

        if (env_flags) {
            switch (src_type) {
                case SourceType::C: { AddEnvironmentFlags({"CPPFLAGS", "CFLAGS"}, &buf); } break;
                case SourceType::Cxx: { AddEnvironmentFlags({"CPPFLAGS", "CXXFLAGS"}, &buf); } break;
                case SourceType::Esbuild:
                case SourceType::QtUi:
                case SourceType::QtResources: { RG_UNREACHABLE(); } break;
            }
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(STDOUT_FILENO)) {
            Fmt(&buf, " -fdiagnostics-color=always");
        } else {
            Fmt(&buf, " -fdiagnostics-color=never");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);

        // Dependencies
        out_cmd->deps_mode = Command::DependencyMode::MakeLike;
        out_cmd->deps_filename = Fmt(alloc, "%1.d", dest_filename ? dest_filename : src_filename).ptr;
    }

    void MakeResourceCommand(const char *rc_filename, const char *dest_filename,
                             Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        out_cmd->cmd_line = Fmt(alloc, "\"%1\" -O coff \"%2\" \"%3\"", windres, rc_filename, dest_filename);
        out_cmd->cache_len = out_cmd->cmd_line.len;
    }

    void MakeLinkCommand(Span<const char *const> obj_filenames,
                         Span<const char *const> libraries, TargetType link_type,
                         uint32_t features, bool env_flags, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Linker
        switch (link_type) {
            case TargetType::Executable: {
                bool static_link = (features & (int)CompileFeature::StaticRuntime);
                Fmt(&buf, "\"%1\"%2", cxx, static_link ? " -static" : "");
            } break;
            case TargetType::Library: { Fmt(&buf, "\"%1\" -shared", cxx); } break;
        }
        Fmt(&buf, " -o \"%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Build mode
        if (!(features & (int)CompileFeature::DebugInfo)) {
            Fmt(&buf, " -s");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto -Wl,-O1");
        }

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        if (libraries.len) {
            HashSet<Span<const char>> framework_paths;

            if (platform != HostPlatform::Windows) {
                Fmt(&buf, " -Wl,--start-group");
            }
            for (const char *lib: libraries) {
                if (platform == HostPlatform::macOS && lib[0] == '!') {
                    Span<const char> directory = {};
                    Span<const char> basename = SplitStrReverse(lib + 1, '/', &directory);

                    if (EndsWith(basename, ".framework")) {
                        basename = basename.Take(0, basename.len - 10);
                    }

                    if (directory.len) {
                        bool inserted = false;
                        framework_paths.TrySet(directory, &inserted);

                        if (inserted) {
                            Fmt(&buf, " -F \"%1\"", directory);
                        }
                    }
                    Fmt(&buf, " -framework %1", basename);
                } else if (strpbrk(lib, RG_PATH_SEPARATORS)) {
                    Fmt(&buf, " %1", lib);
                } else {
                    Fmt(&buf, " -l%1", lib);
                }
            }
            if (platform != HostPlatform::Windows) {
                Fmt(&buf, " -Wl,--end-group");
            }
        }

        // Platform flags and libraries
        Fmt(&buf, " -Wl,--gc-sections");
        switch (platform) {
            case HostPlatform::Windows: {
                Fmt(&buf, " -Wl,--dynamicbase -Wl,--nxcompat");
                if (!i686) {
                    Fmt(&buf, " -Wl,--high-entropy-va");
                }

                Fmt(&buf, " -static-libgcc -static-libstdc++");
                if (!(features & (int)CompileFeature::StaticRuntime)) {
                    Fmt(&buf, " -Wl,-Bstatic -lstdc++ -lpthread -lssp -Wl,-Bdynamic");
                }
            } break;

            case HostPlatform::macOS: {
                Fmt(&buf, " -ldl -pthread -framework CoreFoundation -framework SystemConfiguration");
                Fmt(&buf, " -rpath \"@executable_path/../Frameworks\"");
            } break;

            default: {
                Fmt(&buf, " -pthread -Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code,-z,stack-size=1048576");
                Fmt(&buf, " -static-libgcc -static-libstdc++");

                if (platform == HostPlatform::Linux) {
                    Fmt(&buf, " -ldl -lrt");
                }
                if (link_type == TargetType::Executable) {
                    Fmt(&buf, " -pie");
                }
                if (architecture == HostArchitecture::ARM32) {
                    Fmt(&buf, " -latomic");
                }
            } break;
        }

        // Features
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " -fsanitize=address");
        }
        if (features & (int)CompileFeature::TSan) {
            Fmt(&buf, " -fsanitize=thread");
        }
        if (features & (int)CompileFeature::UBSan) {
            Fmt(&buf, " -fsanitize=undefined");
        }
        if (features & (int)CompileFeature::NoConsole) {
            Fmt(&buf, " -mwindows");
        }

        if (ld) {
            Fmt(&buf, " -fuse-ld=%1", ld);
        }
        if (env_flags) {
            AddEnvironmentFlags("LDFLAGS", &buf);
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(STDOUT_FILENO)) {
            Fmt(&buf, " -fdiagnostics-color=always");
        } else {
            Fmt(&buf, " -fdiagnostics-color=never");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);
    }

    void MakePostCommand(const char *, const char *, Allocator *, Command *) const override { RG_UNREACHABLE(); }
};

#ifdef _WIN32
class MsCompiler final: public Compiler {
    const char *cl;
    const char *rc;
    const char *link;

    int cl_ver = 0;

    BlockAllocator str_alloc;

public:
    MsCompiler(HostArchitecture architecture) : Compiler(HostPlatform::Windows, architecture, "MSVC") {}

    static std::unique_ptr<const Compiler> Create(HostArchitecture architecture, const char *cl)
    {
        std::unique_ptr<MsCompiler> compiler = std::make_unique<MsCompiler>(architecture);

        // Find executables
        {
            Span<const char> prefix;
            Span<const char> suffix;
            Span<const char> version;
            if (!SplitPrefixSuffix(cl, "cl", &prefix, &suffix, &version))
                return nullptr;

            compiler->cl = DuplicateString(cl, &compiler->str_alloc).ptr;
            compiler->rc = Fmt(&compiler->str_alloc, "%1rc%2", prefix, version).ptr;
            compiler->link = Fmt(&compiler->str_alloc, "%1link%2", prefix, version).ptr;
        }

        // Determine CL version
        {
            char cmd[2048];
            Fmt(cmd, "\"%1\"", compiler->cl);

            HeapArray<char> output;
            if (ReadCommandOutput(cmd, &output)) {
                compiler->cl_ver = ParseVersion(cmd, output, "Version");
            }
        }

        return compiler;
    }

    bool CheckFeatures(uint32_t features, uint32_t maybe_features, uint32_t *out_features) const override
    {
        uint32_t supported = 0;

        supported |= (int)CompileFeature::OptimizeSpeed;
        supported |= (int)CompileFeature::OptimizeSize;
        supported |= (int)CompileFeature::HotAssets;
        supported |= (int)CompileFeature::PCH;
        supported |= (int)CompileFeature::Warnings;
        supported |= (int)CompileFeature::DebugInfo;
        supported |= (int)CompileFeature::ASan;
        supported |= (int)CompileFeature::LTO;
        supported |= (int)CompileFeature::CFI;
        supported |= (int)CompileFeature::LinkLibrary;
        supported |= (int)CompileFeature::StaticRuntime;
        supported |= (int)CompileFeature::NoConsole;
        supported |= (int)CompileFeature::AESNI;
        supported |= (int)CompileFeature::AVX2;
        supported |= (int)CompileFeature::AVX512;

        uint32_t unsupported = features & ~supported;
        if (unsupported) {
            LogError("Some features are not supported by %1: %2",
                     name, FmtFlags(unsupported, CompileFeatureOptions));
            return false;
        }

        features |= (supported & maybe_features);

        if ((features & (int)CompileFeature::OptimizeSpeed) && (features & (int)CompileFeature::OptimizeSize)) {
            LogError("Cannot use OptimizeSpeed and OptimizeSize at the same time");
            return false;
        }

        *out_features = features;
        return true;
    }

    const char *GetObjectExtension() const override { return ".obj"; }
    const char *GetLinkExtension(TargetType type) const override
    {
        switch (type) {
            case TargetType::Executable: return ".exe";
            case TargetType::Library: return ".dll";
        }

        RG_UNREACHABLE();
    }
    const char *GetPostExtension(TargetType) const override { return nullptr; }

    void MakePackCommand(Span<const char *const> pack_filenames,
                         const char *pack_options, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        // Strings literals were limited in length before MSVC 2022
        bool use_arrays = (cl_ver < 1930);

        RG::MakePackCommand(pack_filenames, use_arrays, pack_options, dest_filename, alloc, out_cmd);
    }

    void MakePchCommand(const char *pch_filename, SourceType src_type,
                        Span<const char *const> definitions, Span<const char *const> include_directories,
                        Span<const char *const> include_files, uint32_t features, bool env_flags,
                        Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        MakeObjectCommand(pch_filename, src_type, nullptr, definitions, include_directories, {},
                          include_files, features, env_flags, nullptr, alloc, out_cmd);
    }

    const char *GetPchCache(const char *pch_filename, Allocator *alloc) const override
    {
        RG_ASSERT(alloc);

        const char *cache_filename = Fmt(alloc, "%1.pch", pch_filename).ptr;
        return cache_filename;
    }
    const char *GetPchObject(const char *pch_filename, Allocator *alloc) const override
    {
        RG_ASSERT(alloc);

        const char *obj_filename = Fmt(alloc, "%1.obj", pch_filename).ptr;
        return obj_filename;
    }

    void MakeObjectCommand(const char *src_filename, SourceType src_type,
                           const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, Span<const char *const> system_directories,
                           Span<const char *const> include_files, uint32_t features, bool env_flags,
                           const char *dest_filename, Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "\"%1\" /nologo", cl); } break;
            case SourceType::Cxx: { Fmt(&buf, "\"%1\" /nologo /std:c++20 /Zc:__cplusplus", cl); } break;
            case SourceType::Esbuild:
            case SourceType::QtUi:
            case SourceType::QtResources: { RG_UNREACHABLE(); } break;
        }
        if (dest_filename) {
            Fmt(&buf, " \"/Fo%1\"", dest_filename);
        } else {
            Fmt(&buf, " /Yc \"/Fp%1.pch\" \"/Fo%1.obj\"", src_filename);
        }
        Fmt(&buf, " /Zc:preprocessor /showIncludes");
        out_cmd->rsp_offset = buf.len;

        // Build options
        Fmt(&buf, " /I. /EHsc /utf-8");
        if (features & (int)CompileFeature::OptimizeSpeed) {
            Fmt(&buf, " /O2 /DNDEBUG");
        } else if (features & (int)CompileFeature::OptimizeSize) {
            Fmt(&buf, " /O1 /DNDEBUG");
        } else {
            Fmt(&buf, " /Od /RTCsu");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " /GL");
        }
        if (features & (int)CompileFeature::Warnings) {
            Fmt(&buf, " /W4 /wd4200 /wd4706 /wd4100 /wd4127 /wd4702 /wd4815 /wd4206 /wd4456 /wd4457 /wd4458 /wd4459");
        } else {
            Fmt(&buf, " /w");
        }
        if (features & (int)CompileFeature::HotAssets) {
            Fmt(&buf, " /DFELIX_HOT_ASSETS");
        }

        // Include build directory (for generated files)
        Span<const char> dest_directory = GetPathDirectory(dest_filename);
        Fmt(&buf, " \"/I%1\"", dest_directory);

        // Platform flags
        Fmt(&buf, " /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 /DUNICODE /D_UNICODE"
                  " /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE");

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " /Z7 /Zo");
        }
        if (features & (int)CompileFeature::StaticRuntime) {
            Fmt(&buf, " /MT");
        } else {
            Fmt(&buf, " /MD");
        }
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " /fsanitize=address");
        }
        Fmt(&buf, " /GS");
        if (features & (int)CompileFeature::CFI) {
            Fmt(&buf, " /guard:cf /guard:ehcont");
        }

        if (architecture == HostArchitecture::x86_64) {
            if (features & (int)CompileFeature::AVX2) {
                Fmt(&buf, " /arch:AVX2");
            }
            if (features & (int)CompileFeature::AVX512) {
                Fmt(&buf, " /arch:AVX512");
            }
        }

        // Sources and definitions
        Fmt(&buf, " /DFELIX /c /utf-8 \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " \"/FI%1\" \"/Yu%1\" \"/Fp%1.pch\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " \"/%1%2\"", definition[0] != '-' ? 'D' : 'U', definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " \"/I%1\"", include_directory);
        }
        for (const char *system_directory: system_directories) {
            Fmt(&buf, " \"/I%1\"", system_directory);
        }
        for (const char *include_file: include_files) {
            if (PathIsAbsolute(include_file)) {
                Fmt(&buf, " \"/FI%1\"", include_file);
            } else {
                const char *cwd = GetWorkingDirectory();
                Fmt(&buf, " \"/FI%1%/%2\"", cwd, include_file);
            }
        }

        if (env_flags) {
            switch (src_type) {
                case SourceType::C: { AddEnvironmentFlags({"CPPFLAGS", "CFLAGS"}, &buf); } break;
                case SourceType::Cxx: { AddEnvironmentFlags({"CPPFLAGS", "CXXFLAGS"}, &buf); } break;
                case SourceType::Esbuild:
                case SourceType::QtUi:
                case SourceType::QtResources: { RG_UNREACHABLE(); } break;
            }
        }

        out_cmd->cache_len = buf.len;
        out_cmd->cmd_line = buf.TrimAndLeak(1);
        out_cmd->skip_lines = 1;

        // Dependencies
        out_cmd->deps_mode = Command::DependencyMode::ShowIncludes;
    }

    void MakeResourceCommand(const char *rc_filename, const char *dest_filename,
                             Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        out_cmd->cmd_line = Fmt(alloc, "\"%1\" /nologo /FO\"%2\" \"%3\"", rc, dest_filename, rc_filename);
        out_cmd->cache_len = out_cmd->cmd_line.len;
    }

    void MakeLinkCommand(Span<const char *const> obj_filenames,
                         Span<const char *const> libraries, TargetType link_type,
                         uint32_t features, bool env_flags, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Linker
        switch (link_type) {
            case TargetType::Executable: { Fmt(&buf, "\"%1\" /nologo", link); } break;
            case TargetType::Library: { Fmt(&buf, "\"%1\" /nologo /DLL", link); } break;
        }
        Fmt(&buf, " \"/OUT:%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Build mode
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " /LTCG");
        }
        Fmt(&buf, " /DYNAMICBASE /HIGHENTROPYVA /OPT:ref");

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        for (const char *lib: libraries) {
            if (GetPathExtension(lib).len) {
                Fmt(&buf, " %1", lib);
            } else {
                Fmt(&buf, " %1.lib", lib);
            }
        }
        Fmt(&buf, " setargv.obj");

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " /DEBUG:FULL");
        } else {
            Fmt(&buf, " /DEBUG:NONE");
        }
        if (features & (int)CompileFeature::CFI) {
            Fmt(&buf, " /GUARD:cf /GUARD:ehcont");
        }
        if (features & (int)CompileFeature::NoConsole) {
            Fmt(&buf, " /SUBSYSTEM:windows /ENTRY:mainCRTStartup");
        }

        if (env_flags) {
            AddEnvironmentFlags("LDFLAGS", &buf);
        }

        out_cmd->cache_len = buf.len;
        out_cmd->cmd_line = buf.TrimAndLeak(1);
        out_cmd->skip_lines = 1;
    }

    void MakePostCommand(const char *, const char *, Allocator *, Command *) const override { RG_UNREACHABLE(); }
};
#endif

class EmCompiler final: public Compiler {
    const char *cc;
    const char *cxx;

    BlockAllocator str_alloc;

public:
    EmCompiler(HostPlatform platform) : Compiler(platform, HostArchitecture::Web, "EmCC") {}

    static std::unique_ptr<const Compiler> Create(HostPlatform platform, const char *cc)
    {
        std::unique_ptr<EmCompiler> compiler = std::make_unique<EmCompiler>(platform);

        // Find executables
        {
            if (!FindExecutableInPath(cc, &compiler->str_alloc, &cc)) {
                LogError("Could not find '%1' in PATH", cc);
                return nullptr;
            }
            if (platform == HostPlatform::EmscriptenBox && !FindExecutableInPath("wasm2c")) {
                LogError("Could not find 'wasm2c' in PATH");
                return nullptr;
            }

            Span<const char> prefix;
            Span<const char> suffix;
            Span<const char> version;
            if (!SplitPrefixSuffix(cc, "emcc", &prefix, &suffix, &version))
                return nullptr;

            compiler->cc = cc;
            compiler->cxx = Fmt(&compiler->str_alloc, "%1em++%2", prefix, suffix).ptr;
        }

        return compiler;
    }

    bool CheckFeatures(uint32_t features, uint32_t maybe_features, uint32_t *out_features) const override
    {
        uint32_t supported = 0;

        supported |= (int)CompileFeature::OptimizeSpeed;
        supported |= (int)CompileFeature::OptimizeSize;
        supported |= (int)CompileFeature::Warnings;
        supported |= (int)CompileFeature::DebugInfo;

        uint32_t unsupported = features & ~supported;
        if (unsupported) {
            LogError("Some features are not supported by %1: %2",
                     name, FmtFlags(unsupported, CompileFeatureOptions));
            return false;
        }

        features |= (supported & maybe_features);

        if ((features & (int)CompileFeature::OptimizeSpeed) && (features & (int)CompileFeature::OptimizeSize)) {
            LogError("Cannot use OptimizeSpeed and OptimizeSize at the same time");
            return false;
        }

        *out_features = features;
        return true;
    }

    const char *GetObjectExtension() const override { return ".o"; }
    const char *GetLinkExtension(TargetType) const override
    {
        switch (platform) {
            case HostPlatform::EmscriptenNode: return ".js";
            case HostPlatform::EmscriptenWeb: return ".html";
            case HostPlatform::EmscriptenBox: return ".wasm";

            default: { RG_UNREACHABLE(); } break;
        }
    }
    const char *GetPostExtension(TargetType) const override
    {
        const char *ext = (platform == HostPlatform::EmscriptenBox) ? ".c" : nullptr;
        return ext;
    }

    void MakePackCommand(Span<const char *const> pack_filenames,
                         const char *pack_options, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        RG::MakePackCommand(pack_filenames, false, pack_options, dest_filename, alloc, out_cmd);
    }

    void MakePchCommand(const char *, SourceType, Span<const char *const>, Span<const char *const>,
                        Span<const char *const>, uint32_t, bool, Allocator *, Command *) const override { RG_UNREACHABLE(); }

    const char *GetPchCache(const char *, Allocator *) const override { return nullptr; }
    const char *GetPchObject(const char *, Allocator *) const override { return nullptr; }

    void MakeObjectCommand(const char *src_filename, SourceType src_type,
                           const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, Span<const char *const> system_directories,
                           Span<const char *const> include_files, uint32_t features, bool env_flags,
                           const char *dest_filename, Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        // Hide noisy EmCC messages
        {
            static const ExecuteInfo::KeyValue variables[] = {
                { "EMCC_LOGGING", "0" }
            };

            out_cmd->env_variables = variables;
        }

        HeapArray<char> buf(alloc);

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "\"%1\" -std=gnu11", cc); } break;
            case SourceType::Cxx: { Fmt(&buf, "\"%1\" -std=gnu++2a", cxx); } break;
            case SourceType::Esbuild:
            case SourceType::QtUi:
            case SourceType::QtResources: { RG_UNREACHABLE(); } break;
        }
        RG_ASSERT(dest_filename); // No PCH
        Fmt(&buf, " -o \"%1\"", dest_filename);
        Fmt(&buf, " -MD -MF \"%1.d\"", dest_filename ? dest_filename : src_filename);
        out_cmd->rsp_offset = buf.len;

        // Build options
        Fmt(&buf, " -I. -fvisibility=hidden -fno-strict-aliasing -fno-delete-null-pointer-checks -fno-omit-frame-pointer");
        if (features & (int)CompileFeature::OptimizeSpeed) {
            Fmt(&buf, " -O1 -fwrapv -DNDEBUG");
        } else if (features & (int)CompileFeature::OptimizeSize) {
            Fmt(&buf, " -Os -fwrapv -DNDEBUG");
        } else {
            Fmt(&buf, " -O0 -ftrapv");
        }
        if (features & (int)CompileFeature::Warnings) {
            Fmt(&buf, " -Wall -Wextra");
            if (src_type == SourceType::Cxx) {
                Fmt(&buf, " -Wzero-as-null-pointer-constant");
            }
            Fmt(&buf, " -Wreturn-type -Werror=return-type");
        } else {
            Fmt(&buf, " -Wno-everything");
        }
        Fmt(&buf, " -fPIC");

        // Include build directory (for generated files)
        Span<const char> dest_directory = GetPathDirectory(dest_filename);
        Fmt(&buf, " \"-I%1\"", dest_directory);

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " -g");
        }

        // Sources and definitions
        Fmt(&buf, " -DFELIX -c \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " -include \"%1\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " \"-%1%2\"", definition[0] != '-' ? 'D' : 'U', definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " \"-I%1\"", include_directory);
        }
        for (const char *system_directory: system_directories) {
            Fmt(&buf, " -isystem \"%1\"", system_directory);
        }
        for (const char *include_file: include_files) {
            Fmt(&buf, " -include \"%1\"", include_file);
        }

        if (env_flags) {
            switch (src_type) {
                case SourceType::C: { AddEnvironmentFlags({"CPPFLAGS", "CFLAGS"}, &buf); } break;
                case SourceType::Cxx: { AddEnvironmentFlags({"CPPFLAGS", "CXXFLAGS"}, &buf); } break;
                case SourceType::Esbuild:
                case SourceType::QtUi:
                case SourceType::QtResources: { RG_UNREACHABLE(); } break;
            }
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(STDOUT_FILENO)) {
            Fmt(&buf, " -fcolor-diagnostics -fansi-escape-codes");
        } else {
            Fmt(&buf, " -fno-color-diagnostics");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);

        // Dependencies
        out_cmd->deps_mode = Command::DependencyMode::MakeLike;
        out_cmd->deps_filename = Fmt(alloc, "%1.d", dest_filename ? dest_filename : src_filename).ptr;
    }

    void MakeResourceCommand(const char *, const char *, Allocator *, Command *) const override { RG_UNREACHABLE(); }

    void MakeLinkCommand(Span<const char *const> obj_filenames,
                         Span<const char *const> libraries, TargetType link_type,
                         uint32_t, bool env_flags, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Linker
        switch (link_type) {
            case TargetType::Executable: { Fmt(&buf, "\"%1\"", cxx); } break;
            case TargetType::Library: { RG_UNREACHABLE(); } break;
        }
        Fmt(&buf, " -o \"%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        if (libraries.len) {
            Fmt(&buf, " -Wl,--start-group");
            for (const char *lib: libraries) {
                if (strpbrk(lib, RG_PATH_SEPARATORS)) {
                    Fmt(&buf, " %1", lib);
                } else {
                    Fmt(&buf, " -l%1", lib);
                }
            }
            Fmt(&buf, " -Wl,--end-group");
        }

        // Platform flags
        Fmt(&buf, " -s ALLOW_MEMORY_GROWTH=1 -s MAXIMUM_MEMORY=2147483648");
        if (platform == HostPlatform::EmscriptenNode) {
            Fmt(&buf, " -s NODERAWFS=1 -lnodefs.js");
        } else if (platform == HostPlatform::EmscriptenBox) {
            Fmt(&buf, " -s STANDALONE_WASM=1");

            if (link_type == TargetType::Library) {
                Fmt(&buf, " -s SIDE_MODULE=1");
            }
        }

        if (env_flags) {
            AddEnvironmentFlags("LDFLAGS", &buf);
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(STDOUT_FILENO)) {
            Fmt(&buf, " -fcolor-diagnostics -fansi-escape-codes");
        } else {
            Fmt(&buf, " -fno-color-diagnostics");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);
    }

    void MakePostCommand(const char *src_filename, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        Span<const char> cmd = Fmt(alloc, "wasm2c \"%1\" -o \"%2\"", src_filename, dest_filename);
        out_cmd->cmd_line = cmd;
    }
};

std::unique_ptr<const Compiler> PrepareCompiler(HostSpecifier spec)
{
    if (spec.platform == NativePlatform && spec.architecture == NativeArchitecture) {
        if (!spec.cc) {
            for (const SupportedCompiler &supported: SupportedCompilers) {
                if (supported.cc && FindExecutableInPath(supported.cc)) {
                    spec.cc = supported.cc;
                    break;
                }
            }

            if (!spec.cc) {
                LogError("Could not find any supported compiler in PATH");
                return nullptr;
            }
        } else if (!FindExecutableInPath(spec.cc)) {
            LogError("Cannot find compiler '%1' in PATH", spec.cc);
            return nullptr;
        }

        if (spec.ld) {
            if (TestStr(spec.ld, "bfd") || TestStr(spec.ld, "ld")) {
                if (!FindExecutableInPath("ld.bfd")) {
                    LogError("Cannot find linker 'ld' in PATH");
                    return nullptr;
                }

                spec.ld = "bfd";
#ifdef _WIN32
            } else if (TestStr(spec.ld, "link")) {
                if (!FindExecutableInPath("link")) {
                    LogError("Cannot find linker 'link.exe' in PATH");
                    return nullptr;
                }
#endif
            } else {
                char buf[512];
                Fmt(buf, "ld.%1", spec.ld);

                if (!FindExecutableInPath(buf)) {
                    LogError("Cannot find linker '%1' in PATH", buf);
                    return nullptr;
                }
            }
        }

        if (IdentifyCompiler(spec.cc, "clang")) {
            return ClangCompiler::Create(spec.platform, spec.architecture, spec.cc, spec.ld);
        } else if (IdentifyCompiler(spec.cc, "gcc")) {
            return GnuCompiler::Create(spec.platform, spec.architecture, spec.cc, spec.ld);
#ifdef _WIN32
        } else if (IdentifyCompiler(spec.cc, "cl")) {
            if (spec.ld) {
                LogError("Cannot use custom linker with MSVC compiler");
                return nullptr;
            }

            return MsCompiler::Create(spec.architecture, spec.cc);
#endif
        } else {
            LogError("Cannot find driver for compiler '%1'", spec.cc);
            return nullptr;
        }
    } else if (StartsWith(HostPlatformNames[(int)spec.platform], "WASM/Emscripten/")) {
        if (spec.architecture != HostArchitecture::Web) {
            LogError("Emscripten can only build for Web archtiecture, not %1", HostArchitectureNames[(int)spec.architecture]);
            return nullptr;
        }

        if (!spec.cc) {
            spec.cc = "emcc";
        }
        if (!IdentifyCompiler(spec.cc, "emcc")) {
            LogError("Only Emcripten (emcc) can be used for WASM cross-compilation");
            return nullptr;
        }

        if (spec.ld) {
            LogError("Cannot use custom linker for platform '%1'", HostPlatformNames[(int)spec.platform]);
            return nullptr;
        }

        return EmCompiler::Create(spec.platform, spec.cc);
#ifdef __linux__
    } else if (spec.platform == HostPlatform::Windows && spec.architecture == HostArchitecture::x86_64) {
        if (!spec.cc) {
            LogError("Path to cross-platform MinGW must be explicitly specified");
            return nullptr;
        }

        if (IdentifyCompiler(spec.cc, "mingw-w64") || IdentifyCompiler(spec.cc, "w64-mingw32")) {
            return GnuCompiler::Create(spec.platform, spec.architecture, spec.cc, spec.ld);
        } else {
            LogError("Only MinGW-w64 can be used for Windows cross-compilation at the moment");
            return nullptr;
        }
    } else if (spec.platform == HostPlatform::Linux) {
        // Go with GCC if not specified otherwise
        if (!spec.cc) {
            switch (spec.architecture) {
                case HostArchitecture::x86: { spec.cc = "i686-linux-gnu-gcc"; } break;
                case HostArchitecture::x86_64: { spec.cc = "x86_64-linux-gnu-gcc"; } break;
                case HostArchitecture::ARM64: { spec.cc = "aarch64-linux-gnu-gcc"; } break;
                case HostArchitecture::RISCV64: { spec.cc = "riscv64-linux-gnu-gcc"; }  break;

                case HostArchitecture::ARM32:
                case HostArchitecture::AVR:
                case HostArchitecture::Web: {} break;
            }
        }

        if (IdentifyCompiler(spec.cc, "gcc")) {
            return GnuCompiler::Create(spec.platform, spec.architecture, spec.cc, spec.ld);
        } else if (IdentifyCompiler(spec.cc, "clang")) {
            if (!spec.ld) {
                spec.ld = "lld";
            }
            if (!IdentifyCompiler(spec.ld, "lld")) {
                LogError("Use LLD for cross-compiling with Clang");
                return nullptr;
            }

            switch (spec.architecture)  {
                case HostArchitecture::x86:
                case HostArchitecture::x86_64:
                case HostArchitecture::ARM64:
                case HostArchitecture::RISCV64: {} break;

                case HostArchitecture::ARM32:
                case HostArchitecture::AVR:
                case HostArchitecture::Web: {
                    LogError("Clang cross-compilation for '%1' is not supported", HostArchitectureNames[(int)spec.architecture]);
                    return nullptr;
                } break;
            }

            return ClangCompiler::Create(spec.platform, spec.architecture, spec.cc, spec.ld);
        } else {
            LogError("Only GCC or Clang can be used for Linux cross-compilation at the moment");
            return nullptr;
        }
#endif
    } else {
        LogError("Cross-compilation from platform '%1 (%2)' to '%3 (%4)' is not supported",
                 HostPlatformNames[(int)NativePlatform], HostArchitectureNames[(int)NativeArchitecture],
                 HostPlatformNames[(int)spec.platform], HostArchitectureNames[(int)spec.architecture]);
        return nullptr;
    }
}

bool DetermineSourceType(const char *filename, SourceType *out_type)
{
    Span<const char> extension = GetPathExtension(filename);

    if (extension == ".c") {
        if (out_type) {
            *out_type = SourceType::C;
        }
        return true;
    } else if (extension == ".cc" || extension == ".cpp") {
        if (out_type) {
            *out_type = SourceType::Cxx;
        }
        return true;
    } else if (extension == ".js" || extension == ".css") {
        if (out_type) {
            *out_type = SourceType::Esbuild;
        }
        return true;
    } else if (extension == ".ui") {
        if (out_type) {
            *out_type = SourceType::QtUi;
        }
        return true;
    } else if (extension == ".qrc") {
        if (out_type) {
            *out_type = SourceType::QtResources;
        }
        return true;
    } else {
        return false;
    }
}

static const SupportedCompiler CompilerTable[] = {
#if defined(_WIN32)
    {"Clang", "clang"},
    {"MSVC", "cl"},
    {"GCC", "gcc"},
#elif defined(__linux__)
    {"GCC", "gcc"},
    {"Clang", "clang"},
#else
    {"Clang", "clang"},
    {"GCC", "gcc"},
#endif

    {"EmCC", "emcc"}
};
const Span<const SupportedCompiler> SupportedCompilers = CompilerTable;

}
