const axios = require("axios");
const { add, search } = require("./vector");

async function analyze(resume, jobDesc) {
  await add(resume);

  const results = await search(jobDesc);
  const context = results.map(r => r.payload.text).join("\n");

  const prompt = `
Resume:
${resume}

Job Description:
${jobDesc}

Context:
${context}

Give:
- Match %
- Missing skills
- Suggestions
`;

  const response = await axios.post(
    "https://api.openai.com/v1/chat/completions",
    {
      model: "gpt-4o-mini",
      messages: [{ role: "user", content: prompt }]
    },
    {
      headers: {
        Authorization: `Bearer ${process.env.OPENAI_API_KEY}`
      }
    }
  );

  return response.data.choices[0].message.content;
}

module.exports = { analyze };
