//
// ExecuteWriteQueryTool.cpp
//

#include "ExecuteWriteQueryTool.h"
#include "../MCPToolHelpers.h"
#include "../../Security/MCPSQLSanitizer.h"
#include <algorithm>
#include <regex>

namespace DBModels
{
    namespace
    {
        std::string trim(const std::string& s)
        {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
            return s.substr(a, b - a);
        }
        std::string upper(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
            return s;
        }
        bool startsWith(const std::string& s, const std::string& prefix)
        {
            return s.size() >= prefix.size() &&
                   std::equal(prefix.begin(), prefix.end(), s.begin());
        }
    }

    MCPToolResult ExecuteWriteQueryTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);

        if (!params.contains("sql") || !params["sql"].is_string())
            throw MCPToolError::invalidParameters("sql is required");
        const auto sqlUtf8 = params["sql"].get<std::string>();

        // Sanitize + multi-statement guard. Run syntactic checks
        // against a comment/literal-stripped copy so payloads
        // hidden inside comments or strings cannot fool them.
        const std::string codeOnly = MCPSQLSanitizer::stripCommentsAndStrings(sqlUtf8);
        const std::string trimmedUpper = upper(trim(codeOnly));

        std::string withoutTrailingSemi = trimmedUpper;
        if (!withoutTrailingSemi.empty() && withoutTrailingSemi.back() == ';')
        {
            withoutTrailingSemi.pop_back();
            withoutTrailingSemi = trim(withoutTrailingSemi);
        }
        if (withoutTrailingSemi.find(';') != std::string::npos)
            throw MCPToolError::invalidParameters(
                "Multiple statements are not allowed. Send one statement at a time.");

        if (startsWith(trimmedUpper, "SELECT") || startsWith(trimmedUpper, "WITH"))
            throw MCPToolError::invalidParameters(
                "Use the 'query' tool for SELECT / WITH statements. "
                "execute_write_query is for write operations only.");

        // UPDATE/DELETE must contain WHERE and the WHERE clause
        // must pass the permission-engine guard (no `;`, no
        // comments, no trivial `1=1`).
        if (startsWith(trimmedUpper, "UPDATE") || startsWith(trimmedUpper, "DELETE"))
        {
            static const std::regex whereRe(R"(\bWHERE\b)", std::regex::icase);
            if (!std::regex_search(trimmedUpper, whereRe))
                throw MCPToolError::permissionDenied(
                    "UPDATE/DELETE without WHERE clause is not allowed.");

            // Extract the WHERE tail from the sanitized SQL to feed
            // the WHERE validator. Case-insensitive search in the
            // sanitized text, slice everything after it.
            std::smatch m;
            if (std::regex_search(codeOnly, m, whereRe))
            {
                const auto whereTail = trim(std::string(
                    m.suffix().first, m.suffix().second));
                const auto v = ctx.permissionEngine.validateWhereClause(whereTail);
                if (const auto* msg = v.errorMessage())
                    throw MCPToolError::permissionDenied(*msg);
            }
        }

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        auto [adapter, _] = ctx.getAdapter(connId);

        if (perm.requiresUserApproval())
        {
            const auto clientW = MCPToolHelpers::fromUtf8(
                ctx.client.name.empty() ? std::string{"AI client"} : ctx.client.name);
            const std::wstring details = L"SQL:\n" + MCPToolHelpers::fromUtf8(sqlUtf8);
            const bool ok = ctx.requestApproval(
                MCPToolHelpers::fromUtf8(this->name()),
                L"Execute write query",
                details, connId, clientW);
            if (!ok) throw MCPToolError::permissionDenied("User denied the operation.");
        }

        QueryResult r;
        try { r = adapter->execute(MCPToolHelpers::fromUtf8(sqlUtf8)); }
        catch (const std::exception& e)
        { throw MCPToolError::queryFailed(e.what()); }
        if (!r.success) throw MCPToolError::queryFailed(MCPToolHelpers::toUtf8(r.error));

        std::string resp = "Query executed successfully.";
        if (r.totalRows > 0)
            resp += " " + std::to_string(r.totalRows) + " row(s) affected.";
        return MCPToolResult::text(resp);
    }
}
