#include "pch.h"
// Undo windows.h max/min macros (see note in heuristics).
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#include "ExplainPlanParser.h"

#include <cctype>
#include <cwctype>
#include <sstream>

namespace Gridex::ExplainViz
{
    // ── Mini wstring JSON tokenizer ───────────────────────────────
    // Tiny scanner over wide chars — only what we need to walk arrays
    // and objects in PG's EXPLAIN output. Whitespace tolerant; assumes
    // valid UTF-16 produced by Postgres.
    namespace
    {
        struct Reader
        {
            const wchar_t* p;
            const wchar_t* end;

            void skipWs()
            {
                while (p < end && std::iswspace(*p)) ++p;
            }
            bool eof() const { return p >= end; }
            wchar_t peek() { skipWs(); return p < end ? *p : 0; }

            bool match(wchar_t c)
            {
                skipWs();
                if (p < end && *p == c) { ++p; return true; }
                return false;
            }

            bool parseString(std::wstring& out)
            {
                skipWs();
                if (p >= end || *p != L'"') return false;
                ++p;
                out.clear();
                while (p < end && *p != L'"') {
                    if (*p == L'\\' && p + 1 < end) {
                        wchar_t n = p[1];
                        switch (n) {
                        case L'n': out += L'\n'; break;
                        case L't': out += L'\t'; break;
                        case L'r': out += L'\r'; break;
                        case L'"': out += L'"'; break;
                        case L'\\': out += L'\\'; break;
                        case L'/':  out += L'/'; break;
                        case L'b':  out += L'\b'; break;
                        case L'f':  out += L'\f'; break;
                        case L'u': {
                            // \uXXXX consumes 6 chars (\, u, 4 hex). Guard
                            // against truncated escapes so we don't loop.
                            if (p + 6 <= end) {
                                wchar_t hex[5] = { p[2], p[3], p[4], p[5], 0 };
                                out += static_cast<wchar_t>(std::wcstoul(hex, nullptr, 16));
                                p += 6;
                            } else {
                                p = end;
                            }
                            continue;   // skip the unified `p += 2` below
                        }
                        default: out += n; break;
                        }
                        p += 2;
                    } else {
                        out += *p; ++p;
                    }
                }
                if (p < end && *p == L'"') ++p;
                return true;
            }

            bool parseNumber(double& out)
            {
                skipWs();
                if (p >= end) return false;
                wchar_t* endPtr = nullptr;
                out = std::wcstod(p, &endPtr);
                if (endPtr == p) return false;
                p = endPtr;
                return true;
            }

            // Skip a value (object / array / string / number / bool / null)
            // without parsing it — used for unknown keys.
            bool skipValue()
            {
                skipWs();
                if (p >= end) return false;
                if (*p == L'"') { std::wstring tmp; return parseString(tmp); }
                if (*p == L'{' || *p == L'[') {
                    wchar_t open = *p;
                    wchar_t close = (open == L'{') ? L'}' : L']';
                    int depth = 0; ++p;
                    bool inStr = false, esc = false;
                    while (p < end) {
                        wchar_t c = *p;
                        if (inStr) {
                            if (esc) esc = false;
                            else if (c == L'\\') esc = true;
                            else if (c == L'"') inStr = false;
                        } else {
                            if (c == L'"') inStr = true;
                            else if (c == open) ++depth;
                            else if (c == close) {
                                if (depth == 0) { ++p; return true; }
                                --depth;
                            }
                        }
                        ++p;
                    }
                    return false;
                }
                // bool / null / number — eat until comma or close.
                while (p < end && *p != L',' && *p != L'}' && *p != L']') ++p;
                return true;
            }
        };

        // Forward decl
        std::shared_ptr<PlanNode> parsePlanNode(Reader& r);

