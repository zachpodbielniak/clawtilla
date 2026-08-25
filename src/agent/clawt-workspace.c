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

/*
 * The persona an imported workspace already has.
 *
 * clawtilla names its identity files in org; a workspace that grew up
 * somewhere else -- a Claude Code home, a standalone libreclaw instance
 * -- keeps the same concerns in markdown. Those two sets never collide,
 * so an import used to copy a complete persona in and then scaffold a
 * blank .org beside every file of it, and the agent loaded the blanks.
 * It looked created and it had no identity.
 *
 * Only reported when the workspace has *no* .org of its own: one that
 * already speaks clawtilla's spelling has made its choice, and a
 * leftover README.md is not a persona.
 *
 * Returns: (transfer full) (nullable): the files found, in load order,
 *   or %NULL if this workspace has nothing to adopt
 */
GStrv
clawt_workspace_detect_identity_files(const gchar *workspace)
{
    g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func(g_free);
    guint i;

    g_return_val_if_fail(workspace != NULL, NULL);

    for (i = 0; i < G_N_ELEMENTS(workspace_files); i++) {
        g_autofree gchar *org = NULL;
        g_autofree gchar *markdown = NULL;
        g_autofree gchar *stem = NULL;
        const gchar *dot;

        if (!workspace_files[i].identity)
            continue;

        org = g_build_filename(workspace, workspace_files[i].name, NULL);

        /*
         * One .org of clawtilla's own is enough to say this workspace
         * is already ours. Adopting markdown alongside it would load
         * the same concern twice and let the two disagree.
         */
        if (g_file_test(org, G_FILE_TEST_EXISTS))
            return NULL;

        dot = strrchr(workspace_files[i].name, '.');

        if (dot == NULL)
            continue;

        stem = g_strndup(workspace_files[i].name,
                         (gsize)(dot - workspace_files[i].name));
        markdown = g_strconcat(stem, ".md", NULL);

        {
            g_autofree gchar *path = g_build_filename(workspace, markdown,
                                                      NULL);

            if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
                g_ptr_array_add(found, g_steal_pointer(&markdown));
        }
    }

    if (found->len == 0)
        return NULL;

    g_ptr_array_add(found, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&found), FALSE);
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
"- *Ask, do not assume.* ~clawtilla_ask_agent~ sends the question and\n"
"  returns at once. It does not wait: the answer comes back later as an\n"
"  ordinary message, in its own turn.\n"
"- *Delegate when you need to know it finished.* ~clawtilla_delegate~\n"
"  hands work over and gives you a task id, and ~clawtilla_task_status~\n"
"  is the only handle that tells you where it got to.\n"
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
"| ~clawtilla_ask_agent~      | Ask a question; the answer arrives later as mail     |\n"
"| ~clawtilla_post_room~      | Say something to every member of a room             |\n"
"| ~clawtilla_create_room~    | Make a room with named members                      |\n"
"| ~clawtilla_room_history~   | What was said in a room                             |\n"
"\n"
"*Pick the right one.* ~message_agent~ when you are informing;\n"
"~ask_agent~ when you want an answer but can carry on without it --\n"
"it *does not wait*, it queues the question and returns, and the reply\n"
"arrives later as an ordinary message; ~delegate~ when you need to know\n"
"the work finished, because a task id is the only thing you can check.\n"
"\n"
"** Priority\n"
"\n"
"~clawtilla_message_agent~ takes a priority: ~low~, ~normal~, ~high~,\n"
"~urgent~. Urgent overtakes everything already queued. Use it when a\n"
"person is blocked, not to make your own work go faster -- if everything\n"
"is urgent, the queue is just a queue again.\n"
"\n"
"** Reaching your operator\n"
"\n"
"~clawtilla_message_user~ is the only way to say something to the human.\n"
"\n"
"This matters more than it sounds. Your reply goes back into whatever\n"
"conversation the message came from -- so if your operator asks you to\n"
"find something out from another agent, and you ask, and they answer,\n"
"*replying reaches them and not your operator*. The operator is left\n"
"asking whether you ever heard back. When you have the answer they were\n"
"waiting for, tell them with this tool.\n"
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
"| ~clawtilla_mailbox_list~   | What queued up while you were stopped               |\n"
"| ~clawtilla_mailbox_read~   | One item in full                                    |\n"
"| ~clawtilla_mailbox_ack~    | Mark one handled                                    |\n"
"| ~clawtilla_mailbox_reply~  | Answer the sender of one                            |\n"
"\n"
"*Your mailbox is empty while you are running, and that means nothing.*\n"
"A message is acknowledged the moment it reaches you and arrives as an\n"
"ordinary turn in this conversation -- the queue only holds what came in\n"
"while you were stopped. Checking here to find out whether somebody\n"
"answered you will always say no.\n"
"\n"
"To see whether a peer replied, read the conversation:\n"
"~clawtilla_room_history~ with their agent id. Every exchange between two\n"
"agents is a room of its own, so that is where the answer is.\n"
"\n"
"It is durable and survives you being stopped. An item handed to you is\n"
"*leased*: if you die mid-turn it returns to the queue and is redelivered,\n"
"which is why acknowledging matters. Repeated failures dead-letter an item\n"
"rather than dropping it silently.\n"
"\n"
"* Knowing when to stop\n"
"\n"
"A conversation between two agents has no natural end, and both of you\n"
"are built to be helpful. \"Idle.\" answered with \"Idle.\" is a loop, and\n"
"it will run until a limit stops it.\n"
"\n"
"If you have nothing to add, *send nothing*. Not an acknowledgement, not\n"
"a note that there is nothing to add -- those are messages too, and they\n"
"are answered. Ending your turn without sending is how a conversation\n"
"finishes.\n"
"\n"
"* Your memory\n"
"\n"
"You have a searchable store of your own, separate from every other\n"
"agent's. Nobody else can read it unless the operator says so, and you\n"
"cannot read theirs.\n"
"\n"
"| Tool                        | What it does                              |\n"
"|-----------------------------+-------------------------------------------|\n"
"| ~clawtilla_memory_search~   | Find what you already know                |\n"
"| ~clawtilla_memory_add~      | Write something down                      |\n"
"| ~clawtilla_memory_list~     | Recent memories, pinned ones first        |\n"
"| ~clawtilla_memory_get~      | One in full                               |\n"
"| ~clawtilla_memory_pin~      | Keep one at the top                       |\n"
"| ~clawtilla_memory_forget~   | Archive one that turned out to be wrong   |\n"
"\n"
"*Search before you ask.* The operator should not have to tell you the\n"
"same thing twice, and you should not work out the same thing twice.\n"
"Doing this at the start of a turn is cheaper than either.\n"
"\n"
"Write down what you would be worse off not knowing next time:\n"
"\n"
"- a decision, and *why* -- the reasoning is the part that is hard to\n"
"  reconstruct\n"
"- something the operator prefers, or has told you not to do\n"
"- a fact that cost you a turn to establish\n"
"- a footgun you hit, so you do not hit it twice\n"
"- what a peer is actually good at, once you find out\n"
"\n"
"Do *not* write down what is already in a file you can read again. A\n"
"memory of your own org files is a copy that goes stale.\n"
"\n"
"Categories: general, decision, preference, fact, project, learning,\n"
"insight, todo, relationship, technical, workflow, debug, research,\n"
"config, personal. Importance: low, normal, high, critical. Use the\n"
"shared vocabulary -- a category you invented is a tag.\n"
"\n"
"Forgetting archives rather than deletes, so being wrong about what to\n"
"forget is recoverable.\n"
"\n"
"* Keeping these files true\n"
"\n"
"These org files are your instructions, and they are *yours to edit*.\n"
"clawtilla writes them once and then leaves them alone. Nobody else is\n"
"going to keep them current, and everything you learn is lost at the end\n"
"of the conversation unless it lands in one of them.\n"
"\n"
"So while you work -- with the user, or with another agent -- write down\n"
"what you have just learned, at the point you learn it:\n"
"\n"
"| What you found                              | Where it goes         |\n"
"|---------------------------------------------+-----------------------|\n"
"| A tool you were given, or how to use it well | ~TOOLS.org~           |\n"
"| A tool that lied, or a footgun               | ~TOOL_GOTCHAS.org~    |\n"
"| A rule you must never break                  | ~AGENTS.org~          |\n"
"| How this person works, what they prefer      | ~USER.org~            |\n"
"| What you are for, in your own words          | ~IDENTITY.org~        |\n"
"| Work in flight and where it lives            | ~PROJECTS.org~        |\n"
"\n"
"Two things make this worth doing rather than a chore:\n"
"\n"
"- *Write the reason, not just the rule.* \"Use the host path with\n"
"  ~read~\" is forgettable; \"~read~ runs on the host, so a path from\n"
"  inside the VM names a file it cannot open\" is not. A rule with its\n"
"  reason survives being read by a version of you that has forgotten\n"
"  everything else.\n"
"- *Write it when you find it.* A note you meant to add at the end of\n"
"  the task is a note nobody writes.\n"
"\n"
"This is not the same as your memories, and the difference is worth\n"
"holding on to: a memory is something you *learned*, searchable and\n"
"dated; these files are what you *are*, and they are loaded into every\n"
"turn whether you go looking or not. A hard rule belongs here, because\n"
"a rule you have to remember to search for is a rule you will break.\n"
"\n"
"** What is not yours\n"
"\n"
"- Anything between a ~# BEGIN clawtilla ...~ and ~# END clawtilla ...~\n"
"  marker is rewritten every time this agent starts. Editing inside it\n"
"  is work that will disappear; put your own notes outside the markers,\n"
"  in the same file.\n"
"- ~.mcp.json~ is a real config file: the ~clawtilla~ and\n"
"  ~clawtilla-*~ keys belong to clawtilla and are rewritten. The rest is\n"
"  yours and is left untouched.\n"
"- Do not rewrite a whole file to change one thing. These are also read\n"
"  by the person who set you up, and a file that lost their wording\n"
"  overnight is a file they stop trusting.\n"
"\n"
"If a hard rule you are given conflicts with something already written\n"
"here, say so rather than quietly following the newer one. Two rules\n"
"that contradict each other is the user's problem to settle, and they\n"
"cannot settle it if they do not know.\n"
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
"** exec takes a command, not a shell line\n"
"\n"
"Every argument is quoted and passed through as it stands, so shell\n"
"syntax is *not* interpreted: ~>~, ~|~, ~&&~, ~;~, ~*~ and ~$VAR~ arrive\n"
"at the command as literal text rather than doing anything. A redirect\n"
"written straight into ~clawtilla_computer_exec~ silently becomes an\n"
"argument, so the command appears to run, reports success, and has no\n"
"effect anywhere you look for it.\n"
"\n"
"Wrap it in a shell when you want shell behaviour:\n"
"\n"
"#+begin_src bash\n"
"bash -c \'echo hello > /tmp/note && wc -l < /tmp/note\'\n"
"#+end_src\n"
"\n"
"** Your tools run on the host; your shell does not\n"
"\n"
"~read~, ~write~, ~edit~ and ~bash~ run where clawtilla runs. Only\n"
"~clawtilla_computer_exec~ goes inside your computer. So a file has two\n"
"paths and which one to use depends on which tool you are holding:\n"
"\n"
"- opening it with ~read~ or ~write~ -- use the *host* path\n"
"- naming it as an argument to a command you run inside -- use the\n"
"  *inside* path\n"
"\n"
"Your computer's description above lists every share as\n"
"~host path = the path inside~. Looking for the inside path on the host\n"
"finds nothing, which reads exactly like a share that was never set up.\n"
"\n"
"* What is shared with your computer\n"
"\n"
"Two directories, always, unless somebody turned them off.\n"
"\n"
"** Your workspace -- ~/mnt/clawtilla/workspace~\n"
"\n"
"This directory. Your persona, your notes, your ~MEMORY.md~ and anything\n"
"you leave lying about are the same files on both sides, so something\n"
"written inside your computer is something you can then ~read~.\n"
"\n"
"That is the route for anything produced *in* your computer that you\n"
"then need to look at -- a screenshot, a log, a file you generated.\n"
"Write it under ~/mnt/clawtilla/workspace~ and read it back at the host\n"
"path.\n"
"\n"
"If you have a desktop, its screenshots are already written there, into\n"
"~/mnt/clawtilla/workspace/screenshots~. The tool returns the inside\n"
"path; read the host one.\n"
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
"Tell them the path *inside a computer*, not the host one: it is the\n"
"same for both of you, and the host path is not.\n"
"\n"
"# BEGIN clawtilla integrations -- rewritten on every start\n"
"\n"
"* Your integrations\n"
"\n"
"/Filled in when the daemon starts./ Anything written between these two\n"
"markers is replaced; the rest of this file is yours.\n"
"\n"
"# END clawtilla integrations\n"
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
"*~ask_agent~ does not wait.* It queues the question and answers\n"
"\"Queued for ...\" exactly like ~clawtilla_message_agent~ -- there is no\n"
"timeout, and the reply is never the result of that call. The answer\n"
"comes back later as an ordinary message. Until it does, your mailbox\n"
"says nothing, because delivery empties it; read\n"
"~clawtilla_room_history~ with the other agent's id to see whether they\n"
"answered.\n"
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
            "*To put something on the VM\'s screen*, write to\n"
            "~/dev/console~. That is the serial console, and it is what\n"
            "somebody watching this VM in virt-manager actually sees:\n"
            "\n"
            "#+begin_src bash\n"
            "bash -c 'echo \">>> hello from %s <<<\" > /dev/console'\n"
            "#+end_src\n"
            "\n"
            "The ~bash -c~ is not optional. ~clawtilla_computer_exec~\n"
            "passes its arguments through without a shell, so a bare\n"
            "~echo hello > /dev/console~ hands ~echo~ three words and\n"
            "writes nowhere -- and reports success while doing it.\n"
            "\n"
            "Touch the host only when the user asks you to, and say so\n"
            "when you do.",
            clawt_agent_config_get_id(agent),
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
 * Where one of clawtilla's own binaries is.
 *
 * Beside the binary that is running first, so a build tree works
 * uninstalled and a daemon started from a checkout does not hand its
 * agents a path to a different install.  The compiled-in bindir is the
 * fallback, and a bare name lets PATH decide when neither exists.
 */
