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

Failure detection / split-brain prevention
1. We will use a lightweight witness service on a third node to manage leader leases and monotonic epoch numbers.
2. The witness does not carry game state and does not participate in per-tick commit decisions. Its only job is to say which server currently owns the lease for the latest epoch.
3. Before serving clients as leader, the primary must hold the witness lease for the current epoch.
4. The primary renews that lease periodically while also sending heartbeats to the replica.
5. Both servers also watch witness health. A server treats the witness as unavailable after `3` consecutive failed witness renew / ping attempts, or roughly `450ms` with the current `150ms` tick rate.
6. The replica marks the primary as suspected after multiple missed heartbeats. A good starting rule for this project is also `3` missed heartbeats, which is about `450ms` at the current tick rate.
7. Suspicion alone is not enough to promote. The replica may only take over if it successfully acquires a new witness lease for `epoch + 1`.
8. If the witness becomes unavailable, the current leader keeps serving in degraded mode under its current epoch, but no automatic promotion is allowed until witness connectivity returns.
9. If any server sees a message from a higher epoch than its own, it must step down and treat itself as stale.

This is how we avoid split brain. Heartbeats detect that something may be wrong, but only the witness lease allows a server to become the new authority. If the witness is unreachable, we treat that as witness failure / unavailability from our point of view.

Failure tolerance note
- This design still tolerates any one failure.
- If the replica fails, the primary keeps running.
- If the primary fails while the witness is healthy, the replica can promote.
- If the witness fails while a leader is healthy, the current leader keeps running in degraded mode.
- If the witness is unavailable and then the current leader also fails, automatic failover is blocked. That is a second failure, and this design does not try to survive it.

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
1. After heartbeat suspicion, the replica asks the witness for the next lease.
2. If the witness grants the new lease, the replica promotes itself to `epoch + 1`.
3. If the witness does not grant the lease, the replica stays non-authoritative.
4. The new leader resumes from `safe_tick`, not necessarily `published_tick`.
5. It decides the next authoritative ticks from the last safe state using the buffered inputs it already has.
6. Clients drop old-epoch frames and snap to the new leader's authoritative state.

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
- We are adding a small third dependency: the witness service. If the witness is unavailable for too long, safe promotion is blocked, but the current leader is allowed to continue serving in degraded mode.

What this approach gives us
- Fast normal-case gameplay because the primary does not wait on replica ACKs each tick.
- A hot replica that has recent inputs and progress data already buffered.
- Cleaner client behavior during failover because clients only need to trust the highest `epoch` and accept the new authoritative frame.
- A concrete promotion rule that avoids two servers becoming leader at the same time.

Evaluation note
- One of our main performance evaluations should log and graph per-tick lateness over time using `tick_lateness(n) = max(0, tick_publish_at(n) - tick_due_at(n))`.

Current status
- Right now the replica path is still incomplete in code.
- The existing replica logic only receives limited state and does not yet implement full deterministic replay, `published_tick`, `safe_tick`, epoch-based promotion, or client cutover behavior.
