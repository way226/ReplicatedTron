#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cerrno>
#include <condition_variable>

#include "sunlab_config.h"

#define WIDTH 100
#define HEIGHT 50

constexpr int kServerTickMs = 150;
constexpr int kCountdownSeconds = 5;
constexpr int kMatchDurationSeconds = 90;
constexpr int kMinimumPlayersToStart = 1;

bool isPrimary = true;

std::mutex replica_mtx;
std::condition_variable replica_cv;
std::atomic<bool> replicaConnected{false};
int replicaSocket = -1;

std::vector<int> clients;
std::mutex client_mtx;

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
};

enum class GamePhase
{
    Waiting,
    Countdown,
    InProgress,
    Finished
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

int secondsRemaining(const std::chrono::steady_clock::time_point &endTime,
                     const std::chrono::steady_clock::time_point &now)
{
    if (now >= endTime)
        return 0;

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - now).count();
    return static_cast<int>((ms + 999) / 1000);
}

class TronGame
{
private:
    std::vector<std::vector<char>> grid;
    std::unordered_map<int, Player> players;
    std::unordered_set<int> spectators;
    std::mutex mtx;
    int nextId = 1;
    GamePhase phase = GamePhase::Waiting;
    std::chrono::steady_clock::time_point countdownStart{};
    std::chrono::steady_clock::time_point countdownEnd{};
    std::chrono::steady_clock::time_point matchEnd{};
    char winnerSymbol = '\0';
    std::string winnerName;

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

    void startCountdownLocked(const std::chrono::steady_clock::time_point &now)
    {
        phase = GamePhase::Countdown;
        winnerSymbol = '\0';
        winnerName.clear();
        countdownStart = now;
        countdownEnd = now + std::chrono::seconds(kCountdownSeconds);
    }

    void startMatchLocked(const std::chrono::steady_clock::time_point &now)
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
        matchEnd = now + std::chrono::seconds(kMatchDurationSeconds);
        winnerSymbol = '\0';
        winnerName.clear();
    }

    void finishMatchLocked()
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
    }

    std::string renderGridForClientLocked(int socket,
                                          const std::chrono::steady_clock::time_point &now) const
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
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - countdownStart).count();
                bool visible = ((elapsedMs / 250) % 2) == 0;
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
            {
                return "ALIVE | Time remaining: " + std::to_string(remainingSeconds) + "s.";
            }

            if (isPlayer && !alive)
            {
                return "DEAD | Now spectating. Time remaining: " +
                       std::to_string(remainingSeconds) + "s.";
            }

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