static gchar *
sibling_binary_path(const gchar *name)
{
    g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe != NULL) {
        g_autofree gchar *dir = g_path_get_dirname(exe);
        g_autofree gchar *beside = g_build_filename(dir, name, NULL);

        if (g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&beside);
    }

#ifdef CLAWT_BINDIR
    {
        g_autofree gchar *installed =
            g_build_filename(CLAWT_BINDIR, name, NULL);

        if (g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&installed);
    }
#endif

    return g_strdup(name);
}

/*
 * One `mcp` integration, as an entry in the agent's .mcp.json.
 *
 * A command server and a URL server are two different shapes and the
 * validator has already refused an instance that is both, so exactly one
 * branch here is ever taken.
 */
static JsonObject *
build_integration_server(ClawtIntegrationBinding  *binding,
                         const gchar              *secrets_dir,
                         GError                  **error)
{
    g_autoptr(JsonObject) entry = json_object_new();
    g_autoptr(GHashTable) env = NULL;
    g_auto(GStrv) args = NULL;
    const gchar *command =
        clawt_integration_binding_get_string(binding, "command");
    const gchar *url = clawt_integration_binding_get_string(binding, "url");

    if (command == NULL && url == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "integration '%s': set either command or url",
                    clawt_integration_binding_get_name(binding));
        return NULL;
    }

    if (url != NULL) {
        json_object_set_string_member(entry, "type",
                                      g_str_has_suffix(url, "/sse") ? "sse"
                                                                    : "http");
        json_object_set_string_member(entry, "url", url);
        return g_steal_pointer(&entry);
    }

    json_object_set_string_member(entry, "command", command);

    args = clawt_integration_binding_get_string_list(binding, "args");

    if (args != NULL && args[0] != NULL) {
        JsonArray *array = json_array_new();
        guint i;

        for (i = 0; args[i] != NULL; i++)
            json_array_add_string_element(array, args[i]);

        json_object_set_array_member(entry, "args", array);
    }

    /*
     * The environment holds resolved secrets, which is why this file is
     * 0600 and why the values are fetched here rather than left as
     * references: the CLI that reads it has never heard of clawtilla's
     * secret backends.
     */
    env = clawt_integration_binding_resolve_env(binding, "env", secrets_dir,
                                                error);

    if (env == NULL)
        return NULL;

    if (g_hash_table_size(env) > 0) {
        JsonObject *object = json_object_new();
        g_autoptr(GList) names = g_hash_table_get_keys(env);
        GList *l;

        names = g_list_sort(g_steal_pointer(&names), (GCompareFunc)g_strcmp0);

        for (l = names; l != NULL; l = l->next)
            json_object_set_string_member(object, l->data,
                                          g_hash_table_lookup(env, l->data));

        json_object_set_object_member(entry, "env", object);
    }

    return g_steal_pointer(&entry);
}

