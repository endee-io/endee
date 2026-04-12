const pdf = require("pdf-parse");

async function extractText(buffer) {
  const data = await pdf(buffer);
  return data.text;
}

module.exports = { extractText };
