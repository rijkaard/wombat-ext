import * as fs from 'fs';
import * as path from 'path';
import * as cp from 'child_process';
import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind
} from 'vscode-languageclient/node';

/* ── LSP client ──────────────────────────────────────────────────────────── */

let client: LanguageClient;

function startLanguageServer(context: vscode.ExtensionContext): void {
  const serverModule = context.asAbsolutePath(
    path.join('server', 'out', 'server.js')
  );

  const serverOptions: ServerOptions = {
    run: {
      module: serverModule,
      transport: TransportKind.ipc
    },
    debug: {
      module: serverModule,
      transport: TransportKind.ipc,
      options: { execArgv: ['--nolazy', '--inspect=6009'] }
    }
  };

  const config = vscode.workspace.getConfiguration('wombat');
  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'wombat' }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.m')
    },
    initializationOptions: {
      scriptsDirectory: config.get<string>('scriptsDirectory') || ''
    }
  };

  client = new LanguageClient(
    'wombatLanguageServer',
    'Wombat Language Server',
    serverOptions,
    clientOptions
  );

  client.start();
}

/* ── Config ──────────────────────────────────────────────────────────────── */

const CONFIG_FILENAME = '.wombat-compile.json';

interface WombatConfig {
  destination: string;
}

function configPath(sourceDir: string): string {
  return path.join(sourceDir, CONFIG_FILENAME);
}

function loadConfig(sourceDir: string): WombatConfig | null {
  try {
    return JSON.parse(fs.readFileSync(configPath(sourceDir), 'utf8')) as WombatConfig;
  } catch {
    return null;
  }
}

function saveConfig(sourceDir: string, cfg: WombatConfig): void {
  fs.writeFileSync(configPath(sourceDir), JSON.stringify(cfg, null, 2));
}

/* ── Bytecode detection ──────────────────────────────────────────────────── */

/*
 * All Wombat token variants extracted from UoDemo.exe g_TokenVariants.
 * A file whose first uint16_t (LE) matches one of these is bytecode.
 * Source files start with ASCII text ("trigger", "function" …) whose
 * first bytes are always < 0x80, so the two sets never overlap.
 */
