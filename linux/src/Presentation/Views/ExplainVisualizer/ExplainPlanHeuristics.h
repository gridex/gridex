#pragma once
// Pure-function heuristics over a parsed PlanDocument. Two outputs:
//   • Warnings — short, single-issue callouts shown inline (the InfoBar)
//     and as `pill` badges on offending tree nodes.
//   • Recommendations — actionable suggestions surfaced in the bottom
//     "Recommendations" card (ANALYZE, raise work_mem, add index...).
//
// Rules are intentionally simple + transparent. We bias toward false
// negatives (no nag) over false positives — every recommendation must
// be a thing a senior DBA would also say after one glance at the plan.

#include "ExplainPlanTypes.h"

#include <string>
#include <vector>

namespace gridex::explainviz
{
    enum class IssueSeverity { Info, Warn, Bad };

    struct Issue
    {
        std::wstring nodeId;          // PlanNode.id this issue points at
        std::wstring title;           // short summary (used in InfoBar header)
        std::wstring detail;          // longer body
        IssueSeverity severity = IssueSeverity::Warn;
    };

    struct Recommendation
    {
        std::wstring title;           // bold lead, e.g. "Create index on admissions(admit_date)"
        std::wstring rationale;       // single-sentence justification
        std::wstring previewSql;      // SQL to copy/preview when user clicks
        IssueSeverity severity = IssueSeverity::Warn;
    };

    class ExplainPlanHeuristics
    {
    public:
        // Walks the plan and returns the issues found. Sets level=Bad on
        // any node tagged with a Bad-severity issue (mutates in place so
        // the tree's heat coloring matches the InfoBar).
        static std::vector<Issue> analyze(PlanDocument& doc);

        // Build recommendation cards from the same pass. Caller can run
        // this after analyze() so the same data is reused.
        static std::vector<Recommendation> recommend(const PlanDocument& doc,
                                                      const std::vector<Issue>& issues);
    };
}
