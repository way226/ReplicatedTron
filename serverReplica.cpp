#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "replication_protocol.h"
#include "sunlab_config.h"
#include "tick_lateness_logger.h"

#define WIDTH 100
#define HEIGHT 50

constexpr int kServerTickMs = 150;
constexpr int kMinimumPlayersToStart = 1;
constexpr int kCountdownTicks = 34;
constexpr int kMatchDurationTicks = 600;
constexpr int kWitnessLeaseMs = 600;
constexpr int kWitnessTimeoutMs = 80;

std::atomic<bool> runReplica{true};
int primarySocket = -1;
std::mutex primarySocketMtx;

std::vector<int> clients;
std::mutex clientMtx;

std::mt19937 rng{std::random_device{}()};
std::uniform_int_distribution<int> distX(1, WIDTH - 2);
std::uniform_int_distribution<int> distY(1, HEIGHT - 2);

struct MirroredPlayer
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

struct WitnessReply
{
    bool reachable = false;
    bool granted = false;
    std::uint64_t epoch = 0;
    std::string owner;
    int ttlMs = 0;
};

bool setSocketBlocking(int socket, bool blocking)
{
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0)
        return false;

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    return fcntl(socket, F_SETFL, flags) == 0;
}

int connectToHostWithTimeout(const std::string &host, int port, int timeoutMs)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *results = nullptr;
    std::string portText = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), portText.c_str(), &hints, &results);
    if (rc != 0)
        return -1;

    int sock = -1;
    for (addrinfo *candidate = results; candidate != nullptr; candidate = candidate->ai_next)
    {
        sock = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (sock < 0)
            continue;

        if (!setSocketBlocking(sock, false))
        {
            close(sock);
            sock = -1;
            continue;
        }

        int connectRc = connect(sock, candidate->ai_addr, candidate->ai_addrlen);
        if (connectRc == 0)
        {
            setSocketBlocking(sock, true);
            break;
        }

        if (errno != EINPROGRESS)
        {
            close(sock);
            sock = -1;
            continue;
        }

        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);

        timeval timeout{};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;

        int selectRc = select(sock + 1, nullptr, &writeSet, nullptr, &timeout);
        if (selectRc > 0)
        {
            int error = 0;
            socklen_t errorLen = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &errorLen) == 0 && error == 0)
            {
                setSocketBlocking(sock, true);
                break;
            }
        }

        close(sock);
        sock = -1;
    }

    freeaddrinfo(results);
    return sock;
}

bool requestWitnessLine(const std::string &host, int port, const std::string &request, std::string &replyLine)
{
    int sock = connectToHostWithTimeout(host, port, kWitnessTimeoutMs);
    if (sock < 0)
        return false;

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = kWitnessTimeoutMs * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string wire = request + "\n";
    if (send(sock, wire.c_str(), wire.size(), 0) <= 0)
    {
        close(sock);
        return false;
    }

    char buffer[256];
    std::string response;
    while (true)
    {
        int bytes = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes <= 0)
            break;

        response.append(buffer, bytes);
        size_t newline = response.find('\n');
        if (newline != std::string::npos)
        {
            replyLine = response.substr(0, newline);
            if (!replyLine.empty() && replyLine.back() == '\r')
                replyLine.pop_back();
            close(sock);
            return true;
        }
    }

    close(sock);
    return false;
}

WitnessReply parseWitnessReply(const std::string &line)
{
    WitnessReply reply;
    std::vector<std::string> parts = protocolSplit(line, '|');
    if (parts.size() < 4)
        return reply;

    if (parts[0] != "OK" && parts[0] != "DENY" && parts[0] != "PONG")
        return reply;

    reply.reachable = true;
    reply.granted = parts[0] == "OK";
    reply.epoch = static_cast<std::uint64_t>(std::stoull(parts[1]));
    reply.owner = protocolUnescape(parts[2]);
    reply.ttlMs = std::atoi(parts[3].c_str());
    return reply;
}

WitnessReply acquireWitnessLease(const std::string &host, int port,
                                 const std::string &ownerId, std::uint64_t requestedEpoch)
{
    std::string replyLine;
    std::ostringstream request;
    request << "ACQUIRE|" << protocolEscape(ownerId) << "|" << requestedEpoch << "|" << kWitnessLeaseMs;
    if (!requestWitnessLine(host, port, request.str(), replyLine))
        return WitnessReply{};
    return parseWitnessReply(replyLine);
}

WitnessReply renewWitnessLease(const std::string &host, int port,
                               const std::string &ownerId, std::uint64_t epoch)
{
    std::string replyLine;
    std::ostringstream request;
    request << "RENEW|" << protocolEscape(ownerId) << "|" << epoch << "|" << kWitnessLeaseMs;
    if (!requestWitnessLine(host, port, request.str(), replyLine))
        return WitnessReply{};
    return parseWitnessReply(replyLine);
}

WitnessReply pingWitness(const std::string &host, int port)
{
    std::string replyLine;
    if (!requestWitnessLine(host, port, "PING", replyLine))
        return WitnessReply{};
    return parseWitnessReply(replyLine);
}

int secondsRemainingFromTicks(int ticksRemaining)
{
    if (ticksRemaining <= 0)
        return 0;

    int msRemaining = ticksRemaining * kServerTickMs;
    return (msRemaining + 999) / 1000;
}

