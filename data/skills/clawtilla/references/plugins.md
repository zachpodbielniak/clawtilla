# Plugins

A plugin is a shared object, `libclawt-plugin-<id>.so`, loaded into the **daemon's process with the daemon's privileges**. Loading one is trusting it completely. Source: docs/plugins.org, `plugins/example/example-plugin.c`. Keys: [config-keys.md](config-keys.md).

## Operating

```bash
clawtilla plugin list      # id, version, active|idle, description (IPC: plugin.list)
```

"No plugins loaded." means none were found; put the `.so` in `~/.config/clawtilla/plugins/` or set `CLAWT_PLUGIN_PATH`, then restart the daemon (loading happens at start; modules are never unloaded).

```yaml
plugins:
  enabled: true                       # false: nothing scanned or opened at all
  dirs: ["~/src/my-plugin/build"]     # searched before the XDG and system paths
  disabled: [noisy-plugin]            # by <id>; the .so is never opened
  example:                            # per-plugin block, keyed by id
    enabled: false                    # also disables
    mode: "verbose"
```

Search order (first plugin with a given id wins, so a user copy shadows a system one):

1. `$CLAWT_PLUGIN_PATH` (colon-separated)
2. `plugins.dirs`
3. `$XDG_CONFIG_HOME/clawtilla/plugins`
4. the compiled-in `CLAWT_PLUGIN_DIR` (`pkg-config --variable=plugindir clawtilla-1.0`)

The id comes from the **file name**, so disabling works before any plugin code runs. A shadowed copy logs "was not loaded: a plugin called 'x' is already loaded" -- expected when overriding deliberately.

### Configuration traps

- Only four keys under `plugins.<id>` reach the plugin's `configure()`, as strings: `endpoint`, `token_file`, `path`, `mode`. Everything else is silently dropped; a plugin needing more must read the config through the `config` service.
- Any `plugins.<id>` block also draws a harmless `unknown configuration key 'plugins.<id>'` warning at load.

## Failure isolation

| Failure | Result |
|---|---|
| ABI version mismatch | refused before any contract call, both versions named |
| `clawt_plugin_register` missing | refused |
| returns a type not derived from `ClawtPlugin` | refused |
| `activate()` returns FALSE | that plugin disabled; nothing else affected |
| misnamed file | refused (no id) |
| two plugins, one id | first found wins |

Load failures are warnings, and the loader's result never fails daemon start. **But** this covers error returns only: a plugin that segfaults, aborts or `g_error()`s (in a constructor, `class_init`, `configure` or `activate`) takes the daemon down. There is no sandbox; an ELF constructor runs at `dlopen` before any ABI check.

## What a plugin can extend

Two exported symbols:

```c
G_MODULE_EXPORT const guint clawt_plugin_abi_version = CLAWT_PLUGIN_ABI_VERSION; /* 1 */
G_MODULE_EXPORT GType clawt_plugin_register(void) { return EXAMPLE_TYPE_PLUGIN; }
```

`ClawtPluginClass` vfuncs: `get_name`, `get_version`, `get_description`, `configure`, `activate`, `deactivate`, `create_instance` (declared, **never called**), plus 8 padding slots. Capabilities are interfaces detected with `g_type_is_a()`:

| Interface | Wired? | Notes |
|---|---|---|
| `ClawtEventHandler` (`handles`, `handle`) | yes | `handles` asked before each dispatch; unimplemented = every event |
| `ClawtToolProvider` (`list_tools`, `get_params`, `call`) | yes | listed in MCP `tools/list`, subject to per-agent `tools.allow` / `tools.deny`; schema generated from `get_params`; arguments **not validated** before `call()` |
| `ClawtComputerProvider` | **no** | an unknown `computer.type` makes the agent a shadow before any provider is asked |
| `ClawtIntegrationProvider` | **no** | nothing calls it; new integration types go in the integration resolver and renderers |

Plugin tool caveats: every one is described only as "Provided by a plugin.", and none appears in the agent's `TOOLS.org` -- whose managed region claims to be the full list. Describe plugin tools in the agent's persona yourself or the agent will conclude it lacks them. See [mcp-tools.md](mcp-tools.md), [agents.md](agents.md).

Service locator at activate: `clawt_plugin_get_services(plugin)` -> hash table with `agents`, `rooms`, `tasks`, `router`, `events`, `config`. Treat an absent service as a reason to do less.

## Building one

```bash
mkdir -p plugins/mine && cp plugins/example/example-plugin.c plugins/mine/
make plugins                        # also part of plain `make`
ls build/release/plugins/           # libclawt-plugin-mine.so (dir name = id)
make install                        # copies into CLAWT_PLUGIN_DIR
```

Out of tree:

```bash
gcc $(pkg-config --cflags clawtilla-1.0) \
    $(pkg-config --variable=plugin_cflags clawtilla-1.0) \
    my-plugin.c -o libclawt-plugin-mine.so \
    $(pkg-config --variable=plugin_libs clawtilla-1.0)
```

Never link `-lclawt-1.0`: clawtilla symbols stay undefined and resolve from the daemon at `dlopen`. Never overwrite a loaded `.so` in place with `cp` (truncating a mapped file crashes the running daemon); copy to a temporary name and `mv`.

## Not the same as podomation modules

podomation's `container`, `distrobox` and `vm_virtmanager` modules are `.so` files loaded by `ClawtPodBridge` for computer backends (search path overridden by `daemon.pod_module_dir`), and the daemon registers its own in-process `clawtilla` pod module for automation. Neither is a clawtilla plugin and neither shows in `plugin list`. See [routines-and-triggers-2.md](routines-and-triggers-2.md) and [computers.md](computers.md).
