# LQL on libresearch, for the clawtilla_web_query tool

LQL (libre query language) is a pipeline: a **source** produces rows, and each
`|` stage filters, reshapes, sorts or summarizes them. libresearch adds the
`search` and `research` sources, the search predicates, and pushdown of
filters to the engines. Everything here is sent through `clawtilla_web_query`
(`query`, optionally `explain: true`).

When in doubt about what *this* server supports, call
`clawtilla_web_query_environment`: it lists the commands, sources, functions
(with signatures) and constants it actually has.

## The `search` source

```
search "TEXT"[, category: C][, engines: [...]][, page: N][, language: "xx"]
       [, safesearch: "off|moderate|strict"][, time_range: "day|week|month|year"][, limit: N]
```

- `category`: `WEB` (default), `NEWS`, `CODE`, `IMAGES`, `VIDEOS`.
- `limit` 1-100, `page` 1-50.
- Bangs work in the text: `!gh`, `!mdn`, `!arxiv`, `!reddit`. A bang that redirects elsewhere (`!w`) is an error.

Columns: `url`, `title`, `snippet`, `host`, `url_path`, `category`, `engines`
(array), `engine_count`, `position`, `score`, `published` (Timestamp or NULL),
`author`, `thumbnail`, `extra` (a record: `extra.stars`, `extra.language`,
`extra.forks`, ... depending on the engine).

`... | describe` lists the columns at that point.

## Commands

| Command | Example |
|---|---|
| `filter COND` | `filter site("lwn.net") and published >= now() - 14d` |
| `filter_out COND` | `filter_out host == "pinterest.com"` |
| `fields a, b, c = expr` | `fields title, stars = extra.stars, url` |
| `sort x DESC, y` | `sort published DESC` |
| `limit N` | `limit 10` |
| `dedup x` | `dedup host` |
| `summarize agg, by: {k}` | `summarize n = count(), by: {host}` |

`filter` keeps only TRUE (NULL is dropped); `filter_out` drops only TRUE.

## Search predicates

| Predicate | True when |
|---|---|
| `site("x.org")` | host is `x.org` or below it |
| `filetype("pdf")` | URL path ends in `.pdf` |
| `intitle("t")` | title contains t (any case) |
| `inurl("t")` | URL contains t |
| `intext("t")` | title or snippet contains t |
| `phrase("a b")` | exact phrase in title or snippet |

In a `filter` **directly after `search`**, these -- plus `host == "h"`,
`host in [...]`, `published >= T`, `published < T` and comparisons on
`extra.NAME` -- are pushed down to the engines that can express them. The
server then checks every result itself, so every engine supports every query;
pushdown just means you get more matching rows. `explain: true` shows what
went where.

## Language basics

- Literals: `"text"`, `42`, `3.5`, `TRUE`, `FALSE`, `NULL`, durations `250ms 10s 5m 2h 7d 4w`, `now()`, `timestamp("2026-10-01")`, arrays `[1, "a"]`, records `{n: 1}`.
- Operators: `and or not`, `== != < <= > >=`, `in`, `+ - * / %`. Comparisons do not chain.
- `Timestamp - Duration` is a Timestamp: `published >= now() - 7d`.
- Strings join with `concat(a, b)`, not `+`.
- A missing field is NULL. Test it with `is_null(x)`; `x == NULL` is always NULL.
- Functions are `lower_snake_case`, constants `UPPER_SNAKE_CASE`.
- `link(text, url)` makes a markdown link: `fields repo = link(title, url)`.

## Modules (only if the server has them -- check the environment)

| Function | Gives |
|---|---|
| `ai_summarize([text])` | one sentence per row |
| `ai_ask(question[, text])` | a short answer per row |
| `decide_relevance([query])` | P(row is about the query) |
| `decide_ask(question)` | P(yes) |
| `decide_classify(labels)` | the best of two or more labels |
| `research "q"[, pages: N, deep: TRUE]` | a source: an answer row (`kind = "answer"`, `text`) then one row per source (`kind = "source"`, `n`, `url`, `read`, `cited`) |
| `fetch personal_index, match: "w"` | the server's personal index |

Model functions see at most 20 rows per stage -- put `limit` before them --
and refuse when a confidential engine answered.

## Recipes

```
# which hosts dominate a topic
search "wayland compositor" | summarize n = count(), by: {host} | sort n DESC | limit 10

# recent news from two outlets, newest first
search "fedora", category: NEWS
| filter (site("lwn.net") or site("phoronix.com")) and published >= now() - 14d
| sort published DESC | fields published, title, url

# well-starred C repositories
search "yaml parser", category: CODE
| filter extra.stars >= 200 and extra.language == "C"
| sort extra.stars DESC | fields title, stars = extra.stars, url

# PDFs, excluding a host
search "glib main loop" | filter filetype("pdf") and not host == "example.org"

# results several engines agree on
search "gobject introspection" | filter engine_count >= 2 | fields title, url, engines

# rank by relevance with the decide module, keep the confident ones
search "gtk list view" | limit 10 | fields title, url, rel = decide_relevance() | filter rel >= 0.7

# research, then only the sources it read but did not cite
research "rust async runtimes", deep: TRUE | filter kind == "source" and read and not cited | fields n, url
```

## Errors

A failed query answers with the position and a caret:

```
libresearch invalid_query (HTTP 400 ...): expected an expression, found end of query
1:26: expected an expression, found end of query
  search "x" | filter nope(
                           ^
```

Fix the marked spot. `422` means a limit (query length, stages, rows, time);
`502 search_failed` means a search inside the query failed (often a redirecting
bang or no engine for the category); `403` means LQL is switched off on the
server, which no query can fix -- say so to whoever asked.
