// ExplainOutputViewTests.swift
// Pure-logic tests for the OSS-side EXPLAIN output renderer:
// the JSON pretty-printer and the read-only plan tree walker.
//
// We deliberately don't test the SwiftUI views — that's what manual UI test
// plan is for. These tests pin the data-shape contract so the views can't
// silently misbehave when JSON shapes change.

import XCTest
@testable import Gridex

final class ExplainOutputViewTests: XCTestCase {

    // MARK: - JSON pretty printer

    func test_prettyPrint_minifiedJSON_isReformatted() {
        let raw = #"[{"Plan":{"Node Type":"Seq Scan","Total Cost":42.0}}]"#
        let pretty = ExplainJSONPrettyPrinter.prettyPrint(raw)
        XCTAssertNotNil(pretty)
        // Pretty output adds newlines + indentation.
        XCTAssertTrue(pretty!.contains("\n"))
        XCTAssertTrue(pretty!.contains("Node Type"))
        XCTAssertTrue(pretty!.contains("Seq Scan"))
    }

    func test_prettyPrint_invalidJSON_returnsNil() {
        XCTAssertNil(ExplainJSONPrettyPrinter.prettyPrint("not json"))
        XCTAssertNil(ExplainJSONPrettyPrinter.prettyPrint(""))
        XCTAssertNil(ExplainJSONPrettyPrinter.prettyPrint("{ unclosed"))
    }

    func test_prettyPrint_sortedKeys_isStable() {
        // Two semantically identical inputs with different key order must
        // produce the same pretty output (`.sortedKeys` enabled).
        let a = #"{"b":2,"a":1}"#
        let b = #"{"a":1,"b":2}"#
        XCTAssertEqual(ExplainJSONPrettyPrinter.prettyPrint(a),
                       ExplainJSONPrettyPrinter.prettyPrint(b))
    }

    // MARK: - Plan reader — happy path

    func test_planReader_parses_singleNodePlan() {
        let json = """
        [{
            "Plan": {
                "Node Type": "Seq Scan",
                "Startup Cost": 0.0,
                "Total Cost": 22.0,
                "Plan Rows": 1000,
                "Plan Width": 64,
                "Relation Name": "profile"
            }
        }]
        """
        let nodes = ExplainPlanReader.parse(jsonString: json)
        XCTAssertNotNil(nodes)
        XCTAssertEqual(nodes?.count, 1)

        let root = nodes![0]
        XCTAssertEqual(root.nodeType, "Seq Scan")
        XCTAssertTrue(root.summary.contains("cost=0.00..22.00"),
                      "summary must include startup..total cost; got '\(root.summary)'")
        XCTAssertTrue(root.summary.contains("rows=1000"))
        XCTAssertTrue(root.summary.contains("width=64"))
        XCTAssertEqual(root.children.count, 0)

        // Relation Name must surface as an attribute (browseable but not parsed).
        XCTAssertTrue(root.attributes.contains { $0.0 == "Relation Name" && $0.1 == "profile" })
    }

    func test_planReader_walksNestedPlans() {
        let json = """
        [{
            "Plan": {
                "Node Type": "Hash Join",
                "Total Cost": 100.0,
                "Plans": [
                    { "Node Type": "Seq Scan", "Total Cost": 50.0 },
                    {
                        "Node Type": "Hash",
                        "Total Cost": 30.0,
                        "Plans": [
                            { "Node Type": "Index Scan", "Total Cost": 5.0 }
                        ]
                    }
                ]
            }
        }]
        """
        let nodes = ExplainPlanReader.parse(jsonString: json)!
        XCTAssertEqual(nodes.count, 1)

        let root = nodes[0]
        XCTAssertEqual(root.nodeType, "Hash Join")
        XCTAssertEqual(root.children.count, 2)
        XCTAssertEqual(root.children[0].nodeType, "Seq Scan")
        XCTAssertEqual(root.children[1].nodeType, "Hash")
        XCTAssertEqual(root.children[1].children.count, 1)
        XCTAssertEqual(root.children[1].children[0].nodeType, "Index Scan")
    }

