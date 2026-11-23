# Configuration: `daemon`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `daemon`

The daemon itself: where it listens, where it keeps state, how loud it is.

clawtillad owns every agent process, every credential and every socket.
Clients hold no transports of their own -- they send typed commands and
fold one event stream. That is what lets the CLI, the GTK app, the web
client and an in-process cmacs embed all be the same program.

### `daemon.socket`

- Type: path
- Default: `$XDG_RUNTIME_DIR/clawtilla/daemon.sock`
- Since: 0.1.0

Unix socket the clients connect to.

Created with mode 0600 inside a 0700 directory, which is the whole of
the access control: the daemon does not check the peer's uid, so
anything that can open the file is trusted completely.

A unix socket is used rather than a loopback HTTP port because the
filesystem can express that at all, and because any web page the user
visits can reach a loopback port through DNS rebinding.

### `daemon.git`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Make the state directory a git repository on first start.

The workspaces, the org files and this configuration are worth having
a history of. What makes this safe rather than alarming is the
.gitignore written beside it: the same directory holds credentials,
link tokens, mailboxes and memory databases, and the moment to keep
those out of a history is before there is anything in it.

clawtilla never commits. It creates the repository and the ignore
rules; what goes into a commit stays your decision.

Turning this off stops the repository being created. The ignore file
is still written, because a state directory somebody put in git by
hand needs it more, not less.

### `daemon.state_dir`

- Type: path
- Default: `~/.clawtilla`
- Since: 0.1.0

Where agent workspaces, mailboxes, transcripts and credentials live.

Created 0700. Never mounted into any agent's computer, whatever the
mount configuration says -- an agent that could read this directory
could read every other agent's credentials.

### `daemon.pod_module_dir`

- Type: path
- Default: unset
- Since: 0.1.0

Where podomation's loadable modules are, if they are not found for you.

The container and vm computers do their work through podomation's
'container' and 'vm_virtmanager' modules, which libreclaw builds. They
are looked for next to the running binary and then in the install
location, so a normal build and a normal install both work without
setting this.

Set it when the modules live somewhere else -- a system podomation, or
a build tree you are testing against. Naming a directory here means
that directory and no other, so a wrong path fails loudly instead of
being papered over by a stale set found elsewhere.

### `daemon.log_level`

- Type: enum
- Values: error, warning, info, debug
- Default: `info`
- Since: 0.1.0

How much the daemon says: error, warning, info, debug.

Everything at or above the level reaches the log and nothing below.
GLib already drops debug messages unless G_MESSAGES_DEBUG names the
domain, so debug is what turns clawtilla's own on; warning and error
are what quieten a daemon that is working.

Read by clawtillad only. A host embedding ClawtDaemon owns its own
logging, for the same reason it owns its own signal handling.

It said `debug logs every frame on every link` until 0.2.0, and did
not: nothing read this key at all, and there is no per-frame logging
to enable. Turning it up changed nothing and warned about nothing,
because it is not flagged inert either.

### `daemon.tcp_enabled`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Also listen on TCP, so clients on other machines can connect.

Off by default. The unix socket is authenticated by the kernel; a TCP
listener is authenticated by whatever you configure below, so turning
this on without TLS and a token puts the whole fleet on the network.

### `daemon.tailscale`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Also listen on this machine's tailnet address, when there is one.

On by default, and the one network where that is defensible: a peer on
a tailnet is a device you enrolled and WireGuard authenticated, and
nothing outside it can route to a 100.64/10 address at all. It is what
lets the client on a laptop reach the fleet on a workstation with no
tunnel set up by hand.

A token is still required, and generated into <state_dir>/tcp-token if
token_file names nothing -- so the listener is never open, only
reachable. Print it with `clawtilla daemon token`.

Nothing happens when Tailscale is absent, down, or in
userspace-networking mode, since there is then no address to bind.

### `daemon.tcp_address`

- Type: string
- Default: `127.0.0.1`
- Since: 0.1.0

Address to bind when tcp_enabled is true.

### `daemon.tcp_port`

- Type: integer
- Default: `8792`
- Since: 0.1.0

Port to bind for tcp_enabled and for tailscale. Both use this one.

### `daemon.tls_cert`

- Type: path
- Default: unset
- Since: 0.1.0

TLS certificate for the TCP listener. Without one, remote clients talk
in clear text and so do their bearer tokens.

### `daemon.tls_key`

- Type: path
- Default: unset
- Since: 0.1.0

TLS private key matching tls_cert.

### `daemon.token_file`

