#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

struct ReplicatedPlayerState
{
    std::string clientId;
    std::string name;
    char symbol = '?';
    int x = 0;
    int y = 0;
    int spawnX = 0;
    int spawnY = 0;
    std::string dir = "RIGHT";
    bool alive = true;
};

struct ReplicatedInput
{
    std::uint64_t epochHint = 0;
    std::string clientId;
    std::uint64_t clientSeq = 0;
    std::uint64_t targetTick = 0;
    std::string command;
};

struct HeartbeatMessage
{
    std::uint64_t epoch = 1;
    std::uint64_t publishedTick = 0;
    std::uint64_t safeTickHint = 0;
    std::string leaseStatus = "UNKNOWN";
    std::string leaderId;
};

struct SafeTickReport
{
    std::uint64_t epoch = 1;
    std::uint64_t safeTick = 0;
    std::string stateHash;
};

struct TickSeal
{
    std::uint64_t epoch = 1;
    std::uint64_t tick = 0;
    std::vector<ReplicatedInput> acceptedInputs;
    std::vector<std::string> authoritativeEvents;
    std::string stateHash;
};

struct ReplicationCheckpoint
{
    std::uint64_t epoch = 1;
    std::uint64_t tick = 0;
    std::uint64_t publishedTick = 0;
    std::uint64_t safeTick = 0;
    std::string phase = "WAITING";
    int countdownTicksRemaining = 0;
    int matchTicksRemaining = 0;
    std::string winnerName;
    char winnerSymbol = '\0';
    std::vector<ReplicatedPlayerState> players;
    std::vector<std::string> spectators;
    std::vector<std::string> gridRows;
    std::string stateHash;
};

inline std::string protocolEscape(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
    {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.')
        {
            out.push_back(static_cast<char>(c));
            continue;
        }

        static const char hex[] = "0123456789ABCDEF";
        out.push_back('%');
        out.push_back(hex[(c >> 4) & 0x0F]);
        out.push_back(hex[c & 0x0F]);
    }

    return out;
}

inline int protocolHexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    return -1;
}

inline std::string protocolUnescape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '%' && i + 2 < text.size())
        {
            int hi = protocolHexValue(text[i + 1]);
            int lo = protocolHexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }

        out.push_back(text[i]);
    }

    return out;
}

inline std::vector<std::string> protocolSplitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string current;

    for (char c : text)
    {
        if (c == '\r')
            continue;

        if (c == '\n')
        {
            lines.push_back(current);
            current.clear();
            continue;
        }

        current.push_back(c);
    }

    if (!current.empty())
        lines.push_back(current);

    return lines;
}

inline std::vector<std::string> protocolSplit(const std::string &text, char delimiter)
{
    std::vector<std::string> parts;
    std::string current;

    for (char c : text)
    {
        if (c == delimiter)
        {
            parts.push_back(current);
            current.clear();
            continue;
        }

        current.push_back(c);
    }

    parts.push_back(current);
    return parts;
}

inline std::string formatFNV1a64(std::uint64_t value)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

inline std::string fnv1a64Hex(const std::string &text)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text)
    {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }

    return formatFNV1a64(hash);
}

inline std::string buildCheckpointHashMaterial(const ReplicationCheckpoint &checkpoint)
{
    std::vector<ReplicatedPlayerState> players = checkpoint.players;
    std::sort(players.begin(), players.end(), [](const ReplicatedPlayerState &a, const ReplicatedPlayerState &b)
              {
                  if (a.clientId != b.clientId)
                      return a.clientId < b.clientId;
                  return a.name < b.name;
              });

    std::vector<std::string> spectators = checkpoint.spectators;
    std::sort(spectators.begin(), spectators.end());

    std::ostringstream out;
    out << "epoch=" << checkpoint.epoch << "\n";
    out << "tick=" << checkpoint.tick << "\n";
    out << "phase=" << checkpoint.phase << "\n";
    out << "countdown_ticks=" << checkpoint.countdownTicksRemaining << "\n";
    out << "match_ticks=" << checkpoint.matchTicksRemaining << "\n";
    out << "winner_name=" << protocolEscape(checkpoint.winnerName) << "\n";
    out << "winner_symbol=" << (checkpoint.winnerSymbol == '\0' ? "NONE" : std::string(1, checkpoint.winnerSymbol)) << "\n";

    for (const ReplicatedPlayerState &player : players)
    {
        out << "PLAYER|"
            << protocolEscape(player.clientId) << "|"
            << protocolEscape(player.name) << "|"
            << player.symbol << "|"
            << player.x << "|"
            << player.y << "|"
            << player.spawnX << "|"
            << player.spawnY << "|"
            << protocolEscape(player.dir) << "|"
            << (player.alive ? 1 : 0) << "\n";
    }

    for (const std::string &spectator : spectators)
        out << "SPECTATOR|" << protocolEscape(spectator) << "\n";

    out << "GRID\n";
    for (const std::string &row : checkpoint.gridRows)
        out << row << "\n";
    out << "END_GRID\n";
    return out.str();
}