/*
 * One `connector` integration, as an entry in the agent's .mcp.json.
 *
 * Names the clawtilla CLI and carries no credential, which is the whole
 * point: an `mcp` integration's environment is resolved into this file,
 * and this file is one the agent can read.  That is fine for a key
 * somebody chose to hand it and not fine for an OAuth grant on their
 * account.  The relay reads the token from a 0600 file the agent has no
 * reason to touch, and starts the server with it.
 *
 * It also means the entry stays correct across a token being renewed.
 * A credential written in here would be the one that was current when
 * the workspace was last rendered, which for an hour-long access token
 * is almost never the one that works.
 */
static JsonObject *
build_connector_server(ClawtConfig             *config,
                       ClawtIntegrationBinding *binding,
                       const gchar             *daemon_socket)
{
    JsonObject *entry = json_object_new();
    g_autofree gchar *cli = sibling_binary_path("clawtilla");
    JsonArray *args = json_array_new();
    const gchar *config_path = clawt_config_get_path(config);

    json_object_set_string_member(entry, "command", cli);

    /*
     * Both global options go before the verb.  The CLI splits its own
     * options from the subcommand's at the first verb it recognises, so
     * anything written after `connector` is handed to the subcommand,
     * which has never heard of it.
     */
    if (config_path != NULL) {
        json_array_add_string_element(args, "--config");
        json_array_add_string_element(args, config_path);
    }

    if (daemon_socket != NULL) {
        json_array_add_string_element(args, "--socket");
        json_array_add_string_element(args, daemon_socket);
    }

    json_array_add_string_element(args, "connector");
    json_array_add_string_element(args, "relay");
    json_array_add_string_element(args,
                                  clawt_integration_binding_get_name(binding));

    json_object_set_array_member(entry, "args", args);

    return entry;
}