const ALL_VARIANTS: ReadonlySet<number> = new Set([
  0x390C, 0x0F3E, 0x0199, 0x0124, 0x305E, // SM_LPAREN
  0x39B3, 0x2D12, 0x26A6, 0x5D03, 0x1238, // SM_RPAREN
  0x3B25, 0x1E1F, 0x1AD4, 0x7F96, 0x7FF5, // SM_COMMA
  0x0732, 0x0120, 0x5CFD, 0x3E12, 0x3BF6, // SM_SEMI
  0x3A9E, 0x0DDC, 0x5E14, 0x2E40, 0x1CD0, // SM_LBRACE
  0x7EB7, 0x6032, 0x2C3B, 0x15A1, 0x3EF6, // SM_RBRACE
  0x409D, 0x12E1, 0x121F, 0x26CA, 0x3699, // SM_LBRACKET
  0x0902, 0x7BB9, 0x139D, 0x187E, 0x16C5, // SM_RBRACKET
  0x3CD5, 0x13E9, 0x4080, 0x5DB2, 0x33EA, // OP_NOT
  0x23C9, 0x60BF, 0x3CD6, 0x0FBF, 0x2F14, // OP_ADD
  0x047E, 0x368E, 0x2FFF, 0x288F, 0x7DD1, // OP_SUB
  0x261E, 0x5E9D, 0x1916, 0x32E6, 0x401D, // OP_MUL
  0x0384, 0x18D7, 0x0FC9, 0x0E12, 0x2833, // OP_DIV
  0x249E, 0x2B0C, 0x11F4, 0x5DD5, 0x127E, // OP_MOD
  0x0135, 0x07CF, 0x1AF4, 0x0ECC, 0x01D3, // OP_ISEQ
  0x0E90, 0x3A2D, 0x37E6, 0x19D9, 0x252A, // OP_ISNEQ
  0x37E5, 0x1DC0, 0x1481, 0x4087, 0x2B01, // OP_LT
  0x16D4, 0x3A8D, 0x7FBE, 0x0C7B, 0x0C15, // OP_GT
  0x3807, 0x0633, 0x251F, 0x1D18, 0x3492, // OP_LTEQ
  0x19DA, 0x39CE, 0x3BB1, 0x3004, 0x1796, // OP_GTEQ
  0x1F16, 0x182F, 0x2CF7, 0x5ED0, 0x1316, // OP_ASSIGN
  0x5D24, 0x0588, 0x7CFE, 0x2725, 0x0DE5, // OP_LOGAND
  0x13D3, 0x29D8, 0x0A28, 0x09CE, 0x3960, // OP_LOGOR
  0x263D, 0x3B97, 0x4027, 0x138A, 0x282D, // OP_XOR
  0x5CCD, 0x0940, 0x293B, 0x40A5, 0x1D11, // OP_INC
  0x2528, 0x0EA9, 0x3F0B, 0x3087, 0x3F97, // OP_DEC
  0x30F1, 0x3295, 0x01C1, 0x0CE1, 0x3EE9, // TK_INT
  0x3F9A, 0x30A7, 0x2DB5, 0x169A, 0x2FE7, // TK_STRING
  0x10D9, 0x0390, 0x2A38, 0x0728, 0x5C5E, // TK_USTRING
  0x01E1, 0x1030, 0x1BD9, 0x159F, 0x2BA5, // TK_LOC
  0x28E2, 0x2F0C, 0x1289, 0x3382, 0x36C2, // TK_OBJ
  0x26B1, 0x1CDF, 0x27DA, 0x0E29, 0x113E, // TK_LIST
  0x2E39, 0x1D3F, 0x1D5E, 0x1FF1, 0x7E0E, // TK_VOID
  0x06E3, 0x36A1, 0x0C1E, 0x2120, 0x1DCB, // T_OFFSET
  0x12C2, 0x1003, 0x0607, 0x0784, 0x2B0F, // TK_IF
  0x3305, 0x32E7, 0x212C, 0x018E, 0x3308, // TK_ELSE
  0x1EDC, 0x20A8, 0x37BE, 0x01EB, 0x123B, // TK_ENDIF
  0x3106, 0x018C, 0x357E, 0x0A87, 0x5D2B, // TK_WHILE
  0x03FA, 0x0AF0, 0x0786, 0x2332, 0x1295, // TK_ENDWHILE
  0x7DAA, 0x2F0B, 0x1BFC, 0x13F5, 0x1ECA, // TK_FOR
  0x0D9F, 0x388A, 0x15FD, 0x7CB8, 0x1AF6, // TK_ENDFOR
  0x017B, 0x6014, 0x0E99, 0x33CD, 0x27D3, // TK_CONTINUE
  0x7F0D, 0x04F0, 0x183A, 0x1FB4, 0x13A6, // TK_BREAK
  0x190B, 0x3605, 0x20AD, 0x32CF, 0x2CD5, // TK_GOTO
  0x04B0, 0x1927, 0x08FF, 0x31D8, 0x0914, // TK_SWITCH
  0x13F4, 0x3A27, 0x387C, 0x32C1, 0x198C, // TK_ENDSWITCH
  0x3223, 0x17B8, 0x3895, 0x248D, 0x342D, // TK_CASE
  0x5D3D, 0x3260, 0x32DE, 0x2780, 0x31AD, // TK_DEFAULT
  0x5DE9, 0x5EA5, 0x11D5, 0x199F, 0x2F15, // TK_RETURN
  0x0E01, 0x19FE, 0x3821, 0x0B93, 0x0A2F, // TK_FUNCTION
  0x09B3, 0x038F, 0x328A, 0x08AF, 0x5CCA, // TK_TRIGGER
  0x0C95, 0x7CBE, 0x7C27, 0x5D2A, 0x2FA1, // TK_MEMBER
  0x31BE, 0x15B4, 0x07C9, 0x27C0, 0x1B32, // TK_INHERITS
  0x2934, 0x3E09, 0x012C, 0x2CC6, 0x7FA6, // TK_FORWARD
  0x5D17, 0x0A1D, 0x3B29, 0x2BFA, 0x2BB8, // T_STR
  0x17BD, 0x21EB, 0x2015, 0x5DB8, 0x15E1, // T_BYTE
  0x5B60, 0x3D8F, 0x0FF4, 0x275B, 0x3C8A, // T_WORD
  0x188F, 0x5D27, 0x7F5C, 0x01F7, 0x093B, // T_DWORD
  0x3510, 0x0B9B, 0x06E9, 0x3B9E, 0x0B31, // T_ID
  0x2AEA, 0x0860, 0x403E, 0x3925, 0x16F2, // token 0x89
]);

function isBytecode(filePath: string): boolean {
  try {
    const buf = Buffer.alloc(2);
    const fd = fs.openSync(filePath, 'r');
    const bytesRead = fs.readSync(fd, buf, 0, 2, 0);
    fs.closeSync(fd);
    if (bytesRead < 2) return false;
    const v = buf[0] | (buf[1] << 8);
    return ALL_VARIANTS.has(v);
  } catch {
    return false;
  }
}

