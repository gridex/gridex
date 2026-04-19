// MCPApprovalGate.swift
// Gridex
//
// Approval gate for MCP write operations.
// Shows a dialog and waits for user confirmation.

import Foundation
import AppKit
import SwiftUI

actor MCPApprovalGate {
    private var pendingApprovals: [UUID: CheckedContinuation<Bool, Never>] = [:]
    private var sessionApprovals: [SessionApprovalKey: Date] = [:]
    private let sessionTimeout: TimeInterval = 30 * 60 // 30 minutes

    struct SessionApprovalKey: Hashable {
        let connectionId: UUID
        let tool: String
    }

    func requestApproval(
        tool: String,
        description: String,
        details: String,
        connectionId: UUID,
        client: MCPAuditClient,
        timeout: TimeInterval = 60
    ) async -> Bool {
        // Check for session approval
        let key = SessionApprovalKey(connectionId: connectionId, tool: tool)
        if let approvalTime = sessionApprovals[key],
           Date().timeIntervalSince(approvalTime) < sessionTimeout {
            return true
        }

        let requestId = UUID()

        return await withCheckedContinuation { continuation in
            pendingApprovals[requestId] = continuation

            // Show dialog on main thread
            Task { @MainActor in
                let result = await showApprovalDialog(
                    requestId: requestId,
                    tool: tool,
                    description: description,
                    details: details,
                    connectionId: connectionId,
                    client: client
                )

                Task {
                    await self.handleApprovalResult(
                        requestId: requestId,
                        result: result,
                        key: key
                    )
                }
            }

            // Set timeout
            Task {
                try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
                await self.timeoutApproval(requestId: requestId)
            }
        }
    }

    private func handleApprovalResult(requestId: UUID, result: ApprovalResult, key: SessionApprovalKey) {
        guard let continuation = pendingApprovals.removeValue(forKey: requestId) else { return }

        switch result {
        case .approved:
            continuation.resume(returning: true)
        case .approvedForSession:
            sessionApprovals[key] = Date()
            continuation.resume(returning: true)
        case .denied:
            continuation.resume(returning: false)
        }
    }

    private func timeoutApproval(requestId: UUID) {
        guard let continuation = pendingApprovals.removeValue(forKey: requestId) else { return }
        continuation.resume(returning: false)
    }

    func revokeSessionApproval(for connectionId: UUID) {
        sessionApprovals = sessionApprovals.filter { $0.key.connectionId != connectionId }
    }

    func revokeAllSessionApprovals() {
        sessionApprovals.removeAll()
    }

    @MainActor
    private func showApprovalDialog(
        requestId: UUID,
        tool: String,
        description: String,
        details: String,
        connectionId: UUID,
        client: MCPAuditClient
    ) async -> ApprovalResult {
        await withCheckedContinuation { continuation in
            let alert = NSAlert()
            alert.messageText = "\(client.name) wants to:"
            alert.informativeText = """
            \(description)

            \(details)

            Connection: \(connectionId.uuidString.prefix(8))...
            """
            alert.alertStyle = .warning

            alert.addButton(withTitle: "Deny")
            alert.addButton(withTitle: "Approve Once")
            alert.addButton(withTitle: "Approve for Session")

            // Bring app to front
            NSApp.activate(ignoringOtherApps: true)

            let response = alert.runModal()

            switch response {
            case .alertFirstButtonReturn:
                continuation.resume(returning: .denied)
            case .alertSecondButtonReturn:
                continuation.resume(returning: .approved)
            case .alertThirdButtonReturn:
                continuation.resume(returning: .approvedForSession)
            default:
                continuation.resume(returning: .denied)
            }
        }
    }
}

enum ApprovalResult {
    case approved
    case approvedForSession
    case denied
}