class ReplicaNode
{
private:
    std::mutex mtx;
    std::unordered_map<std::string, MirroredPlayer> playersByClientId;
    std::unordered_set<std::string> spectatorsByName;
    std::unordered_map<int, std::string> connectionNames;
    std::unordered_map<int, std::string> connectionClientIds;
    std::vector<std::string> gridRows;
    std::unordered_map<std::uint64_t, std::unordered_map<std::string, ReplicatedInput>> pendingInputs;
    std::uint64_t epoch = 1;
    std::uint64_t currentTick = 0;
    std::uint64_t publishedTick = 0;
    std::uint64_t safeTick = 0;
    std::string phase = "WAITING";
    int countdownTicksRemaining = 0;
    int countdownTicksElapsed = 0;
    int matchTicksRemaining = 0;
    char winnerSymbol = '\0';
    std::string winnerName;
    bool authoritative = false;
    bool checkpointLoaded = false;
    bool replayHealthy = false;
    bool witnessAvailable = true;
    bool checkpointRequested = true;
    int nextPlayerId = 1;
    std::string lastStateHash;
    std::chrono::steady_clock::time_point lastPrimaryHeartbeat = std::chrono::steady_clock::now();

    void resetGridLocked()
    {
        gridRows.assign(HEIGHT, std::string(WIDTH, '.'));

        for (int x = 0; x < WIDTH; x++)
        {
            gridRows[0][x] = '#';
            gridRows[HEIGHT - 1][x] = '#';
        }

        for (int y = 0; y < HEIGHT; y++)
        {
            gridRows[y][0] = '#';
            gridRows[y][WIDTH - 1] = '#';
        }
    }

    int alivePlayersLocked() const
    {
        int count = 0;
        for (const auto &entry : playersByClientId)
        {
            if (entry.second.alive)
                count++;
        }
        return count;
    }

    bool spawnTakenLocked(int x, int y) const
    {
        for (const auto &entry : playersByClientId)
        {
            const MirroredPlayer &player = entry.second;
            if (player.spawnX == x && player.spawnY == y)
                return true;
        }
        return false;
    }

    bool assignSpawnLocked(MirroredPlayer &player)
    {
        for (int tries = 0; tries < 500; tries++)
        {
            int x = distX(rng);
            int y = distY(rng);
            if (spawnTakenLocked(x, y))
                continue;

            player.spawnX = x;
            player.spawnY = y;
            player.x = x;
            player.y = y;
            return true;
        }

        return false;
    }

    void redrawWaitingSpawnsLocked()
    {
        resetGridLocked();
        for (const auto &entry : playersByClientId)
        {
            const MirroredPlayer &player = entry.second;
            if (player.spawnY >= 0 && player.spawnY < HEIGHT &&
                player.spawnX >= 0 && player.spawnX < WIDTH)
            {
                gridRows[player.spawnY][player.spawnX] = player.symbol;
            }
        }
    }

    void startCountdownLocked(std::vector<std::string> *events = nullptr)
    {
        phase = "COUNTDOWN";
        winnerSymbol = '\0';
        winnerName.clear();
        countdownTicksRemaining = kCountdownTicks;
        countdownTicksElapsed = 0;
        checkpointRequested = true;
        if (events != nullptr)
            events->push_back("COUNTDOWN_STARTED");
    }

    void startMatchLocked(std::vector<std::string> *events = nullptr)
    {
        resetGridLocked();

        for (auto &entry : playersByClientId)
        {
            MirroredPlayer &player = entry.second;
            player.alive = true;
            player.dir = "RIGHT";
            player.x = player.spawnX;
            player.y = player.spawnY;
            gridRows[player.y][player.x] = player.symbol;
        }

        phase = "IN_PROGRESS";
        matchTicksRemaining = kMatchDurationTicks;
        winnerSymbol = '\0';
        winnerName.clear();
        checkpointRequested = true;
        if (events != nullptr)
            events->push_back("MATCH_STARTED");
    }

    void finishMatchLocked(std::vector<std::string> *events = nullptr)
    {
        phase = "FINISHED";
        winnerSymbol = '\0';
        winnerName.clear();

        int aliveCount = 0;
        char lastAliveSymbol = '\0';
        std::string lastAliveName;
        for (const auto &entry : playersByClientId)
        {
            const MirroredPlayer &player = entry.second;
            if (!player.alive)
                continue;

            aliveCount++;
            lastAliveSymbol = player.symbol;
            lastAliveName = player.name;
        }

        if (aliveCount == 1)
        {
            winnerSymbol = lastAliveSymbol;
            winnerName = lastAliveName;
        }

        checkpointRequested = true;
        if (events != nullptr)
            events->push_back("MATCH_FINISHED");
    }

    ReplicationCheckpoint buildCheckpointLocked() const
    {
        ReplicationCheckpoint checkpoint;
        checkpoint.epoch = epoch;
        checkpoint.tick = currentTick;
        checkpoint.publishedTick = publishedTick;
        checkpoint.safeTick = safeTick;
        checkpoint.phase = phase;
        checkpoint.countdownTicksRemaining = countdownTicksRemaining;
        checkpoint.matchTicksRemaining = matchTicksRemaining;
        checkpoint.winnerName = winnerName;
        checkpoint.winnerSymbol = winnerSymbol;
        checkpoint.gridRows = gridRows;

        for (const auto &entry : playersByClientId)
        {
            const MirroredPlayer &player = entry.second;
            ReplicatedPlayerState replicated;
            replicated.clientId = player.clientId;
            replicated.name = player.name;
            replicated.symbol = player.symbol;
            replicated.x = player.x;
            replicated.y = player.y;
            replicated.spawnX = player.spawnX;
            replicated.spawnY = player.spawnY;
            replicated.dir = player.dir;
            replicated.alive = player.alive;
            checkpoint.players.push_back(replicated);
        }

        for (const std::string &spectator : spectatorsByName)
            checkpoint.spectators.push_back(spectator);

        checkpoint.stateHash = computeStateHash(checkpoint);
        return checkpoint;
    }

