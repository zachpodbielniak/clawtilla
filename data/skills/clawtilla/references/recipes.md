# Recipes

Short, verified sequences. Commands are in [cli.md](cli.md) and
[cli-2.md](cli-2.md); every key is in [config-keys.md](config-keys.md). A
fleet agent does the same work through its `clawtilla_*` tools where it has
them ([mcp-tools.md](mcp-tools.md)); these are the operator's spellings.

## Change any setting safely

1. Find the key in [config-keys.md](config-keys.md). Agent keys are written
   without the `agents.` prefix when set on one agent (`agents.model.model`
   is `model.model`).
2. Check first: `clawtilla config show` (fleet), `clawtilla agent show <agent>`.
3. Apply it by where the key lives:
   - one agent: `clawtilla agent set <agent> <key> <value>` (lists
     comma-separated);
   - a team or project field: `clawtilla team set ...`, `clawtilla project set ...`;
   - anything fleet-level (`daemon.*`, `orchestration.*`, `defaults.*`, ...):
     `clawtilla config edit` (opens `$EDITOR`, reloads on save), or edit the
     file and run `clawtilla config reload`. There is no `config set`.
4. Validate a hand edit before reloading: `clawtilla config validate --file ~/.clawtilla/config.yaml --strict`.
5. Make it take effect -- `agent set` says which:
   - "Restart it" (tool lists, computer, runtime keys): `clawtilla agent restart <agent>`;
   - any `persona.*` key or an edited persona file: `clawtilla agent reset <agent>`
     (a resumed session is never re-handed a system prompt);
   - a listener (`daemon.webhook_enabled`, `daemon.tailscale`, `daemon.tcp_enabled`):
     restart the daemon -- safely, see "Pause the fleet" below.
6. Undo: `clawtilla history config`, `clawtilla history diff <id>`, `clawtilla history restore <id>`.

## Add an agent with a container computer

1. `clawtilla image` -- pick an image (or any reference podman can pull).
2. `clawtilla agent create --id scout --model sonnet --computer container --image <ref>`
   (starts it; `--no-start` to leave it stopped).
3. `clawtilla agent edit scout` -- write who it is; then `clawtilla agent reset scout`.
4. `clawtilla computer exec scout -- uname -a` to check the machine.
5. Stopping a container destroys it unless `computer.container.keep` is on:
   `clawtilla computer stop scout --remove` is required when it would. See [computers.md](computers.md).

Or `clawtilla agent new --ai "reads my notes and answers questions"` to have it designed.

## Make a chief of staff and a team with a lead

1. `clawtilla agent set chief chief_of_staff true` (at most one agent; it receives work addressed to the fleet and outranks leads).
2. `clawtilla team create research "Literature and sources"`.
3. `clawtilla team assign wren research lead` and `clawtilla team assign kudu research`
   (sets `team` and `team_role`; one lead per team; leads delegate only inside their team).
4. `clawtilla team list` -- check the lead and members.
5. `clawtilla send chief "summarize this week's commits"`, then `clawtilla task list`.

See [teams-and-ephemeral.md](teams-and-ephemeral.md), [orchestration.md](orchestration.md).

## Grant a tool permission

1. Capability tools are booleans: `clawtilla agent set <agent> tools.ephemeral true`,
   `tools.manage_fleet true` (both flagged dangerous), `tools.office true`, `tools.schedule true`.
2. Narrow or widen by name: `clawtilla agent set <agent> tools.allow clawtilla_delegate,clawtilla_task_list`
   (unset = all tools it has the capability for) or `tools.deny <names>` (deny always wins).
3. Decide what needs asking: `clawtilla agent set <agent> policy.rules "exec=ask,delete=deny"`
   (MATCH=MODE, first match wins; modes allow, review, ask, hand_back, deny).
4. `clawtilla agent restart <agent>` -- tools are listed at session start.
5. Answer its asks: `clawtilla decision list`, `clawtilla decision answer <id> "yes"`. See [security.md](security.md).

## Share a folder

