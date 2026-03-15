import express from 'express';
import cors from 'cors';
import multer from 'multer';
import pdfParse from 'pdf-parse';

const app = express();
const upload = multer({ storage: multer.memoryStorage() });
const PORT = 8000;

app.use(cors());
app.use(express.json({ limit: '10mb' }));

const documents = new Map();

function chunkText(text, chunkSize = 220, overlap = 40) {
  const words = text.replace(/\s+/g, ' ').trim().split(' ').filter(Boolean);
  if (!words.length) return [];

  const chunks = [];
  const step = Math.max(1, chunkSize - overlap);

  for (let start = 0; start < words.length; start += step) {
    const end = Math.min(words.length, start + chunkSize);
    const chunkWords = words.slice(start, end);
    if (!chunkWords.length) break;
    chunks.push({
      text: chunkWords.join(' '),
      startWord: start,
      endWord: end,
    });
    if (end >= words.length) break;
  }

  return chunks;
}

function tokenize(text) {
  return (text.toLowerCase().match(/[a-z0-9]{2,}/g) || []);
}

function scoreChunk(question, chunkText) {
  const qTokens = tokenize(question);
  const cTokens = tokenize(chunkText);
  if (!qTokens.length || !cTokens.length) return 0;

  const freq = new Map();
  for (const token of cTokens) {
    freq.set(token, (freq.get(token) || 0) + 1);
  }

  let score = 0;
  for (const token of qTokens) {
    score += freq.get(token) || 0;
  }

  const qSet = new Set(qTokens);
  const cSet = new Set(cTokens);
  let intersection = 0;
  for (const token of qSet) {
    if (cSet.has(token)) intersection += 1;
  }
  const union = new Set([...qSet, ...cSet]).size || 1;

  return score + intersection / union;
}

async function extractText(file) {
  const filename = file.originalname.toLowerCase();

  if (filename.endsWith('.pdf')) {
    const parsed = await pdfParse(file.buffer);
    return parsed.text || '';
  }

  return file.buffer.toString('utf8');
}

function buildAnswer(question, matches) {
  if (!matches.length) {
    return "I couldn't find relevant information in the uploaded documents.";
  }

  const best = matches[0];
  const excerpts = matches
    .slice(0, 3)
    .map((m, index) => `Source ${index + 1} (${m.filename}): ${m.text.slice(0, 500)}`)
    .join('\n\n');

  return [
    `Best match for: "${question}"`,
    '',
    best.text.slice(0, 900),
    '',
    'Supporting passages:',
    excerpts,
  ].join('\n');
}

app.get('/health', (_req, res) => {
  res.json({ status: 'ok', service: 'DocuMind Mock Backend' });
});

app.get('/documents', (_req, res) => {
  const items = [...documents.values()].map((doc) => ({
    doc_id: doc.doc_id,
    filename: doc.filename,
    total_chunks: doc.chunks.length,
  }));
  res.json(items);
});

app.post('/upload', upload.single('file'), async (req, res) => {
  try {
    if (!req.file) {
      return res.status(400).json({ detail: 'No file uploaded.' });
    }

    const filename = req.file.originalname || 'document.txt';
    const text = (await extractText(req.file)).trim();

    if (!text) {
      return res.status(422).json({ detail: 'No readable text found in the file.' });
    }

    const doc_id = `${Date.now()}_${Math.random().toString(36).slice(2, 8)}`;
    const chunks = chunkText(text).map((chunk, index) => ({
      ...chunk,
      chunk_index: index,
      filename,
      similarity: 0,
    }));

    documents.set(doc_id, {
      doc_id,
      filename,
      fullText: text,
      chunks,
    });

    res.json({
      message: 'Document ingested successfully.',
      doc_id,
      filename,
      total_chunks: chunks.length,
    });
  } catch (error) {
    res.status(500).json({ detail: error.message || 'Upload failed.' });
  }
});

app.delete('/documents/:docId', (req, res) => {
  const { docId } = req.params;
  if (!documents.has(docId)) {
    return res.status(404).json({ detail: 'Document not found.' });
  }
  documents.delete(docId);
  res.json({ message: `Document '${docId}' deleted.` });
});

app.post('/query', (req, res) => {
  const { question, top_k = 5, doc_id = null } = req.body || {};

  if (!question || !question.trim()) {
    return res.status(422).json({ detail: 'Question must not be empty.' });
  }

  const pool = [...documents.values()]
    .filter((doc) => !doc_id || doc.doc_id === doc_id)
    .flatMap((doc) => doc.chunks.map((chunk) => ({ ...chunk, doc_id: doc.doc_id })));

  const ranked = pool
    .map((chunk) => ({
      ...chunk,
      similarity: scoreChunk(question, chunk.text),
    }))
    .filter((chunk) => chunk.similarity > 0)
    .sort((a, b) => b.similarity - a.similarity)
    .slice(0, Math.max(1, Math.min(Number(top_k) || 5, 10)));

  const sources = ranked.map((item) => ({
    text: item.text,
    filename: item.filename,
    chunk_index: item.chunk_index,
    similarity: Number((item.similarity / (item.similarity + 1)).toFixed(4)),
  }));

  res.json({
    question,
    answer: buildAnswer(question, sources),
    sources,
  });
});

app.listen(PORT, () => {
  console.log(`DocuMind mock backend running on http://localhost:${PORT}`);
});