    std::vector<ReplicatedInput> collectAcceptedInputsLocked(std::uint64_t targetTick)
    {
        std::vector<ReplicatedInput> accepted;

        for (auto it = pendingInputs.begin(); it != pendingInputs.end();)
        {
            if (it->first < targetTick)
                it = pendingInputs.erase(it);
            else
                ++it;
        }

        auto tickIt = pendingInputs.find(targetTick);
        if (tickIt == pendingInputs.end())
            return accepted;

        for (const auto &entry : tickIt->second)
            accepted.push_back(entry.second);

        std::sort(accepted.begin(), accepted.end(), [](const ReplicatedInput &a, const ReplicatedInput &b)
                  {
                      if (a.clientId != b.clientId)
                          return a.clientId < b.clientId;
                      return a.clientSeq < b.clientSeq;
                  });
        pendingInputs.erase(tickIt);
        return accepted;
    }

    void applyAcceptedInputsLocked(const std::vector<ReplicatedInput> &inputs, std::vector<std::string> &events)
    {
        for (const ReplicatedInput &input : inputs)
        {
            if (input.command == "START")
            {
                bool connected = false;
                for (const auto &entry : connectionClientIds)
                {
                    if (entry.second == input.clientId)
                    {
                        connected = true;
                        break;
                    }
                }

                if (!connected || playersByClientId.size() < kMinimumPlayersToStart)
                    continue;

                if (phase == "WAITING" || phase == "FINISHED")
                {
                    redrawWaitingSpawnsLocked();
                    startCountdownLocked(&events);
                }
                continue;
            }

            auto it = playersByClientId.find(input.clientId);
            if (it == playersByClientId.end())
                continue;

            if (phase != "IN_PROGRESS")
                continue;

            MirroredPlayer &player = it->second;
            if (!player.alive)
                continue;

            if ((input.command == "UP" && player.dir == "DOWN") ||
                (input.command == "DOWN" && player.dir == "UP") ||
                (input.command == "LEFT" && player.dir == "RIGHT") ||
                (input.command == "RIGHT" && player.dir == "LEFT"))
                continue;

            if (input.command == "UP" || input.command == "DOWN" ||
                input.command == "LEFT" || input.command == "RIGHT")
            {
                player.dir = input.command;
            }
        }
    }

    void advanceOneTickLocked(const std::vector<ReplicatedInput> &acceptedInputs,
                              std::uint64_t targetTick,
                              std::vector<std::string> &events)
    {
        currentTick = targetTick;
        applyAcceptedInputsLocked(acceptedInputs, events);

        if (phase == "WAITING")
        {
            publishedTick = currentTick;
            if (authoritative)
                safeTick = currentTick;
            return;
        }

        if (phase == "COUNTDOWN")
        {
            if (playersByClientId.size() < kMinimumPlayersToStart)
            {
                phase = "WAITING";
                winnerSymbol = '\0';
                winnerName.clear();
                redrawWaitingSpawnsLocked();
                checkpointRequested = true;
                events.push_back("COUNTDOWN_CANCELLED");
            }
            else
            {
                countdownTicksElapsed++;
                if (countdownTicksRemaining > 0)
                    countdownTicksRemaining--;

                if (countdownTicksRemaining <= 0)
                    startMatchLocked(&events);
            }

            publishedTick = currentTick;
            if (authoritative)
                safeTick = currentTick;
            return;
        }

        if (phase == "IN_PROGRESS")
        {
            for (auto &entry : playersByClientId)
            {
                MirroredPlayer &player = entry.second;
                if (!player.alive)
                    continue;
                gridRows[player.y][player.x] = '#';
            }

            for (auto &entry : playersByClientId)
            {
                MirroredPlayer &player = entry.second;
                if (!player.alive)
                    continue;

                int nextX = player.x;
                int nextY = player.y;

                if (player.dir == "UP")
                    nextY--;
                else if (player.dir == "DOWN")
                    nextY++;
                else if (player.dir == "LEFT")
                    nextX--;
                else if (player.dir == "RIGHT")
                    nextX++;

                if (nextX < 0 || nextX >= WIDTH || nextY < 0 || nextY >= HEIGHT ||
                    gridRows[nextY][nextX] != '.')
                {
                    player.alive = false;
                    continue;
                }

                player.x = nextX;
                player.y = nextY;
                gridRows[player.y][player.x] = player.symbol;
            }

            if (matchTicksRemaining > 0)
                matchTicksRemaining--;

            if (alivePlayersLocked() <= 1 || matchTicksRemaining <= 0)
                finishMatchLocked(&events);

            publishedTick = currentTick;
            if (authoritative)
                safeTick = currentTick;
            return;
        }

        publishedTick = currentTick;
        if (authoritative)
            safeTick = currentTick;
    }

    std::string statusTextLocked(int socket) const
    {
        std::string clientId;
        auto connectionIt = connectionClientIds.find(socket);
        if (connectionIt != connectionClientIds.end())
            clientId = connectionIt->second;

        auto playerIt = playersByClientId.find(clientId);
        bool isPlayer = playerIt != playersByClientId.end();
        bool alive = isPlayer && playerIt->second.alive;
        char playerSymbol = isPlayer ? playerIt->second.symbol : '\0';

        if (phase == "WAITING")
        {
            if (authoritative)
            {
                if (isPlayer)
                    return "LOBBY | Press R to start. Need at least " + std::to_string(kMinimumPlayersToStart) + " player(s).";
                return "SPECTATING | Press R to start. Need at least " + std::to_string(kMinimumPlayersToStart) + " player(s).";
            }

            if (isPlayer)
                return "LOBBY | Standby replica synced with primary.";
            return "SPECTATING | Standby replica synced with primary.";
        }

        if (phase == "COUNTDOWN")
        {
            int remainingSeconds = secondsRemainingFromTicks(countdownTicksRemaining);
            if (isPlayer)
                return "COUNTDOWN | Match starts in " + std::to_string(remainingSeconds) + "s. Your spawn is flashing.";
            return "SPECTATING | Countdown: " + std::to_string(remainingSeconds) + "s.";
        }

        if (phase == "IN_PROGRESS")
        {
            int remainingSeconds = secondsRemainingFromTicks(matchTicksRemaining);
            if (isPlayer && alive)
            {
                if (authoritative)
                    return "ALIVE | Time remaining: " + std::to_string(remainingSeconds) + "s.";
                return "ALIVE | Standby replica view. Time remaining: " + std::to_string(remainingSeconds) + "s.";
            }

            if (isPlayer && !alive)
            {
                if (authoritative)
                    return "DEAD | Now spectating. Time remaining: " + std::to_string(remainingSeconds) + "s.";
                return "DEAD | Standby replica view. Time remaining: " + std::to_string(remainingSeconds) + "s.";
            }

            if (authoritative)
                return "SPECTATING | Time remaining: " + std::to_string(remainingSeconds) + "s.";
            return "SPECTATING | Standby replica view. Time remaining: " + std::to_string(remainingSeconds) + "s.";
        }

        if (winnerName.empty())
            return "MATCH_OVER | Draw.";

        if (isPlayer && winnerSymbol != '\0' && playerSymbol == winnerSymbol)
            return "MATCH_OVER | WINNER: " + winnerName + "!";

        if (winnerSymbol == '\0')
            return "MATCH_OVER | Winner: " + winnerName;

        return "MATCH_OVER | Winner: " + winnerName + " (" + std::string(1, winnerSymbol) + ")";
    }

public:
    ReplicaNode()
        : gridRows(HEIGHT, std::string(WIDTH, '.'))
    {
        resetGridLocked();
    }

