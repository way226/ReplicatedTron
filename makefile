CXX = g++
CXXFLAGS = -Wall -std=c++17

all: server client

server: single_server.cpp
	$(CXX) $(CXXFLAGS) single_server.cpp -o server

client: single_client.cpp
	$(CXX) $(CXXFLAGS) single_client.cpp -o client

clean:
	rm -f server client