public:
    TronGame()
    {
        resetGridLocked();
    }

    bool registerConnection(int socket, std::string &note)
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (phase == GamePhase::Waiting)
        {
            Player player;
            player.id = nextId++;
            player.socket = socket;
            player.alive = true;
            player.dir = "RIGHT";
            player.symbol = static_cast<char>('A' + ((player.id - 1) % 26));
            player.name = "Player " + std::string(1, player.symbol);

            if (!assignSpawnLocked(player))
            {
                spectators.insert(socket);
                note = "joined as spectator (no free spawn available)";
                return false;
            }

            players[socket] = player;
            grid[player.spawnY][player.spawnX] = player.symbol;
            note = "joined as player " + std::string(1, player.symbol);

            return true;
        }

        spectators.insert(socket);
        if (phase == GamePhase::Countdown)
            note = "joined as spectator (match countdown already started)";
        else if (phase == GamePhase::InProgress)
            note = "joined as spectator (mid-match join blocked)";
        else
            note = "joined as spectator (match already finished)";

        return false;
    }

    void removeConnection(int socket)
    {
        std::lock_guard<std::mutex> lock(mtx);
        bool foundConnection = false;

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

        if (phase == GamePhase::Countdown && players.size() < kMinimumPlayersToStart)
        {
            phase = GamePhase::Waiting;
            winnerSymbol = '\0';
            winnerName.clear();
            redrawWaitingSpawnsLocked();
        }

        if (phase == GamePhase::InProgress && alivePlayersLocked() <= 1)
            finishMatchLocked();

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

    void changeDirection(int socket, const std::string &dir)
    {
        std::lock_guard<std::mutex> lock(mtx);

        auto it = players.find(socket);
        if (it == players.end())
            return;

        if (phase != GamePhase::InProgress)
            return;

        Player &p = it->second;
        if (!p.alive)
            return;

        if ((dir == "UP" && p.dir == "DOWN") ||
            (dir == "DOWN" && p.dir == "UP") ||
            (dir == "LEFT" && p.dir == "RIGHT") ||
            (dir == "RIGHT" && p.dir == "LEFT"))
            return;

        p.dir = dir;
    }

    void requestStart(int socket)
    {
        std::lock_guard<std::mutex> lock(mtx);

        bool isConnected = players.find(socket) != players.end() ||
                           spectators.find(socket) != spectators.end();
        if (!isConnected)
            return;

        if (players.size() < kMinimumPlayersToStart)
            return;

        if (phase != GamePhase::Waiting && phase != GamePhase::Finished)
            return;

        redrawWaitingSpawnsLocked();
        startCountdownLocked(std::chrono::steady_clock::now());
    }

    void setPlayerName(int socket, const std::string &requestedName)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = players.find(socket);
        if (it == players.end())
            return;

        std::string sanitized = sanitizePlayerName(requestedName);
        if (sanitized.empty())
            return;

        it->second.name = sanitized;
    }

    void update()
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto now = std::chrono::steady_clock::now();

        if (phase == GamePhase::Waiting)
            return;

        if (phase == GamePhase::Countdown)
        {
            if (players.size() < kMinimumPlayersToStart)
            {
                phase = GamePhase::Waiting;
                winnerSymbol = '\0';
                winnerName.clear();
                redrawWaitingSpawnsLocked();
                return;
            }

            if (now >= countdownEnd)
                startMatchLocked(now);

            return;
        }

        if (phase == GamePhase::InProgress)
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

            if (alivePlayersLocked() <= 1 || now >= matchEnd)
                finishMatchLocked();

            return;
        }
    }

    std::string buildFrameForClient(int socket)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto now = std::chrono::steady_clock::now();

        auto playerIt = players.find(socket);
        bool isPlayer = playerIt != players.end();
        bool alive = isPlayer && playerIt->second.alive;
        char playerSymbol = isPlayer ? playerIt->second.symbol : '\0';
        std::string playerName = isPlayer ? playerIt->second.name : "NONE";

        int remaining = 0;
        if (phase == GamePhase::Countdown)
            remaining = secondsRemaining(countdownEnd, now);
        else if (phase == GamePhase::InProgress)
            remaining = secondsRemaining(matchEnd, now);

        std::ostringstream out;
        out << "META\n";
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
        out << renderGridForClientLocked(socket, now);
        out << "END\n";
        return out.str();
    }

    std::string buildReplicaSnapshot()
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::string out;
        for (int y = 0; y < HEIGHT; y++)
        {
            for (int x = 0; x < WIDTH; x++)
                out.push_back(grid[y][x]);
            out.push_back('\n');
        }
        out += "END\n";
        return out;
    }
};

void broadcastToReplicas(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(replica_mtx);
    
    if (replicaSocket < 0) return;

    if (send(replicaSocket, msg.c_str(), msg.size(), 0) <= 0)
    {
        close(replicaSocket);
        replicaSocket = -1;
        replicaConnected = false;
        replica_cv.notify_all();
    }
}

void removeClientSocket(int socket)
{
    std::lock_guard<std::mutex> lock(client_mtx);
    auto it = std::remove(clients.begin(), clients.end(), socket);
    clients.erase(it, clients.end());
}

