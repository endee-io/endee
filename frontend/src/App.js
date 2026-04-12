import React, { useState } from "react";
import axios from "axios";

function App() {
  const [file, setFile] = useState(null);
  const [jobDesc, setJobDesc] = useState("");
  const [result, setResult] = useState("");

  const handleSubmit = async () => {
    const formData = new FormData();
    formData.append("resume", file);
    formData.append("jobDesc", jobDesc);

    const res = await axios.post("http://localhost:5000/analyze", formData);
    setResult(res.data.result);
  };

  return (
    <div style={{ padding: 20 }}>
      <h2>AI Resume Analyzer</h2>

      <input type="file" onChange={(e) => setFile(e.target.files[0])} />
      <br /><br />

      <textarea
        placeholder="Paste Job Description"
        onChange={(e) => setJobDesc(e.target.value)}
      />
      <br /><br />

      <button onClick={handleSubmit}>Analyze</button>

      <pre>{result}</pre>
    </div>
  );
}

export default App;
