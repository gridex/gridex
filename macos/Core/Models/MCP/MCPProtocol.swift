// MCPProtocol.swift
// Gridex
//
// MCP (Model Context Protocol) JSON-RPC 2.0 types.

import Foundation

// MARK: - JSON-RPC 2.0 Base Types

struct JSONRPCRequest: Codable, Sendable {
    let jsonrpc: String
    let id: JSONRPCId?
    let method: String
    let params: JSONValue?

    init(id: JSONRPCId? = nil, method: String, params: JSONValue? = nil) {
        self.jsonrpc = "2.0"
        self.id = id
        self.method = method
        self.params = params
    }
}

struct JSONRPCResponse: Codable, Sendable {
    let jsonrpc: String
    let id: JSONRPCId?
    let result: JSONValue?
    let error: JSONRPCError?

    init(id: JSONRPCId?, result: JSONValue) {
        self.jsonrpc = "2.0"
        self.id = id
        self.result = result
        self.error = nil
    }

    init(id: JSONRPCId?, error: JSONRPCError) {
        self.jsonrpc = "2.0"
        self.id = id
        self.result = nil
        self.error = error
    }
}

struct JSONRPCError: Codable, Sendable {
    let code: Int
    let message: String
    let data: JSONValue?

    init(code: Int, message: String, data: JSONValue? = nil) {
        self.code = code
        self.message = message
        self.data = data
    }

    static let parseError = JSONRPCError(code: -32700, message: "Parse error")
    static let invalidRequest = JSONRPCError(code: -32600, message: "Invalid Request")
    static let methodNotFound = JSONRPCError(code: -32601, message: "Method not found")
    static let invalidParams = JSONRPCError(code: -32602, message: "Invalid params")
    static let internalError = JSONRPCError(code: -32603, message: "Internal error")
}

enum JSONRPCId: Codable, Sendable, Hashable {
    case string(String)
    case int(Int)

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if let intVal = try? container.decode(Int.self) {
            self = .int(intVal)
        } else if let strVal = try? container.decode(String.self) {
            self = .string(strVal)
        } else {
            throw DecodingError.typeMismatch(
                JSONRPCId.self,
                DecodingError.Context(codingPath: decoder.codingPath, debugDescription: "Expected string or int")
            )
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .string(let s): try container.encode(s)
        case .int(let i): try container.encode(i)
        }
    }
}

// MARK: - JSON Value (Dynamic)

indirect enum JSONValue: Codable, Sendable, Hashable {
    case null
    case bool(Bool)
    case int(Int)
    case double(Double)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            self = .null
        } else if let b = try? container.decode(Bool.self) {
            self = .bool(b)
        } else if let i = try? container.decode(Int.self) {
            self = .int(i)
        } else if let d = try? container.decode(Double.self) {
            self = .double(d)
        } else if let s = try? container.decode(String.self) {
            self = .string(s)
        } else if let arr = try? container.decode([JSONValue].self) {
            self = .array(arr)
        } else if let obj = try? container.decode([String: JSONValue].self) {
            self = .object(obj)
        } else {
            throw DecodingError.typeMismatch(
                JSONValue.self,
                DecodingError.Context(codingPath: decoder.codingPath, debugDescription: "Unknown JSON type")
            )
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .null: try container.encodeNil()
        case .bool(let b): try container.encode(b)
        case .int(let i): try container.encode(i)
        case .double(let d): try container.encode(d)
        case .string(let s): try container.encode(s)
        case .array(let arr): try container.encode(arr)
        case .object(let obj): try container.encode(obj)
        }
    }

    subscript(key: String) -> JSONValue? {
        if case .object(let obj) = self { return obj[key] }
        return nil
    }

    subscript(index: Int) -> JSONValue? {
        if case .array(let arr) = self, index < arr.count { return arr[index] }
        return nil
    }

    var stringValue: String? {
        if case .string(let s) = self { return s }
        return nil
    }

    var intValue: Int? {
        if case .int(let i) = self { return i }
        return nil
    }

    var boolValue: Bool? {
        if case .bool(let b) = self { return b }
        return nil
    }

    var arrayValue: [JSONValue]? {
        if case .array(let a) = self { return a }
        return nil
    }

    var objectValue: [String: JSONValue]? {
        if case .object(let o) = self { return o }
        return nil
    }
}

// MARK: - MCP Server Info

struct MCPServerInfo: Codable, Sendable {
    let name: String
    let version: String
    let protocolVersion: String
    let capabilities: MCPCapabilities

    static func gridex(version: String) -> MCPServerInfo {
        MCPServerInfo(
            name: "gridex",
            version: version,
            protocolVersion: "2024-11-05",
            capabilities: MCPCapabilities(
                tools: MCPToolsCapability(listChanged: true),
                resources: MCPResourcesCapability(subscribe: true, listChanged: true),
                prompts: MCPPromptsCapability(listChanged: true),
                logging: MCPLoggingCapability()
            )
        )
    }
}

struct MCPCapabilities: Codable, Sendable {
    let tools: MCPToolsCapability?
    let resources: MCPResourcesCapability?
    let prompts: MCPPromptsCapability?
    let logging: MCPLoggingCapability?
}

struct MCPToolsCapability: Codable, Sendable {
    let listChanged: Bool?
}

struct MCPResourcesCapability: Codable, Sendable {
    let subscribe: Bool?
    let listChanged: Bool?
}

struct MCPPromptsCapability: Codable, Sendable {
    let listChanged: Bool?
}

struct MCPLoggingCapability: Codable, Sendable {}

// MARK: - MCP Tool Definition

struct MCPToolDefinition: Codable, Sendable {
    let name: String
    let description: String
    let inputSchema: JSONValue

    init(name: String, description: String, inputSchema: [String: Any]) {
        self.name = name
        self.description = description
        self.inputSchema = Self.convertToJSONValue(inputSchema)
    }

    private static func convertToJSONValue(_ value: Any) -> JSONValue {
        switch value {
        case let b as Bool: return .bool(b)
        case let i as Int: return .int(i)
        case let d as Double: return .double(d)
        case let s as String: return .string(s)
        case let arr as [Any]: return .array(arr.map { convertToJSONValue($0) })
        case let dict as [String: Any]: return .object(dict.mapValues { convertToJSONValue($0) })
        default: return .null
        }
    }
}

// MARK: - MCP Tool Result

struct MCPToolResult: Codable, Sendable {
    let content: [MCPContent]
    let isError: Bool?

    init(text: String, isError: Bool = false) {
        self.content = [MCPContent(type: "text", text: text)]
        self.isError = isError ? true : nil
    }

    init(content: [MCPContent], isError: Bool = false) {
        self.content = content
        self.isError = isError ? true : nil
    }

    static func error(_ message: String) -> MCPToolResult {
        MCPToolResult(text: message, isError: true)
    }
}

struct MCPContent: Codable, Sendable {
    let type: String
    let text: String?
    let data: String?
    let mimeType: String?

    init(type: String, text: String? = nil, data: String? = nil, mimeType: String? = nil) {
        self.type = type
        self.text = text
        self.data = data
        self.mimeType = mimeType
    }
}

// MARK: - MCP Client Info

struct MCPClientInfo: Codable, Sendable {
    let name: String
    let version: String
}

// MARK: - MCP Error Codes

enum MCPErrorCode: Int, Sendable {
    case permissionDenied = -32001
    case approvalTimeout = -32002
    case approvalDenied = -32003
    case connectionError = -32004
    case syntaxError = -32005
    case notFound = -32006
    case rateLimitExceeded = -32007
    case scopeDenied = -32008
    case queryTimeout = -32009
}
