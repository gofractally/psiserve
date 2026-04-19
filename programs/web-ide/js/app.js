// UI glue: file list, Monaco editor, toolbar actions, terminal.

import { vfs } from './vfs.js';
import { runWasm } from './runner.js';
import { compile } from './compiler.js';

// ── Monaco editor loader (AMD from CDN) ─────────────────────────────
function loadMonaco() {
   return new Promise((resolve, reject) => {
      // `require` is the AMD loader injected by Monaco's loader.js (in index.html)
      if (typeof require === 'undefined') {
         reject(new Error('Monaco loader missing — check network or CSP'));
         return;
      }
      require(['vs/editor/editor.main'], () => resolve(window.monaco), reject);
   });
}

// ── Terminal helpers ────────────────────────────────────────────────
const termEl = document.getElementById('terminal');
function appendTerm(text, cls) {
   const span = document.createElement('span');
   if (cls) span.className = cls;
   span.textContent = text;
   termEl.appendChild(span);
   termEl.scrollTop = termEl.scrollHeight;
}
const terminal = {
   stdout: text => appendTerm(text, null),
   stderr: text => appendTerm(text, 'ansi-stderr'),
   info:   text => appendTerm(text, 'ansi-info'),
   ok:     text => appendTerm(text, 'ansi-ok'),
   clear:  () => { termEl.textContent = ''; },
};

// ── Stdin line queue ────────────────────────────────────────────────
const stdinQueue = [];
function stdinProvider() { return stdinQueue.shift() ?? null; }

document.getElementById('stdin-form').addEventListener('submit', ev => {
   ev.preventDefault();
   const input = document.getElementById('stdin-input');
   const line = input.value + '\n';
   stdinQueue.push(line);
   appendTerm('› ' + line, 'ansi-info');
   input.value = '';
});

// ── Status pill ─────────────────────────────────────────────────────
const statusEl = document.getElementById('status');
function setStatus(text, kind = '') {
   statusEl.textContent = text;
   statusEl.className = 'status' + (kind ? ' ' + kind : '');
}

// ── File list + editor ──────────────────────────────────────────────
let monaco = null;
let editor = null;
let currentFile = null;
let compiledWasm = null;    // last compiled bytes (ArrayBuffer)
let compiledName = null;    // suggested download name

function languageFor(path) {
   if (path.endsWith('.cc') || path.endsWith('.cpp') || path.endsWith('.cxx')
    || path.endsWith('.h')  || path.endsWith('.hpp') || path.endsWith('.hh'))
      return 'cpp';
   if (path.endsWith('.c'))  return 'c';
   if (path.endsWith('.md')) return 'markdown';
   if (path.endsWith('.json')) return 'json';
   if (path.endsWith('.js'))   return 'javascript';
   return 'plaintext';
}

function renderFileList() {
   const ul = document.getElementById('file-list');
   ul.innerHTML = '';
   for (const { path } of vfs.list()) {
      const li = document.createElement('li');
      li.textContent = path.replace(/\.[^.]+$/, '');
      if (path === currentFile) li.classList.add('active');
      const extMatch = path.match(/\.([^.]+)$/);
      if (extMatch) {
         const ext = document.createElement('span');
         ext.className = 'ext';
         ext.textContent = '.' + extMatch[1];
         li.appendChild(ext);
      }
      li.addEventListener('click', () => openFile(path));
      ul.appendChild(li);
   }
}

function openFile(path) {
   if (currentFile && editor) {
      // Save current buffer back to VFS before switching
      vfs.write(currentFile, editor.getValue());
   }
   currentFile = path;
   if (editor) {
      const content = vfs.read(path) ?? '';
      const model = monaco.editor.createModel(content, languageFor(path));
      const old = editor.getModel();
      editor.setModel(model);
      if (old) old.dispose();
   }
   renderFileList();
   setStatus('editing ' + path);
}

