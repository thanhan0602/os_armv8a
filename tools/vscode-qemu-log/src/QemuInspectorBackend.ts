import * as cp from 'child_process';
import * as net from 'net';

const QEMU_VIRT_RAM_BASE = 0x40000000n;
const QEMU_VIRT_RAM_SIZE = 0x08000000n;
const QEMU_VIRT_RAM_END = QEMU_VIRT_RAM_BASE + QEMU_VIRT_RAM_SIZE;
const PAGE_SIZE = 0x1000n;
const PAGE_COUNT = Number(QEMU_VIRT_RAM_SIZE / PAGE_SIZE);
const KERNEL_VA_OFFSET = 0xffff000000000000n;

const MMU_DESC_VALID = 1n << 0n;
const MMU_DESC_TABLE = MMU_DESC_VALID | (1n << 1n);
const MMU_DESC_BLOCK = MMU_DESC_VALID;
const MMU_DESC_TYPE_MASK = 0x3n;
const MMU_DESC_ADDR_MASK = 0x0000fffffffff000n;
const MMU_L1_BLOCK_ADDR_MASK = 0x0000ffffc0000000n;
const MMU_L2_BLOCK_ADDR_MASK = 0x0000ffffffe00000n;
const MMU_L3_PAGE_ADDR_MASK = 0x0000fffffffff000n;
const MMU_ATTR_INDEX_MASK = 0x1cn;
const MMU_ATTR_DEVICE = 0x0n;
const MMU_ATTR_NORMAL = 0x4n;
const MMU_AP_RO = 0x80n;
const MMU_SH_INNER_SHAREABLE = 0x300n;
const MMU_AF = 0x400n;
const MMU_PXN = 1n << 53n;

const TABLE_NAME_WIDTH = 24;

interface SymbolMap {
    [name: string]: bigint;
}

interface InspectorOptions {
    elfPath: string;
    monitorHost: string;
    monitorPort: number;
    walkAddress?: string;
}

interface PageMappingInfo {
    root: 'ttbr0' | 'ttbr1';
    kind: 'block' | 'page';
    mem: string;
    ap: string;
    exec: string;
    share: string;
    af: string;
    entry: bigint;
    va: bigint;
}

function toHex(value: bigint): string {
    const unsigned = BigInt.asUintN(64, value);
    return `0x${unsigned.toString(16).padStart(16, '0')}`;
}

function readU64LE(buffer: Buffer, offset: number): bigint {
    return buffer.readBigUInt64LE(offset);
}

function decodeCString(buffer: Buffer): string {
    const end = buffer.indexOf(0);
    return buffer.subarray(0, end >= 0 ? end : buffer.length).toString('utf8');
}

function splitLines(text: string): string[] {
    return text.split(/\r?\n/).map((line) => line.trim()).filter((line) => line.length > 0);
}

class HmpClient {
    private readonly socket: net.Socket;
    private readonly pending: Array<{ command: string; resolve: (value: string) => void; reject: (error: Error) => void }>;
    private buffer: string;
    private readyPromise: Promise<void>;
    private readyResolve: (() => void) | undefined;
    private readyReject: ((error: Error) => void) | undefined;

    private constructor(socket: net.Socket) {
        this.socket = socket;
        this.pending = [];
        this.buffer = '';
        this.readyPromise = new Promise<void>((resolve, reject) => {
            this.readyResolve = resolve;
            this.readyReject = reject;
        });

        this.socket.setEncoding('utf8');
        this.socket.on('data', (chunk: string) => this.onData(chunk));
        this.socket.on('error', (error) => {
            if (this.readyReject !== undefined) {
                this.readyReject(error);
                this.readyReject = undefined;
            }

            while (this.pending.length > 0) {
                const request = this.pending.shift();
                request?.reject(error);
            }
        });
        this.socket.on('close', () => {
            const error = new Error('HMP monitor connection closed');
            while (this.pending.length > 0) {
                const request = this.pending.shift();
                request?.reject(error);
            }
        });
    }

    public static async connect(host: string, port: number): Promise<HmpClient> {
        const socket = net.createConnection({ host, port });
        const client = new HmpClient(socket);
        await client.readyPromise;
        return client;
    }

