// ChatGPTOAuthLoopbackServer.swift
// Gridex
//
// Single-shot loopback HTTP listener used by the ChatGPT OAuth flow.
// The browser receives a `redirect_uri=http://localhost:<port>/auth/callback`
// query param; once it follows the redirect with `code` + `state` we parse
// them out, send a tiny "you can close this window" page back, and tear down.
//
// Trust verified by reverse-engineering Codex CLI: the OAuth client_id accepts
// any loopback port for this redirect path. Binding to 127.0.0.1 (not 0.0.0.0)
// prevents LAN-side hijack.

import Foundation
import Network
import os

/// Result extracted from the browser's GET to `/auth/callback`.
struct ChatGPTOAuthCallback: Sendable {
    let code: String
    let state: String
}

enum ChatGPTOAuthLoopbackError: Error, LocalizedError, CustomStringConvertible {
    case noPortAvailable(tried: [UInt16])
    case timedOut(after: TimeInterval)
    case malformedRequest(reason: String)
    case authError(code: String, description: String?)
    /// `NWListener` reported `.cancelled` before reaching `.ready`. Distinct
    /// from `malformedRequest` because no browser request is involved.
    case listenerCancelled

    var description: String {
        switch self {
        case .noPortAvailable(let tried):
            return "Could not bind any of the loopback ports: \(tried)"
        case .timedOut(let s):
            return "OAuth callback did not arrive within \(Int(s))s"
        case .malformedRequest(let why):
            return "Browser callback was malformed: \(why)"
        case .authError(let code, let desc):
            return "Authorization server returned error '\(code)': \(desc ?? "(no description)")"
        case .listenerCancelled:
            return "OAuth loopback listener was cancelled before becoming ready"
        }
    }

    /// Foundation reads this for `error.localizedDescription`. Without this
    /// override callers see the default `Error._domain`-prefixed string and
    /// lose the `description` payload we built above.
    var errorDescription: String? { description }
}

/// One-shot loopback HTTP server. Create it, call `start()`, then `awaitCallback()`,
/// then it self-tears-down. Re-using an instance after success is not supported.
///
/// The class is `@unchecked Sendable` because `NWListener` callbacks fire on
/// internal queues — we serialize state mutation through a lock.
final class ChatGPTOAuthLoopbackServer: @unchecked Sendable {

    private static let log = Logger(subsystem: "com.gridex.gridex", category: "ChatGPTOAuth")

    private let lock = NSLock()
    private var listener: NWListener?
    private var connections: [NWConnection] = []
    private var continuation: CheckedContinuation<ChatGPTOAuthCallback, Error>?
    private var resolvedResult: Result<ChatGPTOAuthCallback, Error>?
    private var resolved = false
    /// The 180-second timeout sleeper. Tracked so a successful callback can
    /// cancel it instead of leaving a stranded Task that wakes up minutes
    /// later only to find `resolved == true` and exit.
    private var timeoutTask: Task<Void, Never>?

    private(set) var actualPort: UInt16 = 0

    /// Best-effort response body sent to the browser after a successful callback.
    /// Plain HTML (Safari frequently ignores `window.close()` from non-script-opened
    /// tabs, but the JS attempt is harmless on Chrome/Firefox).
    private static let successPage =
"""
<!doctype html><html><head><meta charset="utf-8"><title>Gridex sign-in</title>
<style>body{font:14px -apple-system;color:#333;text-align:center;padding:80px}</style>
</head><body><h2>Sign-in complete</h2>
<p>You can close this tab and return to Gridex.</p>
<script>setTimeout(()=>window.close(),500)</script>
</body></html>
"""

    private static let errorPage =
"""
<!doctype html><html><head><meta charset="utf-8"><title>Gridex sign-in</title>
<style>body{font:14px -apple-system;color:#333;text-align:center;padding:80px}</style>
</head><body><h2>Sign-in failed</h2>
<p>Return to Gridex for details. You can close this tab.</p>
</body></html>
"""

