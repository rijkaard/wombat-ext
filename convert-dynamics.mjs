#!/usr/bin/env node
/**
 * convert-dynamics.mjs — rename Q-coded wombat variable names in dynamic0.mul
 *
 * The server serialises wombat object-variables as NUL-terminated strings in
 * the format:  wom_var=TYPE name value\0
 * When the server ran Q-coded scripts, those `name` tokens are Q-codes (e.g.
 * Q57Q). This tool replaces them with the human-readable names recorded in
 * renames.json, and optionally verifies the round-trip.
 *
 * Usage
 * -----
 *   Forward (Q-codes → names):
 *     node convert-dynamics.mjs [opts] <dynamic0.mul>
 *
 *   Inverse (names → Q-codes):
 *     node convert-dynamics.mjs --inverse [opts] <dynamic0.mul>
 *
 *   Verify round-trip (inverse(forward(original)) == original, byte-exact):
 *     node convert-dynamics.mjs --verify-inverse [opts] \
 *         <original.mul> <converted.mul>
 *
 *   Self-test (synthetic injection + round-trip using any available .mul):
 *     node convert-dynamics.mjs --self-test [opts] <dynamic0.mul>
 *
 * Options
 * -------
 *   --renames <path>   renames.json (default: ./wombat-interpret/renames.json)
 *   --idx <path>       paired index file (default: auto-detected)
 *   --out-data <path>  output data file  (default: <input>-renamed.mul)
 *   --out-idx  <path>  output index file (default: <idx>-renamed.mul)
 *   --inverse          apply inverse mapping (names → Q-codes)
 *   --verbose, -v      print every substitution
 *
 * Index file auto-detection
 * -------------------------
 *   dynamic0.mul  →  dynidx0.mul  (same directory, "dynamic" → "dynidx")
 *
 * What is renamed
 * ---------------
 *   • wom_var=TYPE name value\0        — the `name` token
 *   • wom_scr=script count TYPE name value … \0  — each `name` token
 *     (member variables of attached scripts; serialised inline in the entry)
 *   When the same Q-code appears in renames.json with different names across
 *   scripts (conflicts), the first mapping wins and a warning is printed.
 *
 * Never overwrites the input file.
 */

import { readFileSync, writeFileSync, openSync, writeSync, closeSync, existsSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join, basename, extname } from 'path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const DEFAULT_RENAMES = join(__dirname, 'wombat-interpret/renames.json');

// ─── argument parsing ────────────────────────────────────────────────────────

function usage() {
    console.error(`\
Usage:
  node convert-dynamics.mjs [opts] <dynamic0.mul>                   # forward
  node convert-dynamics.mjs --inverse [opts] <dynamic0.mul>         # inverse
  node convert-dynamics.mjs --verify-inverse [opts] <orig.mul> <conv.mul>
  node convert-dynamics.mjs --self-test [opts] <dynamic0.mul>

Options:
  --renames <path>   renames.json (default: ./wombat-interpret/renames.json)
  --idx <path>       paired index file (default: auto-detected)
  --out-data <path>  output data file
  --out-idx  <path>  output index file
  --inverse          names → Q-codes
  --verbose / -v     print each substitution
`);
    process.exit(1);
}

function parseArgs(argv) {
    const opts = {
        renames:       DEFAULT_RENAMES,
        idx:           null,
        outData:       null,
        outIdx:        null,
        inverse:       false,
        verifyInverse: false,
        selfTest:      false,
        verbose:       false,
        inputs:        [],
    };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if      (a === '--renames')        opts.renames       = argv[++i];
        else if (a === '--idx')            opts.idx           = argv[++i];
        else if (a === '--out-data')       opts.outData       = argv[++i];
        else if (a === '--out-idx')        opts.outIdx        = argv[++i];
        else if (a === '--inverse')        opts.inverse       = true;
        else if (a === '--verify-inverse') opts.verifyInverse = true;
        else if (a === '--self-test')      opts.selfTest      = true;
        else if (a === '--verbose' || a === '-v') opts.verbose = true;
        else if (a.startsWith('-')) { console.error(`Unknown option: ${a}`); usage(); }
        else opts.inputs.push(a);
    }
    return opts;
}

// ─── index file I/O ─────────────────────────────────────────────────────────

// 12 bytes per entry: int32LE offset, int32LE length, int32LE extra
// offset < 0 → unused slot

