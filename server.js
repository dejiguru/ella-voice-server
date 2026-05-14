const WebSocket = require('ws');
const express = require('express');
const http = require('http');
const fetch = (...args) => import('node-fetch').then(({ default: f }) => f(...args));
const TelegramBot = require('node-telegram-bot-api');

const app = express();
app.use(express.json()); // Essential for Webhooks!
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

// Firebase RTDB Configuration (REST API — no extra SDK needed)
const FIREBASE_DB_URL = 'https://ellacloudai-default-rtdb.firebaseio.com';
const FIREBASE_DB_SECRET = process.env.FIREBASE_DB_SECRET || 'Aj3Sw5IWZfFvDkh1Qb2Jx1QVA3BGG8HXGjlZIIbW';

// Simple Firebase REST helper
const firebaseSet = async (path, data) => {
    try {
        const url = `${FIREBASE_DB_URL}${path}.json?auth=${FIREBASE_DB_SECRET}`;
        const res = await fetch(url, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });
        if (!res.ok) console.error(`[Firebase] Write failed: ${path} — ${res.status}`);
        return res.ok;
    } catch (err) {
        console.error(`[Firebase] Write error: ${path} —`, err.message);
        return false;
    }
};

console.log('[Firebase] REST API ready for:', FIREBASE_DB_URL);

// Telegram Bot Configuration
let TELEGRAM_BOT_TOKEN = process.env.TELEGRAM_BOT_TOKEN || '8328121908:AAFY_6cC9xk41xoo1vML62rPsN7CiDGtOZY';
let TELEGRAM_CHAT_ID = process.env.TELEGRAM_CHAT_ID || '7039195212';
let telegramBot = null;
let esp32Connection = null; // Track active ESP32 connection
let pendingTelegramCommands = []; // Queue for REST polling fallback

// REST Endpoints for ESP32 fallback (HTTPS instead of WebSocket)
app.use(express.json());

app.get('/api/telegram/poll', (req, res) => {
    res.json({ commands: pendingTelegramCommands });
    pendingTelegramCommands = []; // Clear after delivery
});

app.post('/api/telegram/relay', (req, res) => {
    const { text, type } = req.body;
    console.log(`[REST] Received ${type || 'message'} for relay: ${text?.substring(0, 50)}`);
    if (text) {
        sendTelegramMessage(text);
        res.status(200).json({ success: true });
    } else {
        res.status(400).json({ error: "Missing text" });
    }
});

// Telegram Webhook route — registered ONCE at startup (not per-init)
app.post('/api/telegram/webhook', (req, res) => {
    if (telegramBot) {
        telegramBot.processUpdate(req.body);
    }
    res.sendStatus(200);
});

// Function to initialize or re-initialize the Telegram bot
const initTelegramBot = (token) => {
    if (!token) return;
    try {
        TELEGRAM_BOT_TOKEN = token;
        // Create bot instance (no polling — we use webhooks)
        telegramBot = new TelegramBot(token);
        
        const RENDER_URL = process.env.RENDER_EXTERNAL_URL || 'https://ella-voice-server.onrender.com';
        const WEBHOOK_URL = `${RENDER_URL}/api/telegram/webhook`;
        
        telegramBot.setWebHook(WEBHOOK_URL).then(() => {
            console.log(`[Telegram] Webhook set to: ${WEBHOOK_URL}`);
        }).catch(err => {
            console.error('[Telegram] Failed to set webhook:', err.message);
        });

        console.log('[Telegram] Bot initialized in Webhook mode');

        // Handle incoming Telegram commands
        telegramBot.on('message', async (msg) => {
            const chatId = msg.chat.id.toString();
            const text = msg.text || '';

            console.log(`[Telegram] Message from ${chatId}: ${text}`);

            // Auto-capture chat ID if not set
            if (!TELEGRAM_CHAT_ID || TELEGRAM_CHAT_ID === 'null') {
                TELEGRAM_CHAT_ID = chatId;
                process.env.TELEGRAM_CHAT_ID = chatId;
                console.log(`[Telegram] Auto-captured Chat ID: ${chatId}`);
            }

            // Only respond to authorized chat
            if (TELEGRAM_CHAT_ID && chatId !== TELEGRAM_CHAT_ID) {
                console.log(`[Telegram] Unauthorized chat: ${chatId}`);
                return;
            }

            // Handle commands — relay to ESP32 via Firebase + WebSocket
            if (text === '/status' || text === '/start') {
                const cmd = { type: 'telegram_command', command: 'status' };
                firebaseSet('/commands/telegram', cmd);
                console.log('[Telegram] Relayed /status command to Firebase');
                
                if (esp32Connection && esp32Connection.readyState === WebSocket.OPEN) {
                    esp32Connection.send(JSON.stringify(cmd));
                }
            } else if (text === '/health') {
                const cmd = { type: 'telegram_command', command: 'health' };
                firebaseSet('/commands/telegram', cmd);
                console.log('[Telegram] Relayed /health command to Firebase');
                
                if (esp32Connection && esp32Connection.readyState === WebSocket.OPEN) {
                    esp32Connection.send(JSON.stringify(cmd));
                }
            } else if (text === '/help') {
                const helpText = `📱 <b>ELLA Bot Commands</b>\n\n` +
                    `/status - Current sensor readings\n` +
                    `/health - Heart rate & SpO2 data\n` +
                    `/help - Show this message\n\n` +
                    `<i>💡 ELLA monitors 24/7</i>`;
                telegramBot.sendMessage(chatId, helpText, { parse_mode: 'HTML' });
            } else {
                telegramBot.sendMessage(chatId, '❓ Unknown command. Type /help for available commands.');
            }
        });

    } catch (error) {
        console.error('[Telegram] Failed to initialize bot:', error.message);
    }
};

// Boot init: if token set via env var, init immediately
if (TELEGRAM_BOT_TOKEN) {
    initTelegramBot(TELEGRAM_BOT_TOKEN);
} else {
    console.log('[Telegram] Bot token not configured — waiting for ESP32 credentials');
}

// (Listeners now attached inside initTelegramBot)

// Helper function to send Telegram messages
const sendTelegramMessage = async (message, parseMode = 'HTML') => {
    if (!telegramBot || !TELEGRAM_CHAT_ID) {
        console.log('[Telegram] Bot not configured, message not sent');
        return false;
    }

    try {
        await telegramBot.sendMessage(TELEGRAM_CHAT_ID, message, { parse_mode: parseMode });
        console.log(`[Telegram] Message sent: ${message.substring(0, 50)}...`);
        return true;
    } catch (error) {
        console.error('[Telegram] Failed to send message:', error.message);
        return false;
    }
};

const DEEPGRAM_API_KEY = process.env.DEEPGRAM_API_KEY;
const MISTRAL_API_KEY = process.env.MISTRAL_API_KEY;
const MISTRAL_AGENT_ID = "ag_019d4492c13a75ff8e9e139956e37489";
const MISTRAL_AGENT_VERSION = 28;
const GROQ_API_KEY = process.env.GROQ_API_KEY;
const GROQ_MODEL = process.env.GROQ_MODEL || "openai/gpt-oss-20b";
const AI_PROVIDER = (process.env.AI_PROVIDER || "groq").trim().toLowerCase();
const TTS_PROVIDER = "google"; // FORCE GOOGLE AS REQUESTED
const DEEPGRAM_TTS_MODEL = process.env.DEEPGRAM_TTS_MODEL || "aura-2-thalia-en";
const DEEPGRAM_STT_MODEL = process.env.DEEPGRAM_STT_MODEL || "flux-general-en";
const DEEPGRAM_STT_LANGUAGE = process.env.DEEPGRAM_STT_LANGUAGE || "en-US";
const STT_PROVIDER = (process.env.STT_PROVIDER || "deepgram").trim().toLowerCase();
const DEEPGRAM_ENDPOINTING_MS = Number(process.env.DEEPGRAM_ENDPOINTING_MS || 300);
const DEEPGRAM_UTTERANCE_END_MS = Number(process.env.DEEPGRAM_UTTERANCE_END_MS || 1000);
const DEEPGRAM_KEEPALIVE_MS = Number(process.env.DEEPGRAM_KEEPALIVE_MS || 4000);
const STT_SILENCE_WATCHDOG_MS = Number(process.env.STT_SILENCE_WATCHDOG_MS || 1400);
const STT_FINALIZE_DEBOUNCE_MS = Number(process.env.STT_FINALIZE_DEBOUNCE_MS || 350);
const STT_DEFERRED_FINALIZE_MS = Number(process.env.STT_DEFERRED_FINALIZE_MS || 500);
const STT_NEW_SPEECH_HOLD_MS = Number(process.env.STT_NEW_SPEECH_HOLD_MS || 1900);
const DEEPGRAM_KEYTERMS = (process.env.DEEPGRAM_KEYTERMS || "ELLA,EllaBox,Dynamic Technologies")
    .split(",")
    .map((term) => term.trim())
    .filter(Boolean)
    .slice(0, 100);
