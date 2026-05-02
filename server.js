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
const DEEPGRAM_TTS_MODEL = process.env.DEEPGRAM_TTS_MODEL || "aura-2-thalia-en";
const USE_DEEPGRAM_TTS = process.env.USE_DEEPGRAM_TTS !== "false";

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
            
            // DEBUG: log raw response so we can see the structure
            console.log("[Mistral RAW]", JSON.stringify(responseData).substring(0, 500));

            if (!res.ok) {
                const detail = responseData.detail || responseData.message || responseData.error || res.statusText;
                throw new Error(`Mistral API ${res.status}: ${JSON.stringify(detail).substring(0, 500)}`);
            }

            // Store the latest conversation ID. Mistral may return a new ID after appends.
            if (responseData.conversation_id || responseData.id) {
                conversationId = responseData.conversation_id || responseData.id;
                console.log(`[Mistral] Conversation: ${conversationId}`);
            }

            // Extract the text reply
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

            // Fallback for different API response shapes
            if (!fullResponse) {
                if (responseData.message?.content) fullResponse = responseData.message.content;
                else if (typeof responseData.message === "string") fullResponse = responseData.message;
            }

            // Clean up the response
            fullResponse = fullResponse.replace(/<think>[\s\S]*?<\/think>/g, '').trim();

            if (!fullResponse) {
                console.error("[Mistral] Empty reply — check API key and agent ID");
                fullResponse = "Sorry, my brain glitched. Ask me again!";
            }

            console.log(`[AI] Mistral Reply: ${fullResponse}`);
            try {
                const audioBuffer = await synthesizeDeepgramSpeech(fullResponse);
                if (audioBuffer) {
                    console.log(`[Deepgram TTS] Streaming ${audioBuffer.length} PCM bytes over WebSocket`);
                    ws.send(JSON.stringify({ type: "tts_audio", text: fullResponse }));
                    await sendDeepgramPcmToEsp(ws, audioBuffer);
                    if (ws.readyState === WebSocket.OPEN) {
                        ws.send(JSON.stringify({ type: "tts_audio_done" }));
                    }
                } else {
                    ws.send(JSON.stringify({ type: "tts", text: fullResponse }));
                }
            } catch (ttsErr) {
                console.error("[Deepgram TTS] Falling back to device TTS:", ttsErr.message);
                ws.send(JSON.stringify({ type: "tts", text: fullResponse }));
            }

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