function readIndex(path) {
    const buf = readFileSync(path);
    const n = Math.floor(buf.length / 12);
    const entries = new Array(n);
    for (let i = 0; i < n; i++) {
        entries[i] = {
            off:   buf.readInt32LE(i * 12),
            len:   buf.readInt32LE(i * 12 + 4),
            extra: buf.readInt32LE(i * 12 + 8),
        };
    }
    return entries;
}

function writeIndex(entries, path) {
    const buf = Buffer.allocUnsafe(entries.length * 12);
    for (let i = 0; i < entries.length; i++) {
        buf.writeInt32LE(entries[i].off,   i * 12);
        buf.writeInt32LE(entries[i].len,   i * 12 + 4);
        buf.writeInt32LE(entries[i].extra, i * 12 + 8);
    }
    writeFileSync(path, buf);
}

// ─── rename map construction ─────────────────────────────────────────────────

// Build two maps from renames.json:
//
//   scrMap  — Map< "scriptname.Qxxxx", newName >
//             Used for wom_scr lookups: only "scriptname.Qxxxx" member-variable
//             entries (no colon).  Lookup key is "scriptname.varname".
//
//   varMap  — Map< Qxxxx, newName >
//             Used for wom_var lookups: flat, first-wins for conflicts.
//             Populated from all entry types (member + local) as a best-effort
//             fallback since wom_var tags are not script-scoped.
//
// renames.json key formats:
//   "scriptname.Qxxxx"           → member variable   → added to scrMap + varMap
//   "scriptname.funcname:Qxxxx"  → local variable    → added to varMap only
//   "Qxxxx"                      → bare (legacy)     → added to varMap only

function buildMaps(renamesPath, verbose) {
    const raw = JSON.parse(readFileSync(renamesPath, 'utf8'));

    const scrMap    = new Map();   // "scriptname.Qxxxx" → newName
    const varMap    = new Map();   // Qxxxx → newName (flat, first-wins)
    const varConflicts = new Map();

    for (const [key, newName] of Object.entries(raw)) {
        const dot = key.indexOf('.');

        if (dot < 0) {
            // Bare Qxxxx entry
            if (/^Q[0-9A-Za-z]{2,6}$/.test(key) && !varMap.has(key))
                varMap.set(key, newName);
            continue;
        }

        const scriptName = key.slice(0, dot);
        const rest       = key.slice(dot + 1);
        const colon      = rest.indexOf(':');
        const qCode      = colon >= 0 ? rest.slice(colon + 1) : rest;

        if (!/^Q[0-9A-Za-z]{2,6}$/.test(qCode)) continue;

        if (colon < 0) {
            // "scriptname.Qxxxx" — member variable: add to scrMap with full key
            scrMap.set(`${scriptName}.${qCode}`, newName);
        }

        // All types feed varMap (first-wins; conflicts are expected for locals)
        if (!varConflicts.has(qCode)) varConflicts.set(qCode, new Set());
        varConflicts.get(qCode).add(newName);
        if (!varMap.has(qCode)) varMap.set(qCode, newName);
    }

    let conflictCount = 0;
    for (const [qCode, names] of varConflicts) {
        if (names.size > 1) {
            conflictCount++;
            if (verbose)
                process.stderr.write(
                    `CONFLICT: ${qCode} → {${[...names].join(', ')}} — using "${varMap.get(qCode)}"\n`
                );
        }
    }
    if (conflictCount > 0)
        process.stderr.write(
            `${conflictCount} Q-code(s) with conflicting wom_var names — ` +
            `first mapping wins. Use -v to list them.\n`
        );

    return { scrMap, varMap };
}

// Invert both maps for --inverse mode.
//   invScrMap: "scriptname.humanName" → Qxxxx
//   invVarMap: humanName → Qxxxx (first-wins)
function invertMaps({ scrMap, varMap }) {
    const invScrMap = new Map();
    for (const [key, newName] of scrMap) {
        // key = "scriptname.Qxxxx"
        const dot        = key.indexOf('.');
        const scriptName = key.slice(0, dot);
        const invKey     = `${scriptName}.${newName}`;
        if (!invScrMap.has(invKey)) invScrMap.set(invKey, key.slice(dot + 1));
    }

    const invVarMap = new Map();
    for (const [qCode, name] of varMap) {
        if (!invVarMap.has(name)) invVarMap.set(name, qCode);
    }

    return { scrMap: invScrMap, varMap: invVarMap };
}

// ─── per-block transformation ────────────────────────────────────────────────

