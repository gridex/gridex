// ChatGPTOAuthService.swift
// Gridex
//
// Owns the full ChatGPT OAuth lifecycle:
//   - signIn(providerId:)         — interactive PKCE flow, opens the browser
//   - tokenBundle(providerId:)    — non-interactive, refreshes if access token
//                                    is within `refreshSkew` of expiring
//   - signOut(providerId:)        — clears the Keychain blob
//   - currentStatus(providerId:)  — UI helper, no network
//
// Concurrency model: actor. In-flight refreshes are coalesced per providerId so
// that a burst of `tokenBundle()` calls only triggers one network round-trip.

import AppKit
import Foundation
import os

actor ChatGPTOAuthService {

    private static let log = Logger(subsystem: "com.gridex.gridex", category: "ChatGPTOAuth")

    private let keychain: KeychainServiceProtocol
    private let urlSession: URLSession
    private var refreshInFlight: [UUID: Task<ChatGPTTokenBundle, Error>] = [:]

    init(keychainService: KeychainServiceProtocol, urlSession: URLSession = .shared) {
        self.keychain = keychainService
        self.urlSession = urlSession
    }

    enum SignInStatus: Equatable, Sendable {
        case signedOut
        case signedIn(email: String?, plan: String?)
    }

    // MARK: - Public API

    /// Runs the full PKCE flow: opens browser, waits for /auth/callback,
    /// exchanges the code for tokens, persists them, returns the bundle.
    /// Call from any context — interaction with NSWorkspace happens on MainActor
    /// internally.
    @discardableResult
    func signIn(providerId: UUID) async throws -> ChatGPTTokenBundle {
        Self.log.info("ChatGPT OAuth sign-in starting for provider \(providerId.uuidString, privacy: .public)")

        let verifier  = PKCE.makeVerifier()
        let challenge = PKCE.challenge(for: verifier)
        let state     = PKCE.makeState()

        let server = ChatGPTOAuthLoopbackServer()
        let port = try await server.start()
        let redirectURI = "http://localhost:\(port)\(ChatGPTOAuthConstants.callbackPath)"

        // Open the browser. NSWorkspace must be touched on the main thread.
        let authURL = buildAuthorizeURL(challenge: challenge, state: state, redirectURI: redirectURI)
        Self.log.debug("Opening browser to authorize URL")
        let didOpenBrowser = await MainActor.run {
            NSWorkspace.shared.open(authURL)
        }
        guard didOpenBrowser else {
            server.stop()
            Self.log.error("ChatGPT OAuth sign-in failed: LaunchServices could not open browser")
            throw GridexError.aiProviderError("Could not open browser for ChatGPT sign-in")
        }

        // Block on the browser callback (or 3-min timeout).
        let callback: ChatGPTOAuthCallback
        do {
            callback = try await server.awaitCallback(timeout: ChatGPTOAuthConstants.callbackTimeout)
        } catch {
            server.stop()
            Self.log.error("OAuth callback failed: \(error.localizedDescription, privacy: .public)")
            throw GridexError.aiProviderError("Sign-in failed: \(error.localizedDescription)")
        }

        // CSRF check — the auth server echoes our `state` back to the redirect URI.
        guard callback.state == state else {
            Self.log.error("OAuth state mismatch — possible CSRF")
            throw GridexError.aiProviderError("State mismatch — sign-in aborted (possible CSRF)")
        }

        // Exchange the authorization code for tokens.
        let tokens = try await exchangeCodeForTokens(
            code: callback.code,
            verifier: verifier,
            redirectURI: redirectURI
        )

        let bundle = try buildBundle(from: tokens)
        try keychain.saveChatGPTTokens(providerId: providerId, bundle: bundle)
        Self.log.info("ChatGPT OAuth sign-in complete; email=\(bundle.email ?? "?", privacy: .private)")

        // Bring Gridex back to the front — Safari may not auto-close the tab.
        await MainActor.run {
            NSApp.activate(ignoringOtherApps: true)
        }

        return bundle
    }

    /// Returns a usable token bundle, refreshing via `refresh_token` if the
    /// stored `access_token` is within `refreshSkew` of expiring.
    /// Coalesces concurrent calls per `providerId`.
    func tokenBundle(providerId: UUID) async throws -> ChatGPTTokenBundle {
        guard let bundle = try keychain.loadChatGPTTokens(providerId: providerId) else {
            throw GridexError.aiAPIKeyMissing
        }

        let exp = JWTDecoder.expiration(of: bundle.accessToken) ?? .distantPast
        if Date() < exp.addingTimeInterval(-ChatGPTOAuthConstants.refreshSkew) {
            return bundle
        }

        if let inflight = refreshInFlight[providerId] {
            return try await inflight.value
        }

        let task = Task<ChatGPTTokenBundle, Error> { [self] in
            try await self.performRefresh(bundle: bundle, providerId: providerId)
        }
        refreshInFlight[providerId] = task
        defer { refreshInFlight[providerId] = nil }
        return try await task.value
    }

    /// Deletes the persisted bundle. Cheap; does not call the auth server.
    func signOut(providerId: UUID) throws {
        Self.log.info("ChatGPT sign-out for \(providerId.uuidString, privacy: .public)")
        try keychain.deleteChatGPTTokens(providerId: providerId)
    }

    /// Non-network status used by the settings sheet to render "Signed in as ..."
    /// chrome.
    func currentStatus(providerId: UUID) -> SignInStatus {
        guard let bundle = try? keychain.loadChatGPTTokens(providerId: providerId) else {
            return .signedOut
        }
        return .signedIn(email: bundle.email, plan: bundle.planType)
    }

    // MARK: - Internal

    private struct TokenResponse: Decodable {
        let access_token: String
        let refresh_token: String?
        let id_token: String?
    }

    private func buildAuthorizeURL(challenge: String, state: String, redirectURI: String) -> URL {
        var comps = URLComponents(url: ChatGPTOAuthConstants.issuer.appendingPathComponent("oauth/authorize"),
                                  resolvingAgainstBaseURL: false)!
        comps.queryItems = [
            URLQueryItem(name: "response_type", value: "code"),
            URLQueryItem(name: "client_id", value: ChatGPTOAuthConstants.clientId),
            URLQueryItem(name: "redirect_uri", value: redirectURI),
            URLQueryItem(name: "scope", value: ChatGPTOAuthConstants.scopes),
            URLQueryItem(name: "code_challenge", value: challenge),
            URLQueryItem(name: "code_challenge_method", value: "S256"),
            URLQueryItem(name: "state", value: state),
            URLQueryItem(name: "id_token_add_organizations", value: "true"),
            URLQueryItem(name: "codex_cli_simplified_flow", value: "true"),
            URLQueryItem(name: "originator", value: ChatGPTOAuthConstants.originator),
        ]
        return comps.url!
    }

    private func exchangeCodeForTokens(code: String, verifier: String, redirectURI: String) async throws -> TokenResponse {
        let url = ChatGPTOAuthConstants.issuer.appendingPathComponent("oauth/token")
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")

        // form-encode body
        let body = [
            "grant_type":    "authorization_code",
            "code":          code,
            "redirect_uri":  redirectURI,
            "client_id":     ChatGPTOAuthConstants.clientId,
            "code_verifier": verifier,
        ]
            .map { "\($0.key)=\(encode($0.value))" }
            .joined(separator: "&")
        req.httpBody = Data(body.utf8)

        let (data, response) = try await urlSession.data(for: req)
        guard let http = response as? HTTPURLResponse else {
            throw GridexError.aiProviderError("Token exchange: non-HTTP response")
        }
        guard (200..<300).contains(http.statusCode) else {
            let detail = parseErrorBody(data)
            Self.log.error("Token exchange failed: HTTP \(http.statusCode) \(detail, privacy: .public)")
            throw GridexError.aiProviderError("Token exchange failed (HTTP \(http.statusCode)): \(detail)")
        }

        do {
            return try JSONDecoder().decode(TokenResponse.self, from: data)
        } catch {
            Self.log.error("Token exchange: malformed body — \(error.localizedDescription, privacy: .public)")
            throw GridexError.aiProviderError("Token exchange returned an unexpected payload")
        }
    }

    /// JSON-bodied refresh per Codex CLI behaviour. The auth server returns the
    /// new access_token (and may rotate refresh_token + id_token). When fields
    /// are absent we keep the old values.
    private func performRefresh(bundle: ChatGPTTokenBundle, providerId: UUID) async throws -> ChatGPTTokenBundle {
        Self.log.info("ChatGPT token refresh for \(providerId.uuidString, privacy: .public)")
        let url = ChatGPTOAuthConstants.issuer.appendingPathComponent("oauth/token")
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")

        let body: [String: String] = [
            "client_id":     ChatGPTOAuthConstants.clientId,
            "grant_type":    "refresh_token",
            "refresh_token": bundle.refreshToken,
        ]
        req.httpBody = try JSONSerialization.data(withJSONObject: body)

        let (data, response) = try await urlSession.data(for: req)
        guard let http = response as? HTTPURLResponse else {
            throw GridexError.aiProviderError("Refresh: non-HTTP response")
        }

        if (200..<300).contains(http.statusCode) {
            let parsed = try JSONDecoder().decode(TokenResponse.self, from: data)
            // Merge new values over the old bundle. refresh_token / id_token may rotate.
            var updated = bundle
            updated.accessToken = parsed.access_token
            if let rt = parsed.refresh_token, !rt.isEmpty { updated.refreshToken = rt }
            if let idt = parsed.id_token, !idt.isEmpty {
                updated.idToken = idt
                applyClaims(into: &updated)
            }
            updated.obtainedAt = Date()
            try keychain.saveChatGPTTokens(providerId: providerId, bundle: updated)
            Self.log.info("Refresh ok; new exp=\(JWTDecoder.expiration(of: updated.accessToken)?.timeIntervalSince1970 ?? 0)")
            return updated
        }

        // Failure path. If the auth server says the refresh_token is no longer
        // valid, blow the bundle away and surface aiAPIKeyMissing — the UI will
        // prompt the user to sign in again.
        let detail = parseErrorBody(data)
        let lower = detail.lowercased()
        let dead = ["refresh_token_expired", "refresh_token_reused", "refresh_token_invalidated"]
        if dead.contains(where: lower.contains) {
            Self.log.notice("Refresh token rejected (\(detail, privacy: .public)); clearing keychain")
            try? keychain.deleteChatGPTTokens(providerId: providerId)
            throw GridexError.aiAPIKeyMissing
        }
        throw GridexError.aiProviderError("Refresh failed (HTTP \(http.statusCode)): \(detail)")
    }

    /// Decodes the id_token claims into the bundle's UI-friendly fields.
    private func applyClaims(into bundle: inout ChatGPTTokenBundle) {
        guard let claims = try? JWTDecoder.payload(of: bundle.idToken) else { return }
        bundle.email     = claims["email"] as? String ?? bundle.email
        bundle.accountId = claims["chatgpt_account_id"] as? String ?? bundle.accountId
        bundle.planType  = claims["chatgpt_plan_type"] as? String ?? bundle.planType
    }

    /// Build a fresh bundle from a successful token exchange response.
    private func buildBundle(from tokens: TokenResponse) throws -> ChatGPTTokenBundle {
        guard let idToken = tokens.id_token, !idToken.isEmpty else {
            throw GridexError.aiProviderError("Token exchange returned no id_token")
        }
        guard let refresh = tokens.refresh_token, !refresh.isEmpty else {
            throw GridexError.aiProviderError("Token exchange returned no refresh_token (offline_access scope missing?)")
        }
        var b = ChatGPTTokenBundle(
            accessToken: tokens.access_token,
            refreshToken: refresh,
            idToken: idToken
        )
        applyClaims(into: &b)
        return b
    }

    private func encode(_ value: String) -> String {
        // Application/x-www-form-urlencoded — URL-encode + use `+` for spaces.
        var allowed = CharacterSet.urlQueryAllowed
        allowed.remove(charactersIn: "+&=")
        return value.addingPercentEncoding(withAllowedCharacters: allowed) ?? value
    }

    private func parseErrorBody(_ data: Data) -> String {
        if let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            if let err = json["error"] as? String {
                if let desc = json["error_description"] as? String { return "\(err): \(desc)" }
                return err
            }
            if let msg = json["message"] as? String { return msg }
        }
        return String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? "(empty body)"
    }
}
