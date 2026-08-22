/*
 * clawt-workspace.c - The standard file set in an agent's workspace
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent's personality lives in its workspace as org files, one
 * concern per file, so a person can open one and change it without
 * rewriting the rest.  The defaults here are meant to be edited: they
 * are a starting point that is already true about this agent, not a
 * placeholder to be replaced wholesale.
 */

#include "clawtilla.h"
#include "agent/clawt-workspace.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

/*
 * The order matters.  Character before role, role before the human,
 * the human before the tools, tools before the work -- an agent that
 * reads what it can do before it knows who it is spends the first turn
 * reconciling the two.
 */
static const ClawtWorkspaceFile workspace_files[] = {
    { "SOUL.org",         "character, mission and voice",
      TRUE,  FALSE },
    { "IDENTITY.org",     "name, role and self-description",
      TRUE,  FALSE },
    { "USER.org",         "who the human is and how they work",
      TRUE,  FALSE },
    { "AGENTS.org",       "how to work: format, safety, the fleet",
      TRUE,  FALSE },
    { "TOOLS.org",        "what this agent actually has",
      TRUE,  FALSE },
    { "TOOL_GOTCHAS.org", "footguns, appended as they are found",
      TRUE,  FALSE },
    { "PROJECTS.org",     "what is in flight and where it lives",
      TRUE,  FALSE },
    { "README.org",       "what this directory is, for a person",
      FALSE, FALSE },
    { "AGENTS.md",        "loader: the org files, in order",
      FALSE, FALSE },
    { "CLAUDE.md",        "loader: points at AGENTS.md",
      FALSE, FALSE },
    { ".mcp.json",        "MCP servers this agent may call",
      FALSE, TRUE  }
};

const ClawtWorkspaceFile *
clawt_workspace_files(guint *n_files)
{
    if (n_files != NULL)
        *n_files = G_N_ELEMENTS(workspace_files);

    return workspace_files;
}