// Parse "wom_var=TYPE name value" → { type, name, value } or null.
// `value` retains the leading space that the C formatter emits
// (format: "wom_var=%3s %s " then value appended).
function parseWomVar(str) {
    if (!str.startsWith('wom_var=')) return null;
    const rest = str.slice(8);           // "int name value"
    const sp1  = rest.indexOf(' ');
    if (sp1 < 0) return null;
    const type  = rest.slice(0, sp1);
    const rest2 = rest.slice(sp1 + 1);  // "name value"
    const sp2   = rest2.indexOf(' ');
    if (sp2 < 0) return null;
    return { type, name: rest2.slice(0, sp2), value: rest2.slice(sp2) };
}

// Advance past a serialized value of the given type3 in str starting at pos.
// Returns [newPos, rawChars] where rawChars is the characters consumed
// (including trailing space if present, to be copied verbatim).
// Mirrors WomScr_LoadFromLine / List_DeserializeFromBuf advancing logic.
function skipValue(str, pos, type3) {
    if (type3 === 'loc') {
        // 3 space-separated integers (X Y Z)
        let out = '';
        for (let t = 0; t < 3; t++) {
            let end = str.indexOf(' ', pos);
            if (end < 0) end = str.length;
            out += str.slice(pos, end);
            pos = end;
            if (pos < str.length && str[pos] === ' ') { out += ' '; pos++; }
        }
        return [pos, out];
    }
    if (type3 === 'str') {
        // ObjVar_EscapeStr encodes ' ' as '\\ ' and '\\' as '\\\\'.
        // ObjVar_UnescapeStr: on '\\' skip the backslash and copy next char;
        // stop at an unescaped ' ' or NUL.  Mirror that skip here.
        let i = pos;
        while (i < str.length && str[i] !== '\0') {
            if (str[i] === '\\') { i += 2; continue; } // backslash + escaped char
            if (str[i] === ' ') break;                 // unescaped space = end
            i++;
        }
        let out = str.slice(pos, i);
        if (i < str.length && str[i] === ' ') { out += ' '; i++; }
        return [i, out];
    }
    if (type3 === 'lis') {
        // count token, then count × list-item (each is "type value_tokens…")
        let end = str.indexOf(' ', pos);
        if (end < 0) end = str.length;
        const count = parseInt(str.slice(pos, end), 10);
        let out = str.slice(pos, end);
        pos = end;
        if (pos < str.length && str[pos] === ' ') { out += ' '; pos++; }
        if (!isNaN(count)) {
            for (let i = 0; i < count; i++) {
                // Each list item: "type3 value_tokens…" (no varname)
                let tEnd = str.indexOf(' ', pos);
                if (tEnd < 0) break;
                const itemType = str.slice(pos, tEnd).toLowerCase().slice(0, 3);
                out += str.slice(pos, tEnd);
                pos = tEnd;
                if (pos < str.length && str[pos] === ' ') { out += ' '; pos++; }
                const [newPos, consumed] = skipValue(str, pos, itemType);
                out += consumed;
                pos = newPos;
            }
        }
        return [pos, out];
    }
    // int, obj, ust, voi: single token (voi has no token — returns immediately)
    if (type3 === 'voi') return [pos, ''];
    let end = str.indexOf(' ', pos);
    if (end < 0) end = str.length;
    let out = str.slice(pos, end);
    pos = end;
    if (pos < str.length && str[pos] === ' ') { out += ' '; pos++; }
    return [pos, out];
}

