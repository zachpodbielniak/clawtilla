/*
 * office-js-harness.js - runs data/web/office.js with stubbed browser globals
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Driven by tests/test-office-js.sh:  node office-js-harness.js <office.js>
 *
 * There is no browser here.  A document, a window, a fetch and a canvas
 * 2D context are stubbed just far enough for the script to evaluate and
 * run its animation frames; every fillText() is recorded.  The scene
 * carries a usage board in the shape clawt_office_board_to_json() makes
 * (the keys tests/test-office-board.c checks) and a pack zone tagged
 * usage_board, so the board-drawing path runs.
 */

'use strict';

var fs = require('fs');
var vm = require('vm');

var texts = [];
var rafs = [];
var failures = [];

function check(ok, what) {
	if (!ok)
		failures.push(what);
}

/* Any property read on a stub element is another stub, or a harmless value. */
function make_element(tag) {
	var el = {
		tagName: String(tag || 'div').toUpperCase(),
		style: { setProperty: function () {} },
		classList: { add: function () {}, remove: function () {}, toggle: function () {},
		             contains: function () { return false; } },
		children: [],
		hidden: false,
		textContent: '',
		clientWidth: 800,
		clientHeight: 600,
		width: 800,
		height: 600,
		dataset: {},
		appendChild: function (child) { el.children.push(child); return child; },
		removeChild: function () {},
		setAttribute: function () {},
		getAttribute: function () { return null; },
		addEventListener: function () {},
		removeEventListener: function () {},
		querySelector: function () { return null; },
		querySelectorAll: function () { return []; },
		getBoundingClientRect: function () {
			return { left: 0, top: 0, right: 800, bottom: 600, width: 800, height: 600 };
		},
		getContext: function () { return make_context(); }
	};

	return el;
}

function make_context() {
	var ctx = {
		fillText: function (text) { texts.push(String(text)); },
		measureText: function (text) { return { width: String(text).length * 6 }; },
		createLinearGradient: function () { return { addColorStop: function () {} }; },
		createRadialGradient: function () { return { addColorStop: function () {} }; },
		getImageData: function () { return { data: [] }; },
		canvas: { width: 800, height: 600 }
	};

	['save', 'restore', 'beginPath', 'closePath', 'rect', 'clip', 'fillRect',
	 'strokeRect', 'clearRect', 'moveTo', 'lineTo', 'arc', 'ellipse', 'fill',
	 'stroke', 'translate', 'scale', 'rotate', 'setTransform', 'setLineDash',
	 'drawImage', 'quadraticCurveTo', 'bezierCurveTo', 'strokeText', 'roundRect',
	 'putImageData', 'arcTo'].forEach(function (name) {
		ctx[name] = function () {};
	});
	return ctx;
}

var board = {
	logins: [
		{ title: 'claude-code: alpha', initial: 'C', status: 'ok', tone: 'warn', tightest: 1,
		  windows: [
			{ label: 'Session (5h)', value: '62% left', reset: 'Tue 9am', tone: 'good',
			  kind: 'meter', remaining: 62 },
			{ label: 'Weekly', value: '21% left', reset: 'Sun 18:00', tone: 'warn',
			  kind: 'meter', remaining: 21 }
		  ] },
		{ title: 'codex: beta', initial: 'X', status: 'ok', tone: 'good', tightest: -1,
		  windows: [
			{ label: 'Credits', value: '$12.40', tone: 'good', kind: 'balance' }
		  ] }
	]
};

var scene = {
	enabled: true,
	world: {
		id: 'harness',
		name: 'Harness',
		zones: [
			{ id: 'wall', tags: ['usage_board'], anchors_2d: [[320, 120]],
			  rect_2d: [160, 40, 320, 160] }
		]
	},
	map: null,
	words: {},
	sprites: [],
	board: board
};

var root = make_element('div');
var scene_el = make_element('script');
var listeners = {};

scene_el.textContent = JSON.stringify(scene);

var stage = make_element('div');
var floor = make_element('canvas');
var actors = make_element('canvas');