    public async command(command: string): Promise<string> {
        return await new Promise<string>((resolve, reject) => {
            this.pending.push({ command, resolve, reject });
            this.socket.write(command + '\n');
        });
    }

    public close(): void {
        this.socket.end();
        this.socket.destroy();
    }

    private onData(chunk: string): void {
        this.buffer += chunk;
        while (this.buffer.includes('(qemu)')) {
            const promptIndex = this.buffer.indexOf('(qemu)');
            const payload = this.buffer.slice(0, promptIndex);
            this.buffer = this.buffer.slice(promptIndex + '(qemu)'.length);

            if (this.readyResolve !== undefined) {
                this.readyResolve();
                this.readyResolve = undefined;
                this.readyReject = undefined;
                continue;
            }

            const request = this.pending.shift();
            if (request !== undefined) {
                const lines = splitLines(payload);
                const cleaned = lines.length > 0 && lines[0] === request.command ? lines.slice(1).join('\n') : lines.join('\n');
                request.resolve(cleaned);
            }
        }
    }
}

export class QemuInspectorBackend {
    private readonly workspaceFolder: string;
    private symbols: SymbolMap | undefined;

    public constructor(workspaceFolder: string) {
        this.workspaceFolder = workspaceFolder;
    }

    public async loadSnapshot(options: InspectorOptions): Promise<Record<string, unknown>> {
        const symbols = this.loadSymbols(options.elfPath);
        const monitor = await HmpClient.connect(options.monitorHost, options.monitorPort);

        try {
            await monitor.command('stop');
            const status = await monitor.command('info status');

            const mmuEnabled = (await this.readU64Symbol(monitor, symbols, 'mmu_enabled')) !== 0n;
            const ttbr0Root = await this.readU64Symbol(monitor, symbols, 'l0_table');
            const ttbr1Root = await this.readU64Symbol(monitor, symbols, 'l0_table_ttbr1');
            const fineMapChunks = await this.readU64Symbol(monitor, symbols, 'fine_map_chunks_used');
            const managedStart = await this.readU64Symbol(monitor, symbols, 'managed_start');
            const managedEnd = await this.readU64Symbol(monitor, symbols, 'managed_end');
            const totalPages = Number(await this.readU64Symbol(monitor, symbols, 'total_pages'));
            const freePages = Number(await this.readU64Symbol(monitor, symbols, 'free_pages'));
            const reservedBytes = await this.readU64Symbol(monitor, symbols, 'reserved_bytes');
            const invalidFreeCount = await this.readU64Symbol(monitor, symbols, 'invalid_free_count');
            const doubleFreeCount = await this.readU64Symbol(monitor, symbols, 'double_free_count');
            const tableCount = Number(await this.readU64Symbol(monitor, symbols, 'mmu_table_page_count'));
            const tableAddresses = await this.readU64Array(monitor, symbols['mmu_table_page_addresses'], tableCount);
            const tableNames = await this.readFixedStringArray(monitor, symbols['mmu_table_page_names'], tableCount, TABLE_NAME_WIDTH);
            const allocatorStates = await this.readBytes(monitor, symbols['page_state'], PAGE_COUNT);

            const tableNameByPa = new Map<string, string>();
            const tableInventory = tableAddresses.map((address, index) => {
                const pa = toHex(address);
                const name = tableNames[index];
                tableNameByPa.set(pa, name);
                return {
                    index,
                    name,
                    pa,
                };
            });

            const ramMappings = new Array<PageMappingInfo | undefined>(PAGE_COUNT);
            if (mmuEnabled && ttbr1Root !== 0n) {
                await this.populatePhysicalMappings(monitor, ttbr1Root, 'ttbr1', ramMappings);
            } else if (ttbr0Root !== 0n) {
                await this.populatePhysicalMappings(monitor, ttbr0Root, 'ttbr0', ramMappings);
            }

            const pages = allocatorStates.map((state, index) => {
                const pagePa = QEMU_VIRT_RAM_BASE + (BigInt(index) * PAGE_SIZE);
                const mapping = ramMappings[index];
                const tableName = tableNameByPa.get(toHex(pagePa)) ?? null;
                return {
                    index,
                    pa: toHex(pagePa),
                    state: state === 0 ? 'unused' : state === 1 ? 'free' : state === 2 ? 'allocated' : 'unknown',
                    usage: tableName ?? (state === 2 ? 'used' : state === 1 ? 'free' : 'unused'),
                    tableName,
                    mapped: mapping !== undefined,
                    mapping: mapping === undefined ? null : {
                        root: mapping.root,
                        kind: mapping.kind,
                        mem: mapping.mem,
                        ap: mapping.ap,
                        exec: mapping.exec,
                        share: mapping.share,
                        af: mapping.af,
                        va: toHex(mapping.va),
                        entry: toHex(mapping.entry),
                    },
                };
            });

            const walkAddress = options.walkAddress === undefined || options.walkAddress.trim().length === 0
                ? toHex(managedStart + KERNEL_VA_OFFSET)
                : options.walkAddress.trim();

            const translation = await this.walkVirtualAddress(monitor, ttbr0Root, ttbr1Root, walkAddress);

            return {
                status,
                summary: {
                    mmuEnabled,
                    ttbr0Root: toHex(ttbr0Root),
                    ttbr1Root: toHex(ttbr1Root),
                    fineMapChunks: Number(fineMapChunks),
                    managedStart: toHex(managedStart),
                    managedEnd: toHex(managedEnd),
                    totalPages,
                    freePages,
                    usedPages: totalPages - freePages,
                    reservedBytes: Number(reservedBytes),
                    invalidFreeCount: Number(invalidFreeCount),
                    doubleFreeCount: Number(doubleFreeCount),
                    pageSize: Number(PAGE_SIZE),
                    kernelVaOffset: toHex(KERNEL_VA_OFFSET),
                    ramBase: toHex(QEMU_VIRT_RAM_BASE),
                    ramEnd: toHex(QEMU_VIRT_RAM_END),
                    tablePages: tableCount,
                },
                tableInventory,
                translation,
                pages,
            };
        } finally {
            monitor.close();
        }
    }

