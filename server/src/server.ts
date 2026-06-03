import * as fs from 'fs';
import * as path from 'path';
import {
  createConnection,
  TextDocuments,
  ProposedFeatures,
  InitializeParams,
  InitializeResult,
  TextDocumentSyncKind,
  CompletionItem,
  CompletionItemKind,
  TextDocumentPositionParams,
  Hover,
  MarkupKind,
  Definition,
  Location,
  Range,
  Position,
  MarkupContent,
  InsertTextFormat,
  Diagnostic,
  DiagnosticSeverity
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';

// ── Engine function catalog ───────────────────────────────────────────────────

interface EngineFn {
  name: string;
  cSignature?: string;
  signature: string;
  description: string;
  aliases: string[];
}

// exact key → entry (preserves original casing from catalog)
const engineCatalog      = new Map<string, EngineFn>();
// lowercase key → canonical entry (prefers mixed-case names for completion labels)
const engineCatalogLower = new Map<string, EngineFn>();

function loadCatalog(): void {
  const catalogPath = path.join(__dirname, '..', 'data', 'catalog.json');
  try {
    const raw = JSON.parse(fs.readFileSync(catalogPath, 'utf8')) as Record<string, EngineFn>;
    for (const [key, entry] of Object.entries(raw)) {
      engineCatalog.set(key, entry);
      const lk = key.toLowerCase();
      const existing = engineCatalogLower.get(lk);
      const isMixed = entry.name !== entry.name.toLowerCase();
      const existingIsMixed = existing && existing.name !== existing.name.toLowerCase();
      if (!existing || (isMixed && !existingIsMixed)) {
        engineCatalogLower.set(lk, entry);
      }
    }
  } catch (e) {
    connection.console.error(`Failed to load engine catalog: ${e}`);
  }
}

function lookupEngine(word: string): { fn: EngineFn; exact: boolean } | null {
  const exact = engineCatalog.get(word);
  if (exact) return { fn: exact, exact: true };
  const fallback = engineCatalogLower.get(word.toLowerCase());
  if (fallback) return { fn: fallback, exact: false };
  return null;
}

// ── Enum catalog ──────────────────────────────────────────────────────────────

interface EnumEntry {
  name: string;
  value: number;
  hex: string;
}

interface ParamEnumSpec {
  name: string;
  pos: number;
  enum: string;
  bitmask: boolean;
}

interface ParamEnumInfo {
  params: ParamEnumSpec[];
}

// enumCatalog: enumName -> sorted entries
const enumCatalog    = new Map<string, EnumEntry[]>();
// paramEnumCatalog: lowercase fn name -> param enum specs
const paramEnumCatalog = new Map<string, ParamEnumInfo>();
// enumNameLookup: constant name -> { typeName, entry } — for hover and validation
const enumNameLookup = new Map<string, { typeName: string; entry: EnumEntry }>();

function loadEnums(): void {
  try {
    const dataDir     = path.join(__dirname, '..', 'data');
    const generatedPath = path.join(dataDir, 'enums.generated.json');
    const basePath      = path.join(dataDir, 'enums.json');

    // Load base (committed fallback)
    const base = JSON.parse(fs.readFileSync(basePath, 'utf8')) as Record<string, EnumEntry[]>;

    // Merge generated over base if the generated file exists
    let merged: Record<string, EnumEntry[]>;
    if (fs.existsSync(generatedPath)) {
      const generated = JSON.parse(fs.readFileSync(generatedPath, 'utf8')) as Record<string, EnumEntry[]>;
      merged = { ...base, ...generated };
    } else {
      merged = base;
    }

    for (const [name, entries] of Object.entries(merged)) {
      enumCatalog.set(name, entries.slice().sort((a, b) => a.value - b.value));
    }
    // Build reverse lookup: constant name -> type + entry
    for (const [typeName, entries] of enumCatalog) {
      for (const entry of entries) {
        if (!enumNameLookup.has(entry.name)) {
          enumNameLookup.set(entry.name, { typeName, entry });
        }
      }
    }
  } catch (e) {
    connection.console.error(`Failed to load enums: ${e}`);
  }

  try {
    const raw = JSON.parse(fs.readFileSync(
      path.join(__dirname, '..', 'data', 'param-enums.json'), 'utf8'
    )) as Record<string, ParamEnumInfo>;
    for (const [fn, info] of Object.entries(raw)) {
      paramEnumCatalog.set(fn.toLowerCase(), info);
    }
  } catch (e) {
    connection.console.error(`Failed to load param-enums: ${e}`);
  }
}

// ── Ambient variables (always available in any script context) ────────────────
// 'this' refers to the item the script is attached to; it is not trigger-specific.
const AMBIENT_VARS: readonly string[] = ['this'];

// ── Trigger implicit variables ────────────────────────────────────────────────
// Maps trigger name → variables injected by the runtime for that trigger.
// Mirrors the table in symtab.c (excludes 'this' which is always available).

const TRIGGER_VARS: Record<string, string[]> = {
  'use':               ['user'],
  'ooruse':            ['user'],
  'use_on':            ['user', 'usedon', 'target', 'place'],
  'targetobj':         ['user', 'usedon', 'target', 'place'],
  'oortargetobj':      ['user', 'usedon'],
  'objaccess':         ['user', 'usedon'],
  'targetloc':         ['user', 'place', 'objtype'],
  'typeselected':      ['user', 'usedon', 'objtype', 'listindex'],
  'hueselected':       ['user', 'objhue'],
  'give':              ['givenobj', 'giver'],
  'wasgotten':         ['getter'],
  'wasdropped':        ['dropper'],
  'death':             ['attacker', 'corpse'],
  'sawdeath':          ['victim', 'attacker', 'corpse'],
  'pkpost':            ['killee', 'killer'],
  'washit':            ['attacker', 'damamt'],
  'ishitting':         ['victim', 'damamt'],
  'mobishitting':      ['victim'],
  'gotattacked':       ['attacker'],
  'killedtarget':      ['attacker'],
  'speech':            ['speaker', 'arg'],
  'message':           ['sender', 'args'],
  'callback':          ['sender', 'args'],
  'convofunc':         ['talker'],
  'textentry':         ['sender', 'button', 'text'],
  'genericgump':       ['entryList'],
  'equip':             ['equippedon'],
  'unequip':           ['unequippedfrom'],
  'lookedat':          ['looker'],
  'canbuy':            ['buyer'],
  'isstackableon':     ['stackon'],
  'multirecycle':      ['oldtype'],
  'typechange':        ['oldtype'],
  'transaccountcheck': ['target', 'transok'],
  'transresponse':     ['target', 'transok'],
  'enterrange':        ['target'],
  'leaverange':        ['target'],
  'acquiredesire':     ['target'],
  'foundfood':         ['target'],
  'time':              ['target'],
};

// Union of all implicit vars across all triggers plus ambient vars
const IMPLICIT_VAR_SET = new Set<string>(AMBIENT_VARS);
for (const vars of Object.values(TRIGGER_VARS)) {
  for (const v of vars) IMPLICIT_VAR_SET.add(v);
}

// Returns trigger names that inject a given variable
function triggersProvidingVar(varName: string): string[] {
  return Object.entries(TRIGGER_VARS)
    .filter(([k, vs]) => k !== 'all' && vs.includes(varName))
    .map(([k]) => k)
    .sort();
}

// ── Tokenizer ─────────────────────────────────────────────────────────────────
// Hand-written lexer so we can correctly skip strings, handle CRLF, and
// associate doc-comments with the symbol that follows them — without needing
// a Java-based ANTLR code-generation step.

type TokKind =
  | 'type_kw' | 'kw' | 'ident'
  | 'string' | 'number'
  | 'lparen' | 'rparen' | 'lbrace' | 'rbrace' | 'lbracket' | 'rbracket'
  | 'semi' | 'comma'
  | 'line_comment' | 'block_comment'
  | 'other';

interface Tok {
  kind: TokKind;
  text: string;
  line: number;  // 0-based
  col: number;
}

const TYPE_KWS  = new Set(['int','obj','void','string','ustring','float','list','loc','str']);
const OTHER_KWS = new Set([
  'if','else','switch','case','default','break','continue','goto',
  'for','while','return',
  'function','trigger','member','forward','inherits','NULL'
]);

function tokenize(src: string): Tok[] {
  const toks: Tok[] = [];
  let i = 0, line = 0, lineStart = 0;

  while (i < src.length) {
    const col = i - lineStart;
    const ch  = src[i];

    if (ch === '\r') { i++; continue; }
    if (ch === '\n') { line++; lineStart = i + 1; i++; continue; }
    if (ch === ' ' || ch === '\t') { i++; continue; }

    if (src[i] === '/' && src[i + 1] === '/') {
      const s = i;
      while (i < src.length && src[i] !== '\n') i++;
      toks.push({ kind: 'line_comment', text: src.slice(s, i), line, col });
      continue;
    }

    if (src[i] === '/' && src[i + 1] === '*') {
      const s = i, sl = line, sc = col;
      i += 2;
      while (i < src.length && !(src[i] === '*' && src[i + 1] === '/')) {
        if (src[i] === '\n') { line++; lineStart = i + 1; }
        i++;
      }
      i += 2;
      toks.push({ kind: 'block_comment', text: src.slice(s, i), line: sl, col: sc });
      continue;
    }

    if (ch === '"') {
      const s = i, sl = line, sc = col;
      i++;
      while (i < src.length && src[i] !== '"') {
        if (src[i] === '\\') i++;
        if (i < src.length && src[i] === '\n') { line++; lineStart = i + 1; }
        i++;
      }
      i++;
      toks.push({ kind: 'string', text: src.slice(s, i), line: sl, col: sc });
      continue;
    }

    if (ch >= '0' && ch <= '9') {
      const s = i;
      if (src[i + 1] === 'x' || src[i + 1] === 'X') {
        i += 2;
        while (i < src.length && /[0-9A-Fa-f]/.test(src[i])) i++;
      } else {
        while (i < src.length && src[i] >= '0' && src[i] <= '9') i++;
      }
      toks.push({ kind: 'number', text: src.slice(s, i), line, col });
      continue;
    }

    if (/[A-Za-z_]/.test(ch)) {
      const s = i;
      while (i < src.length && /[A-Za-z0-9_]/.test(src[i])) i++;
      const word = src.slice(s, i);
      const kind: TokKind = TYPE_KWS.has(word) ? 'type_kw' : OTHER_KWS.has(word) ? 'kw' : 'ident';
      toks.push({ kind, text: word, line, col });
      continue;
    }

    const punct: Record<string, TokKind> = {
      '(': 'lparen', ')': 'rparen',
      '{': 'lbrace', '}': 'rbrace',
      '[': 'lbracket', ']': 'rbracket',
      ';': 'semi', ',': 'comma'
    };
    if (punct[ch]) { toks.push({ kind: punct[ch], text: ch, line, col }); i++; continue; }

    toks.push({ kind: 'other', text: ch, line, col });
    i++;
  }
  return toks;
}

// ── User-defined symbol extraction ────────────────────────────────────────────

type SymbolKind = 'function' | 'trigger' | 'member' | 'variable' | 'forward' | 'param';

interface UserSymbol {
  name: string;
  kind: SymbolKind;
  type: string;
  line: number;
  col: number;
  params?: string;
  docComment?: string;
}

function parseSymbols(src: string): UserSymbol[] {
  const raw = tokenize(src);
  const symbols: UserSymbol[] = [];

  // Build filtered token list (no block comments) and doc-comment map
  const toks: Tok[] = [];
  const docAt = new Map<number, string>();
  let pendingDoc = '';

  for (const t of raw) {
    if (t.kind === 'block_comment') { pendingDoc = ''; continue; }
    if (t.kind === 'line_comment') {
      const txt = t.text.slice(2).trim();
      pendingDoc = pendingDoc ? `${pendingDoc}\n${txt}` : txt;
      continue;
    }
    if (pendingDoc) { docAt.set(toks.length, pendingDoc); pendingDoc = ''; }
    toks.push(t);
  }

  let i = 0;
  const at  = (n = 0): Tok | undefined => toks[i + n];
  const eat = (): Tok | undefined => toks[i++];

  // Consume balanced parens, return inner content joined
  function eatParens(): string {
    const parts: string[] = [];
    eat(); // (
    let depth = 1;
    while (i < toks.length && depth > 0) {
      const t = at();
      if (!t) break;
      if (t.kind === 'lparen')  depth++;
      if (t.kind === 'rparen') { depth--; if (depth === 0) break; }
      parts.push(t.text);
      eat();
    }
    eat(); // )
    return parts.join(' ');
  }

  // Consume a block and extract local variable declarations from it
  function eatBlockExtractVars(): void {
    eat(); // {
    let depth = 1;
    while (i < toks.length && depth > 0) {
      const t = at();
      if (!t) break;
      if (t.kind === 'lbrace') { depth++; eat(); continue; }
      if (t.kind === 'rbrace') { depth--; eat(); continue; }

      // member TYPE NAME [= ...] ; → script-level member declared inside a function body
      if (t.kind === 'kw' && t.text === 'member') {
        eat(); // 'member'
        if (at()?.kind === 'type_kw') {
          const typeTok = eat()!;
          if (at()?.kind === 'ident') {
            const nameTok = eat()!;
            symbols.push({ name: nameTok.text, kind: 'member', type: typeTok.text,
              line: nameTok.line, col: nameTok.col });
          }
        }
        while (i < toks.length &&
               at()?.kind !== 'semi' && at()?.kind !== 'lbrace' && at()?.kind !== 'rbrace') eat();
        if (at()?.kind === 'semi') eat();
        continue;
      }

      // TYPE NAME [not followed by '('] → local variable
      if (t.kind === 'type_kw') {
        const typeTok = eat()!;
        if (at()?.kind === 'ident') {
          const nameTok = eat()!;
          if (at()?.kind !== 'lparen') {
            symbols.push({ name: nameTok.text, kind: 'variable', type: typeTok.text,
              line: nameTok.line, col: nameTok.col });
          }
        }
        // skip to next ';' (don't cross block boundaries)
        while (i < toks.length &&
               at()?.kind !== 'semi' && at()?.kind !== 'lbrace' && at()?.kind !== 'rbrace') eat();
        if (at()?.kind === 'semi') eat();
        continue;
      }

      eat();
    }
  }

  // Skip a balanced brace block with no extraction (used for non-function blocks)
  function eatBlock(): void {
    eat(); // {
    let depth = 1;
    while (i < toks.length && depth > 0) {
      const t = eat();
      if (!t) break;
      if (t.kind === 'lbrace') depth++;
      if (t.kind === 'rbrace') depth--;
    }
  }

  // Skip forward to and past the next semicolon
  function eatToSemi(): void {
    while (i < toks.length && at()?.kind !== 'semi') eat();
    if (at()?.kind === 'semi') eat();
  }

  // Normalise raw inner-paren text to canonical param string
  function fmtParams(raw: string): string {
    return raw.split(',').map(p => p.trim()).filter(Boolean).join(', ');
  }

  while (i < toks.length) {
    const t = at();
    if (!t) break;
    const doc = docAt.get(i) ?? '';

    // ── forward TYPE NAME ( params ) ;
    if (t.kind === 'kw' && t.text === 'forward') {
      eat();
      if (at()?.kind === 'type_kw') {
        const typeTok = eat()!;
        if (at()?.kind === 'ident') {
          const nameTok = eat()!;
          if (at()?.kind === 'lparen') {
            const params = fmtParams(eatParens());
            if (at()?.kind === 'semi') eat();
            symbols.push({ name: nameTok.text, kind: 'forward', type: typeTok.text,
              line: nameTok.line, col: nameTok.col, params,
              docComment: doc || undefined });
          }
        }
      }
      continue;
    }

    // ── function NAME ( params ) ;   ← typeless forward declaration
    //    function NAME ( params ) {   ← typeless function body (skip, not indexed)
    if (t.kind === 'kw' && t.text === 'function') {
      if (at(1)?.kind === 'ident' && at(2)?.kind === 'lparen') {
        eat(); // 'function'
        const nameTok = eat()!;
        const params = fmtParams(eatParens());
        if (at()?.kind === 'semi') {
          eat();
          symbols.push({ name: nameTok.text, kind: 'forward', type: 'void',
            line: nameTok.line, col: nameTok.col, params,
            docComment: doc || undefined });
        } else {
          while (i < toks.length && at()?.kind !== 'lbrace') eat();
          if (at()?.kind === 'lbrace') eatBlockExtractVars();
        }
        continue;
      }
      // 'function' as optional prefix before a TYPE keyword — eat and let type_kw branch handle it
      eat();
      continue;
    }

    // ── trigger [HEX] NAME [(filter)] { body }
    if (t.kind === 'kw' && t.text === 'trigger') {
      eat();
      if (at()?.kind === 'number') eat();              // optional numeric ID
      const nameTok = (at()?.kind === 'ident' || at()?.kind === 'kw') ? eat()! : undefined;
      if (nameTok) {
        symbols.push({ name: nameTok.text, kind: 'trigger', type: 'trigger',
          line: nameTok.line, col: nameTok.col, docComment: doc || undefined });
      }
      if (at()?.kind === 'lparen') eatParens();        // optional filter expression
      while (i < toks.length && at()?.kind !== 'lbrace') eat();
      if (at()?.kind === 'lbrace') eatBlockExtractVars();
      continue;
    }

    // ── member TYPE NAME [= ...] ;
    if (t.kind === 'kw' && t.text === 'member') {
      eat();
      if (at()?.kind === 'type_kw') {
        const typeTok = eat()!;
        if (at()?.kind === 'ident') {
          const nameTok = eat()!;
          symbols.push({ name: nameTok.text, kind: 'member', type: typeTok.text,
            line: nameTok.line, col: nameTok.col, docComment: doc || undefined });
        }
      }
      eatToSemi();
      continue;
    }

    // ── TYPE NAME ( params ) { body }   ← function definition
    //    TYPE NAME ( params ) ;          ← implicit forward (no keyword)
    //    TYPE NAME [= ...] ;             ← variable declaration
    if (t.kind === 'type_kw') {
      const typeTok = eat()!;
      if (at()?.kind !== 'ident') continue;
      const nameTok = eat()!;

      if (at()?.kind === 'lparen') {
        const params = fmtParams(eatParens());
        // Skip tokens between ')' and the '{' or ';' — handles brace-on-next-line
        while (i < toks.length &&
               at()?.kind !== 'lbrace' && at()?.kind !== 'semi' && at()?.kind !== 'rbrace') {
          eat();
        }
        if (at()?.kind === 'lbrace') {
          symbols.push({ name: nameTok.text, kind: 'function', type: typeTok.text,
            line: nameTok.line, col: nameTok.col, params,
            docComment: doc || undefined });
          for (const p of params.split(',')) {
            const pm = p.trim().match(
              /^(int|obj|void|string|float|list|loc|str)\s+([A-Za-z_][A-Za-z0-9_]*)$/
            );
            if (pm) symbols.push({ name: pm[2], kind: 'param', type: pm[1],
              line: nameTok.line, col: nameTok.col });
          }
          eatBlockExtractVars();
        } else if (at()?.kind === 'semi') {
          eat();
          symbols.push({ name: nameTok.text, kind: 'forward', type: typeTok.text,
            line: nameTok.line, col: nameTok.col, params,
            docComment: doc || undefined });
        }
      } else {
        // Variable declaration (with or without initialiser)
        symbols.push({ name: nameTok.text, kind: 'variable', type: typeTok.text,
          line: nameTok.line, col: nameTok.col, docComment: doc || undefined });
        eatToSemi();
      }
      continue;
    }

    eat(); // skip unrecognised token
  }

  return symbols;
}

function symbolSignature(sym: UserSymbol): string {
  if (sym.kind === 'function' || sym.kind === 'forward') {
    return `${sym.type} ${sym.name}(${sym.params ?? ''})`;
  }
  if (sym.kind === 'trigger') return `trigger ${sym.name}`;
  return `${sym.type} ${sym.name}`;
}

// ── Trigger context tracking ──────────────────────────────────────────────────

interface TriggerContext { name: string; startLine: number; endLine: number; }

function parseTriggerContexts(src: string): TriggerContext[] {
  const toks = tokenize(src);
  const contexts: TriggerContext[] = [];
  let i = 0;
  while (i < toks.length) {
    if (toks[i]?.kind === 'kw' && toks[i].text === 'trigger') {
      i++;
      if (toks[i]?.kind === 'number') i++;
      const nameTok = (toks[i]?.kind === 'ident' || toks[i]?.kind === 'kw') ? toks[i] : null;
      const name = nameTok?.text ?? '';
      i++;
      if (toks[i]?.kind === 'lparen') {
        let d = 1; i++;
        while (i < toks.length && d > 0) {
          if (toks[i].kind === 'lparen') d++;
          if (toks[i].kind === 'rparen') d--;
          i++;
        }
      }
      while (i < toks.length && toks[i].kind !== 'lbrace') i++;
      if (i >= toks.length) break;
      const startLine = toks[i].line;
      i++;
      let depth = 1, endLine = startLine;
      while (i < toks.length && depth > 0) {
        if (toks[i].kind === 'lbrace') depth++;
        if (toks[i].kind === 'rbrace') { depth--; endLine = toks[i].line; }
        i++;
      }
      contexts.push({ name, startLine, endLine });
    } else {
      i++;
    }
  }
  return contexts;
}

function triggerNameAtPos(text: string, pos: Position): string | null {
  for (const ctx of parseTriggerContexts(text)) {
    if (pos.line > ctx.startLine && pos.line <= ctx.endLine) return ctx.name;
  }
  return null;
}

// ── Cursor context ────────────────────────────────────────────────────────────

function braceDepthAt(text: string, pos: Position): number {
  const lines = text.split('\n');
  let end = 0;
  for (let i = 0; i < pos.line; i++) end += (lines[i]?.length ?? 0) + 1;
  end += pos.character;

  let depth = 0, inStr = false, inLine = false;
  for (let i = 0; i < end; i++) {
    const ch = text[i];
    if (ch === '\n')                     { inLine = false; continue; }
    if (inLine)                          continue;
    if (inStr) {
      if (ch === '\\')                   i++;
      else if (ch === '"')               inStr = false;
      continue;
    }
    if (ch === '/' && text[i+1] === '/') { inLine = true; i++; continue; }
    if (ch === '/' && text[i+1] === '*') {
      i += 2;
      while (i < end && !(text[i] === '*' && text[i+1] === '/')) i++;
      i++;
      continue;
    }
    if (ch === '"')  { inStr = true; continue; }
    if (ch === '{')  depth++;
    if (ch === '}')  depth--;
  }
  return depth;
}

type CursorCtx =
  | { kind: 'module_root' }
  | { kind: 'after_trigger_kw' }
  | { kind: 'trigger_body'; triggerName: string }
  | { kind: 'function_body' };

function computeCursorCtx(text: string, pos: Position): CursorCtx {
  if (braceDepthAt(text, pos) === 0) {
    // Check whether last meaningful token was 'trigger'
    const lines = text.split('\n');
    let end = 0;
    for (let i = 0; i < pos.line; i++) end += (lines[i]?.length ?? 0) + 1;
    end += pos.character;
    const prefix = tokenize(text.slice(0, end));
    for (let i = prefix.length - 1; i >= 0; i--) {
      const t = prefix[i];
      if (t.kind === 'line_comment' || t.kind === 'block_comment') continue;
      if (t.kind === 'kw' && t.text === 'trigger') return { kind: 'after_trigger_kw' };
      break;
    }
    return { kind: 'module_root' };
  }
  const trigName = triggerNameAtPos(text, pos);
  if (trigName !== null) return { kind: 'trigger_body', triggerName: trigName };
  return { kind: 'function_body' };
}

// ── Cross-file resolution ─────────────────────────────────────────────────────

function getInheritedScriptPaths(
  text: string, currentUri: string, scriptsDir: string,
  visited: Set<string> = new Set()
): string[] {
  const fileDir  = path.dirname(filePathFromUri(currentUri));
  // Candidate search order: file's own directory first, then scriptsDir (if
  // different), so that decoded and Q-coded script trees each resolve their own
  // parent files regardless of what scriptsDir is configured to.
  const searchDirs = scriptsDir && scriptsDir !== fileDir
    ? [fileDir, scriptsDir]
    : [fileDir];

  const result: string[] = [];
  for (const m of text.matchAll(/\binherits\s+([A-Za-z_][A-Za-z0-9_]*)\s*;/g)) {
    let candidate: string | null = null;
    for (const d of searchDirs) {
      const c = path.join(d, m[1] + '.m');
      if (fs.existsSync(c) && !visited.has(c)) { candidate = c; break; }
    }
    if (!candidate) continue;
    visited.add(candidate);
    result.push(candidate);
    try {
      const parentText = fs.readFileSync(candidate, 'utf8');
      const transitive = getInheritedScriptPaths(
        parentText, uriFromFilePath(candidate), scriptsDir, visited
      );
      result.push(...transitive);
    } catch { /* skip unreadable */ }
  }
  return result;
}

function filePathFromUri(uri: string): string {
  try {
    // Handles file:///path (Unix) and file:///C:/path (Windows)
    const p = decodeURIComponent(new URL(uri).pathname);
    return p.replace(/^\/([A-Za-z]:)/, '$1');
  } catch {
    return decodeURIComponent(uri.replace(/^file:\/\//, ''));
  }
}

function uriFromFilePath(filePath: string): string {
  const normalised = filePath.replace(/\\/g, '/');
  return normalised.startsWith('/') ? `file://${normalised}` : `file:///${normalised}`;
}

// ── LSP connection ────────────────────────────────────────────────────────────

const connection = createConnection(ProposedFeatures.all);
const documents  = new TextDocuments(TextDocument);

let scriptsDirectory = '';

connection.onInitialize((params: InitializeParams): InitializeResult => {
  scriptsDirectory = params.initializationOptions?.scriptsDirectory ?? '';
  loadCatalog();
  loadEnums();

  return {
    capabilities: {
      textDocumentSync: TextDocumentSyncKind.Incremental,
      completionProvider: {
        resolveProvider: false,
        triggerCharacters: ['(', '.']
      },
      hoverProvider: true,
      definitionProvider: true
    }
  };
});

// ── Helpers ───────────────────────────────────────────────────────────────────

function isInString(line: string, pos: number): boolean {
  let inStr = false;
  for (let i = 0; i < pos; i++) {
    if (line[i] === '"' && (i === 0 || line[i - 1] !== '\\')) inStr = !inStr;
  }
  return inStr;
}

function wordAt(text: string, pos: Position): string {
  const lines = text.split('\n');
  const line  = (lines[pos.line] ?? '').replace(/\r$/, '');
  if (isInString(line, pos.character)) return '';
  let s = pos.character, e = pos.character;
  while (s > 0 && /\w/.test(line[s - 1])) s--;
  while (e < line.length && /\w/.test(line[e])) e++;
  return line.slice(s, e);
}

// Detect the engine function name and 0-based parameter index at the given position.
// Walks the raw text backwards from the cursor, tracking paren depth and commas.
function callContextAt(text: string, pos: Position): { fnName: string; paramIndex: number } | null {
  const lines = text.split('\n');
  let offset = 0;
  for (let i = 0; i < pos.line; i++) offset += (lines[i] ?? '').length + 1;
  offset += pos.character;

  let depth = 0;
  let commas = 0;
  let inStr = false;
  let i = offset - 1;

  while (i >= 0) {
    const ch = text[i];
    // Naïve string-skip: if we hit a quote, scan back to its opening quote
    if (ch === '"' && !inStr) { inStr = true; i--; continue; }
    if (inStr) { if (ch === '"') inStr = false; i--; continue; }
    if (ch === ')' || ch === ']') { depth++; i--; continue; }
    if (ch === '(' || ch === '[') {
      if (depth > 0) { depth--; i--; continue; }
      // Found the call-site opening paren at depth 0; look for identifier before it
      let j = i - 1;
      while (j >= 0 && (text[j] === ' ' || text[j] === '\t')) j--;
      if (j >= 0 && /\w/.test(text[j])) {
        let e = j;
        while (e > 0 && /\w/.test(text[e - 1])) e--;
        const fnName = text.slice(e, j + 1);
        return { fnName, paramIndex: commas };
      }
      return null;
    }
    if (ch === ',' && depth === 0) { commas++; }
    i--;
  }
  return null;
}

function enumEntryHover(entry: EnumEntry, enumName: string, bitmask: boolean): MarkupContent {
  const bmNote = bitmask ? ' *(bitmask)*' : '';
  return {
    kind: MarkupKind.Markdown,
    value: `\`${entry.hex}\` — **${entry.name}** (${enumName}${bmNote})`
  };
}

function engineHover(fn: EngineFn): MarkupContent {
  const desc    = fn.description ? `\n\n${fn.description}` : '';
  const aliases = fn.aliases.length ? `\n\n*Aliases: ${fn.aliases.join(', ')}*` : '';
  const cSig    = fn.cSignature ? `\n\n\`\`\`c\n${fn.cSignature}\n\`\`\`` : '';
  return {
    kind: MarkupKind.Markdown,
    value: `\`\`\`wombat\n${fn.signature}\n\`\`\`${cSig}\n\n🔧 *Engine built-in*${desc}${aliases}`
  };
}

function userHover(sym: UserSymbol): MarkupContent {
  const doc = sym.docComment ? `\n\n${sym.docComment}` : '';
  let extra = '';
  if (sym.kind === 'trigger') {
    const vars = TRIGGER_VARS[sym.name] ?? [];
    if (vars.length) {
      extra = `\n\n⚡ Implicit variables: ${vars.map(v => `\`${v}\``).join(', ')}`;
    }
  }
  return {
    kind: MarkupKind.Markdown,
    value: `\`\`\`wombat\n${symbolSignature(sym)}\n\`\`\`${doc}${extra}`
  };
}

function implicitVarHover(varName: string): MarkupContent {
  if (varName === 'this') {
    return {
      kind: MarkupKind.Markdown,
      value: `\`\`\`wombat\nthis\n\`\`\`\n\nThe item this script is attached to. Always available in any script context.`
    };
  }
  const triggers = triggersProvidingVar(varName);
  const provided = triggers.length
    ? `\n\nInjected by: ${triggers.map(t => `\`${t}\``).join(', ')}`
    : '';
  return {
    kind: MarkupKind.Markdown,
    value: `\`\`\`wombat\n${varName}\n\`\`\`\n\n⚡ *Runtime-injected trigger variable*${provided}`
  };
}

