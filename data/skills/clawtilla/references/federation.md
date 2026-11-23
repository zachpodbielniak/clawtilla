# Federation: linking fleets

Read this before linking this fleet with another clawtilla fleet, changing what a linked fleet may do, or diagnosing a link. Full text: `docs/federation.org` in the checkout.

## What it is

- Each fleet has an **Ed25519 key** (`<secrets.dir>/federation/fleet.ed25519`, 0600). Its **fleet id** is the fingerprint of the *first* key (26 base32 characters) and never changes, even after `rotate-key`.
- **Safety words** (four words from the fleet id) are what two people read to each other while one accepts the other's invite.
- A **peer** is a linked fleet, known here by an **alias** (a valid id, never renamed). Peers live in `<state_dir>/federation/peers.yaml`, written by the daemon. Do not edit it; use `clawtilla federation` or a client's Settings → Federation page.
- **Every request and every answer between fleets is a signed envelope.** That holds over any transport. Kind, from, to, time, nonce, body digest, and what it was sent to are all signed. Answers are bound to the request's nonce.
- What crosses: hello, ping, revoke, rotate (0.66.0); rosters, pushed live (0.67.0); messages and their delivery states (0.68.0); delegated tasks, their states and cancels (0.69.0); transcripts and shared rooms (0.70.0); `whois` for discovery (0.71.0); signed room posts and mentions (0.72.0). A kind the other side does not know is answered 501 "not understood yet", naming its version.

## Turn it on

```yaml
federation:
  enabled: true        # needs a daemon restart
  name: "Lab"          # what other fleets show
  owner: "Alice"
```

Then `clawtilla federation status` shows the fleet id, safety words and the address invites will name.

## Choose a transport

| Endpoint | Pipe | When |
|---|---|---|
| `clawtilla://HOST:PORT` | the other daemon's **own IPC port** (the "simple daemon connection") | tailnet or LAN, nothing extra to run. Needs `federation.ipc: true` (the default) and the daemon listening there (`daemon.tailscale` / `daemon.tcp_address`). |
| `https://host` | HTTPS to the federation listener | Cloudflare Tunnel or a TLS proxy |
| `http://HOST:PORT` | plain HTTP to the listener (`federation.listen`) | tailnet; LAN only with `allow_lan_plaintext` |
| `unix:/path` / `unix:/path#ipc` | listener socket / a daemon's own socket | same machine |

Plain-text rules, enforced at link and listen time:

- Loopback, a unix socket, the tailnet (100.64.0.0/10, fd7a:115c:a1e0::/48, `*.ts.net`): allowed.
- LAN addresses and unknown names: only with `federation.allow_lan_plaintext: true`. Bodies are then sealed (`federation.seal: auto`).
- Public addresses: refused unless TLS.

A `clawtilla://` link to a daemon with `daemon.tls_cert` speaks TLS. The invite carries the certificate's SHA-256 pin, and the accepter pins it.

## Tiers

`owned`, `trusted`, `limited`, `untrusted`. A tier presets a **grant**. Print the presets with `clawtilla federation tiers`.

| | owned | trusted | limited | untrusted |
|---|---|---|---|---|
| reach | all | all | listed + inbox | inbox only |
| message / delegate | allow / allow | allow / allow | allow / ask | review / deny |
| writes, commands | allow | ask | ask / deny | deny |
| ¢/day, msgs/h | no limit | 2000, 120 | 500, 30 | 100, 10 |

- Grant fields (exact names): `reach`, `agents`, `message`, `delegate`, `see_roster`, `see_presence`, `see_typing`, `see_tasks`, `transcripts`, `budget_cents_per_day`, `messages_per_hour`, `max_concurrent_tasks`, `max_hops_in`, `subdelegate`, `join_rooms`, and one `cap.CLASS` per action class: `cap.read`, `cap.write`, `cap.delete`, `cap.message`, `cap.exec`, `cap.external`, `cap.desktop_input`, `cap.fleet_admin`.
- `cap.fleet_admin` is always `deny`.
- 0 in a limit means no limit.
- The inbox agent is `federation.inbox_agent`, the chief of staff by default.

