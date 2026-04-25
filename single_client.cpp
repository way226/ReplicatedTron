#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <string>
#include <thread>
#include <termios.h>
#include <unordered_map>
#include <vector>
#include <cctype>
#include <netdb.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <sstream>

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

Direction directionBetween(const Position& from, const Position& to)
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

const std::string& headRuneFor(Direction dir)
{
    if (dir == Direction::Up)
        return playerUpRune;
    if (dir == Direction::Left)
        return playerLeftRune;
    if (dir == Direction::Down)
        return playerDownRune;
    return playerRightRune;
}

const std::string& trailRuneFor(Direction connectionA, Direction connectionB)
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

std::vector<std::string> splitLines(const std::string& text)
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

bool looksLikeGrid(const std::vector<std::string>& rows)
{
    if (rows.empty() || rows.front().empty())
        return false;

    size_t width = rows.front().size();
    for (const std::string& row : rows)
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
    const std::string& rawGrid,
    std::unordered_map<char, PlayerRenderState>& playerStates,
    std::vector<std::vector<std::string>>& frozenTrailRunes)
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

    for (const auto& [playerId, currentHead] : heads)
    {
        PlayerRenderState& state = playerStates[playerId];

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

// get single char input (no enter needed)
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

        if (key == "phase")
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

void printStatusBar(const ServerFrame &frame)
{
    std::cout << "--------------------------------------------------------------------------------\n";
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

    bool moveEnabled = frame.role == "PLAYER" && frame.alive && frame.phase == "IN_PROGRESS";
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
    std::string name;
};

void printClientUsage(const char *programName)
{
    std::cout << "Usage: " << programName
              << " [--host <sunlab-host>] [--port 6000-6010] [--name <player-name>]\n";
}

bool parseClientArgs(int argc, char *argv[], ClientStartupOptions &options)
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
        std::cerr << "getaddrinfo failed for " << host << ":" << port << " (" << gai_strerror(rc) << ")\n";
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

    std::string fqdn = toSunlabFqdn(options.host);
    int sock = connectToHost(fqdn, options.port);
    if (sock < 0)
    {
        std::cerr << "Unable to connect to " << fqdn << ":" << options.port << "\n";
        return 1;
    }

    std::cout << "Connected to " << fqdn << ":" << options.port << "\n";

    send(sock, "CLIENT\n", 7, 0);
    std::string nameMessage = "NAME:" + options.name + "\n";
    send(sock, nameMessage.c_str(), nameMessage.size(), 0);

    std::string buffer = "";
    std::unordered_map<char, PlayerRenderState> playerStates;
    std::vector<std::vector<std::string>> frozenTrailRunes;
    std::atomic<bool> controlsEnabled{false};

    // input thread
    std::thread inputThread([&]()
                            {
        while (true) {
            char c = getch();
            std::string command = mapKey(c);
            if (command.empty())
                continue;

            if (command == "START") {
                send(sock, command.c_str(), command.size(), 0);
                continue;
            }

            if (!controlsEnabled.load())
                continue;

            send(sock, command.c_str(), command.size(), 0);
        } });
    inputThread.detach();

    char recv_buffer[1024];

    while (true)
    {
        int valread = recv(sock, recv_buffer, sizeof(recv_buffer), 0);

        if (valread > 0)
        {
            buffer.append(recv_buffer, valread);

            // check for END delimiter
            size_t pos;
            while ((pos = buffer.find("END\n")) != std::string::npos)
            {
                std::string rawFrame = buffer.substr(0, pos);
                buffer.erase(0, pos + 4);

                ServerFrame frame = parseFrame(rawFrame);
                controlsEnabled.store(frame.role == "PLAYER" &&
                                      frame.alive &&
                                      frame.phase == "IN_PROGRESS");

                std::string rendered = renderGrid(frame.grid, playerStates, frozenTrailRunes);
                std::system("clear");
                std::cout << rendered << std::flush;
                printStatusBar(frame);
            }
        }
        else if (valread == 0)
        {
            std::cout << "Disconnected from server.\n";
            break;
        }
        else
        {
            std::perror("recv");
            break;
        }
    }

    close(sock);
    return 0;
}
