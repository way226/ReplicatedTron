#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <algorithm>

#define PORT 54000
#define BUFFER_SIZE 1024

std::vector<int> clients;
std::mutex clients_mutex;

// Broadcast message to all clients
void broadcast(const std::string& message, int sender_socket) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (int client : clients) {
        if (client != sender_socket) {
            send(client, message.c_str(), message.size(), 0);
        }
    }
}

// Handle individual client
void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];

    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(client_socket, buffer, BUFFER_SIZE, 0);

        if (bytes <= 0) {
            std::cout << "Client disconnected\n";
            close(client_socket);

            clients.erase(std::remove(clients.begin(), clients.end(), client_socket), clients.end());

            break;
        }

        std::string message(buffer);
        std::cout << "Received: " << message << std::endl;

        // Broadcast to other players
        broadcast(message, client_socket);
    }
}

int main() {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr;

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_socket, SOMAXCONN);

    std::cout << "Snake server started on port " << PORT << std::endl;

    while (true) {
        sockaddr_in client_addr;
        socklen_t client_size = sizeof(client_addr);

        int client_socket = accept(server_socket, (sockaddr*)&client_addr, &client_size);

        std::cout << "New client connected\n";

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back(client_socket);
        }

        std::thread(handle_client, client_socket).detach();
    }

    close(server_socket);
    return 0;
}