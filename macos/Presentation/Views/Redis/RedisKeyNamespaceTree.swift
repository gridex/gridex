// RedisKeyNamespaceTree.swift
// Gridex
//
// Pure namespace tree construction for Redis key names.

import Foundation

struct RedisKeyNamespaceNode: Identifiable, Equatable {
    let id: String
    let segment: String
    let concreteKey: String?
    let children: [RedisKeyNamespaceNode]
    let descendantKeyCount: Int
}

enum RedisKeyNamespaceTree {

    static func build(keys: [String], delimiter: String) -> [RedisKeyNamespaceNode] {
        precondition(!delimiter.isEmpty)

        var roots: [String: TrieNode] = [:]
        for key in Set(keys) {
            let segments = key.components(separatedBy: delimiter)
            let rootSegment = segments[0]
            let root = roots[rootSegment] ?? TrieNode(segment: rootSegment)
            roots[rootSegment] = root

            var current = root
            for segment in segments.dropFirst() {
                let child = current.children[segment] ?? TrieNode(segment: segment)
                current.children[segment] = child
                current = child
            }
            current.concreteKey = key
        }

        return roots.values
            .sorted { $0.segment < $1.segment }
            .map { materialize($0, path: [$0.segment]) }
    }

    static func displayLabel(for segment: String) -> String {
        segment.isEmpty ? "(empty)" : segment
    }

    private static func materialize(_ node: TrieNode, path: [String]) -> RedisKeyNamespaceNode {
        let children = node.children.values
            .sorted { $0.segment < $1.segment }
            .map { materialize($0, path: path + [$0.segment]) }
        let descendantKeyCount = children.reduce(node.concreteKey == nil ? 0 : 1) {
            $0 + $1.descendantKeyCount
        }

        return RedisKeyNamespaceNode(
            id: nodeID(for: path),
            segment: node.segment,
            concreteKey: node.concreteKey,
            children: children,
            descendantKeyCount: descendantKeyCount
        )
    }

    private static func nodeID(for path: [String]) -> String {
        path.map { "\($0.utf8.count):\($0)" }.joined(separator: "|")
    }

    private final class TrieNode {
        let segment: String
        var concreteKey: String?
        var children: [String: TrieNode] = [:]

        init(segment: String) {
            self.segment = segment
        }
    }
}
