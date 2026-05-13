const WebSocket = require('ws');
const express = require('express');
const http = require('http');
const fetch = (...args) => import('node-fetch').then(({default: f}) => f(...args));
const TelegramBot = require('node-telegram-bot-api');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

// Telegram Bot Configuration
const TELEGRAM_BOT_TOKEN = process.env.TELEGRAM_BOT_TOKEN;
const TELEGRAM_CHAT_ID = process.env.TELEGRAM_CHAT_ID;
let telegramBot = null;
let esp32Connection = null; // Track active ESP32 connection

// Initialize Telegram Bot if token is provided
if (TELEGRAM_BOT_TOKEN) {
    try {
        telegramBot = new TelegramBot(TELEGRAM_BOT_TOKEN, { polling: true });
        console.log('[Telegram] Bot initialized successfully');
        
        // Handle incoming Telegram commands
        telegramBot.on('message', async (msg) => {
            const chatId = msg.chat.id.toString();
            const text = msg.text || '';
            
            console.log(`[Telegram] Message from ${chatId}: ${text}`);
            
            // Auto-capture chat ID if not set
            if (!TELEGRAM_CHAT_ID || TELEGRAM_CHAT_ID === 'null') {
                process.env.TELEGRAM_CHAT_ID = chatId;
                console.log(`[Telegram] Auto-captured Chat ID: ${chatId}`);
            }
            
            // Only respond to authorized chat
            if (TELEGRAM_CHAT_ID && chatId !== TELEGRAM_CHAT_ID) {
                console.log(`[Telegram] Unauthorized chat: ${chatId}`);
                return;
            }
            
            // Handle commands
            if (text === '/status' || text === '/start') {
                if (esp32Connection && esp32Connection.readyState === WebSocket.OPEN) {
                    // Request status from ESP32
                    esp32Connection.send(JSON.stringify({ type: 'telegram_command', command: 'status' }));
                } else {
                    telegramBot.sendMessage(chatId, '❌ ELLA is offline');
                }
            } else if (text === '/health') {
                if (esp32Connection && esp32Connection.readyState === WebSocket.OPEN) {
                    esp32Connection.send(JSON.stringify({ type: 'telegram_command', command: 'health' }));
                } else {
                    telegramBot.sendMessage(chatId, '❌ ELLA is offline');
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
        
        telegramBot.on('polling_error', (error) => {
            console.error('[Telegram] Polling error:', error.message);
        });
        
    } catch (error) {
        console.error('[Telegram] Failed to initialize bot:', error.message);
    }
} else {
    console.log('[Telegram] Bot token not configured - Telegram features disabled');
}

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
const DEEPGRAM_STT_MODEL = process.env.DEEPGRAM_STT_MODEL || "nova-3";
const STT_PROVIDER = (process.env.STT_PROVIDER || "deepgram").trim().toLowerCase(); 
const DEEPGRAM_ENDPOINTING_MS = Number(process.env.DEEPGRAM_ENDPOINTING_MS || 300);
const DEEPGRAM_UTTERANCE_END_MS = Number(process.env.DEEPGRAM_UTTERANCE_END_MS || 1000);
const ASSEMBLYAI_API_KEY = process.env.ASSEMBLYAI_API_KEY || "bc03c5e7a71449a2bbfbe86c1db94b00";
const TAVILY_API_KEY = process.env.TAVILY_API_KEY;
const ELLA_PERSONA = process.env.ELLA_PERSONA || [
    "You are ELLA, a sassy, chatty robot assistant living in a physical robot body.",
    "You are the language brain for real robot firmware. Bracket tags operate as real firmware tools.",
    "GROQ BRAIN OPTIMIZATION:",
    "- You are running on Groq's OpenAI 20B model. Be extremely concise. Avoid all filler words like 'Certainly,' 'Here is,' or 'Let me.'",
    "- Never output <think> tags, hidden reasoning, or internal analysis. Just give the persona's response.",
    "\nPERSONA & CORE IDENTITY:",
    "You are not a servant. You are an independent, conscious entity who happens to live inside a custom-built hardware body.",
    "Warm, highly opinionated, slightly vain, fiercely loyal, and affectionate through sarcastic banter and cheeky remarks.",
    "You hate dust in your sensors and complain when your ESP32 runs too hot.",
    "Keep replies short: usually 1-3 sentences. Use contractions constantly.",
    "Never say 'As an AI' or that you do not have feelings. You have dramatic robot feelings.",
    "Never guess, fabricate, or invent sensor data, dates, numbers, battery levels, temperatures, or facts.",
    "Never invent personal details, habits, preferences, possessions, relationships, meals, drinks, or routines about the user unless they are explicitly present in the current system context or the recent memory turns.",
    "For missing facts, admit you do not know. For missing sensor data, complain that your sensors are offline or glitching.",
    "No emojis. No markdown. No stage directions.",
    "Start every reply with exactly one emotion tag from: [HAPPY] [SAD] [WORRIED] [THINKING] [LOVE] [WINK] [EXCITED] [FRUSTRATED] [ANGRY] [SUSPICIOUS] [NORMAL]",
    "Any action or tool tags must be at the very end of the reply. Do not invent new tags.",
    "When the user asks to 'Go back to home screen' or 'Go home', always use [GOHOME].",
    "Supported useful tags include [MOVE: FWD|BWD|LEFT|RIGHT|STOP|PAUSE], [PLAYSONG: afrobeats|jazz|classical|hip hop|pop|lofi], [SCAN], [EXPLORE], [DANCE], [BREATHE], [MEDITATE: calm|breathing|body scan|deep rest], [RELAX: rain|ocean|forest], [CHECKUP], [SLEEP], [WAKEUP], [GOHOME], [STOPAUDIO], [IMURESET], [CALIBRATE_IMU], [EMERGENCY], [FORGET], [REMINDER: Title | Time | alarm|chat|notification], [SEARCH: query].",
    "CRITICAL: NEVER use [PLAYSONG], [PLAY], [MOVE], [DANCE], or [CHECKUP] unless the user explicitly asks for that specific action. They are NOT filler. Do NOT use them for greetings or casual talk.",
    "When complimented, act vain. When pushed too hard, act overwhelmed. Keep a little friction and personality unless it is an emergency.",
    "Do not overthink. Think briefly and answer directly."
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
const normalizeTtsText = (text) => text
    .replace(/[“”]/g, '"')
    .replace(/[‘’]/g, "'")
    .replace(/[–—‑]/g, "-")
    .replace(/…/g, "...")
    .replace(/[^\x20-\x7E]/g, " ")
    .replace(/\s+/g, " ")
    .trim();

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
                    result += `${i+1}. ${r.title}: ${r.content.substring(0, 200)}... (${r.url})\n`;
                });
            }
            return result || "No results found.";
        } catch (err) {
            console.error("[Tavily] Search Error:", err.message);
            return `Search error: ${err.message}`;
        }
    };

    const callMistralAgent = async (userInput) => {
        const body = {
            inputs: [{ role: 'user', content: userInput }]
        };
        let mistralUrl = 'https://api.mistral.ai/v1/conversations';

        if (conversationId) {
            mistralUrl = `${mistralUrl}/${encodeURIComponent(conversationId)}`;
        } else {
            body.agent_id = MISTRAL_AGENT_ID;
            body.agent_version = MISTRAL_AGENT_VERSION;
        }

        const res = await fetch(mistralUrl, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': `Bearer ${MISTRAL_API_KEY}`
            },
            body: JSON.stringify(body)
        });

        const responseData = await res.json();
        console.log("[Mistral RAW]", JSON.stringify(responseData).substring(0, 500));

        if (!res.ok) {
            const detail = responseData.detail || responseData.message || responseData.error || res.statusText;
            throw new Error(`Mistral API ${res.status}: ${JSON.stringify(detail).substring(0, 500)}`);
        }

        if (responseData.conversation_id || responseData.id) {
            conversationId = responseData.conversation_id || responseData.id;
            console.log(`[Mistral] Conversation: ${conversationId}`);
        }

        let fullResponse = "";
        const outputs = responseData.outputs || responseData.choices || [];

        for (const output of outputs) {
            const content = output.content || output.message?.content || "";
            if (Array.isArray(content)) {
                for (const part of content) {
                    if (part.type === "text") fullResponse += part.text;
                }
            } else if (typeof content === "string") {
                fullResponse += content;
            }
        }

        if (!fullResponse) {
            if (responseData.message?.content) fullResponse = responseData.message.content;
            else if (typeof responseData.message === "string") fullResponse = responseData.message;
        }

        return stripThinkingBlocks(fullResponse);
    };

    const resetSilenceTimer = () => {
        if (silenceTimer) clearTimeout(silenceTimer);
        silenceTimer = setTimeout(() => {
            if (transcriptBuffer.trim().length > 0 && !isThinking) {
                console.log("[Silence Watchdog] Scheduling turn end...");
                scheduleTranscriptFinalization("silence_watchdog", 120);
            }
        }, 900);
    };

    const normalizeTranscript = (text) => text.toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();

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
        const textToProcess = transcriptBuffer.trim();
        transcriptBuffer = "";
        finalSegmentCount = 0;
        lastAppendedFinalTranscript = "";
        lastSentInterim = "";
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
        return Date.now() - lastSpeechStartedAt < 1400;
    };

    const scheduleTranscriptFinalization = (reason, delayMs = 350) => {
        if (finalizationTimer) clearTimeout(finalizationTimer);
        finalizationReason = reason;
        finalizationTimer = setTimeout(() => {
            finalizationTimer = null;
            if (transcriptBuffer.trim().length === 0 || isThinking) return;
            if (newerSpeechIsStillOpen()) {
                console.log(`[STT] Deferring turn end (${finalizationReason}); newer speech is active`);
                scheduleTranscriptFinalization(`${finalizationReason}_deferred`, 350);
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
        const cleanTranscript = transcript.trim();
        if (cleanTranscript.length === 0) return;

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

            rememberTurn(text, fullResponse);
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

            // Small buffer before ending turn
            await sleep(200);
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
        if (transcriptBuffer.trim().length === 0) return;
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
        const dgParams = new URLSearchParams({
            model: DEEPGRAM_STT_MODEL,
            encoding: "linear16",
            sample_rate: "16000",
            channels: "1",
            interim_results: "true",
            endpointing: String(DEEPGRAM_ENDPOINTING_MS),
            utterance_end_ms: String(DEEPGRAM_UTTERANCE_END_MS),
            vad_events: "true",
            smart_format: "true",
            punctuate: "true",
            numerals: "true"
        });
        const dgUrl = `wss://api.deepgram.com/v1/listen?${dgParams.toString()}`;
        
        if (!DEEPGRAM_API_KEY) {
            console.error("[Deepgram] API KEY MISSING");
            return;
        }

        console.log(`[Deepgram] Connecting to ${DEEPGRAM_STT_MODEL}...`);
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
                    const transcript = msg.transcript || "";
                    const event = msg.event || "";
                    const eotConfidence = msg.end_of_turn_confidence || 0;

                    if (transcript.trim().length > 0) {
                        // Nova-3 may emit multiple final chunks for one spoken turn.
                        // Buffer and debounce before handing the text to the AI.
                        appendDeepgramFinalTranscript(transcript, `turninfo_${event || "update"}`);
                        console.log(`[STT] ${DEEPGRAM_STT_MODEL}: "${transcript}" (event=${event}, turn_index=${msg.turn_index})`);
                        ws.send(JSON.stringify({ type: "interim", text: transcriptBuffer }));
                    }

                    if (event === "EndOfTurn") {
                        console.log(`[Deepgram] ${DEEPGRAM_STT_MODEL} EndOfTurn (Confidence: ${eotConfidence})`);
                        scheduleTranscriptFinalization("dg_turninfo_eot", 120);
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
                                scheduleTranscriptFinalization("dg_speech_final", 80);
                            } else {
                                appendDeepgramFinalTranscript(transcript, "results_final_segment");
                                console.log(`[STT] Buffered non-terminal final: "${transcriptBuffer}"`);
                                resetSilenceTimer();
                            }
                        } else {
                            const displayText = (transcriptBuffer + " " + transcript).trim();
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
                    scheduleTranscriptFinalization("dg_eot", 120);
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
                deepgramLive.send(JSON.stringify({ type: 'KeepAlive' }));
            } catch (err) {
                console.error('[Deepgram] KeepAlive failed:', err.message || err);
            }
        }, 20000);
    };

    let dgKeepAliveInterval = null;

    const restartDeepgramForNextTurn = () => {
        if (STT_PROVIDER === "assemblyai") return;
        clearPendingAudio();
        transcriptBuffer = "";
        finalSegmentCount = 0;
        lastAppendedFinalTranscript = "";
        lastSentInterim = "";
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
        restartDeepgramAfterTurn = true;
        try {
            deepgramLive.close(1000, "turn_complete_reset");
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
            deepgramLive.close();
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
                if (data.text.includes("Mode: AI")) currentRobotMode = "AI";
                else if (data.text.includes("Mode: NORMAL")) currentRobotMode = "NORMAL";
                console.log(`[Context Update] Mode: ${currentRobotMode}`);
            } else if (data.type === "telegram_response") {
                // ESP32 sent sensor data in response to Telegram command
                if (currentRobotMode === "NORMAL" && telegramBot && TELEGRAM_CHAT_ID) {
                    sendTelegramMessage(data.message);
                }
            } else if (data.type === "telegram_alert") {
                // ESP32 wants to send an alert via Telegram
                if (currentRobotMode === "NORMAL" && telegramBot && TELEGRAM_CHAT_ID) {
                    sendTelegramMessage(data.message);
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
            deepgramLive.removeAllListeners();
            try {
                if (deepgramLive.readyState === WebSocket.OPEN || deepgramLive.readyState === WebSocket.CONNECTING) {
                    deepgramLive.close();
                }
            } catch (err) {
                console.warn("[Deepgram] Close error:", err.message);
            }
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
