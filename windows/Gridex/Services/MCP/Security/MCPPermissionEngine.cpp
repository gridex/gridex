//
// MCPPermissionEngine.cpp
//
// Notes on porting from Swift:
//  * `actor` → `std::mutex`-guarded members.
//  * NSRegularExpression → `std::regex`. ECMAScript flavor accepts
//    `\b` word anchors; tested empirically on MSVC.
//  * Whitespace helpers use <cctype> isspace to avoid std::isspace
//    locale overloads that choke on char >= 0x80.

#include "MCPPermissionEngine.h"
#include "MCPSQLSanitizer.h"
#include <regex>
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace DBModels
{
    // Static — these allocations happen once at first call.
    namespace
    {
        const std::vector<std::string>& readOnlyPrefixes()
        {
            static const std::vector<std::string> v{
                "SELECT", "SHOW", "EXPLAIN", "DESCRIBE", "DESC", "WITH"
            };
            return v;
        }

        // Must match mac's MCPPermissionEngine.dangerousKeywords exactly
        // — drift here is a security bug.
        const std::regex& dangerousKeywordRegex()
        {
            static const std::regex re(
                R"(\b(INSERT|UPDATE|DELETE|MERGE|UPSERT|DROP|CREATE|ALTER|TRUNCATE|RENAME|)"
                R"(GRANT|REVOKE|CALL|EXEC|EXECUTE|DO|NEXTVAL|SETVAL|LO_IMPORT|LO_EXPORT|)"
                R"(PG_READ_SERVER_FILES|PG_WRITE_SERVER_FILES|PG_READ_BINARY_FILE|PG_LS_DIR|)"
                R"(DBLINK|DBLINK_EXEC|COPY|VACUUM|ANALYZE|REINDEX|CLUSTER|REFRESH|LOCK|UNLOCK|)"
                R"(SET|RESET|BEGIN|COMMIT|ROLLBACK|SAVEPOINT)\b)",
                std::regex::icase | std::regex::ECMAScript);
            return re;
        }

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
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }

        bool startsWith(const std::string& s, const std::string& prefix)
        {
            return s.size() >= prefix.size() &&
                   std::equal(prefix.begin(), prefix.end(), s.begin());
        }
    }

    // ── Mode bookkeeping ─────────────────────────────────────

    void MCPPermissionEngine::setMode(const std::wstring& id, MCPConnectionMode mode)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        modes_[id] = mode;
    }

    MCPConnectionMode MCPPermissionEngine::getMode(const std::wstring& id) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = modes_.find(id);
        return it != modes_.end() ? it->second : MCPConnectionMode::Locked;
    }

    void MCPPermissionEngine::removeMode(const std::wstring& id)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        modes_.erase(id);
    }

    // ── Permission check ─────────────────────────────────────

    MCPPermissionResult MCPPermissionEngine::checkPermission(
        MCPPermissionTier tier, const std::wstring& id) const
    {
        return checkPermission(tier, getMode(id));
    }

    MCPPermissionResult MCPPermissionEngine::checkPermission(
        MCPPermissionTier tier, MCPConnectionMode mode) const
    {
        switch (tier)
        {
            case MCPPermissionTier::Schema:
                return mcpAllowsTier1(mode)
                    ? MCPPermissionResult::allowed()
                    : MCPPermissionResult::denied("Connection is locked. MCP access is disabled.");

            case MCPPermissionTier::Read:
                return mcpAllowsTier2(mode)
                    ? MCPPermissionResult::allowed()
                    : MCPPermissionResult::denied("Connection is locked. MCP access is disabled.");

            case MCPPermissionTier::Write:
                if (!mcpAllowsTier3(mode))
                    return MCPPermissionResult::denied(
                        "This operation requires read-write mode. Ask the user to enable it in Connection Settings > MCP Access.");
                return MCPPermissionResult::requiresApproval();

            case MCPPermissionTier::DDL:
                if (!mcpAllowsTier4(mode))
                    return MCPPermissionResult::denied(
                        "DDL operations require read-write mode. Ask the user to enable it in Connection Settings > MCP Access.");
                return MCPPermissionResult::requiresApproval();

            case MCPPermissionTier::Advanced:
                return mcpAllowsTier5(mode)
                    ? MCPPermissionResult::allowed()
                    : MCPPermissionResult::denied("Connection is locked. MCP access is disabled.");
        }
        return MCPPermissionResult::denied("Unknown tier");
    }

    // ── Read-only SQL validator ──────────────────────────────

    MCPPermissionResult MCPPermissionEngine::validateReadOnlyQuery(const std::string& sql) const
    {
        const std::string code = MCPSQLSanitizer::stripCommentsAndStrings(sql);
        const std::string trimmedUpper = upper(trim(code));

        std::string withoutTrailingSemi = trimmedUpper;
        if (!withoutTrailingSemi.empty() && withoutTrailingSemi.back() == ';')
        {
            withoutTrailingSemi.pop_back();
            withoutTrailingSemi = trim(withoutTrailingSemi);
        }
        if (withoutTrailingSemi.find(';') != std::string::npos)
            return MCPPermissionResult::denied("Multiple statements are not allowed in read-only mode.");

        bool ok = false;
        for (const auto& p : readOnlyPrefixes())
            if (startsWith(trimmedUpper, p)) { ok = true; break; }
        if (!ok)
            return MCPPermissionResult::denied("Only SELECT queries are allowed in read-only mode. This query appears to modify data.");

        std::smatch m;
        if (std::regex_search(code, m, dangerousKeywordRegex()))
        {
            const std::string hit = upper(m.str());
            return MCPPermissionResult::denied(
                "Query contains '" + hit + "' which is not allowed in read-only mode.");
        }

        return MCPPermissionResult::allowed();
    }

    // ── WHERE clause validator ───────────────────────────────

    MCPPermissionResult MCPPermissionEngine::validateWhereClause(const std::string& whereClause) const
    {
        if (whereClause.empty())
            return MCPPermissionResult::denied(
                "WHERE clause is required for UPDATE/DELETE operations. Bare UPDATE/DELETE without WHERE is not allowed.");

        const std::string trimmed = trim(whereClause);
        if (trimmed.empty())
            return MCPPermissionResult::denied("WHERE clause is required for UPDATE/DELETE operations.");

        if (trimmed.find(';') != std::string::npos)
            return MCPPermissionResult::denied("WHERE clause must not contain ';' — statement terminators are forbidden.");
        if (trimmed.find("--") != std::string::npos)
            return MCPPermissionResult::denied("WHERE clause must not contain '--' — SQL line comments are forbidden.");
        if (trimmed.find("/*") != std::string::npos || trimmed.find("*/") != std::string::npos)
            return MCPPermissionResult::denied("WHERE clause must not contain '/*' or '*/' — SQL block comments are forbidden.");

        // Trivial predicate check: uppercase, strip ALL whitespace.
        std::string compact;
        compact.reserve(trimmed.size());
        for (char c : trimmed)
            if (!std::isspace(static_cast<unsigned char>(c)))
                compact.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

        static const std::unordered_set<std::string> trivial{
            "1=1", "TRUE", "1", "'1'='1'",
            "1<>0", "0=0", "2>1", "TRUE=TRUE", "NULLISNULL"
        };
        if (trivial.count(compact) != 0)
            return MCPPermissionResult::denied(
                "Trivial WHERE clause '" + trimmed + "' is not allowed. Provide a meaningful predicate.");

        return MCPPermissionResult::allowed();
    }
}
