// RedisKeyDetailViewStateTests.swift
// Gridex

import XCTest
@testable import Gridex

@MainActor
final class RedisKeyDetailViewStateTests: XCTestCase {

    // MARK: - Load ownership

    func test_newerLoadKeepsOwnershipWhenOlderSuccessCompletesFirst() async {
        let state = RedisKeyDetailViewState()
        let context = redisContext()
        let olderGate = RedisOwnershipTestGate()
        let newerGate = RedisOwnershipTestGate()

        let olderLoad = Task { @MainActor in
            await state.load(in: context) {
                await olderGate.enterAndWait()
                return Optional<Result<RedisKeyDetail, Error>>.some(
                    .success(stringDetail(key: "obsolete", value: "old"))
                )
            }
        }
        await olderGate.waitUntilEntered()

        let newerLoad = Task { @MainActor in
            await state.load(in: context) {
                await newerGate.enterAndWait()
                return Optional<Result<RedisKeyDetail, Error>>.some(
                    .success(stringDetail(key: "current", value: "new"))
                )
            }
        }
        await newerGate.waitUntilEntered()

        await olderGate.release()
        await olderLoad.value

        XCTAssertTrue(state.isLoading)
        XCTAssertNil(state.detail)
        XCTAssertNil(state.errorMessage)

        await newerGate.release()
        await newerLoad.value

        XCTAssertFalse(state.isLoading)
        XCTAssertEqual(state.detail?.key, "current")
        XCTAssertNil(state.errorMessage)
    }

    func test_olderLoadFailureCompletingLastCannotReplaceNewerSuccess() async {
        let state = RedisKeyDetailViewState()
        let context = redisContext()
        let olderGate = RedisOwnershipTestGate()

        let olderLoad = Task { @MainActor in
            await state.load(in: context) {
                await olderGate.enterAndWait()
                return Optional<Result<RedisKeyDetail, Error>>.some(
                    .failure(ExpectedDetailError.obsoleteLoad)
                )
            }
        }
        await olderGate.waitUntilEntered()

        await state.load(in: context) {
            Optional<Result<RedisKeyDetail, Error>>.some(
                .success(stringDetail(key: "current", value: "new"))
            )
        }

        XCTAssertFalse(state.isLoading)
        XCTAssertEqual(state.detail?.key, "current")
        XCTAssertNil(state.errorMessage)

        await olderGate.release()
        await olderLoad.value

        XCTAssertFalse(state.isLoading)
        XCTAssertEqual(state.detail?.key, "current")
        XCTAssertNil(state.errorMessage)
    }

    func test_currentStaleLoadStopsLoadingWithoutPublishingDetailOrError() async {
        let state = RedisKeyDetailViewState()

        await state.load(in: redisContext()) {
            Optional<Result<RedisKeyDetail, Error>>.none
        }

        XCTAssertFalse(state.isLoading)
        XCTAssertNil(state.detail)
        XCTAssertNil(state.errorMessage)
    }

    func test_loadScopesRetainedDetailAndFailureToExactRedisContext() async {
        let state = RedisKeyDetailViewState()
        let db0 = redisContext(databaseName: "db0", databaseRevision: 3)
        let db1 = redisContext(databaseName: "db1", databaseRevision: 4)

        await state.load(in: db0) {
            Optional<Result<RedisKeyDetail, Error>>.some(
                .success(stringDetail(key: "db0:key", value: "db0"))
            )
        }
        XCTAssertEqual(state.detail?.key, "db0:key")

        await state.load(in: db1) {
            Optional<Result<RedisKeyDetail, Error>>.some(
                .failure(ExpectedDetailError.loadFailed)
            )
        }

        XCTAssertFalse(state.isLoading)
        XCTAssertNil(state.detail)
        XCTAssertEqual(state.errorMessage, "detail load failed")
    }

    // MARK: - Mutation ownership

    func test_newerMutationKeepsOwnershipWhenOlderSuccessCompletesFirst() async {
        let state = RedisKeyDetailViewState()
        let context = redisContext()
        let olderGate = RedisOwnershipTestGate()
        let newerGate = RedisOwnershipTestGate()
        var olderSideEffects = 0
        var newerSideEffects = 0

        let olderMutation = Task { @MainActor in
            await state.mutate(
                in: context,
                execute: {
                    await olderGate.enterAndWait()
                    return Optional<Result<RedisKeyDetail?, Error>>.some(
                        .success(stringDetail(key: "obsolete", value: "old"))
                    )
                },
                onCurrentSuccess: { olderSideEffects += 1 }
            )
        }
        await olderGate.waitUntilEntered()

        let newerMutation = Task { @MainActor in
            await state.mutate(
                in: context,
                execute: {
                    await newerGate.enterAndWait()
                    return Optional<Result<RedisKeyDetail?, Error>>.some(
                        .success(stringDetail(key: "current", value: "new"))
                    )
                },
                onCurrentSuccess: { newerSideEffects += 1 }
            )
        }
        await newerGate.waitUntilEntered()

        await olderGate.release()
        let olderApplied = await olderMutation.value

        XCTAssertFalse(olderApplied)
        XCTAssertTrue(state.isMutating)
        XCTAssertNil(state.detail)
        XCTAssertNil(state.errorMessage)
        XCTAssertEqual(olderSideEffects, 0)
        XCTAssertEqual(newerSideEffects, 0)

        await newerGate.release()
        let newerApplied = await newerMutation.value

        XCTAssertTrue(newerApplied)
        XCTAssertFalse(state.isMutating)
        XCTAssertEqual(state.detail?.key, "current")
        XCTAssertNil(state.errorMessage)
        XCTAssertEqual(olderSideEffects, 0)
        XCTAssertEqual(newerSideEffects, 1)
    }

