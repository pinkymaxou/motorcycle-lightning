/* Runtime check for the page's own codecs, run by tools/test_webui.sh in a
 * headless browser. The static checker cannot see whether encodeConfig and
 * adoptDoc still agree — this can, and it is the path a configuration takes
 * when it leaves the module and comes back. */
const out = [];

function bytes(c) { return Array.from(encodeConfig(c, '', '')).join(','); }

function sample() {
    const c = decodeConfig(new Uint8Array());
    c.strips = [
        { led_model: 1, color_order: 2, reversed: true, sections: [
            { led_count: 12, reversed: true, turn: 1, idle: 'f_position',
              brake: 'f_brake', turn_on: 'f_turn_sweep', turn_off: 'f_turn_off',
              aux: '', fx_open: false },
            { led_count: 16, reversed: false, turn: 0, idle: 'f_position',
              brake: 'f_brake_flash', turn_on: '', turn_off: '', aux: '',
              fx_open: false }] },
        { led_model: 0, color_order: 0, reversed: false, sections: [] }];
    c.colors = [0x670000FF, 0xFF0000FF, 0xFF8000FF, 0xFFFFFF80];
    c.hazard_on = 'f_turn_on';
    c.hazard_off = '';
    c.exit_x10 = 12;
    c.holdoff_s = 25;
    c.sta = { ssid: 'home', active: true, pass_set: true };
    c.ap = { ssid: 'MotoMax', pass_set: true };
    return c;
}

/* colors are 0xRRGGBBAA in the page and #RRGGBB + brightness in the file */
for (const v of [0xFF0000FF, 0x670000FF, 0xFFFFFF80, 0x0000FF01]) {
    const back = colorFromHex(colorToHex(v >>> 0), v & 255);
    if (back !== (v >>> 0)) { out.push('FAIL color ' + v.toString(16)); }
}
if (colorFromHex('FF8000', 255) !== 0xFF8000FF) { out.push('FAIL bare hex'); }

/* a configuration must survive export -> file text -> import unchanged */
cfg = sample();
const before = bytes(cfg);
const text = JSON.stringify(exportDoc(), null, 2);
cfg = null;
adoptDoc(JSON.parse(text));
if (before !== bytes(cfg)) { out.push('FAIL config round trip'); }

/* the module-facing direction: what the page encodes, it decodes back */
cfg = sample();
const twice = bytes(decodeConfig(encodeConfig(cfg, '', '')));
if (twice !== bytes(cfg)) { out.push('FAIL decode(encode(c)) != c'); }

/* nothing that is not configuration goes into the file */
cfg = sample();
cfg.strips[0].sections[0].fx_open = true;
const doc = exportDoc();
if (JSON.stringify(doc).includes('fx_open')) { out.push('FAIL fx_open exported'); }
if (JSON.stringify(doc).includes('pass_set')) { out.push('FAIL pass_set exported'); }

/* a hand-edited file with garbage must not freeze or break the page */
cfg = sample();
cfg.sta.pass_set = true;
const bad = JSON.parse(JSON.stringify(exportDoc()));
bad.config.strips[0].sections[0].led_count = -1;
bad.config.strips[0].sections[1].turn = '1';
bad.config.exit_x10 = 1e999;
bad.config.colors = [{ rgb: '#123456', brightness: 7 }];      /* too short */
bad.config.sta.pass_set = false;                             /* lies */
try {
    adoptDoc(bad);
    const enc = encodeConfig(cfg, '', '');            /* must terminate */
    if (!enc.length) { out.push('FAIL bad file encodes to nothing'); }
    if (1 !== cfg.strips[0].sections[1].turn) { out.push('FAIL string turn not coerced'); }
    if (cfg.colors[1] !== sample().colors[1]) { out.push('FAIL short colours not kept'); }
    if (true !== cfg.sta.pass_set) { out.push('FAIL pass_set taken from file'); }
} catch (e) { out.push('FAIL bad file threw: ' + e); }

/* an unknown turn source is a refusal, not a broken render */
const worse = JSON.parse(JSON.stringify(exportDoc()));
worse.config.strips[0].sections[0].turn = 5;
try { adoptDoc(worse); out.push('FAIL turn 5 accepted'); } catch (e) { /* expected */ }

/* a file from a newer page says so */
try { adoptDoc({ motolights: 99, config: { strips: [] } }); out.push('FAIL newer accepted'); }
catch (e) { if (!String(e).includes('newer')) { out.push('FAIL newer message: ' + e); } }

/* and anything else must be refused rather than half-loaded */
try { adoptDoc({ hello: 1 }); out.push('FAIL junk accepted'); }
catch (e) { /* expected */ }

document.title = out.length ? 'PROBE ' + out.join(' | ') : 'PROBE OK';
