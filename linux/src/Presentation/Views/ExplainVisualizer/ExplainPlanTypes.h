#pragma once
// Pure-C++ value types for the Explain Plan Visualizer. Mirrors the shape
// returned by `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)` on Postgres so a
// JSON node maps 1:1 onto a PlanNode in our tree.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gridex::explainviz
{
    // Heuristic-derived severity used by the heat bar + node coloring.
    enum class NodeLevel
    {
        Good,    // <5% of total time, no warnings
        Warn,    // sort spill, partial spill, low-confidence stats
        Bad      // hot path: >25% of total time, or seq scan w/ heavy filter
    };

    // What kind of operation the node represents — drives the glyph and
    // colored badge in the UI. Keep small + extend as needed.
    enum class NodeKind
    {
        Other,
        Limit,
        Sort,
        HashJoin,
        MergeJoin,
        NestedLoop,
        SeqScan,
        IndexScan,
        IndexOnlyScan,
        BitmapHeapScan,
        BitmapIndexScan,
        Hash,
        Aggregate,
        GroupAggregate,
        HashAggregate,
        WindowAgg,
        Materialize,
        Gather,
        GatherMerge,
        Append,
        Result,
        SubqueryScan,
        CteScan,
        Unique,
        Memoize
    };

    struct PlanNode
    {
        std::wstring id;            // generated, used for UI selection (e.g. "n0")
        std::wstring op;             // raw "Node Type" from EXPLAIN
        NodeKind kind = NodeKind::Other;
        std::wstring relation;       // table name when applicable
        std::wstring alias;
        std::wstring detail;         // human-readable extras (filter, sort key, join cond...)

        // Cost numbers from the planner.
        double startupCost = 0.0;
        double totalCost = 0.0;
        double planRows = 0.0;
        int    planWidth = 0;

        // Actual measurements when ANALYZE was used.
        bool   analyzed = false;
        double actualStartup = 0.0;
        double actualTotal = 0.0;       // ms per loop
        double actualRows = 0.0;
        int    actualLoops = 1;
        double pctOfTotal = 0.0;        // own-time share of grand total ms
        double ownMs = 0.0;             // own-time = actualTotal*loops − Σ children own-time

        // Buffer accounting (int64 — large plans can touch billions of blocks).
        int64_t sharedHit = 0;
        int64_t sharedRead = 0;
        int64_t sharedDirtied = 0;
        int64_t sharedWritten = 0;
        int64_t tempRead = 0;
        int64_t tempWritten = 0;

        // Sort-specific
        std::wstring sortMethod;        // "external merge", "quicksort", ...
        std::wstring sortSpaceUsed;
        std::wstring sortSpaceType;     // "Memory" or "Disk"

        // Hash-specific
        int hashBuckets = 0;
        int hashBatches = 0;
        std::wstring hashMemKb;         // raw value, formatted later

        // Index-specific
        std::wstring indexName;
        std::wstring indexCond;

        // Filter accounting
        std::wstring filter;
        int rowsRemovedByFilter = 0;
        int rowsRemovedByIndexRecheck = 0;
        std::wstring joinType;
        std::wstring strategy;

        std::vector<std::wstring> outputCols;

        NodeLevel level = NodeLevel::Good;

        std::vector<std::shared_ptr<PlanNode>> children;
    };

    // Top-level container for one parsed EXPLAIN output.
    struct PlanDocument
    {
        std::shared_ptr<PlanNode> root;
        double planningMs = 0.0;
        double executionMs = 0.0;       // grand total ms (ANALYZE)
        std::wstring rawJson;           // kept so flyout / Copy plan works
        std::wstring originalSql;       // SQL that generated this plan
        std::wstring databaseName;
        std::wstring serverVersion;
        bool analyzed = false;
        bool buffers = false;
    };
}