    func test_olderMutationFailureCompletingLastCannotReplaceNewerSuccess() async {
        let state = RedisKeyDetailViewState()
        let context = redisContext()
        let olderGate = RedisOwnershipTestGate()
        var olderSideEffects = 0
        var newerSideEffects = 0

        let olderMutation = Task { @MainActor in
            await state.mutate(
                in: context,
                execute: {
                    await olderGate.enterAndWait()
                    return Optional<Result<RedisKeyDetail?, Error>>.some(
                        .failure(ExpectedDetailError.obsoleteMutation)
                    )
                },
                onCurrentSuccess: { olderSideEffects += 1 }
            )
        }
        await olderGate.waitUntilEntered()

        let newerApplied = await state.mutate(
            in: context,
            execute: {
                Optional<Result<RedisKeyDetail?, Error>>.some(
                    .success(stringDetail(key: "current", value: "new"))
                )
            },
            onCurrentSuccess: { newerSideEffects += 1 }
        )

        XCTAssertTrue(newerApplied)
        XCTAssertFalse(state.isMutating)
        XCTAssertEqual(state.detail?.key, "current")
        XCTAssertNil(state.errorMessage)
        XCTAssertEqual(olderSideEffects, 0)
        XCTAssertEqual(newerSideEffects, 1)

        await olderGate.release()
        let olderApplied = await olderMutation.value

        XCTAssertFalse(olderApplied)
        XCTAssertFalse(state.isMutating)
        XCTAssertEqual(state.detail?.key, "current")
        XCTAssertNil(state.errorMessage)
        XCTAssertEqual(olderSideEffects, 0)
        XCTAssertEqual(newerSideEffects, 1)
    }

    func test_currentStaleMutationStopsMutatingWithoutPublishingOrRunningSideEffect() async {
        let state = RedisKeyDetailViewState()
        var sideEffects = 0

        let applied = await state.mutate(
            in: redisContext(),
            execute: { Optional<Result<RedisKeyDetail?, Error>>.none },
            onCurrentSuccess: { sideEffects += 1 }
        )

        XCTAssertFalse(applied)
        XCTAssertFalse(state.isMutating)
        XCTAssertNil(state.detail)
        XCTAssertNil(state.errorMessage)
        XCTAssertEqual(sideEffects, 0)
    }

    func test_currentMutationFailurePublishesErrorWithoutRunningSideEffect() async {
        let state = RedisKeyDetailViewState()
        var sideEffects = 0

        let applied = await state.mutate(
            in: redisContext(),
            execute: {
                Optional<Result<RedisKeyDetail?, Error>>.some(
                    .failure(ExpectedDetailError.mutationFailed)
                )
            },
            onCurrentSuccess: { sideEffects += 1 }
        )

        XCTAssertFalse(applied)
        XCTAssertFalse(state.isMutating)
        XCTAssertNil(state.detail)
        XCTAssertEqual(state.errorMessage, "detail mutation failed")
        XCTAssertEqual(sideEffects, 0)
    }

    func test_loadRequestDoesNotSupersedeMutationOwnership() async {
        let state = RedisKeyDetailViewState()
        let context = redisContext()
        let mutationGate = RedisOwnershipTestGate()
        let loadGate = RedisOwnershipTestGate()
        var mutationSideEffects = 0

        let mutation = Task { @MainActor in
            await state.mutate(
                in: context,
                execute: {
                    await mutationGate.enterAndWait()
                    return Optional<Result<RedisKeyDetail?, Error>>.some(
                        .success(stringDetail(key: "mutated", value: "written"))
                    )
                },
                onCurrentSuccess: { mutationSideEffects += 1 }
            )
        }
        await mutationGate.waitUntilEntered()

        let load = Task { @MainActor in
            await state.load(in: context) {
                await loadGate.enterAndWait()
                return Optional<Result<RedisKeyDetail, Error>>.some(
                    .success(stringDetail(key: "loaded", value: "read"))
                )
            }
        }
        await loadGate.waitUntilEntered()

        await mutationGate.release()
        let mutationApplied = await mutation.value

        XCTAssertTrue(mutationApplied)
        XCTAssertFalse(state.isMutating)
        XCTAssertTrue(state.isLoading)
        XCTAssertEqual(state.detail?.key, "mutated")
        XCTAssertEqual(mutationSideEffects, 1)

        await loadGate.release()
        await load.value

        XCTAssertFalse(state.isLoading)
        XCTAssertFalse(state.isMutating)
        XCTAssertEqual(state.detail?.key, "loaded")
        XCTAssertEqual(mutationSideEffects, 1)
    }

