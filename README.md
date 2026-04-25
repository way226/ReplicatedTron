# ReplicatedTron

How to run (interactive)
1. Build with `make`
2. Start with `./launcher`
3. Choose:
   - `1` to start a server, or
   - `2` to connect as a client
4. Enter a Sunlab node host (`ariel ... xena`) and port (`6000` to `6010`)
5. If joining as a client, enter a player name for winner/status messages

Direct run options
1. Start a server with `./server [primary|replica] --host <node> --port <6000-6010>`
2. Start a client with `./client --host <node> --port <6000-6010> [--name <player-name>]`

Gameplay flow
1. Lobby waits for a manual start command from any connected client (`R` key).
2. Current test setup uses a 1-player minimum to allow quick solo starts.
3. Countdown starts (5s); each player sees only their own spawn point flashing.
4. Match starts and runs with a timer.
5. Mid-match joins are blocked from playing and placed into `SPECTATING` mode.
6. Client UI status bar shows role, alive/dead state, phase, timer, and winner.

Right now the replica just recieves the current game state. There is no ownership/leader transfer and the replica does not perform any calculations or logic.