inline std::string computeStateHash(const ReplicationCheckpoint &checkpoint)
{
    return fnv1a64Hex(buildCheckpointHashMaterial(checkpoint));
}

inline std::string serializeHeartbeat(const HeartbeatMessage &heartbeat)
{
    std::ostringstream out;
    out << "HEARTBEAT\n";
    out << "epoch=" << heartbeat.epoch << "\n";
    out << "published_tick=" << heartbeat.publishedTick << "\n";
    out << "safe_tick_hint=" << heartbeat.safeTickHint << "\n";
    out << "lease_status=" << protocolEscape(heartbeat.leaseStatus) << "\n";
    out << "leader_id=" << protocolEscape(heartbeat.leaderId) << "\n";
    out << "END_HEARTBEAT\n";
    return out.str();
}

inline bool parseHeartbeat(const std::string &raw, HeartbeatMessage &heartbeat)
{
    std::vector<std::string> lines = protocolSplitLines(raw);
    if (lines.empty() || lines.front() != "HEARTBEAT")
        return false;

    heartbeat = HeartbeatMessage{};
    for (size_t i = 1; i < lines.size(); i++)
    {
        const std::string &line = lines[i];
        if (line == "END_HEARTBEAT")
            break;

        size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);

        if (key == "epoch")
            heartbeat.epoch = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "published_tick")
            heartbeat.publishedTick = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "safe_tick_hint")
            heartbeat.safeTickHint = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "lease_status")
            heartbeat.leaseStatus = protocolUnescape(value);
        else if (key == "leader_id")
            heartbeat.leaderId = protocolUnescape(value);
    }

    return true;
}

inline std::string serializeSafeTickReport(const SafeTickReport &report)
{
    std::ostringstream out;
    out << "SAFE_TICK\n";
    out << "epoch=" << report.epoch << "\n";
    out << "safe_tick=" << report.safeTick << "\n";
    out << "state_hash=" << protocolEscape(report.stateHash) << "\n";
    out << "END_SAFE_TICK\n";
    return out.str();
}

inline bool parseSafeTickReport(const std::string &raw, SafeTickReport &report)
{
    std::vector<std::string> lines = protocolSplitLines(raw);
    if (lines.empty() || lines.front() != "SAFE_TICK")
        return false;

    report = SafeTickReport{};
    for (size_t i = 1; i < lines.size(); i++)
    {
        const std::string &line = lines[i];
        if (line == "END_SAFE_TICK")
            break;

        size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);

        if (key == "epoch")
            report.epoch = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "safe_tick")
            report.safeTick = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "state_hash")
            report.stateHash = protocolUnescape(value);
    }

    return true;
}

inline std::string serializeTickSeal(const TickSeal &seal)
{
    std::ostringstream out;
    out << "TICK_SEAL\n";
    out << "epoch=" << seal.epoch << "\n";
    out << "tick=" << seal.tick << "\n";
    out << "state_hash=" << protocolEscape(seal.stateHash) << "\n";

    for (const ReplicatedInput &input : seal.acceptedInputs)
    {
        out << "INPUT|"
            << input.epochHint << "|"
            << protocolEscape(input.clientId) << "|"
            << input.clientSeq << "|"
            << input.targetTick << "|"
            << protocolEscape(input.command) << "\n";
    }

    for (const std::string &event : seal.authoritativeEvents)
        out << "EVENT|" << protocolEscape(event) << "\n";

    out << "END_TICK_SEAL\n";
    return out.str();
}

