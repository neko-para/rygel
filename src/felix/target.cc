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

#include "src/core/libcc/libcc.hh"
#include "target.hh"

namespace RG {

struct FileSet {
    HeapArray<const char *> directories;
    HeapArray<const char *> directories_rec;
    HeapArray<const char *> filenames;
    HeapArray<const char *> ignore;
};

struct SourceFeatures {
    uint32_t enable_features;
    uint32_t disable_features;
};

// Temporary struct used until target is created
struct TargetConfig {
    const char *name;
    TargetType type;
    unsigned int platforms;
    bool enable_by_default;

    const char *title;
    const char *version_tag;
    const char *icon_filename;

    FileSet src_file_set;
    const char *c_pch_filename;
    const char *cxx_pch_filename;

    HeapArray<const char *> imports;

    HeapArray<const char *> definitions;
    HeapArray<const char *> export_definitions;
    HeapArray<const char *> include_directories;
    HeapArray<const char *> include_files;
    HeapArray<const char *> libraries;

    HeapArray<const char *> qt_components;
    int64_t qt_version;

    HashMap<const char *, SourceFeatures> src_features;

    uint32_t enable_features;
    uint32_t disable_features;

    const char *bundle_options;

    FileSet pack_file_set;
    const char *pack_options;

    RG_HASHTABLE_HANDLER(TargetConfig, name);
};

static void AppendNormalizedPath(Span<const char> path, Allocator *alloc, HeapArray<const char *> *out_paths)
{
    RG_ASSERT(alloc);

    path = NormalizePath(path, alloc);
    out_paths->Append(path.ptr);
}

static void AppendListValues(Span<const char> str, Allocator *alloc, HeapArray<const char *> *out_libraries)
{
    RG_ASSERT(alloc);

    while (str.len) {
        Span<const char> lib = TrimStr(SplitStrAny(str, " ,", &str));

        if (lib.len) {
            const char *copy = DuplicateString(lib, alloc).ptr;
            out_libraries->Append(copy);
        }
    }
}

static bool EnumerateSortedFiles(const char *directory, bool recursive,
                                 Allocator *alloc, HeapArray<const char *> *out_filenames)
{
    RG_ASSERT(alloc);

    Size start_idx = out_filenames->len;

    if (!EnumerateFiles(directory, nullptr, recursive ? -1  : 0, 1024, alloc, out_filenames))
            return false;

    std::sort(out_filenames->begin() + start_idx, out_filenames->end(),
              [](const char *filename1, const char *filename2) {
        return CmpStr(filename1, filename2) < 0;
    });

    return true;
}

static bool ResolveFileSet(const FileSet &file_set,
                           Allocator *alloc, HeapArray<const char *> *out_filenames)
{
    RG_ASSERT(alloc);

    RG_DEFER_NC(out_guard, len = out_filenames->len) { out_filenames->RemoveFrom(len); };

    out_filenames->Append(file_set.filenames);
    for (const char *directory: file_set.directories) {
        if (!EnumerateSortedFiles(directory, false, alloc, out_filenames))
            return false;
    }
    for (const char *directory: file_set.directories_rec) {
        if (!EnumerateSortedFiles(directory, true, alloc, out_filenames))
            return false;
    }

    out_filenames->RemoveFrom(std::remove_if(out_filenames->begin(), out_filenames->end(),
                                             [&](const char *filename) {
        return std::any_of(file_set.ignore.begin(), file_set.ignore.end(),
                           [&](const char *pattern) { return MatchPathSpec(filename, pattern); });
    }) - out_filenames->begin());

    out_guard.Disable();
    return true;
}

static bool CheckTargetName(Span<const char> name)
{
    const auto test_char = [](char c) { return IsAsciiAlphaOrDigit(c) || c == '_' || c == '-'; };

    if (!name.len) {
        LogError("Target name cannot be empty");
        return false;
    }
    if (!std::all_of(name.begin(), name.end(), test_char)) {
        LogError("Target name must only contain alphanumeric, '_' or '-' characters");
        return false;
    }

    return true;
}

static bool ParseFeatureString(Span<const char> str, uint32_t *out_enable, uint32_t *out_disable)
{
    bool valid = true;

    while (str.len) {
        Span<const char> part = TrimStr(SplitStrAny(str, " ,", &str));

        bool enable;
        if (part.len && part[0] == '-') {
            part = part.Take(1, part.len - 1);
            enable = false;
        } else if (part.len && part[0] == '+') {
            part = part.Take(1, part.len - 1);
            enable = true;
        } else {
            enable = true;
        }

        if (part.len) {
            uint32_t *dest = enable ? out_enable : out_disable;

            if (!OptionToFlagI(CompileFeatureOptions, part, dest)) {
                LogError("Unknown target feature '%1'", part);
                valid = false;
            }
        }
    }

    return valid;
}

bool TargetSetBuilder::LoadIni(StreamReader *st)
{
    RG_DEFER_NC(out_guard, len = set.targets.len) { set.targets.RemoveFrom(len); };

    IniParser ini(st);
    ini.PushLogFilter();
    RG_DEFER { PopLogFilter(); };

    bool valid = true;
    {
        IniProperty prop;
        while (ini.Next(&prop)) {
            if (!prop.section.len) {
                LogError("Property is outside section");
                return false;
            }
            valid &= CheckTargetName(prop.section);

            TargetConfig target_config = {};

            target_config.name = DuplicateString(prop.section, &set.str_alloc).ptr;
            if (set.targets_map.Find(target_config.name)) {
                LogError("Duplicate target name '%1'", target_config.name);
                valid = false;
            }
            target_config.type = TargetType::Executable;
            target_config.platforms = ParseSupportedPlatforms("Desktop Emscripten");
            RG_ASSERT(target_config.platforms);
            target_config.title = target_config.name;
            target_config.version_tag = target_config.name;

            // Type property must be specified first
            if (prop.key == "Type") {
                if (OptionToEnumI(TargetTypeNames, prop.value, &target_config.type)) {
                    target_config.enable_by_default = (target_config.type == TargetType::Executable);
                } else if (prop.value == "ExternalLibrary") { // Compatibility
                    target_config.type = TargetType::Library;
                } else {
                    LogError("Unknown target type '%1'", prop.value);
                    valid = false;
                }
            } else {
                LogError("Property 'Type' must be specified first");
                valid = false;
            }

            while (ini.NextInSection(&prop)) {
                // These properties do not support platform suffixes
                if (prop.key == "Type") {
                    LogError("Target type cannot be changed");
                    valid = false;
                } else if (prop.key == "Platforms" || prop.key == "Hosts") {
                    target_config.platforms = ParseSupportedPlatforms(prop.value);
                    valid &= !!target_config.platforms;
                } else {
                    Span<const char> suffix;
                    prop.key = SplitStr(prop.key, '@', &suffix);

                    if (suffix.len) {
                        bool use_property = false;
                        valid &= MatchPropertySuffix(suffix, &use_property);

                        if (!use_property)
                            continue;
                    }

                    if (prop.key == "EnableByDefault") {
                        valid &= ParseBool(prop.value, &target_config.enable_by_default);
                    } else if (prop.key == "Title") {
                        target_config.title = DuplicateString(prop.value, &set.str_alloc).ptr;
                    } else if (prop.key == "VersionTag") {
                        target_config.version_tag = DuplicateString(prop.value, &set.str_alloc).ptr;
                    } else if (prop.key == "IconFile") {
                        target_config.icon_filename = DuplicateString(prop.value, &set.str_alloc).ptr;
                    } else if (prop.key == "SourceDirectory") {
                        AppendNormalizedPath(prop.value, &set.str_alloc, &target_config.src_file_set.directories);
                    } else if (prop.key == "SourceDirectoryInc") {
                        HeapArray<const char *> *directories = &target_config.src_file_set.directories;
                        Size start_len = directories->len;

                        AppendNormalizedPath(prop.value, &set.str_alloc, directories);

                        Span<const char *> copy = directories->Take(start_len, directories->len - start_len);
                        target_config.include_directories.Append(copy);
                    } else if (prop.key == "SourceDirectoryRec") {
                        AppendNormalizedPath(prop.value, &set.str_alloc, &target_config.src_file_set.directories_rec);
                    } else if (prop.key == "SourceFile") {
                        Span<const char> path = SplitStr(prop.value, ' ', &prop.value);

                        const char *filename = NormalizePath(path, &set.str_alloc).ptr;
                        target_config.src_file_set.filenames.Append(filename);

                        SourceFeatures features = {};
                        valid &= ParseFeatureString(prop.value, &features.enable_features, &features.disable_features);

                        if (features.enable_features || features.disable_features) {
                            target_config.src_features.TrySet(filename, features);
                        }
                    } else if (prop.key == "SourceIgnore") {
                        while (prop.value.len) {
                            Span<const char> part = TrimStr(SplitStrAny(prop.value, " ,", &prop.value));

                            if (part.len) {
                                const char *copy = DuplicateString(part, &set.str_alloc).ptr;
                                target_config.src_file_set.ignore.Append(copy);
                            }
                        }
                    } else if (prop.key == "ImportFrom") {
                        while (prop.value.len) {
                            Span<const char> part = TrimStr(SplitStrAny(prop.value, " ,", &prop.value));

                            if (part.len) {
                                const char *copy = DuplicateString(part, &set.str_alloc).ptr;
                                target_config.imports.Append(copy);
                            }
                        }
                    } else if (prop.key == "IncludeDirectory") {
                        AppendNormalizedPath(prop.value, &set.str_alloc, &target_config.include_directories);
                    } else if (prop.key == "ForceInclude") {
                        AppendNormalizedPath(prop.value, &set.str_alloc, &target_config.include_files);
                    } else if (prop.key == "PrecompileC") {
                        Span<const char> path = SplitStr(prop.value, ' ', &prop.value);

                        target_config.c_pch_filename = NormalizePath(path, &set.str_alloc).ptr;

                        SourceFeatures features = {};
                        valid &= ParseFeatureString(prop.value, &features.enable_features, &features.disable_features);

                        if (features.enable_features || features.disable_features) {
                            target_config.src_features.TrySet(target_config.c_pch_filename, features);
                        }
                    } else if (prop.key == "PrecompileCxx" || prop.key == "PrecompileCXX") {
                        Span<const char> path = SplitStr(prop.value, ' ', &prop.value);

                        target_config.cxx_pch_filename = NormalizePath(path, &set.str_alloc).ptr;

                        SourceFeatures features = {};
                        valid &= ParseFeatureString(prop.value, &features.enable_features, &features.disable_features);

                        if (features.enable_features || features.disable_features) {
                            target_config.src_features.TrySet(target_config.cxx_pch_filename, features);
                        }
                    } else if (prop.key == "Definitions") {
                        AppendListValues(prop.value, &set.str_alloc, &target_config.definitions);
                    } else if (prop.key == "ExportDefinitions") {
                        AppendListValues(prop.value, &set.str_alloc, &target_config.export_definitions);
                    } else if (prop.key == "Features") {
                        valid &= ParseFeatureString(prop.value, &target_config.enable_features, &target_config.disable_features);
                    } else if (prop.key == "Link") {
                        AppendListValues(prop.value, &set.str_alloc, &target_config.libraries);
                    } else if (prop.key == "QtComponents") {
                        AppendListValues(prop.value, &set.str_alloc, &target_config.qt_components);
                    } else if (prop.key == "QtVersion") {
                        target_config.qt_version = ParseVersionString(prop.value, 3);
                        valid &= (target_config.qt_version >= 0);
                    } else if (prop.key == "BundleOptions") {
                        target_config.bundle_options = DuplicateString(prop.value, &set.str_alloc).ptr;
                    } else if (prop.key == "AssetDirectory") {
                        AppendNormalizedPath(prop.value, &set.str_alloc, &target_config.pack_file_set.directories);
                    } else if (prop.key == "AssetDirectoryRec") {
                        AppendNormalizedPath(prop.value, &set.str_alloc, &target_config.pack_file_set.directories_rec);
                    } else if (prop.key == "AssetFile") {
                        AppendNormalizedPath(prop.value, &set.str_alloc, &target_config.pack_file_set.filenames);
                    } else if (prop.key == "AssetIgnore") {
                        while (prop.value.len) {
                            Span<const char> part = TrimStr(SplitStrAny(prop.value, " ,", &prop.value));

                            if (part.len) {
                                const char *copy = DuplicateString(part, &set.str_alloc).ptr;
                                target_config.pack_file_set.ignore.Append(copy);
                            }
                        }
                    } else if (prop.key == "PackOptions" || prop.key == "AssetOptions") {
                        target_config.pack_options = DuplicateString(prop.value, &set.str_alloc).ptr;
                    } else {
                        LogError("Unknown attribute '%1'", prop.key);
                        valid = false;
                    }
                }
            }

            valid &= !!CreateTarget(&target_config);
        }
    }
    if (!ini.IsValid() || !valid)
        return false;

    out_guard.Disable();
    return true;
}

bool TargetSetBuilder::LoadFiles(Span<const char *const> filenames)
{
    bool success = true;

    for (const char *filename: filenames) {
        CompressionType compression_type;
        Span<const char> extension = GetPathExtension(filename, &compression_type);

        bool (TargetSetBuilder::*load_func)(StreamReader *st);
        if (extension == ".ini") {
            load_func = &TargetSetBuilder::LoadIni;
        } else {
            LogError("Cannot load config from file '%1' with unknown extension '%2'",
                     filename, extension);
            success = false;
            continue;
        }

        StreamReader st(filename, compression_type);
        if (!st.IsValid()) {
            success = false;
            continue;
        }
        success &= (this->*load_func)(&st);
    }

    return success;
}

// We steal stuff from TargetConfig so it's not reusable after that
const TargetInfo *TargetSetBuilder::CreateTarget(TargetConfig *target_config)
{
    RG_DEFER_NC(out_guard, len = set.targets.len) { set.targets.RemoveFrom(len); };

    // Heavy type, so create it directly in HeapArray
    TargetInfo *target = set.targets.AppendDefault();

    // Copy/steal simple values
    target->name = target_config->name;
    target->type = target_config->type;
    target->platforms = target_config->platforms;
    target->enable_by_default = target_config->enable_by_default;
    target->title = target_config->title;
    target->version_tag = target_config->version_tag;
    target->icon_filename = target_config->icon_filename;
    std::swap(target->definitions, target_config->definitions);
    std::swap(target->export_definitions, target_config->export_definitions);
    std::swap(target->include_directories, target_config->include_directories);
    std::swap(target->include_files, target_config->include_files);
    std::swap(target->libraries, target_config->libraries);
    std::swap(target->qt_components, target_config->qt_components);
    target->qt_version = target_config->qt_version;
    target->enable_features = target_config->enable_features;
    target->disable_features = target_config->disable_features;
    target->bundle_options = target_config->bundle_options;
    target->pack_options = target_config->pack_options;

    // Resolve imported targets
    {
        HashSet<const char *> handled_imports;

        for (const char *import_name: target_config->imports) {
            const TargetInfo *import = set.targets_map.FindValue(import_name, nullptr);

            if (!import) {
                LogError("Cannot import from unknown target '%1'", import_name);
                return nullptr;
            }
            if (import->type != TargetType::Library) {
                LogError("Cannot import non-library target '%1'", import->name);
                return nullptr;
            }

            for (const TargetInfo *import2: import->imports) {
                bool inserted;
                handled_imports.TrySet(import2->name, &inserted);

                if (inserted) {
                    target->imports.Append(import2);
                }
            }

            bool inserted;
            handled_imports.TrySet(import->name, &inserted);

            if (inserted) {
                target->imports.Append(import);
            }
        }

        for (const TargetInfo *import: target->imports) {
            target->definitions.Append(import->export_definitions);
            target->libraries.Append(import->libraries);
            target->pchs.Append(import->pchs);
            target->sources.Append(import->sources);
        }
    }

    // Gather direct target objects
    {
        HeapArray<const char *> src_filenames;
        if (!ResolveFileSet(target_config->src_file_set, &temp_alloc, &src_filenames))
            return nullptr;

        for (const char *src_filename: src_filenames) {
            const SourceFeatures *features = target_config->src_features.Find(src_filename);

            SourceType src_type;
            if (!DetermineSourceType(src_filename, &src_type))
                continue;

            const SourceFileInfo *src = CreateSource(target, src_filename, src_type, features);
            target->sources.Append(src);
        }
    }

    // PCH
    if (target_config->c_pch_filename) {
        const SourceFeatures *features = target_config->src_features.Find(target_config->c_pch_filename);

        target->c_pch_src = CreateSource(target, target_config->c_pch_filename, SourceType::C, features);
        target->pchs.Append(target->c_pch_src->filename);
    }
    if (target_config->cxx_pch_filename) {
        const SourceFeatures *features = target_config->src_features.Find(target_config->cxx_pch_filename);

        target->cxx_pch_src = CreateSource(target, target_config->cxx_pch_filename, SourceType::Cxx, features);
        target->pchs.Append(target->cxx_pch_src->filename);
    }

    // Deduplicate libraries
    {
        HashSet<const char *> handled;

        Size j = 0;
        for (Size i = 0; i < target->libraries.len; i++) {
            target->libraries[j] = target->libraries[i];

            bool inserted;
            handled.TrySet(target->libraries[i], &inserted);

            j += inserted;
        }
        target->libraries.len = j;
    }

    // Deduplicate PCHs
    {
        HashSet<const char *> handled;

        Size j = 0;
        for (Size i = 0; i < target->pchs.len; i++) {
            target->pchs[j] = target->pchs[i];

            bool inserted;
            handled.TrySet(target->pchs[i], &inserted);

            j += inserted;
        }
        target->pchs.len = j;
    }

    // Deduplicate sources
    {
        HashSet<const char *> handled;

        Size j = 0;
        for (Size i = 0; i < target->sources.len; i++) {
            target->sources[j] = target->sources[i];

            bool inserted;
            handled.TrySet(target->sources[i]->filename, &inserted);

            j += inserted;
        }
        target->sources.len = j;
    }

    // Gather asset filenames
    if (!ResolveFileSet(target_config->pack_file_set, &set.str_alloc, &target->pack_filenames))
        return nullptr;

    set.targets_map.Set(target);

    out_guard.Disable();
    return target;
}

const SourceFileInfo *TargetSetBuilder::CreateSource(const TargetInfo *target, const char *filename,
                                                     SourceType type, const SourceFeatures *features)
{
    bool inserted;
    SourceFileInfo **ptr = set.sources_map.TrySetDefault(filename, &inserted);

    if (inserted) {
        SourceFileInfo *src = set.sources.AppendDefault();

        src->target = target;
        src->filename = DuplicateString(filename, &set.str_alloc).ptr;
        src->type = type;

        if (features) {
            src->enable_features = features->enable_features;
            src->disable_features = features->disable_features;
        }

        *ptr = src;
    }

    return *ptr;
}

void TargetSetBuilder::Finish(TargetSet *out_set)
{
    std::swap(*out_set, set);
}

bool TargetSetBuilder::MatchPropertySuffix(Span<const char> str, bool *out_match)
{
    bool match = true;

    while (str.len) {
        Span<const char> test = SplitStr(str, '@', &str);

        if (!test.len)
            continue;

        // Architecture?
        {
            HostArchitecture architecture;
            if (OptionToEnumI(HostArchitectureNames, test, &architecture)) {
                match &= (architecture == this->architecture);
                continue;
            }
        }

        // Platform?
        {
            unsigned int platforms = ParseSupportedPlatforms(test);
            if (!platforms)
                return false;
            match &= !!(platforms & (1 << (int)platform));
        }
    }

    *out_match = match;
    return true;
}

unsigned int ParseSupportedPlatforms(Span<const char> str)
{
    unsigned int platforms = 0;

    Span<const char> remain = str;
    while (remain.len) {
        Span<const char> part = SplitStrAny(remain, ", ", &remain);

        if (TestStrI(part, "Win32")) {
            // Old name, supported for compatibility (easier bisect)
            platforms |= 1 << (int)HostPlatform::Windows;
            continue;
        }

        if (part.len) {
            for (Size i = 0; i < RG_LEN(HostPlatformNames); i++) {
                Span<const char> name = HostPlatformNames[i];

                while (name.len) {
                    if (StartsWith(name, part)) {
                        if (part.len == name.len || name[part.len] == '/') {
                            platforms |= 1u << i;
                            break;
                        }
                    }

                    SplitStr(name, '/', &name);
                }
            }
        }
    }
    if (!platforms) {
        LogError("Unknown platform or platform family '%1'", str);
    }

    return platforms;
}

int64_t ParseVersionString(Span<const char> str, int components)
{
    RG_ASSERT(components >= 0 && components < 6);

    int64_t version = 0;

    Span<const char> remain = str;

    while (remain.len && components) {
        int component = 0;
        if (!ParseInt(remain, &component, 0, &remain)) {
            LogError("Malformed version string '%1'", str);
            return -1;
        }

        version = (version * 1000) + component;
        components--;

        if (!remain.len || remain[0] != '.')
            break;
        remain.ptr++;
        remain.len--;
    }

    while (components) {
        version *= 1000;
        components--;
    }

    return version;
}

bool LoadTargetSet(Span<const char *const> filenames, HostPlatform platform, HostArchitecture architecture, TargetSet *out_set)
{
    TargetSetBuilder target_set_builder(platform, architecture);
    if (!target_set_builder.LoadFiles(filenames))
        return false;
    target_set_builder.Finish(out_set);

    return true;
}

}
