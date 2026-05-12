#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
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

#define WIDTH 100
#define HEIGHT 50

constexpr int kServerTickMs = 150;
constexpr int kMinimumPlayersToStart = 1;
constexpr int kCountdownTicks = 34;
constexpr int kMatchDurationTicks = 600;
constexpr int kWitnessLeaseMs = 600;
constexpr int kWitnessTimeoutMs = 80;
constexpr int kCheckpointEveryTicks = 10;

std::atomic<bool> runPrimary{true};

std::mutex replicaMtx;
std::atomic<bool> replicaConnected{false};
int replicaSocket = -1;
std::atomic<std::uint64_t> replicaVerifiedSafeTick{0};

std::vector<int> clients;
std::mutex clientMtx;

std::mt19937 rng{std::random_device{}()};
std::uniform_int_distribution<int> distX(1, WIDTH - 2);
std::uniform_int_distribution<int> distY(1, HEIGHT - 2);

struct Player
{
    int id = 0;
    int x = 0;
    int y = 0;
    int spawnX = 0;
    int spawnY = 0;
    std::string dir = "RIGHT";
    int socket = -1;
    bool alive = true;
    char symbol = 'A';
    std::string name;
    std::string clientId;
};

enum class GamePhase
{
    Waiting,
    Countdown,
    InProgress,
    Finished
};

enum class IncomingConnectionType
{
    Client,
    Replica,
    Unknown
};

struct TickArtifacts
{
    TickSeal seal;
    ReplicationCheckpoint checkpoint;
    bool sendCheckpoint = false;
};

struct WitnessReply
{
    bool reachable = false;
    bool granted = false;
    std::uint64_t epoch = 0;
    std::string owner;
    int ttlMs = 0;
};

std::string phaseToString(GamePhase phase)
{
    if (phase == GamePhase::Waiting)
        return "WAITING";
    if (phase == GamePhase::Countdown)
        return "COUNTDOWN";
    if (phase == GamePhase::InProgress)
        return "IN_PROGRESS";
    return "FINISHED";
}

int secondsRemainingFromTicks(int ticksRemaining)
{
    if (ticksRemaining <= 0)
        return 0;

    int msRemaining = ticksRemaining * kServerTickMs;
    return (msRemaining + 999) / 1000;
}

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

class TronGame
{
private:
    std::vector<std::vector<char>> grid;
    std::unordered_map<int, Player> players;
    std::unordered_set<int> spectators;
    std::unordered_map<int, std::string> connectionNames;
    std::unordered_map<int, std::string> connectionClientIds;
    std::unordered_map<std::string, int> socketByClientId;
    std::unordered_map<std::uint64_t, std::unordered_map<std::string, ReplicatedInput>> pendingInputs;
    std::mutex mtx;
    int nextId = 1;
    GamePhase phase = GamePhase::Waiting;
    std::uint64_t epoch = 1;
    std::uint64_t currentTick = 0;
    std::uint64_t publishedTick = 0;
    std::uint64_t safeTick = 0;
    int countdownTicksRemaining = 0;
    int countdownTicksElapsed = 0;
    int matchTicksRemaining = 0;
    char winnerSymbol = '\0';
    std::string winnerName;
    bool checkpointRequested = true;

    void resetGridLocked()
    {
        grid.assign(HEIGHT, std::vector<char>(WIDTH, '.'));

        for (int x = 0; x < WIDTH; x++)
        {
            grid[0][x] = '#';
            grid[HEIGHT - 1][x] = '#';
        }

        for (int y = 0; y < HEIGHT; y++)
        {
            grid[y][0] = '#';
            grid[y][WIDTH - 1] = '#';
        }
    }

    bool spawnTakenLocked(int x, int y) const
    {
        for (const auto &entry : players)
        {
            const Player &p = entry.second;
            if (p.spawnX == x && p.spawnY == y)
                return true;
        }
        return false;
    }

    bool assignSpawnLocked(Player &player)
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

    int alivePlayersLocked() const
    {
        int count = 0;
        for (const auto &entry : players)
        {
            if (entry.second.alive)
                count++;
        }
        return count;
    }

