#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "sunlab_config.h"

enum class LaunchMode
{
    StartWitness,
    StartServerPrimary,
    StartServerReplica,
    ConnectClient
};

LaunchMode promptForMode()
{
    while (true)
    {
        std::cout << "Choose an option:\n";
        std::cout << "  1) Start witness\n";
        std::cout << "  2) Start a server (primary)\n";
        std::cout << "  3) Start a server (replica)\n";
        std::cout << "  4) Connect as a client\n";
        std::cout << "Selection: ";

        std::string input;
        if (!std::getline(std::cin, input))
        {
            std::cerr << "\nInput closed while reading mode.\n";
            std::exit(1);
        }

        std::string normalized = toLower(trim(input));
        if (normalized == "1" || normalized == "witness")
            return LaunchMode::StartWitness;
        if (normalized == "2" || normalized == "primary")
            return LaunchMode::StartServerPrimary;
        if (normalized == "3" || normalized == "replica")
            return LaunchMode::StartServerReplica;
        if (normalized == "4" || normalized == "connect" || normalized == "client")
            return LaunchMode::ConnectClient;

        std::cout << "Please enter 1, 2, 3, or 4.\n\n";
    }
}

bool promptForOptionalSunlabHost(std::string &hostOut)
{
    std::cout << "Optional standby replica host (press Enter to skip): ";
    std::string candidate;
    if (!std::getline(std::cin, candidate))
    {
        std::cerr << "\nInput closed while reading optional host.\n";
        std::exit(1);
    }

    candidate = trim(candidate);
    if (candidate.empty())
        return false;

    if (!isValidSunlabHost(candidate))
    {
        std::cout << "Host must be one of the listed Sunlab nodes.\n";
        return promptForOptionalSunlabHost(hostOut);
    }

    hostOut = normalizeSunlabHost(candidate);
    return true;
}

int main()
{
    LaunchMode mode = promptForMode();
    std::vector<std::string> args;

    if (mode == LaunchMode::StartWitness)
    {
        std::string host = promptForSunlabHost();
        int port = promptForPort();
        args = {"./witness", "--host", host, "--port", std::to_string(port)};
        std::cout << "Launching witness on " << toSunlabFqdn(host) << ":" << port << "\n";
    }
    else if (mode == LaunchMode::StartServerPrimary)
    {
        std::string host = promptForSunlabHost();
        int port = promptForPort();
        std::cout << "Enter witness config:\n";
        std::string witnessHost = promptForSunlabHost();
        int witnessPort = promptForPort();

        args = {"./serverPrimary",
                "--host", host,
                "--port", std::to_string(port),
                "--w_host", witnessHost,
                "--w_port", std::to_string(witnessPort)};

        std::cout << "Launching primary server on " << toSunlabFqdn(host) << ":" << port << "\n";
        std::cout << "Witness: " << toSunlabFqdn(witnessHost) << ":" << witnessPort << "\n";
    }
    else if (mode == LaunchMode::StartServerReplica)
    {
        std::cout << "Enter primary server config:\n";
        std::string primaryHost = promptForSunlabHost();
        int primaryPort = promptForPort();
        std::cout << "Enter replica server config (this machine):\n";
        std::string thisHost = promptForSunlabHost();
        int thisPort = promptForPort();
        std::cout << "Enter witness config:\n";
        std::string witnessHost = promptForSunlabHost();
        int witnessPort = promptForPort();

        args = {"./serverReplica",
                "--p_host", primaryHost,
                "--p_port", std::to_string(primaryPort),
                "--this_host", thisHost,
                "--this_port", std::to_string(thisPort),
                "--w_host", witnessHost,
                "--w_port", std::to_string(witnessPort)};

        std::cout << "Launching replica server on " << toSunlabFqdn(thisHost) << ":" << thisPort << "\n";
        std::cout << "Primary: " << toSunlabFqdn(primaryHost) << ":" << primaryPort << "\n";
        std::cout << "Witness: " << toSunlabFqdn(witnessHost) << ":" << witnessPort << "\n";
    }
    else
    {
        std::cout << "Enter primary server config:\n";
        std::string host = promptForSunlabHost();
        int port = promptForPort();
        std::string playerName = promptForPlayerName();

        args = {"./client", "--host", host, "--port", std::to_string(port), "--name", playerName};

        std::string replicaHost;
        if (promptForOptionalSunlabHost(replicaHost))
        {
            int replicaPort = promptForPort();
            args.push_back("--replica_host");
            args.push_back(replicaHost);
            args.push_back("--replica_port");
            args.push_back(std::to_string(replicaPort));
            std::cout << "Standby replica: " << toSunlabFqdn(replicaHost) << ":" << replicaPort << "\n";
        }

        std::cout << "Launching client for primary " << toSunlabFqdn(host) << ":" << port << "\n";
        std::cout << "Player name: " << playerName << "\n";
    }

    std::vector<char *> execArgs;
    execArgs.reserve(args.size() + 1);
    for (std::string &arg : args)
        execArgs.push_back(arg.data());
    execArgs.push_back(nullptr);

    execvp(execArgs[0], execArgs.data());
    std::perror("execvp");
    return 1;
}
