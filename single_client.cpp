#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <string>
#include <thread>
#include <termios.h>

#define PORT 8080

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

    std::string buffer = "";

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
        int valread = recv(sock, recv_buffer, 1024, 0);

        if (valread > 0)
        {
            buffer.append(recv_buffer, valread);

            // check for END delimiter
            size_t pos;
            while ((pos = buffer.find("END\n")) != std::string::npos)
            {
                std::string grid = buffer.substr(0, pos);
                buffer.erase(0, pos + 4);

                system("clear");
                std::cout << grid << std::flush;
            }
        }
    }

    inputThread.join();
    close(sock);
    return 0;
}