    void redrawWaitingSpawnsLocked()
    {
        resetGridLocked();
        for (const auto &entry : players)
        {
            const Player &p = entry.second;
            if (p.spawnY >= 0 && p.spawnY < HEIGHT && p.spawnX >= 0 && p.spawnX < WIDTH)
                grid[p.spawnY][p.spawnX] = p.symbol;
        }
    }

    bool nameInUseLocked(const std::string &candidate, int exceptSocket = -1) const
    {
        for (const auto &entry : connectionNames)
        {
            if (entry.first == exceptSocket)
                continue;
            if (entry.second == candidate)
                return true;
        }

        return false;
    }

    std::string makeUniqueNameLocked(std::string desired, int exceptSocket = -1) const
    {
        std::string base = sanitizePlayerName(desired);
        if (base.empty())
            base = "Player";

        if (!nameInUseLocked(base, exceptSocket))
            return base;

        for (int suffix = 2; suffix < 1000; suffix++)
        {
            std::string candidate = base + "-" + std::to_string(suffix);
            if (candidate.size() > 24)
                candidate = candidate.substr(0, 24);

            while (!candidate.empty() && candidate.back() == ' ')
                candidate.pop_back();

            if (!candidate.empty() && !nameInUseLocked(candidate, exceptSocket))
                return candidate;
        }

        return base;
    }

    void startCountdownLocked(std::vector<std::string> *events = nullptr)
    {
        phase = GamePhase::Countdown;
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

        for (auto &entry : players)
        {
            Player &p = entry.second;
            p.alive = true;
            p.dir = "RIGHT";
            p.x = p.spawnX;
            p.y = p.spawnY;
            grid[p.y][p.x] = p.symbol;
        }

        phase = GamePhase::InProgress;
        matchTicksRemaining = kMatchDurationTicks;
        winnerSymbol = '\0';
        winnerName.clear();
        checkpointRequested = true;
        if (events != nullptr)
            events->push_back("MATCH_STARTED");
    }

    void finishMatchLocked(std::vector<std::string> *events = nullptr)
    {
        phase = GamePhase::Finished;
        winnerSymbol = '\0';
        winnerName.clear();

        int aliveCount = 0;
        char lastAlive = '\0';
        std::string lastAliveName;

        for (const auto &entry : players)
        {
            const Player &p = entry.second;
            if (p.alive)
            {
                aliveCount++;
                lastAlive = p.symbol;
                lastAliveName = p.name;
            }
        }

        if (aliveCount == 1)
        {
            winnerSymbol = lastAlive;
            winnerName = lastAliveName;
        }

        checkpointRequested = true;
        if (events != nullptr)
            events->push_back("MATCH_FINISHED");
    }

    std::string renderGridForClientLocked(int socket) const
    {
        std::vector<std::string> rows(HEIGHT, std::string(WIDTH, '.'));
        for (int y = 0; y < HEIGHT; y++)
        {
            for (int x = 0; x < WIDTH; x++)
                rows[y][x] = grid[y][x];
        }

        if (phase == GamePhase::Countdown)
        {
            auto playerIt = players.find(socket);
            if (playerIt != players.end())
            {
                const Player &p = playerIt->second;
                bool visible = ((countdownTicksElapsed / 2) % 2) == 0;
                if (!visible && p.spawnY >= 0 && p.spawnY < HEIGHT && p.spawnX >= 0 && p.spawnX < WIDTH)
                    rows[p.spawnY][p.spawnX] = '.';
            }
        }

        std::string out;
        out.reserve((WIDTH + 1) * HEIGHT);
        for (int y = 0; y < HEIGHT; y++)
        {
            out += rows[y];
            out += '\n';
        }
        return out;
    }