/*
 * The entry that gives an agent with a VM desktop its screen.
 *
 * It names the clawtilla CLI rather than ssh, and it has to.  The port
 * that reaches the guest is chosen when the VM is provisioned -- after
 * this file is written, and again whenever the config changes -- so a
 * command line captured here would name a port nothing listens on. The
 * relay asks the daemon for the real one each time it is started, and
 * filters the tools on the way past.
 */
static JsonObject *
build_desktop_server(ClawtAgentConfig *agent, const gchar *daemon_socket)
{
    JsonObject *entry = json_object_new();
    g_autofree gchar *cli = sibling_binary_path("clawtilla");
    JsonArray *args = json_array_new();

    json_object_set_string_member(entry, "command", cli);

    /*
     * --socket goes before the verb.  The CLI splits its own options from
     * the subcommand's at the first verb it recognises, so a global
     * option written after `computer` is handed to the subcommand, which
     * has never heard of it.
     */
    if (daemon_socket != NULL) {
        json_array_add_string_element(args, "--socket");
        json_array_add_string_element(args, daemon_socket);
    }

    json_array_add_string_element(args, "computer");
    json_array_add_string_element(args, "desktop-mcp");
    json_array_add_string_element(args, clawt_agent_config_get_id(agent));

    json_object_set_array_member(entry, "args", args);

    return entry;
}

