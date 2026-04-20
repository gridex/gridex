//
// UpdateRowsTool.cpp
//

#include "UpdateRowsTool.h"
#include "../MCPToolHelpers.h"
#include "../../Security/MCPIdentifierValidator.h"
#include "../../Security/MCPRowCountEstimator.h"

namespace DBModels
{
    namespace
    {
        // Format a JSON value for direct SQL embedding in the SET
        // clause. Uses the adapter's quoteSqlLiteral for strings so
        // dialect-specific escaping (e.g. Postgres E-strings) is
        // respected. Booleans emit `true/false` for Postgres and
        // `1/0` everywhere else.
        std::wstring formatValue(const nlohmann::json& v,
                                  const std::shared_ptr<DatabaseAdapter>& adapter,
                                  DatabaseType dbType)
        {
            if (v.is_null())           return L"NULL";
            if (v.is_boolean())
            {
                const bool b = v.get<bool>();
                return (dbType == DatabaseType::PostgreSQL)
                    ? (b ? L"true" : L"false")
                    : (b ? L"1" : L"0");
            }
            if (v.is_number_integer()) return std::to_wstring(v.get<int64_t>());
            if (v.is_number_float())   return std::to_wstring(v.get<double>());
            const std::wstring s = v.is_string()
                ? MCPToolHelpers::fromUtf8(v.get<std::string>())
                : MCPToolHelpers::fromUtf8(v.dump());
            try { return adapter->quoteSqlLiteral(s); }
            catch (...)
            {
                // Fallback — basic single-quote doubling.
                std::wstring q = L"'";
                for (wchar_t c : s) { if (c == L'\'') q += L"''"; else q += c; }
                q += L"'";
                return q;
            }
        }
    }

    MCPToolResult UpdateRowsTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);
        auto ts = MCPIdentifierValidator::extractTableAndSchema(params);

        if (!params.contains("set") || !params["set"].is_object() || params["set"].empty())
            throw MCPToolError::invalidParameters("set must be a non-empty object");
        for (auto it = params["set"].begin(); it != params["set"].end(); ++it)
            MCPIdentifierValidator::validate(it.key(), "column name");

        if (!params.contains("where") || !params["where"].is_string())
            throw MCPToolError::invalidParameters(
                "where is required. Bare UPDATE without WHERE is not allowed.");
        const auto whereUtf8 = params["where"].get<std::string>();

        // WHERE-clause guard via permission engine.
        {
            const auto v = ctx.permissionEngine.validateWhereClause(whereUtf8);
            if (const auto* msg = v.errorMessage())
                throw MCPToolError::permissionDenied(*msg);
        }

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        auto [adapter, config] = ctx.getAdapter(connId);
        const std::wstring tableW = MCPToolHelpers::fromUtf8(ts.table);
        const std::wstring schemaW = ts.schema.has_value()
            ? MCPToolHelpers::fromUtf8(*ts.schema) : std::wstring{};
        const std::wstring whereW = MCPToolHelpers::fromUtf8(whereUtf8);

        // Qualified table (dialect-aware via adapter's own quoter).
        std::wstring qTable;
        try
        {
            qTable = (schemaW.empty())
                ? adapter->quoteSqlIdentifier(tableW)
                : adapter->quoteSqlIdentifier(schemaW) + L"." + adapter->quoteSqlIdentifier(tableW);
        }
        catch (...) { qTable = tableW; /* fallback — most drivers accept bare */ }

        // Build SET clause.
        std::wstring setClause;
        bool first = true;
        for (auto it = params["set"].begin(); it != params["set"].end(); ++it)
        {
            if (!first) setClause += L", ";
            first = false;
            std::wstring col;
            try { col = adapter->quoteSqlIdentifier(MCPToolHelpers::fromUtf8(it.key())); }
            catch (...) { col = MCPToolHelpers::fromUtf8(it.key()); }
            setClause += col + L" = " + formatValue(it.value(), adapter, config.databaseType);
        }

        const std::wstring sql = L"UPDATE " + qTable + L" SET " + setClause + L" WHERE " + whereW;

        if (perm.requiresUserApproval())
        {
            const int est = MCPRowCountEstimator::estimate(
                adapter, qTable, whereW, config);
            const std::wstring details =
                L"SQL:\n" + sql + L"\n\nEstimated rows affected: " + std::to_wstring(est);
            const auto clientW = MCPToolHelpers::fromUtf8(
                ctx.client.name.empty() ? std::string{"AI client"} : ctx.client.name);
            const bool ok = ctx.requestApproval(
                MCPToolHelpers::fromUtf8(this->name()),
                L"Update rows in '" + tableW + L"' where " + whereW,
                details, connId, clientW);
            if (!ok) throw MCPToolError::permissionDenied("User denied the operation.");
        }

        QueryResult r;
        try { r = adapter->execute(sql); }
        catch (const std::exception& e)
        { throw MCPToolError::queryFailed(e.what()); }
        if (!r.success) throw MCPToolError::queryFailed(MCPToolHelpers::toUtf8(r.error));

        return MCPToolResult::text(
            "Updated " + std::to_string(r.totalRows) + " row(s) in '" + ts.table + "'.");
    }
}
