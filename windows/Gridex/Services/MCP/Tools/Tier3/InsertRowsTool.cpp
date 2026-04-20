//
// InsertRowsTool.cpp
//

#include "InsertRowsTool.h"
#include "../MCPToolHelpers.h"
#include "../../Security/MCPIdentifierValidator.h"

namespace DBModels
{
    namespace
    {
        // Convert a JSON value to the wstring form expected by
        // TableRow. Nulls become the SQL-NULL sentinel so the
        // adapter emits `DEFAULT` / `NULL` rather than the literal
        // string "null".
        std::wstring jsonToCell(const nlohmann::json& v)
        {
            if (v.is_null())            return nullValue();
            if (v.is_boolean())         return v.get<bool>() ? L"true" : L"false";
            if (v.is_number_integer())  return std::to_wstring(v.get<int64_t>());
            if (v.is_number_float())    return std::to_wstring(v.get<double>());
            if (v.is_string())          return MCPToolHelpers::fromUtf8(v.get<std::string>());
            // Array/object — serialize to JSON text and let the adapter
            // store it as-is (useful for Postgres json/jsonb columns).
            return MCPToolHelpers::fromUtf8(v.dump());
        }

        std::string clientNameFromContext(const MCPToolContext& ctx)
        {
            return ctx.client.name.empty() ? std::string{"AI client"} : ctx.client.name;
        }
    }

    MCPToolResult InsertRowsTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);
        auto ts = MCPIdentifierValidator::extractTableAndSchema(params);

        if (!params.contains("rows") || !params["rows"].is_array() || params["rows"].empty())
            throw MCPToolError::invalidParameters("rows must be a non-empty array");

        const auto& rowsJson = params["rows"];

        // Permission + approval (Tier 3 always requires approval in RW mode).
        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        if (perm.requiresUserApproval())
        {
            // Preview first 3 rows for the dialog body.
            std::wstring preview;
            const int previewCap = (std::min<int>)(3, static_cast<int>(rowsJson.size()));
            for (int i = 0; i < previewCap; ++i)
            {
                preview += L"  " + MCPToolHelpers::fromUtf8(rowsJson[i].dump()) + L"\n";
            }
            if (static_cast<int>(rowsJson.size()) > previewCap)
                preview += L"  … and " +
                    std::to_wstring(rowsJson.size() - previewCap) + L" more\n";

            const std::wstring tableW = MCPToolHelpers::fromUtf8(ts.table);
            const std::wstring desc =
                L"Insert " + std::to_wstring(rowsJson.size()) +
                L" row(s) into '" + tableW + L"'";
            const std::wstring details = L"Preview:\n" + preview;

            const auto clientW = MCPToolHelpers::fromUtf8(clientNameFromContext(ctx));
            const bool ok = ctx.requestApproval(
                MCPToolHelpers::fromUtf8(this->name()),
                desc, details, connId, clientW);
            if (!ok) throw MCPToolError::permissionDenied("User denied the operation.");
        }

        auto [adapter, config] = ctx.getAdapter(connId);
        const std::wstring tableW = MCPToolHelpers::fromUtf8(ts.table);
        const std::wstring schemaW = ts.schema.has_value()
            ? MCPToolHelpers::fromUtf8(*ts.schema) : std::wstring{};

        int inserted = 0;
        for (const auto& rowJson : rowsJson)
        {
            if (!rowJson.is_object()) continue;
            TableRow row;
            for (auto it = rowJson.begin(); it != rowJson.end(); ++it)
            {
                const std::wstring key = MCPToolHelpers::fromUtf8(it.key());
                row[key] = jsonToCell(it.value());
            }
            try
            {
                auto r = adapter->insertRow(tableW, schemaW, row);
                if (r.success) ++inserted;
                else
                    throw MCPToolError::queryFailed(MCPToolHelpers::toUtf8(r.error));
            }
            catch (const std::exception& e)
            {
                throw MCPToolError::queryFailed(e.what());
            }
        }

        return MCPToolResult::text(
            "Inserted " + std::to_string(inserted) + " row(s) into '" + ts.table + "'.");
    }
}