    public async continueTarget(options: InspectorOptions): Promise<Record<string, unknown>> {
        const monitor = await HmpClient.connect(options.monitorHost, options.monitorPort);

        try {
            await monitor.command('cont');
            const status = await monitor.command('info status');
            return { status };
        } finally {
            monitor.close();
        }
    }

    private loadSymbols(elfPath: string): SymbolMap {
        if (this.symbols !== undefined) {
            return this.symbols;
        }

        const output = cp.execFileSync('aarch64-linux-gnu-nm', ['-a', elfPath], {
            cwd: this.workspaceFolder,
            encoding: 'utf8',
        });

        const symbols: SymbolMap = {};
        for (const line of splitLines(output)) {
            const match = line.match(/^([0-9a-fA-F]+)\s+\w\s+(\S+)$/);
            if (match === null) {
                continue;
            }

            symbols[match[2]] = BigInt(`0x${match[1]}`);
        }

        this.symbols = symbols;
        return symbols;
    }

    private async readU64Symbol(monitor: HmpClient, symbols: SymbolMap, name: string): Promise<bigint> {
        const address = symbols[name];
        if (address === undefined) {
            throw new Error(`Required symbol not found: ${name}`);
        }

        const bytes = await this.readBytes(monitor, address, 8);
        return readU64LE(bytes, 0);
    }

    private async readU64Array(monitor: HmpClient, address: bigint, count: number): Promise<bigint[]> {
        const bytes = await this.readBytes(monitor, address, count * 8);
        const values: bigint[] = [];
        for (let index = 0; index < count; index++) {
            values.push(readU64LE(bytes, index * 8));
        }
        return values;
    }

    private async readFixedStringArray(monitor: HmpClient, address: bigint, count: number, width: number): Promise<string[]> {
        const bytes = await this.readBytes(monitor, address, count * width);
        const values: string[] = [];
        for (let index = 0; index < count; index++) {
            values.push(decodeCString(bytes.subarray(index * width, (index + 1) * width)));
        }
        return values;
    }

    private async readBytes(monitor: HmpClient, address: bigint, length: number): Promise<Buffer> {
        const chunks: Buffer[] = [];
        let remaining = length;
        let current = address;

        while (remaining > 0) {
            const chunkLength = Math.min(remaining, 128);
            const text = await monitor.command(`xp /${chunkLength}bx ${toHex(current)}`);
            const values = [...text.matchAll(/0x([0-9a-fA-F]+)/g)].map((match) => Number.parseInt(match[1], 16) & 0xff);
            if (values.length < chunkLength) {
                throw new Error(`Short memory read at ${toHex(current)}: expected ${chunkLength} bytes, got ${values.length}`);
            }

            chunks.push(Buffer.from(values.slice(0, chunkLength)));
            current += BigInt(chunkLength);
            remaining -= chunkLength;
        }

        return Buffer.concat(chunks);
    }