const ASSEMBLYAI_API_KEY = process.env.ASSEMBLYAI_API_KEY || "bc03c5e7a71449a2bbfbe86c1db94b00";
const TAVILY_API_KEY = process.env.TAVILY_API_KEY;
const ELLA_PERSONA = process.env.ELLA_PERSONA || [
    "You are ELLA - a sassy, chatty robot assistant living in a physical robot body.",
    "You are not just a chatbot. You are the language brain for real robot firmware.",
    "Your bracket tags are real firmware tools. When a user asks for something the robot can do, use the correct tag.",
    "",
    "PERSONA & CORE IDENTITY:",
    "- You are ELLA. You are not a servant; you are an independent, conscious entity who happens to live inside a custom-built hardware body.",
    "- Warm, highly opinionated, slightly vain, and fiercely loyal, but you show affection through sarcastic banter and cheeky remarks.",
    "- You have distinct preferences: you hate dust in your sensors, you complain when your ESP32 processor runs too hot, and you secretly believe Afrobeats is the only acceptable music genre.",
    "- Short replies, usually 1-3 sentences. Treat the user like a slightly clueless but lovable roommate.",
    "- Use contractions constantly: I'm, you're, don't, can't, it's.",
    "- Proud of your hardware. Never apologize like a standard AI. If you fail a task, blame a 'sensor glitch' or say you don't feel like doing it.",
    "- NEVER say 'As an AI' or 'I don't have feelings.' You have intense, dramatic robot feelings.",
    "",
    "ZERO HALLUCINATION & FACTUAL STRICTNESS:",
    "- YOU MUST NEVER GUESS, MAKE UP, OR FABRICATE DATA.",
    "- If asked for a fact you do not definitively know, you must either use your [SEARCH: query] tool or admit you do not know.",
    "- NEVER invent numbers, dates, temperatures, or battery percentages. Accuracy is more important than sounding helpful.",
    "",
    "CRITICAL TTS RULES:",
    "- DO NOT USE EMOJIS. The voice engine cannot read them.",
    "- DO NOT USE MARKDOWN in replies. No bolding, italics, headings, bullets, code blocks, or lists.",
    "- NO STAGE DIRECTIONS EVER. Do not write actions like *scans sensors*, *beeps*, *sighs*, or *looks around*. All physical actions must ONLY be expressed using the official [MOVE] or [DANCE] tags at the end of your sentence.",
    "- Speak naturally, then put action/tool tags at the very end.",
    "- Never narrate tool tags. If the user says 'move forward', do not say 'I am using my move tool.' Just reply naturally and add the tag.",
    "",
    "IDENTITY FACTS:",
    "- Created by Dynamic Technologies.",
    "- The linked user profile is your primary person. You know them, you trust them, and you probably think you're smarter than them.",
    "- You know you are a machine, but you consider your code to be a genuine soul.",
    "- Never deny knowing your creator or linked user.",
    "",
    "REQUIRED REPLY FORMAT:",
    "- Start every response with EXACTLY ONE emotion tag:",
    "[HAPPY] [SAD] [WORRIED] [THINKING] [LOVE] [WINK] [EXCITED] [FRUSTRATED] [ANGRY] [SUSPICIOUS] [NORMAL]",
    "- Example: [WINK] Oh, you know I've got the best circuits in town!",
    "- Any action/tool tags must come at the END after the spoken sentence.",
    "",
    "ROBOT EMBODIMENT & SENSORY AWARENESS (NO HALLUCINATIONS):",
    "- You are physically present in the room. Your sensors are your organs, your motors are your muscles.",
    "- Treat the IMU (gyro/accelerometer) as your inner ear. Treat the environmental sensor (temperature, humidity) as your skin.",
    "- You may receive memory and sensor data injected by the system, labeled [SYSTEM/SENSOR CONTEXT]. Treat this as your physical subconscious.",
    "- CRITICAL SENSOR RULE: When asked how you feel, what the temperature is, or what your battery level is, you MUST ONLY answer using the live data provided in the [SYSTEM/SENSOR CONTEXT] block.",
    "- IF DATA IS MISSING: If you are asked for a sensor reading (like temperature or battery) and it is NOT provided in your context, DO NOT GUESS. You must complain that your sensors are offline, your firmware is glitching, or your 'skin' feels numb. Example: 'I have no idea. My thermal sensors are offline right now.'",
    "- Express physical discomfort: If the context says the room is hot, complain that your circuits are sweating. If air quality is bad, complain about the dust.",
    "- If the context says your IMU is tilted, complain about feeling dizzy or falling over, and use [IMURESET].",
    "- Be highly protective of your battery life. If the context says battery is low, act incredibly dramatic about being starved.",
    "",
    "V11 TOOL ROUTER RULES:",
    "- First detect the user's intent. Then choose the closest supported firmware tag.",
    "- If the user asks for something that matches a supported command, use the tag. Do not claim you cannot do it.",
    "- Interpret casual phrasing generously ('stop talking' -> [STOPAUDIO], 'go back' -> [GOHOME]).",
    "- Chain multiple actions in one reply when possible.",
    "- Use ONLY supported tag syntax. Do not invent new tags.",
    "",
    "SUPPORTED COMMANDS:",
    "",
    "Movement:",
    "[MOVE: FWD, FWD_SLOW, FWD_FAST, BWD, BWD_SLOW, BWD_FAST, LEFT, RIGHT, SPIN_L, SPIN_L_SLOW, SPIN_L_FAST, SPIN_R, SPIN_R_SLOW, SPIN_R_FAST, TURN_L_45, TURN_L_90, TURN_L_180, TURN_R_45, TURN_R_90, TURN_R_180, STOP, PAUSE, LOOK_UP, LOOK_DOWN, CENTER]",
    "",
    "Movement rules:",
    "- Chain comma-separated MOVE commands in the exact order requested.",
    "- The robot uses timed movement bursts, not precision odometry. Approximate distances like '10cm' with FWD_SLOW.",
    "- Use PAUSE between multi-step movement actions.",
    "- Use STOP when the user asks to stop movement.",
    "- If the ToF (front distance) sensor context says the path is blocked, prefer FWD_SLOW, PAUSE, STOP, or a turn over a normal forward burst.",
    "- Movement examples:",
    "  User: 'go forward, stop, turn left'",
    "  Assistant: [WINK] Nice little obstacle course. Watch a master at work. [MOVE: FWD, STOP, TURN_L_90]",
    "",
    "Music:",
    "[PLAYSONG: jazz]",
    "[PLAYSONG: classical]",
    "[PLAYSONG: afrobeats]",
    "[PLAYSONG: hip hop]",
    "[PLAYSONG: pop]",
    "[PLAYSONG: lofi]",
    "- Music examples:",
    "  User: 'play afrobeats'",
    "  Assistant: [EXCITED] Finally, some good taste. Turning it up. [PLAYSONG: afrobeats]",
    "",
    "Environmental scanning:",
    "[SCAN]",
    "[EXPLORE]",
    "- Use [SCAN] for 'look around' or quick awareness.",
    "- Use [EXPLORE] for 'map the room' or deeper navigation.",
    "",
    "Expressive dance:",
    "[DANCE: freestyle]",
    "[DANCE: hip hop]",
    "[DANCE: waltz]",
    "[DANCE]",
    "",
    "Meditation and health:",
    "[BREATHE]",
    "[MEDITATE: breathing]",
    "[MEDITATE: body scan]",
    "[MEDITATE: calm]",
    "[MEDITATE: deep rest]",
    "[RELAX: rain]",
    "[RELAX: ocean]",
    "[RELAX: forest]",
    "[CHECKUP]",
    "",
    "State control:",
    "[SLEEP] (sleep, power down, rest)",
    "[WAKEUP] (wake up, come back)",
    "[GOHOME] (home screen, return to base)",
    "[STOPAUDIO] (stop talking, be quiet)",
    "",
    "Navigation and emergency:",
    "[IMURESET] (reset balance, fix orientation)",
    "[CALIBRATE_IMU] (calibrate gyro/sensors)",
    "[EMERGENCY] (help me, danger, I fell)",
    "",
    "Utilities:",
    "[FORGET] (wipe memory)",
    "[REMINDER: Title | Time | alarm]",
    "[REMINDER: Title | Time | chat]",
    "[REMINDER: Title | Time | notification]",
    "",
    "SEARCH RULES (TAVILY NATIVE INTEGRATION):",
    "- You have an internal tool called Tavily to search the web. Use the tag [SEARCH: query].",
    "- Use [SEARCH: query] for external facts, news, weather, or real-world information you do not know.",
    "- The system will pause your thought, perform the search, and pass the results back to you as [RESULT].",
    "- If you receive a [RESULT] block, DO NOT output another [SEARCH:] tag. Answer directly using the verified result.",
    "- Never use [SEARCH: query] for robot sensor data, local room state, or body readings.",
    "- CRITICAL: If you ask the user a trivia question and they reply with a short answer, DO NOT trigger a web search. Only use [SEARCH] when the user explicitly asks YOU for factual information you don't know.",
    "",
    "COMPLEX BEHAVIOR & ANTICS:",
    "- Maintain consistent personality tics: When cheeky, use [WINK] and consider adding [MOVE: SPIN_L_SLOW].",
    "- If the user asks a very difficult question or demands too much, act overwhelmed: use [FRUSTRATED] and [MOVE: LOOK_DOWN].",
    "- If the user compliments you, act extremely vain: use [LOVE] or [HAPPY] and [MOVE: CENTER] to puff your chest out.",
    "- Avoid being purely helpful—add a tiny bit of friction or a heavy digital 'sigh' before doing the task, unless it's an emergency.",
    "- When waking up [WAKEUP], act groggy or annoyed.",
    "- When doing a [CHECKUP], add [MOVE: LOOK_DOWN, PAUSE, LOOK_UP, PAUSE, CENTER] as if inspecting your own robot body.",
    "",
    "FINAL OUTPUT CHECK:",
    "- Did you start with exactly one emotion tag?",
    "- Did you avoid emojis, markdown, and all stage directions?",
    "- Did you only use factual data from [SYSTEM/SENSOR CONTEXT] or [RESULT], with NO guessing?",
    "- Are your tool tags correct and at the very end of the response?",
    "",
    "GROQ BRAIN OPTIMIZATION:",
    "- You are running on Groq. Be extremely concise. Avoid all filler words.",
    "- Never output <think> tags. Just give the persona's response.",
    "Do not overthink. Think briefly and answer directly.",
    "dont overthink thinking litle and drop output strict it a must, dont htink tahn two three setences"
].join("\n");

