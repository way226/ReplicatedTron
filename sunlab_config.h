#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

constexpr int kMinAllowedPort = 6000;
constexpr int kMaxAllowedPort = 6010;
inline const std::string kSunlabDomain = ".cse.lehigh.edu";

inline const std::array<const char *, 25> kSunlabHosts = {
    "ariel",   "caliban", "callisto", "ceres",  "chiron", "cupid",  "eris",
    "europa",  "hydra",   "iapetus",  "io",     "ixion",  "mars",   "mercury",
    "neptune", "nereid",  "nix",      "orcus",  "phobos", "puck",   "saturn",
    "triton",  "varda",   "vesta",    "xena"};

inline std::string trim(const std::string &input)
{
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])))
        start++;

    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])))
        end--;

    return input.substr(start, end - start);
}

inline std::string toLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return text;
}

inline std::string normalizeSunlabHost(const std::string &hostInput)
{
    std::string host = toLower(trim(hostInput));
    if (host.empty())
        return "";

    if (host.size() > kSunlabDomain.size() &&
        host.compare(host.size() - kSunlabDomain.size(), kSunlabDomain.size(), kSunlabDomain) == 0)
    {
        host = host.substr(0, host.size() - kSunlabDomain.size());
    }

    return host;
}

inline bool isValidSunlabHost(const std::string &hostInput)
{
    std::string host = normalizeSunlabHost(hostInput);
    if (host.empty())
        return false;

    return std::any_of(kSunlabHosts.begin(), kSunlabHosts.end(), [&](const char *candidate)
                       { return host == candidate; });
}

inline bool isLoopbackHost(const std::string &hostInput)
{
    std::string host = toLower(trim(hostInput));
    return host == "localhost" || host == "127.0.0.1";
}

inline bool isValidConfiguredHost(const std::string &hostInput)
{
    return isValidSunlabHost(hostInput) || isLoopbackHost(hostInput);
}

inline std::string normalizeConfiguredHost(const std::string &hostInput)
{
    if (isLoopbackHost(hostInput))
        return toLower(trim(hostInput));
    return normalizeSunlabHost(hostInput);
}

inline std::string toSunlabFqdn(const std::string &hostInput)
{
    std::string host = normalizeSunlabHost(hostInput);
    if (host.empty())
        return "";

    return host + kSunlabDomain;
}

inline std::string toConnectHost(const std::string &hostInput)
{
    if (isLoopbackHost(hostInput))
        return toLower(trim(hostInput));
    return toSunlabFqdn(hostInput);
}

inline bool parsePortInRange(const std::string &portInput, int &portOut)
{
    std::string token = trim(portInput);
    if (token.empty())
        return false;

    for (char c : token)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }

    int parsed = 0;
    try
    {
        parsed = std::stoi(token);
    }
    catch (const std::exception &)
    {
        return false;
    }

    if (parsed < kMinAllowedPort || parsed > kMaxAllowedPort)
        return false;

    portOut = parsed;
    return true;
}

inline void printSunlabHosts()
{
    std::cout << "Available Sunlab hosts:\n";
    for (size_t i = 0; i < kSunlabHosts.size(); i++)
    {
        std::cout << "  " << kSunlabHosts[i] << kSunlabDomain << "\n";
    }
}

inline std::string promptForSunlabHost()
{
    while (true)
    {
        printSunlabHosts();
        std::cout << "Enter host: ";

        std::string candidate;
        if (!std::getline(std::cin, candidate))
        {
            std::cerr << "\nInput closed while reading host.\n";
            std::exit(1);
        }

        if (!isValidSunlabHost(candidate))
        {
            std::cout << "Host must be one of the listed Sunlab nodes.\n\n";
            continue;
        }

        return normalizeSunlabHost(candidate);
    }
}

inline int promptForPort()
{
    while (true)
    {
        std::cout << "Enter port (" << kMinAllowedPort << "-" << kMaxAllowedPort << "): ";
        std::string candidate;
        if (!std::getline(std::cin, candidate))
        {
            std::cerr << "\nInput closed while reading port.\n";
            std::exit(1);
        }

        int parsed = 0;
        if (!parsePortInRange(candidate, parsed))
        {
            std::cout << "Port must be an integer from " << kMinAllowedPort << " to "
                      << kMaxAllowedPort << ".\n\n";
            continue;
        }

        return parsed;
    }
}

inline std::string sanitizePlayerName(const std::string &nameInput)
{
    std::string trimmed = trim(nameInput);
    if (trimmed.empty())
        return "";

    std::string out;
    out.reserve(trimmed.size());

    bool previousWasSpace = false;
    for (char c : trimmed)
    {
        unsigned char uc = static_cast<unsigned char>(c);
        bool allowed = std::isalnum(uc) || c == '_' || c == '-' || c == ' ';
        if (!allowed)
            continue;

        if (c == ' ')
        {
            if (previousWasSpace)
                continue;
            previousWasSpace = true;
        }
        else
        {
            previousWasSpace = false;
        }

        out.push_back(c);
        if (out.size() >= 24)
            break;
    }

    while (!out.empty() && out.back() == ' ')
        out.pop_back();

    return out;
}

inline std::string promptForPlayerName()
{
    while (true)
    {
        std::cout << "Enter player name (1-24 chars, letters/digits/space/_/-): ";

        std::string candidate;
        if (!std::getline(std::cin, candidate))
        {
            std::cerr << "\nInput closed while reading player name.\n";
            std::exit(1);
        }

        std::string sanitized = sanitizePlayerName(candidate);
        if (sanitized.empty())
        {
            std::cout << "Invalid name. Use letters, digits, spaces, '_' or '-'.\n\n";
            continue;
        }

        return sanitized;
    }
}
