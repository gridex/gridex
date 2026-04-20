//
// MCPIdentifierValidator.cpp
//

#include "MCPIdentifierValidator.h"
#include "../../../Models/MCP/MCPToolError.h"

namespace DBModels { namespace MCPIdentifierValidator {

bool isValid(const std::string& identifier)
{
    if (identifier.empty() || identifier.size() > static_cast<size_t>(kMaxLength))
        return false;

    for (size_t i = 0; i < identifier.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(identifier[i]);
        const bool isLetter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        const bool isDigit  = c >= '0' && c <= '9';
        if (i == 0)
        {
            if (!isLetter) return false;
        }
        else
        {
            if (!isLetter && !isDigit) return false;
        }
    }
    return true;
}

void validate(const std::string& identifier, const std::string& name)
{
    if (!isValid(identifier))
    {
        throw MCPToolError::invalidParameters(
            name + " '" + identifier + "' contains invalid characters. "
            "Allowed: letters, digits, underscore; must start with a letter or underscore; max "
            + std::to_string(kMaxLength) + " chars.");
    }
}

TableSchema extractTableAndSchema(const nlohmann::json& params)
{
    TableSchema ts;
    if (!params.contains("table_name") || !params["table_name"].is_string())
        throw MCPToolError::invalidParameters("table_name is required");

    ts.table = params["table_name"].get<std::string>();
    validate(ts.table, "table_name");

    if (params.contains("schema") && params["schema"].is_string())
    {
        auto s = params["schema"].get<std::string>();
        validate(s, "schema");
        ts.schema = s;
    }
    return ts;
}

}} // namespace