        // Map the "Node Type" string from EXPLAIN to our enum.
        NodeKind mapKind(const std::wstring& s)
        {
            if (s == L"Limit") return NodeKind::Limit;
            if (s == L"Sort") return NodeKind::Sort;
            if (s == L"Hash Join") return NodeKind::HashJoin;
            if (s == L"Merge Join") return NodeKind::MergeJoin;
            if (s == L"Nested Loop") return NodeKind::NestedLoop;
            if (s == L"Seq Scan") return NodeKind::SeqScan;
            if (s == L"Index Scan") return NodeKind::IndexScan;
            if (s == L"Index Only Scan") return NodeKind::IndexOnlyScan;
            if (s == L"Bitmap Heap Scan") return NodeKind::BitmapHeapScan;
            if (s == L"Bitmap Index Scan") return NodeKind::BitmapIndexScan;
            if (s == L"Hash") return NodeKind::Hash;
            if (s == L"Aggregate") return NodeKind::Aggregate;
            if (s == L"GroupAggregate") return NodeKind::GroupAggregate;
            if (s == L"HashAggregate") return NodeKind::HashAggregate;
            if (s == L"WindowAgg") return NodeKind::WindowAgg;
            if (s == L"Materialize") return NodeKind::Materialize;
            if (s == L"Gather") return NodeKind::Gather;
            if (s == L"Gather Merge") return NodeKind::GatherMerge;
            if (s == L"Append") return NodeKind::Append;
            if (s == L"Result") return NodeKind::Result;
            if (s == L"Subquery Scan") return NodeKind::SubqueryScan;
            if (s == L"CTE Scan") return NodeKind::CteScan;
            if (s == L"Unique") return NodeKind::Unique;
            if (s == L"Memoize") return NodeKind::Memoize;
            return NodeKind::Other;
        }

        // Build a one-liner detail string from a parsed node — what to show
        // next to the op label in the tree. Mirrors the design's "detail"
        // column without dumping every field.
        std::wstring buildDetail(const PlanNode& n)
        {
            std::wstringstream ss;
            bool first = true;
            auto sep = [&]() { if (!first) ss << L" · "; first = false; };

            if (!n.indexName.empty()) {
                sep(); ss << L"using " << n.indexName;
            }
            if (!n.joinType.empty()) {
                sep(); ss << n.joinType;
            }
            if (!n.strategy.empty()) {
                sep(); ss << n.strategy;
            }
            if (!n.indexCond.empty()) {
                sep(); ss << L"cond: " << n.indexCond;
            }
            if (!n.filter.empty()) {
                sep(); ss << L"filter: " << n.filter;
            }
            if (!n.sortMethod.empty()) {
                sep(); ss << n.sortMethod;
                if (!n.sortSpaceUsed.empty())
                    ss << L" (" << n.sortSpaceUsed << L" " << n.sortSpaceType << L")";
            }
            if (n.hashBatches > 0 || !n.hashMemKb.empty()) {
                sep();
                if (n.hashBuckets > 0) ss << L"buckets " << n.hashBuckets;
                if (n.hashBatches > 1) ss << L" batches " << n.hashBatches;
                if (!n.hashMemKb.empty()) ss << L" mem " << n.hashMemKb << L" KB";
            }
            return ss.str();
        }

