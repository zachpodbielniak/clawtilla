# Media generation: Meshy and PixelLab

Paid services an agent (or the office) can make art with. Source doc: docs/media-generation.org. Code: src/integration/clawt-media.c (requests, prices, file names, spend), src/core/daemon-media.c (keys, jobs, files), ai-glib's `AiMeshyClient` / `AiPixellabClient`.

| Service | Makes | Priced |
|---|---|---|
| Meshy | 3D models: `text-to-3d`, `image-to-3d`, `multi-image-to-3d`, `retexture`, `remesh`, `rig` (biped/quadruped, with walking and running clips), `animate` | credits, from Meshy's price list |
| PixelLab | pixel art: `image` (16-400 px; `model: pixen` for the newer model), `bitforge`, `rotate`, `inpaint`, `animate` (64x64), `character-4`/`character-8` (16-128), `character-animate`, `tileset-topdown`, `tileset-sidescroller`, `isometric-tile`, `map-object` | dollars, decided by PixelLab after the call |

## Give an agent one

```yaml
integrations:
  - name: meshy
    type: meshy            # or pixellab
    scope: selected
    agents: [artist]
    token: {file: ~/.clawtilla/secrets/meshy.key}
```

`one_per_agent`; the only required key is `token` (a secret reference); `url` only for a proxy/test server (https or loopback). Then **restart the agent** -- the tool list is read at session start. `clawtilla integration health AGENT NAME` checks on paper and never calls the service; the agent's `_balance` tool tries the key.

## Tools (all gated on the binding; offered only to agents in scope)

`clawtilla_meshy_generate` / `clawtilla_pixellab_generate` (external, **asked by default**), `clawtilla_meshy_job` / `clawtilla_pixellab_job` (read), `clawtilla_meshy_cancel` (external), `clawtilla_meshy_balance` / `clawtilla_pixellab_balance` (read). Arguments and gates are in [mcp-tools-2.md](mcp-tools-2.md).

- `params` is the service's own fields, passed untouched; `images` maps a field to a path (`{"image_url": "ref.png"}`), read **only** from the agent's workspace (relative paths) or the exchange, canonicalised.
- A generate answers with a job id; a PixelLab picture is saved at once. Check the rest with the `_job` tool, which saves every named result the first time it sees the job done.
- A finished job's reply names the id a next step needs: Meshy's task id (`input_task_id` to rig/remesh/retexture, `rig_task_id` to animate), PixelLab's character id (`character_id` for `character-animate`). A text-to-3d job's changing id is tracked for you: always ask by the first id.
- Jobs are per agent (`<state_dir>/media/jobs.json`): another agent's id is "not one you started".

## Approvals: the one tool family that asks by default

Every `_generate` files a policy ask with the price first ("Meshy rig: about 5 credits"; PixelLab: "priced by PixelLab when it is done, most calls cost $0.01 to $0.08") **unless a policy rule matches**. To trust an agent: `policy.rules: ["clawtilla_pixellab_*=allow"]`. A `deny` rule turns a service off for one agent without unbinding it. Rules are read before the default.

## Results

- Saved to `<exchange>/<agent>/media/<job>/` (in a computer `/mnt/clawtilla/exchange/<agent>/media/<job>/`), else `<workspace>/media/<job>/`, with `provenance.yaml` (service, operation, prompt, job, cost, terms).
- https only, no credential, capped while read: 48 MiB pictures/sheets, 200 MiB models. A non-https link is listed "not saved".
- **Meshy deletes results after about three days** -- collect them with the `_job` tool in time.

## Spend

- Recorded per agent in `<state_dir>/media-spend.ndjson` when saved; the **daily caps count it** (`daily_agent_budget_micros`, `daily_fleet_budget_micros`).
- Meshy credits count only with `orchestration.meshy_credit_micros` set (micro-dollars per credit); otherwise they are shown as credits and are in no dollar figure -- unknown, not free.
- Spending pages and `clawtilla cost` add "Paid media: $X over N calls, on top of the total" under the table.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| tool not offered | agent not in the integration's scope, or not restarted since |
| "a paid service: each call is asked..." on the link route | that route cannot wait; the `.mcp.json` server asks instead |
| `402` | out of credits / Meshy free plan |
| `429` with a delay / naming concurrency | rate limit / plan's concurrent-job cap |
| "outside your workspace and the exchange" | copy the picture into the workspace or exchange |
| files gone after days | Meshy's retention; run the job again |
