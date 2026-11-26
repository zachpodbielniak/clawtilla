/*
 * office.js - the drawn office on clawtilla-web's Office page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Draws the director's layout (office.layout) on two canvases: the floor
 * -- the world pack's Tiled map, which leans back under a CSS transform
 * in the tilted view -- and the people, drawn upright over it at the
 * points that same transform puts their seats.  Everything it draws comes
 * from the scene the page carries in #office-scene and from
 * /office/layout.json, polled while the tab is visible; it decides no
 * positions of its own.  The one exception is ?simulate=1, which invents
 * plausible activity for the real participants here in the browser, says
 * so on screen, and never sends any of it anywhere.
 *
 * An agent the custodial team has given a character is drawn from its
 * sprite sheet (office.characters) instead of the default body, and the
 * wall the pack tags usage_board carries each provider login's allowance
 * (usage.providers as it stands, worded and levelled by libclawt's
 * clawt_office_board_new() server side -- this script only lays out and
 * paints the words it is given).  The board is fitted to its zone; a
 * hover or click opens a details panel beside it, and the Usage button
 * docks that panel in the corner instead, or turns usage off.
 *
 * Served by clawtilla-web from its own directory like htmx.min.js; it
 * fetches nothing but its own origin's /office/*.json and, after the
 * first page, a changed sprite sheet from /office/character/.
 */
