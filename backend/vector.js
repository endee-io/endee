const axios = require("axios");
const { getEmbedding } = require("./embedding");

const BASE_URL = "http://localhost:8080";
const COLLECTION = "resumes";

async function createCollection() {
  try {
    await axios.put(`${BASE_URL}/collections/${COLLECTION}`, {
      vector_size: 384,
      distance: "Cosine"
    });
  } catch {}
}

async function add(text) {
  const vector = await getEmbedding(text);

  await axios.post(`${BASE_URL}/collections/${COLLECTION}/points`, {
    points: [{
      id: Date.now(),
      vector,
      payload: { text }
    }]
  });
}

async function search(query) {
  const vector = await getEmbedding(query);

  const res = await axios.post(
    `${BASE_URL}/collections/${COLLECTION}/search`,
    { vector, limit: 3 }
  );

  return res.data.result;
}

module.exports = { add, search, createCollection };
