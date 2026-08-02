import * as cp from 'child_process';
import * as vscode from 'vscode';
import { QemuInspectorBackend } from './QemuInspectorBackend';

export class QemuInspectorPanel {
    public static currentPanel: QemuInspectorPanel | undefined;
    private static readonly viewType = 'qemuInspectorViewer';

    private readonly panel: vscode.WebviewPanel;
    private readonly workspaceFolder: string;
    private readonly backend: QemuInspectorBackend;
    private readonly disposables: vscode.Disposable[] = [];
    private qemu: cp.ChildProcess | undefined;
    private walkAddress = '';

    public static createOrShow(_context: vscode.ExtensionContext): void {
        const column = vscode.window.activeTextEditor?.viewColumn ?? vscode.ViewColumn.One;

        if (QemuInspectorPanel.currentPanel !== undefined) {
            QemuInspectorPanel.currentPanel.panel.reveal(column);
            return;
        }

        const workspaceFolder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? '';
        const panel = vscode.window.createWebviewPanel(
            QemuInspectorPanel.viewType,
            'QEMU Page Inspector',
            column,
            {
                enableScripts: true,
                retainContextWhenHidden: true,
            },
        );

        QemuInspectorPanel.currentPanel = new QemuInspectorPanel(panel, workspaceFolder);
    }