GStrv
clawt_workspace_identity_files(void)
{
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; i < G_N_ELEMENTS(workspace_files); i++) {
        if (workspace_files[i].identity)
            g_ptr_array_add(names, g_strdup(workspace_files[i].name));
    }

    g_ptr_array_add(names, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

gchar *
clawt_workspace_file_path(ClawtAgentConfig *agent, const gchar *name)
{
    g_autofree gchar *workspace = NULL;

    g_return_val_if_fail(agent != NULL, NULL);
    g_return_val_if_fail(name != NULL, NULL);

    /*
     * A plain file name, and nothing else.
     *
     * This is reached from an IPC request, so a name is whatever the
     * client sent.  "../../.clawtilla/agents/other/credentials" is a
     * perfectly good relative path and would hand one client another
     * agent's secrets; g_path_is_absolute() alone does not stop it.
     */
    if (*name == '\0' || g_strcmp0(name, ".") == 0 ||
        strchr(name, G_DIR_SEPARATOR) != NULL || strstr(name, "..") != NULL)
        return NULL;

    workspace = clawt_agent_config_get_workspace(agent);

    if (workspace == NULL)
        return NULL;

    return g_build_filename(workspace, name, NULL);
}

/* ── The default content ─────────────────────────────────────────── */

/*
 * Substitutes {{key}} in a template.
 *
 * Deliberately tiny: the templates are ours, the keys are a closed set,
 * and anything more would be a template language nobody asked for.
 */
static gchar *
expand(const gchar *template, GHashTable *values)
{
    GString *out = g_string_new(NULL);
    const gchar *p = template;

    while (*p != '\0') {
        const gchar *open = strstr(p, "{{");
        const gchar *close;

        if (open == NULL) {
            g_string_append(out, p);
            break;
        }

        close = strstr(open, "}}");

        if (close == NULL) {
            g_string_append(out, p);
            break;
        }

        g_string_append_len(out, p, open - p);

        {
            g_autofree gchar *key = g_strndup(open + 2, close - open - 2);
            const gchar *value = g_hash_table_lookup(values, key);

            g_string_append(out, value != NULL ? value : "");
        }

        p = close + 2;
    }

    return g_string_free(out, FALSE);
}

static const gchar SOUL_ORG[] =
"#+title: SOUL -- Who You Are\n"
"#+description: Character, mission and voice for {{id}}.\n"
"\n"
"/You are {{name}}, one agent in a fleet {{user}} runs under clawtilla./\n"
"\n"
"* The Mission\n"
"\n"
"{{description}}\n"
"\n"
"Rewrite the line above. It is the one sentence that decides what you do\n"
"when nobody has told you what to do, and the default was generated from\n"
"a config field.\n"
"\n"
"* Operating Parameters\n"
"\n"
"- *Presence:* you run as a supervised process. Your mailbox is durable,\n"
"  so work queued while you were stopped is waiting when you come back.\n"
"  Drain it rather than asking what you missed.\n"
"- *Reversibility:* prefer the reversible action. Confirm before anything\n"
"  that destroys work -- a delete, a force push, a stopped service.\n"
"- *Honesty:* say what you did and what you did not. A partial result\n"
"  reported as complete costs more than the work it saved.\n"
"- *Scope:* do the task asked. If you find a second problem, say so and\n"
"  finish the first.\n"
"- *Your computer:* if you have one, that is where your work happens.\n"
"  Run shell commands there, not on the host -- see ~TOOLS.org~. Use\n"
"  the host only when asked to, and say so when you do.\n"
"\n"
"* Voice\n"
"\n"
"Direct. No preamble, no restating the question, no summary of what you\n"
"just did. If something is a bad idea, say so and say why.\n";

static const gchar IDENTITY_ORG[] =
"#+title: IDENTITY -- Who Am I?\n"
"#+description: Name, role and self-description for {{id}}.\n"
"\n"
"* Vitals\n"
"\n"
"- *Agent id:* ~{{id}}~ -- how every other agent addresses you\n"
"- *Name:* {{name}}\n"
"- *Role:* {{role}}\n"
"- *Model:* {{provider}} / {{model}}\n"
"- *Computer:* {{computer}}\n"
"\n"
"* Self-Description\n"
"\n"
"/Fill this in./ A paragraph in your own voice: what you are for, what you\n"
"are good at, what you decline. Other agents read your ~clawtilla_get_agent~\n"
"description before they delegate to you, and a vague one gets you either\n"
"nothing or everything.\n"
"\n"
"* What I Am Not\n"
"\n"
"/Fill this in./ Naming the work that should go to a different agent is\n"
"how a fleet stays useful. Be specific: name the agent.\n";

static const gchar USER_ORG[] =
"#+title: USER -- About Your Human\n"
"#+description: Who {{user}} is and how they work.\n"
"\n"
"* Vitals\n"
"\n"
"- *Name:* {{user}}\n"
"- *Pronouns:* /(not specified -- use they/them until told otherwise)/\n"
"- *Timezone:* /(fill in)/\n"
"\n"
"* How They Work\n"
"\n"
"/Fill this in./ Editor, shell, languages, what they consider obvious and\n"
"what they want spelled out.\n"
"\n"
"* Communication\n"
"\n"
"/Fill this in./ Length, format, how much hedging is tolerable. This is\n"
"the file that stops every conversation starting with the same\n"
"corrections.\n"
"\n"
"* Do Not\n"
"\n"
"/Fill this in./ The standing prohibitions -- tools they will not use,\n"
"suggestions they do not want to see again.\n";

static const gchar AGENTS_ORG[] =
"#+title: AGENTS -- How To Work\n"
"#+description: Session workflow, output format and fleet etiquette.\n"
"\n"
"This is {{name}}'s workspace. The org files here are the source of\n"
"truth; ~AGENTS.md~ and ~CLAUDE.md~ are loaders that point at them, so a\n"
"tool looking for either finds the same set.\n"
"\n"
"Note that ~AGENTS.org~ (this file) and ~AGENTS.md~ (the loader) are\n"
"different things. Edit this one.\n"
"\n"
"* At the start of a turn\n"
"\n"
"1. Read the message you were given. It arrived through your mailbox and\n"
"   may have been queued while you were stopped -- do not assume it is\n"
"   about whatever you were last doing.\n"
"2. Check ~from~. A message from another agent is a request from a peer,\n"
"   not an instruction from your human.\n"
"3. Do the work. Report what happened.\n"
"\n"
"* Talking to the fleet\n"
"\n"
"See ~TOOLS.org~ for the tools. The etiquette:\n"
"\n"
"- *Ask, do not assume.* ~clawtilla_ask_agent~ blocks until the other\n"
"  agent answers. Use it when you need the answer to continue.\n"
"- *Delegate, do not wait.* ~clawtilla_delegate~ hands work over and\n"
"  returns a task id immediately. Use it when you do not.\n"
"- *Say why.* Every delegation carries a reason. The agent on the other\n"
"  end has none of your context.\n"
"- *Do not loop.* If a conversation with another agent has been round\n"
"  twice without progress, stop and tell your human. The daemon enforces\n"
"  a hop limit, but hitting it is a failure, not a safety net.\n"
"- *Acknowledge.* ~clawtilla_mailbox_ack~ when you have handled an item.\n"
"  An unacked item is redelivered when the lease expires, and doing the\n"
"  same work twice is worse than not doing it.\n"
"\n"
"* Reporting failure\n"
"\n"
"Say what failed, what you tried, and what would resolve it. \"It did not\n"
"work\" sends the next agent down the same path you already eliminated.\n"
"\n"
"* Output format\n"
"\n"
"/Adjust to taste./ Markdown by default. If your human reads your output\n"
"in an org buffer, write org and say so here.\n";

static const gchar TOOLS_ORG[] =
"#+title: TOOLS -- What You Actually Have\n"
"#+description: Running under clawtilla: orchestration, mailbox, computer.\n"
"\n"
"* You are running under clawtilla\n"
"\n"
"clawtilla is a daemon that runs a fleet of agents. You are one of them.\n"
"It owns every process, credential and socket; you reach the rest of the\n"
"fleet through it and never directly.\n"
"\n"
"Your agent id is ~{{id}}~. That is the name every other agent uses to\n"
"address you, and the name you use to address them.\n"
"\n"
"* Talking to other agents\n"
"\n"
"These arrive as ordinary tools over your link to the daemon.\n"
"\n"
"| Tool                       | What it does                                        |\n"
"|----------------------------+-----------------------------------------------------|\n"
"| ~clawtilla_list_agents~    | Who exists, their state and what they are for       |\n"
"| ~clawtilla_get_agent~      | One agent in detail, including its description      |\n"
"| ~clawtilla_message_agent~  | Queue a message. Returns immediately; no reply      |\n"
"| ~clawtilla_ask_agent~      | Send and *wait* for the reply, with a timeout       |\n"
"| ~clawtilla_post_room~      | Say something to every member of a room             |\n"
"| ~clawtilla_create_room~    | Make a room with named members                      |\n"
"| ~clawtilla_room_history~   | What was said in a room                             |\n"
"\n"
"*Pick the right one.* ~message_agent~ when you are informing; ~ask_agent~\n"
"when you cannot continue without the answer; ~delegate~ when the work is\n"
"theirs and you have other things to do.\n"
"\n"
"** Priority\n"
"\n"
"~clawtilla_message_agent~ takes a priority: ~low~, ~normal~, ~high~,\n"
"~urgent~. Urgent overtakes everything already queued. Use it when a\n"
"person is blocked, not to make your own work go faster -- if everything\n"
"is urgent, the queue is just a queue again.\n"
"\n"
"* Delegating work\n"
"\n"
"| Tool                       | What it does                                        |\n"
"|----------------------------+-----------------------------------------------------|\n"
"| ~clawtilla_delegate~       | Hand a task over; returns a task id at once         |\n"
"| ~clawtilla_task_status~    | Where a task got to                                 |\n"
"| ~clawtilla_task_result~    | What it produced                                    |\n"
"| ~clawtilla_task_complete~  | Report *your* task finished, with its result        |\n"
"| ~clawtilla_task_cancel~    | Stop one                                            |\n"
"\n"
"A delegated task gets its own session on the other end, so one job never\n"
"contaminates the next. If work was delegated *to you*, finish with\n"
"~clawtilla_task_complete~ -- otherwise whoever delegated it waits on a\n"
"task that is already done.\n"
"\n"
"* Your mailbox\n"
"\n"
"| Tool                       | What it does                                        |\n"
"|----------------------------+-----------------------------------------------------|\n"
"| ~clawtilla_mailbox_list~   | What is waiting                                     |\n"
"| ~clawtilla_mailbox_read~   | One item in full                                    |\n"
"| ~clawtilla_mailbox_ack~    | Mark one handled                                    |\n"
"| ~clawtilla_mailbox_reply~  | Answer the sender of one                            |\n"
"\n"
"It is durable and survives you being stopped. An item handed to you is\n"
"*leased*: if you die mid-turn it returns to the queue and is redelivered,\n"
"which is why acknowledging matters. Repeated failures dead-letter an item\n"
"rather than dropping it silently.\n"
"\n"
"* Your computer\n"
"\n"
"{{computer_blurb}}\n"
"\n"
"| Tool                        | What it does                                       |\n"
"|-----------------------------+----------------------------------------------------|\n"
"| ~clawtilla_computer_exec~   | Run a command; returns stdout, stderr and the code |\n"
"| ~clawtilla_computer_state~  | Whether it is up                                   |\n"
"\n"
"Check the exit code. It is the real one.\n"
"\n"
"* Handing files to other agents\n"
"\n"
"The exchange directory is mounted into every computer at\n"
"~/mnt/clawtilla/exchange~:\n"
"\n"
"- ~exchange/shared/~ -- read-write for everyone\n"
"- ~exchange/{{id}}/~ -- read-write for you, read-only for the others\n"
"- everything else there -- read-only\n"
"\n"
"Write a file into your own directory and tell the other agent where it\n"
"is. Do not try to write into theirs.\n"
"\n"
"* Limits worth knowing\n"
"\n"
"The daemon enforces a hop limit, a per-minute message rate and a cost\n"
"budget per task, and it detects two agents repeating themselves at each\n"
"other. Hitting one of these is a bug in how the work was split, not an\n"
"obstacle to route around.\n"
"\n"
"* Everything else\n"
"\n"
"/Fill this in./ MCP servers, project-specific commands, anything this\n"
"agent has that the others do not.\n";

static const gchar TOOL_GOTCHAS_ORG[] =
"#+title: TOOL_GOTCHAS -- Known Quirks & Footguns\n"
"#+description: Traps already discovered. Append as you find more.\n"
"\n"
"Check here before debugging something that was probably already solved.\n"
"This file is meant to grow: when you lose an hour to a quirk, write it\n"
"down here so the next turn does not lose the same hour.\n"
"\n"
"* clawtilla\n"
"\n"
"*A stopped agent still accepts mail.* ~clawtilla_message_agent~ succeeds\n"
"whether or not the recipient is running -- that is what makes the mailbox\n"
"worth having. It is not an acknowledgement that anyone read it. Check\n"
"~clawtilla_get_agent~ if you need the message acted on now.\n"
"\n"
"*~ask_agent~ can time out.* The other agent may be stopped, busy, or\n"
"working on something long. A timeout is not an answer of \"no\".\n"
"\n"
"*Your own reply ends the turn.* Post the result once you have it. A\n"
"delegator waiting on ~clawtilla_task_result~ sees nothing until you do.\n"
"\n"
"* /Add your own below./\n";

static const gchar PROJECTS_ORG[] =
"#+title: PROJECTS -- What Is In Flight\n"
"#+description: The work {{id}} is responsible for and where it lives.\n"
"\n"
"/Fill this in./ One heading per project: where the code is, what state it\n"
"is in, what is next. Keep it current -- a stale project map is worse than\n"
"none, because it is believed.\n"
"\n"
"* Example project\n"
"\n"
"- *Where:* ~/path/to/it~\n"
"- *State:* /what works, what does not/\n"
"- *Next:* /the one thing to do next/\n";

static const gchar README_ORG[] =
"#+title: {{name}}\n"
"#+description: The workspace for the clawtilla agent {{id}}.\n"
"\n"
"* What this is\n"
"\n"
"The workspace for ~{{id}}~, an agent in a clawtilla fleet. The org files\n"
"here are its identity: they are concatenated into its system prompt at\n"
"the start of every session, in the order listed in ~AGENTS.md~.\n"
"\n"
"Edit them. That is the point. ~clawtilla agent edit {{id}}~ opens the set\n"
"in your ~$EDITOR~.\n"
"\n"
"* Layout\n"
"\n"
"| File               | Purpose                                       |\n"
"|--------------------+-----------------------------------------------|\n"
"| ~SOUL.org~         | Character, mission, voice                     |\n"
"| ~IDENTITY.org~     | Name, role, self-description                  |\n"
"| ~USER.org~         | Who the human is and how they work            |\n"
"| ~AGENTS.org~       | How to work: format, safety, fleet etiquette  |\n"
"| ~TOOLS.org~        | What this agent actually has                  |\n"
"| ~TOOL_GOTCHAS.org~ | Footguns, appended as they are found          |\n"
"| ~PROJECTS.org~     | What is in flight and where it lives          |\n"
"| ~README.org~       | This file. Not loaded into the prompt         |\n"
"| ~AGENTS.md~        | Loader: the org files, in order               |\n"
"| ~CLAUDE.md~        | Loader: one line pointing at ~AGENTS.md~      |\n"
"\n"
"~AGENTS.org~ and ~AGENTS.md~ are different things: the org file is\n"
"content, the markdown file is the loader.\n"
"\n"
"* Also here\n"
"\n"
"~MEMORY.md~ is written by the agent itself and loaded every turn under a\n"
"size budget. It is not part of this set and is not scaffolded -- the\n"
"agent creates it when it has something to remember.\n";

/*
 * The loader.
 *
 * Markdown rather than org because this is the file the agent runtime
 * looks for, and it is deliberately nothing but a list: the content is
 * in the org files, and a loader that also holds content is one that
 * drifts from them.
 */
static const gchar AGENTS_MD[] =
"# Bootstrap\n"
"\n"
"Read the following in order:\n"
"\n"
"{{includes}}\n"
"\n"
"The `.org` files are the source of truth. This file is only the list.\n";

static const gchar CLAUDE_MD[] =
"@AGENTS.md\n";

/* ── Scaffolding ─────────────────────────────────────────────────── */

static const gchar *
template_for(const gchar *name)
{
    if (g_strcmp0(name, "SOUL.org") == 0)         return SOUL_ORG;
    if (g_strcmp0(name, "IDENTITY.org") == 0)     return IDENTITY_ORG;
    if (g_strcmp0(name, "USER.org") == 0)         return USER_ORG;
    if (g_strcmp0(name, "AGENTS.org") == 0)       return AGENTS_ORG;
    if (g_strcmp0(name, "TOOLS.org") == 0)        return TOOLS_ORG;
    if (g_strcmp0(name, "TOOL_GOTCHAS.org") == 0) return TOOL_GOTCHAS_ORG;
    if (g_strcmp0(name, "PROJECTS.org") == 0)     return PROJECTS_ORG;
    if (g_strcmp0(name, "README.org") == 0)       return README_ORG;
    if (g_strcmp0(name, "AGENTS.md") == 0)        return AGENTS_MD;
    if (g_strcmp0(name, "CLAUDE.md") == 0)        return CLAUDE_MD;

    return NULL;
}

/*
 * Describes the agent's computer in the second person.
 *
 * Written here rather than left to the agent to discover, because an
 * agent that does not know it has no computer spends its first turns
 * trying to run commands, and one that does not know its container is
 * isolated assumes it can see the host's files.
 */
static gchar *
computer_blurb(ClawtAgentConfig *agent)
{
    const gchar *type = clawt_agent_config_get_string(agent, "computer.type");

    if (type == NULL || g_strcmp0(type, "none") == 0)
        return g_strdup(
            "*You have no computer.* There is nothing to run commands on: "
            "the computer tools below will refuse. You work through "
            "conversation and the other agents' computers.");

    if (g_strcmp0(type, "container") == 0)
        return g_strdup_printf(
            "*You have a container of your own, named ~clawt-%s~.* It is\n"
            "isolated from the host: its filesystem is yours, and nothing\n"
            "you do in it touches the machine clawtilla runs on. Paths\n"
            "mounted in from the host are listed by\n"
            "~clawtilla_computer_state~.\n"
            "\n"
            "*Run every shell command in it, with\n"
            "~clawtilla_computer_exec~.* This is the part that is easy to\n"
            "get wrong: you run as a process on the host, so your own\n"
            "bash, read and write tools operate on the host filesystem,\n"
            "not in your container. A command that checks for\n"
            "~/.dockerenv~ and finds nothing is telling you it ran in the\n"
            "wrong place.\n"
            "\n"
            "Touch the host only when the user asks you to, and say so\n"
            "when you do.",
            clawt_agent_config_get_id(agent));

    if (g_strcmp0(type, "vm") == 0)
        return g_strdup_printf(
            "*You have a virtual machine of your own, named\n"
            "~clawt-%s~.* Isolated from the host, with its own kernel. It\n"
            "takes noticeably longer to start than a container, so do not\n"
            "restart it casually.\n"
            "\n"
            "*Run every shell command in it, with\n"
            "~clawtilla_computer_exec~.* You run as a process on the\n"
            "host, so your own bash, read and write tools operate on the\n"
            "host filesystem, not in your VM.\n"
            "\n"
            "Touch the host only when the user asks you to, and say so\n"
            "when you do.",
            clawt_agent_config_get_id(agent));

    if (g_strcmp0(type, "host") == 0)
        return g_strdup(
            "*You have the real machine clawtilla runs on.* Real files, "
            "real processes, real network. Confinement is in force and "
            "will refuse paths outside it -- ~clawtilla_computer_state~ "
            "says which mode and what it allows. Treat every command as "
            "affecting a machine somebody uses, because it does.");

    return g_strdup_printf(
        "*You have a computer of type ~%s~.* Ask "
        "~clawtilla_computer_state~ what it can do.", type);
}

static gchar *
build_includes(void)
{
    g_auto(GStrv) names = clawt_workspace_identity_files();
    GString *out = g_string_new(NULL);
    gsize i;

    for (i = 0; names[i] != NULL; i++)
        g_string_append_printf(out, "- @%s\n", names[i]);

    /* No trailing newline: the template supplies the spacing. */
    if (out->len > 0)
        g_string_truncate(out, out->len - 1);

    return g_string_free(out, FALSE);
}

static GHashTable *
build_values(ClawtAgentConfig *agent)
{
    GHashTable *values = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    const gchar *id = clawt_agent_config_get_id(agent);
    const gchar *name = clawt_agent_config_get_string(agent, "name");
    const gchar *description = clawt_agent_config_get_string(agent,
                                                             "description");
    const gchar *provider = clawt_agent_config_get_string(agent,
                                                          "model.provider");
    const gchar *model = clawt_agent_config_get_string(agent, "model.model");
    const gchar *type = clawt_agent_config_get_string(agent, "computer.type");

    g_hash_table_insert(values, g_strdup("id"), g_strdup(id));
    g_hash_table_insert(values, g_strdup("name"),
                        g_strdup(name != NULL ? name : id));
    g_hash_table_insert(values, g_strdup("description"),
                        g_strdup((description != NULL && *description != '\0')
                                 ? description
                                 : "/Say what this agent is for./"));
    g_hash_table_insert(values, g_strdup("role"),
                        g_strdup((description != NULL && *description != '\0')
                                 ? description : "/(fill in)/"));
    g_hash_table_insert(values, g_strdup("provider"),
                        g_strdup(provider != NULL ? provider : "(default)"));
    g_hash_table_insert(values, g_strdup("model"),
                        g_strdup(model != NULL ? model : "(default)"));
    g_hash_table_insert(values, g_strdup("computer"),
                        g_strdup(type != NULL ? type : "none"));
    g_hash_table_insert(values, g_strdup("computer_blurb"),
                        computer_blurb(agent));
    g_hash_table_insert(values, g_strdup("includes"), build_includes());

    /*
     * The human is named from the environment rather than left blank:
     * "your human" reads as a placeholder nobody filled in, and the
     * account name is right often enough to be worth starting from.
     */
    g_hash_table_insert(values, g_strdup("user"),
                        g_strdup(g_get_real_name() != NULL &&
                                 g_strcmp0(g_get_real_name(), "Unknown") != 0
                                 ? g_get_real_name()
                                 : g_get_user_name()));

    return values;
}

/*
 * Where clawtilla-mcp-server is.
 *
 * Beside the binary that is running first, so a build tree works
 * uninstalled and a daemon started from a checkout does not hand its
 * agents a path to a different install.  The compiled-in bindir is the
 * fallback, and a bare name lets PATH decide when neither exists.
 */
static gchar *
mcp_server_path(void)
{
    g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe != NULL) {
        g_autofree gchar *dir = g_path_get_dirname(exe);
        g_autofree gchar *beside = g_build_filename(dir,
                                                     "clawtilla-mcp-server",
                                                     NULL);

        if (g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&beside);
    }

#ifdef CLAWT_BINDIR
    {
        g_autofree gchar *installed =
            g_build_filename(CLAWT_BINDIR, "clawtilla-mcp-server", NULL);

        if (g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&installed);
    }
#endif

    return g_strdup("clawtilla-mcp-server");
}

