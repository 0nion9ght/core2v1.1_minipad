#!/usr/bin/env node
/*
 * gen_boot_anim.js - export the boot splash "handwriting" path.
 *
 * Traces the glyph outlines of TEXT as one continuous pen path (letters in order,
 * left-most/outer contour first inside a letter) and writes the polylines as
 * screen-space integers (1/16 px). The firmware rasterises the path onto an A8
 * canvas a few segments per frame (see components/gui/boot_anim.c), so only the
 * path is stored in flash (~2 KB instead of ~400 KB of pre-rendered frames).
 *
 * Usage:  node tools/gen_boot_anim.js
 * Output: components/gui/boot_frames/boot_path.{c,h}
 */
'use strict';

const fs = require('fs');
const path = require('path');

const PROJECT = path.join(__dirname, '..');
const opentype = require(path.join(PROJECT, 'reference', 'lv_font_conv-master',
                                   'node_modules', 'opentype.js'));

// ---------------------------------------------------------------- parameters
const FONT = path.join(PROJECT, 'reference', 'ChillDINGothic.v1.300',
                       'ChillDINGothic v1.300', 'otf', 'ChillDINGothic_Regular.otf');
const OUT_DIR = path.join(PROJECT, 'components', 'gui', 'boot_frames');

const TEXT = 'Shallwe';
const CANVAS_W = 320;
const CANVAS_H = 240;
const TARGET_W = 268;      // ink width budget in px
const TARGET_H = 96;       // ink height budget in px
const BEZIER_STEPS = 8;    // flattening resolution
const Q = 16;              // fixed point divisor for the emitted integers

const font = opentype.loadSync(FONT);

// ------------------------------------------------------------------- outlines
function glyphPolys(ch) {
    const glyph = font.charToGlyph(ch);
    const polys = [];
    let cur = null;
    let cx = 0, cy = 0;
    const push = (x, y) => cur.push({ x, y });

    for (const c of glyph.path.commands) {
        if (c.type === 'M') {
            cur = [];
            polys.push(cur);
            push(c.x, c.y);
            cx = c.x; cy = c.y;
        } else if (c.type === 'L') {
            push(c.x, c.y);
            cx = c.x; cy = c.y;
        } else if (c.type === 'C') {
            for (let i = 1; i <= BEZIER_STEPS; i++) {
                const t = i / BEZIER_STEPS, mt = 1 - t;
                push(mt * mt * mt * cx + 3 * mt * mt * t * c.x1 + 3 * mt * t * t * c.x2 + t * t * t * c.x,
                     mt * mt * mt * cy + 3 * mt * mt * t * c.y1 + 3 * mt * t * t * c.y2 + t * t * t * c.y);
            }
            cx = c.x; cy = c.y;
        } else if (c.type === 'Q') {
            for (let i = 1; i <= BEZIER_STEPS; i++) {
                const t = i / BEZIER_STEPS, mt = 1 - t;
                push(mt * mt * cx + 2 * mt * t * c.x1 + t * t * c.x,
                     mt * mt * cy + 2 * mt * t * c.y1 + t * t * c.y);
            }
            cx = c.x; cy = c.y;
        } else if (c.type === 'Z') {
            if (cur && cur.length > 1) { push(cur[0].x, cur[0].y); }
        }
    }
    return { advance: glyph.advanceWidth, polys: polys.filter((p) => p.length > 1) };
}

function area(pts) {
    let a = 0;
    for (let i = 0; i < pts.length; i++) {
        const p = pts[i], q = pts[(i + 1) % pts.length];
        a += p.x * q.y - q.x * p.y;
    }
    return Math.abs(a) / 2;
}

const minX = (pts) => pts.reduce((m, p) => Math.min(m, p.x), Infinity);

