#include <iostream>
#include <unistd.h>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>

#define PORT 8080
#define WIDTH 20
#define HEIGHT 10

struct Player
{
    int x = WIDTH / 2;
    int y = HEIGHT / 2;
    std::string dir = "RIGHT";
};

class TronGame
{
private:
    std::vector<std::vector<char>> grid;  // '#' = wall/trail, '.' = empty, 'A' = player
    Player player;

public:
    TronGame()
    {
        grid.resize(HEIGHT, std::vector<char>(WIDTH, '.'));
        
        // Optional: add some border walls (makes it more "arena" like)
        for (int x = 0; x < WIDTH; x++) {
            grid[0][x] = '#';
            grid[HEIGHT-1][x] = '#';
        }
        for (int y = 0; y < HEIGHT; y++) {
            grid[y][0] = '#';
            grid[y][WIDTH-1] = '#';
        }

        // Place player in the middle (clear the border if needed)
        player.x = WIDTH / 2;
        player.y = HEIGHT / 2;
        grid[player.y][player.x] = 'A';
    }

    bool move()
    {
        // Clear current player position (will be replaced by trail)
        grid[player.y][player.x] = '#';   // Leave trail behind

        // Move player
        if (player.dir == "UP")    player.y--;
        else if (player.dir == "DOWN")  player.y++;
        else if (player.dir == "LEFT")  player.x--;
        else if (player.dir == "RIGHT") player.x++;

        // Check wall collision (including borders)
        if (player.x < 0 || player.x >= WIDTH || player.y < 0 || player.y >= HEIGHT)
            return false;

        // Check trail collision
        if (grid[player.y][player.x] == '#')
            return false;

        // Place player at new position
        grid[player.y][player.x] = 'A';

        return true;  // move successful
    }

    void changeDirection(const std::string& newDir)
    {
        // Simple prevention of 180° turns (optional but feels better)
        if ((newDir == "UP" && player.dir == "DOWN") ||
            (newDir == "DOWN" && player.dir == "UP") ||
            (newDir == "LEFT" && player.dir == "RIGHT") ||
            (newDir == "RIGHT" && player.dir == "LEFT"))
            return;

        player.dir = newDir;
    }

    std::string buildGrid() const
    {
        std::string output;
        for (int y = 0; y < HEIGHT; y++)
        {
            for (int x = 0; x < WIDTH; x++)
            {
                output += grid[y][x];
            }
            output += '\n';
        }
        output += "END\n";  // delimiter for client
        return output;
    }

    std::string getGameOverMessage() const
    {
        return "GAME OVER\nYou crashed into a wall or your own trail!\nEND\n";
    }
};

int main()
{
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return 1;
    }
    if (listen(server_fd, 1) < 0) {
        perror("listen failed");
        return 1;
    }

    std::cout << "Waiting for client on port " << PORT << "...\n";
    new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
    std::cout << "Client connected!\n";

    TronGame game;
    char buffer[1024] = {0};

    while (true)
    {
        // Non-blocking receive for direction input
        int valread = recv(new_socket, buffer, 1024, MSG_DONTWAIT);
        if (valread > 0)
        {
            std::string input(buffer, valread);
            if (input.find("UP") != std::string::npos)
                game.changeDirection("UP");
            else if (input.find("DOWN") != std::string::npos)
                game.changeDirection("DOWN");
            else if (input.find("LEFT") != std::string::npos)
                game.changeDirection("LEFT");
            else if (input.find("RIGHT") != std::string::npos)
                game.changeDirection("RIGHT");
        }
        memset(buffer, 0, sizeof(buffer));

        // Try to move
        if (!game.move())
        {
            // Game Over
            std::string msg = game.getGameOverMessage();
            send(new_socket, msg.c_str(), msg.size(), 0);
            std::cout << "Game Over! Client disconnected.\n";
            break;
        }

        // Send current grid state
        std::string gridStr = game.buildGrid();
        send(new_socket, gridStr.c_str(), gridStr.size(), 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(150));  // Game speed
    }

    close(new_socket);
    close(server_fd);
    return 0;
}