function isSourceFile(filePath: string): boolean {
  try {
    const buf = Buffer.alloc(32);
    const fd = fs.openSync(filePath, 'r');
    const bytesRead = fs.readSync(fd, buf, 0, 32, 0);
    fs.closeSync(fd);
    if (bytesRead < 2) return true;
    for (let i = 0; i < bytesRead; i++) {
      const b = buf[i];
      if (b > 0x7E && b !== 0x09 && b !== 0x0A && b !== 0x0D) return false;
    }
    return true;
  } catch {
    return true;
  }
}

/* ── Source discovery ────────────────────────────────────────────────────── */

function findSourceFiles(sourceDir: string): string[] {
  return fs.readdirSync(sourceDir)
    .filter(f => f.endsWith('.m'))
    .map(f => path.join(sourceDir, f));
}

function needsCompile(srcPath: string, dstPath: string): boolean {
  try {
    return fs.statSync(srcPath).mtimeMs > fs.statSync(dstPath).mtimeMs;
  } catch {
    return true;
  }
}

/* ── Configuration dialogue ──────────────────────────────────────────────── */

async function promptForDestination(sourceDir: string): Promise<string | undefined> {
  const picked = await vscode.window.showOpenDialog({
    canSelectFiles: false,
    canSelectFolders: true,
    canSelectMany: false,
    openLabel: 'Select bytecode destination directory',
    title: 'Wombat: Choose destination for compiled bytecode'
  });

  if (!picked || picked.length === 0) return undefined;
  const dest = picked[0].fsPath;

  if (path.resolve(dest) === path.resolve(sourceDir)) {
    vscode.window.showErrorMessage(
      'Wombat: Destination must be different from the source directory.'
    );
    return undefined;
  }

  return dest;
}

async function ensureConfig(sourceDir: string): Promise<WombatConfig | undefined> {
  const existing = loadConfig(sourceDir);
  if (existing) return existing;

  const answer = await vscode.window.showInformationMessage(
    `No Wombat compiler config found in ${sourceDir}. Set a destination directory?`,
    'Choose destination', 'Cancel'
  );
  if (answer !== 'Choose destination') return undefined;

  const dest = await promptForDestination(sourceDir);
  if (!dest) return undefined;

  const cfg: WombatConfig = { destination: dest };
  saveConfig(sourceDir, cfg);
  vscode.window.showInformationMessage(
    `Wombat: Config saved to ${CONFIG_FILENAME}. Destination: ${dest}`
  );
  return cfg;
}

/* ── Compilation ─────────────────────────────────────────────────────────── */

interface CompileResult {
  file: string;
  ok: boolean;
  stderr: string;
}

async function compileFile(
  compilerPath: string,
  srcPath: string,
  dstPath: string,
  sdbPath: string
): Promise<CompileResult> {
  return new Promise(resolve => {
    const args = ['-sdb', sdbPath, '-update-sdb', sdbPath, '-o', dstPath, srcPath];
    cp.execFile(compilerPath, args, (err, _stdout, stderr) => {
      resolve({ file: path.basename(srcPath), ok: !err, stderr: stderr.trim() });
    });
  });
}

