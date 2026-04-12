"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
const vscode = require("vscode");
const http = require("http");
function activate(context) {
    const provider = new BoxChatViewProvider(context.extensionUri);
    context.subscriptions.push(vscode.window.registerWebviewViewProvider('box-chat', provider));
    let askCommand = vscode.commands.registerCommand('box.ask', async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor)
            return;
        const selection = editor.document.getText(editor.selection) || editor.document.getText();
        provider.sendQuery(selection);
    });
    context.subscriptions.push(askCommand);
}
class BoxChatViewProvider {
    _extensionUri;
    _view;
    constructor(_extensionUri) {
        this._extensionUri = _extensionUri;
    }
    resolveWebviewView(webviewView) {
        this._view = webviewView;
        webviewView.webview.options = { enableScripts: true };
        webviewView.webview.html = this._getHtmlForWebview(webviewView.webview);
        webviewView.webview.onDidReceiveMessage(async (data) => {
            switch (data.type) {
                case 'chat':
                    const response = await this.callBoxAPI('/chat', { query: data.value });
                    this._view?.webview.postMessage({ type: 'response', value: response.response });
                    break;
            }
        });
    }
    sendQuery(text) {
        this._view?.webview.postMessage({ type: 'query', value: text });
    }
    async callBoxAPI(endpoint, data) {
        return new Promise((resolve, reject) => {
            const postData = JSON.stringify(data);
            const options = {
                hostname: 'localhost',
                port: 8000,
                path: endpoint,
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Content-Length': postData.length
                }
            };
            const req = http.request(options, (res) => {
                let body = '';
                res.on('data', (chunk) => body += chunk);
                res.on('end', () => resolve(JSON.parse(body)));
            });
            req.on('error', (e) => reject(e));
            req.write(postData);
            req.end();
        });
    }
    _getHtmlForWebview(webview) {
        return `
            <!DOCTYPE html>
            <html lang="en">
            <head>
                <style>
                    body { font-family: sans-serif; padding: 10px; color: white; background: #0f172a; }
                    input { width: 100%; padding: 8px; margin-top: 10px; background: #1e293b; color: white; border: 1px solid #3b82f6; }
                    #chat { height: 300px; overflow-y: auto; border-bottom: 1px solid #334155; margin-bottom: 10px; }
                    .msg { margin-bottom: 8px; padding: 5px; border-radius: 4px; }
                    .user { background: #334155; }
                    .bot { background: #1e293b; border-left: 3px solid #3b82f6; }
                </style>
            </head>
            <body>
                <h3>Box Autonomous Chat</h3>
                <div id="chat"></div>
                <input type="text" id="input" placeholder="Ask Box...">
                <script>
                    const vscode = acquireVsCodeApi();
                    const chat = document.getElementById('chat');
                    const input = document.getElementById('input');

                    input.addEventListener('keydown', (e) => {
                        if (e.key === 'Enter') {
                            const val = input.value;
                            chat.innerHTML += '<div class="msg user">' + val + '</div>';
                            vscode.postMessage({ type: 'chat', value: val });
                            input.value = '';
                        }
                    });

                    window.addEventListener('message', event => {
                        const message = event.data;
                        if (message.type === 'response') {
                            chat.innerHTML += '<div class="msg bot">' + message.value + '</div>';
                        }
                    });
                </script>
            </body>
            </html>
        `;
    }
}
//# sourceMappingURL=extension.js.map