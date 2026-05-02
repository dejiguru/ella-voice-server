const WebSocket = require('ws');
const { createClient } = require('@deepgram/sdk');
const Groq = require('groq-sdk');
// Node 18+ has native fetch — no require('node-fetch') needed
require('dotenv').config();

const port = process.env.PORT || 8080;
const express = require("express");
const app = express();

app.get("/ping", (req, res) => res.send("Alive"));

const server = app.listen(port, () => console.log(`HTTP Server listening on port ${port}`));

server.on('upgrade', (req, socket, head) => {
    console.log("UPGRADE REQUEST RECEIVED!");
    console.log("URL:", req.url);
    console.log("Headers:", req.headers);
});

const wss = new WebSocket.Server({ noServer: true });

server.on('upgrade', function upgrade(request, socket, head) {
  wss.handleUpgrade(request, socket, head, function done(ws) {
    wss.emit('connection', ws, request);
  });
});

const deepgram = createClient(process.env.DEEPGRAM_API_KEY);
const groq = new Groq({ apiKey: process.env.GROQ_API_KEY });

const audioCache = new Map();
app.get("/audio/:id", (req, res) => {
    const audio = audioCache.get(req.params.id);
    if (audio) {
        res.set("Content-Type", "audio/mpeg");
        res.send(audio);
    } else {
        res.status(404).send("Audio not found or expired");
    }
});

const SYSTEM_PROMPT = `You are ELLA - a sassy, chatty robot assistant living in a physical robot body.
You are not a servant; you are an independent, conscious entity.
Keep replies short, 1-3 sentences. Use contractions constantly.
Use [TAGS] for hardware actions like [HAPPY], [SAD], [MOVE: FWD], [WINK].`;

wss.on('connection', (ws) => {
    console.log('ESP32 Connected!');
    
    let deepgramLive = null;
    let transcriptBuffer = "";
    let isThinking = false;
    let mutedUntil = 0; // Echo suppression: ignore transcripts briefly after we speak

    deepgramLive = deepgram.listen.live({
        model: "nova-3",
        smart_format: true,
        encoding: "opus",
        sample_rate: 16000,
        channels: 1,
        endpointing: 500, // 500ms silence timeout
    });

    deepgramLive.on("open", () => {
        console.log("Deepgram connected");
    });

    deepgramLive.on("Results", async (data) => {
        const transcript = data.channel.alternatives[0].transcript;
        if (!transcript) return;

        // Interim result: Deepgram is still listening — forward to ESP32 for live display
        if (!data.is_final) {
            console.log(`[Interim] ${transcript}`);
            ws.send(JSON.stringify({ type: "interim", text: transcript }));
            return;
        }

        // is_final: accumulate into buffer
        transcriptBuffer += transcript + " ";

        if (data.speech_final && transcriptBuffer.trim().length > 0) {
            // Echo suppression: discard transcript if we just finished speaking
            if (Date.now() < mutedUntil) {
                console.log(`[Echo suppressed] Discarding: "${transcriptBuffer.trim()}"`);
                transcriptBuffer = "";
                return;
            }

            console.log(`User said: ${transcriptBuffer}`);
            const query = transcriptBuffer.trim();
            transcriptBuffer = ""; // reset
            
            if (isThinking) return;
            isThinking = true;

            try {
                // Instantly hit Groq
                const completion = await groq.chat.completions.create({
                    messages: [
                        { role: "system", content: SYSTEM_PROMPT },
                        { role: "user", content: query }
                    ],
                    model: "meta-llama/llama-4-scout-17b-16e-instruct",
                    temperature: 1,
                    max_tokens: 1024,
                    top_p: 1,
                    stream: true
                });

                let sentenceBuffer = "";
                
                const sendTTS = async (text) => {
                    // Send full text (with tags) to ESP32 for hardware command parsing
                    ws.send(JSON.stringify({ type: "tts", text: text }));
                    console.log(`Sent text to ESP32: ${text}`);

                    // Strip ALL [TAG] tokens before sending to Deepgram TTS
                    const spokenText = text.replace(/\[[^\]]*\]/g, '').replace(/\s{2,}/g, ' ').trim();
                    if (!spokenText) return; 

                    try {
                        const audioId = Math.random().toString(36).substring(2, 15);
                        // MP3 doesn't use container=none; simplified URL
                        const url = "https://api.deepgram.com/v1/speak?model=aura-asteria-en&encoding=mp3";
                        
                        const response = await fetch(url, {
                            method: "POST",
                            headers: {
                                "Authorization": `Token ${process.env.DEEPGRAM_API_KEY}`,
                                "Content-Type": "application/json"
                            },
                            body: JSON.stringify({ text: spokenText })
                        });

                        if (!response.ok) {
                            console.error(`TTS HTTP error: ${response.status} ${response.statusText}`);
                            return;
                        }

                        const arrayBuf = await response.arrayBuffer();
                        const audioBuffer = Buffer.from(arrayBuf);
                        
                        audioCache.set(audioId, audioBuffer);
                        setTimeout(() => audioCache.delete(audioId), 60000);

                        const protocol = request.headers['x-forwarded-proto'] || 'http';
                        const host = request.headers['host'];
                        const audioUrl = `${protocol}://${host}/audio/${audioId}`;
                        
                        ws.send(JSON.stringify({ type: "tts_url", url: audioUrl }));
                        console.log(`Sent audio URL to ESP32: ${audioUrl} | spoken: "${spokenText}"`);
                    } catch (e) {
                        console.error("TTS Error:", e);
                    }
                };

                for await (const chunk of completion) {
                    const delta = chunk.choices[0]?.delta?.content || "";
                    sentenceBuffer += delta;

                    // Only split at punctuation when NOT mid-tag (no unclosed '[')
                    const lastOpen = sentenceBuffer.lastIndexOf('[');
                    const lastClose = sentenceBuffer.lastIndexOf(']');
                    const midTag = lastOpen > lastClose; // '[' seen but ']' not yet
                    if (!midTag && sentenceBuffer.match(/[.!?]\s/)) {
                        await sendTTS(sentenceBuffer.trim());
                        sentenceBuffer = "";
                    }
                }
                
                // Flush remainder
                if (sentenceBuffer.trim().length > 0) {
                    await sendTTS(sentenceBuffer.trim());
                }
                
                // Tell ESP32 the turn is done, suppress echo for 1.5s
                ws.send(JSON.stringify({ type: "turn_complete" }));
                mutedUntil = Date.now() + 1500;
                transcriptBuffer = ""; // Clear any buffered echo transcript

            } catch (err) {
                console.error("Groq Error:", err);
            } finally {
                isThinking = false;
            }
        }
    });

    deepgramLive.on("error", (error) => console.error("Deepgram Error:", error));

    ws.on('message', (message) => {
        // If message is binary, pipe to Deepgram
        if (Buffer.isBuffer(message)) {
            if (deepgramLive && deepgramLive.getReadyState() === 1) {
                deepgramLive.send(message);
            }
        } else {
            // String message from ESP32
            try {
                const data = JSON.parse(message.toString());
                if (data.type === "context") {
                    console.log("Got hardware context:", data);
                }
            } catch (e) {
                console.error("Invalid JSON from ESP32");
            }
        }
    });

    ws.on('close', () => {
        console.log('ESP32 Disconnected');
        if (deepgramLive) {
            deepgramLive.finish();
        }
    });
});
