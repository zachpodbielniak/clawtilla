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
      /*
       * --muted was #787774, which is 4.32:1 on the canvas and
       * 4.14:1 on surface-2 -- under AA's 4.5 for small text, and
       * this is the token every secondary annotation in the client
       * uses: timestamps, day dividers, a link's target beside it,
       * every `.muted` span.  It is the only value in any of the
       * four palettes that was under, and darkening it by 8% clears
       * every light background with room (4.99 / 5.17 / 4.78) while
       * staying the lightest text token, so the hierarchy below --ink
       * and --ink-2 is unchanged.  tests/test-web-render.c checks
       * all four palettes now, so the next one added cannot ship
       * under it quietly.
       */
      "--ink:#111111;--ink-2:#2F3437;--muted:#6E6D6B;"
      "--good-bg:#EDF3EC;--good-fg:#346538;"
      "--warn-bg:#FBF3DB;--warn-fg:#956400;"
      "--bad-bg:#FDEBEC;--bad-fg:#9F2F2D;"
      "--info-bg:#E1F3FE;--info-fg:#1F6C9F;"
      "--neutral-bg:#F1F1EF;--neutral-fg:#5F5E5B;"
      "--sans:system-ui,-apple-system,'Segoe UI','Helvetica Neue',Arial,sans-serif;"
      "--serif:ui-serif,'Iowan Old Style','Palatino Linotype',Palatino,Georgia,serif;"
      "--mono:ui-monospace,'SF Mono','JetBrains Mono','DejaVu Sans Mono',monospace;"
      "--radius:8px;--radius-sm:5px;"
      /*
       * The avatar column a body is indented past -- 28px of face and
       * its 8px gap.  A token rather than a literal because the
       * composer has to agree with a number it does not draw: the
       * transcript spends this on the avatar and the composer spent
       * nothing, so the entry sat in the one column deliberately kept
       * empty, which is the strongest vertical line on the page in the
       * wrong place.  Four rules read it now and none of them can
       * drift.  clawt_chat_body_inset() states the same derivation for
       * the GTK client, whose own pair of numbers is different.
       */
      "--chat-gutter:36px;"
      /*
       * The column and the gap between runs, as tokens rather than
       * literals, so the shipped design and a reader's override use one
       * mechanism.  clawt_appearance_to_css() redefines these on :root
       * when somebody has set them, and emits nothing when they have
       * not -- which is why the value lives here rather than being
       * written into the appearance sheet as a default.
       *
       * Nine tenths of what the chat body has to give, which is
       * CLAWT_APPEARANCE_DEFAULT_PERCENT and the same column the GTK
       * client ships.  It was 40rem -- measured rather than chosen, at
       * 89 characters a line -- and that is a good column and a bad
       * default: it leaves most of a wide display empty, and the reader
       * who wants the conversation to use the screen had to convert
       * their monitor's width to rem to say so.
       *
       * A percentage resolves against each element's own containing
       * block, so this only holds the transcript and the composer on
       * one column because `.transcript` and `.composer` carry
       * identical horizontal padding -- including in the narrow
       * override below, which sets both in one declaration for exactly
       * this reason.
       */
      /*
       * The run gap, and the message gap derived from it.
       *
       * Measured in the browser at the default font: a line is 22px and
       * a markdown paragraph break is one blank line, so 22px of clear
       * space.  The shipped run gap was 26 -- four pixels more than a
       * paragraph, which is not a break a reader can see -- and a new
       * message got 6, a quarter of a paragraph.  Both are now a third
       * of a line apart: 22 / 29 / 36.
       *
       * The message gap is calc()'d off the run gap rather than being a
       * second setting, so a reader who widens the run gap keeps the
       * ordering instead of re-inverting it with one knob.
       */
      "--chat-measure:90%;"
      "--chat-run-gap:36px;--chat-msg-gap:calc(var(--chat-run-gap) - 7px);"
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
    /*
     * 100vh is the viewport with the URL bar retracted, so on a phone
     * the app box is taller than what is visible and the composer --
     * the bottom row of this grid -- starts below the fold.  100dvh
     * is the height that is actually there.
     *
     * Declared twice rather than once: a browser that does not know
     * dvh drops the second and keeps the first, and there is no
     * @supports needed for that.
     */
    ".app{display:grid;grid-template-columns:17rem 1fr;height:100vh;height:100dvh}"
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
    /*
     * The sidebar's own size for clawt_web_avatar() -- smaller than the
     * transcript's .msg-avatar, because a row of these runs down a list
     * a person scans quickly rather than sitting beside one message at a
     * time.
     */
    ".agent-face{display:inline-flex;align-items:center;"
      "justify-content:center;width:24px;height:24px;border-radius:50%;"
      "font-size:11px;font-weight:700;flex:0 0 auto;"
      "background:var(--neutral-bg);color:var(--neutral-fg)}"
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
    /*
     * min(20rem,100%) rather than 20rem: in minmax() a bare length is
     * a hard floor, so auto-fit refuses to take a track below it and
     * the track overflows its container instead of shrinking.  The
     * desktop behaviour is unchanged -- 20rem wins whenever there is
     * room for it.
     */
    ".grid{display:grid;gap:20px;grid-template-columns:repeat(auto-fit,minmax(min(20rem,100%),1fr))}"
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
    /*
     * A decision's options, one per line and read from the left.
     *
     * An option is a sentence rather than a verb, so a row of them wraps
     * into a block with no left edge to come back to. Stacked and
     * left-aligned they read as the list of choices they are, which is
     * how the GTK client draws them too.
     */
    ".decision-options{display:flex;flex-direction:column;gap:8px;"
      "align-items:stretch;margin-bottom:12px}"
    ".decision-options .btn{text-align:left;white-space:normal}"
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
    ".field-inline{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(12rem,100%),1fr));"
      "gap:0 16px}"

    /*
     * Files an agent sent.  Images inline, at a size that is worth
     * looking at without taking the whole column; anything else as a
     * link, because a browser rendering a file a model produced in this
     * origin is how a transcript becomes a script.
     */
    ".attachments{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px;"
      "margin-left:var(--chat-gutter)}"
    ".msg-self .attachments{margin-left:0;justify-content:flex-end}"
    ".attachment-image{max-width:100%;max-height:20rem;border-radius:"
      "var(--radius-sm);border:1px solid var(--line);display:block}"
    ".attachment-file{display:inline-block;padding:6px 12px;font-size:12px;"
      "border:1px solid var(--line);border-radius:var(--radius-sm);"
      "background:var(--surface-2);text-decoration:none}"
    "@media (max-width:26rem){.attachments{margin-left:0}}"

    /* ── Alerts ── */
    /*
     * Three tiers, two of them coloured.  If everything carries a
     * colour, colour stops meaning anything -- and routine entries are
     * the majority the moment the filter widens, so making them quiet is
     * what keeps the loud ones loud.  Severity is carried by weight and
     * container as much as by hue, which is what makes it survive a
     * colourblind reader and a Catppuccin palette alike.
     */
    ".alert-row{display:flex;flex-direction:column;gap:4px;"
      "margin-bottom:10px}"
    ".alert-error,.alert-notice{border:1px solid var(--line);"
      "border-radius:var(--radius);background:var(--surface);"
      "padding:12px 16px}"
    ".alert-error{border-left:3px solid var(--bad-fg)}"
    ".alert-notice{border-left:3px solid var(--info-fg)}"
    /* Unread is a bar, not a colour: it has to survive the tier's own. */
    ".alert-unread{box-shadow:inset 3px 0 0 var(--info-fg)}"
    ".alert-routine{font-size:12px;color:var(--muted);padding:2px 0;"
      "margin-bottom:2px}"
    ".alert-routine .alert-text{display:inline}"
    ".alert-text{white-space:pre-wrap;word-wrap:break-word}"
    ".alert-meta{display:flex;gap:8px;align-items:center;font-size:11px}"
    ".alert-agent{color:var(--info-fg);text-decoration:none}"
    ".alert-agent:hover{text-decoration:underline}"

    /* ── Chat ── */
    ".chat{display:flex;flex-direction:column;height:100%;min-height:0}"
    /*
     * `scrollbar-gutter` on both, and the composer made a scroll
     * container purely so it has a gutter to reserve.
     *
     * The measure is a share of the window now, and a percentage
     * resolves against each element's own containing block -- so a
     * classic scrollbar appearing in the transcript took 15px off the
     * column while the composer, which never scrolls, kept all of it.
     * Measured in a real browser at 1280px: the transcript's column
     * came out 431.09px against the composer's 444.59, a 13.5px step
     * between the words and the box you type them into, and it
     * appeared the moment a conversation grew past one screen.  With a
     * fixed column both were capped at the same number and the
     * scrollbar showed only as a few pixels of centring; a share turns
     * it into a width.
     *
     * Reserving on both sides is what makes them equal, and reserving
     * it *always* is what stops the column stepping sideways the first
     * time a conversation runs past the fold.  Measured at 0.00px
     * difference in width and in left edge, scrolling and not.
     *
     * A browser with overlay scrollbars reserves nothing for either and
     * the pair were already equal; this costs it nothing.
     */
    ".transcript{flex:1;overflow-y:auto;scrollbar-gutter:stable;"
      "padding:28px 32px;min-height:0}"
    /*
     * The measure, whatever unit the reader chose it in.
     *
     * The token is a share of the window by default, so the column
     * grows with the display rather than leaving a wide one two thirds
     * empty.  The numbers behind the old fixed column are still worth
     * recording, because they are what a reader picking `ch` is
     * choosing between: 52rem rendered 117 characters a line at this
     * font and 40rem rendered 89, taken from the browser's own text
     * metrics rather than estimated, against a comfortable 45 to 90 for
     * continuous prose.
     */
    ".transcript-inner{max-width:var(--chat-measure);margin:0 auto}"
    /*
     * A run is consecutive messages from one sender: one header, tight
     * spacing inside, a bigger gap between runs.  That grouping is what
     * makes a stack of paragraphs read as a conversation, and it is the
     * one thing here the GTK client and this one had to agree about --
     * so where a run *begins* is decided in libclawt and only the
     * spacing is decided here.
     */
    ".msg{animation:rise 420ms cubic-bezier(.16,1,.3,1)}"
    ".msg.run-start{margin-top:var(--chat-run-gap)}"
    /*
     * 24, not 6.
     *
     * Measured at the default font: a line is 18px and a markdown
     * paragraph break is one blank line, so a new *message* used to be
     * separated by a third of what separates two paragraphs of one
     * message -- three turns reading as one message with tight
     * paragraphs.  18 / 24 / 30 are even 6px steps, and the window was
     * 19 to 29: at 30 a message reads as a new run and the grouping
     * carries nothing.
     */
    ".msg.run-cont{margin-top:var(--chat-msg-gap);position:relative}"
    /*
     * And the time in the gutter, which is the half space cannot carry.
     * Absolutely placed into the avatar's column so it costs no width
     * that was not already reserved.
     */
    ".msg-time{position:absolute;left:0;top:0;width:var(--chat-gutter);"
      "font-size:11px;color:var(--muted);line-height:1.5;"
      /*
       * Right-aligned on the avatar's edge, with tabular figures, so a
       * column of them is one straight rail rather than ragged on both
       * sides. 36px gutter less the 28px avatar is the 8px gap beside
       * it, so the digits end where the avatar ends.
       *
       * The same convention as the GTK transcript, which arrives at it
       * from the other direction -- an overlay child there, absolute
       * positioning here, both so the time costs no width that was not
       * already reserved. Two clients drawing one kind of content
       * differently is the drift `make parity` cannot see.
       */
      "text-align:right;padding-right:8px;"
      "font-variant-numeric:tabular-nums}"
    "@media (max-width:26rem){.msg-time{display:none}}"
    ".transcript-inner>.msg:first-child{margin-top:0}"
    "@keyframes rise{from{opacity:0;transform:translateY(8px)}"
      "to{opacity:1;transform:none}}"
    /*
     * The run header: face, name, time.  Not uppercased any more -- a
     * name set in small caps reads as metadata about a message rather
     * than as a person saying something.
     */
    ".msg-who{display:flex;align-items:center;gap:8px;font-size:13px;"
      "color:var(--ink-2);margin-bottom:3px;font-weight:600}"
    ".msg-avatar{display:inline-flex;align-items:center;"
      "justify-content:center;width:28px;height:28px;border-radius:50%;"
      "font-size:12px;font-weight:700;flex:0 0 auto;"
      "background:var(--neutral-bg);color:var(--neutral-fg)}"
    /*
     * The `<img>` half of clawt_web_avatar(): decoded and cropped to
     * fill its circle rather than stretched to it, whatever the
     * picture's own aspect ratio was.
     */
    ".web-avatar-img{object-fit:cover}"
    /*
     * The derived tones, from the palette rather than computed, so a
     * colour scheme recolours the faces with everything else.
     */
    ".avatar-tone-0{background:var(--info-bg);color:var(--info-fg)}"
    ".avatar-tone-1{background:var(--good-bg);color:var(--good-fg)}"
    ".avatar-tone-2{background:var(--warn-bg);color:var(--warn-fg)}"
    ".avatar-tone-3{background:var(--bad-bg);color:var(--bad-fg)}"
    ".avatar-tone-4{background:var(--neutral-bg);color:var(--neutral-fg)}"
    ".avatar-tone-5{background:var(--surface-2);color:var(--ink-2)}"
    /*
     * Every body in a run indented to the same 36px -- avatar plus its
     * gap -- so the left edge of the text is unbroken down the run.
     */
    /*
     * No white-space:pre-wrap.  A body was plain text with real
     * newlines in it until clawt_markdown_to_html() started rendering
     * it, and pre-wrap over block markup shows every newline between
     * two tags as a blank line -- so the markup's own formatting would
     * become visible gaps down the transcript.  The line breaks
     * somebody typed survive as <br>, which is what the renderer emits
     * for them.
     */
    ".msg-body{word-wrap:break-word;overflow-wrap:anywhere;"
      "margin-left:var(--chat-gutter)}"
    ".msg-body code{font-family:var(--mono);font-size:var(--mono-size);"
      "background:var(--surface-2);padding:1px 5px;border-radius:4px}"
    ".msg-body pre{font-family:var(--mono);font-size:var(--mono-size);"
      "background:var(--surface-2);border:1px solid var(--line);"
      "border-radius:var(--radius-sm);padding:12px 14px;overflow-x:auto}"
    /* A code span inside a block is already in the block's box. */
    ".msg-body pre code{background:none;padding:0;font-size:inherit}"
    /*
     * The blocks a rendered message can contain.
     *
     * First and last child lose their outer margin so a one-paragraph
     * message -- which is most of them -- occupies exactly the height
     * of its text and the run spacing stays the measured 30px rather
     * than 30 plus whatever a <p> adds.
     */
    ".msg-body p{margin:0 0 12px}"
    ".msg-body>:first-child{margin-top:0}"
    ".msg-body>:last-child{margin-bottom:0}"
    /*
     * Headings inside a message are the agent's outline, not the
     * page's, so they are sized against the body rather than against
     * the chrome -- an agent that opens with `# Title` should not out-
     * shout the topbar.
     */
    ".msg-body h1,.msg-body h2,.msg-body h3,"
      ".msg-body h4,.msg-body h5,.msg-body h6{"
      "font-weight:700;line-height:1.3;margin:18px 0 8px}"
    ".msg-body h1{font-size:1.25em}"
    ".msg-body h2{font-size:1.15em}"
    ".msg-body h3{font-size:1.05em}"
    ".msg-body h4,.msg-body h5,.msg-body h6{font-size:1em}"
    ".msg-body ul,.msg-body ol{margin:0 0 12px;padding-left:1.4em}"
    ".msg-body li{margin:2px 0}"
    ".msg-body li>ul,.msg-body li>ol{margin-bottom:0}"
    ".msg-body blockquote{margin:0 0 12px;padding-left:12px;"
      "border-left:3px solid var(--line-strong);color:var(--ink-2)}"
    ".msg-body hr{border:0;border-top:1px solid var(--line);margin:18px 0}"
    /*
     * A link an agent wrote is not clickable, in either client.  It is
     * one keystroke between a prompt injection and somewhere else, and
     * a person who can see where it goes is a person who can decide --
     * so the text is marked and the target is printed beside it.
     */
    ".msg-body .md-link{text-decoration:underline;"
      "text-underline-offset:2px}"
    ".msg-body .md-url{color:var(--muted);font-size:0.9em;"
      "word-break:break-all}"
    /*
     * A table scrolls inside its own box rather than widening the
     * message.  The GTK client has to choose between a grid and a
     * record layout because a label cannot scroll; here the column
     * beside it simply does not move.
     */
    ".msg-body .md-table{overflow-x:auto;margin:0 0 12px;"
      "border:1px solid var(--line);border-radius:var(--radius-sm)}"
    ".msg-body table{border-collapse:collapse;width:100%;"
      "font-size:0.95em}"
    /*
     * Every one of these resets something the fleet table's `th` sets.
     *
     * That rule styles a *column heading in the chrome* -- uppercase,
     * letter-spaced, 11px, muted -- and a bare `th` selector reaches
     * into a message as well, so an agent writing `| Team |` got TEAM
     * in the tone the page uses for labels rather than for text.
     * Measured on the rendered page at 4.14:1 against its own
     * background, which is the giveaway: nothing in a message should be
     * quieter than the message.  A table an agent wrote is content, and
     * inherits the body it sits in -- which also carries it into the
     * operator's bubble, where the colour is not the page's at all.
     */
    ".msg-body th,.msg-body td{padding:6px 10px;text-align:left;"
      "border-bottom:1px solid var(--line);vertical-align:top;"
      "color:inherit;font-size:inherit;letter-spacing:normal;"
      "text-transform:none;white-space:normal}"
    ".msg-body thead th{background:var(--surface-2);font-weight:700}"
    ".msg-body tbody tr:last-child td{border-bottom:0}"
    ".msg-body .md-c{text-align:center}"
    ".msg-body .md-r{text-align:right}"
    /*
     * The operator's turns are bubbles, and only the operator's.  An
     * agent's turn runs to dozens of lines with headings and code
     * blocks: a container that long stops reading as a message and
     * starts reading as a panel, and a bubble wide enough to be a bubble
     * is too wide to have a measure.  So the bubble goes where it works,
     * and the asymmetry is what says who is speaking at a glance.
     */
    ".msg-self{display:flex;flex-direction:column;align-items:flex-end}"
    ".msg-self .msg-who{color:var(--muted);font-weight:400;font-size:11px}"
    ".msg-self .msg-body{margin-left:0;max-width:26rem;"
      "background:var(--info-fg);color:var(--surface);"
      "padding:8px 12px;border-radius:12px}"
    /* A run of bubbles reads as one utterance. */
    ".msg-self.run-cont .msg-body{border-top-right-radius:4px}"
    ".msg-self .msg-body code,.msg-self .msg-body pre{"
      "background:rgba(255,255,255,0.18);color:inherit;border-color:"
      "transparent}"
    /*
     * The bubble is painted in the accent, so every token inside it has
     * to come from the bubble rather than from the page -- a muted grey
     * that reads on the canvas is invisible on the accent, and that is
     * the operator's own message.
     */
    ".msg-self .msg-body blockquote{border-left-color:"
      "rgba(255,255,255,0.45);color:inherit}"
    ".msg-self .msg-body hr{border-top-color:rgba(255,255,255,0.35)}"
    ".msg-self .msg-body .md-url{color:inherit;opacity:0.75}"
    ".msg-self .msg-body .md-table,"
      ".msg-self .msg-body th,.msg-self .msg-body td{"
      "border-color:rgba(255,255,255,0.35)}"
    /*
     * No tint on the header row inside a bubble.
     *
     * The tint is what separates a header from its body on the page,
     * and over the accent it lightens the ground under the one row
     * drawn in bold: measured at 3.94:1 against 5.67 for every other
     * line in the same bubble, so the heading was the least readable
     * thing in it.  A firmer rule under the row separates it for
     * nothing, and the bold was already doing most of the work.
     */
    ".msg-self .msg-body thead th{background:none;"
      "border-bottom:2px solid rgba(255,255,255,0.55)}"
    /*
     * A date change is a bigger break than a speaker change, so it gets
     * more room than the gap it sits among.
     */
    ".day-divider{display:flex;align-items:center;gap:12px;"
      "margin:30px 0 4px;font-size:11px;color:var(--muted)}"
    ".day-divider::before,.day-divider::after{content:'';flex:1;height:1px;"
      "background:var(--line)}"
    /*
     * The gutter stops being affordable on a narrow screen: at 360px the
     * indent takes the measure below the comfortable range, and the
     * avatar is the first thing that should go rather than the text.
     */
    "@media (max-width:26rem){.msg-body{margin-left:0}"
      ".msg-avatar{display:none}}"

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
      "overflow:hidden;scrollbar-gutter:stable;padding:16px 32px}"
    /*
     * The composer follows the transcript's column.  A full-width entry
     * under a narrow column of text reads as a rendering fault rather
     * than as a layout: the thing you read and the thing you type into
     * should be the same column.
     *
     * The column, though -- not the clamp.  Both are 40rem and centred
     * alike, and they still did not line up, because only the
     * transcript spends anything on the avatar: a body starts a gutter
     * in and the entry started at the clamp, so the strongest vertical
     * line on the page stood inside the column kept empty for faces.
     * box-sizing is border-box throughout, so the padding takes the
     * indent out of the 40rem rather than adding to it and the trailing
     * edges stay together.
     */
    ".composer-inner{max-width:var(--chat-measure);margin:0 auto;display:flex;gap:10px;"
      "align-items:flex-end;padding-left:var(--chat-gutter)}"
    /*
     * Narrow enough and the avatar is hidden, so there is no gutter to
     * stand past and the composer goes back to the clamp with the
     * bodies.  It has to sit *after* the rule above: the selectors are
     * identical in specificity, so source order is the only thing
     * deciding, and grouped with the other narrow overrides -- which
     * appear earlier in this sheet -- it would lose every time and do
     * nothing at all.
     */
    "@media (max-width:26rem){.composer-inner{padding-left:0}}"
    ".composer textarea{flex:1;min-height:2.6rem;max-height:14rem;"
      "font-family:var(--sans);font-size:14px}"
    /*
     * Stop does not wrap or shrink.  It appears mid-conversation, and a
     * button that reflows the row as it arrives moves Send under a
     * cursor that was aiming at it.
     */
    ".composer-inner .stop-turn{white-space:nowrap;flex:none}"

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

    /*
     * The persona-size note.  A badge on its own line above the sentence,
     * because the sentence names the files to shorten and wrapping it
     * around a floated badge would break exactly where somebody reads.
     */
    ".clawt-identity-size{margin-top:.5rem;display:flex;flex-direction:column;"
      "align-items:flex-start;gap:.25rem}"
    ".clawt-identity-size p{margin:0}"

    /*
     * The connection banner.  Full width of the content column and above
     * the page rather than floating over it: it describes a state the
     * whole page is in, and a reader scrolling away from it would be
     * scrolling away from the reason nothing is updating.
     */
    ".clawt-connection-banner{background:var(--warn-bg);color:var(--warn-fg);"
      "border-bottom:1px solid var(--line);padding:.6rem 1rem;"
      "font-size:13px}"
    ".clawt-connection-banner p{margin:0}"
    ".stack>*+*{margin-top:12px}"
    ".htmx-request .htmx-hide{opacity:.5}"

    /* ── The drawer's memory ── */
    /*
     * The checkbox is never seen; it is the drawer's memory.
     *
     * It has to live outside the sidebar because the sidebar is swapped
     * outerHTML on every `sse:fleet` event -- a <details> or any state
     * held inside it would close itself whenever an agent changed state,
     * which on a live fleet is constantly.  A hidden input before it in
     * the same grid, driven by a <label for>, survives every swap.
     *
     * display:none rather than a visually-hidden position:absolute: a
     * label still toggles a display:none checkbox, and this way it takes
     * no grid track.
     */
    ".nav-toggle{display:none}"
    ".nav-button{display:none}"

    /* ── Narrow ── */
    "@media (max-width:56rem){"
      /*
       * Two rows, and both children are placed explicitly.
       *
       * Left to implicit placement the content lands in whichever track
       * comes first, and with the sidebar display:none there is only one
       * item -- so it took the `auto` track and sized to its own
       * content.  Measured on a 375x812 phone: an 812px app box with the
       * composer ending at y=492 and 320px of blank screen below it.
       * A row count cannot describe a layout whose item count changes.
       *
       * minmax(0,1fr) rather than 1fr, because 1fr's minimum is
       * min-content: a long transcript would push the track past the
       * viewport instead of scrolling inside it.
       */
      ".app{grid-template-columns:1fr;grid-template-rows:auto minmax(0,1fr)}"
      /*
       * Closed by default, rather than a 14rem band above every page.
       * On a ~660px phone viewport that band was a third of the screen
       * permanently spent on navigation, and the content began below it
       * on every view.
       */
      ".sidebar{display:none;border-right:0;grid-row:1;grid-column:1;"
        "border-bottom:1px solid var(--line)}"
      ".content{grid-row:2;grid-column:1;min-height:0}"
      ".nav-toggle:checked~.sidebar{display:flex;max-height:60vh;"
        "max-height:60dvh}"
      ".nav-button{display:inline-flex;align-items:center;"
        "justify-content:center;width:2.25rem;height:2.25rem;margin-right:2px;"
        "border:1px solid var(--line-strong);border-radius:var(--radius-sm);"
        "background:var(--surface);color:var(--ink);cursor:pointer;"
        "font-size:15px;line-height:1;flex:none}"
      ".view-pad,.view-wide{padding:20px 18px 40px}"
      ".transcript,.composer{padding-left:18px;padding-right:18px}"
    "}"
    "@media (prefers-reduced-motion:reduce){"
      "*{animation:none!important;transition:none!important}"
    "}";
}
