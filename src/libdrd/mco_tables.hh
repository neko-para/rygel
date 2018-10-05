// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "../common/kutil.hh"
#include "mco_common.hh"

enum class mco_TableType: uint32_t {
    UnknownTable,

    GhmDecisionTree,
    DiagnosisTable,
    ProcedureTable,
    ProcedureExtensionTable,
    GhmRootTable,
    SeverityTable,
    GhmToGhsTable,
    AuthorizationTable,
    SrcPairTable,
    PriceTablePublic,
    PriceTablePrivate,
    GhsMinorationTable
};
static const char *const mco_TableTypeNames[] = {
    "Unknown Table",

    "GHM Decision Tree",
    "Diagnosis Table",
    "Procedure Table",
    "Procedure Extension Table",
    "GHM Root Table",
    "Severity Table",
    "GHM To GHS Table",
    "Authorization Table",
    "SRC Pair Table",
    "Price Table (public)",
    "Price Table (private)",
    "GHS Minoration Table"
};

struct mco_TableInfo {
    struct Section {
        Size raw_offset;
        Size raw_len;
        Size values_count;
        Size value_len;
    };

    const char *filename;
    Date build_date;
    uint16_t version[2];
    Date limit_dates[2];

    char raw_type[9];
    mco_TableType type;

    LocalArray<Section, 16> sections;
};

struct mco_GhmDecisionNode {
    enum class Type {
        Test,
        Ghm
    };

    Type type;
    union {
        struct {
            uint8_t function; // Switch to dedicated enum
            uint8_t params[2];
            Size children_count;
            Size children_idx;
        } test;

        struct {
            mco_GhmCode ghm;
            int16_t error;
        } ghm;
    } u;
};

struct mco_DiagnosisInfo {
    enum class Flag {
        SexDifference = 1
    };

    DiagnosisCode diag;

    uint16_t flags;
    struct Attributes {
        uint8_t raw[37];

        int8_t cmd;
        int8_t jump;

        int8_t severity;
        int8_t cma_minimum_age;
        int8_t cma_maximum_age;
    } attributes[2];
    uint16_t warnings;

    uint16_t exclusion_set_idx;
    ListMask cma_exclusion_mask;

    const Attributes &Attributes(int8_t sex) const
    {
        DebugAssert(sex == 1 || sex == 2);
        return attributes[sex - 1];
    }

    HASH_TABLE_HANDLER(mco_DiagnosisInfo, diag);
};

struct mco_ExclusionInfo {
    uint8_t raw[256];
};

struct mco_ProcedureInfo {
    ProcedureCode proc;
    int8_t phase;
    uint8_t activities;

    Date limit_dates[2];
    uint8_t bytes[54];
    uint8_t extensions;

    static int MaskToDec(int8_t value)
    {
        int dec = 0;
        for (int i = 0; value; i++) {
            if (value & 0x1) {
                dec = (dec * 10) + i;
            }
            value >>= 1;
        }
        return dec;
    }

    int ActivitiesToDec() const { return MaskToDec(activities); }
    int ExtensionsToDec() const { return MaskToDec(extensions); }

    HASH_TABLE_HANDLER(mco_ProcedureInfo, proc);
};

struct mco_ProcedureExtensionInfo {
    ProcedureCode proc;
    int8_t phase;

    int8_t extension;
};

template <Size N>
struct mco_ValueRangeCell {
    struct {
        int min;
        int max;
    } limits[N];
    int value;

    bool Test(Size idx, int value) const
    {
        DebugAssert(idx < N);
        return (value >= limits[idx].min && value < limits[idx].max);
    }
};

struct mco_GhmRootInfo {
    mco_GhmRootCode ghm_root;

    int8_t confirm_duration_treshold;

    bool allow_ambulatory;
    int8_t short_duration_treshold;

    int8_t young_severity_limit;
    int8_t young_age_treshold;
    int8_t old_severity_limit;
    int8_t old_age_treshold;

    int8_t childbirth_severity_list;

