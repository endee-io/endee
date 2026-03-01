import gradio as gr
from core.rag_pipeline import rag_query


def chat_interface(user_input):
    if not user_input.strip():
        return "Please enter a question."

    try:
        answer = rag_query(user_input)
        return answer
    except Exception as e:
        return f"Error: {str(e)}"


with gr.Blocks(title="AI Research Assistant (Endee + RAG)") as app:
    gr.Markdown("# 📚 AI Research Assistant")
    gr.Markdown("Powered by Endee Vector DB + Groq Llama 3.3 70B")

    with gr.Row():
        user_input = gr.Textbox(
            label="Ask a Question",
            placeholder="Type your question here...",
            lines=2
        )

    output = gr.Textbox(
        label="Answer",
        lines=6
    )

    submit_btn = gr.Button("Ask")

    submit_btn.click(
        fn=chat_interface,
        inputs=user_input,
        outputs=output
    )

app.launch()