    func test_planReader_excludesCostAndPlanKeys_fromAttributes() {
        // The summary line already shows cost / rows / width and children are
        // walked separately via Plans. Those keys must NOT appear in the
        // browseable attributes list — that would create duplicate noise.
        let json = """
        [{ "Plan": {
            "Node Type": "Seq Scan",
            "Startup Cost": 0.0, "Total Cost": 22.0,
            "Plan Rows": 1, "Plan Width": 32,
            "Plans": []
        }}]
        """
        let attrs = ExplainPlanReader.parse(jsonString: json)![0].attributes
        let keys = Set(attrs.map(\.0))
        XCTAssertFalse(keys.contains("Plans"))
        XCTAssertFalse(keys.contains("Node Type"))
        XCTAssertFalse(keys.contains("Startup Cost"))
        XCTAssertFalse(keys.contains("Total Cost"))
        XCTAssertFalse(keys.contains("Plan Rows"))
        XCTAssertFalse(keys.contains("Plan Width"))
    }

    func test_planReader_attributesAreSorted() {
        // Stable sort lets snapshot tests + UI rendering be deterministic.
        let json = """
        [{ "Plan": {
            "Node Type": "Index Scan",
            "Total Cost": 1.0,
            "Z Field": "z",
            "A Field": "a",
            "M Field": "m"
        }}]
        """
        let keys = ExplainPlanReader.parse(jsonString: json)![0].attributes.map(\.0)
        XCTAssertEqual(keys, keys.sorted())
    }

    // MARK: - Plan reader — error paths

    func test_planReader_returnsNil_onNonJSON() {
        XCTAssertNil(ExplainPlanReader.parse(jsonString: "Seq Scan on profile  (cost=0.00..22.00 rows=1000 width=64)"))
    }

    func test_planReader_returnsNil_onJSONWithoutPlanKey() {
        // Valid JSON but doesn't match the EXPLAIN shape.
        XCTAssertNil(ExplainPlanReader.parse(jsonString: #"[{"foo":"bar"}]"#))
        XCTAssertNil(ExplainPlanReader.parse(jsonString: "{}"))
        XCTAssertNil(ExplainPlanReader.parse(jsonString: "[]"))
    }

    func test_planReader_returnsNil_onEmptyString() {
        XCTAssertNil(ExplainPlanReader.parse(jsonString: ""))
    }

    // MARK: - Real PG output shape

    func test_planReader_handlesRealishPostgresOutput() {
        // Trimmed but representative of `EXPLAIN (FORMAT JSON, ANALYZE)`
        // output from PG 16. Contains every key shape the renderer cares
        // about: nested Plans, mixed primitive types, optional fields.
        let json = """
        [
          {
            "Plan": {
              "Node Type": "Hash Right Join",
              "Parallel Aware": false,
              "Async Capable": false,
              "Join Type": "Right",
              "Startup Cost": 2264.43,
              "Total Cost": 2369.01,
              "Plan Rows": 290,
              "Plan Width": 358,
              "Actual Startup Time": 0.085,
              "Actual Total Time": 61.929,
              "Actual Rows": 3378,
              "Actual Loops": 1,
              "Hash Cond": "(p.id = u.profile_id)",
              "Plans": [
                {
                  "Node Type": "Seq Scan",
                  "Parent Relationship": "Outer",
                  "Total Cost": 100.0,
                  "Plan Rows": 5000,
                  "Plan Width": 32,
                  "Relation Name": "users"
                }
              ]
            },
            "Planning Time": 0.123,
            "Execution Time": 62.1
          }
        ]
        """
        let nodes = ExplainPlanReader.parse(jsonString: json)
        XCTAssertNotNil(nodes)
        XCTAssertEqual(nodes?.count, 1)
        XCTAssertEqual(nodes?[0].nodeType, "Hash Right Join")
        XCTAssertEqual(nodes?[0].children.count, 1)
        XCTAssertEqual(nodes?[0].children[0].nodeType, "Seq Scan")
        // Booleans surface as "true"/"false" string attributes.
        let parallel = nodes?[0].attributes.first { $0.0 == "Parallel Aware" }
        XCTAssertEqual(parallel?.1, "false")
    }
}