void sendFramesToClients(TronGame &game)
{
    std::vector<int> sockets;
    {
        std::lock_guard<std::mutex> lock(client_mtx);
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
    char buffer[1024];

    while (true)
    {
        int bytes = recv(socket, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (bytes > 0)
        {
            std::string input(buffer, bytes);
            std::string commandInput = input;
            size_t namePos = input.find("NAME:");
            if (namePos != std::string::npos)
            {
                size_t valueStart = namePos + 5;
                size_t valueEnd = input.find_first_of("\r\n", valueStart);
                std::string requestedName = input.substr(
                    valueStart,
                    valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);
                game.setPlayerName(socket, requestedName);

                if (valueEnd == std::string::npos)
                {
                    commandInput.erase(namePos);
                }
                else
                {
                    commandInput.erase(namePos, (valueEnd - namePos) + 1);
                }
            }

            if (commandInput.find("START") != std::string::npos)
                game.requestStart(socket);
            else if (commandInput.find("UP") != std::string::npos)
                game.changeDirection(socket, "UP");
            else if (commandInput.find("DOWN") != std::string::npos)
                game.changeDirection(socket, "DOWN");
            else if (commandInput.find("LEFT") != std::string::npos)
                game.changeDirection(socket, "LEFT");
            else if (commandInput.find("RIGHT") != std::string::npos)
                game.changeDirection(socket, "RIGHT");
        }
        else if (bytes == 0)
        {
            break;
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            break;
        }

        std::memset(buffer, 0, sizeof(buffer));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    game.removeConnection(socket);
    removeClientSocket(socket);
}

void gameLoop(TronGame &game)
{
    while (isPrimary)
    {
        game.update();
        sendFramesToClients(game);
        broadcastToReplicas(game.buildReplicaSnapshot());
        std::this_thread::sleep_for(std::chrono::milliseconds(kServerTickMs));
    }
}

struct ServerStartupOptions
{
    std::string host;
    int port = 0;
};

void printServerUsage(const char *programName)
{
    std::cout << "Usage: " << programName
              << " [primary|replica] [--host <sunlab-host>] [--port 6000-6010]\n";
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

        if (arg == "--host")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--host requires a value.\n";
                return false;
            }

            std::string parsedHost = argv[++i];
            if (!isValidSunlabHost(parsedHost))
            {
                std::cerr << "Invalid host '" << parsedHost
                          << "'. Host must be one of the Sunlab nodes.\n";
                return false;
            }

            options.host = normalizeSunlabHost(parsedHost);
            continue;
        }

        if (arg == "--port")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--port requires a value.\n";
                return false;
            }

            int parsedPort = 0;
            if (!parsePortInRange(argv[++i], parsedPort))
            {
                std::cerr << "Invalid port. Use an integer in [" << kMinAllowedPort
                          << "," << kMaxAllowedPort << "].\n";
                return false;
            }

            options.port = parsedPort;
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

    std::string localHost = localHostnameNormalized();
    if (!localHost.empty() && isValidSunlabHost(localHost) && localHost != options.host)
    {
        std::cerr << "Selected host " << toSunlabFqdn(options.host)
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

    TronGame game;
    std::thread(gameLoop, std::ref(game)).detach();
    std::cout << "PRIMARY running on " << toSunlabFqdn(options.host) << " port " << options.port << "\n";
    std::cout << "Waiting for replica\n";

    {
        while (!replicaConnected)
        {
            int clientSocket = accept(serverFd, nullptr, nullptr);
            if (clientSocket < 0) continue;

            char buf[64] = {0};
            ssize_t bytes = recv(clientSocket, buf, sizeof(buf)-1, MSG_PEEK | MSG_DONTWAIT);

            std::string firstMsg(buf, bytes > 0 ? bytes : 0);

            if (firstMsg.find("REPLICA") != std::string::npos)
            {
                // This is the replica
                {
                    std::lock_guard<std::mutex> lock(replica_mtx);
                    replicaSocket = clientSocket;
                    replicaConnected = true;
                }
                std::cout << "Replica connected\n";
                replica_cv.notify_all();
            }
            else
            { // client has connected before replica
                {
                    std::lock_guard<std::mutex> lock(client_mtx);
                    clients.push_back(clientSocket);
                }

                std::string note;
                game.registerConnection(clientSocket, note);
                std::cout << "Client connected: " << note << "\n";

                std::string frame = game.buildFrameForClient(clientSocket);
                if (send(clientSocket, frame.c_str(), frame.size(), 0) > 0)
                {
                    std::thread(clientHandler, std::ref(game), clientSocket).detach();
                }
                else
                {
                    close(clientSocket);
                }
            }
        }
    }

    while (true)
    {
        int clientSocket = accept(serverFd, nullptr, nullptr);
        if (clientSocket < 0)
            continue;

        {
            std::lock_guard<std::mutex> lock(client_mtx);
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
            continue;
        }

        std::thread(clientHandler, std::ref(game), clientSocket).detach();
    }


    close(serverFd);
    return 0;
}
