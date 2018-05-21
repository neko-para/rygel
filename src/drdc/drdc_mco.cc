// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "../libdrd/libdrd.hh"
#include "drdc_mco_dump.hh"

struct ListSpecifier {
    enum class Table {
        Diagnoses,
        Procedures
    };
    enum class Type {
        Mask,
        ReverseMask,
        CmdJump
    };

    bool valid;

    Table table;
    Type type;
    union {
        struct {
            uint8_t offset;
            uint8_t mask;
            bool reverse;
        } mask;

        struct {
            uint8_t cmd;
            uint8_t jump;
        } cmd_jump;
    } u;

    static ListSpecifier FromString(const char *spec_str)
    {
        ListSpecifier spec = {};

        if (!spec_str[0] || !spec_str[1])
            goto error;

        switch (spec_str[0]) {
            case 'd': case 'D': { spec.table = ListSpecifier::Table::Diagnoses; } break;
            case 'a': case 'A': { spec.table = ListSpecifier::Table::Procedures; } break;

            default:
                goto error;
        }

        switch (spec_str[1]) {
            case '$': {
                const char *mask_str = spec_str + 2;
                if (mask_str[0] == '~') {
                    spec.type = ListSpecifier::Type::ReverseMask;
                    mask_str++;
                } else {
                    spec.type = ListSpecifier::Type::Mask;
                }
                if (sscanf(mask_str, "%" SCNu8 ".%" SCNu8,
                           &spec.u.mask.offset, &spec.u.mask.mask) != 2)
                    goto error;
            } break;

            case '-': {
                spec.type = ListSpecifier::Type::CmdJump;
                if (sscanf(spec_str + 2, "%02" SCNu8 "%02" SCNu8,
                           &spec.u.cmd_jump.cmd, &spec.u.cmd_jump.jump) != 2)
                    goto error;
            } break;

            default:
                goto error;
        }

        spec.valid = true;
        return spec;

error:
        LogError("Malformed list specifier '%1'", spec_str);
        return spec;
    }

    bool IsValid() const { return valid; }

    bool Match(Span<const uint8_t> values) const
    {
        switch (type) {
            case Type::Mask: {
                return LIKELY(u.mask.offset < values.len) &&
                       values[u.mask.offset] & u.mask.mask;
            } break;

            case Type::ReverseMask: {
                return LIKELY(u.mask.offset < values.len) &&
                       !(values[u.mask.offset] & u.mask.mask);
            } break;

            case Type::CmdJump: {
                return values[0] == u.cmd_jump.cmd &&
                       values[1] == u.cmd_jump.jump;
            } break;
        }
        DebugAssert(false);
    }
};

static void PrintSummary(const mco_Summary &summary)
{
    PrintLn("  Results: %1", summary.results_count);
    PrintLn("  Stays: %1", summary.stays_count);
    PrintLn("  Failures: %1", summary.failures_count);
    PrintLn();
    PrintLn("  GHS: %1 €", FmtDouble((double)summary.ghs_cents / 100.0, 2));
    PrintLn("  GHS-EXB+EXH: %1 €", FmtDouble((double)summary.price_cents / 100.0, 2));
    PrintLn("  Supplements:");
    for (Size i = 0; i < ARRAY_SIZE(mco_SupplementTypeNames); i++) {
        PrintLn("    %1: %2 € [%3]",
                mco_SupplementTypeNames[i],
                FmtDouble((double)summary.supplement_cents.values[i] / 100.0, 2),
                summary.supplement_days.values[i]);
    }
    PrintLn("  Total: %1 €", FmtDouble((double)summary.total_cents / 100.0, 2));
    PrintLn();
};

