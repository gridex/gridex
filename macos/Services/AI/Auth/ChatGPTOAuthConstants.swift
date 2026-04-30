// ChatGPTOAuthConstants.swift
// Gridex
//
// Endpoint + parameter constants for the ChatGPT OAuth flow. All values mirror
// OpenAI's Codex CLI v2.x behaviour (see codex-rs/login/src/server.rs in the
// openai/codex repo). Reusing the public Codex client_id is a deliberate
// trade-off — see plan F.2 disclaimer.

import Foundation

enum ChatGPTOAuthConstants {
    /// The public OAuth client_id baked into OpenAI's Codex CLI. Reused here
    /// because the authorize endpoint only accepts loopback redirect URIs that
    /// were registered for this client. OpenAI may revoke it at any time.
    static let clientId = "app_EMoamEEZ73f0CkXaXp7hrann"

    /// Base of the OAuth issuer. `/oauth/authorize` and `/oauth/token` hang off this.
    static let issuer = URL(string: "https://auth.openai.com")!

    /// Scope set required by the Codex CLI flow. `offline_access` is what gives
    /// us a refresh_token; the `api.connectors.*` scopes are what lets the
    /// access_token reach `chatgpt.com/backend-api/codex`.
    static let scopes = "openid profile email offline_access api.connectors.read api.connectors.invoke"

    /// Backend used by signed-in callers. Distinct from `api.openai.com/v1`.
    /// Derived from ProviderType so Core remains the single source of truth.
    static let backend = URL(string: ProviderType.chatGPT.defaultBaseURL)!

    /// Identifier sent as the `originator` query param. Codex CLI uses
    /// `codex_cli_rs`; we identify ourselves so a future audit can tell.
    /// Deviates from the original plan ("gridex_cli") on purpose — this client
    /// is a macOS GUI, not a CLI, and the value is informational only (the
    /// auth server doesn't gate behaviour on it).
    static let originator = "gridex_macos"

    /// 3 minutes — long enough for password manager + 2FA, short enough to
    /// guarantee `signIn` does not hang the UI indefinitely.
    static let callbackTimeout: TimeInterval = 180

    /// Refresh tokens slightly before the JWT actually expires, so a request
    /// in flight at `exp` doesn't hit a 401.
    static let refreshSkew: TimeInterval = 30

    /// Loopback ports tried in order. The OAuth client_id is registered for
    /// `http://localhost:<any-port>/auth/callback`; Codex CLI defaults to 1455.
    /// We follow the same order so that a stuck listener from a prior `codex`
    /// invocation in the same shell doesn't immediately collide.
    static let preferredPorts: [UInt16] = [1455, 1456, 1457, 1458]

    /// Path component of the redirect URI. Used by both the listener and the
    /// authorize-URL builder to stay in sync.
    static let callbackPath = "/auth/callback"
}
