// LLMRetry.swift
// Gridex
//
// Exponential-backoff retry for provider HTTP calls.
// Mirrors goclaw's internal/providers/retry.go — retries on 429, 5xx, and transient
// network errors, with jittered delay. Streaming requests are NOT retried once
// the first byte has been read (caller's responsibility to short-circuit).

import Foundation

struct LLMRetryPolicy: Sendable {
    var maxAttempts: Int
    var baseDelay: TimeInterval
    var maxDelay: TimeInterval

    static let `default` = LLMRetryPolicy(maxAttempts: 3, baseDelay: 0.5, maxDelay: 8.0)
    static let none = LLMRetryPolicy(maxAttempts: 1, baseDelay: 0, maxDelay: 0)
}

enum LLMRetry {
    /// Perform a non-streaming HTTP request with retry on transient failures.
    /// Returns the (data, HTTPURLResponse) pair on success; throws on exhaustion.
    static func perform(
        _ request: URLRequest,
        session: URLSession = .shared,
        policy: LLMRetryPolicy = .default
    ) async throws -> (Data, HTTPURLResponse) {
        var attempt = 0
        var lastError: Error?

        while attempt < policy.maxAttempts {
            attempt += 1
            do {
                let (data, response) = try await session.data(for: request)
                guard let http = response as? HTTPURLResponse else {
                    throw GridexError.aiProviderError("Non-HTTP response")
                }

                if shouldRetry(status: http.statusCode), attempt < policy.maxAttempts {
                    try await sleep(for: delay(attempt: attempt, policy: policy))
                    continue
                }
                return (data, http)
            } catch {
                lastError = error
                if isTransient(error), attempt < policy.maxAttempts {
                    try await sleep(for: delay(attempt: attempt, policy: policy))
                    continue
                }
                throw error
            }
        }
        throw lastError ?? GridexError.aiProviderError("Retry exhausted")
    }

    // MARK: - Helpers

    private static func shouldRetry(status: Int) -> Bool {
        status == 408 || status == 425 || status == 429 || (500...599).contains(status)
    }

    private static func isTransient(_ error: Error) -> Bool {
        let ns = error as NSError
        guard ns.domain == NSURLErrorDomain else { return false }
        switch ns.code {
        case NSURLErrorTimedOut,
             NSURLErrorCannotConnectToHost,
             NSURLErrorNetworkConnectionLost,
             NSURLErrorNotConnectedToInternet,
             NSURLErrorDNSLookupFailed:
            return true
        default:
            return false
        }
    }

    private static func delay(attempt: Int, policy: LLMRetryPolicy) -> TimeInterval {
        let exp = policy.baseDelay * pow(2.0, Double(attempt - 1))
        let capped = min(exp, policy.maxDelay)
        let jitter = Double.random(in: 0...(capped * 0.25))
        return capped + jitter
    }

    private static func sleep(for seconds: TimeInterval) async throws {
        try await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
    }
}