async function runCompile(sourceDir: string, forceAll: boolean): Promise<void> {
  const cfg = await ensureConfig(sourceDir);
  if (!cfg) return;

  const destDir = cfg.destination;

  if (!fs.existsSync(destDir)) {
    const create = await vscode.window.showWarningMessage(
      `Destination directory does not exist: ${destDir}. Create it?`,
      'Create', 'Cancel'
    );
    if (create !== 'Create') return;
    fs.mkdirSync(destDir, { recursive: true });
  }

  const compilerPath = vscode.workspace.getConfiguration('wombat')
    .get<string>('compilerPath', 'wombat-compiler');

  const sdbPath = path.join(destDir, 'sdb.txt');
  const sources = findSourceFiles(sourceDir);

  if (sources.length === 0) {
    vscode.window.showInformationMessage('Wombat: No .m source files found.');
    return;
  }

  const toCompile: string[] = [];
  for (const src of sources) {
    const dst = path.join(destDir, path.basename(src));
    if (!forceAll && !needsCompile(src, dst)) continue;
    if (fs.existsSync(dst) && isSourceFile(dst)) {
      const overwrite = await vscode.window.showWarningMessage(
        `${path.basename(dst)} in destination looks like a source file, not bytecode. Overwrite?`,
        'Overwrite', 'Skip'
      );
      if (overwrite !== 'Overwrite') continue;
    }
    toCompile.push(src);
  }

  if (toCompile.length === 0) {
    vscode.window.showInformationMessage('Wombat: All scripts are up to date.');
    return;
  }

  await vscode.window.withProgress({
    location: vscode.ProgressLocation.Notification,
    title: `Wombat: Compiling ${toCompile.length} script(s)…`,
    cancellable: false
  }, async progress => {
    const errors: CompileResult[] = [];
    let done = 0;

    for (const src of toCompile) {
      const dst = path.join(destDir, path.basename(src));
      const result = await compileFile(compilerPath, src, dst, sdbPath);
      done++;
      progress.report({
        message: `${done}/${toCompile.length} — ${result.file}`,
        increment: 100 / toCompile.length
      });
      if (!result.ok) errors.push(result);
    }

    if (errors.length === 0) {
      vscode.window.showInformationMessage(
        `Wombat: Compiled ${toCompile.length} script(s) successfully.`
      );
    } else {
      const msg = errors.map(e => `${e.file}: ${e.stderr || 'unknown error'}`).join('\n');
      vscode.window.showErrorMessage(
        `Wombat: ${errors.length} error(s):\n${msg}`,
        { modal: true }
      );
    }
  });
}

async function runCompileFile(srcPath: string): Promise<void> {
  if (!srcPath.endsWith('.m')) {
    vscode.window.showErrorMessage('Wombat: Active file is not a .m script.');
    return;
  }

  const sourceDir = path.dirname(srcPath);
  const cfg = await ensureConfig(sourceDir);
  if (!cfg) return;

  const destDir = cfg.destination;

  if (!fs.existsSync(destDir)) {
    const create = await vscode.window.showWarningMessage(
      `Destination directory does not exist: ${destDir}. Create it?`,
      'Create', 'Cancel'
    );
    if (create !== 'Create') return;
    fs.mkdirSync(destDir, { recursive: true });
  }

  const compilerPath = vscode.workspace.getConfiguration('wombat')
    .get<string>('compilerPath', 'wombat-compiler');
  const sdbPath = path.join(destDir, 'sdb.txt');
  const dstPath = path.join(destDir, path.basename(srcPath));

  if (fs.existsSync(dstPath) && isSourceFile(dstPath)) {
    const overwrite = await vscode.window.showWarningMessage(
      `${path.basename(dstPath)} in destination looks like a source file, not bytecode. Overwrite?`,
      'Overwrite', 'Cancel'
    );
    if (overwrite !== 'Overwrite') return;
  }

  const result = await compileFile(compilerPath, srcPath, dstPath, sdbPath);
  if (result.ok) {
    vscode.window.showInformationMessage(`Wombat: Compiled ${result.file}.`);
  } else {
    vscode.window.showErrorMessage(
      `Wombat: ${result.file}: ${result.stderr || 'unknown error'}`,
      { modal: true }
    );
  }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

function activeSourceDir(): string | undefined {
  const editor = vscode.window.activeTextEditor;
  if (editor) return path.dirname(editor.document.uri.fsPath);

  const folders = vscode.workspace.workspaceFolders;
  if (folders && folders.length > 0) return folders[0].uri.fsPath;

  vscode.window.showErrorMessage('Wombat: No file or workspace folder open.');
  return undefined;
}

/* ── Activation ──────────────────────────────────────────────────────────── */

export function activate(context: vscode.ExtensionContext): void {
  startLanguageServer(context);

  context.subscriptions.push(
    vscode.commands.registerCommand('wombat.compile', async () => {
      const dir = activeSourceDir();
      if (dir) await runCompile(dir, false);
    }),

    vscode.commands.registerCommand('wombat.compileAll', async () => {
      const dir = activeSourceDir();
      if (dir) await runCompile(dir, true);
    }),

    vscode.commands.registerCommand('wombat.compileFile', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        vscode.window.showErrorMessage('Wombat: No file is open.');
        return;
      }
      await runCompileFile(editor.document.uri.fsPath);
    }),

    vscode.commands.registerCommand('wombat.configure', async () => {
      const dir = activeSourceDir();
      if (!dir) return;

      const dest = await promptForDestination(dir);
      if (!dest) return;

      saveConfig(dir, { destination: dest });
      vscode.window.showInformationMessage(`Wombat: Destination set to ${dest}`);
    })
  );
}

export function deactivate(): Thenable<void> | undefined {
  return client?.stop();
}
