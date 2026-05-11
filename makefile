CXX = g++
CXXFLAGS = -Wall -std=c++17 -pthread

all: server client launcher

server: server.cpp
	$(CXX) $(CXXFLAGS) server.cpp -o server 

client: client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o client

launcher: launcher.cpp
	$(CXX) $(CXXFLAGS) launcher.cpp -o launcher

clean:
	rm -f server client launcher