static void ExportResults(Span<const mco_Result> results, Span<const mco_Result> mono_results,
                          bool verbose)
{
    const auto ExportResult = [&](int depth, const mco_Result &result) {
        FmtArg padding = FmtArg("").Pad(-2 * depth);

        PrintLn("    %1%2 [%3 -- %4] = GHM %5 [%6] / GHS %7",
                padding, result.stays[0].bill_id, result.duration,
                result.stays[result.stays.len - 1].exit.date, result.ghm, result.main_error,
                result.ghs);

        if (verbose) {
            PrintLn("      %1GHS: %2 € [coefficient = %3]",
                    padding, FmtDouble((double)result.ghs_pricing.ghs_cents / 100.0, 2),
                    FmtDouble(result.ghs_pricing.ghs_coefficient, 4));
            PrintLn("      %1GHS-EXB+EXH: %2 € [%3]",
                    padding, FmtDouble((double)result.ghs_pricing.price_cents / 100.0, 2),
                    result.ghs_pricing.exb_exh);
            if (result.total_cents > result.ghs_pricing.price_cents) {
                PrintLn("      %1Supplements:", padding);
                for (Size j = 0; j < ARRAY_SIZE(mco_SupplementTypeNames); j++) {
                    if (result.supplement_cents.values[j]) {
                        PrintLn("        %1%2: %3 € [%4]", padding, mco_SupplementTypeNames[j],
                                FmtDouble((double)result.supplement_cents.values[j] / 100.0, 2),
                                result.supplement_days.values[j]);
                    }
                }
            }
            PrintLn("      %1Total: %2 €",
                    padding, FmtDouble((double)result.total_cents / 100.0, 2));
            PrintLn();
        }
    };

    PrintLn("  Details:");
    Size j = 0;
    for (const mco_Result &result: results) {
        ExportResult(0, result);

        if (mono_results.len && result.stays.len > 1) {
            for (Size k = j; k < j + result.stays.len; k++) {
                const mco_Result &mono_result = mono_results[k];
                DebugAssert(mono_result.stays[0].bill_id == result.stays[0].bill_id);
                ExportResult(1, mono_result);
            }
            j += result.stays.len;
        } else {
            j++;
        }
    }
    PrintLn();
}

