// RedisRequestCoordinatorTests.swift
// Gridex

import XCTest
@testable import Gridex

@MainActor
final class RedisRequestCoordinatorTests: XCTestCase {

    func test_olderNilCompletionIsSupersededBeforeNilIsClassifiedAsStale() async {
        let coordinator = RedisRequestCoordinator()
        let context = redisContext()
        let olderGate = RedisOwnershipTestGate()
        let newerGate = RedisOwnershipTestGate()

        let olderRequest = Task { @MainActor in
            await coordinator.perform(for: context) {
                await olderGate.enterAndWait()
                return Optional<Result<String, Error>>.none
            }
        }
        await olderGate.waitUntilEntered()

        let newerRequest = Task { @MainActor in
            await coordinator.perform(for: context) {
                await newerGate.enterAndWait()
                return Optional<Result<String, Error>>.none
            }
        }
        await newerGate.waitUntilEntered()

        await olderGate.release()
        let olderCompletion = await olderRequest.value

        guard case .superseded = olderCompletion else {
            return XCTFail("An older nil completion must not settle the newer request")
        }

        await newerGate.release()
        let newerCompletion = await newerRequest.value

        guard case .stale = newerCompletion else {
            return XCTFail("The current nil completion should be classified as stale")
        }
    }

    func test_currentSuccessIsReturnedAsCurrent() async {
        let coordinator = RedisRequestCoordinator()

        let completion = await coordinator.perform(for: redisContext()) {
            Optional<Result<String, Error>>.some(.success("current value"))
        }

        guard case .current(.success(let value)) = completion else {
            return XCTFail("Expected a current success")
        }
        XCTAssertEqual(value, "current value")
    }

    func test_currentFailureIsReturnedAsCurrent() async {
        let coordinator = RedisRequestCoordinator()

        let completion = await coordinator.perform(for: redisContext()) {
            Optional<Result<String, Error>>.some(.failure(ExpectedRequestError.failed))
        }

        guard case .current(.failure(let error)) = completion else {
            return XCTFail("Expected a current failure")
        }
        XCTAssertTrue(error is ExpectedRequestError)
    }

    func test_invalidateSupersedesRunningRequest() async {
        let coordinator = RedisRequestCoordinator()
        let gate = RedisOwnershipTestGate()

        let request = Task { @MainActor in
            await coordinator.perform(for: redisContext()) {
                await gate.enterAndWait()
                return Optional<Result<String, Error>>.some(.success("obsolete"))
            }
        }
        await gate.waitUntilEntered()

        coordinator.invalidate()
        await gate.release()

        let completion = await request.value
        guard case .superseded = completion else {
            return XCTFail("Invalidation must supersede an in-flight request")
        }
    }

    private func redisContext() -> AppState.RedisTabContext {
        AppState.RedisTabContext(
            connectionID: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            databaseName: "db0",
            sessionID: UUID(uuidString: "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA")!,
            databaseRevision: 3
        )
    }
}

private enum ExpectedRequestError: Error {
    case failed
}
