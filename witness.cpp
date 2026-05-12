#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "replication_protocol.h"
#include "sunlab_config.h"

struct WitnessState
{
    std::mutex mtx;
    std::uint64_t epoch = 0;
    std::string owner;
    std::chrono::steady_clock::time_point leaseExpiry = std::chrono::steady_clock::time_point::min();

    bool leaseActiveLocked() const
    {
        return !owner.empty() && std::chrono::steady_clock::now() < leaseExpiry;
    }

    int ttlMsLocked() const
    {
        if (!leaseActiveLocked())
            return 0;

        auto delta = leaseExpiry - std::chrono::steady_clock::now();
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
    }
};

std::string formatReply(const std::string &verb,
                        std::uint64_t epoch,
                        const std::string &owner,
                        int ttlMs)
{
    std::ostringstream out;
    out << verb << "|" << epoch << "|" << protocolEscape(owner.empty() ? "NONE" : owner)
        << "|" << ttlMs << "\n";
    return out.str();
}

std::string handleRequest(WitnessState &state, const std::string &line)
{
    std::vector<std::string> parts = protocolSplit(line, '|');
    if (parts.empty())
        return formatReply("DENY", 0, "", 0);

    std::lock_guard<std::mutex> lock(state.mtx);
    bool leaseActive = state.leaseActiveLocked();

    if (parts[0] == "PING")
        return formatReply("PONG", state.epoch, state.owner, state.ttlMsLocked());

    if (parts[0] == "ACQUIRE" && parts.size() == 4)
    {
        std::string requester = protocolUnescape(parts[1]);
        std::uint64_t requestedEpoch = static_cast<std::uint64_t>(std::stoull(parts[2]));
        int leaseMs = std::atoi(parts[3].c_str());

        if (!leaseActive || state.owner == requester)
        {
            if (state.owner != requester && requestedEpoch <= state.epoch)
                requestedEpoch = state.epoch + 1;
            if (requestedEpoch == 0)
                requestedEpoch = 1;

            state.owner = requester;
            state.epoch = requestedEpoch;
            state.leaseExpiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(leaseMs);
            return formatReply("OK", state.epoch, state.owner, state.ttlMsLocked());
        }

        return formatReply("DENY", state.epoch, state.owner, state.ttlMsLocked());
    }

    if (parts[0] == "RENEW" && parts.size() == 4)
    {
        std::string requester = protocolUnescape(parts[1]);
        std::uint64_t requestedEpoch = static_cast<std::uint64_t>(std::stoull(parts[2]));
        int leaseMs = std::atoi(parts[3].c_str());

        if (!state.owner.empty() && state.owner == requester && state.epoch == requestedEpoch)
        {
            state.leaseExpiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(leaseMs);
            return formatReply("OK", state.epoch, state.owner, state.ttlMsLocked());
        }

        return formatReply("DENY", state.epoch, state.owner, state.ttlMsLocked());
    }

    return formatReply("DENY", state.epoch, state.owner, state.ttlMsLocked());
}

struct WitnessStartupOptions
{
    std::string host;
    int port = 0;
};

void printWitnessUsage(const char *programName)
{
    std::cout << "Usage: " << programName
              << " [--host <sunlab-host>] [--port 6000-6010]\n";
}

std::string localHostnameNormalized()
{
    char hostBuffer[256]{0};
    if (gethostname(hostBuffer, sizeof(hostBuffer) - 1) != 0)
        return "";
    hostBuffer[sizeof(hostBuffer) - 1] = '\0';
    return normalizeSunlabHost(hostBuffer);
}

bool parseWitnessArgs(int argc, char *argv[], WitnessStartupOptions &options)
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
            if (!isValidConfiguredHost(parsedHost))
            {
                std::cerr << "Invalid host '" << parsedHost << "'.\n";
                return false;
            }

            options.host = normalizeConfiguredHost(parsedHost);
            continue;
        }

        if (arg == "--port")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--port requires a value.\n";
                return false;
            }

            if (!parsePortInRange(argv[++i], options.port))
            {
                std::cerr << "Invalid port.\n";
                return false;
            }
            continue;
        }

        if (arg == "--help" || arg == "-h")
        {
            printWitnessUsage(argv[0]);
            std::exit(0);
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
    WitnessStartupOptions options;
    if (!parseWitnessArgs(argc, argv, options))
    {
        printWitnessUsage(argv[0]);
        return 1;
    }

    if (options.host.empty())
        options.host = promptForSunlabHost();
    if (options.port == 0)
        options.port = promptForPort();

    std::string localHost = localHostnameNormalized();
    if (!localHost.empty() && isValidSunlabHost(localHost) && localHost != options.host)
    {
        std::cerr << "Selected host " << toConnectHost(options.host)
                  << " does not match this machine (" << toSunlabFqdn(localHost)
                  << "). SSH into the selected node before starting the witness.\n";
        return 1;
    }

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0)
    {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0)
    {
        std::perror("setsockopt");
        close(serverFd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    address.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    {
        std::perror("bind");
        close(serverFd);
        return 1;
    }

    if (listen(serverFd, 20) != 0)
    {
        std::perror("listen");
        close(serverFd);
        return 1;
    }

    WitnessState state;
    std::cout << "WITNESS running on " << toConnectHost(options.host)
              << " port " << options.port << "\n";

    while (true)
    {
        int clientSocket = accept(serverFd, nullptr, nullptr);
        if (clientSocket < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        std::thread([&state](int socket)
                    {
            std::string request;
            char buffer[256];

            while (true)
            {
                int bytes = recv(socket, buffer, sizeof(buffer), 0);
                if (bytes <= 0)
                    break;

                request.append(buffer, bytes);
                size_t newline = request.find('\n');
                if (newline == std::string::npos)
                    continue;

                std::string line = request.substr(0, newline);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                std::string reply = handleRequest(state, line);
                send(socket, reply.c_str(), reply.size(), 0);
                break;
            }

            close(socket); }, clientSocket)
            .detach();
    }

    close(serverFd);
    return 0;
}
