#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <cctype>
#include <mutex>

#include "replication_protocol.h"
#include "sunlab_config.h"

const std::string playerUpRune = "\xE2\x87\xA1";
const std::string playerLeftRune = "\xE2\x87\xA0";
const std::string playerDownRune = "\xE2\x87\xA3";
const std::string playerRightRune = "\xE2\x87\xA2";

const std::string playerTrailHorizontal = "\xE2\x94\x84";
const std::string playerTrailVertical = "\xE2\x94\x86";
const std::string playerTrailLeftCornerUp = "\xE2\x95\xAD";
const std::string playerTrailLeftCornerDown = "\xE2\x95\xB0";
const std::string playerTrailRightCornerDown = "\xE2\x95\xAF";
const std::string playerTrailRightCornerUp = "\xE2\x95\xAE";

enum class Direction
{
    Up,
    Left,
    Down,
    Right,
    Unknown
};

struct Position
{
    int x = -1;
    int y = -1;
};

struct PlayerRenderState
{
    Position head;
    Direction lastMove = Direction::Unknown;
    bool hasHead = false;
};

Direction directionBetween(const Position &from, const Position &to)
{
    int dx = to.x - from.x;
    int dy = to.y - from.y;

    if (dx == 0 && dy == -1)
        return Direction::Up;
    if (dx == -1 && dy == 0)
        return Direction::Left;
    if (dx == 0 && dy == 1)
        return Direction::Down;
    if (dx == 1 && dy == 0)
        return Direction::Right;

    return Direction::Unknown;
}

Direction opposite(Direction dir)
{
    if (dir == Direction::Up)
        return Direction::Down;
    if (dir == Direction::Down)
        return Direction::Up;
    if (dir == Direction::Left)
        return Direction::Right;
    if (dir == Direction::Right)
        return Direction::Left;
    return Direction::Unknown;
}

const std::string &headRuneFor(Direction dir)
{
    if (dir == Direction::Up)
        return playerUpRune;
    if (dir == Direction::Left)
        return playerLeftRune;
    if (dir == Direction::Down)
        return playerDownRune;
    return playerRightRune;
}

const std::string &trailRuneFor(Direction connectionA, Direction connectionB)
{
    bool hasUp = connectionA == Direction::Up || connectionB == Direction::Up;
    bool hasLeft = connectionA == Direction::Left || connectionB == Direction::Left;
    bool hasDown = connectionA == Direction::Down || connectionB == Direction::Down;
    bool hasRight = connectionA == Direction::Right || connectionB == Direction::Right;

    if (hasLeft && hasRight)
        return playerTrailHorizontal;
    if (hasUp && hasDown)
        return playerTrailVertical;
    if (hasRight && hasDown)
        return playerTrailLeftCornerUp;
    if (hasRight && hasUp)
        return playerTrailLeftCornerDown;
    if (hasLeft && hasUp)
        return playerTrailRightCornerDown;
    if (hasLeft && hasDown)
        return playerTrailRightCornerUp;

    return playerTrailHorizontal;
}

std::vector<std::string> splitLines(const std::string &text)
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

    while (!lines.empty() && lines.back().empty())
        lines.pop_back();

    return lines;
}

bool looksLikeGrid(const std::vector<std::string> &rows)
{
    if (rows.empty() || rows.front().empty())
        return false;

    size_t width = rows.front().size();
    for (const std::string &row : rows)
    {
        if (row.size() != width)
            return false;

        for (char cell : row)
        {
            if (cell != '.' && cell != '#' && !std::isupper(static_cast<unsigned char>(cell)))
                return false;
        }
    }

    return true;
}