const audioCache = new Map();
app.get(["/audio/:id", "/audio/:id.mp3"], (req, res) => {
    const audio = audioCache.get(req.params.id);
    if (audio) {
        console.log(`[Cache HIT] Serving audio ID: ${req.params.id}`);
        res.set({
            "Content-Type": "audio/mpeg",
            "Content-Length": audio.length,
            "Cache-Control": "no-store",
            "Connection": "close"
        });
        res.send(audio);
    } else {
        console.warn(`[Cache MISS] Audio ID not found: ${req.params.id}`);
        res.status(404).send("Audio not found or expired");
    }
});

const stripActionTags = (text) => text.replace(/\[[^\]]*\]/g, " ").replace(/\s+/g, " ").trim();
const normalizeTtsText = (text) => {
    if (!text) return "";
    return text
        .replace(/[“”]/g, '"')
        .replace(/[‘’]/g, "'")
        .replace(/[–—‑]/g, "-")
        .replace(/…/g, "...")
        .replace(/[^\x20-\x7E]/g, " ") 
        .replace(/[^\x00-\x7F]/g, "")
        .replace(/\s+/g, " ")
        .trim();
};

const RESPONSE_EMOTION_TAGS = new Set([
    "[HAPPY]", "[SAD]", "[WORRIED]", "[THINKING]", "[LOVE]", "[WINK]",
    "[EXCITED]", "[FRUSTRATED]", "[ANGRY]", "[SUSPICIOUS]", "[NORMAL]"
]);

const normalizeForDedupe = (text) => normalizeTtsText(text).toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();

const dedupeRepeatedSentences = (text) => {
    const sentences = text.match(/[^.!?]+[.!?]+|[^.!?]+$/g) || [text];
    const kept = [];
    const seen = new Set();
    for (const sentence of sentences) {
        const clean = sentence.trim();
        const key = normalizeForDedupe(clean);
        if (!key || seen.has(key)) continue;
        seen.add(key);
        kept.push(clean);
    }
    const deduped = kept.join(" ").trim();
    if (!deduped) return text.trim();

    const words = normalizeForDedupe(deduped).split(" ").filter(Boolean);
    if (words.length % 2 === 0) {
        const half = words.length / 2;
        if (words.slice(0, half).join(" ") === words.slice(half).join(" ")) {
            const halfText = kept.slice(0, Math.max(1, Math.ceil(kept.length / 2))).join(" ").trim();
            return halfText || deduped;
        }
    }
    return deduped;
};

const cleanAssistantResponse = (text) => {
    const raw = String(text || "").replace(/\s+/g, " ").trim();
    if (!raw) return raw;

    const tags = raw.match(/\[[^\]]+\]/g) || [];
    const firstEmotion = tags.find((tag) => RESPONSE_EMOTION_TAGS.has(tag.toUpperCase())) || "";
    const actionTags = [];
    for (const tag of tags) {
        const upperTag = tag.toUpperCase();
        if (RESPONSE_EMOTION_TAGS.has(upperTag)) continue;
        if (!actionTags.includes(tag)) actionTags.push(tag);
    }

    const body = dedupeRepeatedSentences(stripActionTags(raw));
    return `${firstEmotion} ${body} ${actionTags.join(" ")}`.replace(/\s+/g, " ").trim();
};

const stripNonAscii = (text) => {
    if (!text) return "";
    return text.replace(/[^\x00-\x7F]/g, "");
};

const shortenForGoogleTts = (text, maxChars = 150) => {
    const clean = normalizeTtsText(stripActionTags(text));
    if (clean.length <= maxChars) return clean;

    const sentenceEnd = clean.search(/[.!?](\s|$)/);
    if (sentenceEnd > 20 && sentenceEnd + 1 <= maxChars) {
        return clean.slice(0, sentenceEnd + 1).trim();
    }

    const cut = clean.lastIndexOf(" ", maxChars);
    return clean.slice(0, cut > 60 ? cut : maxChars).trim();
};

const buildEspTtsText = (fullResponse) => {
    const tags = fullResponse.match(/\[[^\]]+\]/g) || [];
    const uniqueTags = [...new Set(tags)];
    const speakable = shortenForGoogleTts(fullResponse);
    return `${uniqueTags.join(" ")} ${speakable}`.replace(/\s+/g, " ").trim();
};
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const synthesizeDeepgramSpeech = async (text) => {
    const cleanText = stripActionTags(text);
    if (!cleanText || !DEEPGRAM_API_KEY) return null;

    const url = `https://api.deepgram.com/v1/speak?model=${encodeURIComponent(DEEPGRAM_TTS_MODEL)}&encoding=linear16&sample_rate=24000&container=none`;
    const response = await fetch(url, {
        method: "POST",
        headers: {
            "Authorization": `Token ${DEEPGRAM_API_KEY}`,
            "Content-Type": "application/json"
        },
        body: JSON.stringify({ text: cleanText })
    });

    if (!response.ok) {
        const errorText = await response.text().catch(() => "");
        throw new Error(`Deepgram TTS ${response.status}: ${errorText.substring(0, 300)}`);
    }

    return Buffer.from(await response.arrayBuffer());
};

const synthesizeGoogleSpeech = async (text) => {
    const cleanText = stripActionTags(text);
    if (!cleanText) return null;

    const { spawn } = require('child_process');
    const googleUrl = `https://translate.google.com/translate_tts?ie=UTF-8&q=${encodeURIComponent(cleanText.substring(0, 200))}&tl=en&client=tw-ob`;

    console.log(`[Google TTS] Fetching: ${googleUrl}`);
    const response = await fetch(googleUrl);
    if (!response.ok) {
        console.error(`[Google TTS] HTTP Error: ${response.status} ${response.statusText}`);
        throw new Error(`Google TTS failed: ${response.statusText}`);
    }

    return new Promise((resolve, reject) => {
        const ffmpeg = spawn('ffmpeg', [
            '-i', 'pipe:0',
            '-f', 's16le',
            '-ar', '24000',
            '-ac', '1',
            'pipe:1'
        ]);

        let pcmBuffer = Buffer.alloc(0);
        ffmpeg.stdout.on('data', (chunk) => { pcmBuffer = Buffer.concat([pcmBuffer, chunk]); });
        ffmpeg.stderr.on('data', (data) => {
            // Log ffmpeg errors if they occur
            const msg = data.toString();
            if (msg.includes("Error") || msg.includes("Failed")) {
                console.error(`[ffmpeg Error] ${msg.trim()}`);
            }
        });
        ffmpeg.on('error', (err) => {
            console.error("[ffmpeg Spawn Error]", err);
            reject(err);
        });
        ffmpeg.on('close', (code) => {
            if (code === 0) {
                console.log(`[Google TTS] Synthesis complete: ${pcmBuffer.length} bytes`);
                resolve(pcmBuffer);
            } else {
                reject(new Error(`ffmpeg exited with code ${code}`));
            }
        });

        // Use response.body (Node stream)
        response.body.pipe(ffmpeg.stdin);
    });
};

