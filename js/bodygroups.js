// bodygroups.js — UI-side helpers for the per-model bodygroup system.
//
// A model can ship a <modelname>.cfg next to the model file:
//
//   bodygroup <group_id> <mesh_name> <option_id> <material_or_hide>
//   jiggle <joint_name> <scale> [damping] [stiffness] [gravity]
//
// The whole selection is ONE packed int32 (variable radix per group, see
// q_shared.h BG_BodygroupGet/Set). This file provides the two functions the
// UI needs:
//
//   getBodygroups(model)       -> fills  info.bodygroup.<i>.name / .count
//                                 (does NOT touch .value, the UI owns it)
//   setBodygroups(cvarName)    -> reads info.bodygroup.<i>.value / .count,
//                                 packs them and writes the int32 into cvarName
//                                 (e.g. "modelgroups", a userinfo cvar — from
//                                 there it reaches the server automatically)
//
// UI sliders are expected to create/maintain info.bodygroup.<i>.value (0-based
// option index) themselves and call setBodygroups() when they change.

function bgStripCfgComments(text) {
    // strip /* ... */ and // comments like COM_Parse does in the engine
    var out = text.replace(/\/\*[\s\S]*?\*\//g, " ");
    out = out.replace(/(^|[^:])\/\/[^\n]*/g, "$1 ");
    return out;
}

// tokenizer mirroring COM_Parse: whitespace separated, "quoted strings" allowed
function bgCfgTokens(text) {
    var tokens = [];
    var re = /"((?:\\.|[^"\\])*)"|([^\s"]+)/g;
    var m = text.match(re);
    if (m) {
        for (var i = 0; i < m.length; i++) {
            var t = m[i];
            if (t.charAt(0) === '"') {
                t = t.slice(1, -1).replace(/\\"/g, '"').replace(/\\\\/g, "\\");
            }
            tokens.push(t);
        }
    }
    return tokens;
}

// model -> possible cfg paths. Accepts a full model path
// ("models/props/barrel.iqm", extension optional) or a bare player model name
// ("uni", lives in models/players/<name>/pmodel).
function bgModelCfgCandidates(model) {
    var list = [];
    var m = String(model === undefined || model === null ? "" : model);
    if (m === "") return list;

    if (m.indexOf("/") >= 0) {
        var stripped = m.replace(/\.(iqm|md3)$/i, "");
        list.push(stripped + ".cfg");
    } else {
        list.push("models/players/" + m + "/pmodel.cfg");
        list.push("models/players/" + m + "/" + m + ".cfg");
        list.push("models/players/" + m + ".cfg");
    }
    return list;
}

// parse the bodygroup lines of a cfg (jiggle lines are ignored here, the
// renderer owns them). Groups must be declared consecutively from 0, exactly
// like the renderer requires; count = max option_id + 1.
function bgParseBodygroupCfg(text) {
    var groups = [];
    var tokens = bgCfgTokens(bgStripCfgComments(String(text)));
    var i = 0;

    while (i < tokens.length) {
        var t = tokens[i];

        if (t === "bodygroup") {
            // bodygroup <group_id> <mesh_name> <option_id> <material_or_hide>
            if (i + 5 > tokens.length) break;
            var groupId = parseInt(tokens[i + 1], 10);
            var mesh = tokens[i + 2];
            var optionId = parseInt(tokens[i + 3], 10);
            var mat = tokens[i + 4];
            i += 5;

            if (isNaN(groupId) || groupId < 0 || groupId >= 16) continue;
            if (isNaN(optionId) || optionId < 0 || optionId >= 32) continue;
            if (mesh === undefined || mat === undefined) continue;

            // a gap in the group ids ends the config (same rule as renderer)
            if (groupId > groups.length) break;

            while (groups.length <= groupId) groups.push({ name: "", count: 0, lines: [] });
            var g = groups[groupId];
            if (optionId + 1 > g.count) g.count = optionId + 1;
            if (g.name === "") g.name = mesh;
            g.lines.push({ mesh: mesh, mat: mat, hide: mat === "hide", option: optionId });
            continue;
        }

        if (t === "jiggle") {
            // jiggle <joint> <scale> [damping] [stiffness] [gravity] — skip the
            // rest of the line (all tokens up to the next known directive)
            i += 1;
            while (i < tokens.length && tokens[i] !== "bodygroup" && tokens[i] !== "jiggle") i++;
            continue;
        }

        i += 1;
    }

    return groups;
}

function bgClearInfoCvars(numGroups) {
    // remove stale groups from a previous model (count "0" = empty slot)
    for (var i = 0; i < 64; i++) {
        var c = cvar.int("info.bodygroup." + i + ".count");
        if (c === null || c === undefined) break;
        if (i >= numGroups) cvar.set("info.bodygroup." + i + ".count", "0");
    }
}

// getBodygroups("uni") or getBodygroups("models/props/barrel.iqm")
// Fills info.bodygroup.<i>.name / .count for every group of the model cfg and
// returns the number of groups (0 = model has no bodygroups).
function getBodygroups(model) {
    var candidates = bgModelCfgCandidates(model);
    var groups = null;
    var i;

    for (i = 0; i < candidates.length; i++) {
        var text = file.open(candidates[i]);
        if (text !== null && text !== undefined) {
            groups = bgParseBodygroupCfg(text);
            break;
        }
    }

    if (groups === null) {
        console.log("#ff5getBodygroups: no cfg found for model '" + model + "'");
        bgClearInfoCvars(0);
        return 0;
    }

    bgClearInfoCvars(groups.length);

    for (i = 0; i < groups.length; i++) {
        cvar.set("info.bodygroup." + i + ".name", groups[i].name);
        cvar.set("info.bodygroup." + i + ".count", String(groups[i].count));
    }

    return groups.length;
}

// setBodygroups("modelgroups") — packs info.bodygroup.<i>.value (UI slider
// state) into a single int32 and writes it into the cvar. If the cvar is a
// userinfo cvar (like "modelgroups") the value reaches the server, which puts
// it into entityState_t::bodygroups.
// Returns the packed value.
function setBodygroups(cvarName) {
    var packed = 0;
    var base = 1;
    var i;

    for (i = 0; i < 16; i++) {
        var count = cvar.int("info.bodygroup." + i + ".count");
        if (count === null || count === undefined || count < 1) break;

        var value = cvar.int("info.bodygroup." + i + ".value");
        if (value === null || value === undefined) value = 0;
        if (value < 0) value = 0;
        if (value >= count) value = count - 1;

        // radix overflow guard (int32), same rule as the engine
        if (base > 2147483647 / count) break;

        packed += value * base;
        base *= count;
    }

    if (packed < 0) packed = 0;
    cvar.set(String(cvarName), String(packed));
    return packed;
}

// convenience: read the packed value of a cvar (server -> UI round trip)
function getBodygroupsPacked(cvarName) {
    var v = cvar.int(cvarName);
    if (v === null || v === undefined || v < 0) v = 0;
    return v;
}
