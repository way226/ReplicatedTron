#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

class TickLatenessLogger
{
public:
    using Clock = std::chrono::steady_clock;

    TickLatenessLogger(std::string path,
                       std::string leaderRole,
                       std::string leaderId,
                       int tickIntervalMs,
                       std::size_t maxSamples)
        : path_(std::move(path)),
          leaderRole_(std::move(leaderRole)),
          leaderId_(std::move(leaderId)),
          tickIntervalMs_(tickIntervalMs),
          maxSamples_(maxSamples)
    {
        if (path_.empty() || path_ == "off")
            return;

        stream_.open(path_, std::ios::out | std::ios::trunc);
        if (!stream_.is_open())
        {
            std::cerr << "Warning: unable to open tick lateness log at " << path_ << ".\n";
            return;
        }

        stream_ << "sample_index,leader_role,leader_id,epoch,tick,due_at_ms,"
                   "tick_started_at_ms,publish_at_ms,tick_runtime_ms,tick_lateness_ms\n";
        stream_.flush();
    }

    bool enabled() const
    {
        return stream_.is_open();
    }

    const std::string &path() const
    {
        return path_;
    }

    std::size_t maxSamples() const
    {
        return maxSamples_;
    }

    void startSchedule(std::uint64_t firstTick, Clock::time_point scheduleStart)
    {
        if (!enabled())
            return;

        firstTick_ = firstTick;
        scheduleStart_ = scheduleStart;
        scheduleActive_ = true;
        samplesWritten_ = 0;
    }

    void logPublishedTick(std::uint64_t epoch,
                          std::uint64_t tick,
                          Clock::time_point tickStartedAt,
                          Clock::time_point publishAt)
    {
        if (!enabled() || !scheduleActive_)
            return;

        if (maxSamples_ != 0 && samplesWritten_ >= maxSamples_)
            return;

        if (tick < firstTick_)
            return;

        const auto dueAt = scheduleStart_ +
                           std::chrono::milliseconds(static_cast<long long>(tick - firstTick_) * tickIntervalMs_);

        const long long dueAtMs = relativeMilliseconds(dueAt);
        const long long tickStartedAtMs = relativeMilliseconds(tickStartedAt);
        const long long publishAtMs = relativeMilliseconds(publishAt);
        const long long tickRuntimeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(publishAt - tickStartedAt).count();
        const long long tickLatenessMs = publishAtMs > dueAtMs ? publishAtMs - dueAtMs : 0;

        stream_ << (samplesWritten_ + 1) << ','
                << leaderRole_ << ','
                << leaderId_ << ','
                << epoch << ','
                << tick << ','
                << dueAtMs << ','
                << tickStartedAtMs << ','
                << publishAtMs << ','
                << tickRuntimeMs << ','
                << tickLatenessMs << '\n';
        samplesWritten_++;
        if (samplesWritten_ % 25 == 0 || (maxSamples_ != 0 && samplesWritten_ == maxSamples_))
            stream_.flush();
    }

private:
    long long relativeMilliseconds(Clock::time_point timestamp) const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(timestamp - scheduleStart_).count();
    }

    std::string path_;
    std::string leaderRole_;
    std::string leaderId_;
    int tickIntervalMs_ = 0;
    std::size_t maxSamples_ = 0;
    std::size_t samplesWritten_ = 0;
    bool scheduleActive_ = false;
    std::uint64_t firstTick_ = 0;
    Clock::time_point scheduleStart_{};
    std::ofstream stream_;
};