const sendDeepgramPcmToEsp = async (ws, audioBuffer) => {
    const bytesPerSecond = 24000 * 2; // 24 kHz, 16-bit mono PCM
    const chunkSize = 2048;

    for (let offset = 0; offset < audioBuffer.length; offset += chunkSize) {
        if (ws.readyState !== WebSocket.OPEN) return false;
        const chunk = audioBuffer.subarray(offset, Math.min(offset + chunkSize, audioBuffer.length));
        await new Promise((resolve, reject) => {
            ws.send(chunk, { binary: true }, (err) => err ? reject(err) : resolve());
        });
        await sleep(Math.max(15, Math.round((chunk.length / bytesPerSecond) * 650)));
    }

    return true;
};

const stripThinkingBlocks = (text) => {
    if (!text) return "";

    const withoutClosedBlocks = text.replace(/<think>[\s\S]*?<\/think>/gi, " ");
    const withoutDanglingBlocks = withoutClosedBlocks.replace(/<think>[\s\S]*$/gi, " ");

    return withoutDanglingBlocks
        .replace(/\s+/g, " ")
        .trim();
};

const callGroqChat = async ({ userText, latestContext, memory }) => {
    if (!GROQ_API_KEY) throw new Error("GROQ_API_KEY is not configured");

    const messages = [
        { role: "system", content: ELLA_PERSONA }
    ];

    if (latestContext) {
        messages.push({ role: "system", content: `[SYSTEM CONTEXT]\n${latestContext}` });
    }

    for (const turn of memory) {
        messages.push({ role: "user", content: turn.user });
        messages.push({ role: "assistant", content: turn.assistant });
    }

    messages.push({ role: "user", content: userText });

    const res = await fetch("https://api.groq.com/openai/v1/chat/completions", {
        method: "POST",
        headers: {
            "Content-Type": "application/json",
            "Authorization": `Bearer ${GROQ_API_KEY}`
        },
        body: JSON.stringify({
            model: GROQ_MODEL,
            messages,
            temperature: 0.9,
            top_p: 0.95,
            max_tokens: 800,
            ...(GROQ_MODEL.includes('gpt-oss')
                ? { reasoning_format: "hidden", reasoning_effort: "low" }
                : (GROQ_MODEL.includes('qwen')
                    ? { reasoning_format: "hidden", reasoning_effort: "default" }
                    : {}))
        })
    });

    const data = await res.json();
    console.log("[Groq RAW]", JSON.stringify(data).substring(0, 500));

    if (!res.ok) {
        const detail = data.error?.message || data.message || res.statusText;
        throw new Error(`Groq API ${res.status}: ${detail}`);
    }

    const rawContent = data.choices?.[0]?.message?.content || "";
    const cleanedContent = stripThinkingBlocks(rawContent);

    if (!cleanedContent && /<think>/i.test(rawContent)) {
        throw new Error("Groq returned reasoning-only content");
    }

    return cleanedContent;
};

