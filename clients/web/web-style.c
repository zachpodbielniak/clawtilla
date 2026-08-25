/*
 * web-style.c - The stylesheet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Kept apart from the markup for the same reason the GTK client keeps its
 * CSS out of clawt-window.c: a stylesheet read as one piece is a design,
 * and the same rules scattered through the code that emits elements are
 * a pile of decisions nobody can see the shape of.
 *
 * Every colour is a token defined on bare :root, redefined under
 * prefers-color-scheme for the default "follow the system" case and again
 * under [data-theme] so an explicit choice wins in both directions.  A
 * colour whose only definition is inside a media query has no value at
 * all for a reader whose browser does not match it.
 */

#include "web-ui.h"

const gchar *clawt_web_stylesheet(void);

const gchar *
clawt_web_stylesheet(void)
{
    return
    ":root{"
      "--canvas:#FBFBFA;--surface:#FFFFFF;--surface-2:#F7F6F3;"
      "--line:#EAEAEA;--line-strong:#DCDCDA;"
      "--ink:#111111;--ink-2:#2F3437;--muted:#787774;"
      "--good-bg:#EDF3EC;--good-fg:#346538;"
      "--warn-bg:#FBF3DB;--warn-fg:#956400;"
      "--bad-bg:#FDEBEC;--bad-fg:#9F2F2D;"
      "--info-bg:#E1F3FE;--info-fg:#1F6C9F;"
      "--neutral-bg:#F1F1EF;--neutral-fg:#5F5E5B;"
      "--sans:system-ui,-apple-system,'Segoe UI','Helvetica Neue',Arial,sans-serif;"
      "--serif:ui-serif,'Iowan Old Style','Palatino Linotype',Palatino,Georgia,serif;"
      "--mono:ui-monospace,'SF Mono','JetBrains Mono','DejaVu Sans Mono',monospace;"
      "--radius:8px;--radius-sm:5px;"
      /* Overridable per browser from the appearance page; the rest of the
       * sheet reads these rather than naming a size directly. */
      "--font-size:14px;--mono-size:12.5px;"
    "}"
    "@media (prefers-color-scheme:dark){:root:not([data-theme=\"light\"]){"
      "--canvas:#151514;--surface:#1C1C1B;--surface-2:#232322;"
      "--line:#2E2E2C;--line-strong:#3A3A38;"
      "--ink:#EDEDEB;--ink-2:#D6D6D3;--muted:#918F8A;"
      "--good-bg:#1D2A1E;--good-fg:#8FCB94;"
      "--warn-bg:#2E2718;--warn-fg:#D9AF56;"
      "--bad-bg:#2E1C1D;--bad-fg:#E08C8A;"
      "--info-bg:#16252E;--info-fg:#7CBDE0;"
      "--neutral-bg:#252524;--neutral-fg:#9C9A95;"
    "}}"
    ":root[data-theme=\"dark\"]{"
      "--canvas:#151514;--surface:#1C1C1B;--surface-2:#232322;"
      "--line:#2E2E2C;--line-strong:#3A3A38;"
      "--ink:#EDEDEB;--ink-2:#D6D6D3;--muted:#918F8A;"
      "--good-bg:#1D2A1E;--good-fg:#8FCB94;"
      "--warn-bg:#2E2718;--warn-fg:#D9AF56;"
      "--bad-bg:#2E1C1D;--bad-fg:#E08C8A;"
      "--info-bg:#16252E;--info-fg:#7CBDE0;"
      "--neutral-bg:#252524;--neutral-fg:#9C9A95;"
    "}"

    /*
     * Catppuccin Mocha, the same palette clawt-appearance.c gives the
     * GTK client -- the colours are shared, the mechanism cannot be.
     * There it redefines libadwaita's named colours; here the sheet is
     * ours, so the palette lands on our own tokens instead.
     *
     * Mocha: base #1e1e2e, mantle #181825, crust #11111b, surface0
     * #313244, surface1 #45475a, text #cdd6f4, subtext0 #a6adc8,
     * blue #89b4fa, green #a6e3a1, yellow #f9e2af, red #f38ba8.
     *
     * After the two dark blocks above, deliberately. A palette carries
     * data-theme too, so `:root:not([data-theme="light"])` matches it as
     * well; the specificity is identical and source order is what
     * settles it. Moving this above them silently gives a Mocha reader
     * the plain dark greys under prefers-color-scheme: dark.
     */
    ":root[data-theme=\"catppuccin-mocha\"]{"
      "--canvas:#1e1e2e;--surface:#181825;--surface-2:#313244;"
      "--line:#313244;--line-strong:#45475a;"
      "--ink:#cdd6f4;--ink-2:#bac2de;--muted:#a6adc8;"
      "--good-bg:#1f2a24;--good-fg:#a6e3a1;"
      "--warn-bg:#2b2620;--warn-fg:#f9e2af;"
      "--bad-bg:#2c2028;--bad-fg:#f38ba8;"
      "--info-bg:#1d2436;--info-fg:#89b4fa;"
      "--neutral-bg:#313244;--neutral-fg:#a6adc8;"
    "}"

    "*{box-sizing:border-box}"
    "html,body{height:100%}"
    "body{margin:0;background:var(--canvas);color:var(--ink);"
      "font-family:var(--sans);font-size:var(--font-size);line-height:1.6;"
      "-webkit-font-smoothing:antialiased}"

    /* ── Frame ── */
    ".app{display:grid;grid-template-columns:17rem 1fr;height:100vh}"
    ".sidebar{border-right:1px solid var(--line);background:var(--surface);"
      "display:flex;flex-direction:column;min-height:0}"
    ".sidebar-head{padding:20px 20px 14px;border-bottom:1px solid var(--line);"
      "display:flex;align-items:baseline;justify-content:space-between;gap:8px}"
    ".wordmark{font-family:var(--serif);font-size:19px;letter-spacing:-0.02em;"
      "line-height:1.1;margin:0}"
    ".sidebar-scroll{overflow-y:auto;flex:1;min-height:0;padding:8px 0 16px}"
    ".sidebar-foot{border-top:1px solid var(--line);padding:12px;"
      "display:flex;gap:8px;flex-wrap:wrap}"
    ".content{display:flex;flex-direction:column;min-width:0;min-height:0}"
    ".topbar{border-bottom:1px solid var(--line);background:var(--surface);"
      "padding:0 24px;display:flex;align-items:center;gap:20px;"
      "min-height:56px;flex-wrap:wrap}"
    ".view{flex:1;overflow-y:auto;min-height:0}"
    ".view-pad{padding:32px 32px 48px;max-width:60rem}"
    ".view-wide{padding:32px 32px 48px}"

    /* ── Nav ── */
    ".tabs{display:flex;gap:2px;flex-wrap:wrap}"
    ".tab{padding:6px 12px;border-radius:var(--radius-sm);color:var(--muted);"
      "text-decoration:none;font-size:13px;transition:background 160ms,color 160ms}"
    ".tab:hover{background:var(--surface-2);color:var(--ink)}"
    ".tab[aria-current=\"page\"]{background:var(--surface-2);color:var(--ink);"
      "font-weight:600}"
    ".topbar-title{font-family:var(--serif);font-size:17px;margin:0;"
      "letter-spacing:-0.02em;white-space:nowrap}"
    ".topbar-spacer{flex:1}"

    /* ── Agent rows ── */
    ".team-head{display:flex;align-items:center;gap:8px;width:100%;"
      "padding:12px 20px 6px;background:none;border:0;cursor:pointer;"
      "font:inherit;color:var(--muted);text-align:left}"
    ".team-head:hover{color:var(--ink)}"
    ".team-name{font-size:11px;letter-spacing:0.08em;text-transform:uppercase;"
      "font-weight:600}"
    ".team-tally{font-family:var(--mono);font-size:11px;color:var(--muted)}"
    ".team-caret{font-family:var(--mono);font-size:11px;width:1em}"
    ".agent-row{display:block;padding:9px 20px;text-decoration:none;"
      "color:var(--ink);border-left:2px solid transparent;"
      "transition:background 140ms}"
    ".agent-row:hover{background:var(--surface-2)}"
    ".agent-row.selected{background:var(--surface-2);"
      "border-left-color:var(--ink)}"
    ".agent-line{display:flex;align-items:center;gap:8px}"
    ".agent-name{font-weight:500;overflow:hidden;text-overflow:ellipsis;"
      "white-space:nowrap}"
    ".agent-meta{display:flex;align-items:center;gap:6px;margin-top:2px;"
      "flex-wrap:wrap}"
    ".dot{width:7px;height:7px;border-radius:9999px;flex:none}"
    ".dot-good{background:var(--good-fg)}"
    ".dot-warn{background:var(--warn-fg)}"
    ".dot-bad{background:var(--bad-fg)}"
    ".dot-neutral{background:var(--muted);opacity:.5}"

    /* ── Badges ── */
    ".badge{display:inline-block;padding:1px 7px;border-radius:9999px;"
      "font-size:10px;letter-spacing:0.06em;text-transform:uppercase;"
      "font-weight:600;white-space:nowrap;font-family:var(--sans)}"
    ".badge-neutral{background:var(--neutral-bg);color:var(--neutral-fg)}"
    ".badge-good{background:var(--good-bg);color:var(--good-fg)}"
    ".badge-warn{background:var(--warn-bg);color:var(--warn-fg)}"
    ".badge-bad{background:var(--bad-bg);color:var(--bad-fg)}"
    ".badge-info{background:var(--info-bg);color:var(--info-fg)}"

    /* ── Cards ── */
    ".card{background:var(--surface);border:1px solid var(--line);"
      "border-radius:var(--radius);padding:24px;margin-bottom:20px}"
    ".card-title{font-family:var(--serif);font-size:17px;margin:0 0 2px;"
      "letter-spacing:-0.02em;line-height:1.25}"
    ".card-sub{color:var(--muted);font-size:13px;margin:0 0 16px}"
    ".card-body>*:last-child{margin-bottom:0}"
    ".grid{display:grid;gap:20px;grid-template-columns:repeat(auto-fit,minmax(20rem,1fr))}"
    "h2.section{font-family:var(--serif);font-size:22px;margin:0 0 4px;"
      "letter-spacing:-0.025em;line-height:1.15}"
    ".lede{color:var(--muted);margin:0 0 24px;max-width:44rem}"

    /* ── Rows ── */
    ".row{display:flex;align-items:baseline;justify-content:space-between;"
      "gap:16px;padding:10px 0;border-bottom:1px solid var(--line)}"
    ".row:last-child{border-bottom:0}"
    ".row-title{color:var(--ink-2)}"
    ".row-value{color:var(--muted);font-family:var(--mono);font-size:var(--mono-size);"
      "text-align:right;word-break:break-word}"
    ".list{border:1px solid var(--line);border-radius:var(--radius);"
      "background:var(--surface);overflow:hidden}"
    ".list-item{padding:14px 18px;border-bottom:1px solid var(--line)}"
    ".list-item:last-child{border-bottom:0}"
    ".list-item-head{display:flex;align-items:center;gap:10px;flex-wrap:wrap}"
    ".list-item-title{font-weight:500}"
    ".list-item-sub{color:var(--muted);font-size:13px;margin-top:3px}"

    /* ── Empty ── */
    ".empty{border:1px dashed var(--line-strong);border-radius:var(--radius);"
      "padding:28px;text-align:center;color:var(--muted);background:none}"
    ".empty-title{color:var(--ink-2);margin:0 0 6px}"
    ".empty-detail{font-size:13px;margin:0;max-width:34rem;"
      "margin-left:auto;margin-right:auto}"

    /* ── Controls ── */
    ".btn{font:inherit;font-size:13px;padding:7px 14px;border-radius:var(--radius-sm);"
      "border:1px solid var(--line-strong);background:var(--surface);"
      "color:var(--ink);cursor:pointer;transition:background 160ms,transform 90ms;"
      "text-decoration:none;display:inline-block;line-height:1.4}"
    ".btn:hover{background:var(--surface-2)}"
    ".btn:active{transform:scale(.98)}"
    ".btn-primary{background:var(--ink);color:var(--canvas);border-color:var(--ink)}"
    ".btn-primary:hover{background:var(--ink-2);border-color:var(--ink-2)}"
    ".btn-danger{color:var(--bad-fg);border-color:var(--bad-fg)}"
    ".btn-danger:hover{background:var(--bad-bg)}"
    ".btn:disabled{opacity:.45;cursor:default}"
    ".btn-row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
    ".field{margin-bottom:16px}"
    ".field>label{display:block;font-size:12px;letter-spacing:0.02em;"
      "color:var(--muted);margin-bottom:5px}"
    "input[type=text],input[type=password],input[type=number],textarea,select{"
      "width:100%;font:inherit;font-size:13.5px;padding:8px 10px;"
      "border:1px solid var(--line-strong);border-radius:var(--radius-sm);"
      "background:var(--surface);color:var(--ink)}"
    "textarea{font-family:var(--mono);font-size:var(--mono-size);resize:vertical}"
    "input:focus,textarea:focus,select:focus{outline:2px solid var(--info-fg);"
      "outline-offset:-1px}"
    ".field-check{display:flex;gap:10px;align-items:flex-start;"
      "margin-bottom:16px}"
    ".field-check input{margin-top:4px;flex:none;width:auto}"
    ".field-check .check-sub{color:var(--muted);font-size:12.5px;margin:2px 0 0}"
    ".field-inline{display:grid;grid-template-columns:repeat(auto-fit,minmax(12rem,1fr));"
      "gap:0 16px}"

    /* ── Chat ── */
    ".chat{display:flex;flex-direction:column;height:100%;min-height:0}"
    ".transcript{flex:1;overflow-y:auto;padding:28px 32px;min-height:0}"
    ".transcript-inner{max-width:52rem;margin:0 auto}"
    ".msg{margin-bottom:22px;animation:rise 420ms cubic-bezier(.16,1,.3,1)}"
    "@keyframes rise{from{opacity:0;transform:translateY(8px)}"
      "to{opacity:1;transform:none}}"
    ".msg-who{font-size:11px;letter-spacing:0.06em;text-transform:uppercase;"
      "color:var(--muted);margin-bottom:5px;font-weight:600}"
    ".msg-body{white-space:pre-wrap;word-wrap:break-word}"
    ".msg-body code{font-family:var(--mono);font-size:var(--mono-size);"
      "background:var(--surface-2);padding:1px 5px;border-radius:4px}"
    ".msg-body pre{font-family:var(--mono);font-size:var(--mono-size);"
      "background:var(--surface-2);border:1px solid var(--line);"
      "border-radius:var(--radius-sm);padding:12px 14px;overflow-x:auto}"
    ".msg-self .msg-who{color:var(--info-fg)}"

    /*
     * The rule drawn where reading left off, and the pill that offers to
     * go there. The GTK client grew both together in one state; here the
     * state is the browser's scroll position, so the two are driven by
     * the script in the page head rather than by the server -- but they
     * are the same pair, and appear only when something has actually
     * arrived rather than merely because the reader scrolled up.
     */
    /*
     * The unread pill -- what has arrived from an agent since its
     * conversation was last opened.  Filled, because every other badge
     * in that row is outlined: filled means for you, outlined means
     * about the agent.  Tokens, never a hex value, so a palette
     * recolours it for free.
     */
    ".unread-badge{display:inline-block;min-width:8px;padding:1px 6px;"
    "border-radius:9px;font-size:11px;font-weight:700;line-height:16px;"
    "text-align:center;background:var(--info-fg);color:var(--surface);"
    "margin-left:6px;}"
    /* Colour is never the only signal; the name goes bold beside it. */
    ".agent-row.is-unread .agent-name{font-weight:700;}"
    ".unread-rule{display:flex;align-items:center;gap:12px;margin:26px 0 22px;"
      "font-size:11px;letter-spacing:0.08em;text-transform:uppercase;"
      "color:var(--bad-fg)}"
    ".unread-rule::before,.unread-rule::after{content:'';flex:1;height:1px;"
      "background:var(--bad-fg);opacity:0.45}"
    ".jump-pill{position:absolute;left:50%;transform:translateX(-50%);"
      "bottom:12px;z-index:5;display:none;align-items:center;gap:7px;"
      "padding:7px 15px;border-radius:9999px;border:1px solid var(--line);"
      "background:var(--surface);color:var(--ink);cursor:pointer;"
      "font-family:var(--sans);font-size:12px;"
      "box-shadow:0 2px 8px rgba(0,0,0,0.10)}"
    ".jump-pill.on{display:inline-flex}"
    ".jump-pill:hover{border-color:var(--line-strong)}"
    /* The transcript is the pill's containing block, so it floats over
     * the messages rather than over the whole frame. */
    ".chat-body{position:relative;display:flex;flex-direction:column;"
      "flex:1;min-height:0}"
    ".composer{border-top:1px solid var(--line);background:var(--surface);"
      "padding:16px 32px}"
    ".composer-inner{max-width:52rem;margin:0 auto;display:flex;gap:10px;"
      "align-items:flex-end}"
    ".composer textarea{flex:1;min-height:2.6rem;max-height:14rem;"
      "font-family:var(--sans);font-size:14px}"

    /* ── Console ── */
    ".console{font-family:var(--mono);font-size:var(--mono-size);white-space:pre-wrap;"
      "background:var(--surface-2);border:1px solid var(--line);"
      "border-radius:var(--radius-sm);padding:14px 16px;overflow-x:auto;"
      "max-height:26rem;overflow-y:auto;margin:0}"
    ".mono{font-family:var(--mono);font-size:var(--mono-size)}"
    "kbd{font-family:var(--mono);font-size:11.5px;border:1px solid var(--line);"
      "border-radius:4px;background:var(--surface-2);padding:1px 5px}"

    /* ── Tables ── */
    ".table-wrap{overflow-x:auto;border:1px solid var(--line);"
      "border-radius:var(--radius);background:var(--surface)}"
    "table{border-collapse:collapse;width:100%;font-size:13px}"
    "th{text-align:left;font-size:11px;letter-spacing:0.06em;"
      "text-transform:uppercase;color:var(--muted);font-weight:600;"
      "padding:12px 16px;border-bottom:1px solid var(--line);white-space:nowrap}"
    "td{padding:11px 16px;border-bottom:1px solid var(--line)}"
    "tr:last-child td{border-bottom:0}"
    "td.num{font-family:var(--mono);text-align:right;white-space:nowrap}"
    "tfoot td{font-weight:600;border-top:1px solid var(--line-strong)}"

    /* ── Notices ── */
    ".notice{border:1px solid var(--line);border-left:3px solid var(--warn-fg);"
      "background:var(--warn-bg);color:var(--warn-fg);padding:12px 16px;"
      "border-radius:var(--radius-sm);margin-bottom:16px;font-size:13px}"
    ".notice-bad{border-left-color:var(--bad-fg);background:var(--bad-bg);"
      "color:var(--bad-fg)}"
    ".notice-info{border-left-color:var(--info-fg);background:var(--info-bg);"
      "color:var(--info-fg)}"
    ".toast{position:fixed;right:20px;bottom:20px;z-index:50;"
      "background:var(--ink);color:var(--canvas);padding:11px 16px;"
      "border-radius:var(--radius-sm);font-size:13px;max-width:24rem;"
      "animation:rise 300ms cubic-bezier(.16,1,.3,1)}"

    /* ── Detail/summary ── */
    "details{border:1px solid var(--line);border-radius:var(--radius);"
      "background:var(--surface);margin-bottom:12px}"
    "summary{padding:13px 18px;cursor:pointer;font-weight:500;"
      "list-style:none;display:flex;align-items:center;gap:10px}"
    "summary::-webkit-details-marker{display:none}"
    "summary::before{content:\"+\";font-family:var(--mono);color:var(--muted);"
      "width:1em}"
    "details[open]>summary::before{content:\"\\2212\"}"
    "details>.details-body{padding:0 18px 18px}"

    "a{color:var(--ink)}"
    ".muted{color:var(--muted)}"
    ".small{font-size:12.5px}"
    ".stack>*+*{margin-top:12px}"
    ".htmx-request .htmx-hide{opacity:.5}"

    /* ── Narrow ── */
    "@media (max-width:56rem){"
      ".app{grid-template-columns:1fr;grid-template-rows:auto 1fr}"
      ".sidebar{border-right:0;border-bottom:1px solid var(--line);"
        "max-height:14rem}"
      ".view-pad,.view-wide{padding:20px 18px 40px}"
      ".transcript,.composer{padding-left:18px;padding-right:18px}"
    "}"
    "@media (prefers-reduced-motion:reduce){"
      "*{animation:none!important;transition:none!important}"
    "}";
}