    /// Starts the listener on the first available port from `preferredPorts`,
    /// then falls back to a kernel-assigned ephemeral port. Returns the port
    /// that was bound.
    func start(preferredPorts: [UInt16] = ChatGPTOAuthConstants.preferredPorts) async throws -> UInt16 {
        // Build a candidate list: preferred first, then `0` (ephemeral) as last resort.
        let candidates = preferredPorts + [0]
        var triedPorts: [UInt16] = []

        for candidate in candidates {
            triedPorts.append(candidate)
            do {
                let port: UInt16 = try await tryBind(port: candidate)
                actualPort = port
                Self.log.info("OAuth loopback listening on 127.0.0.1:\(port)")
                return port
            } catch {
                Self.log.notice("OAuth loopback port \(candidate) unavailable: \(error.localizedDescription, privacy: .public)")
                continue
            }
        }
        throw ChatGPTOAuthLoopbackError.noPortAvailable(tried: triedPorts)
    }

    /// Awaits a single callback or fails with `.timedOut`. Calling this without
    /// a preceding `start()` returns immediately with a programming error.
    func awaitCallback(timeout: TimeInterval) async throws -> ChatGPTOAuthCallback {
        precondition(actualPort != 0 || listener != nil, "must call start() before awaitCallback()")

        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (cont: CheckedContinuation<ChatGPTOAuthCallback, Error>) in
                lock.lock()
                if resolved {
                    let result = self.resolvedResult
                    lock.unlock()
                    switch result {
                    case .success(let callback):
                        cont.resume(returning: callback)
                    case .failure(let error):
                        cont.resume(throwing: error)
                    case .none:
                        cont.resume(throwing: ChatGPTOAuthLoopbackError.malformedRequest(reason: "already resolved"))
                    }
                    return
                }
                self.continuation = cont
                let task = Task { [weak self] in
                    try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
                    guard let self else { return }
                    if Task.isCancelled { return }
                    self.resolveFailure(ChatGPTOAuthLoopbackError.timedOut(after: timeout))
                }
                self.timeoutTask = task
                lock.unlock()
            }
        } onCancel: {
            self.resolveFailure(CancellationError())
        }
    }

    /// Tear down all sockets. Idempotent.
    func stop() {
        lock.lock()
        defer { lock.unlock() }
        listener?.cancel()
        listener = nil
        for c in connections { c.cancel() }
        connections.removeAll()
    }

    // MARK: - Internal

    private func tryBind(port: UInt16) async throws -> UInt16 {
        let nwPort: NWEndpoint.Port = (port == 0) ? .any : NWEndpoint.Port(rawValue: port)!
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv4(.loopback), port: nwPort)

        let listener = try NWListener(using: params)
        self.listener = listener

        listener.newConnectionHandler = { [weak self] conn in
            self?.handleConnection(conn)
        }

        // Bridge `state` → async via continuation. Resume only on .ready/.failed/.cancelled.
        let actualPort: UInt16 = try await withCheckedThrowingContinuation { (cont: CheckedContinuation<UInt16, Error>) in
            var resumed = false
            listener.stateUpdateHandler = { state in
                guard !resumed else { return }
                switch state {
                case .ready:
                    resumed = true
                    let p = listener.port?.rawValue ?? port
                    cont.resume(returning: p)
                case .failed(let err):
                    resumed = true
                    cont.resume(throwing: err)
                case .cancelled:
                    resumed = true
                    cont.resume(throwing: ChatGPTOAuthLoopbackError.listenerCancelled)
                default:
                    break
                }
            }
            listener.start(queue: .global(qos: .userInitiated))
        }
        return actualPort
    }

    private func handleConnection(_ conn: NWConnection) {
        lock.lock()
        connections.append(conn)
        lock.unlock()

        conn.start(queue: .global(qos: .userInitiated))
        receiveRequest(conn, accumulated: Data())
    }

    /// Accumulates bytes until `\r\n\r\n` end-of-headers, then parses the GET line.
    /// We don't need to read the body — the OAuth callback is GET-only.
    private func receiveRequest(_ conn: NWConnection, accumulated: Data) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 8192) { [weak self] chunk, _, isComplete, error in
            guard let self else { return }

            var buffer = accumulated
            if let chunk { buffer.append(chunk) }

            if let error = error {
                Self.log.error("OAuth loopback receive error: \(error.localizedDescription, privacy: .public)")
                conn.cancel()
                self.resolveFailure(error)
                return
            }

            // Look for end-of-headers (CRLF CRLF or LF LF as a fallback).
            if let range = buffer.range(of: Data("\r\n\r\n".utf8)) ?? buffer.range(of: Data("\n\n".utf8)) {
                let head = buffer[..<range.lowerBound]
                self.processHead(head, on: conn)
                return
            }

            if isComplete {
                conn.cancel()
                self.resolveFailure(ChatGPTOAuthLoopbackError.malformedRequest(reason: "connection closed before headers ended"))
                return
            }

            if buffer.count > 64 * 1024 {
                // Defensive cap — a real OAuth callback URL is well under 4 KB.
                conn.cancel()
                self.resolveFailure(ChatGPTOAuthLoopbackError.malformedRequest(reason: "headers exceed 64KB"))
                return
            }

            self.receiveRequest(conn, accumulated: buffer)
        }
    }

    private func processHead(_ head: Data, on conn: NWConnection) {
        guard let text = String(data: head, encoding: .utf8) else {
            sendErrorPage(conn)
            resolveFailure(ChatGPTOAuthLoopbackError.malformedRequest(reason: "request bytes are not UTF-8"))
            return
        }

        let firstLine = text.split(whereSeparator: { $0 == "\r" || $0 == "\n" }).first ?? ""
        let parts = firstLine.split(separator: " ", maxSplits: 2)
        guard parts.count >= 2 else {
            sendErrorPage(conn)
            resolveFailure(ChatGPTOAuthLoopbackError.malformedRequest(reason: "request line malformed: \(firstLine)"))
            return
        }
        // parts[0] = "GET", parts[1] = "/auth/callback?code=...&state=..."
        let pathAndQuery = String(parts[1])

        // Use URLComponents to parse query items robustly.
        // We prepend a fake host because URLComponents doesn't like bare paths.
        guard let comps = URLComponents(string: "http://localhost\(pathAndQuery)") else {
            sendErrorPage(conn)
            resolveFailure(ChatGPTOAuthLoopbackError.malformedRequest(reason: "could not parse path: \(pathAndQuery)"))
            return
        }

        if comps.path != ChatGPTOAuthConstants.callbackPath {
            // Probably a /favicon.ico or stray probe — keep listening for the real one.
            sendErrorPage(conn)
            return
        }

        let items = comps.queryItems ?? []
        let code        = items.first(where: { $0.name == "code" })?.value
        let state       = items.first(where: { $0.name == "state" })?.value
        let errCode     = items.first(where: { $0.name == "error" })?.value
        let errDesc     = items.first(where: { $0.name == "error_description" })?.value

        if let errCode = errCode {
            sendErrorPage(conn)
            resolveFailure(ChatGPTOAuthLoopbackError.authError(code: errCode, description: errDesc))
            return
        }

        guard let code = code, let state = state else {
            sendErrorPage(conn)
            resolveFailure(ChatGPTOAuthLoopbackError.malformedRequest(reason: "missing code or state in callback"))
            return
        }

        sendSuccessPage(conn)
        resolveSuccess(ChatGPTOAuthCallback(code: code, state: state))
    }

    private func sendSuccessPage(_ conn: NWConnection) {
        sendHTML(conn, status: 200, body: Self.successPage)
    }

    private func sendErrorPage(_ conn: NWConnection) {
        sendHTML(conn, status: 400, body: Self.errorPage)
    }

    private func sendHTML(_ conn: NWConnection, status: Int, body: String) {
        let bytes = body.data(using: .utf8) ?? Data()
        let head = """
HTTP/1.1 \(status) OK\r
Content-Type: text/html; charset=utf-8\r
Content-Length: \(bytes.count)\r
Connection: close\r
\r

"""
        var packet = Data(head.utf8)
        packet.append(bytes)
        conn.send(content: packet, completion: .contentProcessed { _ in
            conn.cancel()
        })
    }

    private func resolveSuccess(_ callback: ChatGPTOAuthCallback) {
        lock.lock()
        guard !resolved else { lock.unlock(); return }
        resolved = true
        self.resolvedResult = .success(callback)
        let cont = self.continuation
        let timer = self.timeoutTask
        self.continuation = nil
        self.timeoutTask = nil
        lock.unlock()

        timer?.cancel()
        cont?.resume(returning: callback)
        // Tear down all sockets — listener fires last.
        stop()
    }

    private func resolveFailure(_ error: Error) {
        lock.lock()
        guard !resolved else { lock.unlock(); return }
        resolved = true
        self.resolvedResult = .failure(error)
        let cont = self.continuation
        let timer = self.timeoutTask
        self.continuation = nil
        self.timeoutTask = nil
        lock.unlock()

        timer?.cancel()
        cont?.resume(throwing: error)
        stop()
    }
}
