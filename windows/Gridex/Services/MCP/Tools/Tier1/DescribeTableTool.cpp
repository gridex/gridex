//
// DescribeTableTool.cpp
//

#include "DescribeTableTool.h"
#include "../MCPToolHelpers.h"
#include "../../Security/MCPIdentifierValidator.h"

namespace DBModels
{
    MCPToolResult DescribeTableTool::execute(const nlohmann::json& params, MCPToolContext& ctx)
    {
        const auto connId = MCPTool::extractConnectionId(params);

        const auto perm = ctx.checkPermission(tier(), connId);
        if (const auto* msg = perm.errorMessage())
            throw MCPToolError::permissionDenied(*msg);

        auto ts = MCPIdentifierValidator::extractTableAndSchema(params);

        const std::wstring table  = MCPToolHelpers::fromUtf8(ts.table);
        const std::wstring schema = ts.schema.has_value()
            ? MCPToolHelpers::fromUtf8(*ts.schema) : std::wstring{};

        auto [adapter, config] = ctx.getAdapter(connId);

        auto columns    = adapter->describeTable(table, schema);
        auto indexes    = adapter->listIndexes(table, schema);
        auto foreignKeys= adapter->listForeignKeys(table, schema);

        nlohmann::json result;
        result["name"] = ts.table;
        if (!schema.empty()) result["schema"] = *ts.schema;
        result["database_type"] = MCPToolHelpers::toUtf8(
            DatabaseTypeDisplayName(config.databaseType));

        nlohmann::json cols = nlohmann::json::array();
        nlohmann::json pkCols = nlohmann::json::array();
        for (const auto& c : columns)
        {
            nlohmann::json cj = {
                {"name",     MCPToolHelpers::toUtf8(c.name)},
                {"type",     MCPToolHelpers::toUtf8(c.dataType)},
                {"nullable", c.nullable}
            };
            if (c.isPrimaryKey) { cj["primary_key"] = true; pkCols.push_back(MCPToolHelpers::toUtf8(c.name)); }
            if (!c.defaultValue.empty()) cj["default"] = MCPToolHelpers::toUtf8(c.defaultValue);
            if (!c.comment.empty())      cj["comment"] = MCPToolHelpers::toUtf8(c.comment);
            cols.push_back(std::move(cj));
        }
        result["columns"] = std::move(cols);
        if (!pkCols.empty()) result["primary_key"] = std::move(pkCols);

        if (!indexes.empty())
        {
            nlohmann::json idxs = nlohmann::json::array();
            for (const auto& i : indexes)
            {
                idxs.push_back({
                    {"name",    MCPToolHelpers::toUtf8(i.name)},
                    {"columns", MCPToolHelpers::toUtf8(i.columns)},
                    {"unique",  i.isUnique},
                    {"type",    MCPToolHelpers::toUtf8(i.algorithm.empty() ? L"btree" : i.algorithm)}
                });
            }
            result["indexes"] = std::move(idxs);
        }

        if (!foreignKeys.empty())
        {
            nlohmann::json fks = nlohmann::json::array();
            for (const auto& f : foreignKeys)
            {
                fks.push_back({
                    {"name",              MCPToolHelpers::toUtf8(f.name)},
                    {"column",            MCPToolHelpers::toUtf8(f.column)},
                    {"references_table",  MCPToolHelpers::toUtf8(f.referencedTable)},
                    {"references_column", MCPToolHelpers::toUtf8(f.referencedColumn)}
                });
            }
            result["foreign_keys"] = std::move(fks);
        }

        return MCPToolResult::text(result.dump(2));
    }
}