- Type: path
- Default: unset
- Since: 0.1.0

File holding the bearer token remote clients must present.

Ignored for unix socket connections, which the kernel already vouches
for. Left unset, a token is generated into <state_dir>/tcp-token the
first time a TCP or tailnet listener needs one, so enabling either
cannot leave the daemon open by omission.

### `daemon.device_idle_days`

- Type: integer
- Default: `90`
- Since: 0.15.0

Days a paired device may go unseen before its token stops working.
0 means never.

A device is seen when it connects and, once a minute, for as long as
it stays connected -- so a laptop left open on the fleet never lapses
while somebody is looking at it. An expired device is removed exactly
as `clawtilla device revoke` would remove it, live connections and
all; pair it again with a fresh code. The daemon's own token
(token_file) is the owner credential and never expires.

### `daemon.event_log_days`

- Type: integer
- Default: `30`
- Since: 0.1.0

How many days of per-room event logs to keep. 0 disables the sweep.

The event log is the file you paste into a bug report. Secrets are
scrubbed as it is written, not as it is displayed -- a transcript gets
replayed into every context rebuild, so a leaked key in one would be
permanent.

### `daemon.allow_unconfined_host`

- Type: boolean
- Default: `false`
- Warning: hands over real authority; read security.md first
- Since: 0.1.0

Permit agents to use computer.host.confine: none.

An unconfined host computer means the agent runs commands as you, with
your files, your keys and your network. That needs to be two deliberate
acts rather than one typo, so it requires this AND confirm_host_control
on the agent itself.

### `daemon.automation_dir`

- Type: path
- Default: `~/.clawtilla/pods`
- Since: 0.2.0

Where podomation pods that watch the fleet live.

Every `*.pod` in here is loaded at start. clawtilla registers itself
into podomation as a module, so a pod can bind to what the fleet does
and act on it in the same file:

  pod researcher = clawtilla->new("researcher");

  researcher->on_agent_state where event->state == "error" =>
      clawtilla->restart_agent(agent: "researcher");

  researcher->on_agent_state where event->state == "error" =>
      clawtilla->notify(title: "researcher fell over; restarted it");

An event name cannot carry a dot -- podomation's lexer stops at it --
so it is on_agent_state rather than agent.state.

The constructor's arguments are the scope, and it applies both ways:
a pod named for one agent neither hears about the others nor can act
on them. With no arguments, the whole fleet.

A file that does not parse disables that file and warns; the rest are
loaded.

### `daemon.webhook_enabled`

- Type: boolean
- Default: `false`
- Since: 0.2.0

Whether to accept inbound trigger deliveries.

Off by default. A fleet nothing calls into needs no listener, and a
port opened because a feature exists rather than because somebody
asked for it is a port nobody is watching.

The receiver is its own server on its own port and serves only
/health and the secret /hooks/... paths -- never the IPC surface --
so putting it behind a tunnel exposes nothing else.

### `daemon.webhook_port`

- Type: integer
- Default: `8788`
- Since: 0.2.0

Port the trigger receiver listens on.

It binds loopback and the tailnet address when there is one, and
loopback alone when there is not -- the same rule the web client
follows, and for the same reason: a listener is never widened
because an address was missing.

### `daemon.update_check`

- Type: boolean
- Default: `false`
- Since: 0.2.0

Whether to ask, on a timer, if a newer clawtilla exists.

Off by default, because it is the only thing in the daemon that
reaches the network without somebody asking it to.

It checks and reports; it does not install anything. An unattended
update must not restart the daemon under running turns, so applying
one waits on a hold that can drain first.

The result reaches every client through control.status, including
when the check itself failed -- a check that has been quietly
erroring for a month is worse than none, because nothing to draw
reads as up to date.

### `daemon.update_url`

- Type: string
- Default: `https://gitlab.com/api/v4/projects/zachpodbielniak%2Fclawtilla/releases`
- Since: 0.2.0

Where daemon.update_check asks.

Three answer shapes are understood, because three are real and none
of them is ours: a bare version as text or as a JSON string, an
object carrying version, tag_name or name, and an array of those --
which is what Forgejo, Gitea and GitLab all return from a releases
endpoint. Point it at a self-hosted forge or a file you publish;
nothing here is tied to a particular host.

### `daemon.update_interval_hours`

- Type: integer
- Default: `24`
- Since: 0.2.0

How often to ask, in hours. Clamped to at least one.

Nothing is asked at daemon start: the first check happens one
interval in. Starting a daemon must not reach the network, or every
test fixture that builds one does too.
