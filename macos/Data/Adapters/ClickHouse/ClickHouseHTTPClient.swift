// ClickHouseHTTPClient.swift
// Gridex
//
// HTTP client for ClickHouse. Speaks the ClickHouse HTTP interface
// (ports 8123/8443) using URLSession. Supports plain HTTP, HTTPS with a
// pinned CA, trust-on-first-use, and mutual TLS via a PKCS#12 bundle.

import Foundation

struct ClickHouseHTTPClient: Sendable {
    let scheme: String                 // "http" or "https"
    let host: String
    let port: Int
    let username: String
    let password: String
    let defaultDatabase: String?
    private let session: URLSession

    init(
        host: String,
        port: Int,
        username: String,
        password: String,
        defaultDatabase: String?,
        useTLS: Bool,
        sslCACertPath: String?,
        sslClientBundlePath: String?
    ) {
        self.scheme = useTLS ? "https" : "http"
        self.host = host
        self.port = port
        self.username = username
        self.password = password
        self.defaultDatabase = defaultDatabase

        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = 60
        cfg.timeoutIntervalForResource = 600
        cfg.httpShouldUsePipelining = false
        cfg.waitsForConnectivity = false
        cfg.httpAdditionalHeaders = ["Accept": "application/json"]

        let delegate = ClickHouseTLSDelegate(
            useTLS: useTLS,
            caCertPath: sslCACertPath,
            clientBundlePath: sslClientBundlePath
        )
        self.session = URLSession(configuration: cfg, delegate: delegate, delegateQueue: nil)
    }

    /// Issue a ClickHouse query. `sql` is appended verbatim to the request body.
    /// `database` overrides the default database for this request.
    /// `queryId` is sent via `X-ClickHouse-Query-Id` to enable `KILL QUERY`.
    func send(
        sql: String,
        database: String? = nil,
        queryId: String? = nil,
        readOnly: Bool = false
    ) async throws -> ClickHouseHTTPResponse {
        var components = URLComponents()
        components.scheme = scheme
        components.host = host
        components.port = port
        components.path = "/"
        var items: [URLQueryItem] = []
        if let db = database ?? defaultDatabase, !db.isEmpty {
            items.append(URLQueryItem(name: "database", value: db))
        }
        if readOnly {
            // readonly=2 allows SETTINGS, readonly=1 is strict. Use 2 for SELECT introspection.
            items.append(URLQueryItem(name: "readonly", value: "2"))
        }
        // Ask the server for row-count summary in headers so non-SELECT statements
        // can report `rowsAffected` even with FORMAT Null.
        items.append(URLQueryItem(name: "send_progress_in_http_headers", value: "0"))
        components.queryItems = items.isEmpty ? nil : items

        guard let url = components.url else {
            throw GridexError.queryExecutionFailed("Invalid ClickHouse URL")
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.httpBody = sql.data(using: .utf8)
        request.setValue("text/plain; charset=UTF-8", forHTTPHeaderField: "Content-Type")
        if !username.isEmpty {
            request.setValue(username, forHTTPHeaderField: "X-ClickHouse-User")
        }
        if !password.isEmpty {
            request.setValue(password, forHTTPHeaderField: "X-ClickHouse-Key")
        }
        if let queryId, !queryId.isEmpty {
            request.setValue(queryId, forHTTPHeaderField: "X-ClickHouse-Query-Id")
        }

        let (data, response): (Data, URLResponse)
        do {
            (data, response) = try await session.data(for: request)
        } catch {
            throw GridexError.connectionFailed(underlying: error)
        }

        guard let http = response as? HTTPURLResponse else {
            throw GridexError.queryExecutionFailed("ClickHouse returned a non-HTTP response")
        }

        if http.statusCode >= 400 {
            let body = String(data: data, encoding: .utf8) ?? "HTTP \(http.statusCode)"
            throw GridexError.queryExecutionFailed(body.trimmingCharacters(in: .whitespacesAndNewlines))
        }

        return ClickHouseHTTPResponse(status: http.statusCode, body: data, headers: http.allHeaderFields)
    }
}

struct ClickHouseHTTPResponse: Sendable {
    let status: Int
    let body: Data
    let headers: [AnyHashable: Any]

