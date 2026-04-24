def generate_answer(query, results):
    if not results:
        return "No results found."

    output = "🔎 Top Matches:\n\n"

    for i, r in enumerate(results):
        output += f"""
━━━━━━━━━━━━━━━━━━
Rank {i+1}
📄 File: {r['file']}
⭐ Score: {r['score']:.4f}

Code:
{r['code'][:800]}
━━━━━━━━━━━━━━━━━━
"""

    return output