inline bool parseTickSeal(const std::string &raw, TickSeal &seal)
{
    std::vector<std::string> lines = protocolSplitLines(raw);
    if (lines.empty() || lines.front() != "TICK_SEAL")
        return false;

    seal = TickSeal{};
    for (size_t i = 1; i < lines.size(); i++)
    {
        const std::string &line = lines[i];
        if (line.empty())
            continue;

        if (line == "END_TICK_SEAL")
            break;

        if (line.rfind("INPUT|", 0) == 0)
        {
            std::vector<std::string> parts = protocolSplit(line, '|');
            if (parts.size() != 6)
                return false;

            ReplicatedInput input;
            input.epochHint = static_cast<std::uint64_t>(std::stoull(parts[1]));
            input.clientId = protocolUnescape(parts[2]);
            input.clientSeq = static_cast<std::uint64_t>(std::stoull(parts[3]));
            input.targetTick = static_cast<std::uint64_t>(std::stoull(parts[4]));
            input.command = protocolUnescape(parts[5]);
            seal.acceptedInputs.push_back(input);
            continue;
        }

        if (line.rfind("EVENT|", 0) == 0)
        {
            seal.authoritativeEvents.push_back(protocolUnescape(line.substr(6)));
            continue;
        }

        size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);

        if (key == "epoch")
            seal.epoch = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "tick")
            seal.tick = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "state_hash")
            seal.stateHash = protocolUnescape(value);
    }

    return true;
}

inline std::string serializeCheckpoint(const ReplicationCheckpoint &checkpointInput)
{
    ReplicationCheckpoint checkpoint = checkpointInput;
    if (checkpoint.stateHash.empty())
        checkpoint.stateHash = computeStateHash(checkpoint);

    std::vector<ReplicatedPlayerState> players = checkpoint.players;
    std::sort(players.begin(), players.end(), [](const ReplicatedPlayerState &a, const ReplicatedPlayerState &b)
              {
                  if (a.clientId != b.clientId)
                      return a.clientId < b.clientId;
                  return a.name < b.name;
              });

    std::vector<std::string> spectators = checkpoint.spectators;
    std::sort(spectators.begin(), spectators.end());

    std::ostringstream out;
    out << "CHECKPOINT\n";
    out << "epoch=" << checkpoint.epoch << "\n";
    out << "tick=" << checkpoint.tick << "\n";
    out << "published_tick=" << checkpoint.publishedTick << "\n";
    out << "safe_tick=" << checkpoint.safeTick << "\n";
    out << "phase=" << checkpoint.phase << "\n";
    out << "countdown_ticks=" << checkpoint.countdownTicksRemaining << "\n";
    out << "match_ticks=" << checkpoint.matchTicksRemaining << "\n";
    out << "winner_name=" << protocolEscape(checkpoint.winnerName) << "\n";
    out << "winner_symbol=" << (checkpoint.winnerSymbol == '\0' ? "NONE" : std::string(1, checkpoint.winnerSymbol)) << "\n";
    out << "state_hash=" << protocolEscape(checkpoint.stateHash) << "\n";

    for (const ReplicatedPlayerState &player : players)
    {
        out << "PLAYER|"
            << protocolEscape(player.clientId) << "|"
            << protocolEscape(player.name) << "|"
            << player.symbol << "|"
            << player.x << "|"
            << player.y << "|"
            << player.spawnX << "|"
            << player.spawnY << "|"
            << protocolEscape(player.dir) << "|"
            << (player.alive ? 1 : 0) << "\n";
    }

    for (const std::string &spectator : spectators)
        out << "SPECTATOR|" << protocolEscape(spectator) << "\n";

    out << "GRID\n";
    for (const std::string &row : checkpoint.gridRows)
        out << row << "\n";
    out << "END_GRID\n";
    out << "END_CHECKPOINT\n";
    return out.str();
}