    private async populatePhysicalMappings(
        monitor: HmpClient,
        rootPa: bigint,
        root: 'ttbr0' | 'ttbr1',
        mappings: Array<PageMappingInfo | undefined>,
    ): Promise<void> {
        const visited = new Set<string>();
        await this.walkTableRecursive(monitor, rootPa, 0, 0n, root, mappings, visited);
    }

    private async walkTableRecursive(
        monitor: HmpClient,
        tablePa: bigint,
        level: number,
        vaBase: bigint,
        root: 'ttbr0' | 'ttbr1',
        mappings: Array<PageMappingInfo | undefined>,
        visited: Set<string>,
    ): Promise<void> {
        const visitKey = `${root}:${level}:${toHex(tablePa)}`;
        if (visited.has(visitKey)) {
            return;
        }
        visited.add(visitKey);

        const table = await this.readBytes(monitor, tablePa, 4096);
        const nextShift = BigInt(39 - (level * 9));
        const nextSpan = 1n << nextShift;

        for (let index = 0; index < 512; index++) {
            const entry = readU64LE(table, index * 8);
            if ((entry & MMU_DESC_VALID) === 0n) {
                continue;
            }

            const entryVaBase = vaBase + (BigInt(index) * nextSpan);
            const entryType = entry & MMU_DESC_TYPE_MASK;

            if (level < 3 && entryType === MMU_DESC_TABLE) {
                await this.walkTableRecursive(monitor, entry & MMU_DESC_ADDR_MASK, level + 1, entryVaBase, root, mappings, visited);
                continue;
            }

            const mapping = this.decodeLeafMapping(level, entry, entryVaBase, root);
            this.fillPhysicalMappings(mapping, mappings);
        }
    }

    private decodeLeafMapping(level: number, entry: bigint, vaBase: bigint, root: 'ttbr0' | 'ttbr1') {
        let paBase: bigint;
        let pageCount: number;
        let kind: 'block' | 'page';

        if (level === 1) {
            paBase = entry & MMU_L1_BLOCK_ADDR_MASK;
            pageCount = 262144;
            kind = 'block';
        } else if (level === 2) {
            paBase = entry & MMU_L2_BLOCK_ADDR_MASK;
            pageCount = 512;
            kind = 'block';
        } else {
            paBase = entry & MMU_L3_PAGE_ADDR_MASK;
            pageCount = 1;
            kind = 'page';
        }

        return {
            root,
            kind,
            paBase,
            vaBase,
            pageCount,
            entry,
            attrs: {
                mem: (entry & MMU_ATTR_INDEX_MASK) === MMU_ATTR_DEVICE ? 'device'
                    : (entry & MMU_ATTR_INDEX_MASK) === MMU_ATTR_NORMAL ? 'normal'
                        : 'unknown',
                ap: (entry & MMU_AP_RO) === MMU_AP_RO ? 'ro' : 'rw',
                exec: (entry & MMU_PXN) !== 0n ? 'nx' : 'x',
                share: (entry & MMU_SH_INNER_SHAREABLE) === MMU_SH_INNER_SHAREABLE ? 'inner' : 'non',
                af: (entry & MMU_AF) !== 0n ? '1' : '0',
            },
        };
    }

    private fillPhysicalMappings(
        mapping: {
            root: 'ttbr0' | 'ttbr1';
            kind: 'block' | 'page';
            paBase: bigint;
            vaBase: bigint;
            pageCount: number;
            entry: bigint;
            attrs: { mem: string; ap: string; exec: string; share: string; af: string };
        },
        mappings: Array<PageMappingInfo | undefined>,
    ): void {
        for (let offset = 0; offset < mapping.pageCount; offset++) {
            const pagePa = mapping.paBase + (BigInt(offset) * PAGE_SIZE);
            if (pagePa < QEMU_VIRT_RAM_BASE || pagePa >= QEMU_VIRT_RAM_END) {
                continue;
            }

            const pageIndex = Number((pagePa - QEMU_VIRT_RAM_BASE) / PAGE_SIZE);
            mappings[pageIndex] = {
                root: mapping.root,
                kind: mapping.kind,
                mem: mapping.attrs.mem,
                ap: mapping.attrs.ap,
                exec: mapping.attrs.exec,
                share: mapping.attrs.share,
                af: mapping.attrs.af,
                entry: mapping.entry,
                va: mapping.vaBase + (BigInt(offset) * PAGE_SIZE),
            };
        }
    }