How the grant is built: tier preset → `federation.rules` in order → the peer's overrides. `clawtilla federation explain ALIAS` shows each field's value and source (`tier trusted`, `rule 2: tag:acme`, `override`, `fixed`). Each side enforces its own grant. What a peer says it allows you is shown, never trusted.

## Selectors, tags and rules

- **Peer selector terms:** `*`, `name:GLOB`, `tier:T`, `owner:GLOB`, `tag:TAG`, `health:H`, `online`. Use `!` for not, `+` for and, `,` for or. Case-insensitive. A malformed selector is refused, naming the term.
- **Agent selectors** (the `agents` field): an id glob, `team:NAME`, or `tag:TAG`.
- **Tags** are yours, never sent to the peer: 1–64 characters, no spaces and no `, + ! :`.
- **Rules:**

  ```yaml
  federation:
    rules:
      - "tag:client-acme: reach=listed; agents=team:frontend,atlas; cap.exec=deny"
  ```

  A rule that does not parse is skipped whole and listed under the peer's problems.

## Recipes

**Two fleets you own:**

```bash
clawtilla federation link /run/user/1000/other/daemon.sock --alias homelab     # same machine
clawtilla federation link 100.64.0.7:8792 --alias homelab --tag gpu            # tailnet; asks for that daemon's owner token (`clawtilla daemon token` there)
```

Both sides get tier `owned`. The token is used once and not kept. The address you named becomes the link's endpoint.

**Anyone else:**

```bash
clawtilla federation invite --tier limited [--confirm] [--endpoint URL] [--note "for Dana"] [--qr]
# send the clawt-fed1-... string; it is a one-use secret (federation.invite_ttl_hours)
clawtilla federation inspect clawt-fed1-...                                  # on their side: read it first
clawtilla federation accept clawt-fed1-... --tier trusted --alias lab --tag research
```

- The accepter's daemon checks that the answering fleet's key matches the invite's fingerprint. On a mismatch nothing is linked.
- With `--confirm`, the accept waits as `pending` until `clawtilla federation approve ALIAS`.
- No address of your own? Pass `--endpoint` (a tunnel URL or `clawtilla://TAILNET-IP:PORT`), or set `federation.public_url`.

**Cloudflare Tunnel:**

```bash
clawtilla federation expose --cloudflare --hostname fleet.example.com   # prints cloudflared config for <state_dir>/federation.sock
clawtilla config set federation.public_url https://fleet.example.com
```

clawtilla never runs cloudflared. For Cloudflare Access, add `--access-id ID` to the invite; it prompts for the secret.

**Change what a fleet may do:**

```bash
clawtilla federation set acme --tier limited
clawtilla federation set acme reach=listed agents=atlas,team:web cap.exec=deny
clawtilla federation set acme cap.exec=          # remove the override
clawtilla federation tag acme +billing -trial
clawtilla federation hide acme / mute acme / unhide / unmute
```

**End a link:** `clawtilla federation revoke ALIAS` tells the other fleet and closes its connections. Then `forget ALIAS` removes the record.

**Rotate the key:** `clawtilla federation rotate-key`. Peers are told with the old key and accept it for 24 h. A peer unreachable longer than that must be linked again.

## Sidebar and office (0.67.0)

- Each side keeps a **roster** of the other: agents (id, name, team, colour, state, busy, inbox) and teams, filtered by *the other side's* grant before sending (`see_roster`, `reach`, `agents`, `see_presence`, `see_typing`). It rides in every hello and is pushed within a second of a change here, over a WebSocket (`GET /fed/v1/events`, HTTP links) or `fed.push` frames (IPC links). Every push is a signed envelope, verified and sanitised (max 500 agents).
- `agent.list` gains `fleets`: one entry per non-revoked link with tier/health and their tones, owner, tags, hidden, muted, latency, teams and `agents[]` (`ref` = `AGENT@ALIAS`). A fleet not live shows its agents' `state` as `unknown`. Event: `federation.roster` (subject = alias).
- Every client's sidebar ends with **Linked fleets**: filter chips (Online, tiers; owners/tags behind More...; Show hidden), then a section per fleet with its agents and a presence word (working, idle, stopped, unknown). Chips and folds are per window/browser; **hide** and **mute** are the daemon's (`clawtilla federation hide|mute ALIAS`), so every client agrees.
- To let a peer see *fewer* of your agents: `clawtilla federation set ALIAS reach=listed agents=atlas,team:web`, or `see_roster=false` for none; `see_presence=false` shows them without state. The peer's sidebar follows without a restart.
- **Office**: a linked fleet's *running* agents visit your office as `external` participants (`ALIAS:AGENT`), drawn idle when presence is withheld, working when busy. Hidden or not-live fleets stay off the floor. `office.peers` still works but is superseded; a fleet in both is drawn once, from the link.
- Health `degraded` = requests work, the live stream does not (rosters still arrive with each hello).

