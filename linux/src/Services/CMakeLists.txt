include(FetchContent)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh)

find_package(Qt6 6.4 REQUIRED COMPONENTS Network)

# nlohmann/json — header-only, fetched at configure time.
FetchContent_Declare(json
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(json)

add_library(gridex-services STATIC
    SSH/SSHTunnelManager.cpp
    AI/AIServiceFactory.cpp
    AI/Providers/AnthropicProvider.cpp
    AI/Providers/OpenAIProvider.cpp
    AI/Providers/OllamaProvider.cpp
    AI/Providers/GeminiProvider.cpp
    Export/ExportService.cpp
    Export/DatabaseDumpRunner.cpp
    MCP/MCPServer.cpp
    MCP/Audit/AuditLogger.cpp
    MCP/Security/PermissionEngine.cpp
    MCP/Security/RateLimiter.cpp
    MCP/Security/ApprovalGate.cpp
    MCP/Security/RowCountEstimator.cpp
    MCP/Transport/StdioTransport.cpp
    MCP/Tools/ToolRegistry.cpp
    MCP/Tools/Tier1/ListConnectionsTool.cpp
    MCP/Tools/Tier1/ListTablesTool.cpp
    MCP/Tools/Tier1/DescribeTableTool.cpp
    MCP/Tools/Tier1/ListSchemasTool.cpp
    MCP/Tools/Tier1/GetSampleRowsTool.cpp
    MCP/Tools/Tier1/ListRelationshipsTool.cpp
    MCP/Tools/Tier2/QueryTool.cpp
    MCP/Tools/Tier2/ExplainQueryTool.cpp
    MCP/Tools/Tier2/SearchAcrossTablesTool.cpp
    MCP/Tools/Tier3/InsertRowsTool.cpp
    MCP/Tools/Tier3/UpdateRowsTool.cpp
    MCP/Tools/Tier3/DeleteRowsTool.cpp
    MCP/Tools/Tier3/ExecuteWriteQueryTool.cpp
    Update/UpdateService.cpp
)

target_compile_definitions(gridex-services PUBLIC
    GRIDEX_VERSION="${PROJECT_VERSION}"
)

target_include_directories(gridex-services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(gridex-services
    PUBLIC  gridex-core
            gridex-data
            Qt6::Network
            nlohmann_json::nlohmann_json
    PRIVATE PkgConfig::LIBSSH
)