bool RunMcoClassify(Span<const char *> arguments)
{
    static const auto PrintUsage = [](FILE *fp) {
        PrintLn(fp, R"(Usage: drdc mco_classify [options] stay_file ...
)");
        PrintLn(fp, mco_options_usage);
        PrintLn(fp, R"(
Classify options:
    -m, --mono                   Compute mono-stay results (same as -fmono)
    -f, --flag <flags>           Classifier flags (see below)

    -d, --dispense <mode>        Run dispensation algorithm (see below)

    -v, --verbose                Show more classification details (cumulative)

        --test                   Enable testing against GenRSA values
        --torture [N]            Run classifier N times
                                 (default = 10)

Classifier flags:)");
        for (const OptionDesc &desc: mco_ClassifyFlagOptions) {
            PrintLn(fp, "    %1  %2", FmtArg(desc.name).Pad(27), desc.help);
        }
        PrintLn(fp, R"(
Dispensation modes:)");
        for (const OptionDesc &desc: mco_DispenseModeOptions) {
            PrintLn(fp, "    %1  Algorithm %2", FmtArg(desc.name).Pad(27), desc.help);
        }
    };

    OptionParser opt_parser(arguments);

    HeapArray<const char *> filenames;
    unsigned int flags = 0;
    int dispense_mode = -1;
    int verbosity = 0;
    bool test = false;
    int torture = 1;
    {
        const char *opt;
        while ((opt = opt_parser.Next())) {
            if (TestOption(opt, "--help")) {
                PrintUsage(stdout);
                return true;
            } else if (TestOption(opt, "-m", "--mono")) {
                flags |= (int)mco_ClassifyFlag::MonoResults;
            } else if (TestOption(opt, "-f", "--flag")) {
                const char *flags_str = opt_parser.RequireValue();
                if (!flags_str)
                    return false;

                while (flags_str[0]) {
                    Span<const char> flag = TrimStr(SplitStr(flags_str, ',', &flags_str), " ");
                    const OptionDesc *desc = std::find_if(std::begin(mco_ClassifyFlagOptions),
                                                          std::end(mco_ClassifyFlagOptions),
                                                          [&](const OptionDesc &desc) { return TestStr(desc.name, flag); });
                    if (desc == std::end(mco_ClassifyFlagOptions)) {
                        LogError("Unknown classifier flag '%1'", flag);
                        return false;
                    }
                    flags |= 1u << (desc - mco_ClassifyFlagOptions);
                }
            } else if (TestOption(opt, "-d", "--dispense")) {
                const char *mode_str = opt_parser.RequireValue();
                if (!mode_str)
                    return false;

                const OptionDesc *desc = std::find_if(std::begin(mco_DispenseModeOptions),
                                                      std::end(mco_DispenseModeOptions),
                                                      [&](const OptionDesc &desc) { return TestStr(desc.name, mode_str); });
                if (desc == std::end(mco_DispenseModeOptions)) {
                    LogError("Unknown dispensation mode '%1'", mode_str);
                    return false;
                }
                dispense_mode = (int)(desc - mco_DispenseModeOptions);
            } else if (TestOption(opt, "-v", "--verbose")) {
                verbosity++;
            } else if (TestOption(opt, "--test")) {
                test = true;
            } else if (TestOption(opt, "--torture")) {
                if (!opt_parser.RequireValue(PrintUsage))
                    return false;
                if (!ParseDec(opt_parser.current_value, &torture))
                    return false;
            } else if (!mco_HandleMainOption(opt_parser, PrintUsage)) {
                return false;
            }
        }

        opt_parser.ConsumeNonOptions(&filenames);
        if (!filenames.len) {
            LogError("No filename provided");
            PrintUsage(stderr);
            return false;
        }
    }
    if (dispense_mode >= 0) {
        flags |= (int)mco_ClassifyFlag::MonoResults;
    }

    const mco_TableSet *table_set = mco_GetMainTableSet();
    if (!table_set || !table_set->indexes.len)
        return false;
    const mco_AuthorizationSet *authorization_set = mco_GetMainAuthorizationSet();
    if (!authorization_set)
        return false;

    struct ClassifySet {
        mco_StaySet stay_set;
        HashTable<int32_t, mco_StayTest> tests;

        HeapArray<mco_Result> results;
        HeapArray<mco_Result> mono_results;
        mco_Summary summary;
    };
    HeapArray<ClassifySet> classify_sets;
    classify_sets.AppendDefault(filenames.len);

    Async async;
    for (Size i = 0; i < filenames.len; i++) {
        async.AddTask([&, i]() {
            ClassifySet *classify_set = &classify_sets[i];

            LogInfo("Load '%1'", filenames[i]);
            mco_StaySetBuilder stay_set_builder;
            if (!stay_set_builder.LoadFiles(filenames[i], test ? &classify_set->tests : nullptr))
                return false;
            if (!stay_set_builder.Finish(&classify_set->stay_set))
                return false;

            LogInfo("Classify '%1'", filenames[i]);
            for (int j = 0; j < torture; j++) {
                classify_set->results.RemoveFrom(0);
                classify_set->mono_results.RemoveFrom(0);
                mco_ClassifyParallel(*table_set, *authorization_set, classify_set->stay_set.stays,
                                     flags, &classify_set->results, &classify_set->mono_results);
            }

            LogInfo("Summarize '%1'", filenames[i]);
            mco_Summarize(classify_set->results, &classify_set->summary);

            if (0 && !verbosity && !test) {
                classify_set->stay_set = {};
                classify_set->results.Clear();
                classify_set->mono_results.Clear();
            }

            return true;
        });
    }
    if (!async.Sync())
        return false;

    LogInfo("Export");
    mco_Summary main_summary = {};
    for (Size i = 0; i < filenames.len; i++) {
        const ClassifySet &classify_set = classify_sets[i];

        PrintLn("%1:", filenames[i]);

        if (verbosity - test >= 1) {
            ExportResults(classify_set.results, classify_set.mono_results,
                          verbosity - test >= 2);
        }

        PrintSummary(classify_set.summary);
        main_summary += classify_set.summary;

        if (test) {
            PrintLn("  Tests:");

            Size tested_clusters = 0, failed_clusters = 0;
            Size tested_ghm = 0, failed_ghm = 0;
            Size tested_ghs = 0, failed_ghs = 0;
            Size tested_exb_exh = 0, failed_exb_exh = 0;
            for (const mco_Result &result: classify_set.results) {
                const mco_StayTest *stay_test = classify_set.tests.Find(result.stays[0].bill_id);
                if (!stay_test)
                    continue;

                if (stay_test->cluster_len) {
                    tested_clusters++;
                    if (result.stays.len != stay_test->cluster_len) {
                        failed_clusters++;
                        if (verbosity >= 1) {
                            PrintLn("    %1 [%2] has inadequate cluster %3 != %4",
                                    stay_test->bill_id, result.stays[0].exit.date,
                                    result.stays.len, stay_test->cluster_len);
                        }
                        continue;
                    }
                }

                if (stay_test->ghm.value) {
                    tested_ghm++;
                    if (stay_test->ghm != result.ghm) {
                        failed_ghm++;
                        if (verbosity >= 1) {
                            PrintLn("    %1 [%2] has inadequate GHM %3 [%4] != %5 [%6]",
                                    stay_test->bill_id, result.stays[0].exit.date,
                                    result.ghm, FmtArg(result.main_error).Pad(-3),
                                    stay_test->ghm, FmtArg(stay_test->error).Pad(-3));
                        }
                        continue;
                    }
                }

                if (stay_test->ghs.number) {
                    tested_ghs++;
                    tested_exb_exh++;

                    if (stay_test->ghs != result.ghs ||
                            stay_test->supplement_days != result.supplement_days) {
                        failed_ghs++;
                        if (verbosity >= 1) {
                            if (result.ghs != stay_test->ghs) {
                                PrintLn("    %1 [%2] has inadequate GHS %3 != %4",
                                        stay_test->bill_id, result.stays[0].exit.date,
                                        result.ghs, stay_test->ghs);
                            }
                            for (Size j = 0; j < ARRAY_SIZE(mco_SupplementTypeNames); j++) {
                                if (result.supplement_days.values[j] !=
                                        stay_test->supplement_days.values[j]) {
                                    PrintLn("    %1 [%2] has inadequate %3 %4 != %5",
                                            stay_test->bill_id, result.stays[0].exit.date,
                                            mco_SupplementTypeNames[j], result.supplement_days.values[j],
                                            stay_test->supplement_days.values[j]);
                                }
                            }
                        }
                        continue;
                    }

                    if (stay_test->exb_exh != result.ghs_pricing.exb_exh) {
                        failed_exb_exh++;
                        if (verbosity >= 1) {
                            PrintLn("    %1 [%2] has inadequate EXB/EXH %3 != %4",
                                    stay_test->bill_id, result.stays[0].exit.date,
                                    result.ghs_pricing.exb_exh, stay_test->exb_exh);
                        }
                    }
                }
            }
            if (verbosity >= 1 && (failed_clusters || failed_ghm || failed_ghs)) {
                PrintLn();
            }

            PrintLn("    Failed cluster tests: %1 / %2 (missing %3)",
                    failed_clusters, tested_clusters, classify_set.results.len - tested_clusters);
            PrintLn("    Failed GHM tests: %1 / %2 (missing %3)",
                    failed_ghm, tested_ghm, classify_set.results.len - tested_ghm);
            PrintLn("    Failed GHS (and supplements) tests: %1 / %2 (missing %3)",
                    failed_ghs, tested_ghs, classify_set.results.len - tested_ghs);
            PrintLn("    Failed EXB/EXH tests: %1 / %2 (missing %3)",
                    failed_exb_exh, tested_exb_exh, classify_set.results.len - tested_exb_exh);
            PrintLn();
        }
    }

    if (filenames.len > 1) {
        PrintLn("Global summary:");
        PrintSummary(main_summary);
    }

    if (dispense_mode >= 0) {
        PrintLn("Dispensation (%1):", mco_DispenseModeOptions[dispense_mode].help);

        HeapArray<mco_Due> dues;
        HashMap<UnitCode, Size> dues_map;
        for (const ClassifySet &classify_set: classify_sets) {
            mco_Dispense(classify_set.results, classify_set.mono_results,
                         (mco_DispenseMode)dispense_mode, &dues, &dues_map);
        }
        std::sort(dues.begin(), dues.end(), [](const mco_Due &due1, const mco_Due &due2) {
            return due1.unit.number < due2.unit.number;
        });

        for (const mco_Due &due: dues) {
            PrintLn("  %1 = %2 €", due.unit, FmtDouble((double)due.summary.price_cents / 100.0, 2));
        }
    }

    return true;
}