// ── Completion ────────────────────────────────────────────────────────────────

const TYPE_KW_LIST  = ['int','obj','void','string','ustring','float','list','loc','str'] as const;
const CTRL_KW_LIST  = ['if','else','switch','case','default','break','continue','goto',
                       'for','while','return','NULL'] as const;

connection.onCompletion((params: TextDocumentPositionParams): CompletionItem[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];

  const text = doc.getText();
  const pos  = params.position;
  const seen = new Set<string>();
  const items: CompletionItem[] = [];

  function add(item: CompletionItem): void {
    if (seen.has(item.label)) return;
    seen.add(item.label);
    items.push(item);
  }

  const cursorCtx = computeCursorCtx(text, pos);

  // ── After 'trigger' keyword → offer trigger type names ─────────────────────
  if (cursorCtx.kind === 'after_trigger_kw') {
    for (const [name, vars] of Object.entries(TRIGGER_VARS)) {
      add({
        label: name,
        kind: CompletionItemKind.Event,
        detail: `trigger ${name}`,
        documentation: {
          kind: MarkupKind.Markdown,
          value: vars.length ? `⚡ Implicit variables: ${vars.map(v => `\`${v}\``).join(', ')}` : '*(no trigger-specific implicit variables)*'
        },
        sortText: '0_' + name
      });
    }
    return items;
  }

  // ── Module root → structural + type keywords + current-file symbols ─────────
  if (cursorCtx.kind === 'module_root') {
    for (const kw of ['member', 'function', 'trigger', 'forward', 'inherits']) {
      add({ label: kw, kind: CompletionItemKind.Keyword, sortText: '0_' + kw });
    }
    for (const kw of TYPE_KW_LIST) {
      add({ label: kw, kind: CompletionItemKind.Keyword, sortText: '1_' + kw });
    }
    for (const sym of parseSymbols(text)) {
      if (sym.kind !== 'function' && sym.kind !== 'forward' &&
          sym.kind !== 'member' && sym.kind !== 'trigger') continue;
      add({
        label: sym.name,
        kind: sym.kind === 'trigger' ? CompletionItemKind.Event :
              sym.kind === 'member'  ? CompletionItemKind.Field :
              CompletionItemKind.Function,
        detail: symbolSignature(sym),
        sortText: '2_' + sym.name
      });
    }
    return items;
  }

  // ── Inside function/trigger body ─────────────────────────────────────────────

  // Enum values for annotated engine function parameters (highest priority)
  const callCtx = callContextAt(text, pos);
  if (callCtx) {
    const info = paramEnumCatalog.get(callCtx.fnName.toLowerCase());
    const spec = info?.params.find(p => p.pos === callCtx.paramIndex);
    if (spec) {
      const entries = enumCatalog.get(spec.enum) ?? [];
      const bmNote  = spec.bitmask ? ' (bitmask)' : '';
      for (const entry of entries) {
        add({
          label: entry.name,
          kind: CompletionItemKind.EnumMember,
          detail: `${entry.hex}${bmNote}`,
          documentation: { kind: MarkupKind.Markdown, value: `**${entry.name}** — \`${entry.hex}\` (${spec.enum}${bmNote})` },
          insertText: entry.name,
          insertTextFormat: InsertTextFormat.PlainText,
          sortText: '0_' + entry.hex.padStart(12, '0')
        });
      }
    }
  }

  // User-defined symbols from current file
  for (const sym of parseSymbols(text)) {
    if (sym.kind === 'trigger') continue;
    add({
      label: sym.name,
      kind: sym.kind === 'function' || sym.kind === 'forward' ? CompletionItemKind.Function :
            sym.kind === 'member' ? CompletionItemKind.Field :
            CompletionItemKind.Variable,
      detail: symbolSignature(sym),
      documentation: sym.docComment ? { kind: MarkupKind.PlainText, value: sym.docComment } : undefined,
      sortText: '1_' + sym.name
    });
  }

  // 'this' is always available in any body context
  add({
    label: 'this',
    kind: CompletionItemKind.Variable,
    detail: 'The item this script is attached to',
    sortText: '1_this'
  });

  // Trigger-specific implicit variables (only when inside a trigger body)
  if (cursorCtx.kind === 'trigger_body') {
    const trigVars = TRIGGER_VARS[cursorCtx.triggerName] ?? [];
    for (const v of trigVars) {
      add({
        label: v,
        kind: CompletionItemKind.Variable,
        detail: `Injected by: ${cursorCtx.triggerName}`,
        documentation: { kind: MarkupKind.Markdown, value: '⚡ *Runtime-injected trigger variable*' },
        sortText: '1_' + v
      });
    }
  }

  // Inherited symbols
  for (const scriptPath of getInheritedScriptPaths(text, params.textDocument.uri, scriptsDirectory)) {
    try {
      const inherited = fs.readFileSync(scriptPath, 'utf8');
      for (const sym of parseSymbols(inherited)) {
        if (sym.kind === 'param' || sym.kind === 'variable' || sym.kind === 'trigger') continue;
        add({
          label: sym.name,
          kind: sym.kind === 'function' || sym.kind === 'forward' ? CompletionItemKind.Function : CompletionItemKind.Field,
          detail: `[${path.basename(scriptPath)}] ${symbolSignature(sym)}`,
          sortText: '2_' + sym.name
        });
      }
    } catch { /* skip unreadable */ }
  }

  // Engine functions
  for (const [, fn] of engineCatalogLower) {
    add({
      label: fn.name,
      kind: CompletionItemKind.Function,
      detail: fn.signature,
      documentation: { kind: MarkupKind.Markdown, value: fn.description || '*Engine built-in*' },
      insertText: fn.name,
      insertTextFormat: InsertTextFormat.PlainText,
      sortText: '3_' + fn.name
    });
  }

  // Type keywords (for local declarations)
  for (const kw of TYPE_KW_LIST) {
    add({ label: kw, kind: CompletionItemKind.Keyword, sortText: '4_' + kw });
  }

  // Control-flow keywords
  for (const kw of CTRL_KW_LIST) {
    add({ label: kw, kind: CompletionItemKind.Keyword, sortText: '5_' + kw });
  }

  // 'member' keyword — usable inside bodies per Wombat spec
  add({ label: 'member', kind: CompletionItemKind.Keyword, sortText: '5_member' });

  return items;
});

