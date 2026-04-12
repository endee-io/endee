const { execSync } = require("child_process");

function getEmbedding(text) {
  const output = execSync(`python3 embed.py "${text}"`).toString();
  return JSON.parse(output);
}

module.exports = { getEmbedding };
