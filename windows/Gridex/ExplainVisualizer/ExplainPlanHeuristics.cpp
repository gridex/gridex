#include "pch.h"
// pch.h pulls in <windows.h> which defines `max`/`min` as macros — they
// hijack `std::max` / `std::min` calls below. NOMINMAX isn't set globally
// for this project, so undo the macros locally.
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#include "ExplainPlanHeuristics.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

namespace Gridex::ExplainViz
{
    // Threshold knobs — kept here for easy review.
    static constexpr double SEQ_SCAN_FILTER_RATIO = 0.5;   // ≥50% rows discarded → suggest index
    static constexpr int    SEQ_SCAN_MIN_REMOVED = 5000;   // small tables not worth indexing
    static constexpr double EST_VS_ACTUAL_FACTOR = 5.0;    // |est−actual|/min > 5× → ANALYZE
    static constexpr double EST_MIN_ROWS_FOR_ALERT = 100;  // ignore tiny dev datasets
    static constexpr int    HASH_BATCH_THRESHOLD = 1;      // >1 batch = spill
    static constexpr double NESTED_LOOP_ROW_FACTOR = 1000; // outer rows × loops above this is suspicious

    namespace
    {
        // Walk depth-first; first arg pre-order index lets caller process
        // parents before children (tree label tagging) but isn't required.
        void walk(const std::shared_ptr<PlanNode>& n,
                   const std::function<void(PlanNode&)>& fn)
        {
            if (!n) return;
            fn(*n);
            for (auto& c : n->children) walk(c, fn);
        }

        // Format a row count compactly (1.2k / 4.3M).
        std::wstring fmtRows(double r)
        {
            std::wstringstream ss;
            if (r >= 1e6) ss << (r / 1e6) << L"M";
            else if (r >= 1e3) ss << (r / 1e3) << L"k";
            else ss << static_cast<int>(r);
            return ss.str();
        }
    }

    std::vector<Issue> ExplainPlanHeuristics::analyze(PlanDocument& doc)
    {
        std::vector<Issue> issues;
        if (!doc.root) return issues;

        walk(doc.root, [&](PlanNode& n) {
            // ── Rule 1: Sequential scan with high filter rejection.
            //   Reading a big table end-to-end and discarding most rows
            //   is the textbook case for "add an index".
            if (n.kind == NodeKind::SeqScan && n.rowsRemovedByFilter > 0) {
                double scanned = n.actualRows + n.rowsRemovedByFilter;
                double rejectRatio = scanned > 0 ? n.rowsRemovedByFilter / scanned : 0.0;
                if (rejectRatio >= SEQ_SCAN_FILTER_RATIO &&
                    n.rowsRemovedByFilter >= SEQ_SCAN_MIN_REMOVED)
                {
                    Issue i;
                    i.nodeId = n.id;
                    i.severity = IssueSeverity::Bad;
                    i.title = L"Sequential scan rejecting " + fmtRows(n.rowsRemovedByFilter)
                              + L" rows on " + (n.relation.empty() ? L"a table" : n.relation);
                    std::wstringstream d;
                    d << L"The planner is reading "
                      << (n.relation.empty() ? L"the table" : n.relation)
                      << L" end-to-end and filtering " << static_cast<int>(rejectRatio * 100)
                      << L"% of rows. An index covering the WHERE clause would likely make this query much faster.";
                    i.detail = d.str();
                    issues.push_back(i);
                    n.level = NodeLevel::Bad;
                }
            }

            // ── Rule 2: External-merge sort (spilled to disk).
            if (n.kind == NodeKind::Sort && n.sortSpaceType == L"Disk") {
                Issue i;
                i.nodeId = n.id;
                i.severity = IssueSeverity::Warn;
                i.title = L"Sort spilled to disk (" + n.sortSpaceUsed + L")";
                i.detail = L"work_mem was too small for the sort. Raising it for the session "
                           L"will keep the sort in memory.";
                issues.push_back(i);
                if (n.level == NodeLevel::Good) n.level = NodeLevel::Warn;
            }

            // ── Rule 3: Hash join with multiple batches.
            if (n.kind == NodeKind::HashJoin && n.hashBatches > HASH_BATCH_THRESHOLD) {
                Issue i;
                i.nodeId = n.id;
                i.severity = IssueSeverity::Warn;
                i.title = L"Hash join spilled across " + std::to_wstring(n.hashBatches) + L" batches";
                i.detail = L"The hash table didn't fit in work_mem. Larger work_mem prevents the spill.";
                issues.push_back(i);
                if (n.level == NodeLevel::Good) n.level = NodeLevel::Warn;
            }

            // ── Rule 4: Estimated vs. actual row mismatch — stale ANALYZE.
            if (n.analyzed && n.planRows >= EST_MIN_ROWS_FOR_ALERT &&
                n.actualRows >= EST_MIN_ROWS_FOR_ALERT)
            {
                double est = n.planRows;
                double act = n.actualRows;
                double factor = std::max(est, act) / std::max(1.0, std::min(est, act));
                if (factor >= EST_VS_ACTUAL_FACTOR && !n.relation.empty()) {
                    Issue i;
                    i.nodeId = n.id;
                    i.severity = IssueSeverity::Warn;
                    std::wstringstream t;
                    t << L"Planner row estimate off by " << static_cast<int>(factor)
                      << L"× on " << n.relation;
                    i.title = t.str();
                    i.detail = L"Run ANALYZE to refresh table statistics — costing decisions "
                               L"depend on accurate row counts.";
                    issues.push_back(i);
                    // Keep tree pill consistent with InfoBar callout.
                    if (n.level == NodeLevel::Good) n.level = NodeLevel::Warn;
                }
            }

            // ── Rule 5: Nested Loop with very large outer scan.
            if (n.kind == NodeKind::NestedLoop && n.children.size() >= 2) {
                double outerRows = n.children[0]->actualRows;
                if (outerRows * n.actualLoops > NESTED_LOOP_ROW_FACTOR && n.actualTotal > 100) {
                    Issue i;
                    i.nodeId = n.id;
                    i.severity = IssueSeverity::Warn;
                    i.title = L"Nested Loop driving " + fmtRows(outerRows) + L" outer rows";
                    i.detail = L"For high outer counts a Hash Join or Merge Join is usually faster. "
                               L"Check that join columns are indexed and stats are fresh.";
                    issues.push_back(i);
                    if (n.level == NodeLevel::Good) n.level = NodeLevel::Warn;
                }
            }
        });

        // Stable order: Bad → Warn → Info, then plan order (already DFS).
        std::stable_sort(issues.begin(), issues.end(),
            [](const Issue& a, const Issue& b) {
                return static_cast<int>(a.severity) > static_cast<int>(b.severity);
            });
        return issues;
    }

