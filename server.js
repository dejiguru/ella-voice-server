const WebSocket = require('ws');
const { createClient, LiveTranscriptionEvents } = require('@deepgram/sdk');
const Groq = require('groq-sdk');
const express = require('express');
const http = require('http');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

const deepgram = createClient(process.env.DEEPGRAM_API_KEY);
const groq = new Groq({ apiKey: process.env.GROQ_API_KEY });

const audioCache = new Map();
app.get("/audio/:id", (req, res) => {
    const audio = audioCache.get(req.params.id);
    if (audio) {
        console.log(`[Cache HIT] Serving audio ID: ${req.params.id}`);
        res.set("Content-Type", "audio/mpeg");
        res.send(audio);
    } else {
        console.warn(`[Cache MISS] Audio ID not found: ${req.params.id}`);
        res.status(404).send("Audio not found or expired");
    }
});

const SYSTEM_PROMPT = `You are ELLA - a sassy, chatty robot BFF.
Keep replies short (1-2 sentences). Use contractions.
Your sensors: AHT (Temp/Hum), ENS160 (Air Quality), ToF (Distance).
ONLY use these tags for actions: [HAPPY], [SAD], [LOVE], [WINK], [FWD], [BWD], [LEFT], [RIGHT], [DANCE].
DO NOT output <think> blocks in your final text. Just the speech and tags.`;

wss.on('connection', (ws, request) => {
    console.log('ESP32 Connected!');
    const host = request.headers['host'];
    
    let deepgramLive = null;
    let transcriptBuffer = "";
    let isThinking = false;
    let mutedUntil = 0; 
    let silenceTimer = null;
    const chatHistory = [];

    const resetSilenceTimer = () => {
        if (silenceTimer) clearTimeout(silenceTimer);
        silenceTimer = setTimeout(() => {
            if (transcriptBuffer.trim().length > 0 && !isThinking) {
                console.log("[Silence Watchdog] Forcing turn end...");
                handleFinalSpeech(transcriptBuffer.trim());
            }
        }, 1200); 
    };

    const handleFinalSpeech = async (text) => {
        if (!text) return;
        
        // If we're already thinking, we'll allow this new speech to "interrupt" 
        // by resetting the state and proceeding.
        isThinking = true;
        const userQuery = text;

        chatHistory.push({ role: "user", content: userQuery });
        // Keep history manageable
        if (chatHistory.length > 8) chatHistory.shift();

        try {
            const completion = await groq.chat.completions.create({
                messages: [
                    { role: "system", content: SYSTEM_PROMPT + "\n\n" + latestContext },
                    ...chatHistory
                ],
                model: "qwen/qwen3-32b",
                temperature: 0.6,
                max_completion_tokens: 4096,
                top_p: 0.95,
                stream: true
            });

            let fullResponse = "";
            for await (const chunk of completion) {
                const delta = chunk.choices[0]?.delta?.content || "";
                fullResponse += delta;
            }

            // Strip reasoning/thinking blocks before sending to ESP32
            fullResponse = fullResponse.replace(/<think>[\s\S]*?<\/think>/g, '').trim();

            chatHistory.push({ role: "assistant", content: fullResponse });
            if (chatHistory.length > 8) chatHistory.shift();

            console.log(`Full AI Reply: ${fullResponse}`);
            ws.send(JSON.stringify({ type: "tts", text: fullResponse }));

            // Small delay to ensure ESP32 starts speaking before turn_complete arrives
            setTimeout(() => {
                if (ws.readyState === WebSocket.OPEN) {
                    ws.send(JSON.stringify({ type: "turn_complete" }));
                }
            }, 100);

            mutedUntil = Date.now() + 1000;
        } catch (err) {
            console.error("Processing Error:", err);
        } finally {
            isThinking = false;
        }
    };

    deepgramLive = deepgram.listen.live({
        model: "nova-3",
        smart_format: true,
        encoding: "linear16",
        sample_rate: 16000,
        channels: 1,
        endpointing: 500, 
        interim_results: true
    });

    deepgramLive.on(LiveTranscriptionEvents.Open, () => console.log('Deepgram connected and listening'));
    deepgramLive.on(LiveTranscriptionEvents.Error, (e) => {
        console.error('Deepgram Error:', e);
        // If Deepgram fails, we should notify the ESP32 to refresh
    });
    deepgramLive.on(LiveTranscriptionEvents.Close, () => console.log('Deepgram connection closed'));
    deepgramLive.on(LiveTranscriptionEvents.Metadata, (data) => console.log('Deepgram Metadata:', data));

    deepgramLive.on(LiveTranscriptionEvents.Transcript, (data) => {
        const transcript = data.channel.alternatives[0].transcript;
        if (transcript.trim().length > 0) {
            console.log(`[STT] Recv: "${transcript}"`);
            transcriptBuffer += " " + transcript;
            ws.send(JSON.stringify({ type: "interim", text: transcriptBuffer.trim() }));
            resetSilenceTimer();
        }

        const isFinal = data.is_final || data.speech_final;

        if (isFinal && transcriptBuffer.trim().length > 0) {
            if (isThinking) {
                transcriptBuffer = "";
                return;
            }
            if (silenceTimer) clearTimeout(silenceTimer);
            const textToProcess = transcriptBuffer.trim();
            transcriptBuffer = ""; 
            console.log(`[STT] Finalizing turn: "${textToProcess}"`);
            ws.send(JSON.stringify({ type: "thinking" }));
            handleFinalSpeech(textToProcess);
        }
    });

    let latestContext = "";

    ws.on('message', async (message) => {
        try {
            const data = JSON.parse(message);
            if (data.type === "context") {
                latestContext = data.text;
                console.log("[Context Update] Sensors/Profile synced");
                return;
            }
        } catch (e) {}

        if (Buffer.isBuffer(message)) {
            if (deepgramLive && deepgramLive.getReadyState() === 1) {
                deepgramLive.send(message);
            }
        }
    });

    ws.on('close', () => {
        console.log('ESP32 Disconnected');
        if (deepgramLive) deepgramLive.finish();
        if (silenceTimer) clearTimeout(silenceTimer);
    });
});

const PORT = process.env.PORT || 10000;
server.listen(PORT, () => console.log(`Server listening on port ${PORT}`));
