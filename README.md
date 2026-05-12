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

## Replication Plan

This is the design we want to lock in for replication and failover. The current codebase does not fully implement this plan yet.

Core model
1. The game runs on fixed ticks.
2. Every client input is tagged with a `target_tick`.
3. Clients send each input to both the primary and the replica.
4. The primary is authoritative for the current `epoch`.
5. The replica buffers the same inputs and stays warm.
6. The primary sends heartbeat / progress messages that include its current `epoch` and tick progress.
7. If takeover is allowed, the replica becomes leader for `epoch + 1`.
8. Clients discard frames from older epochs and trust the highest epoch.
9. Clients snap to the replica's first authoritative frame after takeover.

Tick definitions
- `published_tick`: the highest tick the primary has already shown to clients.
- `safe_tick`: the highest contiguous tick the replica can definitely reproduce exactly using only data it already has locally.

For a tick to count as safe, the replica must have:
- a known base state from an earlier tick or checkpoint,
- the exact authoritative input bundle for each tick up through that point, or an authoritative snapshot,
- all deterministic metadata needed to replay those ticks exactly,
- no gaps in the tick history.

If tick `19` is missing, then tick `20` and later are not safe even if some later messages arrived.

Takeover rule
1. The replica promotes itself to `epoch + 1`.
2. It resumes from `safe_tick`, not necessarily `published_tick`.
3. It decides the next authoritative ticks from the last safe state using the buffered inputs it already has.
4. Clients drop old-epoch frames and snap to the new leader's authoritative state.

This means failover may cause a visible correction if clients had already rendered ticks beyond `safe_tick`, but the correction is bounded and deterministic.

Determinism requirements
- Fixed tick progression instead of wall-clock-driven game advancement.
- Per-client sequence numbers to deduplicate and order inputs.
- A deterministic input cutoff rule for when an input is considered on-time for a given tick.
- `epoch` and `tick` fields on every authoritative server frame.
- Deterministic handling of randomness such as spawns, either through shared seeds or explicitly replicated decisions.

## Tradeoffs

Why we chose this approach
- Waiting for a replica ACK on every committed tick would put the replica on the critical path of the live game loop.
- Our target tick rate is `150ms`, so even occasional network or scheduling spikes between primary and replica could make the game stall or run slower than the tick rate we want.
- Separating `published_tick` from `safe_tick` keeps the primary responsive while still giving the replica enough information to take over cleanly.

What we are accepting
- `published_tick` can be ahead of `safe_tick`.
- On failover, players may be snapped back to the replica's latest safe state if the primary had already shown newer ticks that the replica cannot reproduce yet.
- We are explicitly choosing bounded rollback / correction over synchronous per-tick commit latency.

What this approach gives us
- Fast normal-case gameplay because the primary does not wait on replica ACKs each tick.
- A hot replica that has recent inputs and progress data already buffered.
- Cleaner client behavior during failover because clients only need to trust the highest `epoch` and accept the new authoritative frame.

Current status
- Right now the replica path is still incomplete in code.
- The existing replica logic only receives limited state and does not yet implement full deterministic replay, `published_tick`, `safe_tick`, epoch-based promotion, or client cutover behavior.
