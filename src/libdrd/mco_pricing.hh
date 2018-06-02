// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "../common/kutil.hh"
#include "mco_classifier.hh"

struct mco_Pricing {
    Span<const mco_Stay> stays; // Not valid in totals / summaries

    Size results_count;
    Size stays_count;
    Size failures_count;
    int64_t duration;

    int64_t ghs_cents;
    double ghs_coefficient; // Not valid in totals / summaries
    int64_t price_cents;
    int32_t exb_exh;
    mco_SupplementCounters<int32_t> supplement_days;
    mco_SupplementCounters<int64_t> supplement_cents;
    int64_t total_cents;

    mco_Pricing &operator+=(const mco_Pricing &other)
    {
        results_count += other.results_count;
        stays_count += other.stays_count;
        failures_count += other.failures_count;
        duration += other.duration;

        ghs_cents += other.ghs_cents;
        price_cents += other.price_cents;
        supplement_days += other.supplement_days;
        supplement_cents += other.supplement_cents;
        total_cents += other.total_cents;

        return *this;
    }
    mco_Pricing operator+(const mco_Pricing &other)
    {
        mco_Pricing copy = *this;
        copy += other;
        return copy;
    }
};

enum class mco_DispenseMode {
    E,
    Ex,
    Ex2,
    J,
    ExJ,
    ExJ2
};
static const struct OptionDesc mco_DispenseModeOptions[] = {
    {"e", "E"},
    {"ex", "Ex"},
    {"ex2", "Ex'"},
    {"j", "J"},
    {"exj", "ExJ"},
    {"exj2", "Ex'J"}
};

int64_t mco_PriceGhs(const mco_GhsPriceInfo &price_info, double ghs_coefficient, int ghs_duration,
                     bool death, int64_t *out_ghs_cents = nullptr, int32_t *out_exb_exh = nullptr);

void mco_Price(const mco_Result &result, mco_Pricing *out_pricing);
void mco_Price(Span<const mco_Result> results, HeapArray<mco_Pricing> *out_pricings);
void mco_PriceTotal(Span<const mco_Result> results, mco_Pricing *out_pricing);

static inline void mco_Summarize(Span<const mco_Pricing> pricings, mco_Pricing *out_summary)
{
    for (const mco_Pricing &pricing: pricings) {
        *out_summary += pricing;
    }

    out_summary->stays = {};
    out_summary->ghs_coefficient = NAN;
}

void mco_Dispense(Span<const mco_Pricing> pricings, Span<const mco_Result> mono_results,
                  mco_DispenseMode dispense_mode, HeapArray<mco_Pricing> *out_pricings);