    bool isAuthoritative()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return authoritative;
    }

    std::uint64_t getEpoch()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return epoch;
    }

    std::uint64_t getPublishedTick()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return publishedTick;
    }

    std::uint64_t nextPromotionEpoch()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return epoch + 1;
    }

    std::uint64_t getSafeTick()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return safeTick;
    }

    std::string getLastStateHash()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return lastStateHash;
    }

    bool witnessIsAvailable()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return witnessAvailable;
    }

    void setWitnessAvailable(bool available)
    {
        std::lock_guard<std::mutex> lock(mtx);
        witnessAvailable = available;
    }

    long long millisecondsSincePrimaryHeartbeat()
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto delta = std::chrono::steady_clock::now() - lastPrimaryHeartbeat;
        return std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    }

    bool canPromote()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return !authoritative && checkpointLoaded && replayHealthy && witnessAvailable;
    }

    void promoteToLeader(std::uint64_t newEpoch)
    {
        std::lock_guard<std::mutex> lock(mtx);
        authoritative = true;
        epoch = newEpoch;
        currentTick = safeTick;
        publishedTick = safeTick;
        checkpointRequested = true;
        replayHealthy = true;
        std::cout << "Replica promoted to leader for epoch " << epoch
                  << " from safe_tick " << safeTick << ".\n";
    }

    void registerConnection(int socket, std::string &note)
    {
        std::lock_guard<std::mutex> lock(mtx);
        connectionNames[socket] = "";
        connectionClientIds[socket] = "socket-" + std::to_string(socket);
        note = authoritative ? "connected to promoted replica leader" : "connected to standby replica";
    }

    void removeConnection(int socket)
    {
        std::lock_guard<std::mutex> lock(mtx);

        std::string clientId;
        auto clientIt = connectionClientIds.find(socket);
        if (clientIt != connectionClientIds.end())
        {
            clientId = clientIt->second;
            connectionClientIds.erase(clientIt);
        }

        connectionNames.erase(socket);

        if (authoritative && !clientId.empty())
        {
            auto playerIt = playersByClientId.find(clientId);
            if (playerIt != playersByClientId.end())
            {
                MirroredPlayer player = playerIt->second;
                if (phase == "IN_PROGRESS" && player.alive &&
                    player.y >= 0 && player.y < HEIGHT &&
                    player.x >= 0 && player.x < WIDTH)
                {
                    gridRows[player.y][player.x] = '#';
                }
                else if (player.y >= 0 && player.y < HEIGHT &&
                         player.x >= 0 && player.x < WIDTH)
                {
                    gridRows[player.y][player.x] = '.';
                }

                playersByClientId.erase(playerIt);
                checkpointRequested = true;
            }

            if (phase == "COUNTDOWN" && playersByClientId.size() < kMinimumPlayersToStart)
            {
                phase = "WAITING";
                winnerName.clear();
                winnerSymbol = '\0';
                redrawWaitingSpawnsLocked();
            }

            if (phase == "IN_PROGRESS" && alivePlayersLocked() <= 1)
            {
                std::vector<std::string> events;
                finishMatchLocked(&events);
            }

            if (phase == "FINISHED" && playersByClientId.empty())
            {
                phase = "WAITING";
                winnerName.clear();
                winnerSymbol = '\0';
                resetGridLocked();
            }
        }

        close(socket);
    }

    void setViewerIdentity(int socket, const std::string &clientId, const std::string &requestedName)
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::string sanitizedId = sanitizePlayerName(clientId);
        std::string sanitizedName = sanitizePlayerName(requestedName);
        if (sanitizedId.empty())
            return;
        if (sanitizedName.empty())
            sanitizedName = "Player";

        connectionClientIds[socket] = sanitizedId;
        connectionNames[socket] = sanitizedName;

        if (!authoritative)
            return;

        auto playerIt = playersByClientId.find(sanitizedId);
        if (playerIt != playersByClientId.end())
        {
            playerIt->second.name = sanitizedName;
            checkpointRequested = true;
            return;
        }

        if (phase != "WAITING")
        {
            spectatorsByName.insert(sanitizedName);
            return;
        }

        MirroredPlayer player;
        player.clientId = sanitizedId;
        player.name = sanitizedName;
        player.symbol = static_cast<char>('A' + ((nextPlayerId - 1) % 26));
        nextPlayerId++;

        if (!assignSpawnLocked(player))
        {
            spectatorsByName.insert(sanitizedName);
            return;
        }

        playersByClientId[sanitizedId] = player;
        gridRows[player.spawnY][player.spawnX] = player.symbol;
        checkpointRequested = true;
    }

    bool queueMirroredInput(int socket, const ReplicatedInput &input)
    {
        std::lock_guard<std::mutex> lock(mtx);

        auto connectionIt = connectionClientIds.find(socket);
        if (connectionIt == connectionClientIds.end())
            return false;

        if (connectionIt->second != input.clientId)
            return false;

        if (input.targetTick <= currentTick)
            return false;

        if (input.epochHint > epoch && !authoritative)
            return false;

        auto &slot = pendingInputs[input.targetTick][input.clientId];
        if (slot.clientSeq > input.clientSeq)
            return false;

        slot = input;
        return true;
    }

    void applyHeartbeat(const HeartbeatMessage &heartbeat)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (authoritative)
            return;

        if (heartbeat.epoch > epoch)
            epoch = heartbeat.epoch;
        if (heartbeat.publishedTick > publishedTick)
            publishedTick = heartbeat.publishedTick;
        lastPrimaryHeartbeat = std::chrono::steady_clock::now();
    }

    bool applyCheckpoint(const ReplicationCheckpoint &checkpoint, SafeTickReport &report)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (authoritative)
            return false;

        epoch = checkpoint.epoch;
        currentTick = checkpoint.tick;
        publishedTick = checkpoint.publishedTick;
        safeTick = checkpoint.tick;
        phase = checkpoint.phase;
        countdownTicksRemaining = checkpoint.countdownTicksRemaining;
        countdownTicksElapsed = 0;
        matchTicksRemaining = checkpoint.matchTicksRemaining;
        winnerSymbol = checkpoint.winnerSymbol;
        winnerName = checkpoint.winnerName;
        lastPrimaryHeartbeat = std::chrono::steady_clock::now();
        witnessAvailable = true;

        playersByClientId.clear();
        for (const ReplicatedPlayerState &player : checkpoint.players)
        {
            MirroredPlayer mirrored;
            mirrored.clientId = player.clientId;
            mirrored.name = player.name;
            mirrored.symbol = player.symbol;
            mirrored.x = player.x;
            mirrored.y = player.y;
            mirrored.spawnX = player.spawnX;
            mirrored.spawnY = player.spawnY;
            mirrored.dir = player.dir;
            mirrored.alive = player.alive;
            playersByClientId[mirrored.clientId] = mirrored;
        }

        nextPlayerId = static_cast<int>(playersByClientId.size()) + 1;

        spectatorsByName.clear();
        for (const std::string &spectator : checkpoint.spectators)
            spectatorsByName.insert(spectator);

        if (checkpoint.gridRows.size() == static_cast<size_t>(HEIGHT))
            gridRows = checkpoint.gridRows;
        else
            resetGridLocked();

        lastStateHash = checkpoint.stateHash.empty() ? computeStateHash(checkpoint) : checkpoint.stateHash;
        checkpointLoaded = true;
        replayHealthy = true;

        report.epoch = epoch;
        report.safeTick = safeTick;
        report.stateHash = lastStateHash;
        return true;
    }

    bool applyTickSeal(const TickSeal &seal, SafeTickReport &report)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (authoritative || !checkpointLoaded || !replayHealthy)
            return false;

        if (seal.epoch < epoch || seal.tick <= safeTick)
            return false;

        if (seal.tick != currentTick + 1)
        {
            replayHealthy = false;
            return false;
        }

        std::vector<std::string> events;
        advanceOneTickLocked(seal.acceptedInputs, seal.tick, events);

        ReplicationCheckpoint checkpoint = buildCheckpointLocked();
        std::string localHash = checkpoint.stateHash;
        if (localHash != seal.stateHash)
        {
            replayHealthy = false;
            return false;
        }

        epoch = seal.epoch;
        safeTick = seal.tick;
        lastStateHash = localHash;
        report.epoch = epoch;
        report.safeTick = safeTick;
        report.stateHash = lastStateHash;
        return true;
    }

    void leaderTick()
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!authoritative)
            return;

        std::uint64_t nextTick = currentTick + 1;
        std::vector<ReplicatedInput> acceptedInputs = collectAcceptedInputsLocked(nextTick);
        std::vector<std::string> events;
        advanceOneTickLocked(acceptedInputs, nextTick, events);
        ReplicationCheckpoint checkpoint = buildCheckpointLocked();
        lastStateHash = checkpoint.stateHash;
    }

    std::string buildFrameForClient(int socket)
    {
        std::lock_guard<std::mutex> lock(mtx);

        std::string clientId;
        auto clientIt = connectionClientIds.find(socket);
        if (clientIt != connectionClientIds.end())
            clientId = clientIt->second;

        auto playerIt = playersByClientId.find(clientId);
        bool isPlayer = playerIt != playersByClientId.end();
        bool alive = isPlayer && playerIt->second.alive;
        char playerSymbol = isPlayer ? playerIt->second.symbol : '\0';
        std::string playerName = "NONE";
        if (isPlayer)
            playerName = playerIt->second.name;
        else
        {
            auto nameIt = connectionNames.find(socket);
            if (nameIt != connectionNames.end() && !nameIt->second.empty())
                playerName = nameIt->second;
        }

        int remaining = 0;
        if (phase == "COUNTDOWN")
            remaining = secondsRemainingFromTicks(countdownTicksRemaining);
        else if (phase == "IN_PROGRESS")
            remaining = secondsRemainingFromTicks(matchTicksRemaining);

        std::vector<std::string> renderedRows = gridRows;
        if (phase == "COUNTDOWN" && isPlayer)
        {
            const MirroredPlayer &player = playerIt->second;
            bool visible = ((countdownTicksElapsed / 2) % 2) == 0;
            if (!visible &&
                player.spawnY >= 0 && player.spawnY < static_cast<int>(renderedRows.size()) &&
                player.spawnX >= 0 && player.spawnX < static_cast<int>(renderedRows[player.spawnY].size()))
            {
                renderedRows[player.spawnY][player.spawnX] = '.';
            }
        }

        std::ostringstream out;
        out << "META\n";
        out << "epoch=" << epoch << "\n";
        out << "tick=" << currentTick << "\n";
        out << "published_tick=" << publishedTick << "\n";
        out << "safe_tick=" << safeTick << "\n";
        out << "authoritative=" << (authoritative ? 1 : 0) << "\n";
        out << "phase=" << phase << "\n";
        out << "role=" << (isPlayer ? "PLAYER" : "SPECTATOR") << "\n";
        out << "alive=" << (alive ? 1 : 0) << "\n";
        out << "winner=" << (winnerName.empty() ? "NONE" : winnerName) << "\n";
        out << "winner_symbol=" << (winnerSymbol == '\0' ? "NONE" : std::string(1, winnerSymbol)) << "\n";
        out << "time_remaining=" << remaining << "\n";
        out << "player_symbol=" << (isPlayer ? std::string(1, playerSymbol) : "NONE") << "\n";
        out << "player_name=" << playerName << "\n";
        out << "status=" << statusTextLocked(socket) << "\n";
        out << "GRID\n";
        for (const std::string &row : renderedRows)
            out << row << "\n";
        out << "END\n";
        return out.str();
    }
};