inline bool parseCheckpoint(const std::string &raw, ReplicationCheckpoint &checkpoint)
{
    std::vector<std::string> lines = protocolSplitLines(raw);
    if (lines.empty() || lines.front() != "CHECKPOINT")
        return false;

    checkpoint = ReplicationCheckpoint{};
    bool inGrid = false;

    for (size_t i = 1; i < lines.size(); i++)
    {
        const std::string &line = lines[i];
        if (line.empty())
            continue;

        if (inGrid)
        {
            if (line == "END_GRID")
            {
                inGrid = false;
                continue;
            }

            checkpoint.gridRows.push_back(line);
            continue;
        }

        if (line == "GRID")
        {
            inGrid = true;
            continue;
        }

        if (line == "END_CHECKPOINT")
            break;

        if (line.rfind("PLAYER|", 0) == 0)
        {
            std::vector<std::string> parts = protocolSplit(line, '|');
            if (parts.size() != 10)
                return false;

            ReplicatedPlayerState player;
            player.clientId = protocolUnescape(parts[1]);
            player.name = protocolUnescape(parts[2]);
            if (parts[3].size() != 1)
                return false;
            player.symbol = parts[3][0];
            player.x = std::stoi(parts[4]);
            player.y = std::stoi(parts[5]);
            player.spawnX = std::stoi(parts[6]);
            player.spawnY = std::stoi(parts[7]);
            player.dir = protocolUnescape(parts[8]);
            player.alive = parts[9] == "1";
            checkpoint.players.push_back(player);
            continue;
        }

        if (line.rfind("SPECTATOR|", 0) == 0)
        {
            std::vector<std::string> parts = protocolSplit(line, '|');
            if (parts.size() != 2)
                return false;

            checkpoint.spectators.push_back(protocolUnescape(parts[1]));
            continue;
        }

        size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);

        if (key == "epoch")
            checkpoint.epoch = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "tick")
            checkpoint.tick = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "published_tick")
            checkpoint.publishedTick = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "safe_tick")
            checkpoint.safeTick = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "phase")
            checkpoint.phase = value;
        else if (key == "countdown_ticks")
            checkpoint.countdownTicksRemaining = std::stoi(value);
        else if (key == "match_ticks")
            checkpoint.matchTicksRemaining = std::stoi(value);
        else if (key == "winner_name")
            checkpoint.winnerName = protocolUnescape(value);
        else if (key == "winner_symbol")
            checkpoint.winnerSymbol = value == "NONE" || value.empty() ? '\0' : value[0];
        else if (key == "state_hash")
            checkpoint.stateHash = protocolUnescape(value);
    }

    if (inGrid)
        return false;

    if (checkpoint.stateHash.empty())
        checkpoint.stateHash = computeStateHash(checkpoint);

    return true;
}

inline std::string buildHelloLine(const std::string &clientId, const std::string &name)
{
    return "HELLO|" + protocolEscape(clientId) + "|" + protocolEscape(name) + "\n";
}

inline bool parseHelloLine(const std::string &line, std::string &clientId, std::string &name)
{
    if (line.rfind("HELLO|", 0) != 0)
        return false;

    std::vector<std::string> parts = protocolSplit(line, '|');
    if (parts.size() != 3)
        return false;

    clientId = protocolUnescape(parts[1]);
    name = protocolUnescape(parts[2]);
    return true;
}

inline std::string serializeInputLine(const ReplicatedInput &input)
{
    std::ostringstream out;
    out << "INPUT|"
        << input.epochHint << "|"
        << protocolEscape(input.clientId) << "|"
        << input.clientSeq << "|"
        << input.targetTick << "|"
        << protocolEscape(input.command) << "\n";
    return out.str();
}

inline bool parseInputLine(const std::string &line, ReplicatedInput &input)
{
    if (line.rfind("INPUT|", 0) != 0)
        return false;

    std::vector<std::string> parts = protocolSplit(line, '|');
    if (parts.size() != 6)
        return false;

    input = ReplicatedInput{};
    input.epochHint = static_cast<std::uint64_t>(std::stoull(parts[1]));
    input.clientId = protocolUnescape(parts[2]);
    input.clientSeq = static_cast<std::uint64_t>(std::stoull(parts[3]));
    input.targetTick = static_cast<std::uint64_t>(std::stoull(parts[4]));
    input.command = protocolUnescape(parts[5]);
    return true;
}
