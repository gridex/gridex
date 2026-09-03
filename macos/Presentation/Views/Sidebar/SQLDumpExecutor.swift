import Foundation

enum SQLDumpExecutor {
    static func execute(
        statements: [String],
        schema: String?,
        using adapter: any DatabaseAdapter
    ) async -> ImportSQLResult {
        if adapter.databaseType == .postgresql, let schema {
            do {
                try await adapter.executeStatements(statements, inSchemaTransaction: schema)
                return ImportSQLResult(success: statements.count, total: statements.count, firstError: nil)
            } catch {
                return ImportSQLResult(success: 0, total: statements.count, firstError: error.localizedDescription)
            }
        }

        var successCount = 0
        var firstError: String?
        for statement in statements {
            do {
                _ = try await adapter.executeRaw(sql: statement)
                successCount += 1
            } catch {
                if firstError == nil {
                    firstError = error.localizedDescription
                }
            }
        }

        return ImportSQLResult(success: successCount, total: statements.count, firstError: firstError)
    }
}