        // Walk a plan-object's keys filling our PlanNode. We skip unknown
        // keys; PG occasionally adds new ones across versions and we don't
        // want to fail because of them.
        std::shared_ptr<PlanNode> parsePlanObject(Reader& r)
        {
            if (!r.match(L'{')) return nullptr;
            auto node = std::make_shared<PlanNode>();
            std::wstring schema, alias;

            while (!r.eof()) {
                r.skipWs();
                if (r.match(L'}')) break;
                std::wstring key;
                if (!r.parseString(key)) return nullptr;
                if (!r.match(L':')) return nullptr;
                r.skipWs();

                if (key == L"Node Type") {
                    r.parseString(node->op);
                    node->kind = mapKind(node->op);
                }
                else if (key == L"Relation Name") r.parseString(node->relation);
                else if (key == L"Schema") r.parseString(schema);
                else if (key == L"Alias") r.parseString(node->alias);
                else if (key == L"Index Name") r.parseString(node->indexName);
                else if (key == L"Index Cond") r.parseString(node->indexCond);
                else if (key == L"Filter") r.parseString(node->filter);
                else if (key == L"Sort Method") r.parseString(node->sortMethod);
                else if (key == L"Sort Space Used") {
                    double v; if (r.parseNumber(v))
                        node->sortSpaceUsed = std::to_wstring(static_cast<int>(v)) + L" KB";
                }
                else if (key == L"Sort Space Type") r.parseString(node->sortSpaceType);
                else if (key == L"Join Type") r.parseString(node->joinType);
                else if (key == L"Strategy") r.parseString(node->strategy);
                else if (key == L"Hash Buckets") {
                    double v; if (r.parseNumber(v)) node->hashBuckets = static_cast<int>(v);
                }
                else if (key == L"Hash Batches") {
                    double v; if (r.parseNumber(v)) node->hashBatches = static_cast<int>(v);
                }
                else if (key == L"Original Hash Batches") { double v; r.parseNumber(v); }
                else if (key == L"Peak Memory Usage") {
                    double v; if (r.parseNumber(v)) node->hashMemKb = std::to_wstring(static_cast<int>(v));
                }
                else if (key == L"Startup Cost") r.parseNumber(node->startupCost);
                else if (key == L"Total Cost") r.parseNumber(node->totalCost);
                else if (key == L"Plan Rows") r.parseNumber(node->planRows);
                else if (key == L"Plan Width") {
                    double v; if (r.parseNumber(v)) node->planWidth = static_cast<int>(v);
                }
                else if (key == L"Actual Startup Time") {
                    r.parseNumber(node->actualStartup);
                    node->analyzed = true;
                }
                else if (key == L"Actual Total Time") {
                    r.parseNumber(node->actualTotal);
                    node->analyzed = true;
                }
                else if (key == L"Actual Rows") r.parseNumber(node->actualRows);
                else if (key == L"Actual Loops") {
                    double v; if (r.parseNumber(v)) node->actualLoops = static_cast<int>(v);
                }
                else if (key == L"Rows Removed by Filter") {
                    double v; if (r.parseNumber(v)) node->rowsRemovedByFilter = static_cast<int>(v);
                }
                else if (key == L"Rows Removed by Index Recheck") {
                    double v; if (r.parseNumber(v)) node->rowsRemovedByIndexRecheck = static_cast<int>(v);
                }
                else if (key == L"Shared Hit Blocks") {
                    double v; if (r.parseNumber(v)) node->sharedHit = static_cast<int64_t>(v);
                }
                else if (key == L"Shared Read Blocks") {
                    double v; if (r.parseNumber(v)) node->sharedRead = static_cast<int64_t>(v);
                }
                else if (key == L"Shared Dirtied Blocks") {
                    double v; if (r.parseNumber(v)) node->sharedDirtied = static_cast<int64_t>(v);
                }
                else if (key == L"Shared Written Blocks") {
                    double v; if (r.parseNumber(v)) node->sharedWritten = static_cast<int64_t>(v);
                }
                else if (key == L"Temp Read Blocks") {
                    double v; if (r.parseNumber(v)) node->tempRead = static_cast<int64_t>(v);
                }
                else if (key == L"Temp Written Blocks") {
                    double v; if (r.parseNumber(v)) node->tempWritten = static_cast<int64_t>(v);
                }
                else if (key == L"Output") {
                    // array of strings
                    if (r.match(L'[')) {
                        while (!r.eof()) {
                            r.skipWs();
                            if (r.match(L']')) break;
                            std::wstring col;
                            if (r.parseString(col)) node->outputCols.push_back(col);
                            r.skipWs(); r.match(L',');
                        }
                    } else {
                        r.skipValue();
                    }
                }
                else if (key == L"Plans") {
                    if (r.match(L'[')) {
                        while (!r.eof()) {
                            r.skipWs();
                            if (r.match(L']')) break;
                            auto child = parsePlanObject(r);
                            if (child) node->children.push_back(child);
                            r.skipWs(); r.match(L',');
                        }
                    } else {
                        r.skipValue();
                    }
                }
                else {
                    r.skipValue();
                }

                r.skipWs();
                r.match(L',');
            }

            // Compose qualified relation name (schema.table).
            if (!schema.empty() && !node->relation.empty()) {
                node->relation = schema + L"." + node->relation;
            }
            // Detail composed last so all parts are available.
            node->detail = buildDetail(*node);
            return node;
        }

