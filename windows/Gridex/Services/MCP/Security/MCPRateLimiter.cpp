//
// MCPRateLimiter.cpp
//

#include "MCPRateLimiter.h"
#include <algorithm>

namespace DBModels
{
    using TimePoint = std::chrono::system_clock::time_point;

    // ── RateBucket ───────────────────────────────────────────

    int MCPRateLimiter::RateBucket::countInWindow(TimePoint now, int windowSeconds) const
    {
        const auto cutoff = now - std::chrono::seconds(windowSeconds);
        int n = 0;
        for (const auto& t : stamps) if (t > cutoff) ++n;
        return n;
    }

    TimePoint MCPRateLimiter::RateBucket::oldestInWindow(TimePoint now, int windowSeconds) const
    {
        const auto cutoff = now - std::chrono::seconds(windowSeconds);
        TimePoint best{};
        bool have = false;
        for (const auto& t : stamps)
        {
            if (t > cutoff && (!have || t < best)) { best = t; have = true; }
        }
        return have ? best : now;
    }

    void MCPRateLimiter::RateBucket::cleanup(TimePoint now, int maxAgeSeconds)
    {
        const auto cutoff = now - std::chrono::seconds(maxAgeSeconds);
        stamps.erase(
            std::remove_if(stamps.begin(), stamps.end(),
                [cutoff](const TimePoint& t) { return t <= cutoff; }),
            stamps.end());
    }

    // ── Public API ───────────────────────────────────────────

    void MCPRateLimiter::setConfig(const MCPRateLimiterConfig& c)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cfg_ = c;
    }

    MCPRateLimitResult MCPRateLimiter::checkLimit(
        MCPPermissionTier tier, const std::wstring& id)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto now = std::chrono::system_clock::now();
        switch (tier)
        {
            case MCPPermissionTier::Schema:
            case MCPPermissionTier::Read:
            case MCPPermissionTier::Advanced:
                return checkQuery(id, now);
            case MCPPermissionTier::Write:
                return checkWrite(id, now);
            case MCPPermissionTier::DDL:
                return checkDDL(id, now);
        }
        return { true, 0 };
    }

    void MCPRateLimiter::recordUsage(MCPPermissionTier tier, const std::wstring& id)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto now = std::chrono::system_clock::now();
        auto record = [now](auto& map, const std::wstring& key, int maxAge)
        {
            auto& bucket = map[key];
            bucket.record(now);
            bucket.cleanup(now, maxAge);
        };
        switch (tier)
        {
            case MCPPermissionTier::Schema:
            case MCPPermissionTier::Read:
            case MCPPermissionTier::Advanced:
                record(queryBuckets_, id, 3600); break;
            case MCPPermissionTier::Write:
                record(writeBuckets_, id, 60); break;
            case MCPPermissionTier::DDL:
                record(ddlBuckets_, id, 60); break;
        }
    }

    void MCPRateLimiter::resetLimits(const std::wstring& id)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queryBuckets_.erase(id);
        writeBuckets_.erase(id);
        ddlBuckets_.erase(id);
    }

    void MCPRateLimiter::resetAllLimits()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queryBuckets_.clear();
        writeBuckets_.clear();
        ddlBuckets_.clear();
    }

    // ── Internal: per-tier check ─────────────────────────────
    // (callers hold mtx_)

    MCPRateLimitResult MCPRateLimiter::checkQuery(const std::wstring& id, TimePoint now)
    {
        auto& b = queryBuckets_[id];
        const int minuteCount = b.countInWindow(now, 60);
        if (minuteCount >= cfg_.queriesPerMinute)
        {
            const auto oldest = b.oldestInWindow(now, 60);
            const int retry = 60 - static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - oldest).count());
            return { false, std::max(1, retry) };
        }
        const int hourCount = b.countInWindow(now, 3600);
        if (hourCount >= cfg_.queriesPerHour)
        {
            const auto oldest = b.oldestInWindow(now, 3600);
            const int retry = 3600 - static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - oldest).count());
            return { false, std::max(1, retry) };
        }
        return { true, 0 };
    }

    MCPRateLimitResult MCPRateLimiter::checkWrite(const std::wstring& id, TimePoint now)
    {
        auto& b = writeBuckets_[id];
        const int n = b.countInWindow(now, 60);
        if (n >= cfg_.writesPerMinute)
        {
            const auto oldest = b.oldestInWindow(now, 60);
            const int retry = 60 - static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - oldest).count());
            return { false, std::max(1, retry) };
        }
        return { true, 0 };
    }

    MCPRateLimitResult MCPRateLimiter::checkDDL(const std::wstring& id, TimePoint now)
    {
        auto& b = ddlBuckets_[id];
        const int n = b.countInWindow(now, 60);
        if (n >= cfg_.ddlPerMinute)
        {
            const auto oldest = b.oldestInWindow(now, 60);
            const int retry = 60 - static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - oldest).count());
            return { false, std::max(1, retry) };
        }
        return { true, 0 };
    }
}
