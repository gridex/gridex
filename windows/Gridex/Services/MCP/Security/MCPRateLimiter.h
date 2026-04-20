#pragma once
//
// MCPRateLimiter.h
// Gridex
//
// Per-connection sliding-window rate limiter. Mirrors
// macos/Services/MCP/Security/MCPRateLimiter.swift.
//
// Three independent buckets (query / write / DDL) keyed by
// connection id. Limits are injected from AppSettings at
// MCPServer startup (no runtime reconfiguration in v1).

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include "../../../Models/MCP/MCPPermissionTier.h"

namespace DBModels
{
    struct MCPRateLimitResult
    {
        bool allowed = true;
        int retryAfterSeconds = 0;
    };

    struct MCPRateLimiterConfig
    {
        int queriesPerMinute = 60;
        int queriesPerHour   = 1000;
        int writesPerMinute  = 10;
        int ddlPerMinute     = 1;
    };

    class MCPRateLimiter
    {
    public:
        explicit MCPRateLimiter(MCPRateLimiterConfig cfg = {}) : cfg_(cfg) {}

        void setConfig(const MCPRateLimiterConfig& cfg);

        MCPRateLimitResult checkLimit(MCPPermissionTier tier,
                                      const std::wstring& connectionId);
        void recordUsage(MCPPermissionTier tier,
                         const std::wstring& connectionId);

        void resetLimits(const std::wstring& connectionId);
        void resetAllLimits();

    private:
        using TimePoint = std::chrono::system_clock::time_point;

        struct RateBucket
        {
            std::vector<TimePoint> stamps;
            void record(TimePoint now) { stamps.push_back(now); }
            int countInWindow(TimePoint now, int windowSeconds) const;
            TimePoint oldestInWindow(TimePoint now, int windowSeconds) const;
            void cleanup(TimePoint now, int maxAgeSeconds);
        };

        mutable std::mutex mtx_;
        MCPRateLimiterConfig cfg_;
        std::unordered_map<std::wstring, RateBucket> queryBuckets_;
        std::unordered_map<std::wstring, RateBucket> writeBuckets_;
        std::unordered_map<std::wstring, RateBucket> ddlBuckets_;

        MCPRateLimitResult checkQuery(const std::wstring& id, TimePoint now);
        MCPRateLimitResult checkWrite(const std::wstring& id, TimePoint now);
        MCPRateLimitResult checkDDL(const std::wstring& id, TimePoint now);
    };
}