void sendToPrimary(const std::string &message)
{
    std::lock_guard<std::mutex> lock(primarySocketMtx);
    if (primarySocket < 0)
        return;

    if (send(primarySocket, message.c_str(), message.size(), 0) <= 0)
    {
        close(primarySocket);
        primarySocket = -1;
    }
}

void connectToPrimary(const std::string &primaryHost, int primaryPort)
{
    std::cout << "Connecting to primary at " << primaryHost << ":" << primaryPort << "...\n";
    primarySocket = connectToHostWithTimeout(primaryHost, primaryPort, 1500);
    if (primarySocket < 0)
    {
        std::cerr << "Unable to connect to primary " << primaryHost << ":" << primaryPort << "\n";
        std::exit(1);
    }

    const char *hello = "REPLICA\n";
    if (send(primarySocket, hello, std::strlen(hello), 0) <= 0)
    {
        std::perror("send");
        close(primarySocket);
        primarySocket = -1;
        std::exit(1);
    }

    std::cout << "Successfully connected to primary\n";
}

void removeClientSocket(int socket)
{
    std::lock_guard<std::mutex> lock(clientMtx);
    auto it = std::remove(clients.begin(), clients.end(), socket);
    clients.erase(it, clients.end());
}

void sendFramesToClients(ReplicaNode &node)
{
    std::vector<int> sockets;
    {
        std::lock_guard<std::mutex> lock(clientMtx);
        sockets = clients;
    }

    for (int socket : sockets)
    {
        std::string frame = node.buildFrameForClient(socket);
        if (send(socket, frame.c_str(), frame.size(), 0) <= 0)
        {
            node.removeConnection(socket);
            removeClientSocket(socket);
        }
    }
}