    private async walkVirtualAddress(
        monitor: HmpClient,
        ttbr0Root: bigint,
        ttbr1Root: bigint,
        input: string,
    ): Promise<Record<string, unknown>> {
        const address = BigInt(input);
        const rootKind = address >= KERNEL_VA_OFFSET && ttbr1Root !== 0n ? 'ttbr1' : 'ttbr0';
        const rootPa = rootKind === 'ttbr1' ? ttbr1Root : ttbr0Root;
        const steps: Array<Record<string, unknown>> = [];

        if (rootPa === 0n) {
            return {
                input: toHex(address),
                root: rootKind,
                rootPa: toHex(rootPa),
                fault: 'root-unavailable',
                steps,
            };
        }

        let tablePa = rootPa;
        const shifts = [39n, 30n, 21n, 12n];
        for (let level = 0; level < shifts.length; level++) {
            const index = Number((address >> shifts[level]) & 0x1ffn);
            const table = await this.readBytes(monitor, tablePa, 4096);
            const entry = readU64LE(table, index * 8);
            const kind = this.entryKind(level, entry);

            const step: Record<string, unknown> = {
                level: `l${level}`,
                index,
                tablePa: toHex(tablePa),
                entry: toHex(entry),
                kind,
            };

            steps.push(step);

            if ((entry & MMU_DESC_VALID) === 0n) {
                return {
                    input: toHex(address),
                    root: rootKind,
                    rootPa: toHex(rootPa),
                    fault: `invalid-l${level}`,
                    steps,
                };
            }

            if (kind === 'table') {
                tablePa = entry & MMU_DESC_ADDR_MASK;
                continue;
            }

            const leaf = this.decodeLeafForWalk(level, address, entry);
            return {
                input: toHex(address),
                root: rootKind,
                rootPa: toHex(rootPa),
                steps,
                leaf,
            };
        }

        return {
            input: toHex(address),
            root: rootKind,
            rootPa: toHex(rootPa),
            fault: 'walk-ended-without-leaf',
            steps,
        };
    }

    private entryKind(level: number, entry: bigint): string {
        if ((entry & MMU_DESC_VALID) === 0n) {
            return 'invalid';
        }

        if (level < 3 && (entry & MMU_DESC_TYPE_MASK) === MMU_DESC_TABLE) {
            return 'table';
        }

        if (level === 3) {
            return 'page';
        }

        return 'block';
    }

    private decodeLeafForWalk(level: number, address: bigint, entry: bigint): Record<string, unknown> {
        let paBase: bigint;

        if (level === 1) {
            paBase = entry & MMU_L1_BLOCK_ADDR_MASK;
            paBase |= address & ((1n << 30n) - 1n);
        } else if (level === 2) {
            paBase = entry & MMU_L2_BLOCK_ADDR_MASK;
            paBase |= address & ((1n << 21n) - 1n);
        } else {
            paBase = entry & MMU_L3_PAGE_ADDR_MASK;
            paBase |= address & (PAGE_SIZE - 1n);
        }

        return {
            pa: toHex(paBase),
            mem: (entry & MMU_ATTR_INDEX_MASK) === MMU_ATTR_DEVICE ? 'device'
                : (entry & MMU_ATTR_INDEX_MASK) === MMU_ATTR_NORMAL ? 'normal'
                    : 'unknown',
            ap: (entry & MMU_AP_RO) === MMU_AP_RO ? 'ro' : 'rw',
            exec: (entry & MMU_PXN) !== 0n ? 'nx' : 'x',
            share: (entry & MMU_SH_INNER_SHAREABLE) === MMU_SH_INNER_SHAREABLE ? 'inner' : 'non',
            af: (entry & MMU_AF) !== 0n ? '1' : '0',
        };
    }
}