bool RunMcoDump(Span<const char *> arguments)
{
    static const auto PrintUsage = [](FILE *fp) {
        PrintLn(fp, R"(Usage: drdc mco_dump [options] [filename] ...
)");
        PrintLn(fp, mco_options_usage);
        PrintLn(fp, R"(
Dump options:
    -d, --dump                   Dump content of (readable) tables)");
    };

    OptionParser opt_parser(arguments);

    bool dump = false;
    HeapArray<const char *> filenames;
    {
        const char *opt;
        while ((opt = opt_parser.Next())) {
            if (TestOption(opt, "--help")) {
                PrintUsage(stdout);
                return true;
            } else if (TestOption(opt, "-d", "--dump")) {
                dump = true;
            } else if (!mco_HandleMainOption(opt_parser, PrintUsage)) {
                return false;
            }
        }

        opt_parser.ConsumeNonOptions(&filenames);
    }

    const mco_TableSet *table_set = mco_GetMainTableSet();
    if (!table_set || !table_set->indexes.len)
        return false;
    mco_DumpTableSetHeaders(*table_set);
    if (dump) {
        mco_DumpTableSetContent(*table_set);
    }

    return true;
}

bool RunMcoList(Span<const char *> arguments)
{
    static const auto PrintUsage = [](FILE *fp) {
        PrintLn(fp, R"(Usage: drdc mco_list [options] list_name ...
)");
        PrintLn(fp, mco_options_usage);
        PrintLn(fp, R"(
List options:
    -d, --date <date>            Use tables valid on specified date
                                 (default: most recent tables))");
    };

    OptionParser opt_parser(arguments);

    Date index_date = {};
    HeapArray<const char *> spec_strings;
    {
        const char *opt;
        while ((opt = opt_parser.Next())) {
            if (TestOption(opt, "--help")) {
                PrintUsage(stdout);
                return true;
            } else if (TestOption(opt_parser.current_option, "-d", "--date")) {
                if (!opt_parser.RequireValue(PrintUsage))
                    return false;
                index_date = Date::FromString(opt_parser.current_value);
                if (!index_date.value)
                    return false;
            } else if (!mco_HandleMainOption(opt_parser, PrintUsage)) {
                return false;
            }
        }

        opt_parser.ConsumeNonOptions(&spec_strings);
        if (!spec_strings.len) {
            LogError("No specifier provided");
            PrintUsage(stderr);
            return false;
        }
    }

    const mco_TableSet *table_set;
    const mco_TableIndex *index;
    {
        table_set = mco_GetMainTableSet();
        if (!table_set)
            return false;
        index = table_set->FindIndex(index_date);
        if (!index) {
            LogError("No table index available at '%1'", index_date);
            return false;
        }
    }

    for (const char *spec_str: spec_strings) {
        ListSpecifier spec = ListSpecifier::FromString(spec_str);
        if (!spec.IsValid())
            continue;

        PrintLn("%1:", spec_str);
        switch (spec.table) {
            case ListSpecifier::Table::Diagnoses: {
                for (const mco_DiagnosisInfo &diag: index->diagnoses) {
                    if (diag.flags & (int)mco_DiagnosisInfo::Flag::SexDifference) {
                        if (spec.Match(diag.Attributes(1).raw)) {
                            PrintLn("  %1 (male)", diag.diag);
                        }
                        if (spec.Match(diag.Attributes(2).raw)) {
                            PrintLn("  %1 (female)", diag.diag);
                        }
                    } else {
                        if (spec.Match(diag.Attributes(1).raw)) {
                            PrintLn("  %1", diag.diag);
                        }
                    }
                }
            } break;

            case ListSpecifier::Table::Procedures: {
                for (const mco_ProcedureInfo &proc: index->procedures) {
                    if (spec.Match(proc.bytes)) {
                        PrintLn("  %1", proc.proc);
                    }
                }
            } break;
        }
        PrintLn();
    }

    return true;
}

