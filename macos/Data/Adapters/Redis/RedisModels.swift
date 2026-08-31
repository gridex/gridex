// RedisModels.swift
// Gridex
//
// Supporting types for Redis-specific features.

import Foundation

enum RedisKeyType: String, CaseIterable, Sendable {
    case string, hash, list, set, zset
}

enum RedisKeyData: Sendable {
    case string(value: String)
    case hash(fields: [(field: String, value: String)])
    case list(items: [String])
    case set(members: [String])
    case zset(members: [(member: String, score: Double)])
}

struct RedisKeyDetail: Sendable {
    let key: String
    let type: RedisKeyType
    let ttl: Int?        // nil = no expiry
    let data: RedisKeyData
    let memoryBytes: Int?
}

struct RedisInfoSection: Sendable {
    let name: String
    let entries: [(key: String, value: String)]
}

struct RedisSlowLogEntry: Identifiable, Sendable {
    let id: Int
    let timestamp: Date
    let durationMicros: Int
    let command: String
    let clientInfo: String
}

struct RedisKeyScanResult: Sendable, Equatable {
    let keys: [String]
    let isTruncated: Bool
}

struct RedisKeyScanAccumulator {
    private(set) var uniqueKeys: Set<String> = []
    private let maximumCount: Int
    private var discardedUniqueKey = false

    init(maximumCount: Int) {
        precondition(maximumCount > 0)
        self.maximumCount = maximumCount
    }

    var isFull: Bool { uniqueKeys.count >= maximumCount }

    mutating func append(_ keys: [String]) {
        for key in keys where !uniqueKeys.contains(key) {
            if uniqueKeys.count < maximumCount {
                uniqueKeys.insert(key)
            } else {
                discardedUniqueKey = true
            }
        }
    }

    func result(hasMoreCursor: Bool) -> RedisKeyScanResult {
        RedisKeyScanResult(
            keys: uniqueKeys.sorted(),
            isTruncated: hasMoreCursor || discardedUniqueKey
        )
    }
}
