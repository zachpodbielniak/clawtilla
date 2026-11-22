/*
 * clawt-ephemeral-name.c - Names for ephemeral agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-ephemeral-name.h"

/*
 * Every word is lowercase ASCII letters only, so any pair is a valid
 * agent id and none needs escaping anywhere it is shown.  The words are
 * chosen to be unmistakable when read aloud and impossible to take for
 * a description of the work: a helper called `careful-reviewer` would
 * be believed.
 */
static const gchar *const adjectives[] = {
    "turbo", "feral", "quantum", "disco", "cosmic", "sneaky", "wobbly",
    "funky", "zesty", "groovy", "spicy", "fuzzy", "noodly", "bouncy",
    "sparkly", "grumpy", "jazzy", "loopy", "nifty", "rowdy", "snazzy",
    "spooky", "squishy", "thunder", "wonky", "zippy", "bodacious",
    "chonky", "dapper", "electric", "fizzy", "galactic", "hyper",
    "jumbo", "kooky", "laser", "mega", "neon", "plucky", "radical",
    "rocket", "sassy", "techno", "unhinged", "velvet", "wizard",
    "yodeling", "zany", "atomic", "bubbly", "crunchy", "dizzy",
    "extra", "frosty", "goofy", "hasty", "itchy", "jolly", "mighty",
    "nimble", "outlaw", "pixel", "quirky", "rusty", "silly", "tipsy",
    "ultra", "vapor", "wiggly", "zealous"
};

static const gchar *const nouns[] = {
    "narwhal", "kazoo", "pickle", "gremlin", "wombat", "noodle",
    "platypus", "taco", "yeti", "goblin", "llama", "muffin", "badger",
    "pretzel", "walrus", "burrito", "capybara", "axolotl", "banjo",
    "cactus", "dumpling", "falafel", "gecko", "hamster", "iguana",
    "jellybean", "koala", "lobster", "mongoose", "nugget", "ocelot",
    "penguin", "quokka", "raccoon", "sloth", "tamale", "unicorn",
    "vulture", "waffle", "yak", "zebra", "anchovy", "bagel", "corgi",
    "dingo", "emu", "ferret", "gnome", "hedgehog", "jackalope",
    "kraken", "lemur", "meatball", "okapi", "otter", "pancake",
    "quesadilla", "robot", "squid", "toaster", "ukulele", "velociraptor",
    "weasel", "xylophone", "yodeler", "zucchini", "bandicoot",
    "chupacabra", "doughnut", "flamingo"
};

guint
clawt_ephemeral_name_pair_count(void)
{
    return G_N_ELEMENTS(adjectives) * G_N_ELEMENTS(nouns);
}

/* One draw: the global source, or the caller's for a reproducible test. */
static guint
draw(GRand *rand, guint below)
{
    return rand != NULL ? (guint)g_rand_int_range(rand, 0, (gint32)below)
                        : (guint)g_random_int_range(0, (gint32)below);
}

gchar *
clawt_ephemeral_name_generate(GRand              *rand,
                              ClawtNameTakenFunc  taken,
                              gpointer            user_data)
{
    guint attempt;

    /*
     * Plain pairs first.  Sixty-four misses in a row means the fleet is
     * crowded enough that a suffix is kinder than spinning, and a
     * suffix is still a name a person can say.
     */
    for (attempt = 0; attempt < 64; attempt++) {
        g_autofree gchar *name = g_strdup_printf(
            "%s-%s", adjectives[draw(rand, G_N_ELEMENTS(adjectives))],
            nouns[draw(rand, G_N_ELEMENTS(nouns))]);

        if (taken == NULL || !taken(name, user_data))
            return g_steal_pointer(&name);
    }

    for (attempt = 0;; attempt++) {
        g_autofree gchar *name = g_strdup_printf(
            "%s-%s-%02u", adjectives[draw(rand, G_N_ELEMENTS(adjectives))],
            nouns[draw(rand, G_N_ELEMENTS(nouns))], draw(rand, 100));

        if (taken == NULL || !taken(name, user_data) || attempt > 10000)
            return g_steal_pointer(&name);
    }
}