// ── Hover ─────────────────────────────────────────────────────────────────────

connection.onHover((params: TextDocumentPositionParams): Hover | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;

  const text = doc.getText();
  const word = wordAt(text, params.position);
  if (!word) return null;

  const syms = parseSymbols(text);

  // If the cursor is on a trigger declaration line, show trigger info — not the
  // engine built-in that happens to share the same name (e.g. "callback").
  const triggerDecl = syms.find(s => s.kind === 'trigger' && s.name === word && s.line === params.position.line);
  if (!triggerDecl) {
    const engineMatch = lookupEngine(word);
    if (engineMatch) return { contents: engineHover(engineMatch.fn) };
  }

  // Params shadow same-named implicit trigger variables — check them first.
  const paramSym = syms.find(s => s.name === word && s.kind === 'param');
  if (paramSym) return { contents: userHover(paramSym) };

  if (IMPLICIT_VAR_SET.has(word)) return { contents: implicitVarHover(word) };

  // Enum constant name hover: e.g. SKILL_ALCHEMY, DIR_NORTH, TIMER_EVENT_CALLBACK
  const enumHit = enumNameLookup.get(word);
  if (enumHit) {
    const ctx = callContextAt(text, params.position);
    let contextNote = '';
    if (ctx) {
      const info = paramEnumCatalog.get(ctx.fnName.toLowerCase());
      const spec = info?.params.find(p => p.pos === ctx.paramIndex);
      if (spec) {
        contextNote = spec.enum === enumHit.typeName
          ? `\n\n✓ *Correct type (\`${spec.enum}\`) for \`${ctx.fnName}\` arg ${ctx.paramIndex}*`
          : `\n\n⚠ *Wrong type: expected \`${spec.enum}\` for \`${ctx.fnName}\` arg ${ctx.paramIndex}*`;
      }
    }
    return {
      contents: {
        kind: MarkupKind.Markdown,
        value: `\`${enumHit.entry.hex}\` — **${enumHit.entry.name}** (${enumHit.typeName})${contextNote}`
      }
    };
  }

  // Enum value hover: word is an integer literal inside an enum-annotated engine call
  if (/^(0x[0-9A-Fa-f]+|[0-9]+)$/.test(word)) {
    const ctx = callContextAt(text, params.position);
    if (ctx) {
      const info = paramEnumCatalog.get(ctx.fnName.toLowerCase());
      const spec = info?.params.find(p => p.pos === ctx.paramIndex);
      if (spec) {
        const entries = enumCatalog.get(spec.enum) ?? [];
        const numVal = word.startsWith('0x') || word.startsWith('0X')
          ? parseInt(word, 16) : parseInt(word, 10);
        const entry = entries.find(e => e.value === numVal);
        if (entry) {
          return { contents: enumEntryHover(entry, spec.enum, spec.bitmask) };
        }
        // Value not in enum — show the enum name + list of valid values
        const enumList = entries.map(e => `\`${e.hex}\` ${e.name}`).join(', ');
        return {
          contents: {
            kind: MarkupKind.Markdown,
            value: `*${spec.enum}* — known values: ${enumList}`
          }
        };
      }
    }
  }

  // Include params — they shadow same-named members from inherited scripts.
  const sym  = syms.find(s => s.name === word);
  if (sym) return { contents: userHover(sym) };

  for (const scriptPath of getInheritedScriptPaths(text, params.textDocument.uri, scriptsDirectory)) {
    try {
      const inherited = fs.readFileSync(scriptPath, 'utf8');
      const isym = parseSymbols(inherited).find(s => s.name === word && s.kind !== 'param');
      if (isym) {
        return {
          contents: {
            kind: MarkupKind.Markdown,
            value: `${userHover(isym).value}\n\n*from \`${path.basename(scriptPath)}\`*`
          }
        };
      }
    } catch { /* skip */ }
  }

  return null;
});

