import engine
import logging
from telegram import Update
from telegram.ext import ApplicationBuilder, ContextTypes, MessageHandler, filters

# Replace with your token for testing
TOKEN = 'telegrabottokenhere'

logging.basicConfig(format='%(asctime)s - %(name)s - %(levelname)s - %(message)s', level=logging.INFO)

async def handle_message(update: Update, context: ContextTypes.DEFAULT_TYPE):
    user_text = update.message.text
    print(f"Search: {user_text}")
    
    await update.message.reply_text(f"🔍 Finding matches for: '{user_text}'...")

    matches = engine.search_visuals(user_text)

    if not matches:
        await update.message.reply_text("❌ No clear match found.")
        return

    # This loop automatically handles if there is 1 or 2 images
    for i, match in enumerate(matches):
        try:
            photo_path = match['path']
            label = "Best Match" if i == 0 else "Related Match"
            await update.message.reply_photo(
                photo=open(photo_path, 'rb'), 
                caption=f"✅ {label}: {match['name']}"
            )
        except Exception as e:
            print(f"Error: {e}")

if __name__ == '__main__':
    print("🚀 Bot is LIVE! (Adaptive 1-2 Match Mode)")
    app = ApplicationBuilder().token(TOKEN).build()
    app.add_handler(MessageHandler(filters.TEXT & (~filters.COMMAND), handle_message))
    app.run_polling()