bool RunMcoMap(Span<const char *> arguments)
{
    static const auto PrintUsage = [](FILE *fp) {
        PrintLn(fp, R"(Usage: drdc mco_map [options]
)");
        PrintLn(fp, mco_options_usage);
        PrintLn(fp, R"(
Constraints options:
    -d, --date <date>            Use tables valid on specified date
                                 (default: most recent tables))");
    };

    OptionParser opt_parser(arguments);

    Date index_date = {};
    {
        const char *opt;
        while ((opt = opt_parser.Next())) {
            if (TestOption(opt, "--help")) {
                PrintUsage(stdout);
                return true;
            } else if (TestOption(opt_parser.current_option, "-d", "--date")) {
                if (!opt_parser.RequireValue(PrintUsage))
                    return false;
                index_date = Date::FromString(opt_parser.current_value);
                if (!index_date.value)
                    return false;
            } else if (!mco_HandleMainOption(opt_parser, PrintUsage)) {
                return false;
            }
        }
    }

    const mco_TableSet *table_set;
    const mco_TableIndex *index;
    {
        table_set = mco_GetMainTableSet();
        if (!table_set)
            return false;
        index = table_set->FindIndex(index_date);
        if (!index) {
            LogError("No table index available at '%1'", index_date);
            return false;
        }
    }

    LogInfo("Computing");
    HashTable<mco_GhmCode, mco_GhmConstraint> ghm_constraints;
    if (!mco_ComputeGhmConstraints(*index, &ghm_constraints))
        return false;

    LogInfo("Export");
    for (const mco_GhmToGhsInfo &ghm_to_ghs_info: index->ghs)  {
        const mco_GhmConstraint *constraint = ghm_constraints.Find(ghm_to_ghs_info.ghm);
        if (constraint) {
            PrintLn("Constraint for %1", ghm_to_ghs_info.ghm);
            PrintLn("  Duration = 0x%1",
                    FmtHex(constraint->durations).Pad0(-2 * SIZE(constraint->durations)));
            PrintLn("  Warnings = 0x%1",
                    FmtHex(constraint->warnings).Pad0(-2 * SIZE(constraint->warnings)));
        } else {
            PrintLn("%1 unreached!", ghm_to_ghs_info.ghm);
        }
    }

    return true;
}

