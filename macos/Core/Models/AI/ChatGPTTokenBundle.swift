// ChatGPTTokenBundle.swift
// Gridex
//
// OAuth token bundle persisted in Keychain for the .chatGPT provider type.
// Stored as JSON under key "ai.chatgpt.tokens.<provider-uuid>" — a single blob
// so refresh writes are atomic.

import Foundation

struct ChatGPTTokenBundle: Codable, Sendable, Equatable {
    /// JWT — short-lived, used as `Authorization: Bearer ...` against
    /// chatgpt.com/backend-api/codex.
    var accessToken: String

    /// JWT — long-lived, used to mint new access tokens. Server may rotate
    /// this on refresh; callers must merge the new value over the old bundle
    /// (and keep the old refresh_token if the response omits it).
    var refreshToken: String

    /// JWT — carries claims (`chatgpt_account_id`, `email`, `chatgpt_plan_type`).
    /// Persisted so we can re-derive identity without re-querying the server.
    var idToken: String

    /// Mirrors the `chatgpt_account_id` claim from `idToken`. Sent as the
    /// `ChatGPT-Account-ID` header on backend calls when present.
    var accountId: String?

    /// Mirrors the `email` claim. UI-only, for "Signed in as ..." display.
    var email: String?

    /// Mirrors the `chatgpt_plan_type` claim (e.g. "plus", "pro"). UI-only.
    var planType: String?

    /// Wall-clock time the bundle was minted (sign-in or last refresh).
    var obtainedAt: Date

    /// Reserved for future migrations if OpenAI changes the id_token claim
    /// shape. v1 = the layout above.
    var schemaVersion: Int

    init(
        accessToken: String,
        refreshToken: String,
        idToken: String,
        accountId: String? = nil,
        email: String? = nil,
        planType: String? = nil,
        obtainedAt: Date = Date(),
        schemaVersion: Int = 1
    ) {
        self.accessToken   = accessToken
        self.refreshToken  = refreshToken
        self.idToken       = idToken
        self.accountId     = accountId
        self.email         = email
        self.planType      = planType
        self.obtainedAt    = obtainedAt
        self.schemaVersion = schemaVersion
    }
}
