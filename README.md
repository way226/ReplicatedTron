# ReplicatedTron

How to run servers 
1. Start a server with ./server (or ./server primary)
2. Create a separate terminal
3. Start a replica server with ./server replica

How to run clients
1. Start a client with ./client

Right now the replica just recieves the current game state. There is no ownership/leader transfer and the replica does not perform any calculations or logic.