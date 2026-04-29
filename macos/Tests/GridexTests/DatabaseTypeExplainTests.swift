// DatabaseTypeExplainTests.swift — pure-logic checks for DatabaseType.explainSQL.
// No infra needed. Locks the per-engine SQL shape so the QueryEditor button
// and the MCP explain_query tool can't drift from each other again
// (regression for issue #49: hardcoded "EXPLAIN QUERY PLAN" on Postgres).

import XCTest
@testable import Gridex

final class DatabaseTypeExplainTests: XCTestCase {

    private let sql = "SELECT * FROM profile"

    func test_postgres_uses_optionListExplain_notQueryPlan() {
        let out = DatabaseType.postgresql.explainSQL(for: sql)
        XCTAssertEqual(out, "EXPLAIN (ANALYZE false, COSTS true, FORMAT TEXT) \(sql)")
        // Issue #49 receipt: PG must NEVER receive SQLite-style "EXPLAIN QUERY PLAN".
        XCTAssertFalse(out?.uppercased().contains("QUERY PLAN") ?? true,
            "PG EXPLAIN must not contain 'QUERY PLAN' (SQLite-only syntax)")
    }

    func test_mysql_usesPlainExplain() {
        XCTAssertEqual(DatabaseType.mysql.explainSQL(for: sql), "EXPLAIN \(sql)")
    }

    func test_sqlite_usesQueryPlan() {
        XCTAssertEqual(DatabaseType.sqlite.explainSQL(for: sql), "EXPLAIN QUERY PLAN \(sql)")
    }

    func test_mssql_usesShowplanToggle() {
        XCTAssertEqual(DatabaseType.mssql.explainSQL(for: sql),
                       "SET SHOWPLAN_TEXT ON; \(sql); SET SHOWPLAN_TEXT OFF")
    }

    func test_clickhouse_usesPlainExplain() {
        XCTAssertEqual(DatabaseType.clickhouse.explainSQL(for: sql), "EXPLAIN \(sql)")
    }

    func test_mongo_returnsNil() {
        XCTAssertNil(DatabaseType.mongodb.explainSQL(for: sql),
                     "Mongo has no SQL EXPLAIN; must return nil so callers show the right error")
    }

    func test_redis_returnsNil() {
        XCTAssertNil(DatabaseType.redis.explainSQL(for: sql))
    }

    func test_blankSQL_returnsNil_acrossAllEngines() {
        for engine in DatabaseType.allCases {
            XCTAssertNil(engine.explainSQL(for: ""),  "\(engine): empty input")
            XCTAssertNil(engine.explainSQL(for: "   \n\t  "), "\(engine): whitespace-only input")
        }
    }

    func test_trimsLeadingTrailingWhitespace() {
        let padded = "  \n  SELECT 1  \n  "
        XCTAssertEqual(DatabaseType.postgresql.explainSQL(for: padded),
                       "EXPLAIN (ANALYZE false, COSTS true, FORMAT TEXT) SELECT 1")
    }

    func test_allEnginesHandled_noUnknownCase() {
        // Belt-and-braces: if a future engine is added to DatabaseType, this loop
        // forces a decision (return SQL or return nil with a comment).
        for engine in DatabaseType.allCases {
            switch engine {
            case .mongodb, .redis:
                XCTAssertNil(engine.explainSQL(for: sql))
            case .postgresql, .mysql, .sqlite, .mssql, .clickhouse:
                XCTAssertNotNil(engine.explainSQL(for: sql))
            }
        }
    }
}
