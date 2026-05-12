#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>
#include <cstdio>

#include "sunlab_config.h"

enum class LaunchMode
{
    StartServerPrimary,
    StartServerReplica,
    ConnectClient
};

LaunchMode promptForMode()
{
    while (true)
    {
        std::cout << "Choose an option:\n";
        std::cout << "  1) Start a server (primary)\n";
        std::cout << "  2) Start a server (replica)\n";
        std::cout << "  3) Connect as a client\n";
        std::cout << "Selection: ";

        std::string input;
        if (!std::getline(std::cin, input))
        {
            std::cerr << "\nInput closed while reading mode.\n";
            std::exit(1);
        }

        std::string normalized = toLower(trim(input));
        if (normalized == "1" || normalized == "primary")
            return LaunchMode::StartServerPrimary;
        if (normalized == "2" || normalized == "replica")
            return LaunchMode::StartServerReplica;
        if (normalized == "3" || normalized == "connect" || normalized == "client")
            return LaunchMode::ConnectClient;

        std::cout << "Please enter 1, 2, or 3.\n\n";
    }
}

int main()
{
    LaunchMode mode = promptForMode();
    std::string host = promptForSunlabHost();
    int port = promptForPort();
    std::string playerName;

    std::vector<std::string> args;
    if (mode == LaunchMode::StartServerPrimary)
    {
        args = {"./serverPrimary", "--host", host, "--port", std::to_string(port)};
    }
    else if (mode == LaunchMode::StartServerReplica)
    {
        std::cout << "Enter replica server config (this machine):\n";
        std::string this_host = promptForSunlabHost();
        int this_port = promptForPort();
        args = {"./serverReplica", "--p_host", host, "--p_port", std::to_string(port), "--this_host", this_host, "--this_port", std::to_string(this_port)};
    }
    else if (mode == LaunchMode::ConnectClient)
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
    if (mode == LaunchMode::StartServerPrimary)
        std::cout << "primary server";
    else if (mode == LaunchMode::StartServerReplica)
        std::cout << "replica server";
    else if (mode == LaunchMode::ConnectClient)
        std::cout << "client";
    std::cout << " for " << toSunlabFqdn(host) << ":" << port << "\n";
    if (mode == LaunchMode::ConnectClient)
        std::cout << "Player name: " << playerName << "\n";

    execvp(execArgs[0], execArgs.data());
    std::perror("execvp");
    return 1;
}