root.querySelector = function (sel) {
	if (sel === '.office-stage')
		return stage;
	if (sel === '.office-floor')
		return floor;
	if (sel === '.office-actors')
		return actors;
	return null;
};

var doc = make_element('html');

doc.documentElement = make_element('html');
doc.hidden = false;
doc.body = make_element('body');
doc.getElementById = function (id) {
	if (id === 'office-view')
		return root;
	if (id === 'office-scene')
		return scene_el;
	return null;
};
doc.createElement = make_element;

var win = {
	innerWidth: 800,
	innerHeight: 600,
	devicePixelRatio: 1,
	location: { search: '', href: '' },
	localStorage: { getItem: function () { return null; }, setItem: function () {} },
	matchMedia: function () {
		return { matches: false, addEventListener: function () {} };
	},
	addEventListener: function (type, fn) {
		if (!listeners[type])
			listeners[type] = [];
		listeners[type].push(fn);
	},
	requestAnimationFrame: function (fn) { rafs.push(fn); return rafs.length; },
	setTimeout: function () { return 1; },
	setInterval: function () { return 1; },
	clearTimeout: function () {}
};

var sandbox = {
	document: doc,
	window: win,
	location: win.location,
	performance: { now: function () { return 0; } },
	getComputedStyle: function () { return { getPropertyValue: function () { return ''; } }; },
	fetch: function () { return new Promise(function () {}); },
	Image: function () { return make_element('img'); },
	console: console,
	Math: Math, JSON: JSON, Array: Array, Object: Object, String: String,
	Number: Number, Date: Date, RegExp: RegExp, Promise: Promise,
	encodeURIComponent: encodeURIComponent, parseInt: parseInt, parseFloat: parseFloat,
	isNaN: isNaN, isFinite: isFinite
};

var source = fs.readFileSync(process.argv[2], 'utf8');
var i;

try {
	vm.runInNewContext(source, sandbox, { filename: 'office.js' });
	/* a handful of frames: the board is drawn from the actors' pass */
	for (i = 0; i < 4; i++) {
		var fn = rafs.shift();

		if (!fn)
			break;
		fn(1000 * (i + 1));
	}
} catch (e) {
	failures.push('office.js threw: ' + (e && e.stack ? e.stack : e));
}

check(rafs.length > 0 || failures.length > 0, 'no animation frame was ever requested');

/* the board draws each window's label, cut short only by the zone */
board.logins.forEach(function (login) {
	login.windows.forEach(function (w) {
		check(texts.some(function (t) { return t.indexOf(w.label.slice(0, 4)) === 0; }),
		      'canvas text never got the window label "' + w.label + '"');
	});
});

/* Fit is the fraction, aspect kept, and a tall window is not capped at 900. */
check(typeof win.clawtOfficeFitScale === 'function', 'clawtOfficeFitScale was not exported');
if (typeof win.clawtOfficeFitScale === 'function') {
	var fit = win.clawtOfficeFitScale;

	check(Math.abs(fit(1700, 1700, 1000, 1000) - 1.7) < 1e-9,
	      'a fit of 1.7 was not returned as 1.7');
	check(Math.abs(fit(2294, 2231, 1408, 1280) - (2294 / 1408)) < 1e-9,
	      '2294x2231 Fit was floored');
	check(Math.abs(fit(1600, 675, 1408, 1280) - (675 / 1280)) < 1e-9,
	      'a fit below 1 was not left fractional');
	check(fit(0, 100, 100, 100) === 1, 'an empty view did not return 1');
}
win.innerHeight = 2231;
stage.clientWidth = 2294;
(listeners.resize || []).forEach(function (fn) { fn(); });
check(parseFloat(floor.style.height) > 1600,
      'a 2231px window still capped the floor near 900 (height ' +
      floor.style.height + ')');

if (failures.length > 0) {
	failures.forEach(function (f) { console.error('FAIL: ' + f); });
	console.error('drew: ' + JSON.stringify(texts));
	process.exit(1);
}
console.log('ok: ' + texts.length + ' text draws, board labels present');