    // MARK: - Mutation result semantics

    func test_currentRenameSuccessRunsNotificationSideEffectWithoutReplacingDetail() async {
        let state = RedisKeyDetailViewState()
        let context = redisContext()
        var notificationCount = 0

        await state.load(in: context) {
            Optional<Result<RedisKeyDetail, Error>>.some(
                .success(stringDetail(key: "old-name", value: "value"))
            )
        }

        let applied = await state.mutate(
            in: context,
            execute: {
                Optional<Result<RedisKeyDetail?, Error>>.some(.success(nil))
            },
            onCurrentSuccess: { notificationCount += 1 }
        )

        XCTAssertTrue(applied)
        XCTAssertFalse(state.isMutating)
        XCTAssertEqual(state.detail?.key, "old-name")
        XCTAssertNil(state.errorMessage)
        XCTAssertEqual(notificationCount, 1)
    }

    func test_currentTTLHashSetAndZSetMutationsPublishFetchedDetailAndClearDraft() async {
        let scenarios: [(RedisKeyDetail, [String], Int?)] = [
            (
                RedisKeyDetail(
                    key: "ttl:key",
                    type: .string,
                    ttl: 120,
                    data: .string(value: "ttl-value"),
                    memoryBytes: 9
                ),
                ["string:ttl-value"],
                120
            ),
            (
                RedisKeyDetail(
                    key: "hash:key",
                    type: .hash,
                    ttl: nil,
                    data: .hash(fields: [(field: "name", value: "Gridex")]),
                    memoryBytes: 18
                ),
                ["hash:name=Gridex"],
                nil
            ),
            (
                RedisKeyDetail(
                    key: "set:key",
                    type: .set,
                    ttl: nil,
                    data: .set(members: ["alpha", "beta"]),
                    memoryBytes: 27
                ),
                ["set:alpha", "set:beta"],
                nil
            ),
            (
                RedisKeyDetail(
                    key: "zset:key",
                    type: .zset,
                    ttl: nil,
                    data: .zset(members: [(member: "ranked", score: 4.5)]),
                    memoryBytes: 36
                ),
                ["zset:ranked=4.5"],
                nil
            )
        ]

        for (fetchedDetail, expectedData, expectedTTL) in scenarios {
            let state = RedisKeyDetailViewState()
            var draft = "dirty"
            var executeCount = 0

            let applied = await state.mutate(
                in: redisContext(),
                execute: {
                    executeCount += 1
                    return Optional<Result<RedisKeyDetail?, Error>>.some(
                        .success(fetchedDetail)
                    )
                },
                onCurrentSuccess: { draft = "" }
            )

            XCTAssertTrue(applied)
            XCTAssertEqual(executeCount, 1)
            XCTAssertEqual(state.detail?.key, fetchedDetail.key)
            XCTAssertEqual(state.detail?.ttl, expectedTTL)
            XCTAssertEqual(detailDataSnapshot(state.detail?.data), expectedData)
            XCTAssertNil(state.errorMessage)
            XCTAssertFalse(state.isMutating)
            XCTAssertEqual(draft, "")
        }
    }

    // MARK: - Fixtures

    private func redisContext(
        databaseName: String = "db0",
        databaseRevision: UInt64 = 3
    ) -> AppState.RedisTabContext {
        AppState.RedisTabContext(
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: databaseName,
            sessionID: UUID(uuidString: "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA")!,
            databaseRevision: databaseRevision
        )
    }

    private func stringDetail(
        key: String,
        value: String,
        ttl: Int? = nil
    ) -> RedisKeyDetail {
        RedisKeyDetail(
            key: key,
            type: .string,
            ttl: ttl,
            data: .string(value: value),
            memoryBytes: nil
        )
    }

    private func detailDataSnapshot(_ data: RedisKeyData?) -> [String] {
        guard let data else { return [] }
        switch data {
        case .string(let value):
            return ["string:\(value)"]
        case .hash(let fields):
            return fields.map { "hash:\($0.field)=\($0.value)" }
        case .list(let items):
            return items.map { "list:\($0)" }
        case .set(let members):
            return members.map { "set:\($0)" }
        case .zset(let members):
            return members.map { "zset:\($0.member)=\($0.score)" }
        }
    }
}

private enum ExpectedDetailError: LocalizedError {
    case loadFailed
    case obsoleteLoad
    case mutationFailed
    case obsoleteMutation

    var errorDescription: String? {
        switch self {
        case .loadFailed:
            "detail load failed"
        case .obsoleteLoad:
            "obsolete detail load failed"
        case .mutationFailed:
            "detail mutation failed"
        case .obsoleteMutation:
            "obsolete detail mutation failed"
        }
    }
}