    ListMask cma_exclusion_mask;

    HASH_TABLE_HANDLER(mco_GhmRootInfo, ghm_root);
};

struct mco_GhmToGhsInfo {
    mco_GhmCode ghm;
    mco_GhsCode ghs[2]; // 0 for public, 1 for private

    int8_t bed_authorization;
    int8_t unit_authorization;
    int8_t minimal_duration;

    int8_t minimal_age;

    ListMask main_diagnosis_mask;
    ListMask diagnosis_mask;
    LocalArray<ListMask, 4> procedure_masks;

    mco_GhsCode Ghs(Sector sector) const
    {
        StaticAssert((int)Sector::Public == 0);
        return ghs[(int)sector];
    }

    HASH_TABLE_HANDLER(mco_GhmToGhsInfo, ghm);
    HASH_TABLE_HANDLER_N(GhmRootHandler, mco_GhmToGhsInfo, ghm.Root());
};

struct mco_GhsPriceInfo {
    enum class Flag {
        ExbOnce = 1 << 0,
        Minoration = 1 << 1
    };

    mco_GhsCode ghs;

    int32_t ghs_cents;
    int16_t exh_treshold;
    int16_t exb_treshold;
    int32_t exh_cents;
    int32_t exb_cents;
    uint16_t flags;

    HASH_TABLE_HANDLER(mco_GhsPriceInfo, ghs);
};

enum class mco_AuthorizationScope: int8_t {
    Facility,
    Unit,
    Bed
};
static const char *const mco_AuthorizationScopeNames[] = {
    "Facility",
    "Unit",
    "Bed"
};
struct mco_AuthorizationInfo {
    union {
        int16_t value;
        struct {
            mco_AuthorizationScope scope;
            int8_t code;
        } st;
    } type;
    int8_t function;

    HASH_TABLE_HANDLER(mco_AuthorizationInfo, type.value);
};

struct mco_SrcPair {
    DiagnosisCode diag;
    ProcedureCode proc;
};

Date mco_ConvertDate1980(uint16_t days);

bool mco_ParseTableHeaders(Span<const uint8_t> file_data, const char *filename,
                           Allocator *str_alloc, HeapArray<mco_TableInfo> *out_tables);

bool mco_ParseGhmDecisionTree(const uint8_t *file_data, const mco_TableInfo &table,
                              HeapArray<mco_GhmDecisionNode> *out_nodes);
bool mco_ParseDiagnosisTable(const uint8_t *file_data, const mco_TableInfo &table,
                             HeapArray<mco_DiagnosisInfo> *out_diags);
bool mco_ParseExclusionTable(const uint8_t *file_data, const mco_TableInfo &table,
                             HeapArray<mco_ExclusionInfo> *out_exclusions);
bool mco_ParseProcedureTable(const uint8_t *file_data, const mco_TableInfo &table,
                             HeapArray<mco_ProcedureInfo> *out_procs);
bool mco_ParseProcedureExtensionTable(const uint8_t *file_data, const mco_TableInfo &table,
                                      HeapArray<mco_ProcedureExtensionInfo> *out_procedures);
bool mco_ParseGhmRootTable(const uint8_t *file_data, const mco_TableInfo &table,
                           HeapArray<mco_GhmRootInfo> *out_ghm_roots);
bool mco_ParseSeverityTable(const uint8_t *file_data, const mco_TableInfo &table, int section_idx,
                            HeapArray<mco_ValueRangeCell<2>> *out_cells);

bool mco_ParseGhmToGhsTable(const uint8_t *file_data, const mco_TableInfo &table,
                            HeapArray<mco_GhmToGhsInfo> *out_nodes);
bool mco_ParseAuthorizationTable(const uint8_t *file_data, const mco_TableInfo &table,
                                 HeapArray<mco_AuthorizationInfo> *out_auths);
bool mco_ParseSrcPairTable(const uint8_t *file_data, const mco_TableInfo &table, int section_idx,
                           HeapArray<mco_SrcPair> *out_pairs);
