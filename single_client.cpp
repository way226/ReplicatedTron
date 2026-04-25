#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <string>
#include <thread>
#include <termios.h>
#include <unordered_map>
#include <vector>
#include <cctype>

#define PORT 8080

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
    return "";
}

int main()
{
    int sock = 0;
    struct sockaddr_in serv_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    send(sock, "CLIENT\n", 7, 0);

    std::string buffer = "";
    std::unordered_map<char, PlayerRenderState> playerStates;
    std::vector<std::vector<std::string>> frozenTrailRunes;

    // input thread
    std::thread inputThread([&]()
                            {
        while (true) {
            char c = getch();
            std::string dir = mapKey(c);
            if (!dir.empty()) {
                send(sock, dir.c_str(), dir.size(), 0);
            }
        } });

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
                std::string grid = buffer.substr(0, pos);
                buffer.erase(0, pos + 4);

                std::string rendered = renderGrid(grid, playerStates, frozenTrailRunes);
                system("clear");
                std::cout << rendered << std::flush;
            }
        }
    }

    inputThread.join();
    close(sock);
    return 0;
}