// ── Actions ─────────────────────────────────────────────────────────
async function action_compile() {
   if (!currentFile) { terminal.stderr('no file selected\n'); return; }
   setStatus('compiling…');
   vfs.write(currentFile, editor.getValue());
   // Invalidate the previous artifact so Run won't silently execute stale
   // bytes if compile fails.
   const previousWasm = compiledWasm;
   const previousName = compiledName;
   compiledWasm = null;
   compiledName = null;
   try {
      const result = await compile(currentFile, terminal);
      if (result) {
         compiledWasm = result.bytes;
         compiledName = result.name ?? currentFile.replace(/\.[^.]+$/, '') + '.wasm';
         terminal.ok(`compiled: ${compiledName} (${compiledWasm.byteLength} bytes)\n`);
         setStatus('compiled', 'ok');
      } else {
         // compile() returned null — error already surfaced via terminal.stderr()
         setStatus('compile failed', 'err');
         compiledWasm = previousWasm; compiledName = previousName; // keep previous so Run still works
      }
   } catch (e) {
      // Unexpected crash in the compile path (e.g. toolchain WASM traps,
      // network fetch failure). Surface it clearly in the terminal instead
      // of letting the promise rejection disappear into the browser console.
      terminal.stderr(`\ncompile failed: ${e.message || e}\n`);
      if (e.stack) terminal.stderr(String(e.stack).split('\n').slice(0, 6).join('\n') + '\n');
      setStatus('compile crashed', 'err');
      console.error('compile crashed:', e);
      compiledWasm = previousWasm; compiledName = previousName;
   }
}

async function action_run() {
   if (!compiledWasm) {
      terminal.stderr('no compiled .wasm — click Compile or use Load .wasm\n');
      return;
   }
   setStatus('running…');
   try {
      const r = await runWasm(compiledWasm, { terminal, stdinProvider, args: [compiledName || 'program'] });
      setStatus(r.ok ? 'done' : `exit ${r.exitCode}`, r.ok ? 'ok' : 'err');
   } catch (e) {
      terminal.stderr(`\nrun crashed: ${e.message || e}\n`);
      if (e.stack) terminal.stderr(String(e.stack).split('\n').slice(0, 6).join('\n') + '\n');
      setStatus('run crashed', 'err');
      console.error('run crashed:', e);
   }
}

function action_download() {
   if (!compiledWasm) {
      terminal.stderr('nothing to download — compile first\n');
      return;
   }
   const blob = new Blob([compiledWasm], { type: 'application/wasm' });
   const url = URL.createObjectURL(blob);
   const a = document.createElement('a');
   a.href = url;
   a.download = compiledName || 'program.wasm';
   document.body.appendChild(a);
   a.click();
   a.remove();
   URL.revokeObjectURL(url);
}

async function action_load_wasm(file) {
   const buf = await file.arrayBuffer();
   compiledWasm = buf;
   compiledName = file.name;
   terminal.info(`loaded ${file.name} (${buf.byteLength} bytes)\n`);
   setStatus('loaded ' + file.name, 'ok');
}

function action_new() {
   const name = prompt('new file name:', 'untitled.cc');
   if (!name) return;
   if (vfs.read(name) != null) {
      alert(`file already exists: ${name}`);
      return;
   }
   const template = name.endsWith('.cc') || name.endsWith('.cpp') || name.endsWith('.cxx') || name.endsWith('.c')
      ? `// ${name}\n#include <cstdio>\nint main() {\n    std::printf("hello from ${name}\\n");\n    return 0;\n}\n`
      : '';
   vfs.write(name, template);
   openFile(name);
}

function action_rename() {
   if (!currentFile) return;
   const to = prompt(`rename ${currentFile} to:`, currentFile);
   if (!to || to === currentFile) return;
   if (vfs.read(to) != null) { alert(`file already exists: ${to}`); return; }
   // save current buffer first
   vfs.write(currentFile, editor.getValue());
   vfs.rename(currentFile, to);
   currentFile = to;
   renderFileList();
   setStatus('renamed to ' + to);
}

