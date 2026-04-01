#include <iostream>
#include <unistd.h>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>

#define PORT 8080
#define WIDTH 20
#define HEIGHT 10

struct Player
{
    int x = WIDTH / 2;
    int y = HEIGHT / 2;
    std::string dir = "RIGHT";

    void move()
    {
        if (dir == "UP")
            y--;
        else if (dir == "DOWN")
            y++;
        else if (dir == "LEFT")
            x--;
        else if (dir == "RIGHT")
            x++;

        // clamp to grid
        if (x < 0)
            x = 0;
        if (x >= WIDTH)
            x = WIDTH - 1;
        if (y < 0)
            y = 0;
        if (y >= HEIGHT)
            y = HEIGHT - 1;
    }
};

std::string buildGrid(const Player &p)
{
    std::string grid;

    for (int y = 0; y < HEIGHT; y++)
    {
        for (int x = 0; x < WIDTH; x++)
        {
            if (x == p.x && y == p.y)
                grid += 'A';
            else
                grid += '.';
        }
        grid += '\n';
    }

    grid += "END\n"; // delimiter
    return grid;
}

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

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 1);

    std::cout << "Waiting for client...\n";
    new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
    std::cout << "Client connected!\n";

    Player player;
    char buffer[1024] = {0};

    while (true)
    {
        // non-blocking receive
        int valread = recv(new_socket, buffer, 1024, MSG_DONTWAIT);

        if (valread > 0)
        {
            std::string input(buffer, valread);
            if (input.find("UP") != std::string::npos)
                player.dir = "UP";
            else if (input.find("DOWN") != std::string::npos)
                player.dir = "DOWN";
            else if (input.find("LEFT") != std::string::npos)
                player.dir = "LEFT";
            else if (input.find("RIGHT") != std::string::npos)
                player.dir = "RIGHT";
        }

        memset(buffer, 0, sizeof(buffer));

        // update game
        player.move();

        // send grid
        std::string grid = buildGrid(player);
        send(new_socket, grid.c_str(), grid.size(), 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    close(new_socket);
    close(server_fd);
    return 0;
}