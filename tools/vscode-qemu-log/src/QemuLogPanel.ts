import * as vscode from 'vscode';
import * as cp from 'child_process';

interface ColorRule {
    pattern: string;
    color: string;
    background?: string;
    bold?: boolean;
}

export class QemuLogPanel {
    public static currentPanel: QemuLogPanel | undefined;
    private static readonly viewType = 'qemuLogViewer';

    private readonly _panel: vscode.WebviewPanel;
    private readonly _workspaceFolder: string;
    private _qemu: cp.ChildProcess | undefined;
    private _lineBuffer = '';
    private _disposables: vscode.Disposable[] = [];

    // -------------------------------------------------------------------------
    // Public static factory
    // -------------------------------------------------------------------------

    public static createOrShow(context: vscode.ExtensionContext): void {
        const column =
            vscode.window.activeTextEditor?.viewColumn ?? vscode.ViewColumn.One;

        if (QemuLogPanel.currentPanel) {
            QemuLogPanel.currentPanel._panel.reveal(column);
            return;
        }

        const workspaceFolder =
            vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? '';

        const panel = vscode.window.createWebviewPanel(
            QemuLogPanel.viewType,
            'QEMU Log',
            column,
            {
                enableScripts: true,
                retainContextWhenHidden: true,
            },
        );

        QemuLogPanel.currentPanel = new QemuLogPanel(panel, workspaceFolder);
    }

    // -------------------------------------------------------------------------
    // Constructor
    // -------------------------------------------------------------------------

    private constructor(
        panel: vscode.WebviewPanel,
        workspaceFolder: string,
    ) {
        this._panel = panel;
        this._workspaceFolder = workspaceFolder;

        this._panel.onDidDispose(() => this.dispose(), null, this._disposables);

        this._panel.webview.onDidReceiveMessage(
            (msg: { type: string }) => {
                switch (msg.type) {
                    case 'ready':
                        this._sendConfig();
                        this._startQemu();
                        break;
                    case 'restart':
                        this._stopQemu();
                        this._startQemu();
                        break;
                    case 'stop':
                        this._stopQemu();
                        break;
                }
            },
            null,
            this._disposables,
        );

        vscode.workspace.onDidChangeConfiguration(
            (e) => {
                if (e.affectsConfiguration('qemuLog')) {
                    this._sendConfig();
                }
            },
            null,
            this._disposables,
        );

        this._panel.webview.html = this._buildHtml();
    }

    // -------------------------------------------------------------------------
    // Public commands
    // -------------------------------------------------------------------------

    public stop(): void {
        this._stopQemu();
    }

    public restart(): void {
        this._stopQemu();
        this._startQemu();
    }

    // -------------------------------------------------------------------------
    // QEMU process management
    // -------------------------------------------------------------------------

    private _resolveVars(str: string): string {
        return str.replace(/\$\{workspaceFolder\}/g, this._workspaceFolder);
    }

