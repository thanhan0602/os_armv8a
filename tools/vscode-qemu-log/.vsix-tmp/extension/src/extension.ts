import * as vscode from 'vscode';
import { QemuLogPanel } from './QemuLogPanel';
import { QemuInspectorPanel } from './QemuInspectorPanel';

export function activate(context: vscode.ExtensionContext): void {
    context.subscriptions.push(
        vscode.commands.registerCommand('qemuLog.start', () => {
            QemuLogPanel.createOrShow(context);
        }),
        vscode.commands.registerCommand('qemuLog.stop', () => {
            QemuLogPanel.currentPanel?.stop();
        }),
        vscode.commands.registerCommand('qemuLog.restart', () => {
            QemuLogPanel.currentPanel?.restart();
        }),
        vscode.commands.registerCommand('qemuInspector.start', () => {
            QemuInspectorPanel.createOrShow(context);
        }),
    );
}

export function deactivate(): void {
    QemuLogPanel.currentPanel?.dispose();
    QemuInspectorPanel.currentPanel?.dispose();
}
