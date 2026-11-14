// SSHTunnelService.swift
// Gridex
//
// SSH tunnel management for secure database connections.
// Spawns the system OpenSSH client (`/usr/bin/ssh -N -L`) for local port forwarding:
// 127.0.0.1:localPort → remoteHost:remotePort via the SSH host.
//
// Using the system client (instead of an in-process SSH stack) means every key type
// (RSA, Ed25519, ECDSA, encrypted keys), ssh-agent (incl. 1Password / Secretive),
// ~/.ssh/config host aliases, ProxyJump and known_hosts work exactly as in Terminal.

import Foundation
import os

actor SSHTunnelService {
    private var activeTunnels: [UUID: Process] = [:]

    /// PIDs of every running ssh child, across all service instances, so the app can
    /// kill them synchronously on quit (actor calls can't complete in applicationWillTerminate).
    private static let livePIDs = OSAllocatedUnfairLock(initialState: Set<Int32>())

    /// How long to wait for auth + forward setup. Generous because agents like
    /// 1Password may be waiting on a Touch ID prompt.
    private static let establishTimeout: TimeInterval = 30

    func establish(
        connectionId: UUID,
        config: SSHTunnelConfig,
        remoteHost: String,
        remotePort: Int,
        password: String?
    ) async throws -> UInt16 {
        await disconnect(connectionId: connectionId)

        let localPort = try Self.findFreePort()
        let forwardHost = remoteHost.contains(":") ? "[\(remoteHost)]" : remoteHost  // IPv6 literal

        var args = [
            "-N", "-T",
            "-o", "ExitOnForwardFailure=yes",
            "-o", "ConnectTimeout=10",
            "-o", "ServerAliveInterval=30",
            "-o", "ServerAliveCountMax=3",
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", "NumberOfPasswordPrompts=1",
            "-o", "LogLevel=ERROR",
            "-L", "127.0.0.1:\(localPort):\(forwardHost):\(remotePort)",
            "-p", String(config.port),
        ]

        let secret = password ?? ""
        switch config.authMethod {
        case .password:
            // Without this, ssh offers every agent/default key first and can hit the
            // server's MaxAuthTries before it ever gets to the password.
            if !secret.isEmpty {
                args += ["-o", "PubkeyAuthentication=no",
                         "-o", "PreferredAuthentications=keyboard-interactive,password"]
            }
        case .privateKey, .keyWithPassphrase:
            // Key auth only: the secret here is a key passphrase and must never be
            // offered to the server as a password if it falls back to password auth.
            args += ["-o", "PreferredAuthentications=publickey",
                     "-o", "PasswordAuthentication=no",
                     "-o", "KbdInteractiveAuthentication=no"]
            // Empty key path → fall back to ssh's own defaults (agent, ~/.ssh/config).
            if let keyPath = config.keyPath, !keyPath.isEmpty {
                args += ["-i", (keyPath as NSString).expandingTildeInPath, "-o", "IdentitiesOnly=yes"]
            }
        }

        var env = ProcessInfo.processInfo.environment
        if secret.isEmpty {
            args += ["-o", "BatchMode=yes"]  // never block on a prompt nobody can answer
        } else {
            // Answer the password / key-passphrase prompt via SSH_ASKPASS. The secret is
            // passed in the child's environment, never written to disk.
            env["SSH_ASKPASS"] = try Self.askpassScript().path
            env["SSH_ASKPASS_REQUIRE"] = "force"
            env["GRIDEX_SSH_SECRET"] = secret
        }

        if !config.username.isEmpty {
            args += ["-l", config.username]
        }
        args += ["--", config.host]

        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
        process.arguments = args
        process.environment = env
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = FileHandle.nullDevice
        let stderr = Pipe()
        process.standardError = stderr
        process.terminationHandler = { p in
            _ = Self.livePIDs.withLock { $0.remove(p.processIdentifier) }
        }

        do {
            try process.run()
        } catch {
            throw GridexError.sshConnectionFailed(underlying: SSHError(message: error.localizedDescription))
        }
        let pid = process.processIdentifier
        Self.livePIDs.withLock { _ = $0.insert(pid) }

        // ssh only opens the local listener after authentication succeeds, so ssh
        // listening on the port means the tunnel is ready.
        do {
            let deadline = Date().addingTimeInterval(Self.establishTimeout)
            while !Self.isListening(pid: pid, port: localPort) {
                if !process.isRunning {
                    process.waitUntilExit()
                    let output = String(decoding: stderr.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    throw GridexError.sshConnectionFailed(underlying: SSHError(
                        message: output.isEmpty ? "ssh exited with status \(process.terminationStatus)" : output
                    ))
                }
                if Date() > deadline {
                    throw GridexError.sshConnectionFailed(underlying: SSHError(
                        message: "Timed out after \(Int(Self.establishTimeout))s waiting for the tunnel"
                    ))
                }
                try await Task.sleep(for: .milliseconds(100))
            }
        } catch {
            Self.terminate(process)
            throw error
        }

        activeTunnels[connectionId] = process
        return localPort
    }

    func disconnect(connectionId: UUID) async {
        if let process = activeTunnels.removeValue(forKey: connectionId) {
            Self.terminate(process)
        }
    }

    func disconnectAll() async {
        for process in activeTunnels.values {
            Self.terminate(process)
        }
        activeTunnels.removeAll()
    }

    /// Synchronously kills every ssh child process. Call from applicationWillTerminate.
    nonisolated static func terminateAllProcesses() {
        livePIDs.withLock { pids in
            for pid in pids { kill(pid, SIGTERM) }
            pids.removeAll()
        }
    }

    // MARK: - Private

    private struct SSHError: LocalizedError, Sendable {
        let message: String
        var errorDescription: String? { message }
    }

    private static func terminate(_ process: Process) {
        let pid = process.processIdentifier
        if process.isRunning { process.terminate() }
        _ = livePIDs.withLock { $0.remove(pid) }
    }

    /// Tiny SSH_ASKPASS helper that echoes the secret from its environment.
    private static func askpassScript() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("gridex-ssh-askpass.sh")
        try "#!/bin/sh\nprintf '%s\\n' \"$GRIDEX_SSH_SECRET\"\n".write(to: url, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: url.path)
        return url
    }

    private static func loopbackAddress(port: UInt16) -> sockaddr_in {
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        return addr
    }

    /// True once `pid` itself owns a TCP socket listening on `port`. Checking ownership,
    /// rather than just that the port accepts connections, means a local process that
    /// grabbed the port between findFreePort() and ssh's bind can never be handed the
    /// database credentials (ssh then fails the forward and exits instead).
    private static func isListening(pid: Int32, port: UInt16) -> Bool {
        let bufferSize = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nil, 0)
        guard bufferSize > 0 else { return false }
        let stride = MemoryLayout<proc_fdinfo>.stride
        var fds = [proc_fdinfo](repeating: proc_fdinfo(), count: Int(bufferSize) / stride)
        let filled = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, &fds, bufferSize)
        guard filled > 0 else { return false }

        for fd in fds.prefix(Int(filled) / stride) where fd.proc_fdtype == UInt32(PROX_FDTYPE_SOCKET) {
            var info = socket_fdinfo()
            let size = Int32(MemoryLayout<socket_fdinfo>.size)
            guard proc_pidfdinfo(pid, fd.proc_fd, PROC_PIDFDSOCKETINFO, &info, size) == size,
                  info.psi.soi_kind == Int32(SOCKINFO_TCP) else { continue }
            let tcp = info.psi.soi_proto.pri_tcp
            let localPort = UInt16(bigEndian: UInt16(truncatingIfNeeded: tcp.tcpsi_ini.insi_lport))
            if tcp.tcpsi_state == TSI_S_LISTEN && localPort == port {
                return true
            }
        }
        return false
    }

    private static func findFreePort() throws -> UInt16 {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw GridexError.sshTunnelFailed }
        defer { close(fd) }
        var addr = loopbackAddress(port: 0)
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0 else { throw GridexError.sshTunnelFailed }
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        let named = withUnsafeMutablePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) }
        }
        guard named == 0 else { throw GridexError.sshTunnelFailed }
        return UInt16(bigEndian: addr.sin_port)
    }
}
