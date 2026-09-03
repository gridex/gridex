// RedisKeyNamespaceTreeTests.swift
// Gridex

import XCTest
@testable import Gridex

final class RedisKeyNamespaceTreeTests: XCTestCase {

    private let canonicalKeys = [
        "user:1001:profile",
        "user:1001:session",
        "cache:product:42"
    ]

    private let edgeCaseKeys = [
        "plain", "user", "user:1001", "user:",
        ":leading", "a::b", ":", "a:b", "a:b"
    ]

    func test_buildGroupsCanonicalColonNamespacesAndCountsKeys() {
        let roots = RedisKeyNamespaceTree.build(keys: canonicalKeys, delimiter: ":")

        XCTAssertEqual(roots.map(\.segment), ["cache", "user"])
        XCTAssertEqual(roots.map(\.descendantKeyCount), [1, 2])
        XCTAssertEqual(roots[1].children.first?.segment, "1001")
        XCTAssertEqual(roots[1].children.first?.descendantKeyCount, 2)
    }

    func test_leafCarriesOriginalFullKey() {
        let roots = RedisKeyNamespaceTree.build(keys: canonicalKeys, delimiter: ":")
        let profile = roots.first { $0.segment == "user" }?
            .children.first?.children.first { $0.segment == "profile" }

        XCTAssertEqual(profile?.concreteKey, "user:1001:profile")
    }

    func test_preservesEmptySegmentsAndKeyAsNamespace() {
        let roots = RedisKeyNamespaceTree.build(keys: edgeCaseKeys, delimiter: ":")
        let user = roots.first { $0.segment == "user" }

        XCTAssertEqual(user?.concreteKey, "user")
        XCTAssertEqual(user?.descendantKeyCount, 3)
        XCTAssertEqual(user?.children.map(\.segment), ["", "1001"])
        XCTAssertEqual(user?.children.first?.concreteKey, "user:")
        XCTAssertEqual(RedisKeyNamespaceTree.displayLabel(for: ""), "(empty)")
        XCTAssertEqual(roots.first?.segment, "")
        XCTAssertNotNil(roots.first?.children.first { $0.segment == "" })
    }

    func test_deduplicatesAndSortsLexically() {
        let roots = RedisKeyNamespaceTree.build(keys: ["z", "a:b", "a:b", "a:a"], delimiter: ":")

        XCTAssertEqual(roots.map(\.segment), ["a", "z"])
        XCTAssertEqual(roots[0].children.map(\.segment), ["a", "b"])
        XCTAssertEqual(roots[0].descendantKeyCount, 2)
    }

    func test_supportsMultiCharacterDelimiter() {
        let roots = RedisKeyNamespaceTree.build(keys: ["team::one", "team::two"], delimiter: "::")

        XCTAssertEqual(roots.first?.segment, "team")
        XCTAssertEqual(roots.first?.children.map(\.segment), ["one", "two"])
    }

    func test_scanAccumulatorDeduplicatesBeforeCap() {
        var accumulator = RedisKeyScanAccumulator(maximumCount: 2)
        accumulator.append(["a", "a", "b"])

        XCTAssertEqual(
            accumulator.result(hasMoreCursor: false),
            RedisKeyScanResult(keys: ["a", "b"], isTruncated: false)
        )
    }

    func test_scanAccumulatorMarksCursorAndOverflowTruncation() {
        var cursorAccumulator = RedisKeyScanAccumulator(maximumCount: 2)
        cursorAccumulator.append(["a", "b"])
        XCTAssertTrue(cursorAccumulator.result(hasMoreCursor: true).isTruncated)

        var overflowAccumulator = RedisKeyScanAccumulator(maximumCount: 2)
        overflowAccumulator.append(["a", "b", "c"])
        XCTAssertEqual(overflowAccumulator.result(hasMoreCursor: false).keys, ["a", "b"])
        XCTAssertTrue(overflowAccumulator.result(hasMoreCursor: false).isTruncated)
    }

    func test_nodeIDsAreUniqueForEmptyAndOverlappingSegments() {
        let roots = RedisKeyNamespaceTree.build(
            keys: ["a:b", "a::b", ":leading", ":"],
            delimiter: ":"
        )
        let ids = allNodes(in: roots).map(\.id)

        XCTAssertEqual(Set(ids).count, ids.count)
    }

    private func allNodes(in nodes: [RedisKeyNamespaceNode]) -> [RedisKeyNamespaceNode] {
        nodes.flatMap { [$0] + allNodes(in: $0.children) }
    }
}