// ── Go to definition ──────────────────────────────────────────────────────────

connection.onDefinition((params: TextDocumentPositionParams): Definition | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;

  const text = doc.getText();
  const word = wordAt(text, params.position);
  if (!word) return null;

  // Check if the cursor is on the module name in an `inherits <module>;` directive.
  const lines    = text.split('\n');
  const lineText = (lines[params.position.line] ?? '').replace(/\r$/, '');
  const inheritsM = lineText.match(/\binherits\s+([A-Za-z_][A-Za-z0-9_]*)\s*;/);
  if (inheritsM && inheritsM[1] === word) {
    const dir = scriptsDirectory || path.dirname(filePathFromUri(params.textDocument.uri));
    const candidate = path.join(dir, word + '.m');
    if (fs.existsSync(candidate)) {
      return Location.create(
        uriFromFilePath(candidate),
        Range.create(Position.create(0, 0), Position.create(0, 0))
      );
    }
  }

  if (lookupEngine(word)) return null;

  const syms = parseSymbols(text);

  // Prefer the actual definition (function/trigger/member) over a forward declaration.
  const primary = syms.find(s => s.name === word &&
    (s.kind === 'function' || s.kind === 'trigger' || s.kind === 'member'))
    ?? syms.find(s => s.name === word && s.kind === 'forward');
  if (primary) {
    return Location.create(
      params.textDocument.uri,
      Range.create(Position.create(primary.line, primary.col), Position.create(primary.line, 999))
    );
  }

  const varSym = syms.find(s => s.name === word &&
    (s.kind === 'variable' || s.kind === 'param'));
  if (varSym) {
    return Location.create(
      params.textDocument.uri,
      Range.create(Position.create(varSym.line, varSym.col), Position.create(varSym.line, 999))
    );
  }

  for (const scriptPath of getInheritedScriptPaths(text, params.textDocument.uri, scriptsDirectory)) {
    try {
      const inherited = fs.readFileSync(scriptPath, 'utf8');
      const found = parseSymbols(inherited).find(s => s.name === word &&
        (s.kind === 'function' || s.kind === 'trigger' || s.kind === 'member' || s.kind === 'forward'));
      if (found) {
        return Location.create(
          uriFromFilePath(scriptPath),
          Range.create(Position.create(found.line, found.col), Position.create(found.line, 999))
        );
      }
    } catch { /* skip */ }
  }

  return null;
});