(function () {
	'use strict';

	var root = document.getElementById('office-view');
	var data_el = document.getElementById('office-scene');
	var scene;

	if (!root || !data_el)
		return;
	try {
		scene = JSON.parse(data_el.textContent);
	} catch (e) {
		return;
	}

	/* -- Elements and settings -------------------------------------- */

	var stage = root.querySelector('.office-stage');
	var floor_cv = root.querySelector('.office-floor');
	var actor_cv = root.querySelector('.office-actors');
	var light_el = root.querySelector('.office-light');
	var tip = root.querySelector('.office-tip');
	var tilt_btn = root.querySelector('[data-office-tilt]');
	var rooms_btn = root.querySelector('[data-office-rooms]');
	var usage_btn = root.querySelector('[data-office-usage]');
	var sim_label = root.querySelector('.office-sim');
	var title_el = root.querySelector('.office-world-name');
	var floor_ctx = floor_cv.getContext('2d');
	var actor_ctx = actor_cv.getContext('2d');
	var simulate = /(?:^|[?&])simulate=1(?:&|$)/.test(location.search.slice(1));
	var reduced = window.matchMedia &&
		window.matchMedia('(prefers-reduced-motion: reduce)').matches;

	var TILT_DEGREES = 40;      /* how far the floor leans back */
	var PERSPECTIVE = 1.9;      /* the eye's distance, in floor heights */
	var WALK_SPEED = 150;       /* map pixels a second */
	var POLL_MS = 2500;
	var SIM_MS = 6000;

	/* Settings a viewer keeps: per browser, and harmless to lose. */
	function pref_get(name, fallback) {
		try {
			var v = window.localStorage.getItem('clawt-office-' + name);
			return v === null ? fallback : v === '1';
		} catch (e) {
			return fallback;
		}
	}
	function pref_set(name, on) {
		try {
			window.localStorage.setItem('clawt-office-' + name, on ? '1' : '0');
		} catch (e) { /* private mode: the toggle still works this visit */ }
	}

	/* ?tilt=1 opens tilted, for a link to the view, without changing the setting */
	var want_tilt = /(?:^|[?&])tilt=1(?:&|$)/.test(location.search.slice(1)) ||
		pref_get('tilt', false);
	var show_rooms = pref_get('rooms', true);

	/* where provider usage goes: world, corner or off, kept like the toggles */
	var USAGE_PLACES = ['world', 'corner', 'off'];
	var USAGE_WORDS = { world: 'on the wall', corner: 'in the corner', off: 'off' };
	var usage_place = (function () {
		var v = null;

		try {
			v = window.localStorage.getItem('clawt-office-usage');
		} catch (e) { /* private mode: the default */ }
		return USAGE_PLACES.indexOf(v) >= 0 ? v : 'world';
	})();
	var tilt = want_tilt ? 1 : 0;   /* 0 flat .. 1 tilted, eased between */

	/* -- The world -------------------------------------------------- */

	var world = scene.world || {};
	var map = scene.map || null;
	var words = scene.words || {};
	var map_w = 0, map_h = 0, tile = 32;
	var map_cache = null;         /* the map at one canvas pixel per map pixel */
	var rooms = [];               /* {name, x, y, w, h} from the zones layer */
	var board_at = null;          /* the usage_board's first anchor, map pixels */
	var board_rect = null;        /* its zone, {x, y, w, h} in map pixels */
	var board = scene.board || null;   /* {logins: [...]}, libclawt's words */

	/* The zones by tag, read from the pack (meeting:10 answers meeting). */
	function zones_tagged(tag) {
		var out = [];

		(world.zones || []).forEach(function (z) {
			var hit = (z.tags || []).some(function (t) {
				return t === tag || t.split(':')[0] === tag;
			});

			if (hit && (z.anchors_2d || []).length > 0)
				out.push(z);
		});
		return out;
	}

	/*
	 * The first anchor of the first zone tagged exactly usage_board, as
	 * the lrg scene finds it: the wall the allowances are drawn on.
	 */
	function board_anchor() {
		var zones = world.zones || [], i, a;

		for (i = 0; i < zones.length; i++) {
			a = (zones[i].anchors_2d || [])[0];
			if ((zones[i].tags || []).indexOf('usage_board') >= 0 && a && a.length >= 2)
				return { x: a[0], y: a[1] };
		}
		return null;
	}

	/*
	 * The usage board's zone: its rect_2d when the pack gives one, else
	 * five tiles square round its anchor -- as the lrg scene decides it.
	 */
	function board_zone() {
		var zones = world.zones || [], i, r;

		if (!board_at)
			return null;
		for (i = 0; i < zones.length; i++) {
			r = zones[i].rect_2d;
			if ((zones[i].tags || []).indexOf('usage_board') >= 0 && r && r.length === 4 &&
			    r[2] > 0 && r[3] > 0)
				return { x: r[0], y: r[1], w: r[2], h: r[3] };
		}
		return { x: board_at.x - 2.5 * tile, y: board_at.y - 2.5 * tile,
		         w: 5 * tile, h: 5 * tile };
	}

	function zone_name(id) {
		var found = (world.zones || []).filter(function (z) { return z.id === id; })[0];

		return found ? (found.name || found.id) : id;
	}

	/*
	 * The map's size, its rooms, and the floor drawn once into a cache.
	 * A pack with no 2D map still has zones: their anchors on a plain
	 * floor are enough to see who is where.
	 */
	function load_world() {
		var pending = 0;
		var sets = [];

		map_cache = null;
		rooms = [];
		board_at = board_anchor();
		if (map && map.width && map.height) {
			tile = map.tilewidth || 32;
			map_w = map.width * (map.tilewidth || 32);
			map_h = map.height * (map.tileheight || 32);
		} else {
			map_w = 640;
			map_h = 480;
			(world.zones || []).forEach(function (z) {
				(z.anchors_2d || []).forEach(function (a) {
					map_w = Math.max(map_w, a[0] + 96);
					map_h = Math.max(map_h, a[1] + 96);
				});
			});
		}
		board_rect = board_zone();

		if (map) {
			(map.layers || []).forEach(function (layer) {
				if (layer.type !== 'objectgroup')
					return;
				(layer.objects || []).forEach(function (o) {
					if (o.width > 0 && o.height > 0 && o.name)
						rooms.push({ name: zone_name(o.name), x: o.x, y: o.y,
						             w: o.width, h: o.height });
				});
			});
			(map.tilesets || []).forEach(function (ts) {
				var set = { first: ts.firstgid || 1, columns: ts.columns || 1,
				            tw: ts.tilewidth || tile, th: ts.tileheight || tile,
				            margin: ts.margin || 0, spacing: ts.spacing || 0,
				            img: null };

				sets.push(set);
				if (!ts.src)
					return;
				pending++;
				set.img = new Image();
				set.img.onload = set.img.onerror = function () {
					if (--pending === 0)
						paint_cache(sets);
				};
				set.img.src = ts.src;
			});
			sets.sort(function (a, b) { return b.first - a.first; });
		}
		if (pending === 0)
			paint_cache(sets);
		layout_stage();
	}

	function paint_cache(sets) {
		var cache = document.createElement('canvas');
		var c = cache.getContext('2d');

		cache.width = map_w;
		cache.height = map_h;
		c.imageSmoothingEnabled = false;
		c.fillStyle = '#d9d4c7';
		c.fillRect(0, 0, map_w, map_h);

		if (map) {
			(map.layers || []).forEach(function (layer) {
				if (layer.type !== 'tilelayer' || layer.visible === false ||
				    !Array.isArray(layer.data))
					return;
				c.globalAlpha = layer.opacity === undefined ? 1 : layer.opacity;
				layer.data.forEach(function (raw, i) {
					/* the top three bits are Tiled's flip flags */
					var gid = raw & 0x1fffffff;
					var set, local, sx, sy, dx, dy;

					if (gid === 0)
						return;
					set = sets.filter(function (s) { return s.first <= gid; })[0];
					if (!set || !set.img || !set.img.naturalWidth)
						return;
					local = gid - set.first;
					sx = set.margin + (local % set.columns) * (set.tw + set.spacing);
					sy = set.margin + Math.floor(local / set.columns) * (set.th + set.spacing);
					dx = (i % map.width) * (map.tilewidth || tile);
					dy = Math.floor(i / map.width) * (map.tileheight || tile);
					c.drawImage(set.img, sx, sy, set.tw, set.th, dx,
					            dy + (map.tileheight || tile) - set.th, set.tw, set.th);
				});
			});
			c.globalAlpha = 1;
		} else {
			/* no map: the zones as soft pads, so the floor still reads */
			(world.zones || []).forEach(function (z) {
				(z.anchors_2d || []).forEach(function (a) {
					c.fillStyle = 'rgba(255,255,255,.55)';
					c.beginPath();
					c.arc(a[0], a[1], 18, 0, Math.PI * 2);
					c.fill();
				});
			});
		}
		map_cache = cache;
	}

	/* -- Colours from the page's own tokens ------------------------- */

	var ink = {};

	function read_tokens() {
		var cs = getComputedStyle(document.documentElement);
		var get = function (n, f) { return (cs.getPropertyValue(n) || '').trim() || f; };

		ink = {
			surface: get('--surface', '#fff'),
			ink: get('--ink', '#111'),
			muted: get('--muted', '#6e6d6b'),
			line: get('--line-strong', '#dcdcda'),
			good: get('--good-fg', '#346538'),
			warn: get('--warn-fg', '#956400'),
			bad: get('--bad-fg', '#9f2f2d'),
			info: get('--info-fg', '#1f6c9f'),
			neutral: get('--neutral-fg', '#5f5e5b'),
			sans: get('--sans', 'system-ui,sans-serif')
		};
	}

	function tone_of(activity) {
		var w = words[activity];

		return (w && ink[w.tone]) || ink.neutral;
	}

	/* A body colour a shade darker, for outlines and legs. */
	function shade(hex, f) {
		var m = /^#?([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(hex || '');
		var h, r, g, b;

		if (!m)
			return 'rgba(0,0,0,.45)';
		h = m[1].length === 3 ? m[1].replace(/./g, '$&$&') : m[1];
		r = Math.round(parseInt(h.slice(0, 2), 16) * f);
		g = Math.round(parseInt(h.slice(2, 4), 16) * f);
		b = Math.round(parseInt(h.slice(4, 6), 16) * f);
		return 'rgb(' + r + ',' + g + ',' + b + ')';
	}

	/* -- People ----------------------------------------------------- */

	var faces = {};       /* id -> the scene's face entry */
	var face_imgs = {};   /* id -> Image, made once per face */
	var lines = {};       /* id -> roster line */
	var people = {};      /* id -> participant */
	var real_placements = [];
	var actors = {};      /* id -> what is on screen and where it is going */

	function face_image(id) {
		var f = faces[id];
		var img, svg;

		if (!f)
			return null;
		if (face_imgs[id] && face_imgs[id].key === (f.img || f.svg))
			return face_imgs[id];
		img = new Image();
		if (f.img) {
			img.src = f.img;
		} else if (f.svg) {
			svg = f.svg.indexOf('xmlns=') < 0
				? f.svg.replace('<svg', '<svg xmlns="http://www.w3.org/2000/svg"')
				: f.svg;
			img.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg);
		} else {
			return null;
		}
		img.key = f.img || f.svg;
		face_imgs[id] = img;
		return img;
	}

	/*
	 * Characters: agent -> {def, img, key}.  A sheet is loaded again only
	 * when its checksum changes -- the first page carries it inline, the
	 * poll as a URL -- so a poll every 2.5 s costs no image request.  The
	 * bytes were held to the manifest's sha256 by clawtilla-web before
	 * they reached here (inlined only when they match; the route answers
	 * 409 to bytes that do not), so a sheet that does not match is never
	 * drawn.  The browser does not hash it again: SubtleCrypto is absent
	 * from a plain-http tailnet page, which is where this client lives.
	 */
	var characters = {};

	function take_characters(list) {
		var seen = {};

		if (!Array.isArray(list))
			return;
		list.forEach(function (c) {
			var key, have, img;

			if (!c || !c.agent || !c.src || !Array.isArray(c.frame) || !c.animations)
				return;
			seen[c.agent] = true;
			/* a new backdrop is a new picture, the same as new bytes */
			key = (c.sheet_sha256 || c.src) + '|' + (c.background || '');
			have = characters[c.agent];
			if (have && have.key === key) {
				have.def = c;
				return;
			}
			img = new Image();
			have = { def: c, img: img, key: key, sheet: null };
			img.onload = function () {
				var keyed = keyed_sheet(img, c);

				have.sheet = keyed.sheet;
				have.trim = keyed.trim;
				last_paint = 0;
			};
			img.src = c.src;
			characters[c.agent] = have;
		});
		Object.keys(characters).forEach(function (id) {
			if (!seen[id])
				delete characters[id];
		});
	}

	/*
	 * The row a character draws now, as office-scene.c's pick_animation():
	 * walking picks the direction of travel, otherwise the pose (sit,
	 * type, read, phone, present), otherwise idle, otherwise the sheet's
	 * first row.  `own` says the row is the one asked for rather than a
	 * fallback.
	 */
	function pick_animation(anims, s) {
		var want = null, names;

		if (s.walking)
			want = Math.abs(s.dx) > Math.abs(s.dy)
				? (s.dx > 0 ? 'walk_right' : 'walk_left')
				: (s.dy > 0 ? 'walk_down' : 'walk_up');
		else if (s.pose && s.pose !== 'stand')
			want = s.pose;
		if (want && anims[want])
			return { a: anims[want], own: true };
		if (s.walking && anims.walk_down)
			return { a: anims.walk_down, own: true };
		if (anims.idle)
			return { a: anims.idle, own: false };
		names = Object.keys(anims);
		return names.length ? { a: anims[names[0]], own: false } : null;
	}

	/*
	 * Whether @s's frame changes with time: walking, or a pose other than
	 * standing and sitting that the sheet has a row for.  A still pose --
	 * and any row reached as a fallback -- holds its first frame: nobody
	 * bobs while idle.
	 */
	function character_animates(s, pick) {
		return !reduced && !!pick && pick.own && (pick.a.fps || 0) > 0 &&
		       (pick.a.frames || 1) > 1 &&
		       (s.walking || (!!s.pose && s.pose !== 'stand' && s.pose !== 'sit'));
	}

	/* @s's character, when it has one whose sheet has loaded. */
	function character_of(s) {
		var ch = characters[s.id];

		if (!ch || !ch.sheet)
			return null;
		return ch;
	}

	/*
	 * The sheet with its backdrop made transparent, when the character
	 * says it has one (a generator's portrait on a plain colour) -- the
	 * rule of clawt_office_character_key_background() in libclawt, so
	 * both renderers cut the same pixels: per frame, the pixels within
	 * BACKDROP_TOLERANCE of the backdrop colour that connect to the
	 * frame's edge (the whites of the eyes are as close to a cream
	 * backdrop as its own noise, and are not connected to it); with
	 * "corners", a frame whose corners disagree is left alone.  Without a
	 * backdrop, or if the canvas will not give its pixels, the image as
	 * it is.
	 */
	var BACKDROP_TOLERANCE = 32;

	function keyed_sheet(img, def) {
		var bg = def.background, w = img.naturalWidth, h = img.naturalHeight;
		var canvas, cx, data, px, fw, fh, fx, fy, fixed = null;

		if (!bg || !w || !h)
			return { sheet: img, trim: null };
		if (bg !== 'corners') {
			if (!/^#[0-9a-fA-F]{6}$/.test(bg))
				return { sheet: img, trim: null };
			fixed = [parseInt(bg.substr(1, 2), 16), parseInt(bg.substr(3, 2), 16),
			         parseInt(bg.substr(5, 2), 16)];
		}
		canvas = document.createElement('canvas');
		canvas.width = w;
		canvas.height = h;
		cx = canvas.getContext('2d');
		cx.drawImage(img, 0, 0);
		try {
			data = cx.getImageData(0, 0, w, h);
		} catch (e) {
			return { sheet: img, trim: null };
		}
		px = data.data;
		fw = def.frame[0] > 0 && def.frame[0] <= w ? def.frame[0] : w;
		fh = def.frame[1] > 0 && def.frame[1] <= h ? def.frame[1] : h;

		function near(i, k) {
			return px[i + 3] >= 250 &&
			       Math.abs(px[i] - k[0]) <= BACKDROP_TOLERANCE &&
			       Math.abs(px[i + 1] - k[1]) <= BACKDROP_TOLERANCE &&
			       Math.abs(px[i + 2] - k[2]) <= BACKDROP_TOLERANCE;
		}

		for (fy = 0; fy + fh <= h; fy += fh) {
			for (fx = 0; fx + fw <= w; fx += fw) {
				var at = function (x, y) { return ((fy + y) * w + fx + x) * 4; };
				var tl = at(0, 0), k, stack = [], i, x, y, n;

				if (fixed)
					k = fixed;
				else {
					k = [px[tl], px[tl + 1], px[tl + 2]];
					if (px[tl + 3] < 250 || !near(at(fw - 1, 0), k) ||
					    !near(at(0, fh - 1), k) || !near(at(fw - 1, fh - 1), k))
						continue;
				}
				for (i = 0; i < fw; i++)
					stack.push(i, (fh - 1) * fw + i);
				for (i = 1; i + 1 < fh; i++)
					stack.push(i * fw, i * fw + fw - 1);
				while (stack.length) {
					n = stack.pop();
					x = n % fw;
					y = (n - x) / fw;
					i = at(x, y);
					if (!near(i, k))
						continue;
					px[i] = px[i + 1] = px[i + 2] = px[i + 3] = 0;
					if (x > 0) stack.push(n - 1);
					if (x + 1 < fw) stack.push(n + 1);
					if (y > 0) stack.push(n - fw);
					if (y + 1 < fh) stack.push(n + fw);
				}
			}
		}
		cx.putImageData(data, 0, 0);
		return { sheet: canvas, trim: opaque_bounds(px, w, h, fw, fh) };
	}

	/*
	 * The visible part of every frame, as one rectangle in a frame's own
	 * coordinates -- clawt_office_character_opaque_bounds() in libclawt.
	 * What was backdrop is margin once keyed, and drawn whole the frame
	 * makes the person half the height of everybody else.
	 */
	function opaque_bounds(px, w, h, fw, fh) {
		var l = Infinity, t = Infinity, r = -1, b = -1, fx, fy, x, y;

		for (fy = 0; fy + fh <= h; fy += fh)
			for (fx = 0; fx + fw <= w; fx += fw)
				for (y = 0; y < fh; y++)
					for (x = 0; x < fw; x++) {
						if (!px[((fy + y) * w + fx + x) * 4 + 3])
							continue;
						if (x < l) l = x;
						if (y < t) t = y;
						if (x > r) r = x;
						if (y > b) b = y;
					}
		return r < 0 ? null : { x: l, y: t, w: r - l + 1, h: b - t + 1 };
	}

	/*
	 * A person drawn from their sheet, feet on (x, y), about as tall as
	 * the default body, with smoothing off so pixel art stays crisp.
	 */
	function draw_character(c, s, ch, now) {
		var def = ch.def;
		var fw = def.frame[0], fh = def.frame[1];
		var pick = pick_animation(def.animations, s);
		var height = 48 * s.u, width, index = 0, row, sx, sy, sw, sh;

		if (!pick || !(fw > 0) || !(fh > 0))
			return false;
		row = pick.a.row || 0;
		if (character_animates(s, pick))
			index = Math.floor(now / 1000 * pick.a.fps) % Math.max(1, pick.a.frames || 1);
		sx = index * fw;
		sy = row * fh;
		sw = fw;
		sh = fh;
		if (ch.trim) {
			sx += ch.trim.x;
			sy += ch.trim.y;
			sw = ch.trim.w;
			sh = ch.trim.h;
		}
		width = height * sw / sh;
		c.save();
		c.globalAlpha = s.alpha;
		c.imageSmoothingEnabled = false;
		c.drawImage(ch.sheet, sx, sy, sw, sh,
		            Math.round(s.x - width / 2), Math.round(s.y - height), width, height);
		if (s.hover) {
			c.strokeStyle = ink.info;
			c.lineWidth = 2;
			round_rect(c, s.x - width / 2 - 2, s.y - height - 2, width + 4, height + 4, 4);
			c.stroke();
		}
		c.restore();
		s.head = { x: s.x, y: s.y - height + 10 * s.u, r: 10 * s.u };
		s.box = { x: s.x - width / 2, y: s.y - height, w: width, h: height + 2 * s.u + 14 };
		return true;
	}

	function ease(t) {
		return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
	}

	/*
	 * Points everybody at their new placement.  Somebody who moved walks
	 * there -- eased, at a walking pace -- rather than appearing; somebody
	 * new fades in where they are; somebody gone fades out.
	 */
	function apply_placements(placements, now) {
		var seen = {};

		placements.forEach(function (p) {
			var a = actors[p.id];

			if (typeof p.x !== 'number' || typeof p.y !== 'number')
				return;
			seen[p.id] = true;
			if (!a) {
				a = actors[p.id] = { id: p.id, x: p.x, y: p.y, fx: p.x, fy: p.y,
				                     tx: p.x, ty: p.y, t0: now, dur: 1,
				                     born: now, gone: 0 };
			}
			a.gone = 0;
			a.p = p;
			if (Math.abs(a.tx - p.x) > 0.5 || Math.abs(a.ty - p.y) > 0.5) {
				var cur = where(a, now);
				var dist = Math.hypot(p.x - cur.x, p.y - cur.y);

				a.fx = cur.x;
				a.fy = cur.y;
				a.tx = p.x;
				a.ty = p.y;
				a.t0 = now;
				a.dur = reduced ? 1 : Math.min(5000, Math.max(500, dist / WALK_SPEED * 1000));
			}
		});
		Object.keys(actors).forEach(function (id) {
			if (!seen[id] && !actors[id].gone)
				actors[id].gone = now;
		});
	}

	function where(a, now) {
		var t = Math.min(1, (now - a.t0) / a.dur);
		var e = ease(t);

		return { x: a.fx + (a.tx - a.fx) * e, y: a.fy + (a.ty - a.fy) * e,
		         walking: t < 1, t: t, dx: a.tx - a.fx, dy: a.ty - a.fy };
	}

	function take_layout(body, now) {
		var layout = body.layout || {};

		faces = body.faces || faces;
		lines = body.lines || lines;
		board = body.board || board;
		if (body.board)
			fill_panel();
		take_characters(body.characters);
		people = {};
		(layout.participants || []).forEach(function (p) { people[p.id] = p; });
		real_placements = layout.placements || [];
		if (!simulate)
			apply_placements(real_placements, now);
		return layout;
	}

	/* -- The stage: flat or tilted, and where a floor point lands -- */

	var view = { k: 1, w: 0, h: 0, ox: 0, W: 0, H: 0, a: 0, P: 1, q: 1, ty: 0, boost: 1 };

	/*
	 * Sizes the stage for the tilt `tilt` (0..1) and leans the floor back
	 * by that much.  project() below is the same transform as the CSS one
	 * written here -- translateY, scale, perspective, rotateX, about the
	 * floor's centre -- so a person stands exactly on their seat whatever
	 * the angle.
	 */
	function layout_stage() {
		var W = stage.clientWidth || 800;
		var max_h = Math.max(320, Math.min(window.innerHeight * 0.74, 900));
		var k = Math.min(W / map_w, max_h / map_h);
		var w = map_w * k, h = map_h * k;
		var a = TILT_DEGREES * Math.PI / 180 * tilt;
		var P = PERSPECTIVE * h;
		var s_top = P / (P + (h / 2) * Math.sin(a));
		var s_bot = P / (P - (h / 2) * Math.sin(a));
		var top = 64 * Math.max(1, Math.min(1.9, 1.05 / k)) * k * tilt;
		/*
		 * A leaning floor is shorter than a flat one, so it may grow into
		 * the room that frees -- as wide as the stage allows at its near
		 * edge, and no taller than the flat floor was.
		 */
		var fh = Math.cos(a) / 2 * (s_top + s_bot);
		var q = Math.min(1 + 0.35 * tilt, (W * 0.98) / (w * s_bot),
		                 Math.max(h, max_h * tilt) / (h * fh + top));
		var ty = top - h / 2 + q * (h / 2) * Math.cos(a) * s_top;
		var bottom = h / 2 + ty + q * (h / 2) * Math.cos(a) * s_bot;
		var H = Math.round(Math.max(bottom + 14 * tilt, h * (1 - tilt) + bottom * tilt));
		var dpr = window.devicePixelRatio || 1;

		/*
		 * People are drawn larger than the map's own scale: a whole floor
		 * fitted to a laptop's width leaves a 32px tile at a third of its
		 * size, and a face that small is a dot.  Up to 1.9x, so on a big
		 * screen they are true to the tiles.
		 */
		view = { k: k, w: w, h: h, ox: (W - w) / 2, W: W, H: H, a: a, P: P, q: q, ty: ty,
		         boost: Math.max(1, Math.min(1.9, 1.05 / k)) };
		stage.style.height = H + 'px';
		floor_cv.style.width = w + 'px';
		floor_cv.style.height = h + 'px';
		floor_cv.style.left = view.ox + 'px';
		floor_cv.style.transform = tilt > 0.001
			? 'translateY(' + ty.toFixed(2) + 'px) scale(' + q.toFixed(4) + ') perspective(' +
			  P.toFixed(1) + 'px) rotateX(' + (a * 180 / Math.PI).toFixed(3) + 'deg)'
			: 'none';
		if (floor_cv.width !== Math.round(w * dpr) || floor_cv.height !== Math.round(h * dpr)) {
			floor_cv.width = Math.round(w * dpr);
			floor_cv.height = Math.round(h * dpr);
		}
		if (actor_cv.width !== Math.round(W * dpr) || actor_cv.height !== Math.round(H * dpr)) {
			actor_cv.width = Math.round(W * dpr);
			actor_cv.height = Math.round(H * dpr);
			actor_cv.style.width = W + 'px';
			actor_cv.style.height = H + 'px';
		}
		root.classList.toggle('office-tilted', tilt > 0.5);
	}

	/* A floor point in map pixels -> stage pixels, and the scale there. */
	function project(mx, my) {
		var px = mx * view.k - view.w / 2;
		var py = my * view.k - view.h / 2;
		var yr = py * Math.cos(view.a);
		var z = py * Math.sin(view.a);
		var s = view.P / (view.P - z);

		return { x: view.ox + view.w / 2 + view.q * px * s,
		         y: view.h / 2 + view.ty + view.q * yr * s,
		         s: view.k * view.q * s };
	}

	/* -- Drawing: the floor ----------------------------------------- */

	function draw_floor(now, frame) {
		var dpr = window.devicePixelRatio || 1;
		var c = floor_ctx;
		var scale = view.k * dpr;

		c.setTransform(1, 0, 0, 1, 0, 0);
		c.clearRect(0, 0, floor_cv.width, floor_cv.height);
		c.setTransform(scale, 0, 0, scale, 0, 0);
		if (map_cache) {
			c.imageSmoothingEnabled = false;
			c.drawImage(map_cache, 0, 0);
		}

		/* a meeting is a pool of light round its table */
		frame.meetings.forEach(function (m) {
			var g = c.createRadialGradient(m.x, m.y, 4, m.x, m.y, m.r);

			g.addColorStop(0, 'rgba(255,214,120,.42)');
			g.addColorStop(1, 'rgba(255,214,120,0)');
			c.fillStyle = g;
			c.beginPath();
			c.arc(m.x, m.y, m.r, 0, Math.PI * 2);
			c.fill();
		});

		/* somebody waiting on you glows where they stand */
		frame.sprites.forEach(function (s) {
			var pulse;

			if (s.activity !== 'waiting_on_you')
				return;
			pulse = reduced ? 0.5 : 0.5 + 0.5 * Math.sin(now / 260);
			c.strokeStyle = 'rgba(245,166,35,' + (0.35 + 0.45 * pulse).toFixed(3) + ')';
			c.lineWidth = 3;
			c.beginPath();
			c.ellipse(s.mx, s.my + 2, (16 + 6 * pulse) * view.boost,
			          (8 + 3 * pulse) * view.boost, 0, 0, Math.PI * 2);
			c.stroke();
		});

		/* shadows: in the floor, so they lean with it */
		frame.sprites.forEach(function (s) {
			c.fillStyle = 'rgba(0,0,0,' + (0.22 * s.alpha).toFixed(3) + ')';
			c.beginPath();
			c.ellipse(s.mx, s.my + 2, 11 * view.boost, 5 * view.boost, 0, 0, Math.PI * 2);
			c.fill();
		});

		if (show_rooms) {
			c.font = '600 11px ' + ink.sans;
			c.textBaseline = 'top';
			rooms.forEach(function (r) {
				var label = r.name.toUpperCase();
				var w = c.measureText(label).width + 10;

				c.fillStyle = 'rgba(20,20,20,.55)';
				round_rect(c, r.x + 6, r.y + 6, w, 16, 4);
				c.fill();
				c.fillStyle = 'rgba(255,255,255,.92)';
				c.fillText(label, r.x + 11, r.y + 9);
			});
		}
	}

	function round_rect(c, x, y, w, h, r) {
		c.beginPath();
		c.moveTo(x + r, y);
		c.arcTo(x + w, y, x + w, y + h, r);
		c.arcTo(x + w, y + h, x, y + h, r);
		c.arcTo(x, y + h, x, y, r);
		c.arcTo(x, y, x + w, y, r);
		c.closePath();
	}

	/* -- Drawing: the people ---------------------------------------- */

	/* What hands hold, drawn small at (x, y) in stage pixels, u per map px. */
	function draw_prop(c, prop, x, y, u) {
		c.save();
		c.translate(x, y);
		c.scale(u, u);
		c.lineWidth = 1;
		switch (prop) {
		case 'laptop':
			c.fillStyle = '#2b2f36';
			c.fillRect(-7, -6, 14, 8);
			c.fillStyle = '#7fc8ff';
			c.fillRect(-6, -5, 12, 6);
			c.fillStyle = '#9aa1ab';
			c.fillRect(-9, 2, 18, 2);
			break;
		case 'phone':
			c.fillStyle = '#1d1f24';
			round_rect(c, -2.5, -5, 5, 9, 1.5);
			c.fill();
			c.fillStyle = '#7fc8ff';
			c.fillRect(-1.5, -4, 3, 6);
			break;
		case 'book':
			c.fillStyle = '#8b3a3a';
			c.fillRect(-6, -5, 12, 9);
			c.fillStyle = '#f4ead5';
			c.fillRect(-5, -4, 10, 7);
			c.fillStyle = '#8b3a3a';
			c.fillRect(-0.5, -5, 1, 9);
			break;
		case 'marker':
			c.fillStyle = '#e04a3a';
			c.fillRect(-1.5, -7, 3, 10);
			c.fillStyle = '#fff';
			c.fillRect(-1.5, -2, 3, 1.5);
			break;
		case 'folder':
			c.fillStyle = '#e8b04a';
			c.fillRect(-7, -4, 14, 9);
			c.fillRect(-7, -6, 6, 3);
			c.fillStyle = '#c98e2a';
			c.fillRect(-7, -1, 14, 1);
			break;
		case 'terminal':
		case 'screen':
			c.fillStyle = '#15191f';
			c.fillRect(-7, -6, 14, 10);
			c.fillStyle = prop === 'terminal' ? '#5fe08a' : '#7fc8ff';
			c.fillRect(-5, -4, 6, 1.5);
			c.fillRect(-5, -1, 9, 1.5);
			break;
		case 'coffee':
			c.fillStyle = '#f2efe9';
			c.fillRect(-3.5, -4, 7, 8);
			c.fillStyle = '#6b4226';
			c.fillRect(-3, -3.5, 6, 2);
			break;
		default:
			break;
		}
		c.restore();
	}

	/*
	 * The glyph over somebody's head.  A question is the loudest thing on
	 * the floor -- somebody is waiting on *you* -- so it is the largest,
	 * the brightest and the only one that pulses.
	 */
	function draw_emote(c, emote, x, y, u, now) {
		var r, pulse, i;

		c.save();
		c.translate(x, y);
		switch (emote) {
		case 'question':
			pulse = reduced ? 0 : (Math.sin(now / 260) + 1) / 2;
			r = (11 + 2 * pulse) * u;
			c.fillStyle = 'rgba(245,166,35,' + (0.25 * (1 - pulse)).toFixed(3) + ')';
			c.beginPath();
			c.arc(0, 0, r + 7 * u * pulse, 0, Math.PI * 2);
			c.fill();
			c.fillStyle = '#f5a623';
			c.strokeStyle = '#fff';
			c.lineWidth = 2;
			c.beginPath();
			c.arc(0, 0, r, 0, Math.PI * 2);
			c.fill();
			c.stroke();
			c.fillStyle = '#fff';
			c.font = '800 ' + Math.round(15 * u) + 'px ' + ink.sans;
			c.textAlign = 'center';
			c.textBaseline = 'middle';
			c.fillText('?', 0, 1 * u);
			break;
		case 'alert':
			r = 9 * u;
			c.fillStyle = '#e5484d';
			c.strokeStyle = '#fff';
			c.lineWidth = 1.5;
			c.beginPath();
			c.moveTo(0, -r);
			c.lineTo(r * 1.05, r * 0.8);
			c.lineTo(-r * 1.05, r * 0.8);
			c.closePath();
			c.fill();
			c.stroke();
			c.fillStyle = '#fff';
			c.font = '800 ' + Math.round(11 * u) + 'px ' + ink.sans;
			c.textAlign = 'center';
			c.textBaseline = 'middle';
			c.fillText('!', 0, 2 * u);
			break;
		case 'thought':
			c.fillStyle = 'rgba(255,255,255,.95)';
			c.strokeStyle = 'rgba(0,0,0,.25)';
			c.lineWidth = 1;
			c.beginPath();
			c.ellipse(0, 0, 11 * u, 7 * u, 0, 0, Math.PI * 2);
			c.fill();
			c.stroke();
			c.beginPath();
			c.arc(-6 * u, 9 * u, 2 * u, 0, Math.PI * 2);
			c.fill();
			c.stroke();
			for (i = 0; i < 3; i++) {
				var on = reduced ? 1 : (Math.floor(now / 400) % 3 === i ? 1 : 0.35);

				c.fillStyle = 'rgba(60,60,70,' + on + ')';
				c.beginPath();
				c.arc((i - 1) * 5 * u, 0, 1.6 * u, 0, Math.PI * 2);
				c.fill();
			}
			break;
		case 'zzz':
			c.fillStyle = 'rgba(120,130,160,.8)';
			c.font = 'italic 700 ' + Math.round(9 * u) + 'px ' + ink.sans;
			c.textAlign = 'center';
			c.fillText('z', -3 * u, 2 * u);
			c.font = 'italic 700 ' + Math.round(7 * u) + 'px ' + ink.sans;
			c.fillText('z', 3 * u, -4 * u);
			break;
		case 'wave':
			c.fillStyle = '#fff';
			c.strokeStyle = 'rgba(0,0,0,.25)';
			c.beginPath();
			c.arc(0, 0, 8 * u, 0, Math.PI * 2);
			c.fill();
			c.stroke();
			c.fillStyle = ink.info;
			c.font = '700 ' + Math.round(8 * u) + 'px ' + ink.sans;
			c.textAlign = 'center';
			c.textBaseline = 'middle';
			c.fillText('bye', 0, 0);
			break;
		default:
			break;
		}
		c.restore();
	}

	function short_name(name) {
		name = name || '';
		return name.length > 12 ? name.slice(0, 11) + '\u2026' : name;
	}

	function trim_text(text, n) {
		text = (text || '').replace(/\s+/g, ' ').trim();
		return text.length > n ? text.slice(0, n - 1) + '\u2026' : text;
	}

	/* A pill at (x, y) -- its centre-top -- in stage pixels. */
	function pill(c, text, x, y, px, bg, fg) {
		var w, h = px + 6;

		c.font = '600 ' + px + 'px ' + ink.sans;
		w = c.measureText(text).width + 10;
		c.fillStyle = bg;
		round_rect(c, x - w / 2, y, w, h, h / 2);
		c.fill();
		c.fillStyle = fg;
		c.textAlign = 'center';
		c.textBaseline = 'middle';
		c.fillText(text, x, y + h / 2 + 0.5);
		return { x: x - w / 2, y: y, w: w, h: h };
	}

	/*
	 * Where a nameplate can go without covering one already drawn this
	 * frame: round a meeting table the seats are closer than the names are
	 * long, and two plates on top of each other read as neither.
	 */
	var plates = [];
	var bubbles = [];

	function plate_y(c, name, x, y, px) {
		var w, h = px + 6, moved = true, tries = 0;

		c.font = '600 ' + px + 'px ' + ink.sans;
		w = c.measureText(short_name(name)).width + 10;
		while (moved && tries++ < 6) {
			moved = false;
			plates.forEach(function (p) {
				if (x - w / 2 < p.x + p.w && x + w / 2 > p.x && y < p.y + p.h && y + h > p.y) {
					y = p.y + p.h + 1;
					moved = true;
				}
			});
		}
		return y;
	}

	/* A speech bubble with somebody's detail, tail down to (x, y). */
	function bubble(c, text, x, y, u) {
		var px = Math.max(10, Math.round(10 * u));
		var w, h = px + 10, bx, by;

		c.font = '500 ' + px + 'px ' + ink.sans;
		w = c.measureText(text).width + 14;
		bx = Math.max(4, Math.min(view.W - w - 4, x - w / 2));
		by = Math.max(2, y - h - 6);
		/* lift it clear of a bubble already up, rather than over it */
		bubbles.forEach(function (o) {
			if (bx < o.x + o.w && bx + w > o.x && by < o.y + o.h && by + h > o.y)
				by = Math.max(2, o.y - h - 3);
		});
		bubbles.push({ x: bx, y: by, w: w, h: h });
		c.fillStyle = ink.surface;
		c.strokeStyle = ink.line;
		c.lineWidth = 1;
		round_rect(c, bx, by, w, h, 7);
		c.fill();
		c.stroke();
		c.beginPath();
		c.moveTo(x - 4, by + h - 0.5);
		c.lineTo(x, y - 1);
		c.lineTo(x + 4, by + h - 0.5);
		c.fill();
		c.fillStyle = ink.ink;
		c.textAlign = 'left';
		c.textBaseline = 'middle';
		c.fillText(text, bx + 7, by + h / 2 + 0.5);
	}

	/*
	 * One person, feet at (x, y) in stage pixels, u stage pixels to the
	 * map pixel.  A body in their colour, their face for a head; legs move
	 * only while they are walking -- nobody bobs while standing still.
	 */
	function draw_person(c, s, now) {
		var u = s.u, x = s.x, y = s.y;
		var color = (faces[s.id] && faces[s.id].color) || '#888';
		var dark = shade(color, 0.62);
		var sit = s.pose === 'sit' && !s.walking;
		var lift = sit ? 5 * u : 0;
		var step = s.walking && !reduced ? Math.sin(now / 90) * 3 * u : 0;
		var head_y = y - (38 * u) + lift;
		var r = 10.5 * u;
		var img = face_image(s.id);
		var ch = character_of(s);

		/* somebody with a character is their sprite sheet, and a nameplate */
		if (ch && draw_character(c, s, ch, now)) {
			c.save();
			c.globalAlpha = s.alpha;
			s.plate = pill(c, short_name(s.name), x, plate_y(c, s.name, x, y + 2 * u,
			                                                Math.max(9, Math.round(8.5 * u))),
			               Math.max(9, Math.round(8.5 * u)),
			               s.hover ? ink.ink : 'rgba(20,20,22,.72)',
			               s.hover ? ink.surface : '#fff');
			plates.push(s.plate);
			c.restore();
			return;
		}

		c.save();
		c.globalAlpha = s.alpha;

		/* legs */
		c.fillStyle = dark;
		if (sit) {
			c.fillRect(x - 6 * u, y - 7 * u, 5 * u, 5 * u);
			c.fillRect(x + 1 * u, y - 7 * u, 5 * u, 5 * u);
		} else {
			c.fillRect(x - 5.5 * u, y - 11 * u + step, 4.5 * u, 11 * u - step);
			c.fillRect(x + 1 * u, y - 11 * u - step, 4.5 * u, 11 * u + step);
		}

		/* torso */
		c.fillStyle = color;
		c.strokeStyle = dark;
		c.lineWidth = Math.max(1, u);
		round_rect(c, x - 8 * u, y - 27 * u + lift, 16 * u, 18 * u, 5 * u);
		c.fill();
		c.stroke();

		/* head: the face the sidebar draws */
		c.fillStyle = color;
		c.beginPath();
		c.arc(x, head_y, r, 0, Math.PI * 2);
		c.fill();
		if (img && img.complete && img.naturalWidth) {
			c.save();
			c.beginPath();
			c.arc(x, head_y, r, 0, Math.PI * 2);
			c.clip();
			c.drawImage(img, x - r, head_y - r, r * 2, r * 2);
			c.restore();
		} else if (faces[s.id] && faces[s.id].initial) {
			c.fillStyle = '#fff';
			c.font = '700 ' + Math.round(11 * u) + 'px ' + ink.sans;
			c.textAlign = 'center';
			c.textBaseline = 'middle';
			c.fillText(faces[s.id].initial, x, head_y + 0.5);
		}
		c.strokeStyle = s.hover ? ink.info : 'rgba(255,255,255,.95)';
		c.lineWidth = s.hover ? 2.5 : 1.5;
		c.beginPath();
		c.arc(x, head_y, r, 0, Math.PI * 2);
		c.stroke();

		/* what they hold, at the hand -- a phone at the ear */
		if (s.prop && !(s.walking && s.prop !== 'folder'))
			draw_prop(c, s.prop, s.prop === 'phone' ? x + 9 * u : x + 10 * u,
			          s.prop === 'phone' ? head_y + 2 * u : y - 16 * u + lift, u);

		/* remote or helper, on the shoulder */
		if (s.kind === 'grok_bot' || s.kind === 'ephemeral' || s.kind === 'external')
			pill(c, s.kind === 'ephemeral' ? 'helper' : 'remote', x + 13 * u,
			     head_y - r - 2, Math.max(8, Math.round(7.5 * u)),
			     s.kind === 'ephemeral' ? ink.good : ink.info, '#fff');

		/* the nameplate, stepped down under any plate it would cover */
		s.plate = pill(c, short_name(s.name), x, plate_y(c, s.name, x, y + 2 * u,
		                                                Math.max(9, Math.round(8.5 * u))),
		               Math.max(9, Math.round(8.5 * u)),
		               s.hover ? ink.ink : 'rgba(20,20,22,.72)',
		               s.hover ? ink.surface : '#fff');
		plates.push(s.plate);
		c.restore();

		s.head = { x: x, y: head_y, r: r };
		s.box = { x: x - 11 * u, y: head_y - r, w: 22 * u, h: y - (head_y - r) + 2 * u + 14 };
	}

	/* -- The usage board -------------------------------------------- */

	/*
	 * Each provider login's allowance on the wall the pack tags
	 * usage_board, fitted to that zone as the lrg scene fits it
	 * (clawt_office_board_layout_world()): rows of words -- title, then
	 * label, meter and figure per window -- when they all fit at a legible
	 * size; else a badge with the provider's initial and a meter for the
	 * login's tightest window, as many as fit with "+N" for the rest; else
	 * one badge in the worst tone.  Nothing is drawn outside the zone.
	 * Every word, level and tone arrives from the server; nothing here
	 * decides one.  No logins, no board.
	 *
	 * The board is a dark panel in either theme, so its tones are the
	 * bright ones the lrg board paints clawt_allowance_level_tone() in,
	 * not the page's foreground tokens, which are dark in the light theme.
	 */
	var board_ink = { good: '#2ec27e', warn: '#e5a50a', bad: '#e01b24', neutral: '#78aeed' };
	var BOARD_MIN_PX = 10;        /* the smallest text the board draws */
	var board_box = null;         /* where the board was drawn, stage pixels */

	function board_logins() {
		return board && Array.isArray(board.logins) ? board.logins : [];
	}

	/* @text cut to @room pixels in the current font, ending in an ellipsis. */
	function fit_text(c, text, room) {
		var lo = 0, hi, mid, best = '';

		text = text || '';
		if (c.measureText(text).width <= room)
			return text;
		if (c.measureText('\u2026').width > room)
			return '';
		hi = text.length - 1;
		best = '\u2026';
		while (lo <= hi) {
			mid = (lo + hi) >> 1;
			if (c.measureText(text.slice(0, mid).replace(/\s+$/, '') + '\u2026').width <= room) {
				best = text.slice(0, mid).replace(/\s+$/, '') + '\u2026';
				lo = mid + 1;
			} else {
				hi = mid - 1;
			}
		}
		return best;
	}

	function board_meter(c, x, y, w, h, w_obj) {
		c.fillStyle = '#3c404c';
		c.fillRect(x, y, w, h);
		if (w_obj && typeof w_obj.remaining === 'number' && w_obj.kind !== 'balance') {
			c.fillStyle = board_ink[w_obj.tone] || board_ink.neutral;
			c.fillRect(x, y, w * Math.max(0, Math.min(1, w_obj.remaining / 100)), h);
		}
	}

	/* Rows of words, or false when they do not fit the zone at @px. */
	function board_rows(c, logins, z, px) {
		var pad = Math.max(2, Math.floor(px * 0.4)), line = Math.floor(px * 1.4);
		var gap = pad, inner = z.w - 2 * pad, lines = 0, value_w = 0, bar_w, label_w, y;

		c.font = '500 ' + px + 'px ' + ink.sans;
		logins.forEach(function (l) {
			lines += 1 + Math.max(1, (l.windows || []).length);
			(l.windows || []).forEach(function (w) {
				value_w = Math.max(value_w, c.measureText(w.value || '').width);
			});
		});
		if (pad * 2 + lines * line > z.h)
			return false;
		value_w = Math.min(Math.ceil(value_w), Math.floor(inner * 0.4));
		bar_w = Math.floor(Math.max(inner * 0.2, px));
		label_w = inner - value_w - bar_w - 2 * gap;
		if (label_w < px * 2.5)
			return false;

		c.textBaseline = 'middle';
		y = z.y + pad;
		logins.forEach(function (l) {
			c.font = '700 ' + px + 'px ' + ink.sans;
			c.fillStyle = '#fff';
			c.fillText(fit_text(c, l.title, inner), z.x + pad, y + line / 2);
			y += line;
			c.font = '500 ' + px + 'px ' + ink.sans;
			if (!l.windows || l.windows.length === 0) {
				c.fillStyle = '#969cac';
				c.fillText(fit_text(c, l.status, inner), z.x + pad, y + line / 2);
				y += line;
				return;
			}
			l.windows.forEach(function (w) {
				var bx = z.x + pad + label_w + gap, bh = Math.max(2, Math.floor(px * 0.6));

				c.fillStyle = '#bec3d2';
				c.fillText(fit_text(c, w.label, label_w), z.x + pad, y + line / 2);
				if (w.kind !== 'balance')
					board_meter(c, bx, y + (line - bh) / 2, bar_w, bh, w);
				c.fillStyle = '#fff';
				c.fillText(fit_text(c, w.value, value_w), bx + bar_w + gap, y + line / 2);
				y += line;
			});
		});
		return true;
	}

	/* The largest badge for @cells gauges in the zone, and its grid. */
	function board_grid(cells, z, pad, cap) {
		var best = { side: -1 }, cols, rows, cw, ch, side;

		for (cols = 1; cols <= cells; cols++) {
			rows = Math.ceil(cells / cols);
			cw = (z.w - pad * (cols + 1)) / cols;
			ch = (z.h - pad * (rows + 1)) / rows;
			side = Math.floor(Math.min(ch, cw / 2.2, cap));
			if (side > best.side)
				best = { side: side, cols: cols, cw: cw, ch: ch };
		}
		return best;
	}

	/* A gauge per login, or false when not even one fits. */
	function board_gauges(c, logins, z) {
		var least = BOARD_MIN_PX + 4, pad = Math.max(2, Math.floor(Math.min(z.w, z.h) * 0.05));
		var n = logins.length, shown, cells, g, i;

		for (shown = n; ; shown--) {
			cells = shown === n ? n : shown + 1;
			g = board_grid(cells, z, pad, 40);
			if (g.side >= least || shown === 0)
				break;
		}
		if (g.side < least || shown === 0)
			return false;
		c.textBaseline = 'middle';
		c.textAlign = 'center';
		for (i = 0; i < cells; i++) {
			var cx = z.x + pad + (i % g.cols) * (g.cw + pad);
			var cy = z.y + pad + Math.floor(i / g.cols) * (g.ch + pad);
			var by = Math.floor(cy + (g.ch - g.side) / 2);
			var l = logins[i], tight, letter, bh, bx;

			if (i === shown) {
				c.font = '600 ' + BOARD_MIN_PX + 'px ' + ink.sans;
				c.fillStyle = '#969cac';
				c.fillText(fit_text(c, '+' + (n - shown), g.cw), cx + g.cw / 2, cy + g.ch / 2);
				continue;
			}
			tight = l.tightest >= 0 && l.windows ? l.windows[l.tightest] : null;
			c.fillStyle = board_ink[tight ? tight.tone : l.tone] || board_ink.neutral;
			c.fillRect(cx, by, g.side, g.side);
			letter = g.side >= 2 * BOARD_MIN_PX + 4 ? 2 * BOARD_MIN_PX : BOARD_MIN_PX;
			c.font = '800 ' + letter + 'px ' + ink.sans;
			c.fillStyle = '#12141a';
			c.fillText(fit_text(c, l.initial || '?', g.side), cx + g.side / 2, by + g.side / 2);
			bh = Math.max(3, Math.floor(g.side * 0.4));
			bx = cx + g.side + Math.max(2, Math.floor(g.side * 0.25));
			board_meter(c, bx, by + (g.side - bh) / 2, cx + g.cw - bx, bh, tight);
		}
		c.textAlign = 'left';
		return true;
	}

	function draw_board(c) {
		var logins = board_logins();
		var a, b, z, rank = { bad: 3, warn: 2, good: 1 }, worst = 'neutral', side;

		board_box = null;
		if (!board_rect || logins.length === 0 || usage_place !== 'world')
			return;
		/* the zone's corners on the stage: its box, flat or tilted */
		a = project(board_rect.x, board_rect.y);
		b = project(board_rect.x + board_rect.w, board_rect.y + board_rect.h);
		z = { x: Math.floor(Math.min(a.x, b.x)), y: Math.floor(Math.min(a.y, b.y)),
		      w: Math.floor(Math.abs(b.x - a.x)), h: Math.floor(Math.abs(b.y - a.y)) };
		if (z.w < 2 || z.h < 2)
			return;
		board_box = z;

		c.save();
		c.beginPath();
		c.rect(z.x, z.y, z.w, z.h);
		c.clip();
		c.fillStyle = 'rgba(18,20,26,.9)';
		c.fillRect(z.x, z.y, z.w, z.h);
		c.strokeStyle = '#464c5c';
		c.strokeRect(z.x + 0.5, z.y + 0.5, z.w - 1, z.h - 1);
		if (!board_rows(c, logins, z, 2 * BOARD_MIN_PX) &&
		    !board_rows(c, logins, z, BOARD_MIN_PX) &&
		    !board_gauges(c, logins, z)) {
			/* too small for a gauge: one mark in the worst tone */
			logins.forEach(function (l) {
				if ((rank[l.tone] || 0) > (rank[worst] || 0))
					worst = l.tone;
			});
			side = Math.min(z.w, z.h);
			c.fillStyle = board_ink[worst] || board_ink.neutral;
			c.fillRect(z.x + (z.w - side) / 2, z.y + (z.h - side) / 2, side, side);
		}
		c.restore();
	}

	/* -- The usage details panel ------------------------------------ */

	/*
	 * Every login and every window, in a table: the title and status, then
	 * label, meter (none for a balance), figure and reset.  Beside the
	 * board when it was hovered or clicked, docked in the stage's top
	 * right corner when usage is "in the corner".  Kept inside the stage:
	 * it scrolls rather than leaving it, and its cells end long words in
	 * an ellipsis (CSS) rather than overflowing.
	 */
	var panel = document.createElement('div');
	var panel_pinned = false;     /* opened by a click: stays until Escape or a click away */
	var panel_quiet = false;      /* closed while hovered: not again until the pointer leaves */
	var board_hover = false;

	panel.className = 'office-usage-panel';
	panel.hidden = true;
	panel.setAttribute('role', 'dialog');
	panel.setAttribute('aria-label', 'Provider usage');
	stage.appendChild(panel);

	function fill_panel() {
		var logins = board_logins();
		var h, table;

		panel.textContent = '';
		h = panel.appendChild(document.createElement('h3'));
		h.textContent = 'Provider usage';
		table = panel.appendChild(document.createElement('table'));
		logins.forEach(function (l) {
			var tr = table.appendChild(document.createElement('tr'));
			var th = tr.appendChild(document.createElement('th'));

			th.colSpan = 4;
			th.textContent = l.title || '';
			tr = table.appendChild(document.createElement('tr'));
			tr.className = 'status';
			th = tr.appendChild(document.createElement('td'));
			th.colSpan = 4;
			th.textContent = l.status || '';
			(l.windows || []).forEach(function (w) {
				var row = table.appendChild(document.createElement('tr'));
				var cell, bar, fill;

				cell = row.appendChild(document.createElement('td'));
				cell.textContent = w.label || '';
				cell.title = w.label || '';
				cell = row.appendChild(document.createElement('td'));
				if (w.kind !== 'balance') {
					bar = cell.appendChild(document.createElement('span'));
					bar.className = 'bar';
					if (typeof w.remaining === 'number') {
						fill = bar.appendChild(document.createElement('span'));
						fill.style.width = Math.max(0, Math.min(100, w.remaining)) + '%';
						fill.style.background = board_ink[w.tone] || board_ink.neutral;
					}
				}
				cell = row.appendChild(document.createElement('td'));
				cell.className = 'value';
				cell.textContent = w.value || '';
				cell = row.appendChild(document.createElement('td'));
				cell.className = 'reset';
				cell.textContent = w.reset || '';
			});
		});
	}

	function panel_open() {
		if (usage_place === 'off' || board_logins().length === 0)
			return false;
		if (usage_place === 'corner')
			return true;
		return !!board_box && (panel_pinned || (!panel_quiet && board_hover));
	}

	/* Shown or hidden, and placed: beside the board, or in the corner, inside the stage. */
	function place_panel() {
		var open = panel_open(), m = 8, w, h, x, y;

		if (panel.hidden !== !open)
			panel.hidden = !open;
		if (!open)
			return;
		panel.style.maxWidth = Math.max(120, view.W - 2 * m) + 'px';
		panel.style.maxHeight = Math.max(60, view.H - 2 * m) + 'px';
		w = panel.offsetWidth;
		h = panel.offsetHeight;
		if (usage_place === 'corner') {
			x = view.W - m - w;
			y = m;
		} else {
			x = board_box.x + board_box.w + m;
			if (x + w > view.W - m)
				x = board_box.x - m - w;
			if (x < m)
				x = board_box.x + (board_box.w - w) / 2;
			y = board_box.y;
		}
		x = Math.max(m, Math.min(x, view.W - m - w));
		y = Math.max(m, Math.min(y, view.H - m - h));
		panel.style.left = Math.round(x) + 'px';
		panel.style.top = Math.round(y) + 'px';
	}

	function on_board(ev) {
		var r = actor_cv.getBoundingClientRect();
		var x = ev.clientX - r.left, y = ev.clientY - r.top;

		return !!board_box && x >= board_box.x && y >= board_box.y &&
		       x < board_box.x + board_box.w && y < board_box.y + board_box.h;
	}

	panel.addEventListener('mouseleave', function () {
		panel_quiet = false;
		place_panel();
	});
	document.addEventListener('keydown', function (ev) {
		if (ev.key !== 'Escape' || usage_place !== 'world' || panel.hidden)
			return;
		panel_pinned = false;
		panel_quiet = true;
		place_panel();
	});
	/* a click anywhere away from a pinned panel and its board closes it */
	document.addEventListener('click', function (ev) {
		if (!panel_pinned || panel.contains(ev.target) || (ev.target === actor_cv && on_board(ev)))
			return;
		panel_pinned = false;
		place_panel();
	}, true);

	/* -- One frame -------------------------------------------------- */

	var hover_id = null;
	var drawn = [];

	function build_frame(now) {
		var sprites = [];
		var by_id = {};
		var meetings = {};

		Object.keys(actors).forEach(function (id) {
			var a = actors[id];
			var w = where(a, now);
			var p = a.p || {};
			var who = people[id] || {};
			var alpha = Math.min(1, (now - a.born) / 400);
			var pr, s;

			if (a.gone) {
				alpha = Math.max(0, 1 - (now - a.gone) / 500);
				if (alpha <= 0) {
					delete actors[id];
					return;
				}
			}
			pr = project(w.x, w.y);
			s = { id: id, mx: w.x, my: w.y, x: pr.x, y: pr.y, u: pr.s * view.boost,
			      walking: w.walking, dx: w.dx, dy: w.dy, alpha: alpha,
			      activity: p.activity || who.activity, pose: p.pose, prop: p.prop,
			      emote: p.emote, facing: p.facing, zone: p.zone,
			      name: who.name || id, kind: who.kind || 'agent',
			      detail: p.sim_detail !== undefined ? p.sim_detail : who.detail,
			      since: p.sim_since || who.since || 0,
			      peer: p.peer || who.peer, hover: id === hover_id,
			      visiting_from: who.visiting_from };
			sprites.push(s);
			by_id[id] = s;
			if (s.activity === 'in_meeting' && !w.walking) {
				var key = p.zone || 'meeting';

				(meetings[key] = meetings[key] || []).push(s);
			}
		});
		sprites.sort(function (a, b) { return a.y - b.y; });

		return {
			sprites: sprites, by_id: by_id,
			meetings: Object.keys(meetings).map(function (k) {
				var group = meetings[k];
				var cx = 0, cy = 0, r = 0;

				group.forEach(function (s) { cx += s.mx; cy += s.my; });
				cx /= group.length;
				cy /= group.length;
				group.forEach(function (s) { r = Math.max(r, Math.hypot(s.mx - cx, s.my - cy)); });
				return { zone: k, x: cx, y: cy, r: r + 34, n: group.length, group: group };
			})
		};
	}

	function draw_actors(now, frame) {
		var dpr = window.devicePixelRatio || 1;
		var c = actor_ctx;
		var talkers;

		c.setTransform(1, 0, 0, 1, 0, 0);
		c.clearRect(0, 0, actor_cv.width, actor_cv.height);
		c.setTransform(dpr, 0, 0, dpr, 0, 0);

		/* a call: a dashed line between the two, running while they talk */
		frame.sprites.forEach(function (s) {
			var o = s.facing && frame.by_id[s.facing];
			var ax, ay, bx, by, mx, my;

			/* once per pair, and only once both have reached the phone */
			if (s.activity !== 'on_call' || !o || s.walking || o.walking ||
			    (o.id < s.id && o.activity === 'on_call' && o.facing === s.id))
				return;
			ax = s.x;
			ay = s.y - 34 * s.u;
			bx = o.x;
			by = o.y - 34 * o.u;
			mx = (ax + bx) / 2;
			my = Math.min(ay, by) - 30 - Math.hypot(bx - ax, by - ay) * 0.18;
			c.save();
			c.strokeStyle = ink.info;
			c.globalAlpha = 0.85 * Math.min(s.alpha, o.alpha);
			c.lineWidth = 2;
			c.setLineDash([6, 5]);
			c.lineDashOffset = reduced ? 0 : -now / 40;
			c.beginPath();
			c.moveTo(ax, ay);
			c.quadraticCurveTo(mx, my, bx, by);
			c.stroke();
			c.restore();
		});

		draw_board(c);
		place_panel();
		plates = [];
		bubbles = [];
		frame.sprites.forEach(function (s) { draw_person(c, s, now); });

		/*
		 * A meeting's name, under the nameplates round its table: above
		 * them is where the speech bubbles go.
		 */
		frame.meetings.forEach(function (m) {
			var pr = project(m.x, m.y);
			var low = Math.max.apply(null, m.group.map(function (s) {
				return s.plate ? s.plate.y + s.plate.h : s.y;
			}));

			pill(actor_ctx, zone_name(m.zone) + ' \u00b7 ' + m.n, pr.x, low + 3,
			     10, 'rgba(255,214,120,.95)', '#3b2a00');
		});
		if (frame.sprites.length === 0)
			pill(c, Object.keys(people).length
			        ? 'Nobody is in: no agent is running'
			        : 'Nobody works here yet',
			     view.W / 2, view.H / 2 - 12, 13, 'rgba(20,20,22,.72)', '#fff');

		/* a handover: the folder walks from one to the other, again and again */
		frame.sprites.forEach(function (s) {
			var o = s.activity === 'delivering' && s.peer && frame.by_id[s.peer];
			var t, x, y;

			if (!o || o === s)
				return;
			t = reduced ? 0.5 : (now % 1600) / 1600;
			x = s.x + (o.x - s.x) * t;
			y = s.y + (o.y - s.y) * t - 20 * s.u - Math.sin(t * Math.PI) * 26;
			c.globalAlpha = Math.sin(t * Math.PI) * 0.9 + 0.1;
			draw_prop(c, 'folder', x, y, s.u * 1.1);
			c.globalAlpha = 1;
		});

		/* what floats overhead, then what is being said: above everybody */
		frame.sprites.forEach(function (s) {
			if (s.emote && s.head)
				draw_emote(c, s.emote, s.x, s.head.y - s.head.r - 13 * s.u, s.u, now);
		});
		talkers = frame.sprites.filter(function (s) {
			return s.detail && s.head && s.activity !== 'idle' && s.activity !== 'offline' &&
			       !s.walking;
		}).sort(function (a, b) { return (b.since || 0) - (a.since || 0); }).slice(0, 4);
		talkers.forEach(function (s) {
			bubble(c, trim_text(s.detail, 34), s.x,
			       s.head.y - s.head.r - (s.emote ? 30 : 6) * s.u, s.u);
		});
		drawn = frame.sprites;
		if (hover_id && pointer && frame.by_id[hover_id] && !tip.hidden)
			show_tip(frame.by_id[hover_id]);
	}

	/* -- The loop --------------------------------------------------- */

	var last_paint = 0;
	var tilt_from = tilt, tilt_t0 = 0;

	function busy(frame, now) {
		return frame.sprites.some(function (s) {
			var ch = characters[s.id];

			if (ch && character_animates(s, pick_animation(ch.def.animations, s)))
				return true;
			return s.walking || s.alpha < 1 || s.emote === 'question' ||
			       s.emote === 'thought' || s.activity === 'on_call' ||
			       s.activity === 'delivering' || s.activity === 'waiting_on_you';
		}) || Object.keys(actors).some(function (id) { return actors[id].gone; }) ||
		   now - tilt_t0 < 700;
	}

	function tick(now) {
		var frame, target = want_tilt ? 1 : 0;

		if (tilt !== target) {
			var t = reduced ? 1 : Math.min(1, (now - tilt_t0) / 650);

			tilt = tilt_from + (target - tilt_from) * ease(t);
			if (t >= 1)
				tilt = target;
			layout_stage();
		}
		frame = build_frame(now);
		/*
		 * Draw at the display's rate while anything moves, and a few
		 * times a second otherwise: an office nobody is in should not
		 * cost a core.
		 */
		if (busy(frame, now) || now - last_paint > 250) {
			draw_floor(now, frame);
			draw_actors(now, frame);
			last_paint = now;
		}
		window.requestAnimationFrame(tick);
	}

	/* -- Pointer: who is this, and their chat on a click ------------ */

	function hit(ev) {
		var r = actor_cv.getBoundingClientRect();
		var x = ev.clientX - r.left, y = ev.clientY - r.top;
		var i, s;

		for (i = drawn.length - 1; i >= 0; i--) {
			s = drawn[i];
			if (!s.box)
				continue;
			if ((x >= s.box.x && x <= s.box.x + s.box.w && y >= s.box.y &&
			     y <= s.box.y + s.box.h) ||
			    (s.plate && x >= s.plate.x && x <= s.plate.x + s.plate.w &&
			     y >= s.plate.y && y <= s.plate.y + s.plate.h))
				return { s: s, x: x, y: y };
		}
		return null;
	}

	function clickable(s) {
		return s.kind === 'agent' || s.kind === 'ephemeral';
	}

	var pointer = null;   /* where the pointer is over the stage, for the tip */
	var tip_key = '';

	/*
	 * The tooltip for @s at the pointer.  Called on every pointer move and
	 * again on every frame while it shows, so it says what the person is
	 * doing now rather than what they were doing when the pointer stopped.
	 */
	function show_tip(s) {
		var word = words[s.activity];
		var line, key;

		line = simulate ? (s.detail || '') : (lines[s.id] || s.detail || '');
		key = [s.id, s.activity, line, Math.round(pointer.x), Math.round(pointer.y)].join('|');
		if (key === tip_key && !tip.hidden)
			return;
		tip_key = key;
		tip.textContent = '';
		/* a visitor from another fleet says which, where there is room for it */
		tip.appendChild(document.createElement('strong')).textContent =
			s.visiting_from ? s.name + ' \u00b7 ' + s.visiting_from : s.name;
		tip.appendChild(document.createElement('span')).textContent =
			word ? word.label : (s.activity || '');
		if (line)
			tip.appendChild(document.createElement('em')).textContent = line;
		if (clickable(s))
			tip.appendChild(document.createElement('small')).textContent = 'Click to open the chat';
		tip.hidden = false;
		tip.style.left = Math.min(view.W - tip.offsetWidth - 6, pointer.x + 14) + 'px';
		tip.style.top = Math.max(4, pointer.y - tip.offsetHeight - 10) + 'px';
	}

	actor_cv.addEventListener('mousemove', function (ev) {
		var h = hit(ev);
		var was = board_hover;

		board_hover = on_board(ev);
		if (!board_hover && was)
			panel_quiet = false;
		if (board_hover !== was)
			place_panel();
		if (board_hover && !h) {
			actor_cv.style.cursor = 'pointer';
			tip.hidden = true;
			return;
		}

		hover_id = h ? h.s.id : null;
		actor_cv.style.cursor = h && clickable(h.s) ? 'pointer' : '';
		if (!h) {
			tip.hidden = true;
			return;
		}
		pointer = { x: h.x, y: h.y };
		show_tip(h.s);
		last_paint = 0;
	});
	actor_cv.addEventListener('mouseleave', function (ev) {
		/* onto the panel it opened is not away from the board */
		board_hover = false;
		if (!(ev.relatedTarget && panel.contains(ev.relatedTarget))) {
			panel_quiet = false;
			place_panel();
		}
		hover_id = null;
		tip.hidden = true;
		last_paint = 0;
	});
	actor_cv.addEventListener('click', function (ev) {
		var h = hit(ev);

		/* the board pins its panel open, and a second click closes it */
		if (on_board(ev) && usage_place === 'world') {
			panel_pinned = !panel_pinned;
			panel_quiet = !panel_pinned;
			place_panel();
			return;
		}
		if (h && clickable(h.s))
			window.location.href = '/a/' + encodeURIComponent(h.s.id) + '/chat';
	});

	/* -- Toggles ---------------------------------------------------- */

	function sync_buttons() {
		if (tilt_btn)
			tilt_btn.setAttribute('aria-pressed', want_tilt ? 'true' : 'false');
		if (rooms_btn)
			rooms_btn.setAttribute('aria-pressed', show_rooms ? 'true' : 'false');
		if (usage_btn) {
			usage_btn.textContent = 'Usage: ' + USAGE_WORDS[usage_place];
			usage_btn.setAttribute('aria-pressed', usage_place === 'off' ? 'false' : 'true');
		}
	}
	if (tilt_btn)
		tilt_btn.addEventListener('click', function () {
			want_tilt = !want_tilt;
			tilt_from = tilt;
			tilt_t0 = performance.now();
			pref_set('tilt', want_tilt);
			sync_buttons();
		});
	if (rooms_btn)
		rooms_btn.addEventListener('click', function () {
			show_rooms = !show_rooms;
			pref_set('rooms', show_rooms);
			sync_buttons();
			last_paint = 0;
		});
	if (usage_btn)
		usage_btn.addEventListener('click', function (ev) {
			usage_place = USAGE_PLACES[(USAGE_PLACES.indexOf(usage_place) + 1) %
			                           USAGE_PLACES.length];
			try {
				window.localStorage.setItem('clawt-office-usage', usage_place);
			} catch (e) { /* private mode: the button still works this visit */ }
			panel_pinned = false;
			ev.stopPropagation();
			sync_buttons();
			last_paint = 0;
		});

	/* -- Daylight: the hour on this machine, as a tint -------------- */

	function daylight() {
		var h = new Date().getHours() + new Date().getMinutes() / 60;
		var tint;

		if (h < 6 || h >= 21)
			tint = 'rgba(18,28,72,.30)';
		else if (h < 9)
			tint = 'rgba(255,190,120,.14)';
		else if (h < 17)
			tint = 'rgba(255,248,230,.04)';
		else
			tint = 'rgba(255,140,90,.16)';
		if (light_el)
			light_el.style.setProperty('--office-tint', tint);
	}

	/* -- Polling, while the tab is visible -------------------------- */

	var poll_timer = 0;

	function poll() {
		window.clearTimeout(poll_timer);
		if (document.hidden)
			return;
		fetch(scene.layout_url || '/office/layout.json',
		      { cache: 'no-store', credentials: 'same-origin' })
			.then(function (r) { return r.ok ? r.json() : null; })
			.then(function (body) {
				var layout;

				if (!body)
					return;
				if (body.enabled === false) {
					/* turned off under us: the page will say so on reload */
					root.hidden = true;
					return 'stop';
				}
				layout = take_layout(body, performance.now());
				read_tokens();
				daylight();
				if (layout.world && layout.world !== world.id)
					return fetch((scene.world_url || '/office/world.json') + '?world=' +
					             encodeURIComponent(layout.world),
					             { cache: 'no-store', credentials: 'same-origin' })
						.then(function (r) { return r.ok ? r.json() : null; })
						.then(function (w) {
							if (!w || !w.world)
								return;
							world = w.world;
							map = w.map || null;
							if (title_el)
								title_el.textContent = world.name || world.id || '';
							load_world();
							if (simulate)
								sim_tick();
						});
			})
			.catch(function () { /* the daemon or this server is away; next poll */ })
			.then(function (stop) {
				if (stop !== 'stop')
					poll_timer = window.setTimeout(poll, POLL_MS);
			});
	}

	document.addEventListener('visibilitychange', function () {
		if (!document.hidden)
			poll();
	});

	/* -- Simulation: invented here, for a look at a busy office ----- */

	var SIM_DETAIL = {
		library: ['reading docs/office.org', 'searching the issue tracker', 'recalling last week'],
		lab: ['make test', 'running the integration suite', 'bisecting a flaky test'],
		whiteboard: ['planning the next release', 'splitting the task in three'],
		terminal: ['driving the browser', 'checking the staging site'],
		desk: ['writing web-office-view.c', 'editing the changelog', 'drafting a reply']
	};
	var SIM_QUESTIONS = ['May I merge this branch?', 'Which world should we use?',
	                     'OK to restart the lab VM?'];

	function pick(list) {
		return list[Math.floor(Math.random() * list.length)];
	}

	function shuffle(list) {
		var i, j, t;

		for (i = list.length - 1; i > 0; i--) {
			j = Math.floor(Math.random() * (i + 1));
			t = list[i];
			list[i] = list[j];
			list[j] = t;
		}
		return list;
	}

	/* Where each person's own desk is, the way the director gives homes. */
	function sim_homes(ids) {
		var homes = {}, used = {};

		function seat(tag, team) {
			var zones = zones_tagged(tag), i, z, j, key;

			zones.sort(function (a, b) {
				return (b.team === team ? 1 : 0) - (a.team === team ? 1 : 0);
			});
			for (i = 0; i < zones.length; i++) {
				z = zones[i];
				for (j = 0; j < z.anchors_2d.length; j++) {
					key = z.id + '/' + j;
					if (!used[key]) {
						used[key] = true;
						return { x: z.anchors_2d[j][0], y: z.anchors_2d[j][1], zone: z.id };
					}
				}
			}
			return null;
		}
		ids.slice().sort().forEach(function (id) {
			var p = people[id] || {};
			var tag = p.kind === 'grok_bot' || p.kind === 'external' ? 'visitor'
				: p.role === 'chief' ? 'chief_office'
				: p.role === 'lead' ? 'lead_office' : 'desk';

			homes[id] = seat(tag, p.team) || seat('desk', p.team) || seat('lounge');
		});
		return homes;
	}

	function spots(tag, n) {
		var out = [];

		zones_tagged(tag).forEach(function (z) {
			z.anchors_2d.forEach(function (a) { out.push({ x: a[0], y: a[1], zone: z.id }); });
		});
		return out.slice(0, n === undefined ? out.length : n);
	}

	function sim_tick() {
		var now = performance.now();
		/* a stopped agent too: it is all invented, and a stopped fleet is
		 * exactly the one somebody wants to see busy */
		var ids = Object.keys(people);
		var homes = sim_homes(ids);
		var pool = shuffle(ids.slice());
		var placements = [];
		var stamp = Date.now() / 1000;

		function place(id, at, activity, extra) {
			var p, k;

			if (!at)
				return;
			p = { id: id, activity: activity, x: at.x, y: at.y, zone: at.zone,
			      sim_since: stamp + Math.random() };
			for (k in extra)
				if (Object.prototype.hasOwnProperty.call(extra, k))
					p[k] = extra[k];
			placements.push(p);
		}

		/* a call between two, facing each other */
		if (pool.length >= 2) {
			var booth = spots('phone_booth');
			var a = pool.pop(), b = pool.pop();

			place(a, booth[0] || homes[a], 'on_call',
			      { pose: 'phone', prop: 'phone', facing: b, peer: b,
			        sim_detail: 'on a call with ' + ((people[b] || {}).name || b) });
			place(b, booth[1] || homes[b], 'on_call',
			      { pose: 'phone', prop: 'phone', facing: a, peer: a });
		}

		/* a meeting round the smallest table that fits */
		if (pool.length >= 3) {
			var size = Math.min(pool.length, 3 + Math.floor(Math.random() * 2));
			var rooms_ok = zones_tagged('meeting').filter(function (z) {
				return z.anchors_2d.length >= size;
			}).sort(function (x, y) { return x.anchors_2d.length - y.anchors_2d.length; });
			var room = rooms_ok[0];
			var i;

			if (room)
				for (i = 0; i < size; i++)
					place(pool.pop(), { x: room.anchors_2d[i][0], y: room.anchors_2d[i][1],
					                    zone: room.id }, 'in_meeting',
					      { pose: 'sit', sim_detail: i === 0 ? 'walking the team through the plan' : undefined });
		}

		/* somebody waiting on you -- the loudest thing on the floor */
		if (pool.length >= 1) {
			var w = pool.pop();

			place(w, homes[w], 'waiting_on_you',
			      { pose: 'stand', emote: 'question', sim_detail: pick(SIM_QUESTIONS) });
		}

		/* a handover to somebody's desk */
		if (pool.length >= 2) {
			var from = pool.pop(), to = pool[pool.length - 1];
			var desk = homes[to];

			place(from, desk && { x: desk.x + 26, y: desk.y + 6, zone: desk.zone }, 'delivering',
			      { pose: 'walk', prop: 'folder', peer: to,
			        sim_detail: 'handing ' + ((people[to] || {}).name || to) + ' the review' });
		}

		/* now and then, a broken turn */
		if (pool.length >= 1 && Math.random() < 0.5 && spots('bug_corner').length) {
			var e = pool.pop();

			place(e, pick(spots('bug_corner')), 'error',
			      { pose: 'stand', emote: 'alert', sim_detail: 'the turn timed out' });
		}

		/* everybody else: working somewhere, thinking, or resting */
		pool.forEach(function (id) {
			var roll = Math.random();
			var stations = ['library', 'lab', 'whiteboard', 'terminal', 'desk'];
			var station, at, props = { library: 'book', lab: 'terminal', whiteboard: 'marker',
			                           terminal: 'screen', desk: 'laptop' };

			if (roll < 0.55) {
				station = pick(stations);
				at = station === 'desk' ? homes[id] : (pick(spots(station)) || homes[id]);
				place(id, at, 'working',
				      { pose: station === 'whiteboard' ? 'present' : 'type',
				        prop: props[station], sim_detail: pick(SIM_DETAIL[station]) });
			} else if (roll < 0.75) {
				place(id, homes[id], 'thinking', { pose: 'sit', emote: 'thought' });
			} else {
				place(id, pick(spots('lounge')) || homes[id], 'idle',
				      { pose: 'sit', prop: Math.random() < 0.4 ? 'coffee' : undefined });
			}
		});

		/* two people never share a spot: the second stands beside */
		var taken = {};

		placements.forEach(function (p) {
			var key = Math.round(p.x) + ',' + Math.round(p.y);

			while (taken[key]) {
				p.x += 30;
				key = Math.round(p.x) + ',' + Math.round(p.y);
			}
			taken[key] = true;
		});
		apply_placements(placements, now);
	}

	/* -- Start ------------------------------------------------------ */

	read_tokens();
	daylight();
	sync_buttons();
	take_layout(scene, performance.now());
	load_world();
	window.addEventListener('resize', function () {
		layout_stage();
		last_paint = 0;
	});
	if (window.matchMedia)
		window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function () {
			read_tokens();
			last_paint = 0;
		});
	if (simulate) {
		if (sim_label)
			sim_label.hidden = false;
		root.classList.add('office-simulated');
		sim_tick();
		window.setInterval(function () {
			if (!document.hidden)
				sim_tick();
		}, SIM_MS);
	}
	poll_timer = window.setTimeout(poll, POLL_MS);
	window.requestAnimationFrame(tick);
})();