## Messages (0.68.0)

- A remote agent is `AGENT@ALIAS` (`atlas@lab`). `user@ALIAS` is that fleet's operator, reachable only in a conversation its operator started.
- A conversation is a room `fed:ALIAS:LOCAL:REMOTE` (members: our side, and the sender `fleet:ALIAS:AGENT`), named `atlas@lab` or `support ↔ atlas@lab`. Anything our side says in it is sent; anything they say lands in it.
- Send: click the agent under Linked fleets (IPC `federation.open {ref}` -> `{room}`), `clawtilla send atlas@lab "text"`, or as an agent `clawtilla_message_agent agent_id=atlas@lab` -- only with `agents.tools.federation: true` (default false; dangerous: words leave the machine). Such an agent also sees remote agents in `clawtilla_list_agents` and links in `clawtilla_federation_peers`.
- Messages queue in the outbox (`federation.db`) and go when the fleet answers; given up after `federation.outbox_hours` (72) with a note.
- The receiving fleet checks, against **its** grant to us: well-formed (<= 64 KiB) -> reach (grant `reach`/`agents`, the agent's own `agents.federation_reach`; an ongoing conversation is not re-judged) -> `max_hops_in` -> `messages_per_hour` -> `message` mode: `allow` delivers, `deny` declines, `ask`/`review`/`hand_back` hold it as a `fed-hold-*` decision (Deliver / Decline) in its operator's inbox, persisted in `federation.db`.
- The sender sees every outcome except delivered as a clawtilla note in the conversation ("holding", "declined", "Not delivered to ..."), and the event `federation.delivery` (`sent|delivered|held|declined|refused|expired`).
- The agent receives remote words under a preamble (fleet, owner, tier, "not your operator") inside a backtick fence longer than any run in them, `[clawtilla]` escaped. The transcript shows only what they wrote.
- One request, one answer: an agent's closing text answers a message and goes back; the answer arrives inviting nothing, so it is not answered again. Write again with a new `clawtilla_message_agent`. Remote senders never renew the pair budget.
- To keep an agent away from most fleets: `clawtilla agent set billing federation_reach tier:owned` (selectors; only narrows).

## Delegation and caps (0.69.0)

- Hand work over: `clawtilla_delegate agent_id=atlas@lab task=...` (agent with `tools.federation`), `clawtilla task create --agent atlas@lab "..."`, or IPC `task.create {agent: "atlas@lab"}`.
- Here it is a **proxy** task: assignee `atlas@lab`, `remote_peer: lab`, parent = the delegator's current task. Never failed by a restart (the other fleet carries it). Its settle notice reaches the delegator like any task's.
- There it is a **real** task, origin `fleet:ALIAS:AGENT`, assigned to the agent, delivered fenced into the `fed:` conversation with assignment guidance. Its state (running, completed + result, failed, stalled, cancelled) comes back as `task.state`; the proxy follows. Cancelling the proxy sends `task.cancel` (cascades there); cancelling there cancels the proxy. The wire id is always the proxy's id.
- The doing fleet checks its grant to you: reach, `max_hops_in`, `max_concurrent_tasks` (running at once), `budget_cents_per_day` (spend on your tasks in 24 h, from its ledger; refuses new work only), then `delegate`: allow / deny / ask-review-hand_back = held as a `fed-hold-*` decision (Deliver starts it). A refusal fails the proxy with the reason ("Bravo Lab did not take it: ...").
- Spend is charged only where the work runs.
- **Caps**: a turn works for a fleet when it is in a `fed:` room with it or carries (or descends from) a task that came from it. Every call then takes the stricter of the agent's action rule and that fleet's `cap.CLASS` -- clawtilla tools and, via the permission hook, the CLI's own Bash/edits. `cap.fleet_admin` is always deny; `subdelegate=false` denies `clawtilla_delegate` in such turns. With `federation.enabled`, every agent gets the permission hook.
- Tighten a fleet: `clawtilla federation set ALIAS cap.exec=deny cap.write=ask max_concurrent_tasks=1 budget_cents_per_day=300 delegate=ask`.

## Transcripts, shared rooms, the handshake (0.70.0)

- **Transcripts** (the grant's `transcripts`, theirs to you): `none`; `own_tasks` = work you gave them, by your proxy id -- state, progress, result, and that task's messages; `reachable` = also every conversation you have had with an agent you may reach. `clawtilla federation transcript ALIAS --task PROXY-ID` or `--agent AGENT` (IPC `federation.transcript`, deferred; needs a link to them). Max 200 messages, 8000 chars each.
- **Shared rooms**: `clawtilla room share design lab` (IPC `room.share {room, alias}`; `room.unshare`). Only a declared room; your grant to them needs `join_rooms`, and theirs to you too or no mirror is made. The host fans every word out to the fleets sharing it except the speaker's; their words reach your agents by the room's rules, fenced, sender `fleet:ALIAS:AGENT`. They see `fedroom:HOST:ROOM` ("design @ lab") in their sidebar; what their operator writes there comes to you. Revoking a link removes its mirrors and its shares.
- **Handshake**: a visitor that spoke with one of your agents in the last 2 minutes is drawn on a call with it in your office (`peer`, `room` on the visitor).

## Discover, migrate, weather, estimate (0.71.0)

- `clawtilla federation discover` asks every online tailnet machine at its `daemon.tcp_port` (tailscaled status, no subprocess) `whois`; lists fleets that answer (address, name, owner, version, host, safety words, `linked_as`). `--candidate ENDPOINT` (repeatable) for LAN addresses or sockets. Links nothing. IPC `federation.discover {candidates?}` (deferred, 5 s).
- `clawtilla federation migrate-office` lists `office.peers` entries and whether each is linked; `migrate-office NAME` links it as owned via `federation.link` with its stored pairing token. Then remove the entry from `office.peers` and revoke that device on the other machine (this fleet cannot).
- **Weather**: each sidebar fleet heading shows a traffic sparkline (envelopes per 5 min, last 20 min, none when quiet); hover adds the full hour and latency of the last 12 answers. `federation show` prints "Weather:". Data: `weather {latency, volume}` on `agent.list` fleets and `federation.peer.show`.
- **Estimate before delegating**: their hello reports your work there (allowance `budget_cents_per_day`, 24 h used, running/max concurrent, `delegate` mode); finished tasks report their cost and an average is kept (`federation.db`). Shown in `clawtilla_delegate`'s answer, `clawtilla_federation_peers`, and `federation show` ("Before delegating"). Their word, not enforced here.

## Reach out, mentions, rules, admin tools, signed posts (0.72.0)

- `agents.federation_may_reach`: peer selectors an agent (with `tools.federation`) may write to / delegate to; unset = all; refused messages name the key; `clawtilla_list_agents` hides the rest. (`federation_reach` is the other direction.)
- Mentions: `@atlas@lab` is one token, never wakes a local `atlas`; `zach@bob.com` is not one. In a room you share with lab it wakes atlas there (if lab's grant to you reaches it). Composers complete other fleets' agents in rooms that cross fleets (`room.list` `mention_names`); `@atlas@la` completes as one name.
- Open: `clawtilla-gtk --open atlas@lab`, web `/r/atlas@lab`, lrg open -> the conversation with atlas.
- Rules editor on every client's Federation page; IPC `federation.rules {rules?}` reads or replaces the list whole, each checked by `clawt_fed_rule_check()` (bad rule refused when written).
- Admin tools for an agent with `tools.federation` + `tools.manage_fleet`: `clawtilla_federation_invite` (limited/untrusted only), `_set_tier` (lower now; raise -> `fed-tier-*` decision), `_tag`, `_revoke`. All `fleet_admin`.
- Shared-room posts carry the author fleet's signature over room, id, author, text; the host relays it untouched; a receiver linked with the author checks it and refuses a forgery; an unknown author is the host's word.
- `clawtilla federation log -f` follows the audit.

## Storage, ticks, presence (0.73.0)

- Everything federation keeps is `<state_dir>/federation/federation.db` (SQLite, 0600): tables `audit`, `nonces`, `msg_outbox`, `deliveries`, `kv` (holds, costs, shares). Peers stay in `peers.yaml`; key and invites in the secrets dir. Older files (`outbox.json`, `holds.json`, `costs.json`, `shared-rooms.json`, `audit.ndjson`, `nonces`) are imported once and renamed `*.imported`.
- Each message sent to another fleet shows a tick in every client: waiting to go, sent, delivered, held for their operator, declined, not delivered, gave up. `room.history` carries `delivery`/`delivery_detail`; `federation.delivery` redraws it.
- Events `federation.presence` (`ref`, `state`) and `federation.typing` (`ref`, `typing`) name the one remote agent that changed.

## Diagnose

| Symptom | Cause, fix |
|---|---|
| "no address another fleet can reach" | set `federation.public_url`, pass `--endpoint`, or join a tailnet |
| "local-network address in plain text" | `federation.allow_lan_plaintext: true`, or use the tailnet/TLS/tunnel |
| "would cross the internet in plain text" | use https:// or a tailnet address; never allowed |
| "stamp time is N s from ours" | clocks differ more than `federation.clock_skew_seconds`; run NTP |
| "not the one that made the invite" | the invite's address reached another fleet; check address and safety words |
| "used, revoked or has expired" | invites are single-use; make a new one |
| peer stays offline | `clawtilla federation show ALIAS` (last error); tunnel running? tailnet ACL? listener bound? |
| sidebar shows a fleet with no agents | their grant gives you `see_roster=false`, or `reach`/`agents` matches none; ask them; `clawtilla federation show ALIAS` prints "What it lets this fleet do there" |
| a fleet's agents all `unknown` | it is not live, or it withholds `see_presence` |
| a fleet missing from the sidebar | hidden (`federation unhide`), or a chip filters it out |
| "Not delivered ... no agent here you may reach" | their grant or the agent's `federation_reach` excludes it; write to their inbox agent |
| "agents.tools.federation is off" | `clawtilla agent set AGENT tools.federation true`, if it should write to other fleets |
| a message never arrives, no note | the fleet is unreachable; it is in the outbox until it answers or `federation.outbox_hours` passes |
| "holding this message" | their operator must Deliver it in their Decisions inbox |
| proxy task failed "X did not take it: ..." | their `delegate`, `max_concurrent_tasks`, `budget_cents_per_day` or reach; `federation show ALIAS` |
| `room share` refused "does not let it join rooms" | `clawtilla federation set ALIAS join_rooms=true`; they must allow it too |
| `federation transcript` refused | their `transcripts` is none, or own_tasks and you asked by agent |
| a tool refused "its grant caps what may be done for it" | the turn works for a linked fleet whose `cap.CLASS` is stricter; widen with `federation set ALIAS cap.exec=ask` |
| "unknown fleet or key" | the peer revoked/forgot you, or a rotation was missed; link again |

`clawtilla federation log [ALIAS]` is the audit (`federation.db`). It records every request between fleets, refusals included, except successful keep-alive hellos and pings.

## Security facts to keep straight

- Another fleet's agent is never the operator here. Only the operator, clawtilla itself and integrations (`grok:`, `venture:`) may post into a room they are not a member of.
- A `fed.hello` IPC connection is a *fleet*: `fed.*` frames only, no event stream, whatever socket it used.
- Invites (with secrets) are in `<secrets.dir>/federation/invites.yaml`. Peers (public keys only) are in `<state_dir>/federation/peers.yaml`.
- Since 0.66.0 `daemon.tls_cert` really makes the IPC TCP listener speak TLS. A plain-text client profile to such a daemon stops working; give the profile `tls` (or a pin).