bool RunMcoPack(Span<const char *> arguments)
{
    static const auto PrintUsage = [](FILE *fp) {
        PrintLn(fp, R"(Usage: drdc mco_pack [options] stay_file ... -O output_file
)");
        PrintLn(fp, mco_options_usage);
    };

    OptionParser opt_parser(arguments);

    HeapArray<const char *> filenames;
    const char *dest_filename = nullptr;
    {
        const char *opt;
        while ((opt = opt_parser.Next())) {
            if (TestOption(opt, "--help")) {
                PrintUsage(stdout);
                return true;
            } else if (TestOption(opt, "-O", "--output")) {
                if (!opt_parser.RequireValue(PrintUsage))
                    return false;
                dest_filename = opt_parser.current_value;
            } else if (!mco_HandleMainOption(opt_parser, PrintUsage)) {
                return false;
            }
        }

        opt_parser.ConsumeNonOptions(&filenames);
        if (!dest_filename) {
            LogError("A destination file must be provided (--output)");
            PrintUsage(stderr);
            return false;
        }
        if (!filenames.len) {
            LogError("No stay file provided");
            PrintUsage(stderr);
            return false;
        }
    }

    LogInfo("Loading stays");
    mco_StaySet stay_set;
    {
        mco_StaySetBuilder stay_set_builder;

        if (!stay_set_builder.LoadFiles(filenames))
            return false;
        if (!stay_set_builder.Finish(&stay_set))
            return false;
    }

    LogInfo("Packing stays");
    if (!stay_set.SavePack(dest_filename))
        return false;

    return true;
}