std::string renderGrid(
    const std::string &rawGrid,
    std::unordered_map<char, PlayerRenderState> &playerStates,
    std::vector<std::vector<std::string>> &frozenTrailRunes)
{
    std::vector<std::string> rows = splitLines(rawGrid);
    if (!looksLikeGrid(rows))
        return rawGrid;

    int height = static_cast<int>(rows.size());
    int width = static_cast<int>(rows[0].size());

    if (frozenTrailRunes.size() != static_cast<size_t>(height) ||
        (!frozenTrailRunes.empty() && frozenTrailRunes[0].size() != static_cast<size_t>(width)))
    {
        frozenTrailRunes.assign(height, std::vector<std::string>(width, ""));
        playerStates.clear();
    }

    std::unordered_map<char, Position> heads;
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            char cell = rows[y][x];
            if (std::isupper(static_cast<unsigned char>(cell)))
                heads[cell] = Position{x, y};
        }
    }

    for (auto it = playerStates.begin(); it != playerStates.end();)
    {
        if (heads.find(it->first) == heads.end())
            it = playerStates.erase(it);
        else
            ++it;
    }

    for (const auto &[playerId, currentHead] : heads)
    {
        PlayerRenderState &state = playerStates[playerId];

        if (state.hasHead)
        {
            Direction currentMove = directionBetween(state.head, currentHead);
            if (currentMove != Direction::Unknown)
            {
                int trailX = state.head.x;
                int trailY = state.head.y;
                if (trailY >= 0 && trailY < height &&
                    trailX >= 0 && trailX < width &&
                    rows[trailY][trailX] == '#' &&
                    frozenTrailRunes[trailY][trailX].empty())
                {
                    if (state.lastMove == Direction::Unknown)
                    {
                        if (currentMove == Direction::Up || currentMove == Direction::Down)
                            frozenTrailRunes[trailY][trailX] = playerTrailVertical;
                        else
                            frozenTrailRunes[trailY][trailX] = playerTrailHorizontal;
                    }
                    else
                    {
                        Direction trailToPrevious = opposite(state.lastMove);
                        frozenTrailRunes[trailY][trailX] = trailRuneFor(trailToPrevious, currentMove);
                    }
                }

                state.lastMove = currentMove;
            }
        }

        state.head = currentHead;
        state.hasHead = true;
    }

    std::string rendered;
    rendered.reserve(rawGrid.size() * 3);

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            char cell = rows[y][x];
            if (std::isupper(static_cast<unsigned char>(cell)))
            {
                auto stateIt = playerStates.find(cell);
                Direction dir = stateIt == playerStates.end() ? Direction::Right : stateIt->second.lastMove;
                rendered += headRuneFor(dir);
            }
            else if (cell == '#' && !frozenTrailRunes[y][x].empty())
            {
                rendered += frozenTrailRunes[y][x];
            }
            else
            {
                rendered.push_back(cell);
            }
        }
        rendered.push_back('\n');
    }

    return rendered;
}

char getch()
{
    struct termios oldt, newt;
    char ch;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}

std::string mapKey(char c)
{
    if (c == 'w')
        return "UP";
    if (c == 's')
        return "DOWN";
    if (c == 'a')
        return "LEFT";
    if (c == 'd')
        return "RIGHT";
    if (c == 'r' || c == 'R')
        return "START";
    return "";
}

struct ServerFrame
{
    unsigned long long epoch = 0;
    unsigned long long tick = 0;
    unsigned long long publishedTick = 0;
    unsigned long long safeTick = 0;
    bool authoritative = true;
    std::string phase = "UNKNOWN";
    std::string role = "PLAYER";
    bool alive = true;
    std::string winner = "NONE";
    int timeRemaining = 0;
    std::string playerSymbol = "NONE";
    std::string status;
    std::string grid;
};

void trimLineEnd(std::string &line)
{
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
}

ServerFrame parseFrame(const std::string &rawFrame)
{
    ServerFrame frame;
    std::istringstream stream(rawFrame);
    std::string line;

    if (!std::getline(stream, line))
        return frame;
    trimLineEnd(line);

    if (line != "META")
    {
        frame.grid = rawFrame;
        return frame;
    }

    while (std::getline(stream, line))
    {
        trimLineEnd(line);
        if (line == "GRID")
            break;

        size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);

        if (key == "epoch")
            frame.epoch = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "tick")
            frame.tick = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "published_tick")
            frame.publishedTick = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "safe_tick")
            frame.safeTick = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "authoritative")
            frame.authoritative = value == "1";
        else if (key == "phase")
            frame.phase = value;
        else if (key == "role")
            frame.role = value;
        else if (key == "alive")
            frame.alive = value == "1";
        else if (key == "winner")
            frame.winner = value;
        else if (key == "time_remaining")
            frame.timeRemaining = std::atoi(value.c_str());
        else if (key == "player_symbol")
            frame.playerSymbol = value;
        else if (key == "status")
            frame.status = value;
    }

    std::ostringstream gridStream;
    while (std::getline(stream, line))
    {
        trimLineEnd(line);
        gridStream << line << '\n';
    }

    frame.grid = gridStream.str();
    return frame;
}

