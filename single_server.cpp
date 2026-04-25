#include <iostream>
#include <unistd.h>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <random>

#define PORT 8080
#define WIDTH 100
#define HEIGHT 50

std::mt19937 rng{std::random_device{}()};
std::uniform_int_distribution<int> distX(1, WIDTH - 2);
std::uniform_int_distribution<int> distY(1, HEIGHT - 2);

struct Player
{
    int id;
    int x;
    int y;
    std::string dir;
    int socket;
    bool alive;
    char symbol;
};

class TronGame
{
private:
    std::vector<std::vector<char>> grid;
    std::unordered_map<int, Player> players; // key = socket
    std::mutex mtx;
    std::atomic<int> nextId{1};

public:
    TronGame()
    {
        grid.resize(HEIGHT, std::vector<char>(WIDTH, '.'));

        // borders
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

    void addPlayer(int socket)
    {
        std::lock_guard<std::mutex> lock(mtx);
        

        Player p;
        p.id = nextId++;
        p.socket = socket;
        p.x = distX(rng);
        p.y = distY(rng);
        p.dir = "RIGHT";
        p.alive = true;
        p.symbol = 'A' + (p.id % 26);

        players[socket] = p;
        grid[p.y][p.x] = p.symbol;

        std::cout << "Player " << p.id << " joined\n";
    }

    void removePlayer(int socket)
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (players.count(socket))
        {
            auto &p = players[socket];
            grid[p.y][p.x] = '#';
            players.erase(socket);
        }

        close(socket);
    }

    void changeDirection(int socket, const std::string &dir)
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (!players.count(socket)) return;

        Player &p = players[socket];

        if ((dir == "UP" && p.dir == "DOWN") ||
            (dir == "DOWN" && p.dir == "UP") ||
            (dir == "LEFT" && p.dir == "RIGHT") ||
            (dir == "RIGHT" && p.dir == "LEFT"))
            return;

        p.dir = dir;
    }

    void moveAll()
    {
        std::lock_guard<std::mutex> lock(mtx);

        for (auto &pair : players)
        {
            Player &p = pair.second;
            if (!p.alive) continue;

            // leave trail
            grid[p.y][p.x] = '#';

            // move
            if (p.dir == "UP") p.y--;
            else if (p.dir == "DOWN") p.y++;
            else if (p.dir == "LEFT") p.x--;
            else if (p.dir == "RIGHT") p.x++;

            // collision
            if (p.x < 0 || p.x >= WIDTH || p.y < 0 || p.y >= HEIGHT ||
                grid[p.y][p.x] == '#')
            {
                p.alive = false;
                continue;
            }

            grid[p.y][p.x] = p.symbol;
        }
    }

    std::string buildGrid()
    {
        std::lock_guard<std::mutex> lock(mtx);

        std::string out;
        for (int y = 0; y < HEIGHT; y++)
        {
            for (int x = 0; x < WIDTH; x++)
                out += grid[y][x];
            out += '\n';
        }

        out += "END\n";
        return out;
    }

    std::vector<int> getSockets()
    {
        std::lock_guard<std::mutex> lock(mtx);

        std::vector<int> sockets;
        for (auto &p : players)
            sockets.push_back(p.first);

        return sockets;
    }
};

// ---------------- SERVER THREADS ----------------

void clientHandler(TronGame &game, int socket)
{
    char buffer[1024];

    while (true)
    {
        int valread = recv(socket, buffer, sizeof(buffer), MSG_DONTWAIT);

        if (valread > 0)
        {
            std::string input(buffer, valread);

            if (input.find("UP") != std::string::npos)
                game.changeDirection(socket, "UP");
            else if (input.find("DOWN") != std::string::npos)
                game.changeDirection(socket, "DOWN");
            else if (input.find("LEFT") != std::string::npos)
                game.changeDirection(socket, "LEFT");
            else if (input.find("RIGHT") != std::string::npos)
                game.changeDirection(socket, "RIGHT");
        }

        memset(buffer, 0, sizeof(buffer));

        if (valread == 0)
        {
            game.removePlayer(socket);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void gameLoop(TronGame &game)
{
    while (true)
    {
        game.moveAll();

        std::string state = game.buildGrid();
        auto sockets = game.getSockets();

        for (int s : sockets)
        {
            send(s, state.c_str(), state.size(), 0);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

// ---------------- MAIN ----------------

int main()
{
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        return 1;
    }

    if (listen(server_fd, 10) < 0)
    {
        perror("listen failed");
        return 1;
    }

    std::cout << "Server running on port " << PORT << "\n";

    TronGame game;

    // Start game loop
    std::thread(gameLoop, std::ref(game)).detach();

    while (true)
    {
        int client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);

        if (client_socket >= 0)
        {
            std::cout << "Client connected\n";

            game.addPlayer(client_socket);

            std::thread(clientHandler, std::ref(game), client_socket).detach();
        }
    }

    close(server_fd);
    return 0;
}