//
// MCPSQLSanitizer.cpp
//

#include "MCPSQLSanitizer.h"

namespace DBModels { namespace MCPSQLSanitizer {

std::string stripCommentsAndStrings(const std::string& sql)
{
    std::string result;
    result.reserve(sql.size());
    const size_t n = sql.size();
    size_t i = 0;

    while (i < n)
    {
        const char c = sql[i];
        const char next = (i + 1 < n) ? sql[i + 1] : '\0';

        // -- line comment: consume until newline
        if (c == '-' && next == '-')
        {
            while (i < n && sql[i] != '\n') ++i;
            continue;
        }

        // /* block comment */
        if (c == '/' && next == '*')
        {
            i += 2;
            while (i < n)
            {
                if (sql[i] == '*' && i + 1 < n && sql[i + 1] == '/')
                {
                    i += 2;
                    break;
                }
                ++i;
            }
            continue;
        }

        // 'single-quoted literal' with '' escape support
        if (c == '\'')
        {
            ++i; // skip opening quote
            while (i < n)
            {
                if (sql[i] == '\'')
                {
                    if (i + 1 < n && sql[i + 1] == '\'')
                    {
                        // escaped '' inside literal — keep scanning
                        i += 2;
                        continue;
                    }
                    ++i; // consume closing quote
                    break;
                }
                ++i;
            }
            result.push_back(' ');
            continue;
        }

        result.push_back(c);
        ++i;
    }

    return result;
}

}} // namespace