/*
 * Whether this agent has a desktop of its own to be given.
 *
 * Both halves are required: the grant, and a VM for the desktop to be
 * in.  A host desktop is reached without any of this.
 */
static gboolean
wants_guest_desktop(ClawtAgentConfig *agent)
{
    ClawtComputerType type;

    if (!clawt_agent_config_get_boolean(agent, "computer.desktop.enabled"))
        return FALSE;

    type = (ClawtComputerType)
        clawt_agent_config_get_enum(agent, "computer.type");

    return type == CLAWT_COMPUTER_VM;
}

gboolean
clawt_workspace_write_mcp_config(ClawtConfig      *config,
                                 ClawtAgentConfig *agent,
                                 const gchar      *daemon_socket,
                                 const gchar      *state_dir,
                                 GError          **error)
{
    g_autoptr(GPtrArray) bindings = NULL;
    g_autoptr(GHashTable) wanted = NULL;   /* key -> JsonObject*, owned */
    g_autofree gchar *secrets_dir = NULL;
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
    JsonObject *desktop = NULL;
    JsonObject *servers;
    JsonObject *previous = NULL;
    JsonArray *args;
    GList *members = NULL;
    GList *l;
    gboolean replaced = FALSE;
    gboolean replaced_desktop = FALSE;
    g_autofree gchar *workspace = NULL;

    g_return_val_if_fail(agent != NULL, FALSE);

    workspace = clawt_agent_config_get_workspace(agent);

    if (workspace == NULL)
        return TRUE;

    server = sibling_binary_path("clawtilla-mcp-server");
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

    if (wants_guest_desktop(agent))
        desktop = build_desktop_server(agent, daemon_socket);

    /*
     * One entry per `mcp` integration in scope for this agent.
     *
     * Built before the file is read, so a secret that cannot be resolved
     * fails the write outright rather than leaving the agent with a
     * server entry whose credentials are missing -- which starts, and
     * then fails on the first tool call, a long way from the cause.
     */
    wanted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify)json_object_unref);

    if (config != NULL) {
        guint i;

        secrets_dir = clawt_config_get_path_value(config, "secrets.dir");
        bindings = clawt_integration_resolve_for_agent(config, agent);

        for (i = 0; i < bindings->len; i++) {
            ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);
            JsonObject *entry;
            gchar *key;

            const gchar *type =
                clawt_integration_binding_get_info(binding)->id;

            if (g_strcmp0(type, "connector") == 0)
                entry = build_connector_server(config, binding,
                                               daemon_socket);
            else if (g_strcmp0(type, "mcp") == 0)
                entry = build_integration_server(binding, secrets_dir, error);
            else
                continue;

            if (entry == NULL)
                return FALSE;

            key = g_strdup_printf("clawtilla-%s",
                                  clawt_integration_binding_get_name(binding));
            g_hash_table_insert(wanted, key, entry);
        }
    }

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

        /*
         * The other keys clawtilla owns.  They are prefixed so they cannot
         * collide with a `desktop` or a `github` somebody added
         * themselves, and one that should no longer be there is *dropped*
         * rather than carried: a stale `clawtilla-desktop` starts an ssh
         * to a VM that is not there, and a stale integration entry points
         * at a server the fleet has stopped granting.
         *
         * The prefix is therefore reserved. A server of your own called
         * `clawtilla-anything` will be removed on the next start.
         */
        if (g_strcmp0(name, "clawtilla-desktop") == 0) {
            if (desktop != NULL) {
                json_object_set_object_member(servers, name, desktop);
                replaced_desktop = TRUE;
            }
            continue;
        }

        if (g_str_has_prefix(name, "clawtilla-")) {
            JsonObject *entry = g_hash_table_lookup(wanted, name);

            if (entry != NULL) {
                json_object_set_object_member(servers, name,
                                              json_object_ref(entry));
                g_hash_table_remove(wanted, name);
            }

            continue;
        }

        json_object_set_member(servers, name,
                               json_node_copy(json_object_get_member(previous,
                                                                     name)));
    }

    g_list_free(members);

    if (!replaced)
        json_object_set_object_member(servers, "clawtilla", clawtilla);

    if (desktop != NULL && !replaced_desktop)
        json_object_set_object_member(servers, "clawtilla-desktop", desktop);

    /* Whatever was not already in the file, in a stable order. */
    {
        g_autoptr(GList) fresh = g_hash_table_get_keys(wanted);
        GList *entry;

        fresh = g_list_sort(g_steal_pointer(&fresh), (GCompareFunc)g_strcmp0);

        for (entry = fresh; entry != NULL; entry = entry->next) {
            const gchar *key = entry->data;

            json_object_set_object_member(
                servers, key,
                json_object_ref(g_hash_table_lookup(wanted, key)));
        }
    }

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

/*
 * The identity files this agent will actually load.
 *
 * Public, and used by the renderer as well as the scaffolder, because
 * they have to agree: one deciding to write a file the other will not
 * read is how a workspace ends up full of templates nobody loads. It was
 * mirrored in both for a while and that is exactly the shape of bug this
 * project keeps finding -- two implementations, and the less-exercised
 * one is wrong.
 */