// Process a wom_scr NUL-terminated string, renaming member variable names.
// Format (from dynamic.c serialisation):
//   wom_scr=<scriptname> <count> [<type3> <varname> <value_tokens>…]…
//
// scrMap key is "scriptname.varname" — so renaming is scoped per script:
//   renames.json "poisonsk.Q5JG" → "poison_potion" is only applied inside
//   "wom_scr=poisonsk …", not in any other script's entry.
//
// Returns a new string if any varname was renamed, or null if unchanged.
function processWomScr(str, scrMap, verbose) {
    if (!str.startsWith('wom_scr=')) return null;
    let p = 8; // after "wom_scr="

    // Script name
    let nameEnd = str.indexOf(' ', p);
    if (nameEnd < 0) return null;
    const scriptName = str.slice(p, nameEnd);
    let result = str.slice(0, nameEnd + 1); // "wom_scr=<name> "
    p = nameEnd + 1;

    // Variable count
    let countEnd = str.indexOf(' ', p);
    if (countEnd < 0) return null;
    const count = parseInt(str.slice(p, countEnd), 10);
    if (isNaN(count) || count < 0) return null;
    result += str.slice(p, countEnd + 1); // "<count> "
    p = countEnd + 1;

    let modified = false;

    for (let vi = 0; vi < count; vi++) {
        // Type token
        let typeEnd = str.indexOf(' ', p);
        if (typeEnd < 0) break;
        const type3 = str.slice(p, typeEnd).toLowerCase().slice(0, 3);
        result += str.slice(p, typeEnd + 1);
        p = typeEnd + 1;

        // Varname token — look up "scriptname.varname" in scrMap
        let varnameEnd = str.indexOf(' ', p);
        if (varnameEnd < 0) break;
        const varname = str.slice(p, varnameEnd);
        const newName = scrMap.get(`${scriptName}.${varname}`);
        if (newName && newName !== varname) {
            result += newName + ' ';
            modified = true;
            if (verbose) process.stdout.write(
                `  wom_scr ${scriptName}:${varname} → ${newName}\n`);
        } else {
            result += str.slice(p, varnameEnd + 1);
        }
        p = varnameEnd + 1;

        // Skip value tokens
        const [newP, consumed] = skipValue(str, p, type3);
        result += consumed;
        p = newP;
    }

    if (p < str.length) result += str.slice(p);

    return modified ? result : null;
}

// Transform a block Buffer: scan NUL-terminated strings, rename wom_var names
// and wom_scr member variable names.
// maps = { scrMap, varMap } — both maps already oriented for the current direction
//   (forward: Q-code → name; inverse: name → Q-code).
// Returns the original Buffer unchanged if no substitution was made (no alloc).
// Otherwise returns a new Buffer (may be larger or smaller).
function transformBlock(block, { scrMap, varMap }, verbose) {
    let pos      = 0;
    let modified = false;
    const parts  = [];

    while (pos < block.length) {
        // Find end of this NUL-terminated string.
        let nul = block.indexOf(0, pos);
        if (nul < 0) nul = block.length - 1;  // malformed: treat rest as last string

        // Both wom_var and wom_scr start with 'w'.
        if (block[pos] === 0x77 /* 'w' */) {
            const str = block.slice(pos, nul).toString('latin1');

            if (str.startsWith('wom_var=')) {
                const parsed = parseWomVar(str);
                if (parsed) {
                    const newName = varMap.get(parsed.name);
                    if (newName) {
                        const newStr = `wom_var=${parsed.type} ${newName}${parsed.value}\0`;
                        if (verbose) process.stdout.write(`  ${str}  →  ${newStr.slice(0, -1)}\n`);
                        parts.push(Buffer.from(newStr, 'latin1'));
                        modified = true;
                        pos = nul + 1;
                        continue;
                    }
                }
            } else if (str.startsWith('wom_scr=')) {
                const newStr = processWomScr(str, scrMap, verbose);
                if (newStr !== null) {
                    parts.push(Buffer.from(newStr + '\0', 'latin1'));
                    modified = true;
                    pos = nul + 1;
                    continue;
                }
            }
        }

        // No substitution — use a zero-copy slice of the original block.
        parts.push(block.slice(pos, nul + 1));
        pos = nul + 1;
    }

    if (!modified) return block;  // fast path: no allocation
    return Buffer.concat(parts);
}

// ─── file-level conversion ───────────────────────────────────────────────────

/**
 * Read data + index, apply nameMap to every block, write new data + index.
 * Blocks are written one at a time to avoid accumulating a large in-memory
 * copy of the entire output file.
 * Returns { substitutions, byteDelta }.
 */
function convertFiles(dataPath, idxPath, outDataPath, outIdxPath, maps, verbose) {
    const data  = readFileSync(dataPath);  // ~38 MB, needed for random-offset access
    const index = readIndex(idxPath);

    // Sort valid slots by their position in the input file so we write them
    // in the same relative order (preserving locality).
    const validSlots = [];
    for (let i = 0; i < index.length; i++) {
        const e = index[i];
        if (e.off >= 0 && e.len > 0) validSlots.push(i);
    }
    validSlots.sort((a, b) => index[a].off - index[b].off);

    // Open output data file for incremental writing.
    const fd = openSync(outDataPath, 'w');

    // Copy index entries; we'll patch off and len for valid slots.
    const newIndex = index.map(e => ({ ...e }));

    let writeOff      = 0;
    let substitutions = 0;

    for (const i of validSlots) {
        const { off, len, extra } = index[i];
        const block    = data.slice(off, off + len);
        const newBlock = transformBlock(block, maps, verbose);

        writeSync(fd, newBlock);

        newIndex[i].off = writeOff;
        newIndex[i].len = newBlock.length;
        writeOff += newBlock.length;

        if (newBlock !== block) substitutions++;
    }

    closeSync(fd);
    writeIndex(newIndex, outIdxPath);

    const origSize = validSlots.reduce((s, i) => s + index[i].len, 0);
    return { substitutions, byteDelta: writeOff - origSize };
}

