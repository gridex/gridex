import XCTest
@testable import Gridex

final class SidebarSchemaSelectionTests: XCTestCase {
    func test_postgresPreservesPreviouslySelectedAccessibleSchema() {
        XCTAssertEqual(
            SidebarSchemaSelection.resolve(
                previous: "tenant_b",
                for: .postgresql,
                schemas: ["public", "tenant_a", "tenant_b"]
            ),
            "tenant_b"
        )
    }

    func test_postgresPrefersPublicWhenPreviousSchemaIsMissing() {
        XCTAssertEqual(
            SidebarSchemaSelection.resolve(
                previous: "removed",
                for: .postgresql,
                schemas: ["tenant_a", "public"]
            ),
            "public"
        )
    }

    func test_postgresUsesFirstSortedSchemaWhenPublicIsAbsent() {
        XCTAssertEqual(
            SidebarSchemaSelection.resolve(
                previous: nil,
                for: .postgresql,
                schemas: ["tenant_b", "tenant_a"]
            ),
            "tenant_a"
        )
    }

    func test_nonPostgresNeverCreatesSidebarSchemaSelection() {
        XCTAssertNil(
            SidebarSchemaSelection.resolve(
                previous: "app",
                for: .mysql,
                schemas: ["app", "information_schema"]
            )
        )
    }

    func test_sidebarItemRetainsSchemaForNestedActions() {
        let item = SidebarItem(
            title: "orders",
            type: .table("orders"),
            schema: "tenant_a"
        )
        XCTAssertEqual(item.schema, "tenant_a")
    }

    func test_tableReferenceDistinguishesSameNameAcrossSchemas() {
        let publicOrders = AppState.TableReference(name: "orders", schema: "public")
        let tenantOrders = AppState.TableReference(name: "orders", schema: "tenant_a")
        XCTAssertNotEqual(publicOrders, tenantOrders)
        XCTAssertEqual(Set([publicOrders, tenantOrders]).count, 2)
    }
}