    private _startQemu(): void {
        const cfg = vscode.workspace.getConfiguration('qemuLog');
        const bin = cfg.get<string>(
            'qemuBinary',
            'qemu-system-aarch64',
        );
        const kernel = this._resolveVars(
            cfg.get<string>('kernelImage', '${workspaceFolder}/build/kernel8.img'),
        );
        const extraArgs = cfg.get<string[]>('qemuArgs', [
            '-machine', 'virt,gic-version=2',
            '-cpu', 'max',
            '-nographic',
            '-serial', 'mon:stdio',
        ]);

        const args = [...extraArgs, '-kernel', kernel];

        this._lineBuffer = '';
        this._postStatus(`Starting: ${bin} ${args.join(' ')}`);

        try {
            this._qemu = cp.spawn(bin, args, {
                cwd: this._workspaceFolder,
                stdio: ['ignore', 'pipe', 'pipe'],
            });

            // Line-buffer stdout so partial chunks don't produce half-lines
            this._qemu.stdout?.on('data', (chunk: Buffer) => {
                this._lineBuffer += chunk.toString('utf8');
                const lines = this._lineBuffer.split('\n');
                this._lineBuffer = lines.pop() ?? '';
                for (const line of lines) {
                    this._postLog(line);
                }
            });

            this._qemu.stderr?.on('data', (chunk: Buffer) => {
                for (const line of chunk.toString('utf8').split('\n')) {
                    if (line.trim()) {
                        this._postLog('[qemu-stderr] ' + line);
                    }
                }
            });

            this._qemu.on('close', (code: number | null) => {
                // Flush remaining buffer on close
                if (this._lineBuffer.trim()) {
                    this._postLog(this._lineBuffer);
                    this._lineBuffer = '';
                }
                this._postStatus(`QEMU exited (code ${code ?? '?'})`);
                this._qemu = undefined;
            });

            this._qemu.on('error', (err: Error) => {
                this._postStatus(`QEMU error: ${err.message}`);
                this._qemu = undefined;
            });

            this._postStatus(`QEMU running — ${bin}`);
        } catch (err: unknown) {
            this._postStatus(
                `Failed to start QEMU: ${err instanceof Error ? err.message : String(err)}`,
            );
        }
    }

    private _stopQemu(): void {
        if (this._qemu) {
            this._qemu.kill('SIGTERM');
            this._qemu = undefined;
            this._postStatus('QEMU stopped');
        }
    }

    // -------------------------------------------------------------------------
    // Webview messaging helpers
    // -------------------------------------------------------------------------

    private _sendConfig(): void {
        const cfg = vscode.workspace.getConfiguration('qemuLog');
        this._panel.webview.postMessage({
            type: 'config',
            colorRules: cfg.get<ColorRule[]>('colorRules', []),
            maxLines: cfg.get<number>('maxLines', 20000),
        });
    }

    private _postLog(line: string): void {
        this._panel.webview.postMessage({ type: 'log', line });
    }

    private _postStatus(text: string): void {
        this._panel.webview.postMessage({ type: 'status', text });
    }

    // -------------------------------------------------------------------------
    // Dispose
    // -------------------------------------------------------------------------

    public dispose(): void {
        this._stopQemu();
        QemuLogPanel.currentPanel = undefined;
        this._panel.dispose();
        for (const d of this._disposables) {
            d.dispose();
        }
        this._disposables = [];
    }

    // -------------------------------------------------------------------------
    // Webview HTML
    // -------------------------------------------------------------------------

