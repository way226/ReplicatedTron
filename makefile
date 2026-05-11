CXX = g++
CXXFLAGS = -Wall -std=c++17 -pthread

all: serverPrimary serverReplica client launcher

serverPrimary: serverPrimary.cpp
	$(CXX) $(CXXFLAGS) serverPrimary.cpp -o serverPrimary

serverReplica: serverReplica.cpp
	$(CXX) $(CXXFLAGS) serverReplica.cpp -o serverReplica

client: client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o client

launcher: launcher.cpp
	$(CXX) $(CXXFLAGS) launcher.cpp -o launcher

clean:
	rm -f serverPrimary serverReplica client launcher