void printStatusBar(const ServerFrame &frame, const std::string &sourceLabel)
{
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "Source: " << sourceLabel;
    std::cout << " | Epoch: " << frame.epoch;
    std::cout << " | Tick: " << frame.tick;
    std::cout << " | Published: " << frame.publishedTick;
    std::cout << " | Safe: " << frame.safeTick << "\n";
    std::cout << "Role: " << frame.role << " | ";

    if (frame.role == "PLAYER")
        std::cout << (frame.alive ? "ALIVE" : "DEAD");
    else
        std::cout << "SPECTATING";

    std::cout << " | Phase: " << frame.phase;

    if (frame.phase == "COUNTDOWN" || frame.phase == "IN_PROGRESS")
        std::cout << " | Time Remaining: " << frame.timeRemaining << "s";

    if (frame.winner != "NONE")
        std::cout << " | Winner: " << frame.winner;

    std::cout << "\n";
    if (!frame.status.empty())
        std::cout << frame.status << "\n";

    bool moveEnabled = frame.authoritative &&
                       frame.role == "PLAYER" &&
                       frame.alive &&
                       frame.phase == "IN_PROGRESS";
    std::cout << "Controls: R=start";
    if (moveEnabled)
        std::cout << " | WASD=move";
    else
        std::cout << " | WASD=move (disabled)";
    std::cout << "\n";
}

struct ClientStartupOptions
{
    std::string host;
    int port = 0;
    std::string replicaHost;
    int replicaPort = 0;
    std::string name;
};

void printClientUsage(const char *programName)
{
    std::cout << "Usage: " << programName
              << " [--host <sunlab-host>] [--port 6000-6010]"
              << " [--replica_host <sunlab-host>] [--replica_port 6000-6010]"
              << " [--name <player-name>]\n";
}

bool parseClientArgs(int argc, char *argv[], ClientStartupOptions &options)
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

        if (arg == "--replica_host")
        {
            if (!parseHostArg(options.replicaHost))
            {
                std::cerr << "Invalid --replica_host value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--replica_port")
        {
            if (!parsePortArg(options.replicaPort))
            {
                std::cerr << "Invalid --replica_port value.\n";
                return false;
            }
            continue;
        }

        if (arg == "--name")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--name requires a value.\n";
                return false;
            }

            std::string parsedName = sanitizePlayerName(argv[++i]);
            if (parsedName.empty())
            {
                std::cerr << "Invalid --name value. Use letters, digits, spaces, '_' or '-'.\n";
                return false;
            }

            options.name = parsedName;
            continue;
        }

        if (arg == "--help" || arg == "-h")
        {
            printClientUsage(argv[0]);
            std::exit(0);
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        return false;
    }

    return true;
}

int connectToHost(const std::string &host, int port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *results = nullptr;
    std::string portText = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), portText.c_str(), &hints, &results);
    if (rc != 0)
    {
        std::cerr << "getaddrinfo failed for " << host << ":" << port
                  << " (" << gai_strerror(rc) << ")\n";
        return -1;
    }

    int sock = -1;
    for (addrinfo *candidate = results; candidate != nullptr; candidate = candidate->ai_next)
    {
        sock = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (sock < 0)
            continue;

        if (connect(sock, candidate->ai_addr, candidate->ai_addrlen) == 0)
            break;

        close(sock);
        sock = -1;
    }

    freeaddrinfo(results);
    return sock;
}

std::string buildClientId(const std::string &playerName)
{
    std::string base = sanitizePlayerName(playerName);
    if (base.empty())
        base = "Player";

    std::string compact;
    compact.reserve(base.size());
    for (char c : base)
    {
        if (c == ' ')
            compact.push_back('_');
        else
            compact.push_back(c);
    }

    if (compact.size() > 14)
        compact = compact.substr(0, 14);

    std::uint64_t seed = static_cast<std::uint64_t>(::getpid()) ^
                         static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream suffix;
    suffix << std::hex << (seed & 0xFFFFFF);

    std::string out = compact + "-" + suffix.str();
    if (out.size() > 24)
        out = out.substr(0, 24);
    return out;
}

