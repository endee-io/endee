import os

import torch
from dotenv import load_dotenv
from transformers import AutoModelForCausalLM, AutoTokenizer

load_dotenv()

MODEL_NAME = os.getenv("HF_MODEL_NAME", "gpt2")
DEVICE = torch.device("cpu")

TOKENIZER = AutoTokenizer.from_pretrained(MODEL_NAME)
MODEL = AutoModelForCausalLM.from_pretrained(MODEL_NAME)
MODEL.to(DEVICE)
MODEL.eval()


def generate_answer(question: str, context: str) -> str:
    """Generate an answer using the retrieved context and question."""
    prompt = (
        "Use the context below to answer the question.\n"
        "Context:\n"
        f"{context}\n\n"
        "Question: "
        f"{question}\n"
        "Answer:"
    )

    inputs = TOKENIZER(
        prompt,
        return_tensors="pt",
        truncation=True,
        max_length=900,
    ).to(DEVICE)

    with torch.no_grad():
        output_ids = MODEL.generate(
            **inputs,
            max_new_tokens=180,
            temperature=0.7,
            top_p=0.9,
            do_sample=True,
            pad_token_id=TOKENIZER.eos_token_id,
            eos_token_id=TOKENIZER.eos_token_id,
            num_return_sequences=1,
        )

    generated_text = TOKENIZER.decode(output_ids[0][inputs.input_ids.shape[1] :], skip_special_tokens=True)
    answer_text = generated_text.strip()

    if answer_text.lower().startswith("answer:"):
        answer_text = answer_text[len("answer:") :].strip()

    if not answer_text:
        answer_text = "I could not generate a confident answer from the document."

    return answer_text
