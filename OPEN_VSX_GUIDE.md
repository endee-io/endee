# Publishing Box to Open VSX

This guide explains how to package and publish the Box VSCode extension to the [Open VSX Registry](https://open-vsx.org/).

## Prerequisites
1.  **Open VSX Account**: Sign up at [open-vsx.org](https://open-vsx.org/).
2.  **Namespace**: Claim a namespace (e.g., your username).
3.  **Access Token**: Generate a personal access token (PAT) in your settings.

## Steps to Publish

### 1. Install Tooling
```bash
npm install -g ovsx vsce
```

### 2. Prepare the Extension
```bash
# Navigate to the extension folder
cd ide/vscode
# Install dependencies
npm install
# Compile TypeScript to JavaScript
npm run compile
```

### 3. Package and Publish
```bash
# Login and Publish
ovsx publish -p <YOUR_ACCESS_TOKEN>
```

Alternatively, you can package to a `.vsix` file and upload manually:
```bash
vsce package
# Result: box-autonomous-agent-1.0.0.vsix
```

## ⚠️ Configuration
Ensure the `publisher` field in `package.json` matches your Open VSX namespace EXACTLY before publishing.