// ─── verify inverse ──────────────────────────────────────────────────────────

/**
 * Apply the inverse of nameMap to the converted file; compare the resulting
 * sequential block data byte-for-byte with the original file's sequential
 * block data.  Exits with code 1 on mismatch.
 *
 * "Sequential block data" means all valid blocks concatenated in offset order,
 * which is what convertFiles produces.
 */
function verifyInverse(origDataPath, origIdxPath, convDataPath, convIdxPath, maps) {
    console.log('Building inverse map…');
    const inv      = invertMaps(maps);
    const origData = readFileSync(origDataPath);
    const convData = readFileSync(convDataPath);
    const origIdx  = readIndex(origIdxPath);
    const convIdx  = readIndex(convIdxPath);

    // Build sequential block buffers for both files.
    function collectBlocks(data, idx) {
        const slots = [];
        for (let i = 0; i < idx.length; i++) {
            if (idx[i].off >= 0 && idx[i].len > 0) slots.push(i);
        }
        slots.sort((a, b) => idx[a].off - idx[b].off);
        return slots.map(i => data.slice(idx[i].off, idx[i].off + idx[i].len));
    }

    const origBlocks = collectBlocks(origData, origIdx);
    const convBlocks = collectBlocks(convData, convIdx);

    if (origBlocks.length !== convBlocks.length) {
        console.error(`Block count mismatch: orig=${origBlocks.length} conv=${convBlocks.length}`);
        process.exit(1);
    }

    let firstDiffBlock = -1;
    let firstDiffByte  = -1;

    for (let b = 0; b < origBlocks.length; b++) {
        const restored = transformBlock(convBlocks[b], inv, false);
        if (!restored.equals(origBlocks[b])) {
            firstDiffBlock = b;
            for (let j = 0; j < Math.max(restored.length, origBlocks[b].length); j++) {
                if (restored[j] !== origBlocks[b][j]) { firstDiffByte = j; break; }
            }
            const A = origBlocks[b].slice(firstDiffByte, firstDiffByte + 32);
            const B = restored.slice(firstDiffByte, firstDiffByte + 32);
            console.error('✗ Inverse verification FAILED');
            console.error(`  Block ${b}, byte ${firstDiffByte}`);
            console.error(`  Original   : ${A.toString('hex')}  "${A.toString('latin1').replace(/\0/g, '·')}"`);
            console.error(`  Reconstructed: ${B.toString('hex')}  "${B.toString('latin1').replace(/\0/g, '·')}"`);
            process.exit(1);
        }
    }

    console.log('✓ Inverse verification PASSED — byte-for-byte identical to original');
}

// ─── self-test ───────────────────────────────────────────────────────────────

/**
 * Synthetic round-trip test:
 *   1. Scan the real data file for wom_var names that appear as VALUES in
 *      the renames map (human-readable → Q-code, via inverse lookup).
 *   2. Replace those names with their Q-codes in a temp in-memory copy.
 *   3. Forward-transform the synthetic copy.
 *   4. Assert the result equals the original block data byte-for-byte.
 *
 * This exercises the full round-trip even when no Q-coded dynamics file exists.
 */