function action_delete() {
   if (!currentFile) return;
   if (!confirm(`delete ${currentFile}?`)) return;
   const deleted = currentFile;
   vfs.remove(deleted);
   const remaining = vfs.list();
   currentFile = null;
   if (remaining.length) openFile(remaining[0].path);
   else {
      if (editor) editor.setModel(monaco.editor.createModel('', 'plaintext'));
      renderFileList();
   }
   terminal.info(`deleted ${deleted}\n`);
}

// ── Boot ────────────────────────────────────────────────────────────
async function main() {
   // Wire toolbar
   document.getElementById('btn-compile').addEventListener('click', action_compile);
   document.getElementById('btn-run').addEventListener('click', action_run);
   document.getElementById('btn-download').addEventListener('click', action_download);
   document.getElementById('input-wasm').addEventListener('change', ev => {
      const f = ev.target.files?.[0];
      if (f) action_load_wasm(f);
      ev.target.value = ''; // allow re-upload of same filename
   });
   document.getElementById('btn-new').addEventListener('click', action_new);
   document.getElementById('btn-rename').addEventListener('click', action_rename);
   document.getElementById('btn-delete').addEventListener('click', action_delete);
   document.getElementById('btn-clear-term').addEventListener('click', () => terminal.clear());

   // Terminal welcome banner
   terminal.info('wasm-ide — write C/C++, compile to WebAssembly, run in the browser\n');
   terminal.info('──────────────────────────────────────────────────────────────────\n');

   // Try to preload a sample .wasm so Run works before the user compiles/uploads
   try {
      const r = await fetch('samples/hello.wasm');
      if (r.ok) {
         compiledWasm = await r.arrayBuffer();
         compiledName = 'hello.wasm';
         terminal.info(`sample loaded: samples/hello.wasm (${compiledWasm.byteLength} bytes). Hit Run.\n`);
      }
   } catch {
      // samples/ not present — fine, just no demo
   }

   // Load Monaco
   setStatus('loading editor…');
   try {
      monaco = await loadMonaco();
   } catch (e) {
      setStatus('editor failed to load', 'err');
      terminal.stderr(`failed to load Monaco: ${e.message}\n`);
      return;
   }

   // Construct editor
   editor = monaco.editor.create(document.getElementById('editor'), {
      value: '',
      language: 'plaintext',
      theme: 'vs-dark',
      automaticLayout: true,
      minimap: { enabled: false },
      scrollBeyondLastLine: false,
      fontSize: 13,
      fontFamily: "'SF Mono', Menlo, Monaco, Consolas, 'Courier New', monospace",
      lineNumbers: 'on',
      wordWrap: 'off',
      tabSize: 3,
      insertSpaces: true,
   });

   // Auto-save on change (debounced)
   let saveTimer = null;
   editor.onDidChangeModelContent(() => {
      if (!currentFile) return;
      clearTimeout(saveTimer);
      saveTimer = setTimeout(() => {
         if (currentFile) vfs.write(currentFile, editor.getValue());
      }, 250);
   });

   // Initial file selection
   vfs.onChange(renderFileList);
   const files = vfs.list();
   const preferred = files.find(f => f.path.endsWith('.cc') || f.path.endsWith('.cpp'))
                   ?? files[0];
   if (preferred) openFile(preferred.path);
   else renderFileList();

   setStatus('ready', 'ok');

   // Keyboard shortcuts
   document.addEventListener('keydown', ev => {
      // Cmd/Ctrl+B: compile; Cmd/Ctrl+Enter: run
      const cmd = ev.metaKey || ev.ctrlKey;
      if (!cmd) return;
      if (ev.key === 'b') { ev.preventDefault(); action_compile(); }
      else if (ev.key === 'Enter') { ev.preventDefault(); action_run(); }
   });
}

main();