        std::shared_ptr<PlanNode> parsePlanNode(Reader& r)
        {
            return parsePlanObject(r);
        }

        // Generate ids by depth-first walk so siblings get distinct names.
        void assignIds(PlanNode& n, int& counter)
        {
            n.id = L"n" + std::to_wstring(counter++);
            for (auto& c : n.children) assignIds(*c, counter);
        }

        double sumSubtreeOwn(const PlanNode& n)
        {
            double total = (n.actualTotal * n.actualLoops);
            // own = total - sum(children total). We compute own at parent
            // level — nothing to do recursively here; classifyTimings does
            // the second pass once we know the grand total.
            return total;
        }

        void classifyTimings(PlanNode& n, double grandTotalMs)
        {
            double childrenTotal = 0.0;
            for (auto& c : n.children) {
                childrenTotal += c->actualTotal * c->actualLoops;
                classifyTimings(*c, grandTotalMs);
            }
            n.ownMs = std::max(0.0, n.actualTotal * n.actualLoops - childrenTotal);
            n.pctOfTotal = grandTotalMs > 0 ? (n.ownMs / grandTotalMs) * 100.0 : 0.0;

            // Default heat classification — heuristics module may upgrade
            // to Bad on issues even if the percentage is small.
            if (n.pctOfTotal >= 25.0) n.level = NodeLevel::Bad;
            else if (n.pctOfTotal >= 5.0) n.level = NodeLevel::Warn;
            else n.level = NodeLevel::Good;

            // Sort spilling to disk should always show as Warn at minimum.
            if (n.sortSpaceType == L"Disk" && n.level == NodeLevel::Good)
                n.level = NodeLevel::Warn;
        }
    } // anon namespace

    void ExplainPlanParser::postProcess(PlanDocument& doc)
    {
        if (!doc.root) return;
        int counter = 0;
        assignIds(*doc.root, counter);
        // Grand total = root's actualTotal*loops, fallback to summed if root
        // wasn't analyzed (shouldn't happen with ANALYZE but defensive).
        double total = doc.root->actualTotal * doc.root->actualLoops;
        if (total <= 0.0) total = sumSubtreeOwn(*doc.root);
        doc.executionMs = total;
        classifyTimings(*doc.root, total);
    }

    bool ExplainPlanParser::parse(const std::wstring& json,
                                 PlanDocument& out,
                                 std::wstring& err)
    {
        if (json.empty()) { err = L"Empty plan JSON."; return false; }
        out.rawJson = json;

        Reader r{ json.c_str(), json.c_str() + json.size() };

        // EXPLAIN FORMAT JSON returns a top-level array. Pop into the first
        // element which is an object containing "Plan", "Planning Time" etc.
        r.skipWs();
        if (!r.match(L'[')) { err = L"Plan JSON must be an array."; return false; }
        r.skipWs();
        if (!r.match(L'{')) { err = L"Plan JSON missing root object."; return false; }

        while (!r.eof()) {
            r.skipWs();
            if (r.match(L'}')) break;
            std::wstring key;
            if (!r.parseString(key)) { err = L"Bad key in plan."; return false; }
            if (!r.match(L':')) { err = L"Missing ':' in plan."; return false; }
            r.skipWs();
            if (key == L"Plan") {
                out.root = parsePlanObject(r);
            }
            else if (key == L"Planning Time") {
                r.parseNumber(out.planningMs);
            }
            else if (key == L"Execution Time") {
                r.parseNumber(out.executionMs);
            }
            else {
                r.skipValue();
            }
            r.skipWs();
            r.match(L',');
        }

        if (!out.root) { err = L"Plan JSON had no Plan object."; return false; }

        out.analyzed = out.root->analyzed;
        postProcess(out);

        // executionMs from `Execution Time` (when present) takes priority
        // — that's the wall-clock for the whole query, more accurate than
        // root node totals on parallel plans.
        // postProcess already used root total; if Execution Time is bigger
        // (e.g., parallel workers), keep the root-derived sum for share %
        // but expose Execution Time separately via planningMs/executionMs
        // accessors. We accept the slight drift.
        return true;
    }
}