function selfTest(dataPath, idxPath, maps) {
    console.log('Self-test: building synthetic Q-coded copy…');
    const inv = invertMaps(maps);

    const data  = readFileSync(dataPath);
    const index = readIndex(idxPath);

    const slots = [];
    for (let i = 0; i < index.length; i++) {
        if (index[i].off >= 0 && index[i].len > 0) slots.push(i);
    }
    slots.sort((a, b) => index[a].off - index[b].off);

    const origBlocks = slots.map(i => data.slice(index[i].off, index[i].off + index[i].len));

    let injected = 0;
    const synBlocks = origBlocks.map(block => {
        const xformed = transformBlock(block, inv, false);
        if (xformed !== block) injected++;
        return xformed;
    });

    if (injected === 0) {
        console.log('Self-test: no wom_var names overlap with renames.json values.');
        console.log('  Verifying forward pass makes zero changes…');
    } else {
        console.log(`Self-test: injected Q-codes in ${injected} block(s).`);
    }

    // Forward-transform the synthetic blocks and compare with originals.
    let ok = true;
    for (let b = 0; b < synBlocks.length; b++) {
        const restored = transformBlock(synBlocks[b], maps, false);
        if (!restored.equals(origBlocks[b])) {
            ok = false;
            let j = 0;
            while (restored[j] === origBlocks[b][j]) j++;
            const A = origBlocks[b].slice(j, j + 32);
            const B = restored.slice(j, j + 32);
            console.error(`✗ Self-test FAILED at block ${b}, byte ${j}`);
            console.error(`  Original     : ${A.toString('hex')}  "${A.toString('latin1').replace(/\0/g, '·')}"`);
            console.error(`  After fwd    : ${B.toString('hex')}  "${B.toString('latin1').replace(/\0/g, '·')}"`);
            break;
        }
    }
    if (ok) {
        console.log(`✓ Self-test PASSED — ${injected} injection(s), round-trip byte-exact`);
    } else {
        process.exit(1);
    }
}

// ─── output path helpers ─────────────────────────────────────────────────────

function idxPathFor(dataPath) {
    const dir  = dirname(dataPath);
    const base = basename(dataPath);

    // Primary convention: dynamic0.mul ↔ dynidx0.mul
    const cand1 = join(dir, base.replace(/^dynamic/, 'dynidx'));
    if (cand1 !== dataPath && existsSync(cand1)) return cand1;

    // Secondary: insert "idx" before the first digit
    const cand2 = join(dir, base.replace(/(\d)/, 'idx$1'));
    if (existsSync(cand2)) return cand2;

    return null;
}

function renamedPath(p, suffix) {
    const ext  = extname(p);
    return join(dirname(p), basename(p, ext) + suffix + ext);
}

// ─── main ────────────────────────────────────────────────────────────────────

const opts = parseArgs(process.argv.slice(2));
if (opts.inputs.length === 0) usage();

const maps = buildMaps(opts.renames, opts.verbose);
console.log(`Loaded ${maps.scrMap.size} script-scoped + ${maps.varMap.size} flat mappings from ${opts.renames}\n`);

// ── verify-inverse ────────────────────────────────────────────────────────────
if (opts.verifyInverse) {
    if (opts.inputs.length < 2) {
        console.error('--verify-inverse requires: <original.mul> <converted.mul>');
        usage();
    }
    const [origData, convData] = opts.inputs;
    const origIdx = opts.idx || idxPathFor(origData);
    const convIdx = idxPathFor(convData);
    if (!origIdx) { console.error(`Cannot find index for ${origData}`); process.exit(1); }
    if (!convIdx) { console.error(`Cannot find index for ${convData}`); process.exit(1); }
    verifyInverse(origData, origIdx, convData, convIdx, maps);
    process.exit(0);
}

// ── self-test ─────────────────────────────────────────────────────────────────
if (opts.selfTest) {
    const dataPath = opts.inputs[0];
    const idxPath  = opts.idx || idxPathFor(dataPath);
    if (!idxPath) { console.error(`Cannot find index for ${dataPath}`); process.exit(1); }
    selfTest(dataPath, idxPath, maps);
    process.exit(0);
}

// ── forward / inverse ─────────────────────────────────────────────────────────
const dataPath = opts.inputs[0];
const idxPath  = opts.idx || idxPathFor(dataPath);
if (!idxPath) { console.error(`Cannot find index file for ${dataPath}`); process.exit(1); }

const activeMaps = opts.inverse ? invertMaps(maps) : maps;
const suffix     = opts.inverse ? '-qcoded' : '-renamed';
const outData    = opts.outData || renamedPath(dataPath, suffix);
const outIdx     = opts.outIdx  || renamedPath(idxPath,  suffix);

if (outData === dataPath || outIdx === idxPath) {
    console.error('Output path equals input — aborting to avoid overwrite.');
    process.exit(1);
}

console.log(`Data:  ${dataPath}  →  ${outData}`);
console.log(`Index: ${idxPath}   →  ${outIdx}`);
console.log(`Mode:  ${opts.inverse ? 'inverse (names → Q-codes)' : 'forward (Q-codes → names)'}\n`);

const { substitutions, byteDelta } = convertFiles(
    dataPath, idxPath, outData, outIdx, activeMaps, opts.verbose
);

const sign = byteDelta >= 0 ? '+' : '';
console.log(`Done — ${substitutions} block(s) modified, ${sign}${byteDelta} bytes delta.`);
