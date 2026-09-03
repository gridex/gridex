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

struct RedisKeyScanPage: Sendable, Equatable {
    let cursor: String?
    let keys: [String]?
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

enum RedisKeyScanLoop {
    static func run(
        maximumCount: Int,
        pageBudget: Int,
        fetchPage: @Sendable (String) async throws -> RedisKeyScanPage?
    ) async throws -> RedisKeyScanResult {
        precondition(pageBudget > 0)

        var cursor = "0"
        var scannedPageCount = 0
        var accumulator = RedisKeyScanAccumulator(maximumCount: maximumCount)

        while true {
            try Task.checkCancellation()
            let page = try await fetchPage(cursor)
            try Task.checkCancellation()

            guard let page,
                  let nextCursor = page.cursor,
                  let cursorValue = UInt64(nextCursor),
                  let keys = page.keys else {
                throw GridexError.queryExecutionFailed("Malformed SCAN response")
            }

            accumulator.append(keys)
            scannedPageCount += 1

            let hasMoreCursor = cursorValue != 0
            if !hasMoreCursor || accumulator.isFull || scannedPageCount >= pageBudget {
                return accumulator.result(hasMoreCursor: hasMoreCursor)
            }

            cursor = nextCursor
        }
    }
}

struct RedisDatabaseSelectionState: Sendable, Equatable {
    private(set) var currentDatabase: Int

    init(initialDatabase: Int) {
        currentDatabase = initialDatabase
    }

    mutating func recordSuccessfulSelect(arguments: [String]) {
        guard arguments.count == 1,
              let selectedDatabase = Int(arguments[0]),
              selectedDatabase >= 0 else {
            return
        }
        currentDatabase = selectedDatabase
    }
}