    private constructor(panel: vscode.WebviewPanel, workspaceFolder: string) {
        this.panel = panel;
        this.workspaceFolder = workspaceFolder;
        this.backend = new QemuInspectorBackend(workspaceFolder);

        this.panel.onDidDispose(() => this.dispose(), null, this.disposables);
        this.panel.webview.onDidReceiveMessage((message: { type: string; walkAddress?: string }) => {
            switch (message.type) {
                case 'ready':
                    this.sendConfig();
                    this.postStatus('Inspector ready');
                    if (this.getConfig().get<boolean>('autoStartQemu', true)) {
                        this.startQemu();
                    }
                    break;
                case 'start':
                    this.startQemu();
                    break;
                case 'stop':
                    this.stopQemu();
                    break;
                case 'pause':
                  this.walkAddress = message.walkAddress?.trim() ?? this.walkAddress;
                    void this.refreshSnapshot();
                    break;
                case 'continue':
                    void this.continueQemu();
                    break;
                case 'refresh':
                    this.walkAddress = message.walkAddress?.trim() ?? this.walkAddress;
                    void this.refreshSnapshot();
                    break;
            }
        }, null, this.disposables);

        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration('qemuInspector')) {
                this.sendConfig();
            }
        }, null, this.disposables);

        this.panel.webview.html = this.buildHtml();
    }

    public dispose(): void {
        this.stopQemu();
        QemuInspectorPanel.currentPanel = undefined;
        this.panel.dispose();
        for (const disposable of this.disposables) {
            disposable.dispose();
        }
        this.disposables.length = 0;
    }

    private getConfig(): vscode.WorkspaceConfiguration {
        return vscode.workspace.getConfiguration('qemuInspector');
    }

    private resolveVars(value: string): string {
        return value.replace(/\$\{workspaceFolder\}/g, this.workspaceFolder);
    }

    private sendConfig(): void {
        const config = this.getConfig();
        this.panel.webview.postMessage({
            type: 'config',
            defaults: {
                walkAddress: this.walkAddress.length > 0 ? this.walkAddress : config.get<string>('defaultWalkAddress', ''),
            },
        });
    }

    private startQemu(): void {
        if (this.qemu !== undefined) {
            this.postStatus('QEMU inspector target already running');
            return;
        }

        const config = this.getConfig();
        const qemuBinary = config.get<string>('qemuBinary', 'qemu-system-aarch64');
        const kernelImage = this.resolveVars(config.get<string>('kernelImage', '${workspaceFolder}/build/kernel8.img'));
        const monitorHost = config.get<string>('monitorHost', '127.0.0.1');
        const monitorPort = config.get<number>('monitorPort', 4444);
        const qemuArgs = config.get<string[]>('qemuArgs', [
            '-machine', 'virt,gic-version=3',
            '-cpu', 'max',
            '-nographic',
            '-serial', 'stdio',
        ]);
        const args = [
            ...qemuArgs,
          '-monitor', `tcp:${monitorHost}:${monitorPort},server,nowait`,
            '-kernel', kernelImage,
        ];

        try {
            this.qemu = cp.spawn(qemuBinary, args, {
                cwd: this.workspaceFolder,
                stdio: ['ignore', 'pipe', 'pipe'],
            });

            this.qemu.stdout?.on('data', (chunk: Buffer) => {
                this.postStatus(`UART: ${chunk.toString('utf8').trim()}`);
            });
            this.qemu.stderr?.on('data', (chunk: Buffer) => {
                const text = chunk.toString('utf8').trim();
                if (text.length > 0) {
                    this.postStatus(`QEMU stderr: ${text}`);
                }
            });
            this.qemu.on('close', (code: number | null) => {
                this.postStatus(`QEMU exited (code ${code ?? '?'})`);
                this.qemu = undefined;
            });
            this.qemu.on('error', (error: Error) => {
                this.postStatus(`QEMU error: ${error.message}`);
                this.qemu = undefined;
            });

            this.postStatus(`Started inspector target on ${monitorHost}:${monitorPort}`);
        } catch (error) {
            this.postStatus(`Failed to start QEMU: ${error instanceof Error ? error.message : String(error)}`);
        }
    }

    private stopQemu(): void {
        if (this.qemu !== undefined) {
            this.qemu.kill('SIGTERM');
            this.qemu = undefined;
            this.postStatus('QEMU stopped');
        }
    }

    private async continueQemu(): Promise<void> {
        const config = this.getConfig();
        try {
        const status = await this.backend.continueTarget({
                elfPath: this.resolveVars(config.get<string>('elfPath', '${workspaceFolder}/build/kernel8.elf')),
                monitorHost: config.get<string>('monitorHost', '127.0.0.1'),
                monitorPort: config.get<number>('monitorPort', 4444),
                walkAddress: this.walkAddress,
            });
        this.postStatus(`Target continued (${JSON.stringify(status)})`);
        } catch (error) {
        this.postStatus(`Continue failed: ${error instanceof Error ? error.message : String(error)}`);
        }
    }

    private async refreshSnapshot(): Promise<void> {
        const config = this.getConfig();
        this.postStatus('Refreshing MMU/page snapshot');

        try {
            const snapshot = await this.backend.loadSnapshot({
                elfPath: this.resolveVars(config.get<string>('elfPath', '${workspaceFolder}/build/kernel8.elf')),
                monitorHost: config.get<string>('monitorHost', '127.0.0.1'),
                monitorPort: config.get<number>('monitorPort', 4444),
                walkAddress: this.walkAddress,
            });
            this.panel.webview.postMessage({ type: 'snapshot', snapshot });
            this.postStatus('Snapshot updated');
        } catch (error) {
            this.postStatus(`Snapshot failed: ${error instanceof Error ? error.message : String(error)}`);
        }
    }

    private postStatus(text: string): void {
        this.panel.webview.postMessage({ type: 'status', text });
    }

    private buildHtml(): string {
        return /* html */ `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta http-equiv="Content-Security-Policy"
        content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
  <title>QEMU Page Inspector</title>
  <style>
    :root {
      --bg: #15181d;
      --panel: #1f242b;
      --panel-2: #262d36;
      --line: #39424d;
      --text: #d8dee9;
      --muted: #8c9aaa;
      --accent: #3aa675;
      --warn: #ffb454;
      --danger: #ef6b73;
      --table: #7f6df2;
      --free: #2ea597;
      --used: #e0883e;
      --unused: #4b5563;
      --exec: #86efac;
      --ro: #7dd3fc;
    }

    * { box-sizing: border-box; }
    body {
      margin: 0;
      font: 12px/1.45 'Cascadia Code', 'Fira Code', monospace;
      background: radial-gradient(circle at top left, #233040 0%, var(--bg) 46%);
      color: var(--text);
      height: 100vh;
      overflow: hidden;
      display: grid;
      grid-template-rows: auto auto 1fr;
    }
    button, input {
      font: inherit;
      color: inherit;
    }
    .toolbar {
      display: flex;
      gap: 8px;
      align-items: center;
      padding: 10px 12px;
      border-bottom: 1px solid var(--line);
      background: rgba(16, 18, 22, 0.9);
      flex-wrap: wrap;
    }
    .btn {
      border: 1px solid var(--line);
      background: var(--panel);
      border-radius: 999px;
      padding: 6px 12px;
      cursor: pointer;
    }
    .btn:hover { border-color: var(--accent); }
    .field {
      display: flex;
      gap: 6px;
      align-items: center;
      margin-left: auto;
    }
    .field input {
      min-width: 280px;
      border: 1px solid var(--line);
      background: var(--panel);
      border-radius: 999px;
      padding: 6px 12px;
      outline: none;
    }
    .status {
      padding: 6px 12px;
      border-bottom: 1px solid var(--line);
      color: var(--muted);
      background: rgba(31, 36, 43, 0.8);
    }
    .layout {
      display: grid;
      grid-template-columns: minmax(300px, 420px) 1fr;
      gap: 12px;
      padding: 12px;
      min-height: 0;
    }
    .panel {
      background: linear-gradient(180deg, rgba(38,45,54,0.96), rgba(24,28,34,0.96));
      border: 1px solid var(--line);
      border-radius: 16px;
      overflow: hidden;
      min-height: 0;
      display: flex;
      flex-direction: column;
    }
    .panel h2 {
      margin: 0;
      padding: 12px 14px;
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: #b5c1ce;
      border-bottom: 1px solid var(--line);
      background: rgba(0,0,0,0.12);
    }
    .panel-body {
      padding: 12px 14px;
      overflow: auto;
      min-height: 0;
    }
    .summary-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 8px;
      margin-bottom: 12px;
    }
    .card {
      border: 1px solid var(--line);
      border-radius: 12px;
      padding: 10px;
      background: rgba(255,255,255,0.02);
    }
    .label { color: var(--muted); font-size: 11px; }
    .value { margin-top: 4px; word-break: break-all; }
    .pill-row {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
      margin-bottom: 12px;
    }
    .pill {
      border-radius: 999px;
      padding: 4px 10px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,0.03);
    }
    .legend {
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
      margin-bottom: 12px;
      color: var(--muted);
    }
    .legend span {
      display: inline-flex;
      align-items: center;
      gap: 6px;
    }
    .swatch {
      width: 12px;
      height: 12px;
      border-radius: 3px;
      display: inline-block;
      border: 1px solid rgba(255,255,255,0.2);
    }
    .map-wrap {
      display: grid;
      grid-template-columns: minmax(0, 1fr) 260px;
      gap: 12px;
      min-height: 0;
      height: 100%;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(128, 8px);
      gap: 2px;
      align-content: start;
      padding: 10px;
      background: rgba(0,0,0,0.14);
      border-radius: 12px;
      border: 1px solid var(--line);
      min-height: 0;
      overflow: auto;
    }
    .cell {
      width: 8px;
      height: 8px;
      border-radius: 2px;
      cursor: pointer;
      border: 1px solid transparent;
    }
    .cell.unused { background: var(--unused); }
    .cell.free { background: var(--free); }
    .cell.used { background: var(--used); }
    .cell.table { background: var(--table); }
    .cell.exec { box-shadow: 0 0 0 1px var(--exec) inset; }
    .cell.ro { border-color: var(--ro); }
    .cell.selected { outline: 1px solid #ffffff; transform: scale(1.25); }
    .detail {
      border: 1px solid var(--line);
      border-radius: 12px;
      background: rgba(0,0,0,0.14);
      padding: 12px;
      overflow: auto;
    }
    .detail pre, .walk pre {
      white-space: pre-wrap;
      word-break: break-word;
      color: #cbd5e1;
      margin: 0;
    }
    .walk-step {
      border-left: 2px solid var(--line);
      padding-left: 10px;
      margin-bottom: 8px;
    }
    .table-list {
      display: grid;
      gap: 6px;
      margin-top: 10px;
    }
    .table-item {
      border: 1px solid var(--line);
      border-radius: 10px;
      padding: 8px 10px;
      background: rgba(255,255,255,0.02);
    }
  </style>
</head>
<body>
  <div class="toolbar">
    <button class="btn" id="start">Start QEMU</button>
    <button class="btn" id="pause">Pause + Snapshot</button>
    <button class="btn" id="continue">Continue</button>
    <button class="btn" id="refresh">Refresh Snapshot</button>
    <button class="btn" id="stop">Stop QEMU</button>
    <div class="field">
      <label for="walk">VA</label>
      <input id="walk" placeholder="0xffff000040097000" />
    </div>
  </div>
  <div class="status" id="status">Waiting for inspector</div>
  <div class="layout">
    <div class="panel">
      <h2>Translation Snapshot</h2>
      <div class="panel-body">
        <div class="summary-grid" id="summary"></div>
        <div class="pill-row" id="pills"></div>
        <div class="panel walk">
          <h2>VA to PA Walk</h2>
          <div class="panel-body" id="walk-body"></div>
        </div>
        <div class="panel" style="margin-top: 12px;">
          <h2>Table Inventory</h2>
          <div class="panel-body table-list" id="tables"></div>
        </div>
      </div>
    </div>
    <div class="panel">
      <h2>Physical Page Map</h2>
      <div class="panel-body" style="min-height: 0; display: flex; flex-direction: column;">
        <div class="legend">
          <span><i class="swatch" style="background: var(--unused)"></i>unused</span>
          <span><i class="swatch" style="background: var(--free)"></i>free</span>
          <span><i class="swatch" style="background: var(--used)"></i>allocated</span>
          <span><i class="swatch" style="background: var(--table)"></i>page table</span>
          <span><i class="swatch" style="background: transparent; border-color: var(--ro)"></i>read-only</span>
          <span><i class="swatch" style="background: transparent; box-shadow: 0 0 0 1px var(--exec) inset"></i>executable</span>
        </div>
        <div class="map-wrap">
          <div class="grid" id="grid"></div>
          <div class="detail" id="detail">Select a page cell to inspect allocator state and mapping attributes.</div>
        </div>
      </div>
    </div>
  </div>

  <script>
    const vscode = acquireVsCodeApi();
    const statusEl = document.getElementById('status');
    const summaryEl = document.getElementById('summary');
    const pillsEl = document.getElementById('pills');
    const walkBodyEl = document.getElementById('walk-body');
    const gridEl = document.getElementById('grid');
    const detailEl = document.getElementById('detail');
    const tablesEl = document.getElementById('tables');
    const walkInputEl = document.getElementById('walk');

    let selectedIndex = -1;
    let currentSnapshot = null;

    function setStatus(text) {
      statusEl.textContent = text;
    }

    function card(label, value) {
      const div = document.createElement('div');
      div.className = 'card';
      div.innerHTML = '<div class="label"></div><div class="value"></div>';
      div.querySelector('.label').textContent = label;
      div.querySelector('.value').textContent = String(value);
      return div;
    }

    function renderSummary(snapshot) {
      const summary = snapshot.summary;
      summaryEl.replaceChildren(
        card('TTBR0 root', summary.ttbr0Root),
        card('TTBR1 root', summary.ttbr1Root),
        card('Managed start', summary.managedStart),
        card('Managed end', summary.managedEnd),
        card('Free pages', summary.freePages),
        card('Used pages', summary.usedPages),
        card('Reserved bytes', summary.reservedBytes),
        card('Table pages', summary.tablePages),
      );

      pillsEl.replaceChildren(...[
        `MMU ${summary.mmuEnabled ? 'enabled' : 'disabled'}`,
        `Fine chunks ${summary.fineMapChunks}`,
        `Kernel VA offset ${summary.kernelVaOffset}`,
        `Invalid free ${summary.invalidFreeCount}`,
        `Double free ${summary.doubleFreeCount}`,
      ].map((text) => {
        const span = document.createElement('span');
        span.className = 'pill';
        span.textContent = text;
        return span;
      }));
    }

    function renderWalk(snapshot) {
      walkBodyEl.innerHTML = '';
      const translation = snapshot.translation;
      const header = document.createElement('div');
      header.className = 'card';
      header.innerHTML = '<div class="label"></div><div class="value"></div>';
      header.querySelector('.label').textContent = `input ${translation.input} via ${translation.root}`;
      header.querySelector('.value').textContent = translation.fault
        ? `fault=${translation.fault}`
        : `pa=${translation.leaf.pa} mem=${translation.leaf.mem} ap=${translation.leaf.ap} exec=${translation.leaf.exec}`;
      walkBodyEl.appendChild(header);

      translation.steps.forEach((step) => {
        const div = document.createElement('div');
        div.className = 'walk-step';
        div.innerHTML = `<pre>${step.level} index=${step.index}\nentry=${step.entry}\ntable=${step.tablePa}\nkind=${step.kind}</pre>`;
        walkBodyEl.appendChild(div);
      });
    }

    function renderTables(snapshot) {
      tablesEl.innerHTML = '';
      snapshot.tableInventory.forEach((item) => {
        const div = document.createElement('div');
        div.className = 'table-item';
        div.innerHTML = `<div>${item.name}</div><div class="label">${item.pa}</div>`;
        tablesEl.appendChild(div);
      });
    }

    function pageClasses(page, index) {
      const classes = ['cell'];
      if (page.tableName) {
        classes.push('table');
      } else if (page.state === 'free') {
        classes.push('free');
      } else if (page.state === 'allocated') {
        classes.push('used');
      } else {
        classes.push('unused');
      }
      if (page.mapping && page.mapping.exec === 'x') {
        classes.push('exec');
      }
      if (page.mapping && page.mapping.ap === 'ro') {
        classes.push('ro');
      }
      if (index === selectedIndex) {
        classes.push('selected');
      }
      return classes.join(' ');
    }

    function renderDetail(page) {
      if (!page) {
        detailEl.textContent = 'Select a page cell to inspect allocator state and mapping attributes.';
        return;
      }

      const mapping = page.mapping;
      detailEl.innerHTML = `<pre>page index=${page.index}
physical=${page.pa}
allocator=${page.state}
usage=${page.usage}
table=${page.tableName ?? '-'}
mapped=${page.mapped}
${mapping ? `va=${mapping.va}
root=${mapping.root}
kind=${mapping.kind}
mem=${mapping.mem}
ap=${mapping.ap}
exec=${mapping.exec}
share=${mapping.share}
af=${mapping.af}
entry=${mapping.entry}` : 'mapping=-'}</pre>`;
    }

    function renderGrid(snapshot) {
      currentSnapshot = snapshot;
      const fragment = document.createDocumentFragment();
      snapshot.pages.forEach((page, index) => {
        const cell = document.createElement('button');
        cell.className = pageClasses(page, index);
        cell.title = `${page.pa} ${page.state}${page.tableName ? ` ${page.tableName}` : ''}`;
        cell.addEventListener('click', () => {
          selectedIndex = index;
          renderGrid(currentSnapshot);
          renderDetail(page);
        });
        fragment.appendChild(cell);
      });

      gridEl.innerHTML = '';
      gridEl.appendChild(fragment);
      renderDetail(snapshot.pages[selectedIndex] ?? null);
    }

    function renderSnapshot(snapshot) {
      renderSummary(snapshot);
      renderWalk(snapshot);
      renderTables(snapshot);
      renderGrid(snapshot);
    }

    document.getElementById('start').addEventListener('click', () => vscode.postMessage({ type: 'start' }));
    document.getElementById('pause').addEventListener('click', () => vscode.postMessage({ type: 'pause', walkAddress: walkInputEl.value }));
    document.getElementById('continue').addEventListener('click', () => vscode.postMessage({ type: 'continue' }));
    document.getElementById('refresh').addEventListener('click', () => vscode.postMessage({ type: 'refresh', walkAddress: walkInputEl.value }));
    document.getElementById('stop').addEventListener('click', () => vscode.postMessage({ type: 'stop' }));

    window.addEventListener('message', (event) => {
      const message = event.data;
      if (message.type === 'status') {
        setStatus(message.text);
      } else if (message.type === 'config') {
        walkInputEl.value = message.defaults.walkAddress || '';
      } else if (message.type === 'snapshot') {
        renderSnapshot(message.snapshot);
      }
    });

    vscode.postMessage({ type: 'ready' });
  </script>
</body>
</html>`;
    }
}