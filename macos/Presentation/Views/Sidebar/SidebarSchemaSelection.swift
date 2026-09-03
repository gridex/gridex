// SidebarSchemaSelection.swift
// Gridex

enum SidebarSchemaSelection {
    static func resolve(
        previous: String?,
        for databaseType: DatabaseType,
        schemas: [String]
    ) -> String? {
        guard databaseType == .postgresql else { return nil }
        let available = Array(Set(schemas)).sorted()
        if let previous, available.contains(previous) { return previous }
        if available.contains("public") { return "public" }
        return available.first
    }
}