// ── Diagnostics ───────────────────────────────────────────────────────────────

function validateDocument(doc: TextDocument): void {
  try {
    validateDocumentImpl(doc);
  } catch (e) {
    connection.console.error(`wombat: validateDocument crashed: ${e}`);
    connection.sendDiagnostics({ uri: doc.uri, diagnostics: [] });
  }
}

function validateDocumentImpl(doc: TextDocument): void {
  const text = doc.getText();
  const diagnostics: Diagnostic[] = [];

  // Build valid-name set ──────────────────────────────────────────────────────
  const validNames = new Set<string>(IMPLICIT_VAR_SET);

  // Current-file declared symbols
  for (const s of parseSymbols(text)) validNames.add(s.name);

  // Inherited members and functions
  for (const p of getInheritedScriptPaths(text, doc.uri, scriptsDirectory)) {
    try {
      for (const s of parseSymbols(fs.readFileSync(p, 'utf8'))) {
        if (s.kind !== 'param' && s.kind !== 'variable') validNames.add(s.name);
      }
    } catch { /* skip unreadable */ }
  }

  // Engine (both casings)
  for (const name of engineCatalog.keys())      validNames.add(name);
  for (const name of engineCatalogLower.keys()) validNames.add(name);

  // Enum constant names (e.g. SKILL_ALCHEMY, DIR_NORTH, TIMER_EVENT_CALLBACK)
  for (const name of enumNameLookup.keys()) validNames.add(name);

  const toks = tokenize(text);

  // Goto labels: both definitions (IDENT ':') and targets ('goto' IDENT)
  for (let i = 0; i < toks.length; i++) {
    if (toks[i].kind === 'ident' && toks[i + 1]?.kind === 'other' && toks[i + 1].text === ':') {
      validNames.add(toks[i].text);
    }
    if (toks[i].kind === 'kw' && toks[i].text === 'goto' && toks[i + 1]?.kind === 'ident') {
      validNames.add(toks[i + 1].text);
    }
  }

  // Returns the previous non-comment token before index i, or undefined
  function prevNonComment(i: number): Tok | undefined {
    for (let k = i - 1; k >= 0; k--) {
      const t = toks[k];
      if (t.kind !== 'line_comment' && t.kind !== 'block_comment') return t;
    }
    return undefined;
  }

  // Check each identifier token ───────────────────────────────────────────────
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i];
    if (t.kind !== 'ident') continue;

    // L"..." wide-string prefix
    if (toks[i + 1]?.kind === 'string') continue;

    const prev = prevNonComment(i);

    // Member access (field after '.' or ':')
    if (prev?.kind === 'other' && (prev.text === '.' || prev.text === ':')) continue;

    // Declaration positions — name already in validNames via parseSymbols
    if (prev?.kind === 'type_kw') continue;
    if (prev?.kind === 'kw' && (
        prev.text === 'trigger'  ||
        prev.text === 'inherits' ||
        prev.text === 'goto'     ||
        prev.text === 'member'   ||
        prev.text === 'function' ||
        prev.text === 'forward'
    )) continue;

    // Label definition: IDENT ':'
    if (toks[i + 1]?.kind === 'other' && toks[i + 1].text === ':') continue;

    const name = t.text;
    if (validNames.has(name)) continue;

    // Case-sensitivity warning for engine functions
    const engineMatch = lookupEngine(name);
    if (engineMatch) {
      if (!engineMatch.exact) {
        diagnostics.push({
          severity: DiagnosticSeverity.Warning,
          range: Range.create(Position.create(t.line, t.col), Position.create(t.line, t.col + name.length)),
          message: `'${name}' does not match engine function name — did you mean '${engineMatch.fn.name}'?`,
          source: 'wombat'
        });
      }
      continue;
    }

    diagnostics.push({
      severity: DiagnosticSeverity.Error,
      range: Range.create(Position.create(t.line, t.col), Position.create(t.line, t.col + name.length)),
      message: `Undefined symbol '${name}'`,
      source: 'wombat'
    });
  }

  // ── Enum type-checking pass ──────────────────────────────────────────────────
  // Walk tokens forward with a call-frame stack.
  // When an enum constant is found at an annotated param position, check the type.
  {
    interface FwdFrame { fnName: string; paramIdx: number; depth: number; }
    const stack: FwdFrame[] = [];
    let depth = 0;

    const nextNonComment = (start: number): Tok | undefined => {
      for (let k = start; k < toks.length; k++) {
        const t = toks[k];
        if (t.kind !== 'line_comment' && t.kind !== 'block_comment') return t;
      }
      return undefined;
    };

    for (let i = 0; i < toks.length; i++) {
      const tok = toks[i];
      if (tok.kind === 'line_comment' || tok.kind === 'block_comment') continue;

      if (tok.kind === 'ident') {
        const next = nextNonComment(i + 1);
        if (next?.kind === 'lparen') {
          stack.push({ fnName: tok.text, paramIdx: 0, depth: depth + 1 });
        } else if (enumNameLookup.has(tok.text) && stack.length > 0) {
          const frame = stack[stack.length - 1];
          if (frame.depth === depth) {
            const info = paramEnumCatalog.get(frame.fnName.toLowerCase());
            const spec = info?.params.find(p => p.pos === frame.paramIdx);
            if (spec) {
              const hit = enumNameLookup.get(tok.text)!;
              if (hit.typeName !== spec.enum) {
                diagnostics.push({
                  severity: DiagnosticSeverity.Error,
                  range: Range.create(
                    Position.create(tok.line, tok.col),
                    Position.create(tok.line, tok.col + tok.text.length)
                  ),
                  message: `Enum type mismatch: '${tok.text}' is ${hit.typeName}, expected ${spec.enum}`,
                  source: 'wombat'
                });
              }
            }
          }
        }
      } else if (tok.kind === 'lparen') {
        depth++;
      } else if (tok.kind === 'rparen') {
        if (stack.length > 0 && stack[stack.length - 1].depth === depth) stack.pop();
        depth--;
      } else if (tok.kind === 'comma') {
        if (stack.length > 0 && stack[stack.length - 1].depth === depth) {
          stack[stack.length - 1].paramIdx++;
        }
      }
    }
  }

  connection.sendDiagnostics({ uri: doc.uri, diagnostics });
}

documents.onDidChangeContent(change => validateDocument(change.document));
documents.onDidOpen(e => validateDocument(e.document));

// ── Start ─────────────────────────────────────────────────────────────────────

documents.listen(connection);
connection.listen();