bool mco_ParseGhsMinorationTable(const uint8_t *file_data, const mco_TableInfo &table,
                                 HeapArray<mco_GhsCode> *out_minored_ghs);

bool mco_ParsePriceTable(Span<const uint8_t> file_data, const mco_TableInfo &table,
                         double *out_ghs_coefficient,
                         HeapArray<mco_GhsPriceInfo> *out_ghs_prices,
                         mco_SupplementCounters<int32_t> *out_supplement_prices);

struct mco_TableIndex {
    Date limit_dates[2];
    bool valid;

    const mco_TableInfo *tables[ARRAY_SIZE(mco_TableTypeNames)];
    uint32_t changed_tables;

    Span<const mco_GhmDecisionNode> ghm_nodes;
    Span<const mco_DiagnosisInfo> diagnoses;
    Span<const mco_ExclusionInfo> exclusions;
    Span<const mco_ProcedureInfo> procedures;
    Span<const mco_GhmRootInfo> ghm_roots;
    Span<const mco_ValueRangeCell<2>> gnn_cells;
    Span<const mco_ValueRangeCell<2>> cma_cells[3];

    Span<const mco_GhmToGhsInfo> ghs;
    Span<const mco_AuthorizationInfo> authorizations;
    Span<const mco_SrcPair> src_pairs[2];

    double ghs_coefficient[2];
    Span<const mco_GhsPriceInfo> ghs_prices[2];
    mco_SupplementCounters<int32_t> supplement_prices[2];

    const HashTable<DiagnosisCode, const mco_DiagnosisInfo *> *diagnoses_map;
    const HashTable<ProcedureCode, const mco_ProcedureInfo *> *procedures_map;
    const HashTable<mco_GhmRootCode, const mco_GhmRootInfo *> *ghm_roots_map;

    const HashTable<mco_GhmCode, const mco_GhmToGhsInfo *> *ghm_to_ghs_map;
    const HashTable<mco_GhmRootCode, const mco_GhmToGhsInfo *, mco_GhmToGhsInfo::GhmRootHandler> *ghm_root_to_ghs_map;
    const HashTable<int16_t, const mco_AuthorizationInfo *> *authorizations_map;

    const HashTable<mco_GhsCode, const mco_GhsPriceInfo *> *ghs_prices_map[2];

    const mco_DiagnosisInfo *FindDiagnosis(DiagnosisCode diag) const;
    Span<const mco_ProcedureInfo> FindProcedure(ProcedureCode proc) const;
    const mco_ProcedureInfo *FindProcedure(ProcedureCode proc, int8_t phase, Date date) const;
    const mco_GhmRootInfo *FindGhmRoot(mco_GhmRootCode ghm_root) const;

    Span<const mco_GhmToGhsInfo> FindCompatibleGhs(mco_GhmCode ghm) const;
    Span<const mco_GhmToGhsInfo> FindCompatibleGhs(mco_GhmRootCode ghm_root) const;
    const mco_AuthorizationInfo *FindAuthorization(mco_AuthorizationScope scope, int8_t type) const;

    double GhsCoefficient(Sector sector) const;
    const mco_GhsPriceInfo *FindGhsPrice(mco_GhsCode ghs, Sector sector) const;
    const mco_SupplementCounters<int32_t> &SupplementPrices(Sector sector) const;
};

class mco_TableSet {
public:
    HeapArray<mco_TableInfo> tables;
    HeapArray<mco_TableIndex> indexes;

    struct {
        BlockQueue<HeapArray<mco_GhmDecisionNode>, 16> ghm_nodes;
        BlockQueue<HeapArray<mco_DiagnosisInfo>, 16> diagnoses;
        BlockQueue<HeapArray<mco_ExclusionInfo>, 16> exclusions;
        BlockQueue<HeapArray<mco_ProcedureInfo>, 16> procedures;
        BlockQueue<HeapArray<mco_GhmRootInfo>, 16> ghm_roots;
        BlockQueue<HeapArray<mco_ValueRangeCell<2>>, 16> gnn_cells;
        BlockQueue<HeapArray<mco_ValueRangeCell<2>>, 16> cma_cells[3];
        BlockQueue<HeapArray<mco_GhmToGhsInfo>, 16> ghs;
        BlockQueue<HeapArray<mco_AuthorizationInfo>, 16> authorizations;
        BlockQueue<HeapArray<mco_SrcPair>, 16> src_pairs[2];