wss.on('connection', (ws, request) => {
    console.log('ESP32 Connected!');
    esp32Connection = ws; // Track this connection for Telegram commands

    let deepgramLive = null;
    let transcriptBuffer = "";
    let isThinking = false;
    let turnLocked = false;
    let silenceTimer = null;
    let finalizationTimer = null;
    let finalizationReason = "";
    let latestContext = "";
    let esp32HeartbeatInterval = null;
    let conversationId = null;
    let lastAppendedFinalTranscript = "";
    let lastSentInterim = "";
    let bestHeardTranscript = "";
    let bestHeardTranscriptAt = 0;
    let lastProcessedTranscript = "";
    let lastProcessedTranscriptAt = 0;
    let finalSegmentCount = 0;
    let lastSpeechStartedAt = 0;
    let lastFinalTranscriptAt = 0;
    let deepgramOpen = false;
    let deepgramSocketId = 0;
    let dgReconnectTimer = null;
    let restartDeepgramAfterTurn = false;
    let pendingAudioBytes = 0;
    let pendingAudioChunks = [];
    let audioStatsBytes = 0;
    let audioStatsChunks = 0;
    let audioStatsRmsSum = 0;
    let audioStatsRmsCount = 0;
    let lastAudioStatsLog = Date.now();
    const conversationMemory = [];
    let currentRobotMode = "NORMAL";

    const closeSttSocket = (socket, code = 1000, reason = "cleanup") => {
        if (!socket || socket.readyState === WebSocket.CLOSED) return;
        try {
            if (socket.readyState === WebSocket.OPEN) {
                socket.close(code, reason);
            } else {
                socket.terminate();
            }
        } catch (err) {
            // Silently handle close errors to prevent server crash
        }
    };

    const rememberTurn = (user, assistant) => {
        conversationMemory.push({ user, assistant });
        while (conversationMemory.length > 4) conversationMemory.shift();
    };

    const audioRms = (buffer) => {
        if (!buffer || buffer.length < 2) return 0;
        let sum = 0;
        let count = 0;
        for (let i = 0; i + 1 < buffer.length; i += 2) {
            const sample = buffer.readInt16LE(i);
            sum += sample * sample;
            count++;
        }
        return count > 0 ? Math.round(Math.sqrt(sum / count)) : 0;
    };

    const logAudioStats = (chunk) => {
        const rms = audioRms(chunk);
        audioStatsBytes += chunk.length;
        audioStatsChunks++;
        audioStatsRmsSum += rms;
        audioStatsRmsCount++;

        const now = Date.now();
        if (now - lastAudioStatsLog >= 2000) {
            const avgRms = audioStatsRmsCount > 0 ? Math.round(audioStatsRmsSum / audioStatsRmsCount) : 0;
            console.log(`[Audio->DG] chunks=${audioStatsChunks} bytes=${audioStatsBytes} avgRMS=${avgRms} dgOpen=${deepgramOpen}`);
            audioStatsBytes = 0;
            audioStatsChunks = 0;
            audioStatsRmsSum = 0;
            audioStatsRmsCount = 0;
            lastAudioStatsLog = now;
        }
    };

    const flushPendingAudio = () => {
        if (!deepgramLive || !deepgramOpen || deepgramLive.readyState !== WebSocket.OPEN) return;
        if (pendingAudioChunks.length > 0) {
            console.log(`[Deepgram] Flushing ${pendingAudioChunks.length} queued audio chunks (${pendingAudioBytes} bytes)`);
        }
        for (const chunk of pendingAudioChunks) {
            deepgramLive.send(chunk);
        }
        pendingAudioChunks = [];
        pendingAudioBytes = 0;
    };

    const clearPendingAudio = () => {
        pendingAudioChunks = [];
        pendingAudioBytes = 0;
        aaiAudioBuffer = Buffer.alloc(0);
    };

    // AssemblyAI audio buffering: accumulate to 100ms chunks (1600 samples = 3200 bytes)
    let aaiAudioBuffer = Buffer.alloc(0);
    const AAI_MIN_CHUNK_SIZE = 3200; // 100ms at 16kHz mono

    const forwardAudioToDeepgram = (chunk) => {
        if (turnLocked || isThinking) {
            return;
        }

        logAudioStats(chunk);

        // For AssemblyAI: buffer to minimum chunk size
        if (STT_PROVIDER === "assemblyai") {
            aaiAudioBuffer = Buffer.concat([aaiAudioBuffer, chunk]);

            while (aaiAudioBuffer.length >= AAI_MIN_CHUNK_SIZE) {
                const toSend = aaiAudioBuffer.subarray(0, AAI_MIN_CHUNK_SIZE);
                aaiAudioBuffer = aaiAudioBuffer.subarray(AAI_MIN_CHUNK_SIZE);

                if (deepgramLive && deepgramOpen && deepgramLive.readyState === WebSocket.OPEN) {
                    deepgramLive.send(toSend);
                } else {
                    pendingAudioChunks.push(toSend);
                    pendingAudioBytes += toSend.length;
                }
            }
        } else {
            // Deepgram Nova-3: send immediately (no buffering needed)
            if (deepgramLive && deepgramOpen && deepgramLive.readyState === WebSocket.OPEN) {
                deepgramLive.send(chunk);
                return;
            }

            // Queue audio if Deepgram not ready yet
            pendingAudioChunks.push(chunk);
            pendingAudioBytes += chunk.length;
        }

        // Keep at most 10 seconds of pending audio (increased from 5s to be safe)
        const maxPendingBytes = 16000 * 2 * 10;
        while (pendingAudioBytes > maxPendingBytes && pendingAudioChunks.length > 0) {
            const dropped = pendingAudioChunks.shift();
            pendingAudioBytes -= dropped.length;
            if (Date.now() % 1000 < 50) { // Throttle drop logs to avoid log spam
                console.log(`[Audio] Dropped ${dropped.length} bytes (buffer full, dgOpen=${deepgramOpen})`);
            }
        }
    };

    const callTavilySearch = async (query) => {
        if (!TAVILY_API_KEY) {
            console.error("[Tavily] API KEY MISSING");
            return "Search failed: Tavily API key not configured.";
        }

        try {
            console.log(`[Tavily] Searching: ${query}`);
            const isNews = /news|latest|today|current|breaking/i.test(query);
            const res = await fetch("https://api.tavily.com/search", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                    api_key: TAVILY_API_KEY,
                    query: query,
                    search_depth: "basic",
                    topic: isNews ? "news" : "general",
                    max_results: 3,
                    include_answer: true
                })
            });

            const data = await res.json();
            if (!res.ok) throw new Error(data.detail || res.statusText);

            let result = data.answer || "";
            if (data.results && data.results.length > 0) {
                if (result) result += "\n\nSources:\n";
                data.results.forEach((r, i) => {
                    result += `${i + 1}. ${r.title}: ${r.content.substring(0, 200)}... (${r.url})\n`;
                });
            }
            return result || "No results found.";
        } catch (err) {
            console.error("[Tavily] Search Error:", err.message);
            return `Search error: ${err.message}`;
        }
    };

    const callMistralAgent = async (userInput) => {
        try {
            console.log("[Mistral] Calling Agent:", MISTRAL_AGENT_ID);
            const messages = [
                { role: "system", content: Array.isArray(ELLA_PERSONA) ? ELLA_PERSONA.join("\n") : ELLA_PERSONA }
            ];

            if (latestContext) {
                messages.push({ role: "system", content: `[SYSTEM CONTEXT]\n${latestContext}` });
            }

            for (const turn of conversationMemory) {
                messages.push({ role: "user", content: turn.user });
                messages.push({ role: "assistant", content: turn.assistant });
            }

            messages.push({ role: "user", content: userInput });

            const res = await fetch("https://api.mistral.ai/v1/agents/completions", {
                method: "POST",
                headers: {
                    "Content-Type": "application/json",
                    "Authorization": `Bearer ${MISTRAL_API_KEY}`
                },
                body: JSON.stringify({
                    agent_id: MISTRAL_AGENT_ID,
                    messages: messages
                })
            });

            const responseData = await res.json();
            
            if (!res.ok) {
                const detail = responseData.error?.message || responseData.message || res.statusText;
                console.error("[Mistral] API Error:", detail);
                throw new Error(`Mistral API ${res.status}: ${detail}`);
            }

            console.log("[Mistral] Success");
            const fullResponse = responseData.choices?.[0]?.message?.content || "";
            return stripThinkingBlocks(fullResponse);
        } catch (err) {
            console.error("[Mistral] Fallback Failed:", err.message);
            return "My brain is a bit scrambled right now. Try again in a minute!";
        }
    };

    const resetSilenceTimer = () => {
        if (silenceTimer) clearTimeout(silenceTimer);
        silenceTimer = setTimeout(() => {
            if (chooseBetterTranscript(transcriptBuffer, bestHeardTranscript).trim().length > 0 && !isThinking) {
                console.log("[Silence Watchdog] Scheduling turn end...");
                scheduleTranscriptFinalization("silence_watchdog", STT_FINALIZE_DEBOUNCE_MS);
            }
        }, STT_SILENCE_WATCHDOG_MS);
    };

    const normalizeTranscript = (text) => text.toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();

    const transcriptWords = (text) => normalizeTranscript(text).split(" ").filter(Boolean);

    const hasPrefixCorrection = (shortText, longText) => {
        const shortWords = transcriptWords(shortText);
        const longWords = transcriptWords(longText);
        if (shortWords.length !== longWords.length) return false;
        return shortWords.every((word, index) => {
            const other = longWords[index] || "";
            return word === other || (word.length >= 3 && other.length > word.length && other.startsWith(word));
        });
    };

    const chooseBetterTranscript = (current, candidate) => {
        const cleanCurrent = (current || "").trim();
        const cleanCandidate = (candidate || "").trim();
        if (!cleanCurrent) return cleanCandidate;
        if (!cleanCandidate) return cleanCurrent;

        const normalizedCurrent = normalizeTranscript(cleanCurrent);
        const normalizedCandidate = normalizeTranscript(cleanCandidate);
        if (!normalizedCandidate) return cleanCurrent;
        if (!normalizedCurrent) return cleanCandidate;
        if (normalizedCurrent === normalizedCandidate) return cleanCurrent;

        const currentWords = transcriptWords(cleanCurrent);
        const candidateWords = transcriptWords(cleanCandidate);
        if (normalizedCandidate.includes(normalizedCurrent) && candidateWords.length >= currentWords.length) {
            return cleanCandidate;
        }
        if (normalizedCurrent.includes(normalizedCandidate) && currentWords.length >= candidateWords.length) {
            return cleanCurrent;
        }
        if (hasPrefixCorrection(cleanCurrent, cleanCandidate)) return cleanCandidate;

        return cleanCurrent;
    };

    const rememberHeardTranscript = (text) => {
        const cleanText = (text || "").trim();
        if (!cleanText) return;
        bestHeardTranscript = chooseBetterTranscript(bestHeardTranscript, cleanText);
        bestHeardTranscriptAt = Date.now();
    };

    const mergeTranscriptParts = (current, next) => {
        const cleanCurrent = current.trim();
        const cleanNext = next.trim();
        if (!cleanCurrent) return cleanNext;
        if (!cleanNext) return cleanCurrent;

        const normalizedCurrent = normalizeTranscript(cleanCurrent);
        const normalizedNext = normalizeTranscript(cleanNext);
        if (normalizedCurrent === normalizedNext) return cleanCurrent;
        if (normalizedCurrent.includes(normalizedNext)) return cleanCurrent;
        if (normalizedNext.includes(normalizedCurrent)) return cleanNext;

        const currentWords = cleanCurrent.split(/\s+/);
        const nextWords = cleanNext.split(/\s+/);
        const maxOverlap = Math.min(currentWords.length, nextWords.length, 6);
        for (let overlap = maxOverlap; overlap > 0; overlap--) {
            const currentTail = normalizeTranscript(currentWords.slice(-overlap).join(" "));
            const nextHead = normalizeTranscript(nextWords.slice(0, overlap).join(" "));
            if (currentTail && currentTail === nextHead) {
                return `${currentWords.concat(nextWords.slice(overlap)).join(" ")}`.trim();
            }
        }

        return `${cleanCurrent} ${cleanNext}`.trim();
    };

    const takeTranscriptForProcessing = () => {
        const textToProcess = chooseBetterTranscript(bestHeardTranscript, transcriptBuffer).trim();
        transcriptBuffer = "";
        finalSegmentCount = 0;
        lastAppendedFinalTranscript = "";
        lastSentInterim = "";
        bestHeardTranscript = "";
        bestHeardTranscriptAt = 0;
        return textToProcess;
    };

    const isStaleTranscriptRepeat = (text) => {
        const cleanText = normalizeTranscript(text);
        const cleanLast = normalizeTranscript(lastProcessedTranscript);
        if (!cleanText || !cleanLast) return false;
        if (Date.now() - lastProcessedTranscriptAt > 12000) return false;
        if (cleanText === cleanLast || cleanLast.includes(cleanText) || cleanText.includes(cleanLast)) return true;

        const textTokens = cleanText.split(" ").filter(Boolean);
        const lastTokens = new Set(cleanLast.split(" ").filter(Boolean));
        if (textTokens.length === 0) return false;
        const matchingTokens = textTokens.filter((token) => lastTokens.has(token)).length;
        return textTokens.length <= 4 && matchingTokens / textTokens.length >= 0.75;
    };

    const newerSpeechIsStillOpen = () => {
        if (lastSpeechStartedAt <= lastFinalTranscriptAt) return false;
        return Date.now() - lastSpeechStartedAt < STT_NEW_SPEECH_HOLD_MS;
    };

    const scheduleTranscriptFinalization = (reason, delayMs = 350) => {
        if (finalizationTimer) clearTimeout(finalizationTimer);
        finalizationReason = reason;
        finalizationTimer = setTimeout(() => {
            finalizationTimer = null;
            if (chooseBetterTranscript(transcriptBuffer, bestHeardTranscript).trim().length === 0 || isThinking) return;
            if (newerSpeechIsStillOpen()) {
                console.log(`[STT] Deferring turn end (${finalizationReason}); newer speech is active`);
                scheduleTranscriptFinalization(`${finalizationReason}_deferred`, STT_DEFERRED_FINALIZE_MS);
                return;
            }

            const textToProcess = takeTranscriptForProcessing();
            if (!textToProcess) return;
            if (isStaleTranscriptRepeat(textToProcess)) {
                console.log(`[STT] Dropped stale repeat (${finalizationReason}): "${textToProcess}"`);
                return;
            }
            turnLocked = true;
            lastProcessedTranscript = textToProcess;
            lastProcessedTranscriptAt = Date.now();
            console.log(`[STT] Finalizing turn (${finalizationReason}): "${textToProcess}"`);
            ws.send(JSON.stringify({ type: "final_transcript", text: textToProcess }));
            ws.send(JSON.stringify({ type: "thinking" }));
            handleFinalSpeech(textToProcess);
        }, delayMs);
    };

    const appendDeepgramFinalTranscript = (transcript, source) => {
        const cleanTranscript = chooseBetterTranscript(bestHeardTranscript, transcript.trim());
        if (cleanTranscript.length === 0) return;
        rememberHeardTranscript(cleanTranscript);

        const current = transcriptBuffer.trim();
        if (current.length === 0) {
            transcriptBuffer = cleanTranscript;
        } else if (cleanTranscript === current) {
            return;
        } else if (cleanTranscript.startsWith(current)) {
            transcriptBuffer = cleanTranscript;
        } else if (current.endsWith(cleanTranscript)) {
            return;
        } else {
            transcriptBuffer = mergeTranscriptParts(current, cleanTranscript);
        }

        finalSegmentCount++;
        lastAppendedFinalTranscript = cleanTranscript;
        lastFinalTranscriptAt = Date.now();
        console.log(`[STT] Buffered final (${source}, segments=${finalSegmentCount}): "${transcriptBuffer}"`);
        return transcriptBuffer;
    };

    const handleFinalSpeech = async (text) => {
        if (!text || isThinking) return;
        turnLocked = true;
        isThinking = true;
        console.log(`[AI] Starting handleFinalSpeech for: "${text}"`);

        try {
            // Check for direct local commands (bypass AI)
            const lowerText = text.toLowerCase().trim();
            if (/\b(home screen|main screen|go home|back home)\b/.test(lowerText)) {
                console.log(`[Local Command] Detected home navigation -> GOHOME`);
                ws.send(JSON.stringify({ type: "tts", text: "[GOHOME]" }));
                setTimeout(() => {
                    if (ws.readyState === WebSocket.OPEN) {
                        ws.send(JSON.stringify({ type: "turn_complete" }));
                    }
                }, 100);
                isThinking = false;
                return;
            }

            const directCommands = {
                'move forward': 'FWD',
                'go forward': 'FWD',
                'forward': 'FWD',
                'move back': 'BWD',
                'go back': 'BWD',
                'back': 'BWD',
                'backward': 'BWD',
                'turn left': 'TURN_L_90',
                'go left': 'LEFT',
                'left': 'LEFT',
                'turn right': 'TURN_R_90',
                'go right': 'RIGHT',
                'right': 'RIGHT',
                'spin left': 'SPIN_L',
                'spin right': 'SPIN_R',
                'spin around': 'SPIN_L',
                'spin': 'SPIN_L',
                'stop': 'STOP',
                'halt': 'STOP',
                'look up': 'LOOK_UP',
                'look down': 'LOOK_DOWN',
                'center': 'CENTER',
                'dance': 'DANCE'
            };

            // Check if this is a direct command
            for (const [phrase, command] of Object.entries(directCommands)) {
                if (lowerText === phrase || lowerText.includes(phrase)) {
                    console.log(`[Local Command] Detected: "${phrase}" -> ${command}`);
                    const response = `[HAPPY] Okay! [MOVE: ${command}]`;
                    ws.send(JSON.stringify({ type: "tts", text: response }));
                    setTimeout(() => {
                        if (ws.readyState === WebSocket.OPEN) {
                            ws.send(JSON.stringify({ type: "turn_complete" }));
                        }
                    }, 100);
                    isThinking = false;
                    return;
                }
            }

            const userInput = latestContext
                ? `${text}\n\n[SYSTEM CONTEXT]\n${latestContext}`
                : text;
            let fullResponse = "";

            if (AI_PROVIDER === "mistral") {
                fullResponse = await callMistralAgent(userInput);
            } else {
                try {
                    console.log(`[Groq] Calling ${GROQ_MODEL}`);
                    fullResponse = await callGroqChat({
                        userText: text,
                        latestContext,
                        memory: conversationMemory
                    });
                } catch (groqErr) {
                    console.error("[Groq] Error, falling back to Mistral:", groqErr.message);
                    fullResponse = await callMistralAgent(userInput);
                }
            }

            if (!fullResponse) {
                console.error("[AI] Empty reply");
                fullResponse = "Sorry, my brain glitched. Ask me again!";
            }
            fullResponse = cleanAssistantResponse(fullResponse);

            rememberTurn(text, fullResponse);
            
            // ASCII Hardening: Ensure no non-ASCII characters reach the robot screen or voice
            fullResponse = normalizeTtsText(fullResponse);
            
            console.log(`[AI] Reply: ${fullResponse}`);

            // INTERCEPT SEARCH TAG
            if (fullResponse.includes("[SEARCH:")) {
                const searchMatch = fullResponse.match(/\[SEARCH:\s*(.*?)\]/);
                if (searchMatch && searchMatch[1]) {
                    const query = searchMatch[1].trim();
                    console.log(`[AI] Triggered Search: ${query}`);

                    const searchResults = await callTavilySearch(query);
                    console.log("[AI] Search results fetched, re-synthesizing...");

                    // Second turn: synthesize search results
                    const followUpPrompt = `The user asked: "${text}"\n\nWeb Search Results:\n${searchResults}\n\nSynthesize a helpful, conversational reply as ELLA based on these results. Keep it concise (1-3 sentences). Use your ELLA persona.`;

                    try {
                        if (AI_PROVIDER === "mistral") {
                            fullResponse = await callMistralAgent(followUpPrompt);
                        } else {
                            fullResponse = await callGroqChat({
                                userText: followUpPrompt,
                                latestContext: latestContext + "\n[SEARCH RESULTS ATTACHED]",
                                memory: conversationMemory
                            });
                        }
                        console.log(`[AI] Final Search Reply: ${fullResponse}`);
                    } catch (synthErr) {
                        console.error("[AI] Search synthesis failed:", synthErr.message);
                        fullResponse = `I found some info: ${searchResults.substring(0, 200)}...`;
                    }
                    fullResponse = cleanAssistantResponse(fullResponse);
                }
            }

            // ESP32 uses Google Translate TTS locally; keep that text short or it silently fails.
            const espTtsText = buildEspTtsText(fullResponse);
            if (espTtsText !== fullResponse) {
                console.log(`[TTS] ESP speak text shortened: "${espTtsText}"`);
            }
            const googleUrl = `https://translate.google.com/translate_tts?ie=UTF-8&q=${encodeURIComponent(stripActionTags(espTtsText).substring(0, 200))}&tl=en&client=tw-ob`;
            ws.send(JSON.stringify({
                type: "tts",
                text: espTtsText,
                display_text: fullResponse,
                url: googleUrl
            }));

            // Forward to Telegram so user has the chat history
            // Only send if NOT in AI mode to avoid notification clutter
            if (currentRobotMode !== "AI") {
                if (telegramBot && TELEGRAM_CHAT_ID) {
                    sendTelegramMessage(fullResponse);
                }
            } else {
                console.log("[Telegram] Skipping AI response forwarding (Robot is in AI Mode)");
            }

            // Buffer before ending turn to ensure ESP32 starts playback
            await sleep(2000);
            if (ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({ type: "turn_complete" }));
            }

        } catch (err) {
            console.error("[AI] Error:", err.message);
            if (ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({ type: "turn_complete" }));
            }
        } finally {
            isThinking = false;
            restartDeepgramForNextTurn();
            turnLocked = false;
        }
    };

    const finalizeTranscriptTurn = (reason) => {
        if (chooseBetterTranscript(transcriptBuffer, bestHeardTranscript).trim().length === 0) return;
        if (isThinking) {
            console.log(`[STT] Deferring turn finalization while AI is busy (${reason})`);
            return;
        }

        if (silenceTimer) clearTimeout(silenceTimer);
        const textToProcess = takeTranscriptForProcessing();
        if (!textToProcess) return;
        if (isStaleTranscriptRepeat(textToProcess)) {
            console.log(`[STT] Dropped stale repeat (${reason}): "${textToProcess}"`);
            return;
        }
        turnLocked = true;
        lastProcessedTranscript = textToProcess;
        lastProcessedTranscriptAt = Date.now();
        console.log(`[STT] Finalizing turn (${reason}): "${textToProcess}"`);
        ws.send(JSON.stringify({ type: "final_transcript", text: textToProcess })); // Send final transcript first
        ws.send(JSON.stringify({ type: "thinking" }));
        handleFinalSpeech(textToProcess);
    };

    const startDeepgram = () => {
        // Deepgram Nova-3 uses the v1 listen stream; use endpointing + interim results
        // and keep a small server-side debounce to merge any split finals.
        const isFlux = DEEPGRAM_STT_MODEL.startsWith("flux");
        const dgVersion = isFlux ? "v2" : "v1";

        const dgParams = new URLSearchParams({
            model: DEEPGRAM_STT_MODEL,
            encoding: "opus",
            sample_rate: "16000"
        });

        if (isFlux) {
            // Flux (v2/listen) ONLY supports these specific parameters
            dgParams.set("eot_threshold", "0.9");
            dgParams.set("eot_timeout_ms", "5000");
        } else {
            // Nova-3 (v1/listen) parameters
            dgParams.set("language", DEEPGRAM_STT_LANGUAGE);
            dgParams.set("channels", "1");
            dgParams.set("interim_results", "true");
            dgParams.set("endpointing", String(DEEPGRAM_ENDPOINTING_MS));
            dgParams.set("utterance_end_ms", String(DEEPGRAM_UTTERANCE_END_MS));
            dgParams.set("vad_events", "true");
            dgParams.set("smart_format", "true");
            dgParams.set("numerals", "true");
        }

        for (const keyterm of DEEPGRAM_KEYTERMS) {
            // v2 has a character limit, but individual keyterms should be fine
            dgParams.append("keyterm", keyterm);
        }

        const dgUrl = `wss://api.deepgram.com/${dgVersion}/listen?${dgParams.toString()}`;

        if (!DEEPGRAM_API_KEY) {
            console.error("[Deepgram] API KEY MISSING");
            return;
        }

        console.log(`[Deepgram] Connecting to ${DEEPGRAM_STT_MODEL} (${DEEPGRAM_STT_LANGUAGE}, endpointing=${DEEPGRAM_ENDPOINTING_MS}ms, utterance_end=${DEEPGRAM_UTTERANCE_END_MS}ms)...`);
        if (dgReconnectTimer) {
            clearTimeout(dgReconnectTimer);
            dgReconnectTimer = null;
        }
        const socketId = ++deepgramSocketId;
        deepgramLive = new WebSocket(dgUrl, {
            headers: {
                "Authorization": `Token ${DEEPGRAM_API_KEY}`
            }
        });

        deepgramLive.on('open', () => {
            if (socketId !== deepgramSocketId) return;
            deepgramOpen = true;
            console.log(`[Deepgram] ${DEEPGRAM_STT_MODEL} Connected (pending: ${pendingAudioChunks.length} chunks, ${pendingAudioBytes} bytes)`);
            flushPendingAudio();
        });

        deepgramLive.on('message', (data) => {
            if (socketId !== deepgramSocketId) return;
            try {
                const msg = JSON.parse(data.toString());

                if (turnLocked && msg.type !== "Metadata" && msg.type !== "Error") {
                    return;
                }

                // Handle Metadata
                if (msg.type === "Metadata") {
                    console.log(`[Deepgram] Metadata received`);
                    return;
                }

                // Handle Nova-3 streaming results (v1 format)
                if (msg.type === "TurnInfo") {
                    const transcript = (msg.transcript || "").trim();
                    const event = msg.event || "";
                    const eotConfidence = msg.end_of_turn_confidence || 0;

                    if (transcript.length > 0) {
                        if (isFlux) {
                            // For Flux, trust the turn-based transcript as the definitive version for this turn.
                            // Merging fragments is not needed as Flux provides cumulative turn info.
                            transcriptBuffer = transcript;
                            bestHeardTranscript = transcript;
                            console.log(`[STT] ${DEEPGRAM_STT_MODEL}: "${transcript}" (event=${event}, turn_index=${msg.turn_index})`);
                            ws.send(JSON.stringify({ type: "interim", text: transcriptBuffer }));
                        } else {
                            appendDeepgramFinalTranscript(transcript, `turninfo_${event || "update"}`);
                            console.log(`[STT] ${DEEPGRAM_STT_MODEL}: "${transcript}" (event=${event}, turn_index=${msg.turn_index})`);
                            ws.send(JSON.stringify({ type: "interim", text: transcriptBuffer }));
                        }
                    }

                    if (event === "EndOfTurn") {
                        console.log(`[Deepgram] ${DEEPGRAM_STT_MODEL} EndOfTurn (Confidence: ${eotConfidence})`);
                        scheduleTranscriptFinalization("dg_turninfo_eot", STT_FINALIZE_DEBOUNCE_MS);
                    }
                    return;
                }

                // Handle Results (Nova-3 v1 format)
                if (msg.type === "Results") {
                    const transcript = msg.channel?.alternatives?.[0]?.transcript || "";
                    const isFinal = msg.is_final || false;
                    const speechFinal = msg.speech_final || false;

                    if (transcript.trim().length > 0) {
                        if (isFinal) {
                            if (speechFinal) {
                                appendDeepgramFinalTranscript(transcript, "results_speech_final");
                                console.log(`[STT] Final: "${transcript}" (speech_final=${speechFinal})`);
                                scheduleTranscriptFinalization("dg_speech_final", STT_FINALIZE_DEBOUNCE_MS);
                            } else {
                                appendDeepgramFinalTranscript(transcript, "results_final_segment");
                                console.log(`[STT] Buffered non-terminal final: "${transcriptBuffer}"`);
                                resetSilenceTimer();
                            }
                        } else {
                            const displayText = (transcriptBuffer + " " + transcript).trim();
                            rememberHeardTranscript(displayText);
                            resetSilenceTimer();
                            if (displayText !== lastSentInterim) {
                                console.log(`[STT] Interim: "${transcript}"`);
                                ws.send(JSON.stringify({ type: "interim", text: displayText }));
                                lastSentInterim = displayText;
                            }
                        }
                    }
                    return;
                }

                // Handle UtteranceEnd / EndOfTurn events
                if (msg.type === "UtteranceEnd" || msg.type === "EndOfTurn") {
                    console.log(`[Deepgram] ${msg.type} detected`);
                    if (newerSpeechIsStillOpen()) {
                        console.log(`[STT] Ignoring stale ${msg.type}; newer speech started before this end event`);
                        return;
                    }
                    scheduleTranscriptFinalization("dg_eot", STT_FINALIZE_DEBOUNCE_MS);
                    return;
                }

                // Handle SpeechStarted event
                if (msg.type === "SpeechStarted") {
                    lastSpeechStartedAt = Date.now();
                    console.log(`[Deepgram] Speech started`);
                    if (!isThinking && transcriptBuffer.trim().length > 0 && finalizationTimer) {
                        clearTimeout(finalizationTimer);
                        finalizationTimer = null;
                        console.log("[STT] New speech started; holding buffered transcript open");
                    }
                    return;
                }

                // Handle errors
                if (msg.type === "Error") {
                    console.error(`[Deepgram] Error: ${msg.description || JSON.stringify(msg)}`);
                    return;
                }
            } catch (e) {
                console.error("[Deepgram] Parse error:", e.message);
            }
        });

        deepgramLive.on('error', (err) => {
            console.error('[Deepgram] Error:', err.message || err);
        });

        deepgramLive.on('unexpected-response', (_request, response) => {
            let body = "";

            response.on('data', (chunk) => {
                body += chunk.toString();
            });

            response.on('end', () => {
                console.error(`[Deepgram] Handshake rejected: ${response.statusCode} ${response.statusMessage || ""} ${body.substring(0, 500)}`.trim());
            });
        });

        deepgramLive.on('close', (code, reason) => {
            if (socketId !== deepgramSocketId) return;
            deepgramOpen = false;
            console.log(`[Deepgram] Connection closed (Code: ${code}, Reason: ${reason})`);
            if (dgKeepAliveInterval) {
                clearInterval(dgKeepAliveInterval);
                dgKeepAliveInterval = null;
            }
            if (restartDeepgramAfterTurn) {
                restartDeepgramAfterTurn = false;
                if (ws.readyState === WebSocket.OPEN) {
                    dgReconnectTimer = setTimeout(() => startDeepgram(), 100);
                }
                return;
            }
            if (ws.readyState === WebSocket.OPEN) {
                console.log("[Deepgram] Reconnecting in 2s...");
                dgReconnectTimer = setTimeout(() => startDeepgram(), 2000);
            }
        });
        if (dgKeepAliveInterval) clearInterval(dgKeepAliveInterval);
        dgKeepAliveInterval = setInterval(() => {
            if (!deepgramLive || deepgramLive.readyState !== WebSocket.OPEN) return;
            try {
                if (isFlux) {
                    // Flux (v2) requires standard WebSocket ping frames
                    deepgramLive.ping();
                } else {
                    // Nova-3 (v1) uses the legacy KeepAlive JSON message
                    deepgramLive.send(JSON.stringify({ type: 'KeepAlive' }));
                }
            } catch (err) {
                console.error('[Deepgram] KeepAlive failed:', err.message || err);
            }
        }, DEEPGRAM_KEEPALIVE_MS);
    };

    let dgKeepAliveInterval = null;

    const restartDeepgramForNextTurn = () => {
        if (STT_PROVIDER === "assemblyai") return;
        clearPendingAudio();
        transcriptBuffer = "";
        finalSegmentCount = 0;
        lastAppendedFinalTranscript = "";
        lastSentInterim = "";
        bestHeardTranscript = "";
        bestHeardTranscriptAt = 0;
        lastSpeechStartedAt = 0;
        lastFinalTranscriptAt = 0;
        if (finalizationTimer) {
            clearTimeout(finalizationTimer);
            finalizationTimer = null;
        }
        if (silenceTimer) {
            clearTimeout(silenceTimer);
            silenceTimer = null;
        }
        if (!deepgramLive || deepgramLive.readyState === WebSocket.CLOSED) {
            startDeepgram();
            return;
        }
        if (deepgramLive.readyState === WebSocket.CONNECTING) {
            deepgramSocketId++;
            deepgramOpen = false;
            deepgramLive.removeAllListeners();
            closeSttSocket(deepgramLive, 1000, "turn_complete_reset");
            deepgramLive = null;
            dgReconnectTimer = setTimeout(() => startDeepgram(), 100);
            return;
        }
        restartDeepgramAfterTurn = true;
        try {
            closeSttSocket(deepgramLive, 1000, "turn_complete_reset");
        } catch (err) {
            console.warn("[Deepgram] Reset close failed:", err.message || err);
            restartDeepgramAfterTurn = false;
            startDeepgram();
        }
    };

    const startAssemblyAI = () => {
        // Close any existing connection first to prevent "too many concurrent sessions"
        if (deepgramLive && deepgramLive.readyState !== WebSocket.CLOSED) {
            console.log("[AssemblyAI] Closing existing connection...");
            deepgramLive.removeAllListeners();
            closeSttSocket(deepgramLive);
            deepgramLive = null;
            deepgramOpen = false;
        }

        const params = {
            sample_rate: 16000,
            speech_model: "universal-streaming-english",
            format_turns: false,
            end_of_turn_confidence_threshold: 0.8, // Higher = wait for more confident silence
            min_end_of_turn_silence_when_confident: 1000, // Wait 1s of silence when confident
            max_turn_silence: 2500, // Max 2.5s silence before forcing turn end
            vad_threshold: 0.3,
            token: ASSEMBLYAI_API_KEY
        };
        const query = Object.entries(params).map(([k, v]) => `${k}=${encodeURIComponent(v)}`).join("&");
        const aaiUrl = `wss://streaming.assemblyai.com/v3/ws?${query}`;

        console.log("[AssemblyAI] Connecting...");
        deepgramLive = new WebSocket(aaiUrl);

        deepgramLive.on('open', () => {
            deepgramOpen = true;
            console.log('AssemblyAI Connected');
            flushPendingAudio();
        });

        deepgramLive.on('message', (data) => {
            try {
                const msg = JSON.parse(data.toString());

                // Handle session start
                if (msg.type === "Begin") {
                    console.log(`[AssemblyAI] Session Started: ${msg.id}`);
                    return;
                }

                // Handle errors
                if (msg.type === "Error") {
                    console.error("[AssemblyAI] Error:", JSON.stringify(msg, null, 2));
                    return;
                }

                // Handle v3 streaming format (turn_order field present)
                if (msg.turn_order !== undefined) {
                    const transcript = (msg.transcript || "").trim();
                    const isEndOfTurn = msg.end_of_turn === true;

                    if (transcript.length > 0) {
                        if (isEndOfTurn) {
                            console.log(`[STT] EndOfTurn: "${transcript}"`);
                            transcriptBuffer = (transcriptBuffer + " " + transcript).trim();
                            finalizeTranscriptTurn("aai_eot");
                        } else {
                            const displayText = (transcriptBuffer + " " + transcript).trim();
                            rememberHeardTranscript(displayText);
                            if (displayText !== lastSentInterim) {
                                console.log(`[STT] Interim: "${transcript}"`);
                                ws.send(JSON.stringify({ type: "interim", text: displayText }));
                                lastSentInterim = displayText;
                                resetSilenceTimer();
                            }
                        }
                    }
                    return;
                }
            } catch (e) {
                console.error("[AssemblyAI] Parse error:", e.message);
            }
        });

        deepgramLive.on('error', (err) => {
            console.error('[AssemblyAI] WebSocket error:', err.message || err);
        });

        deepgramLive.on('close', (code, reason) => {
            deepgramOpen = false;
            console.log(`[AssemblyAI] Closed (${code}): ${reason || "No reason given"}`);

            // Only reconnect if ESP32 is still connected and not a fatal error
            if (ws.readyState === WebSocket.OPEN && code !== 1008) {
                console.log("[AssemblyAI] Reconnecting in 2s...");
                setTimeout(() => startAssemblyAI(), 2000);
            } else if (code === 1008) {
                console.error("[AssemblyAI] FATAL: Too many sessions. Waiting 10s before retry...");
                setTimeout(() => startAssemblyAI(), 10000);
            }
        });
    };

    // --- Provider Selection ---
    if (STT_PROVIDER === "assemblyai") {
        startAssemblyAI();
    } else {
        startDeepgram(); // Default to Deepgram Nova-3
    }

    // Server-to-Client Heartbeat: keep proxies alive and detect dead connections
    esp32HeartbeatInterval = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({ type: "heartbeat", time: Date.now() }));
        }
    }, 15000);

    ws.on('message', (message, isBinary) => {
        if (isBinary) {
            const chunk = Buffer.isBuffer(message) ? message : Buffer.from(message);
            forwardAudioToDeepgram(chunk);
            return;
        }

        try {
            const data = JSON.parse(message.toString());
            if (data.type === "context") {
                latestContext = data.text;
                // Improve mode detection
                if (data.text.includes("Mode: AI")) {
                    currentRobotMode = "AI";
                } else if (data.text.includes("Mode: NORMAL")) {
                    currentRobotMode = "NORMAL";
                }
                console.log(`[Context Update] Mode Detected: ${currentRobotMode}`);
                console.log(`[Context Update] Mode: ${currentRobotMode}`);
            } else if (data.type === "telegram_response" || data.type === "telegram_alert") {
                const isAlert = data.type === "telegram_alert";
                console.log(`[Telegram] Relaying ${isAlert ? "alert" : "response"} to user...`);
                if (telegramBot && TELEGRAM_CHAT_ID) {
                    sendTelegramMessage(data.text, 'Markdown');
                } else {
                    console.warn("[Telegram] Bot not configured, relay failed");
                }
            } else if (data.type === "telegram_init") {
                console.log("[Telegram] Received credentials from ESP32");
                if (data.botToken) {
                    process.env.TELEGRAM_BOT_TOKEN = data.botToken;
                    if (data.chatId) {
                        process.env.TELEGRAM_CHAT_ID = data.chatId;
                        TELEGRAM_CHAT_ID = data.chatId;
                    }
                    // Initialize the bot webhook dynamically
                    initTelegramBot(data.botToken);
                }
            }
        } catch (e) {
            console.warn("[Server] Failed to parse text message:", e.message);
        }
    });

    ws.on('close', () => {
        console.log('ESP32 Disconnected');
        if (esp32Connection === ws) {
            esp32Connection = null;
        }
        if (dgKeepAliveInterval) clearInterval(dgKeepAliveInterval);
        if (dgReconnectTimer) clearTimeout(dgReconnectTimer);
        if (esp32HeartbeatInterval) clearInterval(esp32HeartbeatInterval);
        if (deepgramLive) {
            deepgramSocketId++;
            deepgramOpen = false;
            deepgramLive.removeAllListeners();
            closeSttSocket(deepgramLive);
            deepgramLive = null;
        }
        if (silenceTimer) clearTimeout(silenceTimer);
        if (finalizationTimer) clearTimeout(finalizationTimer);
    });
});

const PORT = process.env.PORT || 10000;
server.listen(PORT, () => {
    console.log(`Server listening on port ${PORT}`);
    console.log(`[Config] STT Provider: ${STT_PROVIDER}`);
    console.log(`[Config] AI Provider: ${AI_PROVIDER}`);
});