- Whole fleet: `clawtilla folders add ~/source` (add `--ro`, `--team T` or `--agent A`; repeatable).
- One agent: `clawtilla agent mount add scout ~/src /work rw`, then `clawtilla agent restart scout`.
- A source this user does not own: add `--relabel none`. Check with `clawtilla folders list` / `clawtilla agent mount list scout`.
- A VM guest only sees a share added before its first boot: `clawtilla computer rebuild <agent>` (agent stopped). See [computers.md](computers.md).

## Add an MCP connector and sign in

1. `clawtilla connector catalog`.
2. Remote server: `clawtilla connector add notion --url https://mcp.notion.com/mcp --scope selected --agents scout`,
   then `clawtilla connector connect notion`.
   Token instead of OAuth: `clawtilla connector add gh --provider github --account work --command github-mcp-server --scope all`,
   then `clawtilla connector key gh` (token on stdin).
3. `clawtilla connector list` -- live or not; `clawtilla agent restart scout`.
4. A plain MCP server with no credential: `clawtilla integration add github mcp scope=all command=npx args=-y,@modelcontextprotocol/server-github`.
See [connectors.md](connectors.md), [integrations.md](integrations.md).

## Schedule a routine

1. `clawtilla routine add standup chief description="Yesterday's commits" instructions="Summarise the last 24 hours of commits." schedule=weekdays at=09:00`
   (custom: `schedule=custom cron="0 */6 * * *"`).
2. `clawtilla routine run standup` to try it; `clawtilla routine list` for the next run.
3. Change with `clawtilla routine set standup enabled=false`. See [routines-and-triggers.md](routines-and-triggers.md).

## Add a webhook trigger

1. Turn on the receiver: `daemon.webhook_enabled: true` via `clawtilla config edit`, then restart the daemon (port `daemon.webhook_port`, default 8788).
2. `clawtilla trigger add ci builder provider=forgejo repo=zach/clawtilla events=push instructions="A push landed on {{repo}} {{ref}}. Run the tests."`
   -- copy the secret and address now; they are printed once.
3. Point the forge's webhook at it; `clawtilla trigger capture ci` shows the first delivery.
4. `clawtilla trigger set ci enabled=true`; `clawtilla trigger deliveries ci` afterwards. Lost secret: `clawtilla trigger rotate ci`.

## Give a skill to a team

1. `clawtilla skill import ~/src/repo/skills/release` (or `clawtilla skill new release "..."`).
2. Read it: `clawtilla skill show release` -- it lands disabled.
3. `clawtilla skill enable release`, then `clawtilla skill assign release team:research` (or an agent id, or `fleet`).
4. `clawtilla skill test release --agent wren` if it has tests. See [skills-and-teach.md](skills-and-teach.md).

## Pause the fleet and resume (and restart the daemon safely)

1. `clawtilla pause` (or `clawtilla pause <agent>`): delivery stops, running turns finish.
2. `clawtilla status` until draining reaches zero; queued work stays queued.
3. Restart if needed: `systemctl --user restart clawtilla` (the unit from `clawtilla --generate-systemd-service`).
4. `clawtilla resume`.

## Back up and restore

1. `clawtilla pause`, wait for zero, then stop the daemon (`systemctl --user stop clawtilla`).
2. `clawtilla backup create ~/.clawtilla ~/fleet.clawt`, then `clawtilla backup verify ~/fleet.clawt`.
3. Restore into a new directory: `clawtilla backup preview ~/fleet.clawt`, `clawtilla backup restore ~/fleet.clawt ~/restored-fleet`.
4. Credentials, tokens and sessions are not in it: sign connectors in again. See [operating.md](operating.md).

## Reach a remote daemon

1. On the daemon's machine: `clawtilla daemon token` (tailnet listener on by default, `daemon.tailscale`).
2. Here: `clawtilla remote add workstation 100.72.0.41 --token "$(ssh box clawtilla daemon token)"`,
   or pair instead: `clawtilla device pair --name laptop` there, `clawtilla remote pair workstation 100.72.0.41 <code>` here.
3. `clawtilla --profile workstation agent list` (global flags go before the command).
   One-off: `clawtilla -H 100.72.0.41 --token T status`. See [clients.md](clients.md).

## Link this fleet with another

