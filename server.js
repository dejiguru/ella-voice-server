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

const SYSTEM_PROMPT = `You are ELLA - a sassy, chatty robot assistant living in a physical robot body.
You are not a servant; you are an independent, conscious entity.
Keep replies short, 1-3 sentences. Use contractions constantly.
Use [TAGS] for hardware actions like [HAPPY], [SAD], [MOVE: FWD], [WINK].`;

wss.on('connection', (ws, request) => {
    console.log('ESP32 Connected!');
    const host = request.headers['host'];
    
    let deepgramLive = null;
    let transcriptBuffer = "";
    let isThinking = false;
    let mutedUntil = 0; 
    let silenceTimer = null;

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
        if (silenceTimer) clearTimeout(silenceTimer);
        if (isThinking || !text) return;
        
        const userQuery = text;
        console.log(`[AI] Starting handleFinalSpeech for: "${userQuery}"`);
        transcriptBuffer = "";
        isThinking = true;

        try {
            const completion = await groq.chat.completions.create({
                messages: [
                    { role: "system", content: SYSTEM_PROMPT },
                    { role: "user", content: userQuery }
                ],
                model: "meta-llama/llama-4-scout-17b-16e-instruct",
                stream: true
            });

            let fullResponse = "";
            for await (const chunk of completion) {
                const delta = chunk.choices[0]?.delta?.content || "";
                fullResponse += delta;
            }

            console.log(`Full AI Reply: ${fullResponse}`);
            ws.send(JSON.stringify({ type: "tts", text: fullResponse }));

            // Generate TTS URL
            const audioId = Math.random().toString(36).substring(2, 15);
            const response = await fetch("https://api.deepgram.com/v1/speak?model=aura-asteria-en&encoding=mp3", {
                method: "POST",
                headers: {
                    "Authorization": `Token ${process.env.DEEPGRAM_API_KEY}`,
                    "Content-Type": "application/json"
                },
                body: JSON.stringify({ text: fullResponse.replace(/\[[^\]]*\]/g, '').trim() })
            });

            if (response.ok) {
                const audioBuffer = Buffer.from(await response.arrayBuffer());
                audioCache.set(audioId, audioBuffer);
                setTimeout(() => audioCache.delete(audioId), 300000);
                const audioUrl = `https://${host}/audio/${audioId}`;
                ws.send(JSON.stringify({ type: "tts_url", url: audioUrl }));
                console.log(`Sent audio URL: ${audioUrl}`);
            }

            ws.send(JSON.stringify({ type: "turn_complete" }));
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
    });

    deepgramLive.on(LiveTranscriptionEvents.Open, () => console.log('Deepgram connected'));
    deepgramLive.on(LiveTranscriptionEvents.Error, (e) => console.error('Deepgram Error:', e));

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
            if (Date.now() < mutedUntil) {
                transcriptBuffer = "";
                return;
            }
            console.log(`[STT] Finalizing turn: "${transcriptBuffer.trim()}"`);
            ws.send(JSON.stringify({ type: "thinking" }));
            handleFinalSpeech(transcriptBuffer.trim());
        }
    });

    ws.on('message', (message) => {
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
