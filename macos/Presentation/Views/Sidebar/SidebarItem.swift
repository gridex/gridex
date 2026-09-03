// SidebarItem.swift
// Gridex

import Foundation

class SidebarItem: Identifiable {
    let id: UUID
    let title: String
    let type: SidebarItemType
    let schema: String?
    let iconName: String
    var children: [SidebarItem]
    var badge: String?
    var isFavorite: Bool

    init(
        id: UUID = UUID(),
        title: String,
        type: SidebarItemType,
        schema: String? = nil,
        iconName: String = "",
        children: [SidebarItem] = [],
        badge: String? = nil,
        isFavorite: Bool = false
    ) {
        self.id = id
        self.title = title
        self.type = type
        self.schema = schema
        self.iconName = iconName
        self.children = children
        self.badge = badge
        self.isFavorite = isFavorite
    }
}

enum SidebarItemType: Hashable {
    case connection(ConnectionConfig)
    case database(String)
    case schema(String)
    case table(String)
    case view(String)
    case materializedView(String)
    case function(String)
    case procedure(String)
    case enumType(String)
    case group(String)

    static func == (lhs: SidebarItemType, rhs: SidebarItemType) -> Bool {
        switch (lhs, rhs) {
        case (.connection(let a), .connection(let b)): return a.id == b.id
        case (.database(let a), .database(let b)): return a == b
        case (.schema(let a), .schema(let b)): return a == b
        case (.table(let a), .table(let b)): return a == b
        case (.view(let a), .view(let b)): return a == b
        case (.materializedView(let a), .materializedView(let b)): return a == b
        case (.function(let a), .function(let b)): return a == b
        case (.procedure(let a), .procedure(let b)): return a == b
        case (.enumType(let a), .enumType(let b)): return a == b
        case (.group(let a), .group(let b)): return a == b
        default: return false
        }
    }

    func hash(into hasher: inout Hasher) {
        switch self {
        case .connection(let c): hasher.combine("connection"); hasher.combine(c.id)
        case .database(let s): hasher.combine("database"); hasher.combine(s)
        case .schema(let s): hasher.combine("schema"); hasher.combine(s)
        case .table(let s): hasher.combine("table"); hasher.combine(s)
        case .view(let s): hasher.combine("view"); hasher.combine(s)
        case .materializedView(let s): hasher.combine("matview"); hasher.combine(s)
        case .function(let s): hasher.combine("function"); hasher.combine(s)
        case .procedure(let s): hasher.combine("procedure"); hasher.combine(s)
        case .enumType(let s): hasher.combine("enum"); hasher.combine(s)
        case .group(let s): hasher.combine("group"); hasher.combine(s)
        }
    }
}
