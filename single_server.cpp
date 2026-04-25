#include <iostream>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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

// =====================================================
// MODE FLAG
// =====================================================
bool isPrimary = true;

// =====================================================
// REPLICAS
// =====================================================
std::vector<int> replicas;
std::mutex replica_mtx;

// =====================================================
// CLIENTS (FIX: REQUIRED)
// =====================================================
std::vector<int> clients;
std::mutex client_mtx;

// =====================================================
// RANDOM
// =====================================================
std::mt19937 rng{std::random_device{}()};
std::uniform_int_distribution<int> distX(1, WIDTH - 2);
std::uniform_int_distribution<int> distY(1, HEIGHT - 2);

// =====================================================
// PLAYER
// =====================================================
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

// =====================================================
// GAME STATE
// =====================================================
class TronGame
{
private:
    std::vector<std::vector<char>> grid;
    std::unordered_map<int, Player> players;
    std::mutex mtx;
    std::atomic<int> nextId{1};

public:
    TronGame()
    {
        grid.resize(HEIGHT, std::vector<char>(WIDTH, '.'));

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

        if (!players.count(socket))
            return;

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
            if (!p.alive)
                continue;

            grid[p.y][p.x] = '#';

            if (p.dir == "UP")
                p.y--;
            else if (p.dir == "DOWN")
                p.y++;
            else if (p.dir == "LEFT")
                p.x--;
            else if (p.dir == "RIGHT")
                p.x++;

            if (p.x < 0 || p.x >= WIDTH ||
                p.y < 0 || p.y >= HEIGHT ||
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
};

// =====================================================
// BROADCAST REPLICAS
// =====================================================
void broadcastToReplicas(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(replica_mtx);

    for (auto it = replicas.begin(); it != replicas.end();)
    {
        if (send(*it, msg.c_str(), msg.size(), 0) <= 0)
        {
            close(*it);
            it = replicas.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// =====================================================
// BROADCAST CLIENTS (FIX)
// =====================================================
void broadcastToClients(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(client_mtx);

    for (auto it = clients.begin(); it != clients.end();)
    {
        if (send(*it, msg.c_str(), msg.size(), 0) <= 0)
        {
            close(*it);
            it = clients.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// =====================================================
// CLIENT HANDLER
// =====================================================
void clientHandler(TronGame &game, int socket)
{
    char buffer[1024];

    while (true)
    {
        int n = recv(socket, buffer, sizeof(buffer), MSG_DONTWAIT);

        if (n > 0)
        {
            std::string input(buffer, n);

            if (input.find("UP") != std::string::npos)
                game.changeDirection(socket, "UP");
            else if (input.find("DOWN") != std::string::npos)
                game.changeDirection(socket, "DOWN");
            else if (input.find("LEFT") != std::string::npos)
                game.changeDirection(socket, "LEFT");
            else if (input.find("RIGHT") != std::string::npos)
                game.changeDirection(socket, "RIGHT");
        }

        if (n == 0)
        {
            game.removePlayer(socket);
            break;
        }

        memset(buffer, 0, sizeof(buffer));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

// =====================================================
// GAME LOOP (FIXED)
// =====================================================
void gameLoop(TronGame &game)
{
    while (isPrimary)
    {
        game.moveAll();

        std::string state = game.buildGrid();

        // ✅ SEND TO CLIENTS (FIX)
        broadcastToClients(state);

        // ✅ SEND TO REPLICAS
        broadcastToReplicas(state);

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

// =====================================================
// MAIN
// =====================================================
int main(int argc, char *argv[])
{
    if (argc > 1 && std::string(argv[1]) == "replica")
        isPrimary = false;

    int server_fd;
    sockaddr_in address{};
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    address.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr *)&address, sizeof(address));
    listen(server_fd, 10);

    std::cout << (isPrimary ? "PRIMARY" : "REPLICA")
              << " running on port " << PORT << "\n";

    TronGame game;

    if (isPrimary)
    {
        std::thread(gameLoop, std::ref(game)).detach();

        while (true)
        {
            int client_socket = accept(server_fd, nullptr, nullptr);

            if (client_socket >= 0)
            {
                std::cout << "Client connected\n";

                game.addPlayer(client_socket);

                // initial sync
                std::string init = game.buildGrid();
                send(client_socket, init.c_str(), init.size(), 0);

                {
                    std::lock_guard<std::mutex> lock(client_mtx);
                    clients.push_back(client_socket);
                }

                std::thread(clientHandler, std::ref(game), client_socket).detach();
            }
        }
    }
    else
    {
        while (true)
        {
            int sock = accept(server_fd, nullptr, nullptr);

            if (sock >= 0)
            {
                std::lock_guard<std::mutex> lock(replica_mtx);
                replicas.push_back(sock);

                std::cout << "Replica connected\n";
            }
        }
    }

    close(server_fd);
    return 0;
}