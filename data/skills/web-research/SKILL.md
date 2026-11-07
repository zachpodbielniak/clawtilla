---
name: web-research
description: Search the web, read pages, run LQL queries and get cited research answers through the fleet's libresearch server (the clawtilla_web_* tools). Use whenever a task needs information from the web, and especially when the question needs structure -- only certain sites, recent results, file types, star counts, counting or reshaping -- which is what LQL (clawtilla_web_query) is for.
---

# Web research through libresearch

You have five tools that reach a libresearch server the fleet runs. It is a
metasearch engine: one query goes to many engines at once and the results are
merged and ranked.

| Tool | Use it for |
|---|---|
| `clawtilla_web_search` | a quick lookup: a page, a doc, a fact |
| `clawtilla_web_fetch` | reading one page's text (with `archive: fallback` for a page that is gone) |
| `clawtilla_web_query` | anything with structure: LQL, below |
| `clawtilla_web_research` | a *question* that needs several sources read and an answer citing them; takes minutes |
| `clawtilla_web_query_environment` | what this server's LQL can name: commands, functions, sources |

## Choosing

1. **A lookup** -- one thing you can recognise when you see it: `clawtilla_web_search`, then `clawtilla_web_fetch` the result you will rely on. Cite the URL you read, not the snippet.
2. **A filtered or shaped answer** -- "only docs.gtk.org", "this week", "PDFs", "repos over 100 stars", "which hosts dominate", "newest first": write an LQL query. Do not search and then sift results by hand; the server filters, and pushes most filters down to the engines so you get more of what you asked for.
3. **A question** that needs reading and synthesis: `clawtilla_web_research` (or LQL `research "question", deep: TRUE` when you want the sources as rows to filter). Read the cited sources you depend on; an uncited or unread source supports nothing.

## LQL in one minute

A pipeline: a source, then stages joined by `|`.

```
search "gtk4 list view", category: CODE
| filter site("docs.gtk.org") and published >= now() - 30d
| sort score DESC
| limit 5
| fields title, url
```

- Source: `search "TEXT"` with optional `category:` (`WEB NEWS CODE IMAGES VIDEOS`), `time_range:`, `engines: [...]`, `limit:`. Bangs work in the text (`!gh`, `!arxiv`).
- Stages: `filter`, `filter_out`, `fields` (keep, rename or compute: `fields title, stars = extra.stars`), `sort x DESC`, `limit N`, `dedup`, `summarize n = count(), by: {host}`.
- Search predicates: `site("x.org")`, `filetype("pdf")`, `intitle("t")`, `inurl("t")`, `intext("t")`, `phrase("a b")`.
- Columns: `url title snippet host published engines engine_count score extra` (`extra.stars`, `extra.language`, ... for code results).
- Literals: strings `"x"`, booleans `TRUE`/`FALSE`, durations `7d 2h 30m`, `now()`, `timestamp("2026-10-01")`.
- The shorthand works too: `gtk docs | filter site("docs.gtk.org")`.

**Check before you spend.** Set `explain: true` on a query you have not run before: it shows which filters reached which engines and costs nothing. A filter listed as "checked locally" still works, but returns fewer rows from that engine.

**When a query fails**, the error carries the line, column and a caret under the problem. Fix that spot; do not rewrite the whole query. When you are unsure a function or source exists on this server, call `clawtilla_web_query_environment` rather than guessing -- it answers for this server, modules included.

The full reference, with recipes, is [references/lql.md](references/lql.md). Read it before your first non-trivial query.

## Limits worth knowing

- A query has a time limit (usually 30 s). Put `limit` early; model functions (`ai_summarize`, `decide_relevance`) see at most 20 rows per stage.
- If a search touched a confidential engine, model functions refuse. Do not route the rows to a model another way.
- Rate limits are per minute; a `rate_limited` answer means wait, not retry at once.
- Results name the engines that did not answer. A short list with failed engines is not the whole web's answer -- say so if it matters.