    std::string statusTextLocked(bool isPlayer, bool alive, char playerSymbol,
                                 const std::string &playerName,
                                 int remainingSeconds) const
    {
        (void)playerName;
        if (phase == GamePhase::Waiting)
        {
            if (isPlayer)
                return "LOBBY | Press R to start. Need at least " +
                       std::to_string(kMinimumPlayersToStart) + " player(s).";
            return "SPECTATING | Press R to start. Need at least " +
                   std::to_string(kMinimumPlayersToStart) + " player(s).";
        }

        if (phase == GamePhase::Countdown)
        {
            if (isPlayer)
                return "COUNTDOWN | Match starts in " + std::to_string(remainingSeconds) +
                       "s. Your spawn is flashing.";
            return "SPECTATING | Countdown: " + std::to_string(remainingSeconds) + "s.";
        }

        if (phase == GamePhase::InProgress)
        {
            if (isPlayer && alive)
                return "ALIVE | Time remaining: " + std::to_string(remainingSeconds) + "s.";

            if (isPlayer && !alive)
                return "DEAD | Now spectating. Time remaining: " +
                       std::to_string(remainingSeconds) + "s.";

            return "SPECTATING | Time remaining: " + std::to_string(remainingSeconds) + "s.";
        }

        if (winnerName.empty())
            return "MATCH_OVER | Draw.";

        if (isPlayer && winnerSymbol != '\0' && playerSymbol == winnerSymbol)
            return "MATCH_OVER | WINNER: " + winnerName + "!";

        if (winnerSymbol == '\0')
            return "MATCH_OVER | Winner: " + winnerName;

        return "MATCH_OVER | Winner: " + winnerName + " (" + std::string(1, winnerSymbol) + ")";
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
            auto socketIt = socketByClientId.find(input.clientId);
            if (socketIt == socketByClientId.end())
                continue;

            int socket = socketIt->second;

            if (input.command == "START")
            {
                bool isConnected = players.find(socket) != players.end() ||
                                   spectators.find(socket) != spectators.end();
                if (!isConnected || players.size() < kMinimumPlayersToStart)
                    continue;

                if (phase == GamePhase::Waiting || phase == GamePhase::Finished)
                {
                    redrawWaitingSpawnsLocked();
                    startCountdownLocked(&events);
                }
                continue;
            }

            auto it = players.find(socket);
            if (it == players.end())
                continue;

            if (phase != GamePhase::InProgress)
                continue;

            Player &p = it->second;
            if (!p.alive)
                continue;

            if ((input.command == "UP" && p.dir == "DOWN") ||
                (input.command == "DOWN" && p.dir == "UP") ||
                (input.command == "LEFT" && p.dir == "RIGHT") ||
                (input.command == "RIGHT" && p.dir == "LEFT"))
                continue;

            if (input.command == "UP" || input.command == "DOWN" ||
                input.command == "LEFT" || input.command == "RIGHT")
            {
                p.dir = input.command;
            }
        }
    }

    ReplicationCheckpoint buildReplicationCheckpointLocked() const
    {
        ReplicationCheckpoint checkpoint;
        checkpoint.epoch = epoch;
        checkpoint.tick = currentTick;
        checkpoint.publishedTick = publishedTick;
        checkpoint.safeTick = safeTick;
        checkpoint.phase = phaseToString(phase);
        checkpoint.countdownTicksRemaining = countdownTicksRemaining;
        checkpoint.matchTicksRemaining = matchTicksRemaining;
        checkpoint.winnerName = winnerName;
        checkpoint.winnerSymbol = winnerSymbol;

        for (const auto &entry : players)
        {
            const Player &player = entry.second;
            ReplicatedPlayerState replicaPlayer;
            replicaPlayer.clientId = player.clientId;
            replicaPlayer.name = player.name;
            replicaPlayer.symbol = player.symbol;
            replicaPlayer.x = player.x;
            replicaPlayer.y = player.y;
            replicaPlayer.spawnX = player.spawnX;
            replicaPlayer.spawnY = player.spawnY;
            replicaPlayer.dir = player.dir;
            replicaPlayer.alive = player.alive;
            checkpoint.players.push_back(replicaPlayer);
        }

        for (int socket : spectators)
        {
            auto it = connectionNames.find(socket);
            if (it != connectionNames.end())
                checkpoint.spectators.push_back(it->second);
        }

        for (int y = 0; y < HEIGHT; y++)
        {
            std::string row;
            row.reserve(WIDTH);
            for (int x = 0; x < WIDTH; x++)
                row.push_back(grid[y][x]);
            checkpoint.gridRows.push_back(row);
        }

        checkpoint.stateHash = computeStateHash(checkpoint);
        return checkpoint;
    }