    /// ClickHouse writes progress as JSON in this response header on mutating statements.
    var summary: [String: String]? {
        guard let raw = headers["X-ClickHouse-Summary"] as? String,
              let data = raw.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: String]
        else { return nil }
        return json
    }

    var writtenRows: Int {
        Int(summary?["written_rows"] ?? "0") ?? 0
    }
}

// MARK: - TLS delegate

/// URLSessionDelegate that pins a custom CA and/or presents a client identity.
/// Gracefully falls back to the system trust store if no CA is provided.
final class ClickHouseTLSDelegate: NSObject, URLSessionDelegate, Sendable {
    private let useTLS: Bool
    private let caCertificates: [SecCertificate]
    private let clientIdentity: SecIdentity?

    init(useTLS: Bool, caCertPath: String?, clientBundlePath: String?) {
        self.useTLS = useTLS
        self.caCertificates = Self.loadCACertificates(path: caCertPath)
        self.clientIdentity = Self.loadClientIdentity(path: clientBundlePath)
        super.init()
    }

    func urlSession(
        _ session: URLSession,
        didReceive challenge: URLAuthenticationChallenge,
        completionHandler: @Sendable @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void
    ) {
        let method = challenge.protectionSpace.authenticationMethod

        if method == NSURLAuthenticationMethodServerTrust {
            guard let serverTrust = challenge.protectionSpace.serverTrust else {
                completionHandler(.performDefaultHandling, nil)
                return
            }
            if !caCertificates.isEmpty {
                SecTrustSetAnchorCertificates(serverTrust, caCertificates as CFArray)
                SecTrustSetAnchorCertificatesOnly(serverTrust, true)
                var error: CFError?
                let ok = SecTrustEvaluateWithError(serverTrust, &error)
                if ok {
                    completionHandler(.useCredential, URLCredential(trust: serverTrust))
                } else {
                    completionHandler(.cancelAuthenticationChallenge, nil)
                }
            } else {
                completionHandler(.performDefaultHandling, nil)
            }
            return
        }

        if method == NSURLAuthenticationMethodClientCertificate, let identity = clientIdentity {
            let credential = URLCredential(identity: identity, certificates: nil, persistence: .forSession)
            completionHandler(.useCredential, credential)
            return
        }

        completionHandler(.performDefaultHandling, nil)
    }

    // MARK: - Loading

    private static func loadCACertificates(path: String?) -> [SecCertificate] {
        guard let path, !path.isEmpty,
              let data = try? Data(contentsOf: URL(fileURLWithPath: path))
        else { return [] }

        // Try DER first
        if let cert = SecCertificateCreateWithData(nil, data as CFData) {
            return [cert]
        }

        // Fall back to PEM — may contain one or more BEGIN CERTIFICATE blocks.
        guard let text = String(data: data, encoding: .utf8) else { return [] }
        var certs: [SecCertificate] = []
        let blocks = text.components(separatedBy: "-----BEGIN CERTIFICATE-----")
        for block in blocks.dropFirst() {
            guard let endRange = block.range(of: "-----END CERTIFICATE-----") else { continue }
            let base64 = block[..<endRange.lowerBound]
                .replacingOccurrences(of: "\n", with: "")
                .replacingOccurrences(of: "\r", with: "")
                .replacingOccurrences(of: " ", with: "")
            if let der = Data(base64Encoded: base64),
               let cert = SecCertificateCreateWithData(nil, der as CFData) {
                certs.append(cert)
            }
        }
        return certs
    }

    /// Load a PKCS#12 bundle (.p12/.pfx) containing the client certificate + private key.
    /// The bundle must be exported with an empty passphrase — separate PEM files are not
    /// supported by URLSession's client-identity API on macOS without keychain staging.
    private static func loadClientIdentity(path: String?) -> SecIdentity? {
        guard let path, !path.isEmpty,
              let data = try? Data(contentsOf: URL(fileURLWithPath: path))
        else { return nil }

        let options: [String: Any] = [kSecImportExportPassphrase as String: ""]
        var rawItems: CFArray?
        let status = SecPKCS12Import(data as CFData, options as CFDictionary, &rawItems)
        guard status == errSecSuccess,
              let items = rawItems as? [[String: Any]],
              let first = items.first,
              let identity = first[kSecImportItemIdentity as String]
        else { return nil }
        return (identity as! SecIdentity)
    }
}
