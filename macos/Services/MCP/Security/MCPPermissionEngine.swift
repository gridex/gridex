// MCPPermissionEngine.swift
// Gridex
//
// Validates MCP tool permissions against connection modes.

import Foundation

actor MCPPermissionEngine {
    private var connectionModes: [UUID: MCPConnectionMode] = [:]

    func setMode(_ mode: MCPConnectionMode, for connectionId: UUID) {
        connectionModes[connectionId] = mode
    }

    func getMode(for connectionId: UUID) -> MCPConnectionMode {
        connectionModes[connectionId] ?? .locked
    }

    func removeMode(for connectionId: UUID) {
        connectionModes[connectionId] = nil
    }

    func checkPermission(tier: MCPPermissionTier, connectionId: UUID) -> MCPPermissionResult {
        let mode = getMode(for: connectionId)
        return checkPermission(tier: tier, mode: mode)
    }

    func checkPermission(tier: MCPPermissionTier, mode: MCPConnectionMode) -> MCPPermissionResult {
        switch tier {
        case .schema:
            return mode.allowsTier1 ? .allowed : .denied("Connection is locked. MCP access is disabled.")

        case .read:
            return mode.allowsTier2 ? .allowed : .denied("Connection is locked. MCP access is disabled.")

        case .write:
            if !mode.allowsTier3 {
                return .denied("This operation requires read-write mode. Ask the user to enable it in Connection Settings > MCP Access.")
            }
            return .requiresApproval

        case .ddl:
            if !mode.allowsTier4 {
                return .denied("DDL operations require read-write mode. Ask the user to enable it in Connection Settings > MCP Access.")
            }
            return .requiresApproval

        case .advanced:
            return mode.allowsTier5 ? .allowed : .denied("Connection is locked. MCP access is disabled.")
        }
    }

    func validateReadOnlyQuery(_ sql: String) -> MCPPermissionResult {
        let code = MCPSQLSanitizer.stripCommentsAndStrings(sql)
        let trimmedUpper = code.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()

        // One optional trailing ';' is fine; anything else is multi-statement.
        let withoutTrailingSemi = trimmedUpper.hasSuffix(";")
            ? String(trimmedUpper.dropLast()).trimmingCharacters(in: .whitespacesAndNewlines)
            : trimmedUpper
        if withoutTrailingSemi.contains(";") {
            return .denied("Multiple statements are not allowed in read-only mode.")
        }

        let isReadOnly = Self.readOnlyPrefixes.contains { trimmedUpper.hasPrefix($0) }
        if !isReadOnly {
            return .denied("Only SELECT queries are allowed in read-only mode. This query appears to modify data.")
        }

        let range = NSRange(code.startIndex..., in: code)
        if let match = Self.dangerousKeywordRegex.firstMatch(in: code, options: [], range: range),
           let matchRange = Range(match.range, in: code) {
            let hit = String(code[matchRange]).uppercased()
            return .denied("Query contains '\(hit)' which is not allowed in read-only mode.")
        }

        return .allowed
    }

    private static let readOnlyPrefixes = ["SELECT", "SHOW", "EXPLAIN", "DESCRIBE", "DESC", "WITH"]

    private static let dangerousKeywords = [
        // DML / DDL
        "INSERT", "UPDATE", "DELETE", "MERGE", "UPSERT",
        "DROP", "CREATE", "ALTER", "TRUNCATE", "RENAME",
        "GRANT", "REVOKE",
        // Procedural execution
        "CALL", "EXEC", "EXECUTE", "DO",
        // Sequence / state mutations
        "NEXTVAL", "SETVAL",
        // PostgreSQL filesystem / server access
        "LO_IMPORT", "LO_EXPORT",
        "PG_READ_SERVER_FILES", "PG_WRITE_SERVER_FILES",
        "PG_READ_BINARY_FILE", "PG_LS_DIR",
        "DBLINK", "DBLINK_EXEC",
        // Bulk / admin
        "COPY", "VACUUM", "ANALYZE", "REINDEX", "CLUSTER", "REFRESH",
        "LOCK", "UNLOCK",
        // Session state
        "SET", "RESET", "BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT"
    ]

    private static let dangerousKeywordRegex: NSRegularExpression = {
        let alternation = dangerousKeywords
            .map { NSRegularExpression.escapedPattern(for: $0) }
            .joined(separator: "|")
        return try! NSRegularExpression(pattern: "\\b(\(alternation))\\b", options: .caseInsensitive)
    }()

    /// Validate a WHERE clause supplied by an MCP client.
    /// Blocks statement terminators, SQL comments, and trivial tautologies.
    func validateWhereClause(_ whereClause: String?) -> MCPPermissionResult {
        guard let whereClause else {
            return .denied("WHERE clause is required for UPDATE/DELETE operations. Bare UPDATE/DELETE without WHERE is not allowed.")
        }
        let trimmed = whereClause.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            return .denied("WHERE clause is required for UPDATE/DELETE operations.")
        }

        // Reject injection vectors outright
        if trimmed.contains(";") {
            return .denied("WHERE clause must not contain ';' — statement terminators are forbidden.")
        }
        if trimmed.contains("--") {
            return .denied("WHERE clause must not contain '--' — SQL line comments are forbidden.")
        }
        if trimmed.contains("/*") || trimmed.contains("*/") {
            return .denied("WHERE clause must not contain '/*' or '*/' — SQL block comments are forbidden.")
        }

        // Reject trivial predicates (exact match after whitespace removal)
        let compact = trimmed.uppercased()
            .components(separatedBy: .whitespacesAndNewlines).joined()
        let trivials: Set<String> = [
            "1=1", "TRUE", "1", "'1'='1'",
            "1<>0", "0=0", "2>1", "TRUE=TRUE", "NULLISNULL"
        ]
        if trivials.contains(compact) {
            return .denied("Trivial WHERE clause '\(trimmed)' is not allowed. Provide a meaningful predicate.")
        }

        return .allowed
    }
}

enum MCPPermissionResult: Sendable {
    case allowed
    case requiresApproval
    case denied(String)

    var isAllowed: Bool {
        if case .allowed = self { return true }
        return false
    }

    var requiresUserApproval: Bool {
        if case .requiresApproval = self { return true }
        return false
    }

    var errorMessage: String? {
        if case .denied(let msg) = self { return msg }
        return nil
    }
}