void clientHandler(ReplicaNode &node, int socket)
{
    std::string pending;
    char buffer[1024];

    while (runReplica.load())
    {
        int bytes = recv(socket, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (bytes > 0)
        {
            pending.append(buffer, bytes);

            size_t newline = std::string::npos;
            while ((newline = pending.find('\n')) != std::string::npos)
            {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                if (line.empty() || line == "CLIENT")
                    continue;

                std::string clientId;
                std::string playerName;
                if (parseHelloLine(line, clientId, playerName))
                {
                    node.setViewerIdentity(socket, clientId, playerName);
                    sendFramesToClients(node);
                    continue;
                }

                ReplicatedInput input;
                if (parseInputLine(line, input))
                {
                    node.queueMirroredInput(socket, input);
                    continue;
                }
            }
        }
        else if (bytes == 0)
        {
            break;
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    node.removeConnection(socket);
    removeClientSocket(socket);
}

bool consumePrimaryMessage(std::string &buffer, std::string &messageType, std::string &rawMessage)
{
    while (!buffer.empty())
    {
        if (buffer.rfind("REPLICA\n", 0) == 0)
        {
            buffer.erase(0, 8);
            continue;
        }

        if (buffer.rfind("\n", 0) == 0 || buffer.rfind("\r", 0) == 0)
        {
            buffer.erase(0, 1);
            continue;
        }

        struct Candidate
        {
            const char *prefix;
            const char *terminator;
            const char *type;
        };

        static const Candidate candidates[] = {
            {"HEARTBEAT\n", "END_HEARTBEAT\n", "HEARTBEAT"},
            {"TICK_SEAL\n", "END_TICK_SEAL\n", "TICK_SEAL"},
            {"CHECKPOINT\n", "END_CHECKPOINT\n", "CHECKPOINT"}};

        for (const Candidate &candidate : candidates)
        {
            if (buffer.rfind(candidate.prefix, 0) != 0)
                continue;

            size_t endPos = buffer.find(candidate.terminator);
            if (endPos == std::string::npos)
                return false;

            rawMessage = buffer.substr(0, endPos + std::strlen(candidate.terminator));
            buffer.erase(0, endPos + std::strlen(candidate.terminator));
            messageType = candidate.type;
            return true;
        }

        size_t newline = buffer.find('\n');
        if (newline == std::string::npos)
            return false;
        buffer.erase(0, newline + 1);
    }

    return false;
}

void primarySyncLoop(ReplicaNode &node)
{
    std::string buffer;
    char recvBuffer[4096];

    while (runReplica.load() && !node.isAuthoritative())
    {
        int bytes = recv(primarySocket, recvBuffer, sizeof(recvBuffer), 0);
        if (bytes > 0)
        {
            buffer.append(recvBuffer, bytes);

            std::string type;
            std::string raw;
            while (consumePrimaryMessage(buffer, type, raw))
            {
                if (type == "HEARTBEAT")
                {
                    HeartbeatMessage heartbeat;
                    if (parseHeartbeat(raw, heartbeat))
                        node.applyHeartbeat(heartbeat);
                    continue;
                }

                if (type == "CHECKPOINT")
                {
                    ReplicationCheckpoint checkpoint;
                    if (parseCheckpoint(raw, checkpoint))
                    {
                        SafeTickReport report;
                        if (node.applyCheckpoint(checkpoint, report))
                            sendToPrimary(serializeSafeTickReport(report));
                        sendFramesToClients(node);
                    }
                    continue;
                }

                if (type == "TICK_SEAL")
                {
                    TickSeal seal;
                    if (parseTickSeal(raw, seal))
                    {
                        SafeTickReport report;
                        if (node.applyTickSeal(seal, report))
                            sendToPrimary(serializeSafeTickReport(report));
                        sendFramesToClients(node);
                    }
                    continue;
                }
            }

            continue;
        }

        if (bytes == 0)
        {
            std::cout << "Primary disconnected.\n";
            break;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            std::perror("recv from primary");
            break;
        }
    }

    std::lock_guard<std::mutex> lock(primarySocketMtx);
    if (primarySocket >= 0)
    {
        close(primarySocket);
        primarySocket = -1;
    }
}

void witnessMonitorLoop(ReplicaNode &node,
                        const std::string &witnessHost,
                        int witnessPort,
                        const std::string &ownerId)
{
    int witnessFailures = 0;

    while (runReplica.load())
    {
        if (node.isAuthoritative())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        WitnessReply pingReply = pingWitness(witnessHost, witnessPort);
        if (!pingReply.reachable)
        {
            witnessFailures++;
            if (witnessFailures >= 3)
                node.setWitnessAvailable(false);
        }
        else
        {
            witnessFailures = 0;
            node.setWitnessAvailable(true);
        }

        if (node.canPromote() && node.millisecondsSincePrimaryHeartbeat() > (3LL * kServerTickMs))
        {
            std::uint64_t requestedEpoch = node.nextPromotionEpoch();
            WitnessReply acquireReply = acquireWitnessLease(witnessHost, witnessPort, ownerId, requestedEpoch);
            if (acquireReply.reachable && acquireReply.granted && acquireReply.epoch >= requestedEpoch)
            {
                node.promoteToLeader(acquireReply.epoch);
                {
                    std::lock_guard<std::mutex> lock(primarySocketMtx);
                    if (primarySocket >= 0)
                    {
                        close(primarySocket);
                        primarySocket = -1;
                    }
                }
                sendFramesToClients(node);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kServerTickMs));
    }
}

void leaderLoop(ReplicaNode &node,
                const std::string &witnessHost,
                int witnessPort,
                const std::string &ownerId,
                std::shared_ptr<TickLatenessLogger> tickLogger)
{
    int missedRenewals = 0;
    bool tickScheduleStarted = false;
    auto nextTickDueAt = TickLatenessLogger::Clock::time_point{};

    while (runReplica.load())
    {
        if (!node.isAuthoritative())
        {
            tickScheduleStarted = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (!tickScheduleStarted)
        {
            nextTickDueAt = TickLatenessLogger::Clock::now();
            if (tickLogger)
                tickLogger->startSchedule(node.getPublishedTick() + 1, nextTickDueAt);
            tickScheduleStarted = true;
        }

        auto tickStart = TickLatenessLogger::Clock::now();

        WitnessReply renewReply = renewWitnessLease(witnessHost, witnessPort, ownerId, node.getEpoch());
        if (!renewReply.reachable)
        {
            missedRenewals++;
            if (missedRenewals >= 3)
                node.setWitnessAvailable(false);
        }
        else
        {
            missedRenewals = 0;
            node.setWitnessAvailable(true);
            if (!renewReply.granted && renewReply.epoch > node.getEpoch())
            {
                std::cerr << "Witness reports a newer leader epoch " << renewReply.epoch
                          << ". Stopping promoted replica.\n";
                runReplica = false;
                break;
            }
        }

        node.leaderTick();
        sendFramesToClients(node);
        auto publishAt = TickLatenessLogger::Clock::now();

        if (tickLogger)
        {
            tickLogger->logPublishedTick(node.getEpoch(),
                                         node.getPublishedTick(),
                                         tickStart,
                                         publishAt);
        }

        nextTickDueAt += std::chrono::milliseconds(kServerTickMs);
        if (TickLatenessLogger::Clock::now() < nextTickDueAt)
            std::this_thread::sleep_until(nextTickDueAt);
    }
}

struct ServerStartupOptions
{
    std::string primary_host;
    int primary_port = 0;
    std::string this_host;
    int this_port = 0;
    std::string witness_host;
    int witness_port = 0;
    std::string tick_log_path = "logs/tick_lateness_replica.csv";
    std::size_t tick_log_limit = 10000;
};

void printServerUsage(const char *programName)
{
    std::cout << "Usage: " << programName
              << " [--p_host <sunlab-host>] [--p_port 6000-6010]"
              << " [--this_host <sunlab-host>] [--this_port 6000-6010]"
              << " [--w_host <sunlab-host>] [--w_port 6000-6010]"
              << " [--tick_log <path|off>] [--tick_log_limit <count>]\n";
}

std::string localHostnameNormalized()
{
    char hostBuffer[256]{0};
    if (gethostname(hostBuffer, sizeof(hostBuffer) - 1) != 0)
        return "";
    hostBuffer[sizeof(hostBuffer) - 1] = '\0';
    return normalizeSunlabHost(hostBuffer);
}

bool parseServerArgs(int argc, char *argv[], ServerStartupOptions &options)
{
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        auto parseHostArg = [&](std::string &out) -> bool
        {
            if (i + 1 >= argc)
                return false;
            std::string parsedHost = argv[++i];
            if (!isValidConfiguredHost(parsedHost))
                return false;
            out = normalizeConfiguredHost(parsedHost);
            return true;
        };

        auto parsePortArg = [&](int &out) -> bool
        {
            if (i + 1 >= argc)
                return false;
            return parsePortInRange(argv[++i], out);
        };

        auto parseTickLogLimitArg = [&](std::size_t &out) -> bool
        {
            if (i + 1 >= argc)
                return false;

            std::string value = trim(argv[++i]);
            if (value.empty())
                return false;

            try
            {
                size_t consumed = 0;
                unsigned long long parsed = std::stoull(value, &consumed, 10);
                if (consumed != value.size())
                    return false;
                out = static_cast<std::size_t>(parsed);
                return true;
            }
            catch (const std::exception &)
            {
                return false;
            }
        };

        if (arg == "--p_host")
        {
            if (!parseHostArg(options.primary_host))
            {
                std::cerr << "Invalid --p_host value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--p_port")
        {
            if (!parsePortArg(options.primary_port))
            {
                std::cerr << "Invalid --p_port value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--this_host")
        {
            if (!parseHostArg(options.this_host))
            {
                std::cerr << "Invalid --this_host value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--this_port")
        {
            if (!parsePortArg(options.this_port))
            {
                std::cerr << "Invalid --this_port value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--w_host")
        {
            if (!parseHostArg(options.witness_host))
            {
                std::cerr << "Invalid --w_host value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--w_port")
        {
            if (!parsePortArg(options.witness_port))
            {
                std::cerr << "Invalid --w_port value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--tick_log")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Invalid --tick_log value.\n";
                return false;
            }
            options.tick_log_path = argv[++i];
            continue;
        }

        if (arg == "--tick_log_limit")
        {
            if (!parseTickLogLimitArg(options.tick_log_limit))
            {
                std::cerr << "Invalid --tick_log_limit value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--help" || arg == "-h")
        {
            printServerUsage(argv[0]);
            std::exit(0);
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
    ServerStartupOptions options;
    if (!parseServerArgs(argc, argv, options))
    {
        printServerUsage(argv[0]);
        return 1;
    }

    if (options.primary_host.empty())
    {
        std::cout << "Enter primary server config:\n";
        options.primary_host = promptForSunlabHost();
    }
    if (options.primary_port == 0)
        options.primary_port = promptForPort();
    if (options.this_host.empty())
    {
        std::cout << "Enter replica server config (this machine):\n";
        options.this_host = promptForSunlabHost();
    }
    if (options.this_port == 0)
        options.this_port = promptForPort();
    if (options.witness_host.empty())
    {
        std::cout << "Enter witness host:\n";
        options.witness_host = promptForSunlabHost();
    }
    if (options.witness_port == 0)
        options.witness_port = promptForPort();

    std::string localHost = localHostnameNormalized();
    if (!localHost.empty() && isValidSunlabHost(localHost) && localHost != options.this_host)
    {
        std::cerr << "Selected host " << toConnectHost(options.this_host)
                  << " does not match this machine (" << toSunlabFqdn(localHost)
                  << "). SSH into the selected node before starting the server.\n";
        return 1;
    }

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0)
    {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0)
    {
        std::perror("setsockopt");
        close(serverFd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.this_port);
    address.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    {
        std::perror("bind");
        close(serverFd);
        return 1;
    }

    if (listen(serverFd, 20) != 0)
    {
        std::perror("listen");
        close(serverFd);
        return 1;
    }

    ReplicaNode node;
    std::cout << "REPLICA running on " << toConnectHost(options.this_host) << " port " << options.this_port << "\n";
    std::cout << "Witness on " << toConnectHost(options.witness_host) << ":" << options.witness_port << "\n";
    auto tickLogger = std::make_shared<TickLatenessLogger>(options.tick_log_path,
                                                           "promoted_replica",
                                                           toConnectHost(options.this_host) + ":" + std::to_string(options.this_port),
                                                           kServerTickMs,
                                                           options.tick_log_limit);
    if (tickLogger->enabled())
    {
        std::cout << "Tick lateness log: " << tickLogger->path();
        if (tickLogger->maxSamples() == 0)
            std::cout << " (all authoritative ticks after promotion)\n";
        else
            std::cout << " (first " << tickLogger->maxSamples() << " authoritative ticks after promotion)\n";
    }
    else
    {
        std::cout << "Tick lateness log: disabled\n";
    }

    connectToPrimary(toConnectHost(options.primary_host), options.primary_port);
    std::string ownerId = toConnectHost(options.this_host) + ":" + std::to_string(options.this_port);
    std::thread(primarySyncLoop, std::ref(node)).detach();
    std::thread(witnessMonitorLoop,
                std::ref(node),
                toConnectHost(options.witness_host),
                options.witness_port,
                ownerId)
        .detach();
    std::thread(leaderLoop,
                std::ref(node),
                toConnectHost(options.witness_host),
                options.witness_port,
                ownerId,
                tickLogger)
        .detach();

    while (runReplica.load())
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverFd, &readSet);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        int rc = select(serverFd + 1, &readSet, nullptr, nullptr, &timeout);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        if (rc == 0 || !FD_ISSET(serverFd, &readSet))
            continue;

        int clientSocket = accept(serverFd, nullptr, nullptr);
        if (clientSocket < 0)
            continue;

        {
            std::lock_guard<std::mutex> lock(clientMtx);
            clients.push_back(clientSocket);
        }

        std::string note;
        node.registerConnection(clientSocket, note);
        std::cout << "Client connected: " << note << "\n";

        std::string frame = node.buildFrameForClient(clientSocket);
        if (send(clientSocket, frame.c_str(), frame.size(), 0) <= 0)
        {
            node.removeConnection(clientSocket);
            removeClientSocket(clientSocket);
            continue;
        }

        std::thread(clientHandler, std::ref(node), clientSocket).detach();
    }

    close(serverFd);
    return 0;
}
