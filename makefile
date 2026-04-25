CXX = g++
CXXFLAGS = -Wall -std=c++17 -pthread

all: server client launcher

server: single_server.cpp
	$(CXX) $(CXXFLAGS) single_server.cpp -o server 

client: single_client.cpp
	$(CXX) $(CXXFLAGS) single_client.cpp -o client

launcher: launcher.cpp
	$(CXX) $(CXXFLAGS) launcher.cpp -o launcher

clean:
	rm -f server client launcher