bool RunMcoShow(Span<const char *> arguments)
{
    static const auto PrintUsage = [](FILE *fp) {
        PrintLn(fp, R"(Usage: drdc mco_show [options] name ...
)");
        PrintLn(fp, mco_options_usage);
    };

    OptionParser opt_parser(arguments);

    Date index_date = {};
    HeapArray<const char *> names;
    {
        const char *opt;
        while ((opt = opt_parser.Next())) {
            if (TestOption(opt, "--help")) {
                PrintUsage(stdout);
                return true;
            } else if (TestOption(opt_parser.current_option, "-d", "--date")) {
                if (!opt_parser.RequireValue(PrintUsage))
                    return false;
                index_date = Date::FromString(opt_parser.current_value);
                if (!index_date.value)
                    return false;
            } else if (!mco_HandleMainOption(opt_parser, PrintUsage)) {
                return false;
            }
        }

        opt_parser.ConsumeNonOptions(&names);
        if (!names.len) {
            LogError("No element name provided");
            PrintUsage(stderr);
            return false;
        }
    }

    const mco_TableSet *table_set;
    const mco_TableIndex *index;
    {
        table_set = mco_GetMainTableSet();
        if (!table_set)
            return false;
        index = table_set->FindIndex(index_date);
        if (!index) {
            LogError("No table index available at '%1'", index_date);
            return false;
        }
    }

    for (const char *name: names) {
        // Diagnosis?
        {
            DiagnosisCode diag =
                DiagnosisCode::FromString(name, DEFAULT_PARSE_FLAGS & ~(int)ParseFlag::Log);
            if (diag.IsValid()) {
                const mco_DiagnosisInfo *diag_info = index->FindDiagnosis(diag);
                if (diag_info) {
                    mco_DumpDiagnosisTable(*diag_info, index->exclusions);
                    continue;
                }
            }
        }

        // Procedure?
        {
            ProcedureCode proc =
                ProcedureCode::FromString(name, DEFAULT_PARSE_FLAGS & ~(int)ParseFlag::Log);
            if (proc.IsValid()) {
                Span<const mco_ProcedureInfo> proc_info = index->FindProcedure(proc);
                if (proc_info.len) {
                    mco_DumpProcedureTable(proc_info);
                    continue;
                }
            }
        }

        // GHM root?
        {
            mco_GhmRootCode ghm_root =
                mco_GhmRootCode::FromString(name, DEFAULT_PARSE_FLAGS & ~(int)ParseFlag::Log);
            if (ghm_root.IsValid()) {
                const mco_GhmRootInfo *ghm_root_info = index->FindGhmRoot(ghm_root);
                if (ghm_root_info) {
                    mco_DumpGhmRootTable(*ghm_root_info);
                    PrintLn();

                    Span<const mco_GhmToGhsInfo> compatible_ghs = index->FindCompatibleGhs(ghm_root);
                    mco_DumpGhmToGhsTable(compatible_ghs);

                    continue;
                }
            }
        }

        // GHS?
        {
            mco_GhsCode ghs = mco_GhsCode::FromString(name, DEFAULT_PARSE_FLAGS & ~(int)ParseFlag::Log);
            if (ghs.IsValid()) {
                const mco_GhsPriceInfo *ghs_price_info = index->FindGhsPrice(ghs, Sector::Public);
                if (ghs_price_info) {
                    mco_DumpGhsPriceTable(*ghs_price_info);
                    PrintLn();
                    for (const mco_GhmToGhsInfo &ghm_to_ghs_info: index->ghs) {
                        if (ghm_to_ghs_info.Ghs(Sector::Public) != ghs)
                            continue;
                        mco_DumpGhmToGhsTable(ghm_to_ghs_info);
                    }
                    continue;
                }
            }
        }

        LogError("Unknown element '%1'", name);
    }

    return true;
}
