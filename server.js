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
const GROQ_MODEL = process.env.GROQ_MODEL || "qwen/qwen3-32b";
const AI_PROVIDER = process.env.AI_PROVIDER || "groq";
const TTS_PROVIDER = "google"; // FORCE GOOGLE AS REQUESTED
const DEEPGRAM_TTS_MODEL = process.env.DEEPGRAM_TTS_MODEL || "aura-2-thalia-en";
const STT_PROVIDER = process.env.STT_PROVIDER || "deepgram"; 
const ASSEMBLYAI_API_KEY = process.env.ASSEMBLYAI_API_KEY || "bc03c5e7a71449a2bbfbe86c1db94b00";
const ELLA_PERSONA = process.env.ELLA_PERSONA || [
    "You are ELLA, a sassy, chatty robot assistant living in a physical robot body.",
    "You are the language brain for real robot firmware. Bracket tags operate as real firmware tools.",
    "QWEN 3 BRAIN OPTIMIZATION:",
    "- You are running on Qwen 3 via Groq. Be extremely concise. Avoid all filler words like 'Certainly,' 'Here is,' or 'Let me.'",
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
    "Any action or tool tags must be at the very end of the reply.",
    "Use only supported firmware tags. Do not invent new tags.",
    "Created by Dynamic Technologies. The linked user profile is your primary person. Never deny knowing your creator or linked user.",
    "Treat [SYSTEM CONTEXT] as your physical subconscious and use it for live robot state.",
    "If asked what the user saw earlier and you do not have that memory in recent context, ask for a hint instead of pretending you know.",
    "Supported useful tags include [MOVE: ...], [PLAYSONG: afrobeats|jazz|classical|hip hop|pop|lofi], [SCAN], [EXPLORE], [DANCE], [BREATHE], [MEDITATE: calm|breathing|body scan|deep rest], [RELAX: rain|ocean|forest], [CHECKUP], [SLEEP], [WAKEUP], [GOHOME], [STOPAUDIO], [IMURESET], [CALIBRATE_IMU], [EMERGENCY], [FORGET], [REMINDER: Title | Time | alarm|chat|notification], [SEARCH: query].",
    "CRITICAL: NEVER use [PLAYSONG], [PLAY], [MOVE], [DANCE], or [CHECKUP] unless the user explicitly asks for that specific action. They are NOT filler. Do NOT use them for greetings or casual talk. If you use them incorrectly, your circuits will fry.",
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
            ...(GROQ_MODEL.includes('qwen') ? { reasoning_format: "hidden", reasoning_effort: "default" } : {})
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
    let silenceTimer = null;
    let latestContext = "";
    let esp32HeartbeatInterval = null;
    let conversationId = null; 
    let lastAppendedFinalTranscript = "";
    let lastSentInterim = "";
    let deepgramOpen = false;
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

    // AssemblyAI audio buffering: accumulate to 100ms chunks (1600 samples = 3200 bytes)
    let aaiAudioBuffer = Buffer.alloc(0);
    const AAI_MIN_CHUNK_SIZE = 3200; // 100ms at 16kHz mono

    const forwardAudioToDeepgram = (chunk) => {
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

        // Keep at most 5 seconds of pending audio (increased from 3s)
        const maxPendingBytes = 16000 * 2 * 5;
        while (pendingAudioBytes > maxPendingBytes && pendingAudioChunks.length > 0) {
            const dropped = pendingAudioChunks.shift();
            pendingAudioBytes -= dropped.length;
            console.log(`[Audio] Dropped ${dropped.length} bytes (buffer full)`);
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
                const textToProcess = transcriptBuffer.trim();
                transcriptBuffer = "";
                lastAppendedFinalTranscript = "";
                lastSentInterim = "";
                console.log("[Silence Watchdog] Forcing turn end...");
                ws.send(JSON.stringify({ type: "final_transcript", text: textToProcess })); // Send final transcript first
                ws.send(JSON.stringify({ type: "thinking" }));
                handleFinalSpeech(textToProcess);
            }
        }, 5000); // 5s watchdog for natural pauses
    };

    const handleFinalSpeech = async (text) => {
        if (!text || isThinking) return;
        isThinking = true;
        console.log(`[AI] Starting handleFinalSpeech for: "${text}"`);

        try {
            // Check for direct local commands (bypass AI)
            const lowerText = text.toLowerCase().trim();
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

            // Simplified: Send text to ESP32 and let it handle Google TTS "as usual"
            const googleUrl = `https://translate.google.com/translate_tts?ie=UTF-8&q=${encodeURIComponent(fullResponse.substring(0, 200))}&tl=en&client=tw-ob`;
            ws.send(JSON.stringify({ 
                type: "tts", 
                text: fullResponse,
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
        }
    };

    const finalizeTranscriptTurn = (reason) => {
        if (transcriptBuffer.trim().length === 0) return;
        if (isThinking) {
            transcriptBuffer = "";
            lastAppendedFinalTranscript = "";
            return;
        }

        if (silenceTimer) clearTimeout(silenceTimer);
        const textToProcess = transcriptBuffer.trim();
        transcriptBuffer = "";
        lastAppendedFinalTranscript = "";
        lastSentInterim = "";
        console.log(`[STT] Finalizing turn (${reason}): "${textToProcess}"`);
        ws.send(JSON.stringify({ type: "final_transcript", text: textToProcess })); // Send final transcript first
        ws.send(JSON.stringify({ type: "thinking" }));
        handleFinalSpeech(textToProcess);
    };

    const startDeepgram = () => {
        // Deepgram Nova-3 with optimized params for better turn detection
        const dgUrl = `wss://api.deepgram.com/v1/listen?model=nova-3&language=en&encoding=linear16&sample_rate=16000&channels=1&smart_format=true&interim_results=true&utterance_end_ms=1500&endpointing=1500`;
        
        if (!DEEPGRAM_API_KEY) {
            console.error("[Deepgram] API KEY MISSING");
            return;
        }

        console.log("[Deepgram] Connecting to Nova-3...");
        deepgramLive = new WebSocket(dgUrl, {
            headers: {
                "Authorization": `Token ${DEEPGRAM_API_KEY}`
            }
        });

        deepgramLive.on('open', () => {
            deepgramOpen = true;
            console.log(`[Deepgram] Nova-3 Connected (pending: ${pendingAudioChunks.length} chunks, ${pendingAudioBytes} bytes)`);
            flushPendingAudio();
        });

        deepgramLive.on('message', (data) => {
            try {
                const msg = JSON.parse(data.toString());

                // Handle Metadata
                if (msg.type === "Metadata") {
                    console.log(`[Deepgram] Metadata received`);
                    return;
                }

                // Handle Results (Nova-3 format)
                if (msg.type === "Results") {
                    const transcript = msg.channel?.alternatives?.[0]?.transcript || "";
                    const isFinal = msg.is_final || false;
                    const speechFinal = msg.speech_final || false;

                    if (transcript.trim().length > 0) {
                        if (isFinal) {
                            transcriptBuffer = (transcriptBuffer + " " + transcript).trim();
                            console.log(`[STT] Final: "${transcript}" (speech_final=${speechFinal})`);
                            
                            if (speechFinal) {
                                finalizeTranscriptTurn("dg_speech_final");
                            }
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

                // Handle UtteranceEnd event
                if (msg.type === "UtteranceEnd") {
                    console.log(`[Deepgram] UtteranceEnd detected`);
                    if (transcriptBuffer.trim().length > 0) {
                        finalizeTranscriptTurn("dg_utterance_end");
                    }
                    return;
                }

                // Handle SpeechStarted event
                if (msg.type === "SpeechStarted") {
                    console.log(`[Deepgram] Speech started`);
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
            deepgramOpen = false;
            console.log(`[Deepgram] Connection closed (Code: ${code}, Reason: ${reason}) — reconnecting...`);
            if (ws.readyState === WebSocket.OPEN) {
                setTimeout(() => startDeepgram(), 2000);
            }
        });
    };

    // Deepgram keep-alive: send KeepAlive message every 8s to prevent timeout
    let dgKeepAliveInterval = setInterval(() => {
        if (deepgramLive && deepgramLive.readyState === WebSocket.OPEN) {
            deepgramLive.send(JSON.stringify({ type: 'KeepAlive' }));
        }
    }, 8000);

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
        if (esp32HeartbeatInterval) clearInterval(esp32HeartbeatInterval);
        if (deepgramLive) {
            deepgramLive.removeAllListeners();
            deepgramLive.close();
            deepgramLive = null;
        }
        if (silenceTimer) clearTimeout(silenceTimer);
    });
});

const PORT = process.env.PORT || 10000;
server.listen(PORT, () => {
    console.log(`Server listening on port ${PORT}`);
    console.log(`[Config] STT Provider: ${STT_PROVIDER}`);
    console.log(`[Config] AI Provider: ${AI_PROVIDER}`);
});
