"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.deactivate = exports.activate = void 0;

const vscode = require("vscode");
const QemuLogPanel_1 = require("./QemuLogPanel");
const QemuInspectorPanel_1 = require("./QemuInspectorPanel");

function activate(context) {
    context.subscriptions.push(
        vscode.commands.registerCommand("qemuLog.start", () => {
            QemuLogPanel_1.QemuLogPanel.createOrShow(context);
        }),
        vscode.commands.registerCommand("qemuLog.stop", () => {
            QemuLogPanel_1.QemuLogPanel.currentPanel?.stop();
        }),
        vscode.commands.registerCommand("qemuLog.restart", () => {
            QemuLogPanel_1.QemuLogPanel.currentPanel?.restart();
        }),
        vscode.commands.registerCommand("qemuInspector.start", () => {
            QemuInspectorPanel_1.QemuInspectorPanel.createOrShow(context);
        }),
    );
}
exports.activate = activate;

function deactivate() {
    QemuLogPanel_1.QemuLogPanel.currentPanel?.dispose();
    QemuInspectorPanel_1.QemuInspectorPanel.currentPanel?.dispose();
}
exports.deactivate = deactivate;