struct RenderDecision
{
    bool shouldRender = false;
    ServerFrame frame;
    std::string sourceLabel;
};

struct SharedDisplayState
{
    std::mutex mtx;
    ServerFrame authoritativeFrame;
    bool hasAuthoritative = false;
    unsigned long long highestAuthoritativeEpoch = 0;
    unsigned long long highestAuthoritativeTick = 0;
    unsigned long long latestSeenTick = 0;
    bool controlsEnabled = false;

    RenderDecision considerFrame(const ServerFrame &frame, const std::string &sourceLabel)
    {
        std::lock_guard<std::mutex> lock(mtx);
        latestSeenTick = std::max(latestSeenTick, frame.tick);

        RenderDecision decision;
        if (!frame.authoritative)
            return decision;

        bool newer = !hasAuthoritative ||
                     frame.epoch > highestAuthoritativeEpoch ||
                     (frame.epoch == highestAuthoritativeEpoch && frame.tick >= highestAuthoritativeTick);
        if (!newer)
            return decision;

        authoritativeFrame = frame;
        hasAuthoritative = true;
        highestAuthoritativeEpoch = frame.epoch;
        highestAuthoritativeTick = frame.tick;
        controlsEnabled = frame.role == "PLAYER" && frame.alive && frame.phase == "IN_PROGRESS";

        decision.shouldRender = true;
        decision.frame = authoritativeFrame;
        decision.sourceLabel = sourceLabel;
        return decision;
    }

    void snapshotForInput(unsigned long long &epochOut,
                          unsigned long long &targetTickOut,
                          bool &controlsOut)
    {
        std::lock_guard<std::mutex> lock(mtx);
        epochOut = hasAuthoritative ? highestAuthoritativeEpoch : 0;
        unsigned long long baseTick = hasAuthoritative ? highestAuthoritativeTick : latestSeenTick;
        targetTickOut = baseTick + 2;
        controlsOut = controlsEnabled;
    }
};

struct ConnectionTarget
{
    std::string label;
    std::string host;
    int port = 0;
    int socket = -1;
    std::atomic<bool> connected{false};
};

bool sendHello(ConnectionTarget &target, const std::string &clientId, const std::string &playerName)
{
    if (target.socket < 0)
        return false;

    if (send(target.socket, "CLIENT\n", 7, 0) <= 0)
        return false;

    std::string hello = buildHelloLine(clientId, playerName);
    return send(target.socket, hello.c_str(), hello.size(), 0) > 0;
}

void receiverLoop(ConnectionTarget &target,
                  SharedDisplayState &displayState,
                  std::mutex &renderMtx,
                  std::unordered_map<char, PlayerRenderState> &playerStates,
                  std::vector<std::vector<std::string>> &frozenTrailRunes)
{
    std::string buffer;
    char recvBuffer[2048];

    while (target.connected.load())
    {
        int bytes = recv(target.socket, recvBuffer, sizeof(recvBuffer), 0);
        if (bytes > 0)
        {
            buffer.append(recvBuffer, bytes);

            size_t pos;
            while ((pos = buffer.find("END\n")) != std::string::npos)
            {
                std::string rawFrame = buffer.substr(0, pos);
                buffer.erase(0, pos + 4);

                ServerFrame frame = parseFrame(rawFrame);
                RenderDecision decision = displayState.considerFrame(frame, target.label);
                if (!decision.shouldRender)
                    continue;

                std::string rendered = renderGrid(decision.frame.grid, playerStates, frozenTrailRunes);

                std::lock_guard<std::mutex> renderLock(renderMtx);
                std::system("clear");
                std::cout << rendered << std::flush;
                printStatusBar(decision.frame, decision.sourceLabel);
            }
            continue;
        }

        if (bytes == 0)
        {
            std::lock_guard<std::mutex> renderLock(renderMtx);
            std::cout << target.label << " disconnected.\n";
            break;
        }

        std::lock_guard<std::mutex> renderLock(renderMtx);
        std::perror(("recv " + target.label).c_str());
        break;
    }

    target.connected = false;
    if (target.socket >= 0)
    {
        close(target.socket);
        target.socket = -1;
    }
}

