// Skarn for VS Code -- starts the skarn_lsp language server for .skn files.
//
// The server speaks the Language Server Protocol over stdin/stdout and reports the
// type checker's errors and warnings while a file is edited. Its location comes from
// the `skarn.server.path` setting: an absolute path to skarn_lsp(.exe) -- the Windows release
// zip ships it -- or just `skarn_lsp` when it is on the PATH. Changing the setting takes effect after
// "Developer: Reload Window".

'use strict';

const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function activate(context) {
    const command = vscode.workspace.getConfiguration('skarn').get('server.path') || 'skarn_lsp';
    const serverOptions = { command, transport: TransportKind.stdio };
    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'skarn' }],
    };
    client = new LanguageClient('skarn', 'Skarn Language Server', serverOptions, clientOptions);
    context.subscriptions.push(client);
    client.start().catch((err) => {
        vscode.window.showErrorMessage(
            `Skarn: could not start the language server '${command}' (${err.message || err}). ` +
            'Set "skarn.server.path" to the full path of skarn_lsp.exe from the Skarn release, ' +
            'or put its folder on the PATH.');
    });
}

function deactivate() {
    return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
