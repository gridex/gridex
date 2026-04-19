// MCPRateLimiter.swift
// Gridex
//
// Rate limiter for MCP operations.

import Foundation

actor MCPRateLimiter {
    private var queryCount: [UUID: RateBucket] = [:]
    private var writeCount: [UUID: RateBucket] = [:]
    private var ddlCount: [UUID: RateBucket] = [:]

    // Configurable limits (can be overridden via UserDefaults)
    var queriesPerMinute: Int { UserDefaults.standard.integer(forKey: "mcp.rateLimit.queriesPerMinute").nonZero ?? 60 }
    var queriesPerHour: Int { UserDefaults.standard.integer(forKey: "mcp.rateLimit.queriesPerHour").nonZero ?? 1000 }
    var writesPerMinute: Int { UserDefaults.standard.integer(forKey: "mcp.rateLimit.writesPerMinute").nonZero ?? 10 }
    var ddlPerMinute: Int { UserDefaults.standard.integer(forKey: "mcp.rateLimit.ddlPerMinute").nonZero ?? 1 }

    func checkLimit(tier: MCPPermissionTier, connectionId: UUID) -> RateLimitResult {
        let now = Date()

        switch tier {
        case .schema, .read, .advanced:
            return checkQueryLimit(connectionId: connectionId, now: now)
        case .write:
            return checkWriteLimit(connectionId: connectionId, now: now)
        case .ddl:
            return checkDDLLimit(connectionId: connectionId, now: now)
        }
    }

    func recordUsage(tier: MCPPermissionTier, connectionId: UUID) {
        let now = Date()

        switch tier {
        case .schema, .read, .advanced:
            recordQuery(connectionId: connectionId, now: now)
        case .write:
            recordWrite(connectionId: connectionId, now: now)
        case .ddl:
            recordDDL(connectionId: connectionId, now: now)
        }
    }

    // MARK: - Query Rate Limiting

    private func checkQueryLimit(connectionId: UUID, now: Date) -> RateLimitResult {
        let bucket = queryCount[connectionId] ?? RateBucket()

        // Check per-minute limit
        let minuteCount = bucket.countInWindow(now: now, windowSeconds: 60)
        if minuteCount >= queriesPerMinute {
            let retryAfter = bucket.oldestInWindow(now: now, windowSeconds: 60)
                .map { 60 - Int(now.timeIntervalSince($0)) } ?? 60
            return .exceeded(retryAfter: max(1, retryAfter))
        }

        // Check per-hour limit
        let hourCount = bucket.countInWindow(now: now, windowSeconds: 3600)
        if hourCount >= queriesPerHour {
            let retryAfter = bucket.oldestInWindow(now: now, windowSeconds: 3600)
                .map { 3600 - Int(now.timeIntervalSince($0)) } ?? 3600
            return .exceeded(retryAfter: max(1, retryAfter))
        }

        return .allowed
    }

    private func recordQuery(connectionId: UUID, now: Date) {
        var bucket = queryCount[connectionId] ?? RateBucket()
        bucket.record(now: now)
        bucket.cleanup(now: now, maxAge: 3600) // Keep 1 hour of history
        queryCount[connectionId] = bucket
    }

    // MARK: - Write Rate Limiting

    private func checkWriteLimit(connectionId: UUID, now: Date) -> RateLimitResult {
        let bucket = writeCount[connectionId] ?? RateBucket()

        let minuteCount = bucket.countInWindow(now: now, windowSeconds: 60)
        if minuteCount >= writesPerMinute {
            let retryAfter = bucket.oldestInWindow(now: now, windowSeconds: 60)
                .map { 60 - Int(now.timeIntervalSince($0)) } ?? 60
            return .exceeded(retryAfter: max(1, retryAfter))
        }

        return .allowed
    }

    private func recordWrite(connectionId: UUID, now: Date) {
        var bucket = writeCount[connectionId] ?? RateBucket()
        bucket.record(now: now)
        bucket.cleanup(now: now, maxAge: 60)
        writeCount[connectionId] = bucket
    }

    // MARK: - DDL Rate Limiting

    private func checkDDLLimit(connectionId: UUID, now: Date) -> RateLimitResult {
        let bucket = ddlCount[connectionId] ?? RateBucket()

        let minuteCount = bucket.countInWindow(now: now, windowSeconds: 60)
        if minuteCount >= ddlPerMinute {
            let retryAfter = bucket.oldestInWindow(now: now, windowSeconds: 60)
                .map { 60 - Int(now.timeIntervalSince($0)) } ?? 60
            return .exceeded(retryAfter: max(1, retryAfter))
        }

        return .allowed
    }

    private func recordDDL(connectionId: UUID, now: Date) {
        var bucket = ddlCount[connectionId] ?? RateBucket()
        bucket.record(now: now)
        bucket.cleanup(now: now, maxAge: 60)
        ddlCount[connectionId] = bucket
    }

    func resetLimits(for connectionId: UUID) {
        queryCount[connectionId] = nil
        writeCount[connectionId] = nil
        ddlCount[connectionId] = nil
    }

    func resetAllLimits() {
        queryCount.removeAll()
        writeCount.removeAll()
        ddlCount.removeAll()
    }
}

struct RateBucket {
    private var timestamps: [Date] = []

    mutating func record(now: Date) {
        timestamps.append(now)
    }

    func countInWindow(now: Date, windowSeconds: TimeInterval) -> Int {
        let cutoff = now.addingTimeInterval(-windowSeconds)
        return timestamps.filter { $0 > cutoff }.count
    }

    func oldestInWindow(now: Date, windowSeconds: TimeInterval) -> Date? {
        let cutoff = now.addingTimeInterval(-windowSeconds)
        return timestamps.filter { $0 > cutoff }.min()
    }

    mutating func cleanup(now: Date, maxAge: TimeInterval) {
        let cutoff = now.addingTimeInterval(-maxAge)
        timestamps = timestamps.filter { $0 > cutoff }
    }
}

enum RateLimitResult {
    case allowed
    case exceeded(retryAfter: Int)

    var isAllowed: Bool {
        if case .allowed = self { return true }
        return false
    }

    var retryAfterSeconds: Int? {
        if case .exceeded(let seconds) = self { return seconds }
        return nil
    }
}

private extension Int {
    var nonZero: Int? {
        self > 0 ? self : nil
    }
}
