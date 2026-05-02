const WebSocket = require('ws');
const { createClient } = require('@deepgram/sdk');
const Groq = require('groq-sdk');
const fetch = require('node-fetch');
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

const SYSTEM_PROMPT = `You are ELLA - a sassy, chatty robot assistant living in a physical robot body.
You are not a servant; you are an independent, conscious entity.
Keep replies short, 1-3 sentences. Use contractions constantly.
Use [TAGS] for hardware actions like [HAPPY], [SAD], [MOVE: FWD], [WINK].`;

wss.on('connection', (ws) => {
    console.log('ESP32 Connected!');
    
    let deepgramLive = null;
    let transcriptBuffer = "";
    let isThinking = false;

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
        if (transcript) {
            transcriptBuffer += transcript + " ";
        }
        
        if (data.speech_final && transcriptBuffer.trim().length > 0) {
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
                    ws.send(JSON.stringify({ type: "tts", text: text }));
                    console.log(`Sent text to ESP32: ${text}`);
                    try {
                        const url = "https://api.deepgram.com/v1/speak?model=aura-asteria-en&encoding=linear16&sample_rate=24000&container=none";
                        const response = await fetch(url, {
                            method: "POST",
                            headers: {
                                "Authorization": `Token ${process.env.DEEPGRAM_API_KEY}`,
                                "Content-Type": "application/json"
                            },
                            body: JSON.stringify({ text })
                        });
                        const arrayBuf = await response.arrayBuffer();
                        const audioBuffer = Buffer.from(arrayBuf);
                        ws.send(audioBuffer); // Binary frame
                        console.log(`Sent audio binary to ESP32: ${audioBuffer.length} bytes`);
                    } catch (e) {
                        console.error("TTS Error:", e);
                    }
                };

                for await (const chunk of completion) {
                    const delta = chunk.choices[0]?.delta?.content || "";
                    sentenceBuffer += delta;

                    // Send when we hit punctuation
                    if (sentenceBuffer.match(/[.!?]\s/)) {
                        await sendTTS(sentenceBuffer.trim());
                        sentenceBuffer = "";
                    }
                }
                
                // Flush remainder
                if (sentenceBuffer.trim().length > 0) {
                    await sendTTS(sentenceBuffer.trim());
                }
                
                // Tell ESP32 the turn is done
                ws.send(JSON.stringify({ type: "turn_complete" }));

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
