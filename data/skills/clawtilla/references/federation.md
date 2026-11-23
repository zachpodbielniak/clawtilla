# Federation: linking fleets

Read this before linking this fleet with another clawtilla fleet, changing what a linked fleet may do, or diagnosing a link. Full text: `docs/federation.org` in the checkout.

## What it is

- Each fleet has an **Ed25519 key** (`<secrets.dir>/federation/fleet.ed25519`, 0600). Its **fleet id** is the fingerprint of the *first* key (26 base32 characters) and never changes, even after `rotate-key`.
- **Safety words** (four words from the fleet id) are what two people read to each other while one accepts the other's invite.
- A **peer** is a linked fleet, known here by an **alias** (a valid id, never renamed). Peers live in `<state_dir>/federation/peers.yaml`, written by the daemon. Do not edit it; use `clawtilla federation` or a client's Settings → Federation page.
- **Every request and every answer between fleets is a signed envelope.** That holds over any transport. Kind, from, to, time, nonce, body digest, and what it was sent to are all signed. Answers are bound to the request's nonce.
- What crosses in 0.66.0: hello, ping, revoke, rotate. Not yet: messages (0.68.0), delegation (0.69.0), remote agents in the sidebar (0.67.0), shared rooms (0.70.0). A kind the other side does not know is answered 501 "not understood yet", naming its version.

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
| "unknown fleet or key" | the peer revoked/forgot you, or a rotation was missed; link again |

`clawtilla federation log [ALIAS]` is the audit (`<state_dir>/federation/audit.ndjson`). It records every request between fleets, refusals included, except successful keep-alive hellos and pings.

## Security facts to keep straight

- Another fleet's agent is never the operator here. Only the operator, clawtilla itself and integrations (`grok:`, `venture:`) may post into a room they are not a member of.
- A `fed.hello` IPC connection is a *fleet*: `fed.*` frames only, no event stream, whatever socket it used.
- Invites (with secrets) are in `<secrets.dir>/federation/invites.yaml`. Peers (public keys only) are in `<state_dir>/federation/peers.yaml`.
- Since 0.66.0 `daemon.tls_cert` really makes the IPC TCP listener speak TLS. A plain-text client profile to such a daemon stops working; give the profile `tls` (or a pin).