public:
    TronGame()
    {
        resetGridLocked();
    }

    void setEpoch(std::uint64_t newEpoch)
    {
        std::lock_guard<std::mutex> lock(mtx);
        epoch = newEpoch;
        checkpointRequested = true;
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

    void setReplicaSafeTickHint(std::uint64_t newSafeTick)
    {
        std::lock_guard<std::mutex> lock(mtx);
        safeTick = std::min(newSafeTick, publishedTick);
    }

    bool registerConnection(int socket, std::string &note)
    {
        std::lock_guard<std::mutex> lock(mtx);
        connectionClientIds[socket] = "socket-" + std::to_string(socket);

        if (phase == GamePhase::Waiting)
        {
            Player player;
            player.id = nextId++;
            player.socket = socket;
            player.alive = true;
            player.dir = "RIGHT";
            player.symbol = static_cast<char>('A' + ((player.id - 1) % 26));
            player.name = makeUniqueNameLocked("Player " + std::string(1, player.symbol));
            player.clientId = connectionClientIds[socket];

            if (!assignSpawnLocked(player))
            {
                spectators.insert(socket);
                connectionNames[socket] = makeUniqueNameLocked("Spectator");
                note = "joined as spectator (no free spawn available)";
                checkpointRequested = true;
                return false;
            }

            players[socket] = player;
            connectionNames[socket] = player.name;
            socketByClientId[player.clientId] = socket;
            grid[player.spawnY][player.spawnX] = player.symbol;
            note = "joined as player " + std::string(1, player.symbol);
            checkpointRequested = true;
            return true;
        }

        spectators.insert(socket);
        connectionNames[socket] = makeUniqueNameLocked("Spectator");
        socketByClientId[connectionClientIds[socket]] = socket;
        if (phase == GamePhase::Countdown)
            note = "joined as spectator (match countdown already started)";
        else if (phase == GamePhase::InProgress)
            note = "joined as spectator (mid-match join blocked)";
        else
            note = "joined as spectator (match already finished)";

        checkpointRequested = true;
        return false;
    }

    void removeConnection(int socket)
    {
        std::lock_guard<std::mutex> lock(mtx);
        bool foundConnection = false;

        auto clientIdIt = connectionClientIds.find(socket);
        if (clientIdIt != connectionClientIds.end())
        {
            socketByClientId.erase(clientIdIt->second);
            connectionClientIds.erase(clientIdIt);
        }

        auto playerIt = players.find(socket);
        if (playerIt != players.end())
        {
            foundConnection = true;
            const Player &p = playerIt->second;
            if (p.y >= 0 && p.y < HEIGHT && p.x >= 0 && p.x < WIDTH)
            {
                if (phase == GamePhase::InProgress && p.alive)
                    grid[p.y][p.x] = '#';
                else
                    grid[p.y][p.x] = '.';
            }
            players.erase(playerIt);
        }

        if (spectators.erase(socket) > 0)
            foundConnection = true;

        connectionNames.erase(socket);
        checkpointRequested = true;

        if (phase == GamePhase::Countdown && players.size() < kMinimumPlayersToStart)
        {
            phase = GamePhase::Waiting;
            winnerSymbol = '\0';
            winnerName.clear();
            redrawWaitingSpawnsLocked();
        }

        if (phase == GamePhase::InProgress && alivePlayersLocked() <= 1)
        {
            std::vector<std::string> events;
            finishMatchLocked(&events);
        }

        if (phase == GamePhase::Finished && players.empty())
        {
            phase = GamePhase::Waiting;
            winnerSymbol = '\0';
            winnerName.clear();
            resetGridLocked();
        }

        if (foundConnection)
            close(socket);
    }

    void setClientIdentity(int socket, const std::string &clientId, const std::string &requestedName)
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::string sanitizedId = sanitizePlayerName(clientId);
        std::string sanitizedName = sanitizePlayerName(requestedName);
        if (sanitizedId.empty())
            return;
        if (sanitizedName.empty())
            sanitizedName = "Player";

        std::string oldClientId = connectionClientIds[socket];
        socketByClientId.erase(oldClientId);

        connectionClientIds[socket] = sanitizedId;
        socketByClientId[sanitizedId] = socket;

        std::string finalName = makeUniqueNameLocked(sanitizedName, socket);
        connectionNames[socket] = finalName;

        auto playerIt = players.find(socket);
        if (playerIt != players.end())
        {
            playerIt->second.clientId = sanitizedId;
            playerIt->second.name = finalName;
        }

        checkpointRequested = true;
    }

    bool queueInput(int socket, const ReplicatedInput &input)
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (input.clientId.empty() || input.command.empty())
            return false;

        auto connectionIt = connectionClientIds.find(socket);
        if (connectionIt == connectionClientIds.end())
            return false;

        if (connectionIt->second != input.clientId)
            return false;

        if (input.epochHint != 0 && input.epochHint != epoch)
            return false;

        if (input.targetTick <= currentTick)
            return false;

        auto &slot = pendingInputs[input.targetTick][input.clientId];
        if (slot.clientSeq > input.clientSeq)
            return false;

        slot = input;
        return true;
    }

    std::string buildFrameForClient(int socket)
    {
        std::lock_guard<std::mutex> lock(mtx);

        auto playerIt = players.find(socket);
        bool isPlayer = playerIt != players.end();
        bool alive = isPlayer && playerIt->second.alive;
        char playerSymbol = isPlayer ? playerIt->second.symbol : '\0';
        std::string playerName = "NONE";
        if (isPlayer)
            playerName = playerIt->second.name;
        else
        {
            auto connectionIt = connectionNames.find(socket);
            if (connectionIt != connectionNames.end() && !connectionIt->second.empty())
                playerName = connectionIt->second;
        }

        int remaining = 0;
        if (phase == GamePhase::Countdown)
            remaining = secondsRemainingFromTicks(countdownTicksRemaining);
        else if (phase == GamePhase::InProgress)
            remaining = secondsRemainingFromTicks(matchTicksRemaining);

        std::ostringstream out;
        out << "META\n";
        out << "epoch=" << epoch << "\n";
        out << "tick=" << currentTick << "\n";
        out << "published_tick=" << publishedTick << "\n";
        out << "safe_tick=" << safeTick << "\n";
        out << "authoritative=1\n";
        out << "phase=" << phaseToString(phase) << "\n";
        out << "role=" << (isPlayer ? "PLAYER" : "SPECTATOR") << "\n";
        out << "alive=" << (alive ? 1 : 0) << "\n";
        out << "winner=" << (winnerName.empty() ? "NONE" : winnerName) << "\n";
        out << "winner_symbol=" << (winnerSymbol == '\0' ? "NONE" : std::string(1, winnerSymbol)) << "\n";
        out << "time_remaining=" << remaining << "\n";
        out << "player_symbol=" << (isPlayer ? std::string(1, playerSymbol) : "NONE") << "\n";
        out << "player_name=" << playerName << "\n";
        out << "status=" << statusTextLocked(isPlayer, alive, playerSymbol, playerName, remaining) << "\n";
        out << "GRID\n";
        out << renderGridForClientLocked(socket);
        out << "END\n";
        return out.str();
    }

    TickArtifacts advanceTick()
    {
        std::lock_guard<std::mutex> lock(mtx);
        currentTick++;

        std::vector<ReplicatedInput> acceptedInputs = collectAcceptedInputsLocked(currentTick);
        std::vector<std::string> authoritativeEvents;
        applyAcceptedInputsLocked(acceptedInputs, authoritativeEvents);

        if (phase == GamePhase::Waiting)
        {
            publishedTick = currentTick;
        }
        else if (phase == GamePhase::Countdown)
        {
            if (players.size() < kMinimumPlayersToStart)
            {
                phase = GamePhase::Waiting;
                winnerSymbol = '\0';
                winnerName.clear();
                redrawWaitingSpawnsLocked();
                checkpointRequested = true;
                authoritativeEvents.push_back("COUNTDOWN_CANCELLED");
            }
            else
            {
                countdownTicksElapsed++;
                if (countdownTicksRemaining > 0)
                    countdownTicksRemaining--;

                if (countdownTicksRemaining <= 0)
                    startMatchLocked(&authoritativeEvents);
            }

            publishedTick = currentTick;
        }
        else if (phase == GamePhase::InProgress)
        {
            for (auto &entry : players)
            {
                Player &p = entry.second;
                if (!p.alive)
                    continue;
                grid[p.y][p.x] = '#';
            }

            for (auto &entry : players)
            {
                Player &p = entry.second;
                if (!p.alive)
                    continue;

                int nextX = p.x;
                int nextY = p.y;

                if (p.dir == "UP")
                    nextY--;
                else if (p.dir == "DOWN")
                    nextY++;
                else if (p.dir == "LEFT")
                    nextX--;
                else if (p.dir == "RIGHT")
                    nextX++;

                if (nextX < 0 || nextX >= WIDTH || nextY < 0 || nextY >= HEIGHT ||
                    grid[nextY][nextX] != '.')
                {
                    p.alive = false;
                    continue;
                }

                p.x = nextX;
                p.y = nextY;
                grid[p.y][p.x] = p.symbol;
            }

            if (matchTicksRemaining > 0)
                matchTicksRemaining--;

            if (alivePlayersLocked() <= 1 || matchTicksRemaining <= 0)
                finishMatchLocked(&authoritativeEvents);

            publishedTick = currentTick;
        }
        else
        {
            publishedTick = currentTick;
        }

        ReplicationCheckpoint checkpoint = buildReplicationCheckpointLocked();

        TickArtifacts artifacts;
        artifacts.checkpoint = checkpoint;
        artifacts.seal.epoch = epoch;
        artifacts.seal.tick = currentTick;
        artifacts.seal.acceptedInputs = acceptedInputs;
        artifacts.seal.authoritativeEvents = authoritativeEvents;
        artifacts.seal.stateHash = checkpoint.stateHash;
        artifacts.sendCheckpoint = checkpointRequested ||
                                   (currentTick % kCheckpointEveryTicks == 0) ||
                                   !authoritativeEvents.empty();
        checkpointRequested = false;
        return artifacts;
    }
};

