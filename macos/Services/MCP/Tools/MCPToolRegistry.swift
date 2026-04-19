// MCPToolRegistry.swift
// Gridex
//
// Registry for all available MCP tools.

import Foundation

actor MCPToolRegistry {
    private var tools: [String: any MCPTool] = [:]

    init() {
        // Register default tools synchronously during init
        // Tier 1: Schema Introspection
        tools[ListConnectionsTool().name] = ListConnectionsTool()
        tools[ListTablesTool().name] = ListTablesTool()
        tools[DescribeTableTool().name] = DescribeTableTool()
        tools[ListSchemasTool().name] = ListSchemasTool()
        tools[GetSampleRowsTool().name] = GetSampleRowsTool()
        tools[ListRelationshipsTool().name] = ListRelationshipsTool()

        // Tier 2: Query Execution
        tools[QueryTool().name] = QueryTool()
        tools[ExplainQueryTool().name] = ExplainQueryTool()
        tools[SearchAcrossTablesTool().name] = SearchAcrossTablesTool()

        // Tier 3: Data Modification (requires approval)
        tools[InsertRowsTool().name] = InsertRowsTool()
        tools[UpdateRowsTool().name] = UpdateRowsTool()
        tools[DeleteRowsTool().name] = DeleteRowsTool()
        tools[ExecuteWriteQueryTool().name] = ExecuteWriteQueryTool()
    }

    private func registerDefaultToolsAsync() {
        // Tier 1: Schema Introspection
        register(ListConnectionsTool())
        register(ListTablesTool())
        register(DescribeTableTool())
        register(ListSchemasTool())
        register(GetSampleRowsTool())
        register(ListRelationshipsTool())

        // Tier 2: Query Execution
        register(QueryTool())
        register(ExplainQueryTool())
        register(SearchAcrossTablesTool())
    }

    func register(_ tool: any MCPTool) {
        tools[tool.name] = tool
    }

    func unregister(_ name: String) {
        tools[name] = nil
    }

    func get(_ name: String) -> (any MCPTool)? {
        tools[name]
    }

    func allTools() -> [any MCPTool] {
        Array(tools.values)
    }

    func definitions() -> [MCPToolDefinition] {
        tools.values.map { $0.definition() }
    }
}
