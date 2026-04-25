#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>
#include <cstdio>

#include "sunlab_config.h"

enum class LaunchMode
{
    StartServer,
    ConnectClient
};

LaunchMode promptForMode()
{
    while (true)
    {
        std::cout << "Choose an option:\n";
        std::cout << "  1) Start a server\n";
        std::cout << "  2) Connect as a client\n";
        std::cout << "Selection: ";

        std::string input;
        if (!std::getline(std::cin, input))
        {
            std::cerr << "\nInput closed while reading mode.\n";
            std::exit(1);
        }

        std::string normalized = toLower(trim(input));
        if (normalized == "1" || normalized == "start" || normalized == "server")
            return LaunchMode::StartServer;
        if (normalized == "2" || normalized == "connect" || normalized == "client")
            return LaunchMode::ConnectClient;

        std::cout << "Please enter 1 or 2.\n\n";
    }
}

int main()
{
    LaunchMode mode = promptForMode();
    std::string host = promptForSunlabHost();
    int port = promptForPort();
    std::string playerName;

    std::vector<std::string> args;
    if (mode == LaunchMode::StartServer)
    {
        args = {"./server", "--host", host, "--port", std::to_string(port)};
    }
    else
    {
        playerName = promptForPlayerName();
        args = {"./client", "--host", host, "--port", std::to_string(port), "--name", playerName};
    }

    std::vector<char *> execArgs;
    execArgs.reserve(args.size() + 1);
    for (std::string &arg : args)
        execArgs.push_back(arg.data());
    execArgs.push_back(nullptr);

    std::cout << "Launching ";
    if (mode == LaunchMode::StartServer)
        std::cout << "server";
    else
        std::cout << "client";
    std::cout << " for " << toSunlabFqdn(host) << ":" << port << "\n";
    if (mode == LaunchMode::ConnectClient)
        std::cout << "Player name: " << playerName << "\n";

    execvp(execArgs[0], execArgs.data());
    std::perror("execvp");
    return 1;
}