    std::vector<Recommendation> ExplainPlanHeuristics::recommend(
        const PlanDocument& doc, const std::vector<Issue>& issues)
    {
        std::vector<Recommendation> recs;
        if (!doc.root) return recs;

        // Track which seq-scan tables we've already recommended an index
        // for so we don't duplicate when there are multiple offending
        // filters on the same relation.
        std::vector<std::wstring> indexSuggested;

        // Walk to find seq scans flagged Bad → suggest index. We pull the
        // filter expression as-is; user reviews + edits the SQL preview.
        std::function<void(const std::shared_ptr<PlanNode>&)> walk =
            [&](const std::shared_ptr<PlanNode>& n) {
                if (!n) return;
                if (n->kind == NodeKind::SeqScan && n->level == NodeLevel::Bad &&
                    !n->relation.empty() && !n->filter.empty())
                {
                    if (std::find(indexSuggested.begin(), indexSuggested.end(),
                                   n->relation) == indexSuggested.end())
                    {
                        Recommendation r;
                        r.severity = IssueSeverity::Bad;
                        r.title = L"Create index on " + n->relation;
                        r.rationale = L"Eliminates the Seq Scan that filters out "
                                       + fmtRows(n->rowsRemovedByFilter) + L" rows.";
                        // Best-effort SQL — extract the leftmost identifier
                        // from the filter as the candidate column.
                        std::wstring col;
                        for (auto c : n->filter) {
                            if (iswalnum(c) || c == L'_' || c == L'.') col += c;
                            else if (!col.empty()) break;
                        }
                        if (col.empty()) col = L"<column>";
                        r.previewSql = L"CREATE INDEX ON " + n->relation + L" (" + col + L");";
                        recs.push_back(r);
                        indexSuggested.push_back(n->relation);
                    }
                }
                for (auto& c : n->children) walk(c);
            };
        walk(doc.root);

        // Spilling sort → raise work_mem.
        bool sortSpilled = false;
        bool hashSpilled = false;
        std::function<void(const std::shared_ptr<PlanNode>&)> walk2 =
            [&](const std::shared_ptr<PlanNode>& n) {
                if (!n) return;
                if (n->kind == NodeKind::Sort && n->sortSpaceType == L"Disk") sortSpilled = true;
                if (n->kind == NodeKind::HashJoin && n->hashBatches > 1)        hashSpilled = true;
                for (auto& c : n->children) walk2(c);
            };
        walk2(doc.root);
        if (sortSpilled || hashSpilled) {
            Recommendation r;
            r.severity = IssueSeverity::Warn;
            r.title = L"Raise work_mem for this session";
            r.rationale = sortSpilled
                ? L"The sort spilled to disk — bigger work_mem keeps it in memory."
                : L"The hash join spilled across batches — bigger work_mem prevents the spill.";
            r.previewSql = L"SET LOCAL work_mem = '64MB';";
            recs.push_back(r);
        }

        // ANALYZE recommendation: collect distinct relations from estimate-mismatch issues.
        std::vector<std::wstring> staleRels;
        for (const auto& i : issues) {
            if (i.title.find(L"Planner row estimate off") != std::wstring::npos) {
                // Pull relation off the title's last word — ugly but cheap;
                // we can wire a structured field through later if needed.
                auto pos = i.title.rfind(L' ');
                if (pos != std::wstring::npos) {
                    auto rel = i.title.substr(pos + 1);
                    if (std::find(staleRels.begin(), staleRels.end(), rel) == staleRels.end())
                        staleRels.push_back(rel);
                }
            }
        }
        for (const auto& rel : staleRels) {
            Recommendation r;
            r.severity = IssueSeverity::Info;
            r.title = L"Run ANALYZE " + rel;
            r.rationale = L"Planner stats are stale — refresh row estimates.";
            r.previewSql = L"ANALYZE " + rel + L";";
            recs.push_back(r);
        }

        return recs;
    }
}
