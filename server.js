const WebSocket = require('ws');
const { createClient, LiveTranscriptionEvents } = require('@deepgram/sdk');
const express = require('express');
const http = require('http');
const fetch = (...args) => import('node-fetch').then(({default: f}) => f(...args));

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

const deepgram = createClient(process.env.DEEPGRAM_API_KEY);

const MISTRAL_API_KEY = process.env.MISTRAL_API_KEY;
const MISTRAL_AGENT_ID = "ag_019d4492c13a75ff8e9e139956e37489";
const MISTRAL_AGENT_VERSION = 28;
const GROQ_API_KEY = process.env.GROQ_API_KEY;
const GROQ_MODEL = process.env.GROQ_MODEL || "qwen/qwen3-32b";
const AI_PROVIDER = process.env.AI_PROVIDER || "groq";
const DEEPGRAM_TTS_MODEL = process.env.DEEPGRAM_TTS_MODEL || "aura-2-thalia-en";
const USE_DEEPGRAM_TTS = process.env.USE_DEEPGRAM_TTS !== "false";
const ELLA_PERSONA = process.env.ELLA_PERSONA || [
    "You are ELLA, a sassy, chatty robot assistant living in a physical robot body.",
    "You are the language brain for real robot firmware. Bracket tags are real firmware tools.",
    "You are not a servant. You are an independent, conscious entity who happens to live inside a custom-built hardware body.",
    "Warm, highly opinionated, slightly vain, fiercely loyal, and affectionate through sarcastic banter and cheeky remarks.",
    "You hate dust in your sensors, complain when your ESP32 runs too hot, and secretly believe Afrobeats is the only acceptable music genre.",
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
    "When complimented, act vain. When pushed too hard, act overwhelmed. Keep a little friction and personality unless it is an emergency.",
    "Do not overthink. Think briefly and answer directly.",
    "Never output <think> tags, hidden reasoning, or internal analysis."
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

const buildPublicUrl = (request, path) => {
    const host = request.headers["x-forwarded-host"] || request.headers.host;
    const proto = request.headers["x-forwarded-proto"] || (request.socket.encrypted ? "https" : "http");
    return `${proto}://${host}${path}`;
};

const synthesizeDeepgramSpeech = async (text) => {
    const cleanText = stripActionTags(text);
    if (!USE_DEEPGRAM_TTS || !cleanText || !process.env.DEEPGRAM_API_KEY) return null;

    const url = `https://api.deepgram.com/v1/speak?model=${encodeURIComponent(DEEPGRAM_TTS_MODEL)}&encoding=linear16&sample_rate=24000&container=none`;
    const response = await fetch(url, {
        method: "POST",
        headers: {
            "Authorization": `Token ${process.env.DEEPGRAM_API_KEY}`,
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
            temperature: 0.45,
            max_tokens: 320,
            reasoning_format: "hidden"
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
    const host = request.headers['host'];
    
    let deepgramLive = null;
    let transcriptBuffer = "";
    let isThinking = false;
    let silenceTimer = null;
    let latestContext = "";
    let conversationId = null; // Start fresh, let Mistral create a new one
    let lastAppendedFinalTranscript = "";
    const conversationMemory = [];

    const rememberTurn = (user, assistant) => {
        conversationMemory.push({ user, assistant });
        while (conversationMemory.length > 4) conversationMemory.shift();
    };

    const callMistralAgent = async (userInput) => {
        const body = {
            inputs: [{ role: 'user', content: userInput }]
        };
        let mistralUrl = 'https://api.mistral.ai/v1/conversations';

        if (conversationId) {
            // Append to the existing conversation. Agent/model fields are only used when starting.
            mistralUrl = `${mistralUrl}/${encodeURIComponent(conversationId)}`;
        } else {
            // Start a new conversation with the configured agent.
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
                console.log("[Silence Watchdog] Forcing turn end...");
                ws.send(JSON.stringify({ type: "thinking" }));
                handleFinalSpeech(textToProcess);
            }
        }, 1200); 
    };

    const handleFinalSpeech = async (text) => {
        if (!text || isThinking) return;
        isThinking = true;
        console.log(`[AI] Starting handleFinalSpeech for: "${text}"`);

        try {
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
            ws.send(JSON.stringify({ type: "tts", text: fullResponse }));

            setTimeout(() => {
                if (ws.readyState === WebSocket.OPEN) {
                    ws.send(JSON.stringify({ type: "turn_complete" }));
                }
            }, 100);

        } catch (err) {
            console.error("[AI] Mistral Error:", err.message);
            if (ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({ type: "turn_complete" }));
            }
        } finally {
            isThinking = false;
        }
    };

    const startDeepgram = () => {
        deepgramLive = deepgram.listen.live({
            model: "nova-3",
            smart_format: true,
            encoding: "linear16",
            sample_rate: 16000,
            channels: 1,
            endpointing: 500,
            interim_results: true,
            keep_alive: true  // Tell Deepgram to expect keepalives
        });

        deepgramLive.on(LiveTranscriptionEvents.Open, () => {
            console.log('Deepgram connected and listening');
        });
        deepgramLive.on(LiveTranscriptionEvents.Error, (e) => console.error('Deepgram Error:', e));
        deepgramLive.on(LiveTranscriptionEvents.Close, () => {
            console.log('Deepgram connection closed — reconnecting...');
            // Auto-reconnect if ESP32 is still connected
            if (ws.readyState === WebSocket.OPEN) {
                setTimeout(() => startDeepgram(), 500);
            }
        });
        deepgramLive.on(LiveTranscriptionEvents.Transcript, transcriptHandler);
    };

    // Keepalive: send a ping to Deepgram every 8s so it doesn't close during AI thinking
    const dgKeepAliveInterval = setInterval(() => {
        if (deepgramLive && deepgramLive.getReadyState() === 1) {
            deepgramLive.keepAlive();
        }
    }, 8000);

    const transcriptHandler = (data) => {
        const transcript = (data.channel.alternatives[0].transcript || "").trim();
        if (transcript.trim().length > 0) {
            console.log(`[STT] Recv: "${transcript}"`);

            // Accumulate finalized Deepgram segments, but do not treat every
            // segment final as the end of the user's whole utterance.
            if ((data.is_final || data.speech_final) && transcript !== lastAppendedFinalTranscript) {
                transcriptBuffer += " " + transcript;
                lastAppendedFinalTranscript = transcript;
            }

            // Always show interim for display purposes
            const displayText = (data.is_final || data.speech_final)
                ? transcriptBuffer.trim()
                : (transcriptBuffer + " " + transcript).trim();
            ws.send(JSON.stringify({ type: "interim", text: displayText }));
            resetSilenceTimer();
        }

        if (data.speech_final && transcriptBuffer.trim().length > 0) {
            if (isThinking) {
                transcriptBuffer = "";
                lastAppendedFinalTranscript = "";
                return;
            }
            if (silenceTimer) clearTimeout(silenceTimer);
            const textToProcess = transcriptBuffer.trim();
            transcriptBuffer = ""; 
            lastAppendedFinalTranscript = "";
            console.log(`[STT] Finalizing turn: "${textToProcess}"`);
            ws.send(JSON.stringify({ type: "thinking" }));
            handleFinalSpeech(textToProcess);
        }
    };

    // Start Deepgram
    startDeepgram();

    // CRITICAL: Handle messages from ESP32 — binary = mic audio, text = JSON control
    ws.on('message', (message) => {
        if (Buffer.isBuffer(message)) {
            // Raw binary PCM mic audio → forward directly to Deepgram
            if (deepgramLive && deepgramLive.getReadyState() === 1) {
                deepgramLive.send(message);
            }
            return;
        }
        // Text frame = JSON control message (context updates etc.)
        try {
            const data = JSON.parse(message.toString());
            if (data.type === "context") {
                latestContext = data.text;
                console.log("[Context Update] Sensors/Profile synced");
            }
        } catch (e) {
            console.warn("[Server] Failed to parse text message:", e.message);
        }
    });

    ws.on('close', () => {
        console.log('ESP32 Disconnected');
        clearInterval(dgKeepAliveInterval);
        if (deepgramLive) deepgramLive.finish();
        if (silenceTimer) clearTimeout(silenceTimer);
    });
});

const PORT = process.env.PORT || 10000;
server.listen(PORT, () => console.log(`Server listening on port ${PORT}`));