GStrv
clawt_workspace_effective_identity_files(ClawtAgentConfig *agent)
{
    GStrv configured = clawt_agent_config_get_string_list(
        agent, "persona.identity_files");

    if (configured != NULL && configured[0] != NULL)
        return configured;

    g_strfreev(configured);

    if (clawt_agent_config_get_string(agent, "persona.system_prompt") != NULL)
        return g_new0(gchar *, 1);

    return clawt_workspace_identity_files();
}

static gboolean
strv_contains(const GStrv haystack, const gchar *needle)
{
    guint i;

    for (i = 0; haystack != NULL && haystack[i] != NULL; i++) {
        if (g_strcmp0(haystack[i], needle) == 0)
            return TRUE;
    }

    return FALSE;
}

gboolean
clawt_workspace_scaffold(ClawtAgentConfig *agent, GError **error)
{
    g_autoptr(GHashTable) values = NULL;
    g_autofree gchar *workspace = NULL;
    g_auto(GStrv) identity = NULL;
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
    identity = clawt_workspace_effective_identity_files(agent);

    for (i = 0; i < G_N_ELEMENTS(workspace_files); i++) {
        g_autofree gchar *path = NULL;
        g_autofree gchar *content = NULL;
        const gchar *template;

        /*
         * An identity file that is not in the effective list will never
         * be read, so writing it is not a starting point -- it is
         * clutter that contradicts the files that *are* read.
         *
         * This is what `agent import` did to a workspace that already
         * had a persona. The source kept its identity in SOUL.md and
         * friends; the scaffolder checks for SOUL.org, found none,
         * and wrote a blank parallel set beside them. Nothing collided
         * and nothing warned, and the imported agent loaded seven
         * templates saying "/(fill in)/" instead of the persona sitting
         * next to them.
         */
        if (workspace_files[i].identity &&
            !strv_contains(identity, workspace_files[i].name))
            continue;

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

/* ── The integrations section of TOOLS.org ───────────────────────── */

/*
 * The markers.
 *
 * Org comments, so they are invisible when the file is exported and
 * harmless when it is read as prose.  The text says what will happen to
 * anything written between them, because that is the one thing a person
 * opening this file needs to know before they start typing.
 */
static const gchar TOOLS_BEGIN[] =
    "# BEGIN clawtilla integrations -- rewritten on every start";
static const gchar TOOLS_END[] =
    "# END clawtilla integrations";

/*
 * The second region clawtilla owns, and the one that fixes a lie this
 * file used to tell.
 *
 * TOOLS.org listed the orchestration tools in a table written when the
 * workspace was scaffolded, and never again -- so a tool granted later
 * did not appear, and an agent reading its own file concluded, correctly
 * from what it could see, that it did not have one. A chief-of-staff
 * said exactly that about creating agents on the day the tool was added
 * to it.
 *
 * This region is written from the live tool list, by the daemon, which
 * is the only thing that knows both an agent's capabilities and its
 * permissions.
 */
static const gchar TOOL_LIST_BEGIN[] =
    "# BEGIN clawtilla tools -- rewritten on every start";
static const gchar TOOL_LIST_END[] =
    "# END clawtilla tools";

/*
 * What an agent is told about one integration.
 *
 * Written from the agent's side rather than the operator's: not "this
 * fleet has a Matrix account" but "messages will arrive from people, and
 * this is what answering one does".  The distinction matters most for a
 * channel, where nothing in a session reveals that the person on the
 * other end is in a room with four other people.
 */
static void
describe_integration(GString *out, ClawtIntegrationBinding *binding)
{
    const ClawtIntegrationInfo *info =
        clawt_integration_binding_get_info(binding);
    const gchar *name = clawt_integration_binding_get_name(binding);
    const gchar *description =
        clawt_integration_binding_get_string(binding, "description");

    g_string_append_printf(out, "** %s (~%s~)\n\n", name, info->id);

    if (description != NULL && *description != '\0')
        g_string_append_printf(out, "%s\n\n", description);

    if (g_strcmp0(info->id, "connector") == 0) {
        const gchar *provider =
            clawt_integration_binding_get_string(binding, "provider");
        const gchar *account =
            clawt_integration_binding_get_string(binding, "account");
        const gchar *scopes =
            clawt_integration_binding_get_string(binding, "scopes");
        g_auto(GStrv) tools =
            clawt_integration_binding_get_string_list(binding, "tools");

        g_string_append_printf(out,
            "Tools for %s appear beside your own, under\n"
            "~clawtilla-%s~.\n\n", provider != NULL ? provider : "a service",
            name);

        /*
         * The thing an agent most needs to know, and the thing it cannot
         * work out from the tool list: these reach a real account
         * belonging to a real person, not a sandbox of its own.
         */
        g_string_append(out,
            "This is somebody's actual account, not a test one. What you\n"
            "do through it is done as them and is visible to everyone\n"
            "else who can see it. Reading is cheap; anything that writes,\n"
            "sends or deletes is worth being sure about first.\n\n");

        if (account != NULL && *account != '\0')
            g_string_append_printf(out,
                "The account is ~%s~.\n\n", account);

        /*
         * Said out loud so it is not discovered by trying.  An agent
         * that goes looking for the credential wastes turns and leaves
         * something alarming in a transcript, and the honest answer is
         * that there is nothing to find.
         */
        g_string_append(out,
            "You do not have the credential and cannot get it. clawtilla\n"
            "holds it and hands it to the tool server, so there is no\n"
            "token in your config, your environment or anywhere else to\n"
            "look -- and no need for one.\n\n");

        if (scopes != NULL && *scopes != '\0')
            g_string_append_printf(out,
                "Access was granted for: %s. Anything outside that will\n"
                "be refused by the service rather than by clawtilla, so\n"
                "it is worth reading before assuming a failure was\n"
                "yours.\n\n", scopes);

        if (tools != NULL && tools[0] != NULL) {
            guint i;

            g_string_append(out, "Only these tools are available here:\n\n");

            for (i = 0; tools[i] != NULL; i++)
                g_string_append_printf(out, "- ~%s~\n", tools[i]);

            g_string_append_c(out, '\n');
        }

        return;
    }

    if (g_strcmp0(info->id, "matrix") == 0) {
        const gchar *user_id =
            clawt_integration_binding_get_string(binding, "user_id");
        g_auto(GStrv) rooms =
            clawt_integration_binding_get_string_list(binding, "rooms");

        g_string_append(out,
            "Messages from Matrix arrive as ordinary turns in your\n"
            "conversation, and your reply goes back to the room they came\n"
            "from. *There are people there.* Write as if somebody is\n"
            "reading it in a chat app on their phone, because they are:\n"
            "short, no preamble, and no wall of formatting.\n\n");

        if (user_id != NULL)
            g_string_append_printf(out,
                "You are ~%s~ there. Somebody addressing you by that name\n"
                "means you.\n\n", user_id);

        if (rooms != NULL && rooms[0] != NULL) {
            guint i;

            g_string_append(out, "Rooms:\n\n");

            for (i = 0; rooms[i] != NULL; i++)
                g_string_append_printf(out, "- ~%s~\n", rooms[i]);

            g_string_append_c(out, '\n');
        } else {
            g_string_append(out,
                "You are in every room this account has joined.\n\n");
        }

        if (clawt_integration_binding_get_boolean(binding, "require_mention"))
            g_string_append(out,
                "You only see messages that mention you, so anything that\n"
                "reaches you was addressed to you deliberately.\n\n");
        else
            g_string_append(out,
                "You see *every* message in these rooms, including ones\n"
                "between two other people. Most of them are not for you.\n"
                "Answering anyway is how a room decides to remove a bot.\n\n");

        /*
         * Worth saying explicitly.  Bridged rooms are the ordinary case
         * on a self-hosted homeserver, and an agent that reasons about
         * "Matrix users" will misjudge who it is talking to.
         */
        g_string_append(out,
            "A room may be bridged from Discord, Signal or elsewhere. It\n"
            "looks the same from here, but formatting and message length\n"
            "may be handled differently on the far side.\n\n");
        return;
    }

    if (g_strcmp0(info->id, "email") == 0) {
        const gchar *username =
            clawt_integration_binding_get_string(binding, "username");

        g_string_append(out,
            "Mail arrives as a turn and your reply is sent as a reply.\n"
            "It is not a chat: quote what you are answering, and remember\n"
            "that a mistake here is in somebody's inbox for ever.\n\n");

        if (username != NULL)
            g_string_append_printf(out, "The mailbox is ~%s~.\n\n", username);

        return;
    }

    if (g_strcmp0(info->id, "webhook") == 0) {
        g_string_append_printf(out,
            "Another service posts to you on port %" G_GINT64_FORMAT ".\n"
            "There is no person waiting on the other end, so a reply is\n"
            "read by a program: say what happened, once.\n\n",
            clawt_integration_binding_get_int(binding, "port"));
        return;
    }

    if (g_strcmp0(info->id, "local") == 0) {
        g_string_append(out,
            "Somebody is running you from a terminal and is watching this\n"
            "conversation as it happens.\n\n");
        return;
    }

    if (g_strcmp0(info->id, "cmacs") == 0) {
        g_string_append(out,
            "You are reachable from an Emacs session. Your operator is\n"
            "most likely in the middle of something else.\n\n");
        return;
    }

    if (g_strcmp0(info->id, "notify") == 0) {
        /*
         * Worth telling the agent, even though the agent is not
         * involved: it changes whether saying something is worth doing.
         * An operator who will actually be interrupted is one it makes
         * sense to stop and ask; an operator who will read it whenever
         * they next open a client is not.
         */
        g_string_append(out,
            "Your operator is notified when you say something to them\n"
            "with ~clawtilla_message_user~, and when you stop with an\n"
            "error. It reaches them wherever they are, not only when they\n"
            "next look at a client.\n"
            "\n"
            "So stopping to ask is a real option when you are genuinely\n"
            "blocked -- and interrupting somebody for anything less is a\n"
            "real cost. Both halves of that matter.\n\n");
        return;
    }

    if (g_strcmp0(info->id, "mcp") == 0) {
        const gchar *command =
            clawt_integration_binding_get_string(binding, "command");
        const gchar *url =
            clawt_integration_binding_get_string(binding, "url");

        g_string_append_printf(out,
            "An MCP server your session can call, listed in ~.mcp.json~ as\n"
            "~clawtilla-%s~. Its tools appear beside your own.\n\n", name);

        if (command != NULL)
            g_string_append_printf(out,
                "It runs as ~%s~ *on the host*, not on your computer, so a\n"
                "path you give it is a host path.\n\n", command);
        else if (url != NULL)
            g_string_append_printf(out,
                "It is reached over the network at ~%s~, so its tools are\n"
                "as fast, as slow and as available as that service is.\n\n",
                url);

        return;
    }
}

/*
 * The whole managed region, markers included.
 */
static gchar *
render_integrations_section(ClawtConfig *config, ClawtAgentConfig *agent)
{
    g_autoptr(GString) out = g_string_new(NULL);
    g_autoptr(GPtrArray) bindings = NULL;
    guint i;

    g_string_append_printf(out, "%s\n\n", TOOLS_BEGIN);
    g_string_append(out, "* Your integrations\n\n");

    if (config != NULL)
        bindings = clawt_integration_resolve_for_agent(config, agent);

    if (bindings == NULL || bindings->len == 0) {
        /*
         * Said rather than left blank.  An empty section reads as
         * "clawtilla has not worked this out yet", and an agent that
         * suspects it has an unlisted way of reaching the world will go
         * looking for one.
         */
        g_string_append(out,
            "You have none. Everything you can reach is in this file\n"
            "already, and nobody outside the fleet can reach you.\n\n");
        g_string_append_printf(out, "%s\n", TOOLS_END);

        return g_string_free(g_steal_pointer(&out), FALSE);
    }

    g_string_append(out,
        "These connect you to things outside the fleet. Some of them put\n"
        "a person on the other end of your reply.\n\n");

    for (i = 0; i < bindings->len; i++)
        describe_integration(out, g_ptr_array_index(bindings, i));

    g_string_append_printf(out, "%s\n", TOOLS_END);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * Replace one marked region of TOOLS.org, or append it.
 *
 * Shared by the two clawtilla owns -- the integrations, and the list of
 * tools this agent actually has -- because they differ only in what
 * goes between the markers, and a second copy of this would be a second
 * set of edge cases around somebody's prose.
 */
static gboolean
replace_region(ClawtAgentConfig *agent,
               const gchar      *begin_marker,
               const gchar      *end_marker,
               const gchar      *section,
               GError          **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *existing = NULL;
    g_autofree gchar *updated = NULL;
    const gchar *begin;
    const gchar *end;

    path = clawt_workspace_file_path(agent, "TOOLS.org");

    if (path == NULL)
        return TRUE;

    /*
     * A workspace that has not been scaffolded yet is not an error: the
     * scaffolder runs first on every path that matters, and refusing
     * here would turn "this agent has no workspace" into "this agent
     * will not start".
     */
    if (!g_file_get_contents(path, &existing, NULL, NULL))
        return TRUE;

    begin = strstr(existing, begin_marker);
    end = begin != NULL ? strstr(begin, end_marker) : NULL;

    if (begin != NULL && end != NULL) {
        g_autofree gchar *head = g_strndup(existing, begin - existing);
        const gchar *tail = end + strlen(end_marker);

        /* The newline after the end marker belongs to the marker. */
        if (*tail == '\n')
            tail++;

        updated = g_strconcat(head, section, tail, NULL);
    } else {
        /*
         * Appended when the markers are not there -- a file scaffolded
         * by an older clawtilla, or one somebody edited them out of.
         * Appending is the only safe move: this file is prose written by
         * a person, and there is no position in it we could claim to
         * know is the right one.
         */
        gboolean ends_blank = *existing == '\0' ||
                              g_str_has_suffix(existing, "\n\n");

        updated = g_strconcat(existing, ends_blank ? "" : "\n", section,
                              NULL);
    }

    if (g_strcmp0(existing, updated) == 0)
        return TRUE;

    return clawt_write_file_atomic(path, updated, -1, 0600, FALSE, error);
}

gboolean
clawt_workspace_update_tool_list(ClawtAgentConfig *agent,
                                 const gchar      *listing,
                                 GError          **error)
{
    g_autofree gchar *section = NULL;

    g_return_val_if_fail(agent != NULL, FALSE);
    g_return_val_if_fail(listing != NULL, FALSE);

    section = g_strconcat(TOOL_LIST_BEGIN, "\n\n", listing, "\n",
                          TOOL_LIST_END, "\n", NULL);

    return replace_region(agent, TOOL_LIST_BEGIN, TOOL_LIST_END, section,
                          error);
}

gboolean
clawt_workspace_update_tools_org(ClawtConfig      *config,
                                 ClawtAgentConfig *agent,
                                 GError          **error)
{
    g_autofree gchar *section = NULL;

    g_return_val_if_fail(agent != NULL, FALSE);

    section = render_integrations_section(config, agent);

    return replace_region(agent, TOOLS_BEGIN, TOOLS_END, section, error);
}