    private _buildHtml(): string {
        // Template literal — no ${…} interpolations are used inside the HTML/JS
        // sections below; all dollar-brace sequences are intentionally absent.
        return /* html */ `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta http-equiv="Content-Security-Policy"
        content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
  <title>QEMU Log</title>
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

    body {
      font-family: 'Cascadia Code', 'Fira Code', 'Consolas', 'Courier New', monospace;
      font-size: 12px;
      line-height: 1.6;
      background: #1e1e1e;
      color: #d4d4d4;
      display: flex;
      flex-direction: column;
      height: 100vh;
      overflow: hidden;
    }

    /* ── Toolbar ── */
    #toolbar {
      display: flex;
      align-items: center;
      gap: 5px;
      padding: 5px 8px;
      background: #2d2d30;
      border-bottom: 1px solid #3c3c3c;
      flex-shrink: 0;
      flex-wrap: wrap;
    }
    #filter-wrap {
      display: flex;
      align-items: center;
      flex: 1;
      min-width: 160px;
      background: #3c3c3c;
      border: 1px solid #555;
      border-radius: 3px;
      padding: 0 6px;
    }
    #filter-wrap.invalid { border-color: #f44747; }
    #filter {
      flex: 1;
      background: transparent;
      color: #d4d4d4;
      border: none;
      outline: none;
      font-family: inherit;
      font-size: 12px;
      padding: 3px 0;
    }
    #filter-clear {
      background: none;
      border: none;
      color: #666;
      cursor: pointer;
      font-size: 14px;
      line-height: 1;
      padding: 0 2px;
    }
    #filter-clear:hover { color: #d4d4d4; }

    /* ── Buttons ── */
    .btn {
      padding: 3px 8px;
      background: #3c3c3c;
      color: #cccccc;
      border: 1px solid #555;
      border-radius: 3px;
      cursor: pointer;
      font-size: 11px;
      white-space: nowrap;
      user-select: none;
    }
    .btn:hover { background: #4a4a4a; }
    .btn.active { background: #0e639c; border-color: #0e639c; color: #fff; }
    .btn.stop-btn:hover { background: #6e1414; border-color: #f44747; }

    /* ── Status bar ── */
    #info-bar {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-left: auto;
      flex-shrink: 0;
    }
    #count  { color: #858585; font-size: 11px; white-space: nowrap; }
    #status {
      color: #858585;
      font-size: 11px;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      max-width: 340px;
    }

    /* ── Log output ── */
    #log-wrap {
      flex: 1;
      overflow-y: scroll;
      overflow-x: auto;
      padding: 2px 0;
    }
    .line {
      padding: 0 8px;
      white-space: pre;
      min-height: 19px;
    }
    .line:hover { background: rgba(255,255,255,0.04) !important; }
    .line.hidden { display: none; }
    #empty-msg {
      color: #555;
      padding: 14px;
      text-align: center;
      display: none;
    }

    /* ── Legend ── */
    #legend-bar {
      display: flex;
      align-items: center;
      gap: 4px 12px;
      padding: 3px 8px;
      background: #252526;
      border-top: 1px solid #3c3c3c;
      flex-shrink: 0;
      flex-wrap: wrap;
    }
    #legend-bar.collapsed { display: none; }
    .leg-item {
      display: flex;
      align-items: center;
      gap: 4px;
      font-size: 10px;
      color: #858585;
    }
    .leg-dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      flex-shrink: 0;
    }
    #legend-toggle {
      font-size: 10px;
      color: #555;
      cursor: pointer;
      padding: 0 4px;
      user-select: none;
      flex-shrink: 0;
    }
    #legend-toggle:hover { color: #ccc; }
  </style>
</head>
<body>
  <div id="toolbar">
    <div id="filter-wrap">
      <input id="filter" type="text" placeholder="Filter logs (regex)…"
             autocomplete="off" spellcheck="false">
      <button id="filter-clear" title="Clear filter">&#x2715;</button>
    </div>

    <button class="btn" id="btn-clear"   title="Clear all lines">Clear</button>
    <button class="btn" id="btn-pause"   title="Pause / resume new lines">Pause</button>
    <button class="btn" id="btn-wrap"    title="Toggle line wrapping">Wrap</button>
    <button class="btn" id="btn-restart" title="Restart QEMU">Restart</button>
    <button class="btn stop-btn" id="btn-stop" title="Stop QEMU">Stop</button>

    <label style="font-size:11px;display:flex;align-items:center;gap:4px;
                  cursor:pointer;white-space:nowrap;user-select:none;">
      <input type="checkbox" id="auto-scroll" checked> Auto-scroll
    </label>

    <div id="info-bar">
      <span id="count">0 lines</span>
      <span id="status">Waiting…</span>
    </div>

    <span id="legend-toggle">&#x25BC; Legend</span>
  </div>

  <div id="log-wrap">
    <div id="log-output"></div>
    <div id="empty-msg">No lines match the current filter</div>
  </div>

  <div id="legend-bar"></div>

  <script>
    /* ------------------------------------------------------------------ */
    /*  VS Code API                                                         */
    /* ------------------------------------------------------------------ */
    const vscode = acquireVsCodeApi();

    /* ------------------------------------------------------------------ */
    /*  DOM refs                                                            */
    /* ------------------------------------------------------------------ */
    const logOutput    = document.getElementById('log-output');
    const logWrap      = document.getElementById('log-wrap');
    const filterEl     = document.getElementById('filter');
    const filterWrap   = document.getElementById('filter-wrap');
    const statusEl     = document.getElementById('status');
    const countEl      = document.getElementById('count');
    const emptyMsg     = document.getElementById('empty-msg');
    const legendBar    = document.getElementById('legend-bar');
    const autoScroll   = document.getElementById('auto-scroll');
    const btnPause     = document.getElementById('btn-pause');
    const btnWrap      = document.getElementById('btn-wrap');
    const legendToggle = document.getElementById('legend-toggle');

    /* ------------------------------------------------------------------ */
    /*  State                                                               */
    /* ------------------------------------------------------------------ */
    let colorRules   = [];
    let maxLines     = 20000;
    let paused       = false;
    let wrapLines    = false;
    let filterRegex  = null;
    let totalLines   = 0;
    let pending      = [];
    let rafId        = null;
    let legendOpen   = true;

    /* ------------------------------------------------------------------ */
    /*  Utilities                                                           */
    /* ------------------------------------------------------------------ */

    // Strip ANSI CSI escape sequences (colours, cursor moves, etc.)
    function stripAnsi(s) {
      return s
        .replace(/\\x1b\\[[\\d;]*[A-Za-z]/g, '')
        .replace(/\\x1b[()][0-2B]/g, '');
    }

    function matchRule(line) {
      for (const r of colorRules) {
        try {
          if (new RegExp(r.pattern, 'i').test(line)) { return r; }
        } catch (_) { /* skip invalid regex */ }
      }
      return null;
    }

    /* ------------------------------------------------------------------ */
    /*  DOM line building                                                   */
    /* ------------------------------------------------------------------ */

    function makeLine(raw) {
      const text = stripAnsi(raw);
      const el   = document.createElement('div');
      el.className  = 'line';
      el.dataset.t  = text;           // store for filter re-apply
      if (wrapLines) { el.style.whiteSpace = 'pre-wrap'; }

      const rule = matchRule(text);
      if (rule) {
        if (rule.color)      { el.style.color           = rule.color;      }
        if (rule.background) { el.style.backgroundColor = rule.background; }
        if (rule.bold) {
          const b = document.createElement('b');
          b.textContent = text;
          el.appendChild(b);
          applyFilterVisibility(el, text);
          return el;
        }
      }
      el.textContent = text;
      applyFilterVisibility(el, text);
      return el;
    }

    function applyFilterVisibility(el, text) {
      if (filterRegex && !filterRegex.test(text)) {
        el.classList.add('hidden');
      }
    }

    /* ------------------------------------------------------------------ */
    /*  Batched flush (rAF)                                                 */
    /* ------------------------------------------------------------------ */

    function flushPending() {
      rafId = null;
      if (!pending.length) { return; }

      const frag = document.createDocumentFragment();
      for (const line of pending) {
        frag.appendChild(makeLine(line));
        totalLines++;
      }
      pending = [];

      logOutput.appendChild(frag);

      // Drop oldest lines when over the limit
      while (logOutput.childElementCount > maxLines) {
        logOutput.removeChild(logOutput.firstChild);
        totalLines = Math.max(0, totalLines - 1);
      }

      countEl.textContent = totalLines + ' lines';

      if (autoScroll.checked) {
        logWrap.scrollTop = logWrap.scrollHeight;
      }
      syncEmptyMsg();
    }

    function scheduleFlush() {
      if (!rafId) { rafId = requestAnimationFrame(flushPending); }
    }

    /* ------------------------------------------------------------------ */
    /*  Filter                                                              */
    /* ------------------------------------------------------------------ */

    function rebuildFilter() {
      const val = filterEl.value.trim();
      if (!val) {
        filterRegex = null;
        filterWrap.classList.remove('invalid');
      } else {
        try {
          filterRegex = new RegExp(val, 'i');
          filterWrap.classList.remove('invalid');
        } catch (_) {
          filterRegex = null;
          filterWrap.classList.add('invalid');
          return;
        }
      }
      for (const el of logOutput.children) {
        const t = el.dataset.t || el.textContent || '';
        el.classList.toggle('hidden', !!(filterRegex && !filterRegex.test(t)));
      }
      syncEmptyMsg();
    }

    function syncEmptyMsg() {
      if (!logOutput.childElementCount) {
        emptyMsg.style.display = 'none';
        return;
      }
      const anyVisible = Array.from(logOutput.children)
        .some(el => !el.classList.contains('hidden'));
      emptyMsg.style.display = anyVisible ? 'none' : 'block';
    }

    /* ------------------------------------------------------------------ */
    /*  Legend                                                              */
    /* ------------------------------------------------------------------ */

    function buildLegend() {
      legendBar.innerHTML = '';
      for (const r of colorRules) {
        const item = document.createElement('div');
        item.className = 'leg-item';

        const dot = document.createElement('span');
        dot.className = 'leg-dot';
        dot.style.background = r.color || '#888';

        const lbl = document.createElement('span');
        // Make the regex pattern human-readable in the legend
        lbl.textContent = r.pattern.replace(/\\\\/g, '\\');

        item.appendChild(dot);
        item.appendChild(lbl);
        legendBar.appendChild(item);
      }
    }

    /* ------------------------------------------------------------------ */
    /*  Event handlers                                                      */
    /* ------------------------------------------------------------------ */

    filterEl.addEventListener('input', rebuildFilter);

    document.getElementById('filter-clear').addEventListener('click', () => {
      filterEl.value = '';
      rebuildFilter();
      filterEl.focus();
    });

    document.getElementById('btn-clear').addEventListener('click', () => {
      logOutput.innerHTML = '';
      totalLines = 0;
      pending    = [];
      countEl.textContent = '0 lines';
      syncEmptyMsg();
    });

    btnPause.addEventListener('click', () => {
      paused = !paused;
      btnPause.textContent = paused ? 'Resume' : 'Pause';
      btnPause.classList.toggle('active', paused);
      if (!paused) { scheduleFlush(); }
    });

    btnWrap.addEventListener('click', () => {
      wrapLines = !wrapLines;
      btnWrap.classList.toggle('active', wrapLines);
      const ws = wrapLines ? 'pre-wrap' : 'pre';
      for (const el of logOutput.children) { el.style.whiteSpace = ws; }
    });

    document.getElementById('btn-restart').addEventListener('click', () => {
      logOutput.innerHTML = '';
      totalLines = 0;
      pending    = [];
      countEl.textContent = '0 lines';
      vscode.postMessage({ type: 'restart' });
    });

    document.getElementById('btn-stop').addEventListener('click', () => {
      vscode.postMessage({ type: 'stop' });
    });

    legendToggle.addEventListener('click', () => {
      legendOpen = !legendOpen;
      legendBar.classList.toggle('collapsed', !legendOpen);
      legendToggle.innerHTML = (legendOpen ? '&#x25BC;' : '&#x25B6;') + ' Legend';
    });

    /* ------------------------------------------------------------------ */
    /*  Message handler (extension → webview)                              */
    /* ------------------------------------------------------------------ */

    window.addEventListener('message', ({ data }) => {
      switch (data.type) {

        case 'log':
          if (!paused) {
            pending.push(data.line);
            scheduleFlush();
          }
          break;

        case 'status':
          statusEl.textContent = data.text;
          break;

        case 'config':
          colorRules = data.colorRules || [];
          if (data.maxLines) { maxLines = data.maxLines; }
          buildLegend();
          // Re-apply colors to already-rendered lines
          for (const el of logOutput.children) {
            const line = el.dataset.t || el.textContent || '';
            const r = matchRule(line);
            el.style.color           = r ? (r.color      || '') : '';
            el.style.backgroundColor = r ? (r.background || '') : '';
          }
          break;
      }
    });

    /* ------------------------------------------------------------------ */
    /*  Ready signal → extension starts QEMU                               */
    /* ------------------------------------------------------------------ */
    vscode.postMessage({ type: 'ready' });
  </script>
</body>
</html>`;
    }
}