1. Both fleets: `federation: {enabled: true, name: ..., owner: ...}`, restart the daemon; `clawtilla federation status`.
2. Your own other daemon: `clawtilla federation link 100.64.0.7:8792 --alias homelab` (tailnet; asks for its owner token) or a socket path.
3. Anyone else: `clawtilla federation invite --tier limited` here, send the `clawt-fed1-...` string;
   there: `clawtilla federation inspect ...` then `clawtilla federation accept ... --tier trusted --alias you`; read the safety words to each other.
4. Narrow it: `clawtilla federation set ALIAS reach=listed agents=atlas cap.exec=deny`; tag it: `clawtilla federation tag ALIAS +client-acme`.
5. Through a tunnel: `clawtilla federation expose --cloudflare --hostname H`, then `federation.public_url`. See [federation.md](federation.md).

## Write to an agent in another fleet

1. Find it: the sidebar's Linked fleets, or `clawtilla agent list` (its `fleets`), as `AGENT@ALIAS`.
2. `clawtilla send atlas@lab "can you check last night's build?"`; read the answer with `clawtilla room history fed:lab:user:atlas`.
3. Let one of your agents do it too: `clawtilla agent set support tools.federation true`; it then uses `clawtilla_message_agent agent_id=atlas@lab`.
4. Make a fleet's messages wait for you: `clawtilla federation set ALIAS message=ask` -- each lands as a Deliver/Decline decision.
5. Keep an agent out of reach of most fleets: `clawtilla agent set billing federation_reach tier:owned`. See [federation.md](federation.md), "Messages".

## Hand work to an agent in another fleet

1. `clawtilla task create --agent atlas@lab "summarise last week's incidents"` (or an agent with `tools.federation`: `clawtilla_delegate agent_id=atlas@lab`).
2. Watch it: `clawtilla task list` (the proxy, assignee `atlas@lab`); their fleet may hold it for its operator.
3. Call it off: `clawtilla task cancel <id>` -- it is cancelled there too.
4. On the doing side, limit what work for that fleet may do: `clawtilla federation set ALIAS cap.exec=deny max_concurrent_tasks=2 budget_cents_per_day=500`. See [federation.md](federation.md), "Delegation and caps".

## Check spend and plan limits

1. `clawtilla cost --today`, `clawtilla cost --days 7 --by origin`, `clawtilla cost --by model --csv > spend.csv`.
2. `clawtilla limits` (live, up to 60 s) or `clawtilla limits --cached`.
3. Caps: `orchestration.daily_fleet_budget_micros` (fleet per day), `orchestration.task_budget_usd` (per task and everything under it),
   a project's `monthly_budget_usd` (`clawtilla project set <id> monthly_budget_usd 40`). See [spending-and-usage.md](spending-and-usage.md).

## Recover a stuck task

1. `clawtilla activity` -- is a turn actually running, and its last step.
2. `clawtilla task show <task> --journal`; `clawtilla wait list <agent>`; `clawtilla decision list` (it may be waiting on you).
3. A stopped agent holds its queue: `clawtilla mailbox list <agent>`, `clawtilla mailbox dead <agent>`, `clawtilla mailbox requeue <agent> <item>`.
4. Wedged turn: `clawtilla agent interrupt <agent>`; still bad: `clawtilla agent restart <agent>`.
5. Give up or rerun: `clawtilla task cancel <task>` or `clawtilla task retry <task> "reason"`; incidents: `clawtilla task incidents`.
6. History: `clawtilla event list <task>`, `clawtilla sentinel --days 1 --output -`. See [tasks-and-projects.md](tasks-and-projects.md).

## Export and import a fleet pack

1. `clawtilla fleet export ~/fleet-pack` -- secrets and machine paths are left out and listed to fill.
2. Elsewhere: `clawtilla fleet import ~/fleet-pack --preview`.
3. `clawtilla fleet import ~/fleet-pack --rename agent:scout=scout2` (also `--skip`, `--replace KIND:ID`; `--allow-dangerous` only on purpose).
4. Agents arrive held (`runtime.autostart: false`) and skills disabled: fill credentials, `clawtilla skill enable ...`, then `clawtilla agent start <agent>`.
