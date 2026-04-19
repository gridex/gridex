// swift-tools-version: 5.10

import PackageDescription

let package = Package(
    name: "Gridex",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .executable(name: "Gridex", targets: ["Gridex"]),
    ],
    dependencies: [
        // PostgreSQL driver
        .package(url: "https://github.com/vapor/postgres-nio.git", from: "1.21.0"),
        // MySQL driver
        .package(url: "https://github.com/vapor/mysql-nio.git", from: "1.7.0"),
        // TLS for Redis and other NIO connections
        .package(url: "https://github.com/apple/swift-nio-ssl.git", from: "2.27.0"),
        // SSH tunneling
        .package(url: "https://github.com/apple/swift-nio-ssh.git", from: "0.8.0"),
        // Redis driver
        .package(url: "https://github.com/swift-server/RediStack.git", from: "1.6.0"),
        // MongoDB driver (pure Swift, NIO-based)
        .package(url: "https://github.com/orlandos-nl/MongoKitten.git", from: "7.9.0"),
        // MSSQL driver (TDS 7.4, pure Swift NIO, no FreeTDS)
        .package(url: "https://github.com/vkuttyp/CosmoSQLClient-Swift.git", branch: "main"),
        // Sparkle — macOS auto-update framework
        .package(url: "https://github.com/sparkle-project/Sparkle", from: "2.6.0"),
    ],
    targets: [
        .executableTarget(
            name: "Gridex",
            dependencies: [
                .product(name: "PostgresNIO", package: "postgres-nio"),
                .product(name: "MySQLNIO", package: "mysql-nio"),
                .product(name: "RediStack", package: "RediStack"),
                .product(name: "MongoKitten", package: "MongoKitten"),
                .product(name: "MongoClient", package: "MongoKitten"),
                .product(name: "CosmoMSSQL", package: "CosmoSQLClient-Swift"),
                .product(name: "NIOSSL", package: "swift-nio-ssl"),
                .product(name: "NIOSSH", package: "swift-nio-ssh"),
                .product(name: "Sparkle", package: "Sparkle"),
            ],
            path: "macos",
            exclude: [
                "Resources/Info.plist",
            ],
            resources: [
                .copy("Resources/Gridex.entitlements"),
                .process("Resources/Assets.xcassets"),
            ],
            linkerSettings: [
                .linkedLibrary("sqlite3"),
                .unsafeFlags([
                    "-Xlinker", "-sectcreate",
                    "-Xlinker", "__TEXT",
                    "-Xlinker", "__info_plist",
                    "-Xlinker", "macos/Resources/Info.plist",
                ]),
            ]
        ),
    ]
)