        BlockQueue<HeapArray<mco_GhsPriceInfo>, 16> ghs_prices[2];
    } store;

    struct {
        BlockQueue<HashTable<DiagnosisCode, const mco_DiagnosisInfo *>, 16> diagnoses;
        BlockQueue<HashTable<ProcedureCode, const mco_ProcedureInfo *>, 16> procedures;
        BlockQueue<HashTable<mco_GhmRootCode, const mco_GhmRootInfo *>, 16> ghm_roots;
        BlockQueue<HashTable<mco_GhmCode, const mco_GhmToGhsInfo *>, 16> ghm_to_ghs;
        BlockQueue<HashTable<mco_GhmRootCode, const mco_GhmToGhsInfo *,
                               mco_GhmToGhsInfo::GhmRootHandler>, 16> ghm_root_to_ghs;
        BlockQueue<HashTable<int16_t, const mco_AuthorizationInfo *>, 16> authorizations;

        BlockQueue<HashTable<mco_GhsCode, const mco_GhsPriceInfo *>, 16> ghs_prices[2];
    } maps;

    LinkedAllocator str_alloc;

    const mco_TableIndex *FindIndex(Date date = {}) const;
    mco_TableIndex *FindIndex(Date date = {})
        { return (mco_TableIndex *)((const mco_TableSet *)this)->FindIndex(date); }
};

class mco_TableSetBuilder {
    struct TableLoadInfo {
        Size table_idx;
        Span<uint8_t> raw_data;
        Size prev_index_idx;
    };

    LinkedAllocator file_alloc;
    HeapArray<TableLoadInfo> table_loads;

    mco_TableSet set;

public:
    bool LoadTab(StreamReader &st);
    bool LoadPrices(StreamReader &st);
    bool LoadFiles(Span<const char *const> filenames);

    bool Finish(mco_TableSet *out_set);

private:
    bool CommitIndex(Date start_date, Date end_date, TableLoadInfo *current_tables[]);
    void HandleDependencies(mco_TableSetBuilder::TableLoadInfo *current_tables[],
                            Span<const std::pair<mco_TableType, mco_TableType>> pairs);
};

class mco_ListSpecifier {
public:
    enum class Table {
        Invalid,
        Diagnoses,
        Procedures
    };
    enum class Type {
        All,
        Mask,
        ReverseMask,
        Cmd,
        CmdJump
    };

    Table table;

    Type type = Type::All;
    union {
        struct {
            uint8_t offset;
            uint8_t mask;
            bool reverse;
        } mask;

        int8_t cmd;

        struct {
            int8_t cmd;
            int8_t jump;
        } cmd_jump;
    } u;

    mco_ListSpecifier(Table table = Table::Invalid) : table(table) {}

    static mco_ListSpecifier FromString(const char *spec_str);

    bool IsValid() const { return table != Table::Invalid; }

    bool Match(Span<const uint8_t> values) const
    {
        switch (type) {
            case Type::All: { return true; } break;

            case Type::Mask: {
                return LIKELY(u.mask.offset < values.len) &&
                       values[u.mask.offset] & u.mask.mask;
            } break;

            case Type::ReverseMask: {
                return LIKELY(u.mask.offset < values.len) &&
                       !(values[u.mask.offset] & u.mask.mask);
            } break;

            case Type::Cmd: {
                return values[0] == u.cmd;
            } break;

            case Type::CmdJump: {
                return values[0] == u.cmd_jump.cmd &&
                       values[1] == u.cmd_jump.jump;
            } break;
        }
        DebugAssert(false);
    }
};