int main(int argc, char *argv[])
{
    ClientStartupOptions options;
    if (!parseClientArgs(argc, argv, options))
    {
        printClientUsage(argv[0]);
        return 1;
    }

    if (options.host.empty())
        options.host = promptForSunlabHost();
    if (options.port == 0)
        options.port = promptForPort();
    if (options.name.empty())
    {
        const char *user = std::getenv("USER");
        if (user == nullptr)
            user = std::getenv("USERNAME");

        if (user != nullptr)
            options.name = sanitizePlayerName(user);
        if (options.name.empty())
            options.name = promptForPlayerName();
    }

    std::string clientId = buildClientId(options.name);

    ConnectionTarget primary;
    primary.label = "PRIMARY";
    primary.host = toConnectHost(options.host);
    primary.port = options.port;
    primary.socket = connectToHost(primary.host, primary.port);
    if (primary.socket < 0)
    {
        std::cerr << "Unable to connect to " << primary.host << ":" << primary.port << "\n";
        return 1;
    }
    primary.connected = true;

    ConnectionTarget replica;
    bool haveReplica = !options.replicaHost.empty() && options.replicaPort != 0;
    if (haveReplica)
    {
        replica.label = "REPLICA";
        replica.host = toConnectHost(options.replicaHost);
        replica.port = options.replicaPort;
        replica.socket = connectToHost(replica.host, replica.port);
        if (replica.socket < 0)
        {
            std::cerr << "Warning: unable to connect to standby replica "
                      << replica.host << ":" << replica.port << ". Continuing with primary only.\n";
            haveReplica = false;
        }
        else
        {
            replica.connected = true;
        }
    }

    if (!sendHello(primary, clientId, options.name))
    {
        std::cerr << "Failed to send hello to primary.\n";
        close(primary.socket);
        return 1;
    }

    if (haveReplica && !sendHello(replica, clientId, options.name))
    {
        std::cerr << "Warning: failed to send hello to replica. Continuing without standby connection.\n";
        close(replica.socket);
        replica.socket = -1;
        replica.connected = false;
        haveReplica = false;
    }

    std::cout << "Connected to primary " << primary.host << ":" << primary.port << "\n";
    if (haveReplica)
        std::cout << "Connected to replica " << replica.host << ":" << replica.port << "\n";
    std::cout << "Player name: " << options.name << "\n";
    std::cout << "Client ID: " << clientId << "\n";

    SharedDisplayState displayState;
    std::mutex renderMtx;
    std::unordered_map<char, PlayerRenderState> playerStates;
    std::vector<std::vector<std::string>> frozenTrailRunes;

    std::thread primaryThread(receiverLoop,
                              std::ref(primary),
                              std::ref(displayState),
                              std::ref(renderMtx),
                              std::ref(playerStates),
                              std::ref(frozenTrailRunes));

    std::thread replicaThread;
    if (haveReplica)
    {
        replicaThread = std::thread(receiverLoop,
                                    std::ref(replica),
                                    std::ref(displayState),
                                    std::ref(renderMtx),
                                    std::ref(playerStates),
                                    std::ref(frozenTrailRunes));
    }

    std::atomic<unsigned long long> clientSeq{0};

    std::thread inputThread([&]()
                            {
        while (primary.connected.load() || (haveReplica && replica.connected.load()))
        {
            char c = getch();
            std::string command = mapKey(c);
            if (command.empty())
                continue;

            unsigned long long epochHint = 0;
            unsigned long long targetTick = 0;
            bool controlsEnabled = false;
            displayState.snapshotForInput(epochHint, targetTick, controlsEnabled);

            if (command != "START" && !controlsEnabled)
                continue;

            ReplicatedInput input;
            input.epochHint = epochHint;
            input.clientId = clientId;
            input.clientSeq = ++clientSeq;
            input.targetTick = targetTick;
            input.command = command;

            std::string wire = serializeInputLine(input);
            if (primary.connected.load() && primary.socket >= 0)
                send(primary.socket, wire.c_str(), wire.size(), 0);
            if (haveReplica && replica.connected.load() && replica.socket >= 0)
                send(replica.socket, wire.c_str(), wire.size(), 0);
        } });
    inputThread.detach();

    primaryThread.join();
    if (haveReplica)
        replicaThread.join();

    if (primary.socket >= 0)
        close(primary.socket);
    if (haveReplica && replica.socket >= 0)
        close(replica.socket);

    return 0;
}
