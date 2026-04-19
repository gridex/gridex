// MCPServer.swift
// Gridex
//
// Main MCP Server actor that handles all MCP protocol communication.

import Foundation

enum MCPTransportMode {
    case stdio      // CLI mode - uses stdin/stdout
    case httpOnly   // GUI mode - HTTP transport only (no blocking stdin)
}

actor MCPServer: StdioTransportDelegate {
    private let transport: StdioTransport?
    private let toolRegistry: MCPToolRegistry
    private let permissionEngine: MCPPermissionEngine
    private let auditLogger: MCPAuditLogger
    private let rateLimiter: MCPRateLimiter
    private let approvalGate: MCPApprovalGate

    private let connectionManager: ConnectionManager
    private let connectionRepository: any ConnectionRepository

    private var isRunning = false
    private var clientInfo: MCPClientInfo?

    private let serverVersion: String
    private let transportMode: MCPTransportMode

    init(
        connectionManager: ConnectionManager,
        connectionRepository: any ConnectionRepository,
        serverVersion: String = "1.0.0",
        transportMode: MCPTransportMode = .httpOnly
    ) {
        self.connectionManager = connectionManager
        self.connectionRepository = connectionRepository
        self.serverVersion = serverVersion
        self.transportMode = transportMode

        // Only create stdio transport in CLI mode
        self.transport = transportMode == .stdio ? StdioTransport() : nil
        self.toolRegistry = MCPToolRegistry()
        self.permissionEngine = MCPPermissionEngine()
        self.auditLogger = MCPAuditLogger()
        self.rateLimiter = MCPRateLimiter()
        self.approvalGate = MCPApprovalGate()
    }

    // MARK: - Lifecycle

    func start() async {
        guard !isRunning else { return }
        isRunning = true

        if let transport = transport {
            await transport.setDelegate(self)
            await transport.start()
        }
        // TODO: Start HTTP transport if httpEnabled
    }

    func stop() async {
        isRunning = false
        if let transport = transport {
            await transport.stop()
        }
        await auditLogger.close()
    }

    // MARK: - Permission Management

    func setConnectionMode(_ mode: MCPConnectionMode, for connectionId: UUID) async {
        await permissionEngine.setMode(mode, for: connectionId)
    }

    func getConnectionMode(for connectionId: UUID) async -> MCPConnectionMode {
        await permissionEngine.getMode(for: connectionId)
    }

    // MARK: - StdioTransportDelegate

    nonisolated func transport(_ transport: StdioTransport, didReceiveRequest request: JSONRPCRequest) async {
        await handleRequest(request)
    }

    // MARK: - Request Handling

    private func handleRequest(_ request: JSONRPCRequest) async {
        let response: JSONRPCResponse

        switch request.method {
        case "initialize":
            response = await handleInitialize(request)

        case "initialized":
            // Client acknowledges initialization, no response needed
            return

        case "tools/list":
            response = await handleToolsList(request)

        case "tools/call":
            response = await handleToolCall(request)

        case "ping":
            response = JSONRPCResponse(id: request.id, result: .object(["pong": .bool(true)]))

        case "shutdown":
            response = JSONRPCResponse(id: request.id, result: .null)
            await transport?.send(response)
            await stop()
            return

        default:
            response = JSONRPCResponse(id: request.id, error: .methodNotFound)
        }

        await transport?.send(response)
    }

    private func handleInitialize(_ request: JSONRPCRequest) async -> JSONRPCResponse {
        // Extract client info
        if let params = request.params,
           let clientInfoObj = params["clientInfo"]?.objectValue {
            let name = clientInfoObj["name"]?.stringValue ?? "unknown"
            let version = clientInfoObj["version"]?.stringValue ?? "0.0.0"
            clientInfo = MCPClientInfo(name: name, version: version)
        }

        let serverInfo = MCPServerInfo.gridex(version: serverVersion)

        let result: JSONValue = .object([
            "serverInfo": .object([
                "name": .string(serverInfo.name),
                "version": .string(serverInfo.version)
            ]),
            "protocolVersion": .string(serverInfo.protocolVersion),
            "capabilities": .object([
                "tools": .object(["listChanged": .bool(true)]),
                "resources": .object(["subscribe": .bool(true), "listChanged": .bool(true)]),
                "prompts": .object(["listChanged": .bool(true)]),
                "logging": .object([:])
            ])
        ])

        return JSONRPCResponse(id: request.id, result: result)
    }

    private func handleToolsList(_ request: JSONRPCRequest) async -> JSONRPCResponse {
        let definitions = await toolRegistry.definitions()

        var toolsArray: [JSONValue] = []
        for def in definitions {
            toolsArray.append(.object([
                "name": .string(def.name),
                "description": .string(def.description),
                "inputSchema": def.inputSchema
            ]))
        }

        let result: JSONValue = .object([
            "tools": .array(toolsArray)
        ])

        return JSONRPCResponse(id: request.id, result: result)
    }

    private func handleToolCall(_ request: JSONRPCRequest) async -> JSONRPCResponse {
        guard let params = request.params,
              let toolName = params["name"]?.stringValue else {
            return JSONRPCResponse(id: request.id, error: .invalidParams)
        }

        let toolParams = params["arguments"] ?? .object([:])

        guard let tool = await toolRegistry.get(toolName) else {
            let error = JSONRPCError(
                code: MCPErrorCode.notFound.rawValue,
                message: "Tool '\(toolName)' not found"
            )
            return JSONRPCResponse(id: request.id, error: error)
        }

        let startTime = Date()
        let client = MCPAuditClient(
            name: clientInfo?.name ?? "unknown",
            version: clientInfo?.version ?? "0.0.0",
            transport: "stdio"
        )

        // Extract connection ID for audit
        var connectionId: UUID?
        var connectionType: DatabaseType?
        if let connIdStr = toolParams["connection_id"]?.stringValue,
           let connId = UUID(uuidString: connIdStr) {
            connectionId = connId
            if let conn = await connectionManager.activeConnection(for: connId) {
                connectionType = conn.config.databaseType
            }
        }

        let context = MCPToolContext(
            connectionManager: connectionManager,
            permissionEngine: permissionEngine,
            auditLogger: auditLogger,
            rateLimiter: rateLimiter,
            approvalGate: approvalGate,
            client: client,
            connectionRepository: connectionRepository
        )

        do {
            let toolResult = try await tool.execute(params: toolParams, context: context)
            let durationMs = Int(Date().timeIntervalSince(startTime) * 1000)

            // Get connection mode for audit
            var connMode: MCPConnectionMode = .locked
            if let connId = connectionId {
                connMode = await permissionEngine.getMode(for: connId)
            }

            // Log to audit
            let auditEntry = MCPAuditEntry(
                tool: toolName,
                tier: tool.tier,
                connectionId: connectionId,
                connectionType: connectionType,
                client: client,
                input: MCPAuditInput(
                    sql: toolParams["sql"]?.stringValue,
                    paramsCount: toolParams["params"]?.arrayValue?.count
                ),
                result: MCPAuditResult(
                    status: toolResult.isError == true ? .error : .success,
                    durationMs: durationMs
                ),
                security: MCPAuditSecurity(mode: connMode)
            )
            await auditLogger.log(auditEntry)

            // Build response
            var contentArray: [JSONValue] = []
            for content in toolResult.content {
                var contentObj: [String: JSONValue] = ["type": .string(content.type)]
                if let text = content.text { contentObj["text"] = .string(text) }
                if let data = content.data { contentObj["data"] = .string(data) }
                if let mimeType = content.mimeType { contentObj["mimeType"] = .string(mimeType) }
                contentArray.append(.object(contentObj))
            }

            var resultObj: [String: JSONValue] = ["content": .array(contentArray)]
            if toolResult.isError == true {
                resultObj["isError"] = .bool(true)
            }

            return JSONRPCResponse(id: request.id, result: .object(resultObj))

        } catch {
            let durationMs = Int(Date().timeIntervalSince(startTime) * 1000)
            let errorMessage = sanitizeError(error)

            // Get connection mode for audit
            var errorConnMode: MCPConnectionMode = .locked
            if let connId = connectionId {
                errorConnMode = await permissionEngine.getMode(for: connId)
            }

            // Log error to audit
            let auditEntry = MCPAuditEntry(
                tool: toolName,
                tier: tool.tier,
                connectionId: connectionId,
                connectionType: connectionType,
                client: client,
                input: MCPAuditInput(
                    sql: toolParams["sql"]?.stringValue,
                    paramsCount: toolParams["params"]?.arrayValue?.count
                ),
                result: MCPAuditResult(status: .error, durationMs: durationMs),
                security: MCPAuditSecurity(mode: errorConnMode),
                error: errorMessage
            )
            await auditLogger.log(auditEntry)

            // Return error as tool result (not JSON-RPC error)
            let toolResult = MCPToolResult.error(errorMessage)
            var contentArray: [JSONValue] = []
            for content in toolResult.content {
                contentArray.append(.object([
                    "type": .string(content.type),
                    "text": .string(content.text ?? "")
                ]))
            }

            return JSONRPCResponse(
                id: request.id,
                result: .object([
                    "content": .array(contentArray),
                    "isError": .bool(true)
                ])
            )
        }
    }

    private func sanitizeError(_ error: Error) -> String {
        // Never expose sensitive information
        let message = error.localizedDescription

        // Remove potential file paths
        let sanitized = message
            .replacingOccurrences(of: #"/Users/[^/\s]+"#, with: "[path]", options: .regularExpression)
            .replacingOccurrences(of: #"/home/[^/\s]+"#, with: "[path]", options: .regularExpression)

        // Remove potential connection strings
        let noConnStrings = sanitized
            .replacingOccurrences(of: #"postgres://[^\s]+"#, with: "[connection]", options: .regularExpression)
            .replacingOccurrences(of: #"mysql://[^\s]+"#, with: "[connection]", options: .regularExpression)
            .replacingOccurrences(of: #"mongodb://[^\s]+"#, with: "[connection]", options: .regularExpression)

        return noConnStrings
    }

    // MARK: - Accessors for testing

    var auditLog: MCPAuditLogger { auditLogger }
    var tools: MCPToolRegistry { toolRegistry }
}
