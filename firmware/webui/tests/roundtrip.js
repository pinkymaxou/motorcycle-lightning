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

/* and anything else must be refused rather than half-loaded */
try { adoptDoc({ hello: 1 }); out.push('FAIL junk accepted'); }
catch (e) { /* expected */ }

document.title = out.length ? 'PROBE ' + out.join(' | ') : 'PROBE OK';
