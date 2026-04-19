// StdioTransport.swift
// Gridex
//
// MCP stdio transport for communication with AI clients.

import Foundation

actor StdioTransport {
    private var isRunning = false
    private var inputTask: Task<Void, Never>?
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder

    private weak var _delegate: StdioTransportDelegate?

    init() {
        self.encoder = JSONEncoder()
        self.encoder.outputFormatting = .sortedKeys

        self.decoder = JSONDecoder()
    }

    func setDelegate(_ delegate: StdioTransportDelegate?) {
        _delegate = delegate
    }

    func start() async {
        guard !isRunning else { return }
        isRunning = true

        inputTask = Task { [weak self] in
            await self?.readLoop()
        }
    }

    func stop() async {
        isRunning = false
        inputTask?.cancel()
        inputTask = nil
    }

    private func readLoop() async {
        while isRunning && !Task.isCancelled {
            guard let line = readLine() else {
                // EOF
                break
            }

            guard !line.isEmpty else { continue }

            do {
                guard let data = line.data(using: .utf8) else { continue }
                let request = try decoder.decode(JSONRPCRequest.self, from: data)
                await _delegate?.transport(self, didReceiveRequest: request)
            } catch {
                let errorResponse = JSONRPCResponse(id: nil, error: .parseError)
                await send(errorResponse)
            }
        }
    }

    func send(_ response: JSONRPCResponse) async {
        do {
            let data = try encoder.encode(response)
            guard var line = String(data: data, encoding: .utf8) else { return }
            line.append("\n")

            if let outputData = line.data(using: .utf8) {
                FileHandle.standardOutput.write(outputData)
            }
        } catch {
            print("[MCP Transport] Failed to send response: \(error)", to: &standardError)
        }
    }

    func sendNotification(method: String, params: JSONValue?) async {
        let notification = JSONRPCRequest(id: nil, method: method, params: params)
        do {
            let data = try encoder.encode(notification)
            guard var line = String(data: data, encoding: .utf8) else { return }
            line.append("\n")

            if let outputData = line.data(using: .utf8) {
                FileHandle.standardOutput.write(outputData)
            }
        } catch {
            print("[MCP Transport] Failed to send notification: \(error)", to: &standardError)
        }
    }
}

protocol StdioTransportDelegate: AnyObject, Sendable {
    func transport(_ transport: StdioTransport, didReceiveRequest request: JSONRPCRequest) async
}

// Helper for writing to stderr
private var standardError = StandardErrorOutputStream()

private struct StandardErrorOutputStream: TextOutputStream {
    mutating func write(_ string: String) {
        FileHandle.standardError.write(Data(string.utf8))
    }
}