void broadcastToReplica(const std::string &message)
{
    std::lock_guard<std::mutex> lock(replicaMtx);
    if (replicaSocket < 0)
        return;

    if (send(replicaSocket, message.c_str(), message.size(), 0) <= 0)
    {
        close(replicaSocket);
        replicaSocket = -1;
        replicaConnected = false;
    }
}

void removeClientSocket(int socket)
{
    std::lock_guard<std::mutex> lock(clientMtx);
    auto it = std::remove(clients.begin(), clients.end(), socket);
    clients.erase(it, clients.end());
}

void clientHandler(TronGame &game, int socket);

IncomingConnectionType classifyIncomingConnection(int socket)
{
    char buffer[64]{0};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

    while (std::chrono::steady_clock::now() < deadline)
    {
        int bytes = recv(socket, buffer, sizeof(buffer) - 1, MSG_PEEK | MSG_DONTWAIT);
        if (bytes > 0)
        {
            std::string pending(buffer, bytes);
            size_t lineEnd = pending.find_first_of("\r\n");
            if (lineEnd == std::string::npos)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::string banner = pending.substr(0, lineEnd);
            if (banner == "REPLICA")
                return IncomingConnectionType::Replica;
            if (banner == "CLIENT")
                return IncomingConnectionType::Client;
            return IncomingConnectionType::Unknown;
        }

        if (bytes == 0)
            return IncomingConnectionType::Unknown;

        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return IncomingConnectionType::Unknown;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return IncomingConnectionType::Unknown;
}

void consumeBannerLine(int socket)
{
    std::string pending;
    char buffer[128];

    while (true)
    {
        int bytes = recv(socket, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (bytes <= 0)
        {
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return;
        }

        pending.append(buffer, bytes);
        size_t newline = pending.find('\n');
        if (newline != std::string::npos)
            return;
    }
}

void replicaSafeTickReader(TronGame &game, int socket)
{
    consumeBannerLine(socket);

    std::string buffer;
    char recvBuffer[1024];

    while (runPrimary.load())
    {
        int bytes = recv(socket, recvBuffer, sizeof(recvBuffer), 0);
        if (bytes > 0)
        {
            buffer.append(recvBuffer, bytes);

            size_t endPos = std::string::npos;
            while ((endPos = buffer.find("END_SAFE_TICK\n")) != std::string::npos)
            {
                std::string rawReport = buffer.substr(0, endPos + 14);
                buffer.erase(0, endPos + 14);

                SafeTickReport report;
                if (!parseSafeTickReport(rawReport, report))
                    continue;

                if (report.epoch != game.getEpoch())
                    continue;

                replicaVerifiedSafeTick.store(report.safeTick);
                game.setReplicaSafeTickHint(report.safeTick);
            }

            continue;
        }

        if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
            break;
    }

    {
        std::lock_guard<std::mutex> lock(replicaMtx);
        if (replicaSocket == socket)
        {
            close(replicaSocket);
            replicaSocket = -1;
            replicaConnected = false;
        }
    }
}

void startClientSession(TronGame &game, int clientSocket)
{
    {
        std::lock_guard<std::mutex> lock(clientMtx);
        clients.push_back(clientSocket);
    }

    std::string note;
    game.registerConnection(clientSocket, note);
    std::cout << "Client connected: " << note << "\n";

    std::string frame = game.buildFrameForClient(clientSocket);
    if (send(clientSocket, frame.c_str(), frame.size(), 0) <= 0)
    {
        game.removeConnection(clientSocket);
        removeClientSocket(clientSocket);
        return;
    }

    std::thread(clientHandler, std::ref(game), clientSocket).detach();
}

void handleIncomingConnection(TronGame &game, int socket)
{
    IncomingConnectionType kind = classifyIncomingConnection(socket);
    if (kind == IncomingConnectionType::Replica)
    {
        std::lock_guard<std::mutex> lock(replicaMtx);
        if (replicaSocket >= 0)
        {
            std::cout << "Replica already connected; rejecting extra replica connection.\n";
            close(socket);
            return;
        }

        replicaSocket = socket;
        replicaConnected = true;
        std::cout << "Replica connected\n";
        std::thread(replicaSafeTickReader, std::ref(game), socket).detach();
        return;
    }

    if (kind == IncomingConnectionType::Client)
    {
        startClientSession(game, socket);
        return;
    }

    std::cout << "Rejected connection with missing or unknown startup banner.\n";
    close(socket);
}

void sendFramesToClients(TronGame &game)
{
    std::vector<int> sockets;
    {
        std::lock_guard<std::mutex> lock(clientMtx);
        sockets = clients;
    }

    for (int socket : sockets)
    {
        std::string frame = game.buildFrameForClient(socket);
        if (send(socket, frame.c_str(), frame.size(), 0) <= 0)
        {
            game.removeConnection(socket);
            removeClientSocket(socket);
        }
    }
}

void clientHandler(TronGame &game, int socket)
{
    std::string pending;
    char buffer[1024];

    while (runPrimary.load())
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
                    game.setClientIdentity(socket, clientId, playerName);
                    continue;
                }

                ReplicatedInput input;
                if (parseInputLine(line, input))
                {
                    game.queueInput(socket, input);
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

    game.removeConnection(socket);
    removeClientSocket(socket);
}

void gameLoop(TronGame &game,
              const std::string &witnessHost,
              int witnessPort,
              const std::string &leaderId)
{
    int missedWitnessRenewals = 0;
    bool witnessAvailable = true;

    while (runPrimary.load())
    {
        auto tickStart = std::chrono::steady_clock::now();

        WitnessReply renewReply = renewWitnessLease(witnessHost, witnessPort, leaderId, game.getEpoch());
        if (!renewReply.reachable)
        {
            missedWitnessRenewals++;
            if (missedWitnessRenewals >= 3)
                witnessAvailable = false;
        }
        else
        {
            missedWitnessRenewals = 0;
            witnessAvailable = true;
            if (!renewReply.granted && renewReply.epoch > game.getEpoch())
            {
                std::cerr << "Witness reports newer leader epoch " << renewReply.epoch
                          << ". Stepping down primary loop.\n";
                runPrimary = false;
                break;
            }
        }

        game.setReplicaSafeTickHint(replicaVerifiedSafeTick.load());
        TickArtifacts artifacts = game.advanceTick();
        sendFramesToClients(game);

        HeartbeatMessage heartbeat;
        heartbeat.epoch = artifacts.checkpoint.epoch;
        heartbeat.publishedTick = artifacts.checkpoint.publishedTick;
        heartbeat.safeTickHint = artifacts.checkpoint.safeTick;
        heartbeat.leaseStatus = witnessAvailable ? "HEALTHY" : "DEGRADED";
        heartbeat.leaderId = leaderId;

        broadcastToReplica(serializeHeartbeat(heartbeat));
        broadcastToReplica(serializeTickSeal(artifacts.seal));
        if (artifacts.sendCheckpoint)
            broadcastToReplica(serializeCheckpoint(artifacts.checkpoint));

        auto elapsed = std::chrono::steady_clock::now() - tickStart;
        auto sleepFor = std::chrono::milliseconds(kServerTickMs) -
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        if (sleepFor.count() > 0)
            std::this_thread::sleep_for(sleepFor);
    }
}

struct ServerStartupOptions
{
    std::string host;
    int port = 0;
    std::string witness_host;
    int witness_port = 0;
};

void printServerUsage(const char *programName)
{
    std::cout << "Usage: " << programName
              << " [--host <sunlab-host>] [--port 6000-6010]"
              << " [--w_host <sunlab-host>] [--w_port 6000-6010]\n";
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

        if (arg == "--host")
        {
            if (!parseHostArg(options.host))
            {
                std::cerr << "Invalid --host value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--port")
        {
            if (!parsePortArg(options.port))
            {
                std::cerr << "Invalid --port value.\n";
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

    if (options.host.empty())
        options.host = promptForSunlabHost();
    if (options.port == 0)
        options.port = promptForPort();
    if (options.witness_host.empty())
    {
        std::cout << "Enter witness host:\n";
        options.witness_host = promptForSunlabHost();
    }
    if (options.witness_port == 0)
        options.witness_port = promptForPort();

    std::string localHost = localHostnameNormalized();
    if (!localHost.empty() && isValidSunlabHost(localHost) && localHost != options.host)
    {
        std::cerr << "Selected host " << toConnectHost(options.host)
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
    address.sin_port = htons(options.port);
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

    std::string leaderId = toConnectHost(options.host) + ":" + std::to_string(options.port);
    WitnessReply acquireReply = acquireWitnessLease(toConnectHost(options.witness_host),
                                                    options.witness_port,
                                                    leaderId,
                                                    1);
    if (!acquireReply.reachable || !acquireReply.granted)
    {
        std::cerr << "Unable to acquire witness lease for primary leadership.\n";
        close(serverFd);
        return 1;
    }

    TronGame game;
    game.setEpoch(acquireReply.epoch);
    std::thread(gameLoop,
                std::ref(game),
                toConnectHost(options.witness_host),
                options.witness_port,
                leaderId)
        .detach();

    std::cout << "PRIMARY running on " << toConnectHost(options.host) << " port " << options.port << "\n";
    std::cout << "Witness on " << toConnectHost(options.witness_host) << ":" << options.witness_port << "\n";

    while (runPrimary.load())
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

        handleIncomingConnection(game, clientSocket);
    }

    close(serverFd);
    return 0;
}
