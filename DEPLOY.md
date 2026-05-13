# Deploy Telegram Bot to Render

## Quick Start

### 1. Install Dependencies
```bash
cd backend
npm install
```

### 2. Get Telegram Bot Token

1. Open Telegram and message [@BotFather](https://t.me/BotFather)
2. Send `/newbot`
3. Follow prompts:
   - Bot name: `ELLA Assistant` (or whatever you want)
   - Bot username: `ella_assistant_bot` (must end in `_bot`)
4. Copy the token (looks like: `7111914232:AAGstJfQW6ktr02JUv15jfgyoKOz9gjwxQA`)

### 3. Add to Render Environment Variables

Go to your Render dashboard → Your service → Environment

Add:
```
TELEGRAM_BOT_TOKEN=your_token_here
```

**Optional:** (will auto-capture on first message)
```
TELEGRAM_CHAT_ID=your_chat_id
```

### 4. Deploy

```bash
git add .
git commit -m "Add Telegram bot support"
git push
```

Render will automatically redeploy.

### 5. Test

1. Find your bot on Telegram (search for the username you created)
2. Send `/start` - Bot will respond and auto-capture your chat ID
3. Send `/status` - Should get "ELLA is offline" (until ESP32 connects)
4. Send `/help` - Should get command list

### 6. Connect ESP32

Once ESP32 connects to the server:
- `/status` will return real sensor data
- `/health` will return health vitals
- Alerts will be sent automatically

## Commands

- `/start` - Initialize bot and capture chat ID
- `/status` - Get current sensor readings
- `/health` - Get heart rate and SpO2 data
- `/help` - Show available commands

## Logs

Check Render logs to see:
```
[Telegram] Bot initialized successfully
[Telegram] Message from 123456789: /status
[Telegram] Message sent: 🤖 ELLA Status Report...
```

## Security

- Bot only responds to the authorized chat ID
- First message auto-captures chat ID
- All other chats are ignored

## Troubleshooting

**Bot not responding:**
- Check `TELEGRAM_BOT_TOKEN` is set correctly
- Check Render logs for errors
- Verify bot username is correct

**"ELLA is offline" message:**
- ESP32 not connected to server yet
- Check ESP32 serial monitor for WebSocket connection status

**Messages not sending:**
- Check `TELEGRAM_CHAT_ID` is set (or send `/start`)
- Check Render logs for Telegram API errors
