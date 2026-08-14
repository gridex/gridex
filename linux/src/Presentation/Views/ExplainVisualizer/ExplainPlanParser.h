#pragma once
// Parser for Postgres `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)` output.
// We hand-roll a tiny JSON consumer (no nlohmann dependency to match the
// rest of the codebase) — only the subset of PG's plan schema we
// actually surface in the UI is read; unknown keys are skipped.

#include "ExplainPlanTypes.h"

#include <string>

namespace gridex::explainviz
{
    class ExplainPlanParser
    {
    public:
        // Parses the JSON string returned in the single text cell of an
        // `EXPLAIN (FORMAT JSON)` query. Returns true on success and fills
        // `out`. On failure returns false and writes a human-readable
        // explanation into `err`.
        static bool parse(const std::wstring& json,
                          PlanDocument& out,
                          std::wstring& err);

        // Convenience: re-walk the doc and recompute derived values
        // (NodeLevel, pctOfTotal, ownMs, generated ids). Called automatically
        // at the end of `parse`; exposed for tests and re-classification
        // after heuristics tweak any node-level fields.
        static void postProcess(PlanDocument& doc);
    };
}