function buildPath() {
    const perGlyph = TEXT.split('').map((ch) => {
        const g = glyphPolys(ch);
        const ordered = g.polys.slice().sort((a, b) => {
            if (Math.abs(minX(a) - minX(b)) > g.advance * 0.15) { return minX(a) - minX(b); }
            return area(b) - area(a); /* outer contour first */
        });
        return { advance: g.advance, polys: ordered };
    });

    let minXX = Infinity, maxXX = -Infinity, minYY = Infinity, maxYY = -Infinity;
    let cursor = 0;
    for (const g of perGlyph) {
        for (const poly of g.polys) {
            for (const p of poly) {
                minXX = Math.min(minXX, p.x + cursor); maxXX = Math.max(maxXX, p.x + cursor);
                minYY = Math.min(minYY, p.y); maxYY = Math.max(maxYY, p.y);
            }
        }
        cursor += g.advance;
    }

    const scale = Math.min(TARGET_W / (maxXX - minXX), TARGET_H / (maxYY - minYY));
    const offX = (CANVAS_W - (maxXX - minXX) * scale) / 2;
    const offY = (CANVAS_H - (maxYY - minYY) * scale) / 2;

    const polys = [];
    cursor = 0;
    for (const g of perGlyph) {
        for (const poly of g.polys) {
            polys.push(poly.map((p) => ({
                x: (p.x + cursor - minXX) * scale + offX,
                y: (maxYY - p.y) * scale + offY, /* font y grows up */
            })));
        }
        cursor += g.advance;
    }

    let total = 0;
    for (const poly of polys) {
        for (let i = 1; i < poly.length; i++) {
            total += Math.hypot(poly[i].x - poly[i - 1].x, poly[i].y - poly[i - 1].y);
        }
    }
    return { polys, total, scale };
}

// -------------------------------------------------------------------- emit C
function emit(polys, total, scale) {
    fs.mkdirSync(OUT_DIR, { recursive: true });

    const pts = [];
    const starts = [0];
    for (const poly of polys) {
        for (const p of poly) {
            pts.push({ x: Math.round(p.x * Q), y: Math.round(p.y * Q) });
        }
        starts.push(pts.length);
    }

    let c = '/* generated by tools/gen_boot_anim.js - do not edit */\n';
    c += '#include "boot_path.h"\n\n';
    c += `const boot_path_pt_t boot_path_points[] = {\n`;
    for (let i = 0; i < pts.length; i += 6) {
        c += '    ' + pts.slice(i, i + 6).map((p) => `{${p.x}, ${p.y}}`).join(', ') + ',\n';
    }
    c += '};\n\n';

    c += `const uint16_t boot_path_poly_start[] = {\n    `;
    c += starts.map((s) => s.toString()).join(', ');
    c += ',\n};\n\n';
    c += `const uint16_t boot_path_poly_count = ${polys.length};\n`;
    c += `const uint32_t boot_path_total_q = ${Math.round(total * Q)};\n`;

    const h = `/* generated by tools/gen_boot_anim.js - do not edit */
#pragma once

#include <stdint.h>

/* Pen path in screen space, 1/16 px units. Polylines are slices of
 * boot_path_points[]: polyline i spans [poly_start[i], poly_start[i+1]). */
typedef struct {
    int16_t x;
    int16_t y;
} boot_path_pt_t;

extern const boot_path_pt_t boot_path_points[];
extern const uint16_t boot_path_poly_start[];
extern const uint16_t boot_path_poly_count;
extern const uint32_t boot_path_total_q;
`;

    fs.writeFileSync(path.join(OUT_DIR, 'boot_path.c'), c);
    fs.writeFileSync(path.join(OUT_DIR, 'boot_path.h'), h);

    const srcBytes = pts.length * 4 + starts.length * 2;
    console.log(`text='${TEXT}' scale=${scale.toFixed(3)} path=${total.toFixed(0)}px ` +
                `polys=${polys.length} points=${pts.length} flash≈${(srcBytes / 1024).toFixed(1)}KB`);
}

const { polys, total, scale } = buildPath();
emit(polys, total, scale);