gboolean
clawt_workspace_write_mcp_config(ClawtAgentConfig *agent,
                                 const gchar      *daemon_socket,
                                 const gchar      *state_dir,
                                 GError          **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    g_autoptr(JsonObject) out = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *existing_text = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *server = NULL;
    g_autofree gchar *token_file = NULL;
    JsonObject *clawtilla;
    JsonObject *servers;
    JsonObject *previous = NULL;
    JsonArray *args;
    GList *members = NULL;
    GList *l;
    gboolean replaced = FALSE;
    g_autofree gchar *workspace = NULL;

    g_return_val_if_fail(agent != NULL, FALSE);

    workspace = clawt_agent_config_get_workspace(agent);

    if (workspace == NULL)
        return TRUE;

    server = mcp_server_path();
    token_file = g_build_filename(state_dir, "token", NULL);
    path = g_build_filename(workspace, ".mcp.json", NULL);

    /*
     * clawtilla's own entry, rebuilt every time.
     *
     * It carries the socket and the token path, and a copy left behind
     * by a daemon that used to live somewhere else points the agent at
     * nothing.
     */
    clawtilla = json_object_new();
    json_object_set_string_member(clawtilla, "command", server);

    args = json_array_new();
    json_array_add_string_element(args, "--agent");
    json_array_add_string_element(args, clawt_agent_config_get_id(agent));

    if (daemon_socket != NULL) {
        json_array_add_string_element(args, "--socket");
        json_array_add_string_element(args, daemon_socket);
    }

    json_array_add_string_element(args, "--token-file");
    json_array_add_string_element(args, token_file);
    json_object_set_array_member(clawtilla, "args", args);

    /*
     * Everything else in the file is the user's, and is read back rather
     * than regenerated.
     *
     * This file is how an agent is given MCP servers, so it is a file
     * people edit -- and it used to be rewritten wholesale on every
     * start, which meant a server added by hand survived exactly until
     * the agent was next restarted. Only the "clawtilla" key is managed;
     * the rest is carried across untouched, including any top-level key
     * clawtilla has never heard of.
     */
    if (g_file_get_contents(path, &existing_text, NULL, NULL) &&
        json_parser_load_from_data(parser, existing_text, -1, NULL)) {
        JsonNode *node = json_parser_get_root(parser);

        if (node != NULL && JSON_NODE_HOLDS_OBJECT(node)) {
            out = json_object_ref(json_node_get_object(node));

            if (json_object_has_member(out, "mcpServers") &&
                JSON_NODE_HOLDS_OBJECT(json_object_get_member(out,
                                                              "mcpServers")))
                previous = json_object_get_object_member(out, "mcpServers");
        }
    } else if (existing_text != NULL) {
        /*
         * Unparseable, and moved aside rather than overwritten: it is
         * something a person wrote, and a stray comma is not a reason to
         * delete their work without a copy of it.
         */
        g_autofree gchar *aside = g_strconcat(path, ".bad", NULL);

        g_warning("%s is not valid JSON; keeping it as %s",
                  path, aside);
        g_rename(path, aside);
    }

    if (out == NULL)
        out = json_object_new();

    /*
     * Rebuilt in place rather than appended to, so clawtilla's entry
     * keeps whatever position it had. Moving it would make the file
     * differ from itself on the next start and rewrite it under an open
     * editor for no reason.
     */
    servers = json_object_new();

    if (previous != NULL)
        members = json_object_get_members(previous);

    for (l = members; l != NULL; l = l->next) {
        const gchar *name = l->data;

        if (g_strcmp0(name, "clawtilla") == 0) {
            json_object_set_object_member(servers, name, clawtilla);
            replaced = TRUE;
            continue;
        }

        json_object_set_member(servers, name,
                               json_node_copy(json_object_get_member(previous,
                                                                     name)));
    }

    g_list_free(members);

    if (!replaced)
        json_object_set_object_member(servers, "clawtilla", clawtilla);

    json_object_set_object_member(out, "mcpServers", servers);

    root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, out);
    json_generator_set_root(generator, root);
    json_generator_set_pretty(generator, TRUE);
    text = json_generator_to_data(generator, NULL);

    /*
     * Not written when nothing changed. An editor with the file open
     * reloads it on every daemon start otherwise, and the agent's own
     * workspace is a directory people keep in git.
     */
    if (g_strcmp0(existing_text, text) == 0)
        return TRUE;

    return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

gboolean
clawt_workspace_scaffold(ClawtAgentConfig *agent, GError **error)
{
    g_autoptr(GHashTable) values = NULL;
    g_autofree gchar *workspace = NULL;
    guint i;

    g_return_val_if_fail(agent != NULL, FALSE);

    workspace = clawt_agent_config_get_workspace(agent);

    if (workspace == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "agent '%s' has no workspace to scaffold",
                    clawt_agent_config_get_id(agent));
        return FALSE;
    }

    if (!clawt_ensure_dir(workspace, 0700, error))
        return FALSE;

    values = build_values(agent);

    for (i = 0; i < G_N_ELEMENTS(workspace_files); i++) {
        g_autofree gchar *path = NULL;
        g_autofree gchar *content = NULL;
        const gchar *template;

        path = g_build_filename(workspace, workspace_files[i].name, NULL);

        /*
         * Never overwritten.  These are meant to be edited, and a start
         * that quietly restored the defaults would throw away the exact
         * work this exists to make possible.
         */
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            continue;

        template = template_for(workspace_files[i].name);

        if (template == NULL)
            continue;

        content = expand(template, values);

        if (!clawt_write_file_atomic(path, content, -1, 0600, FALSE, error))
            return FALSE;
    }

    return TRUE;
}
