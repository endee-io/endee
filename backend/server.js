const express = require("express");
const multer = require("multer");
const cors = require("cors");
require("dotenv").config();

const { extractText } = require("./parser");
const { analyze } = require("./rag");
const { createCollection } = require("./vector");

const app = express();
const upload = multer();

app.use(cors());
app.use(express.json());

createCollection();

app.post("/analyze", upload.single("resume"), async (req, res) => {
  try {
    const text = await extractText(req.file.buffer);
    const result = await analyze(text, req.body.jobDesc);
    res.json({ result });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.listen(5000, () => console.log("Backend running on port 5000"));
