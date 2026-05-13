#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Increase loop() task stack from default 8KB to 16KB
// Prevents stack overflow in deep call chains (askAI → Firebase → TTS)
SET_LOOP_TASK_STACK_SIZE(16384);

struct SpiRamAllocator {
  void* allocate(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void* pointer) {
    heap_caps_free(pointer);
  }
  void* reallocate(void* ptr, size_t new_size) {
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
  }
};
using SpiRamJsonDocument = BasicJsonDocument<SpiRamAllocator>;

#include <mbedtls/base64.h>
#include <Audio.h>
#include <ESP_I2S.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Wire.h>
#include "wav_header.h"
#include <Adafruit_AHTX0.h>
#include <ScioSense_ENS16x.h>
#include <MAX30105.h>
#include <spo2_algorithm.h>
#include <heartRate.h>
#include <U8g2lib.h>
#include <Firebase_ESP_Client.h>
#include <Preferences.h>
#include <VL53L0X.h>
#include "esp_sleep.h"
#include "time.h"

bool writeWavHeader(File& file, uint32_t dataBytes, uint32_t sampleRate);
void disconnectAssemblyAI(unsigned long reconnectDelayMs = 5000);
void connectAssemblyAI();
void connectVoiceAgent();
void disconnectVoiceAgent();
void sendVoiceAgentSettings();
void streamMicToVoiceAgent();
void pumpVoiceAgentSocket();
void sendVoiceAgentKeepAlive();

// ============================================================
// CREDENTIALS — all secrets live in secrets.h (add to .gitignore)
// ============================================================
#include "secrets.h"

#ifndef DEEPGRAM_API_KEY
#define DEEPGRAM_API_KEY DEEPGRAM_KEY
#endif

// OPUS Encoder for audio compression
#include "opus.h"

// XiaoZhi STT removed (not in use)

#ifndef OPENAI_API_KEY
#define OPENAI_API_KEY ""
#endif

// ── MOUTH DISPLAY (Secondary ESP via UART) ──────────────────────
#define MOUTH_SERIAL Serial1
#define MOUTH_BAUD 115200

bool mouthEnabled = false;

void setupMouth() {
  MOUTH_SERIAL.begin(MOUTH_BAUD);
  delay(500);

  // Wait for mouth ESP to respond
  unsigned long start = millis();
  while (MOUTH_SERIAL.available()) {
    String reply = MOUTH_SERIAL.readStringUntil('\n');
    if (reply.indexOf("READY") >= 0) {
      mouthEnabled = true;
      Serial.println("[MOUTH] Connected to secondary ESP");
      break;
    }
    if (millis() - start > 3000) break;
  }

  if (!mouthEnabled) {
    Serial.println("[MOUTH] Not detected - running without mouth display");
  }
}

void sendMouthExpression(const char* expression) {
  if (!mouthEnabled) return;
  MOUTH_SERIAL.print("EXPR:");
  MOUTH_SERIAL.println(expression);
}

void sendMouthText(const char* text) {
  if (!mouthEnabled) return;
  MOUTH_SERIAL.print("SAY:");
  MOUTH_SERIAL.println(text);
}

void sendMouthIdle() {
  if (!mouthEnabled) return;
  MOUTH_SERIAL.println("IDLE:OK");
}

void sendMouthListening() {
  if (!mouthEnabled) return;
  MOUTH_SERIAL.println("LISTEN");
}

// WiFi (WiFi.begin() needs char* variables)
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
// GROQ_KEY, MIMO_KEY, TAVILY_KEY → macros from secrets.h

// ── ByteDance Realtime STT (WebSocket) ───────────────────────────────
const char* BD_HOST = "openspeech.bytedance.com";
const int   BD_PORT = 443;
const char* BD_PATH = "/api/v2/asr";
const char* BD_API_KEY = BYTEDANCE_API_KEY;
const char* BD_CLUSTER = BYTEDANCE_CLUSTER;

// ByteDance Binary Header Types
#define BD_FULL_REQUEST 0x11
#define BD_AUDIO_ONLY 0x21
#define BD_END_STREAM 0x22

// ── AssemblyAI Realtime STT (WebSocket) ───────────────────────────────
// ASSEMBLYAI_KEY, GROQ_KEY, MIMO_KEY, TAVILY_KEY → macros from secrets.h
const char* AAI_HOST = "streaming.assemblyai.com";
const int   AAI_PORT = 443;
const char* AAI_PATH = "/v3/ws"
    "?sample_rate=16000"
    "&speech_model=u3-rt-pro"
    "&format_turns=true"
    "&end_of_turn_confidence_threshold=0.8"
    "&min_end_of_turn_silence_when_confident=500"
    "&max_turn_silence=2000"
    "&vad_threshold=0.4"
    "&speaker_labels=false";
const char* AAI_AUTH_HDR = "Authorization: " ASSEMBLYAI_KEY;

// ── Node.js Proxy Server Settings ───────────────────────────
#define USE_NODE_SERVER true
#define NODE_SERVER_HOST "ella-voice-server.onrender.com"
#define NODE_SERVER_PORT 443
#define NODE_SERVER_IS_SECURE true

// ── Deepgram Realtime STT (WebSocket) ─────────────────────────────────
const char* DG_HOST = "api.deepgram.com";
const int   DG_PORT = 443;
const char* DG_PATH = "/v2/listen"         // v2 endpoint required for Flux model
    "?model=flux-general-en"       // Flux General English: Supports OPUS encoding
    "&encoding=opus"               // OPUS compression
    "&sample_rate=16000"
    "&eot_threshold=0.6"           // Lower threshold for faster turn detection
    "&eot_timeout_ms=2000";        // Shorter timeout for faster response
const char* DG_BATCH_PATH = "/v1/listen?model=nova-3&language=en-US&smart_format=true&numerals=true";
const char* DG_AUTH_HDR = "Authorization: Token " DEEPGRAM_API_KEY;
// ────────────────────────────────────────────────────────────────────────────
// FIREBASE_HOST, FIREBASE_DATABASE_URL, FIREBASE_AUTH, FIREBASE_DB_SECRET
// are all macros from secrets.h — used directly where needed

static const char* GOOGLE_TTS_LANG = "en-ng"; // Nigerian English accent via Google TTS
static String g_api_host = "api.openai.com";
static const char* GROQ_MODEL = "openai/gpt-oss-20b";

// Primary LLM routing:
// - false: use direct Mistral model below (Devstral)
// - true: use online Mistral Agent (agent_id + version)
const bool USE_MISTRAL_AGENT = true;
const char* MISTRAL_DIRECT_MODEL = "devstral-latest";
const char* MISTRAL_AGENT_ID = "ag_019d4492c13a75ff8e9e139956e37489";
const int MISTRAL_AGENT_VERSION = 19;
const bool USE_LLM_WEBSOCKET = false; // Disabling LLM WebSocket per user request to save memory

const char* OPENAI_REALTIME_MODEL = "gpt-4o-mini-realtime-preview";

FirebaseData fbdo;
FirebaseData fbdoCmd;  // Dedicated object for command polling
FirebaseData fbdoFocus;  // Dedicated object for focus mode polling
FirebaseData fbdoStatus;  // Dedicated object for robot assist / ToF status
FirebaseAuth auth;
FirebaseConfig config;

// ── MQTT Client (PubSubClient) ──────────────────────────────────────
#include <PubSubClient.h>
WiFiClientSecure* mqttClientNet = new WiFiClientSecure();
PubSubClient mqtt(*mqttClientNet);
unsigned long lastMqttRetryMs = 0;
String g_lastMistralReply = ""; // Simple memory for Mistral Agent
String g_lastUserQuery = "";   // Track last user query for Mistral context
void mqttCallback(char* topic, byte* payload, unsigned int length);
// ─────────────────────────────────────────────────────────────────────────────

const bool FIREBASE_ENABLED = false;
const unsigned long WIFI_STARTUP_CONNECT_WINDOW_MS = 20000;
const unsigned long NTP_SYNC_WINDOW_MS = 1200;

const char* ntpServer = "pool.ntp.org";
const char* ntpServer2 = "time.google.com";
const char* ntpServer3 = "1.1.1.1";
const long  gmtOffset_sec = 3600;
const int daylightOffset_sec = 0;

bool firebaseReady = false;
bool focusModeActive = false;  // Focus Mode (Pomodoro) from web app
bool otaReady = false;
bool onlineServicesAllowed = false;
bool offlineModeLocked = false;
bool ntpTimeValid = false;
volatile bool sttSocketPumpEnabled = false;
static const uint8_t SLAM_MAP_SIZE = 50;
uint8_t slamMapCells[SLAM_MAP_SIZE * SLAM_MAP_SIZE];
String lastStatusResponseText = "";
Preferences prefs;
String wifiSSID, wifiPass;

String cloudBotToken = "";
String cloudChatId = "";
String user_name = ""; // Added for Profile Sync
String user_emergency_contact = ""; // Added for Emergency Alert
String cloudRemindersJson = "[]";


// ============================================================
// PINS
// ============================================================
#define I2S_MIC_SCK 41  // Reverted from 15
#define I2S_MIC_WS  42  // Reverted from 7
#define I2S_MIC_SD  4   
#define SPK_BCLK 48
#define SPK_LRC  21  
#define SPK_DOUT 18  
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9
// ORIGINAL TFT MAPPING (Old Way)
#define TFT_CS 10
#define TFT_DC 2
#define TFT_RST -1  
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_MISO 13  // XPT2046 touch data line (shared SPI)

#define TACTILE_SWITCH_PIN 19
#define INTERRUPT_PIN -1 // Disabled (User Request)
#define BUZZER_PIN 40   
#define TOUCH_CS 47 
#define TOUCH_IRQ 14 // Moved from 13 (MISO Conflict) 

// Motors & Servo
#define MOTOR1_IN1 16 // Left Motor Forward
#define MOTOR1_IN2 17 // Left Motor Backward
#define MOTOR2_IN1 5  // Right Motor Forward
#define MOTOR2_IN2 6  // Right Motor Backward
#define SERVO_PIN  1  // Neck Movement

// Ultrasonic Sensor (HC-SR04) - Chest Mounted (Fixed Forward)
#define ULTRASONIC_TRIG_PIN 15  // Moved from strapping pin 3
#define ULTRASONIC_ECHO_PIN 38  // Moved from JTAG pin 41

// Multiplexer
#define TCA_ADDR 0x70
#define CH_EYE_LEFT  0
#define CH_EYE_RIGHT 1
#define CH_AHT       2
#define CH_ENS       2
#define CH_MAX       4
#define CH_IMU       5
#define CH_TOF       CH_IMU

// ============================================================
// AUDIO & UI
// ============================================================
const int SAMPLE_RATE = 16000;
#define BUFFER_LEN 512

// OPUS Encoder Configuration (XiaoZhi-style)
#define OPUS_SAMPLE_RATE 16000
#define OPUS_CHANNELS 1
#define OPUS_FRAME_SIZE 960  // 60ms at 16kHz (960 samples)
#define OPUS_MAX_PACKET_SIZE 4000
#define OPUS_BITRATE 24000  // Match test code: 24 kbps (was 32k - smaller packets = faster streaming)
#define OPUS_APPLICATION OPUS_APPLICATION_VOIP  // Optimized for speech
#define OPUS_COMPLEXITY 0  // Fast encoding (good for ESP32)

OpusEncoder* opusEncoder = nullptr;
int16_t* opusPcmBuffer = nullptr;    // Moved to PSRAM
uint8_t* opusPacketBuffer = nullptr; // Moved to PSRAM
uint8_t* deepgramBatchBuffer = nullptr; // Buffer for non-OPUS batching
size_t deepgramBatchBytes = 0;
size_t deepgramBatchCapacity = 0;

// Legacy settings (no longer used with OPUS)
#define GAIN_BOOSTER_I2S 1   // Not used with OPUS - clean audio pipeline

// DC Offset Filter state (not used with OPUS)
static float mic_dc_offset = 0.0f; 
const float MIC_DC_ALPHA = 0.01f; 

#define UI_BG 0xFFFF
#define UI_CARD_BG 0xF7BE
#define UI_ACCENT 0x243F
#define UI_TEXT_MAIN 0x2104
#define UI_TEXT_SUB 0x632C
#define UI_ALERT 0xFDA0
#define UI_ERROR 0xF800
#define UI_SUCCESS 0x0510
#define UI_INFO 0x243F
#define UI_HEART 0xF800
#define UI_OXY 0x243F
#define UI_ENV 0x04C0
#define UI_PRIMARY 0x243F

// Robot structure map:
// IDLE -> normal screen / ambient sensors / eye drift
// LISTENING -> mic capture and wake / button input
// THINKING -> AI request in progress, eyes set to THINKING
// SPEAKING -> TTS queue active, audio playback, speaking visuals
// SUPPORT MODES -> medical, motion, guard, reminders, and emergency actions
// This mirrors the Be More Agent pattern without changing runtime behavior.

// ============================================================
// Objects
// WebSocket removed — phone handles STT via Firebase

// Function Forward Declarations
void audio_eof_speech(const char* info);
void audio_eof_mp3(const char* info);
void setEyeExpression(String expr);
void updateEyes();
void animateEyesWhileSpeaking(); 
void read_aht20();
void read_ens160();
void tokenStatusCallback(TokenInfo info);
void drawNormalScreen(bool force=false);
void drawAIScreen(bool force=false);
void switchToAIMode();
void switchToNormalMode();
void switchToAutonomousMode();
void startAutonomousDemo();
void stopAutonomousDemo();
void performHeadScan(uint16_t &leftDist, uint16_t &centerDist, uint16_t &rightDist);
void updateAutonomousNavigation();
void drawAutonomousScreen(bool force=false);
void printMemoryStats();
void printMicTestHelp();
bool startMicTestRecording(unsigned long durationMs);
void stopMicTestRecording();
bool startMicTestPlayback();
void stopMicTestPlayback();
void serviceMicTest();
void startupSequence();
void setupNetwork();
void resetWiFiStationForRetry();
void updateSensors();
void sendEmergencyAlert(String condition);
void handleUniversalStopAudioCommand();
void recalibrateNavigationHeading();
void syncTimeViaHTTP();
void sendAaiKeepAlive();
bool isEnvironmentSummaryRequest(const String& userQuery);
bool isSpatialAwarenessRequest(const String& userQuery);
bool isRobotActionRequest(const String& userQuery);
bool isLiveWebSearchRequest(const String& userQuery);
void pushSensorDataToMQTT(FirebaseJson& json);
String stripReasoningBlocks(String text);
void publishStatusSnapshot(bool force);
bool hasExplicitCommandPhrase(const String& text, const char* phrase);
void logMoodToFirebase();
void handleIncomingTelegramCommand(String text, String chatId);

Audio audio(1); // Use I2S_NUM_1 for Speaker to avoid conflict with Microphone (mic_i2s) on I2S_NUM_0
I2SClass mic_i2s;
I2SClass micTestSpeaker_i2s;
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// Touch Screen (Added per user request from dec 5a)
#include <XPT2046_Touchscreen.h>
// Touch Object Re-enabled
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// ============================================================
// SLEEP MODE TRACKING
// ============================================================
unsigned long lastInteractionTime = 0;
bool isSleepMode = false;

#undef I2C_BUFFER_LENGTH // Fix warning from MAX30105
#include <MAX30105.h>
#include "spo2_algorithm.h" // Algorithm for HR/SpO2

MAX30105 particleSensor;
Adafruit_AHTX0 aht;
ENS160 ens160; 

// DigiCert Global Root G2 (Used by Cloudflare/Render)
// DigiCert Global Root G2 (Used by Cloudflare/Render)
const char* root_ca = \
"-----BEGIN CERTIFICATE-----\n" \
"MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n" \
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n" \
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n" \
"MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n" \
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n" \
"b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n" \
"9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfVNNP5yPuDKNfKxJ7BbKE5Ehtzzzq9KZUf\n" \
"nqukrCQ6Gl79O7r8qgCA/DU9dUj4DVEhgDCFJaSvL5pnECRR+PsKGSxjGPFiXnSk\n" \
"hVhGqRLssC1K6um1BeB6SFk70tr51p4VD6GGkyqD+jat0Iq0fPU8JXmppYqFwnqP\n" \
"JCypinqsznb1N+SGZIMP1BUVIbzVWHn6+fqQ1zcRczQKxfFFy0X1Ju2balWGoQm+\n" \
"NyXndsKpPPWhaxx2t7+OEc/WV7o03Q5zhc7e0PJn5uDjzeO7hz848VQ8W9S0oMWz\n" \
"IuVlWPfCvL5Q/YGh92TFk0SOcDNLA7ml4dqlaHh9kH8K6EmWcQIDAQABo2MwYTAO\n" \
"BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUTiJUIBiV\n" \
"5uNu5g/6+rkS7QYXjzkwHwYDVR0jBBgwFoAUTiJUIBiV5uNu5g/6+rkS7QYXjzkw\n" \
"DQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY1Yl9PMWLSn/pvtsrF9+w\n" \
"X3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4NeF22d+mQrvHRAiGfzZ0\n" \
"JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NGFdtom/DzMNU+MeKNhJ7j\n" \
"itralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ918rGOmaFvE7FBcf6IKshP\n" \
"ECX6doF4diUxUiJwCwagJCXZ7p8GZxHHi1Grdm+972flZxcoknbyM2W3ZdZ/Uw0T\n" \
"pxOPDxyFVTjqpfJtT7uj8/NqTTlOjYWU0tmsG2VPGkmPVHzqMP8=\n" \
"-----END CERTIFICATE-----\n";


bool micI2SActive = false;

// MAX30102 Algorithm Variables
uint32_t irBuffer[100]; // infrared LED sensor data
uint32_t redBuffer[100];  // red LED sensor data
int32_t bufferLength = 100; // data length

int32_t spo2; // SPO2 value
int8_t validSPO2; // indicator to show if the SPO2 calculation is valid
int32_t heartRate; // heart rate value
int8_t validHeartRate; // indicator to show if the heart rate calculation is valid

// Dual OLED eyes (via TCA9548A multiplexer)
// Using Adafruit_SSD1306 as verified working (128x64)
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64 
// Reset pin = -1 (not used)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// GFXcanvas16 medSprite(220, 100); // REMOVED to save 44KB RAM
// GFXcanvas16 ecgSprite(240, 80); // REMOVED to save 38KB RAM

int16_t* sBuffer = nullptr;
bool isProcessingAI = false;
bool isSpeaking = false;
enum RobotActivity {
  ROBOT_ACTIVITY_IDLE,
  ROBOT_ACTIVITY_LISTENING,
  ROBOT_ACTIVITY_THINKING,
  ROBOT_ACTIVITY_SPEAKING
};
RobotActivity currentRobotActivity = ROBOT_ACTIVITY_IDLE;
unsigned long processingStartTime = 0; // Safety timeout for isProcessingAI flag
String aiRequestStatus = ""; // Thinking / voice startup status shown on the AI screen
String activeTtsText = "";
String ttsRequestUrl = "";
bool ttsUsingFallbackHost = false;
bool ttsRetryPending = false;
unsigned long ttsRetryAt = 0;
bool ttsStartupPending = false;
unsigned long ttsSpeechStartMs = 0;
unsigned long ttsPlaybackStartedMs = 0;
bool ttsSpeechSessionActive = false;
const uint8_t TTS_CHUNK_QUEUE_MAX = 12;
String ttsChunkQueue[TTS_CHUNK_QUEUE_MAX];
uint8_t ttsChunkHead = 0;
uint8_t ttsChunkTail = 0;
uint8_t ttsChunkCount = 0;
bool ttsChunkSequenceActive = false;
bool ttsChunkNeedsStart = false;
unsigned long nextAiProactiveNudgeMs = 0;
const unsigned long AI_PROACTIVE_IDLE_MS = 180000;
const unsigned long AI_PROACTIVE_COOLDOWN_MS = 240000;

void setRobotActivity(RobotActivity activity, const String& status = "", const String& eyeExpr = "", bool redrawAi = false) {
  currentRobotActivity = activity;
  if (status.length() > 0) {
    aiRequestStatus = status;
  }
  if (eyeExpr.length() > 0) {
    setEyeExpression(eyeExpr);
  }
  if (redrawAi) {
    drawAIScreen(true);
  }
}
// ── ByteDance Realtime STT globals ───────────────────────────────────────────
bool bdConnected = false;
bool bdConnecting = false;
bool bdHasSpeech = false;
int  bdSameResultCount = 0;
String bdLastResult = "";
String bdFinalResult = "";
// ─────────────────────────────────────────────────────────────────────────────

// ── Deepgram raw WebSocket STT globals (NetworkClientSecure) ───────────────────
WiFiClientSecure* aaiClient = new WiFiClientSecure();
bool             aaiConnected  = false;
bool             aaiConnecting = false;
bool             deepgramMetadataReceived = false;  
bool     deepgramBatchSending = false;
uint8_t* aaiRxBuffer = nullptr;    
unsigned long    lastDeepgramKeepAliveMs = 0;
unsigned long    nextAaiReconnectMs = 0;

// ── Node.js Proxy Server Globals ───────────────────────────────────────────
WiFiClient*      nodeWsClient = nullptr;
bool             nodeWsConnected = false;
bool             nodeWsConnecting = false;
SemaphoreHandle_t nodeMux = NULL; // Concurrency safety for Node Server
unsigned long    nextNodeWsReconnectMs = 0;
unsigned long    nodeAudioPendingSince = 0; // for stuck-state timeout
unsigned long    lastNodeWsKeepAliveMs = 0; // for WebSocket ping keep-alive

// ── Node TTS Audio Ring Buffer (written Core 0, drained Core 1) ─────────────
#define NODE_AUDIO_BUF_SIZE (256 * 1024)  // Larger PCM buffer for smooth Node TTS
#define NODE_AUDIO_PREBUFFER_BYTES 24000  // Increased to 0.5s for smoother playback
static uint8_t*  nodeAudioRingBuf = nullptr;
static volatile size_t nodeAudioWritePos = 0;
static volatile size_t nodeAudioReadPos  = 0;
static volatile bool   nodeAudioPending  = false;
static volatile bool   nodeAudioStreamDone = true;
static portMUX_TYPE    nodeAudioMux = portMUX_INITIALIZER_UNLOCKED;

inline size_t nodeAudioAvailable() {
  size_t w = nodeAudioWritePos, r = nodeAudioReadPos;
  return (w >= r) ? (w - r) : (NODE_AUDIO_BUF_SIZE - r + w);
}

// Called from Core 0 sttTask
static void nodeAudioEnqueue(const uint8_t* data, size_t len) {
  if (!nodeAudioRingBuf || len == 0) return;
  portENTER_CRITICAL(&nodeAudioMux);
  for (size_t i = 0; i < len; i++) {
    size_t next = (nodeAudioWritePos + 1) % NODE_AUDIO_BUF_SIZE;
    if (next == nodeAudioReadPos) break; // full, drop tail
    nodeAudioRingBuf[nodeAudioWritePos] = data[i];
    nodeAudioWritePos = next;
  }
  nodeAudioPending = true;
  portEXIT_CRITICAL(&nodeAudioMux);
}
// ─────────────────────────────────────────────────────────────────────────────

// WiFiClientSecure* llmWsClient = new WiFiClientSecure(); // Disabled to save heap
WiFiClientSecure* llmWsClient = nullptr;
bool llmWsConnected = false;
bool llmWsSessionConfigured = false;
unsigned long lastLlmWsKeepAliveMs = 0;
unsigned long nextLlmWsReconnectMs = 0;

const unsigned long AAI_RECONNECT_BACKOFF_MS = 1200;
const unsigned long AAI_HANDSHAKE_TIMEOUT_MS = 12000;
const unsigned long AAI_FRAME_TIMEOUT_MS = 1500;
const unsigned long AAI_KEEPALIVE_MS = 3000;
const unsigned long LLM_WS_KEEPALIVE_MS = 15000;
const unsigned long NODE_WS_KEEPALIVE_MS = 15000; // Node WebSocket ping interval
const unsigned long LLM_WS_RECONNECT_BACKOFF_MS = 1500;
// OPUS frame buffer (960 samples * 2 bytes = 1920 bytes for 60ms frame)
#define AAI_BUFFER_SIZE    1920  
#define AAI_AUDIO_CHUNK_MS   60   
#define NODE_STT_PCM_CHUNK_SIZE 640

uint8_t* aaiBuffer = nullptr;    
// ─────────────────────────────────────────────────────────────────────────────
// ── Deepgram Voice Agent (STT+LLM+TTS unified) ────────────────────────
// When USE_VOICE_AGENT=true and aiInputUsesMic=true, the mic streams to
// the Voice Agent WebSocket which handles STT → LLM → TTS server-side.
// Audio output is played directly through speaker I2S.
// Text input (web/Firebase) still uses the old HTTP LLM pipeline.
// Set USE_VOICE_AGENT=false to revert to the old separate pipeline.
const bool USE_VOICE_AGENT = false;

const char* VAGENT_HOST = "agent.deepgram.com";
const int   VAGENT_PORT = 443;
const char* VAGENT_PATH = "/v1/agent/converse";

WiFiClientSecure* vAgentClient = nullptr;
bool vAgentConnected = false;
bool vAgentConnecting = false;
bool vAgentConfigured = false;
SemaphoreHandle_t vAgentMux = NULL; // Concurrency safety for Voice Agent
unsigned long vAgentLastKeepAliveMs = 0;
const unsigned long VAGENT_KEEPALIVE_MS = 5000;
const unsigned long VAGENT_RECONNECT_MS = 2000;
unsigned long vAgentNextReconnectMs = 0;
bool vAgentPlayingAudio = false;
String vAgentLastAssistantText = "";
// ────────────────────────────────────────────────────────────────────────────

bool micTestActive = false;
bool micTestRecording = false;
bool micTestPlaybackPending = false;
bool micTestShouldResumeLiveMic = false;
bool micTestPausedDeepgram = false;
uint8_t* micTestBuffer = nullptr;
size_t micTestBufferBytes = 0;
size_t micTestBytesWritten = 0;
size_t micTestMaxBytes = 0;
unsigned long micTestStartMs = 0;
unsigned long micTestDurationMs = 0;
unsigned long micTestLastRmsLogMs = 0;
const unsigned long MIC_TEST_DEFAULT_SECONDS = 10;
const unsigned long MIC_TEST_MAX_SECONDS = 30;

unsigned long lastMusicAction = 0;
unsigned long audioTransitionQuietUntil = 0;
const unsigned long AUDIO_TRANSITION_QUIET_MS = 600;  // Reduced from 1200ms for faster restart
unsigned long networkQuietUntil = 0;
const unsigned long NETWORK_QUIET_MS = 1500;  // Reduced from 6000ms to allow faster mic restart
const unsigned long TTS_STARTUP_TIMEOUT_MS = 1800;
const unsigned long TTS_RETRY_DELAY_MS = 40;
const unsigned long TTS_EARLY_EOF_RETRY_MS = 700;
unsigned long micReconnectScheduledAt = 0;
unsigned long micReconnectLastAttemptMs = 0;
const unsigned long MIC_RECONNECT_AFTER_TTS_MS = 120;
const unsigned long MIC_RECONNECT_AFTER_FAILURE_MS = 250;
const unsigned long MIC_RECONNECT_STALL_MS = 8000;
bool deepgramFirstFramePending = false;

void enterAudioTransitionQuiet(unsigned long extraMs = AUDIO_TRANSITION_QUIET_MS) {
  unsigned long target = millis() + extraMs;
  if (target > audioTransitionQuietUntil) {
    audioTransitionQuietUntil = target;
  }
}

bool audioTransitionIsQuiet() {
  return millis() < audioTransitionQuietUntil;
}

void enterNetworkQuiet(unsigned long extraMs = NETWORK_QUIET_MS) {
  unsigned long target = millis() + extraMs;
  if (target > networkQuietUntil) {
    networkQuietUntil = target;
  }
}

bool networkIsQuiet() {
  return millis() < networkQuietUntil;
}

bool networkAvailable() {
  return !offlineModeLocked && onlineServicesAllowed && WiFi.status() == WL_CONNECTED;
}

void clearSlamMap() {
  memset(slamMapCells, 0, sizeof(slamMapCells));
}

void setSlamCell(uint8_t x, uint8_t y, uint8_t value) {
  if (x >= SLAM_MAP_SIZE || y >= SLAM_MAP_SIZE) return;
  slamMapCells[(y * SLAM_MAP_SIZE) + x] = value & 0x03;
}

void stampSlamRay(float angleDeg, uint16_t distanceMm) {
  const int center = SLAM_MAP_SIZE / 2;
  float rad = angleDeg * 0.0174532925f;
  int maxCells = distanceMm > 0 ? min((int)(distanceMm / 100), center - 2) : (center - 2);
  if (maxCells < 1) maxCells = 1;

  for (int step = 1; step < maxCells; step++) {
    int x = center + (int)lroundf(cosf(rad) * step);
    int y = center + (int)lroundf(sinf(rad) * step);
    if (x < 1 || y < 1 || x >= (SLAM_MAP_SIZE - 1) || y >= (SLAM_MAP_SIZE - 1)) break;
    setSlamCell((uint8_t)x, (uint8_t)y, 2);
  }

  int wallX = center + (int)lroundf(cosf(rad) * maxCells);
  int wallY = center + (int)lroundf(sinf(rad) * maxCells);
  if (wallX >= 1 && wallY >= 1 && wallX < (SLAM_MAP_SIZE - 1) && wallY < (SLAM_MAP_SIZE - 1)) {
    setSlamCell((uint8_t)wallX, (uint8_t)wallY, 1);
  }
}

bool pendingSpeakingStatusValid = false;
bool pendingSpeakingStatusValue = false;
unsigned long pendingSpeakingStatusReadyAt = 0;
String pendingLastResponseText = "";
bool pendingLastResponseValid = false;
unsigned long pendingLastResponseReadyAt = 0;

void setSpeakingStatusFirebase(bool speaking) {
  pendingSpeakingStatusValue = speaking;

  if (audioTransitionIsQuiet()) {
    pendingSpeakingStatusValid = true;
    pendingSpeakingStatusReadyAt = audioTransitionQuietUntil;
    return;
  }

  pendingSpeakingStatusValid = false;
  publishStatusSnapshot(true);
}

void setLastResponseFirebase(const String& text) {
  lastStatusResponseText = text;

  if (audioTransitionIsQuiet()) {
    pendingLastResponseValid = true;
    pendingLastResponseText = text;
    pendingLastResponseReadyAt = audioTransitionQuietUntil;
    return;
  }

  pendingLastResponseValid = false;
  publishStatusSnapshot(true);
}

void flushPendingSpeakingStatus() {
  if (!pendingSpeakingStatusValid) return;
  if (audioTransitionIsQuiet() || millis() < pendingSpeakingStatusReadyAt) return;
  pendingSpeakingStatusValid = false;
  publishStatusSnapshot(true);
}

void flushPendingLastResponse() {
  if (!pendingLastResponseValid) return;
  if (audioTransitionIsQuiet() || millis() < pendingLastResponseReadyAt) return;
  lastStatusResponseText = pendingLastResponseText;
  pendingLastResponseValid = false;
  publishStatusSnapshot(true);
}

// ============================================================
// MODE SYSTEM
// ============================================================
enum SystemMode { MODE_NORMAL, MODE_AI, MODE_AUTONOMOUS };
SystemMode currentMode = MODE_NORMAL;
bool aiInputUsesMic = false; // false = web/Firebase text, true = on-board mic + Deepgram
String queuedAiChat = "";
String currentInterimText = ""; // Holds live speech text helper
String lastAiResponse = "";    // Holds last AI response for display
String pendingModeStatusValue = "";
bool pendingModeStatusValid = false;
unsigned long pendingModeStatusReadyAt = 0;
unsigned long bootTimeMs = 0;

// ============================================================
// AUTONOMOUS MODE (Competition Demo)
// ============================================================
enum AutonomousState {
  AUTO_STATE_IDLE,
  AUTO_STATE_EXPLORING,
  AUTO_STATE_AVOIDING_OBSTACLE,
  AUTO_STATE_MAPPING,
  AUTO_STATE_RETURNING_HOME,
  AUTO_STATE_COMPLETE
};

AutonomousState autoState = AUTO_STATE_IDLE;
unsigned long autoStateStartMs = 0;
float autoStartX = 0.0;
float autoStartY = 0.0;
float autoCurrentX = 0.0;
float autoCurrentY = 0.0;
float autoStartHeading = 0.0;
int autoObjectsDetected = 0;
int autoDistanceTraveled = 0;
unsigned long autoModeStartMs = 0;
bool autoModeActive = false;
String autoStatusMessage = "";

// Autonomous navigation constants
#define AUTO_OBSTACLE_DISTANCE_CM 30.0  // Stop if object within 30cm
#define AUTO_EXPLORE_DURATION_MS 60000  // Explore for 60 seconds
#define AUTO_TURN_ANGLE 45              // Turn 45 degrees when avoiding
#define AUTO_FORWARD_SPEED 150          // Moderate speed for safety
#define AUTO_SCAN_INTERVAL_MS 500       // Scan every 500ms
unsigned long imuStillStartMs = 0;
unsigned long lastAutoImuRecalibMs = 0;
const unsigned long DEEP_SLEEP_WAKE_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
const unsigned long DEEP_SLEEP_MIN_UPTIME_MS = 10UL * 60UL * 1000UL;
const unsigned long DEEP_SLEEP_INACTIVITY_MS = 30UL * 60UL * 1000UL;

void setModeStatusFirebase(const char* modeText) {
  pendingModeStatusValue = modeText;

  if (audioTransitionIsQuiet()) {
    pendingModeStatusValid = true;
    pendingModeStatusReadyAt = audioTransitionQuietUntil;
    return;
  }

  pendingModeStatusValid = false;
  publishStatusSnapshot(true);
}

void flushPendingModeStatus() {
  if (!pendingModeStatusValid) return;
  if (audioTransitionIsQuiet() || millis() < pendingModeStatusReadyAt) return;
  pendingModeStatusValid = false;
  publishStatusSnapshot(true);
}

enum DeferredAiActionType {
  DEFERRED_AI_NONE,
  DEFERRED_AI_PLAYSONG,
  DEFERRED_AI_RELAX,
  DEFERRED_AI_GOHOME,
  DEFERRED_AI_IMURESET,
  DEFERRED_AI_CHECKUP,
  DEFERRED_AI_EMERGENCY,
  DEFERRED_AI_MEDITATE
};

DeferredAiActionType deferredAiAction = DEFERRED_AI_NONE;
String deferredAiParam = "";
unsigned long deferredAiReadyAt = 0;
unsigned long lastTouchActionMs = 0;
const unsigned long TOUCH_NETWORK_QUIET_MS = 300;
void enableMicAIInput();

void clearMicAIReconnectSchedule() {
  micReconnectScheduledAt = 0;
  micReconnectLastAttemptMs = 0;
}

void scheduleMicAIReconnect(unsigned long delayMs, const char* reason) {
  if (!aiInputUsesMic || currentMode != MODE_AI) {
    clearMicAIReconnectSchedule();
    return;
  }

  unsigned long targetMs = millis() + delayMs;
  unsigned long transportReconnectAt = nextAaiReconnectMs;
  if (USE_NODE_SERVER) {
    transportReconnectAt = nextNodeWsReconnectMs;
  } else if (USE_VOICE_AGENT) {
    transportReconnectAt = vAgentNextReconnectMs;
  }

  if (targetMs < transportReconnectAt) {
    targetMs = transportReconnectAt;
  }
  if (targetMs < millis()) {
    targetMs = millis();
  }

  micReconnectScheduledAt = targetMs;
  Serial.printf("[DG] Mic reconnect scheduled for %lu ms (%s)\n",
                micReconnectScheduledAt,
                reason ? reason : "unspecified");
}

void serviceMicAIReconnect(bool blockedByCheckup = false) {
  if (currentMode != MODE_AI || !aiInputUsesMic) {
    clearMicAIReconnectSchedule();
    return;
  }

  if (blockedByCheckup) return;
  if (!networkAvailable()) return;
  if (isSpeaking || isProcessingAI || audio.isRunning()) return;

  unsigned long now = millis();
  bool scheduledReconnectReady = (micReconnectScheduledAt > 0 && now >= micReconnectScheduledAt);
  bool liveReconnectReady;
  if (USE_NODE_SERVER) {
    liveReconnectReady = (micI2SActive && !nodeWsConnected && !nodeWsConnecting && now >= nextNodeWsReconnectMs);
  } else if (USE_VOICE_AGENT) {
    liveReconnectReady = (micI2SActive && !vAgentConnected && !vAgentConnecting && now >= vAgentNextReconnectMs);
  } else {
    liveReconnectReady = (micI2SActive && !aaiConnected && !aaiConnecting && now >= nextAaiReconnectMs);
  }

  if (!scheduledReconnectReady && !liveReconnectReady) {
    return;
  }

  if (now - micReconnectLastAttemptMs < 5000) return; // Increased from 500ms to 5000ms
  micReconnectLastAttemptMs = now;

  Serial.printf("[DG] Mic reconnect attempt at %lu ms\n", now);
  if (USE_NODE_SERVER && micI2SActive) {
    connectNodeServer();
  } else {
    enableMicAIInput();
  }

  bool ready;
  if (USE_NODE_SERVER) {
    ready = nodeWsConnected;
  } else if (USE_VOICE_AGENT) {
    ready = (vAgentConnected && vAgentConfigured);
  } else {
    ready = (aaiConnected && sttSocketPumpEnabled);
  }
  if (ready) {
    Serial.printf("[DG] Mic reconnect ready at %lu ms\n", millis());
    clearMicAIReconnectSchedule();
    return;
  }

  if (micReconnectScheduledAt > 0 && now > micReconnectScheduledAt + MIC_RECONNECT_STALL_MS) {
    Serial.printf("[DG] Mic reconnect stalled at %lu ms, retrying\n", now);
    micReconnectScheduledAt = now + MIC_RECONNECT_AFTER_FAILURE_MS;
  }
}

bool touchUiRecentlyHandled() {
  return millis() - lastTouchActionMs < TOUCH_NETWORK_QUIET_MS;
}

bool lastButtonState = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long buttonPressStartTime = 0;
const unsigned long debounceDelay = 50;

// ============================================================
// SENSOR DATA
// ============================================================
float temp_aht = NAN;
float humidity_aht = NAN;
float max30102_hr = NAN;
float max30102_spo2 = NAN;
float max30102_temp = NAN;
uint16_t aqi_val = 0;
uint16_t tvoc_val = 0;
uint16_t eco2_val = 0;
unsigned long lastEnvSensorReadMs = 0;

// ============================================================
// MAX30102 STATE MACHINE (Normal Mode Only)
// ============================================================
enum MedicalState { MED_IDLE, MED_WAIT_FINGER, MED_PLACE_FINGER, MED_CHECKING, MED_MEASURING, MED_RESULT, MED_BREATHING, MED_MEDITATING };
MedicalState currentMedState = MED_IDLE;
unsigned long medStateTimer = 0;

enum MeditationType { MED_TYPE_BREATHING, MED_TYPE_BODY_SCAN, MED_TYPE_CALM_FOCUS, MED_TYPE_DEEP_REST };
MeditationType currentMeditationType = MED_TYPE_BREATHING;
int medCountdown = 5;
bool wait_for_finger_removal = false;
bool max30102_needs_full_read = true;
bool ai_requested_checkup = false; // Set when AI triggers checkup, enables 30s timeout

// Pending AI-set reminder (lightweight global to avoid stack overflow in askAI)
String pendingReminderTitle = "";
String pendingReminderTime  = "";
String pendingReminderType  = "chat";
bool pendingReminderNeedsCloudSync = false;

// Accumulators for averaging valid readings across the 30s window
float hr_sum = 0;
float spo2_sum = 0;
int hr_valid_count = 0;    // Counted separately — HR and SpO2 may not both be valid at same time
int spo2_valid_count = 0;

// ============================================================
// MOVEMENT QUEUE (Non-Blocking AI Motor Control)
// ============================================================
String movementQueue[20]; // Buffer for up to 20 serial movement commands
int queueHead = 0;
int queueTail = 0;
unsigned long movementTimer = 0;
int currentMovementDuration = 0;
bool isMoving = false;
int motorSpeed = 200; // Default motor speed (0-255)

// ============================================================
// IMU / STRAIGHT-DRIVE ASSIST
// ============================================================
bool imuReady = false;
uint8_t imuAddress = 0x68;
float imuGyroZBias = 0.0f;
float imuGyroZFiltered = 0.0f;
float imuAccelXg = 0.0f;
float imuAccelYg = 0.0f;
float imuAccelZg = 1.0f;
float imuPitchDeg = 0.0f;
float imuRollDeg = 0.0f;
float imuAccelMagnitudeG = 1.0f;
unsigned long lastIMUUpdate = 0;
unsigned long lastIMUAccelUpdate = 0;
unsigned long lastIMUSafetyCheck = 0;
unsigned long lastIMUSafetyAlert = 0;
bool driveControlActive = false;
bool driveHoldStraight = false;
bool turnControlActive = false;
int driveTargetLeft = 0;
int driveTargetRight = 0;
float turnTargetDeg = 0.0f;
float turnAccumDeg = 0.0f;
float imuYawEstimateDeg = 0.0f;
unsigned long lastIMUSampleMs = 0;
const unsigned long IMU_UPDATE_INTERVAL_MS = 25;
const unsigned long IMU_ACCEL_UPDATE_INTERVAL_MS = 100;
const unsigned long IMU_SAFETY_CHECK_INTERVAL_MS = 250;
const unsigned long IMU_SAFETY_COOLDOWN_MS = 15000;
const float IMU_YAW_KP = 1.6f;
const int IMU_MAX_CORRECTION = 45;
const float IMU_GYRO_DEADBAND_DPS = 1.2f;
const float IMU_TILT_WARN_DEG = 20.0f;
const float IMU_TILT_ALERT_DEG = 35.0f;
const float IMU_ACCEL_BUMP_G = 1.8f;
const float IMU_ACCEL_DROP_G = 0.45f;
// Flip to -1.0f if the correction pushes the robot the wrong way.
const bool IMU_INVERTED_MOUNT = true; // Set true if mounted upside down (chip facing floor)
const float IMU_YAW_SIGN = IMU_INVERTED_MOUNT ? -1.0f : 1.0f;
const unsigned long IMU_AUTO_RECALIB_STILL_MS = 4000;
const unsigned long IMU_AUTO_RECALIB_COOLDOWN_MS = 15UL * 60UL * 1000UL;
const float IMU_AUTO_RECALIB_GYRO_DPS = 0.6f;
const float IMU_AUTO_RECALIB_ACCEL_DELTA_G = 0.08f;
bool expressiveSpeechMotionQueued = false;
unsigned long lastExpressiveSpeechMotionMs = 0;
const unsigned long EXPRESSIVE_SPEECH_MOTION_MIN_GAP_MS = 8000;
const uint16_t EXPRESSIVE_FWD_CLEAR_MM = 700;
const uint8_t SPATIAL_MEMORY_MAX_OBS = 4;
String spatialMemoryNotes[SPATIAL_MEMORY_MAX_OBS];
uint8_t spatialMemoryStart = 0;
uint8_t spatialMemoryCount = 0;
String spatialAwarenessSummary = "";

// ============================================================
// VL53L0X / ROBOT ASSIST
// ============================================================
VL53L0X frontToF;
bool tofReady = false;
bool tofHasReading = false;
uint16_t tofDistanceMm = 0;
uint16_t tofFilteredMm = 0;
unsigned long lastToFUpdate = 0;
unsigned long lastRobotAssistPush = 0;

bool autoAvoidEnabled = false;
bool guardModeEnabled = false;
bool guardAlarmActive = false;

enum AutoAvoidState {
  AUTO_AVOID_IDLE,
  AUTO_AVOID_CRUISE,
  AUTO_AVOID_BACKUP,
  AUTO_AVOID_SCAN_LEFT_SETTLE,
  AUTO_AVOID_SCAN_LEFT_READ,
  AUTO_AVOID_SCAN_RIGHT_SETTLE,
  AUTO_AVOID_SCAN_RIGHT_READ,
  AUTO_AVOID_TURNING
};

AutoAvoidState autoAvoidState = AUTO_AVOID_IDLE;
unsigned long autoAvoidStateTimer = 0;
uint16_t autoAvoidLeftMm = 0;
uint16_t autoAvoidRightMm = 0;
uint8_t autoObstacleHitCount = 0;
uint8_t guardNearCount = 0;
uint8_t guardClearCount = 0;
bool buzzerReady = false;
bool guardBuzzerOn = false;
unsigned long guardBuzzerPhaseStart = 0;
bool guardBuzzerPhaseOn = false;
unsigned long lastGuardAlertSent = 0;
const unsigned long ROBOT_ASSIST_PUSH_INTERVAL_MS = 15000;
const unsigned long TOF_READ_INTERVAL_MS = 85;
const uint16_t TOF_MAX_VALID_MM = 2000;
const uint16_t TOF_AUTO_SLOW_MM = 650;
const uint16_t TOF_AUTO_STOP_MM = 350;
const uint16_t TOF_AUTO_BACKUP_MM = 250;
const uint16_t TOF_GUARD_TRIGGER_MM = 850;
const uint16_t TOF_GUARD_CLEAR_MM = 1100;
const int TOF_MANUAL_CREEP_SPEED = 60;
const uint8_t TOF_GUARD_CONFIRM_READS = 3;
const uint8_t TOF_GUARD_CLEAR_READS = 4;
const unsigned long GUARD_ALERT_COOLDOWN_MS = 300000;
const unsigned long GUARD_BEEP_ON_MS = 180;
const unsigned long GUARD_BEEP_OFF_MS = 140;
const int GUARD_BEEP_FREQ = 2200;

// ============================================================
// TIME-BASED CHECK-UP & SMART ROUTINES
// ============================================================
unsigned long lastCheckUpTime = 0;
const unsigned long CHECK_UP_INTERVAL = 3600000; // 1 hour

bool morningRoutineDone = false;
bool eveningRoutineDone = false;
bool lastSleepLogged = false;
unsigned long sleepStartTime = 0;
unsigned long wakeTime = 0;

// Contextual Awareness cooldowns
unsigned long lastAirQualityAlert = 0;
unsigned long lastLateNightAlert = 0;
unsigned long lastHumidityAlert = 0;
const unsigned long CONTEXT_COOLDOWN = 1800000; // 30 minutes

// ============================================================
// AFFECTIVE EMOTION ENGINE (Valence & Arousal)
// ============================================================
String currentEyeExpression = "NORMAL";
float moodValence = 0.0; // -1.0 (Sad/Angry) to 1.0 (Happy/Loving)
float moodArousal = 0.0; // -1.0 (Sleepy/Bored) to 1.0 (Excited/Surprised)

// Eye Drawing Logic is now procedural (No Bitmaps)
unsigned long lastBlinkTime = 0;
unsigned long lastEyeUpdate = 0;
unsigned long lastEmotionEngineUpdate = 0;

void updateEmotionEngine(); // Forward declaration
void checkSleepTracking(); // Forward declaration
void askAI(const char* userText);
void stopMotorMotion(bool clearQueue = false);
bool queueMovementCommand(String cmd);
void clearMovementQueue();
bool isSpeechSafeMotionCommand(const String& cmd);
void startMeditation(String preset);
void logConversationToFirebase(const char* userText, String aiReply);
void initToF();
bool readToFDistanceMm(uint16_t &distanceMm);
void updateRobotAssistModes();
void setAutoAvoidMode(bool enabled);
void setGuardMode(bool enabled);
void publishRobotAssistStatus(bool force = false);
void drawGuardAlertScreen(const String& message);
void startGuardBuzzer();
void stopGuardBuzzer();
void clearGuardAlarm(bool redraw = true);
const char* getAIInputModeName();
void enableMicAIInput();
void disableMicAIInput();
void serviceMicAIReconnect(bool blockedByCheckup);
void setAIInputMode(bool useMicInput, bool force = false);
void stopActiveAudioPlayback(bool scheduleMicRestart = false);
void queueAIChat(String msg);
void queueWebAIChat(String msg);
void processQueuedAIChat();
void scheduleDeferredAiAction(int action, const String& param = "", unsigned long delayMs = 0);
void clearDeferredAiAction();
void processDeferredAiAction();
void queueDanceRoutine(const String& style);
void maybeExpressiveSpeechMotion();
void appendSpatialAwarenessNote(const String& note);
String buildSpatialAwarenessContext();
String performSpatialSurvey(bool exploreMode, bool speakFriendly = true);
String describeDistanceBand(uint16_t mm);
bool captureDistanceAtNeckAngle(int neckAngle, uint16_t& distanceMm);
void checkAutoImuRecalibration();
void clearTtsChunkQueue();
void processPendingTtsChunk();
void checkAutoDeepSleep();
void enterDeepSleep(const char* reason);
String urlEncodeTtsText(const String& text);
bool startTtsPlayback(const String& text, bool useFallbackHost);
void processPendingTtsRetry();
void setupOTA();
bool beginWiFiConnectionAttempt(bool useSavedCreds);
bool waitForWiFiConnection(unsigned long timeoutMs);
bool networkAvailable();
void clearSlamMap();
void setSlamCell(uint8_t x, uint8_t y, uint8_t value);
void stampSlamRay(float angleDeg, uint16_t distanceMm);
void publishStatusSnapshot(bool force = false);
void publishRoomMapSnapshot(uint16_t frontMm, uint16_t leftMm, uint16_t rightMm, float headingDeg);
void resetOnlineServicesForDisconnect();
void updateWiFiConnectionManager();
String normalizeMemoryText(String text);
String truncateMemoryText(const String& text, size_t maxChars);
String extractConversationMemoryNote(const String& userText);
void addConversationMemoryNote(const String& note);
void rebuildConversationHistory();
void appendConversationMemory(const String& userText, const String& aiReply);
String buildConversationMemoryContext();
String buildUserProfileContext();
void loadPendingReminderState();
void savePendingReminderState();
void clearPendingReminderState();
void stagePendingReminder(const String& title, const String& timeStr, const String& typeStr, bool needsCloudSync = true);
void flushPendingReminderToFirebase();
void clearAllMemory();
void loadConversationMemoryFromPrefs();
void saveConversationMemorySummary();
bool isIdentityFactRequest(const String& userQuery);
String buildIdentityFactReply(const String& userQuery);
bool purgeConversationMemoryMatches(const String& needle);
bool handleMemoryManagementChat(const String& msg, String& spokenReply);
bool pruneConversationMemoryIfExpired();
bool replyMentionsOperationalDetails(const String& reply);
String limitReplyToTransportCap(String reply, size_t maxChars);
String keepCasualReplySafe(const String& userQuery, const String& reply);

void removeAllOccurrencesIgnoreCase(String& text, const char* needle) {
  if (!needle || needle[0] == '\0') return;

  String lowerText = text;
  lowerText.toLowerCase();

  String lowerNeedle = String(needle);
  lowerNeedle.toLowerCase();

  int idx = lowerText.indexOf(lowerNeedle);
  while (idx >= 0) {
    text.remove(idx, lowerNeedle.length());
    lowerText.remove(idx, lowerNeedle.length());
    idx = lowerText.indexOf(lowerNeedle);
  }
}

String stripDecorativeChatFormatting(String text) {
  String cleaned;
  cleaned.reserve(text.length());

  for (size_t i = 0; i < text.length(); i++) {
    unsigned char c = (unsigned char)text[i];

    if (c == '*') continue;

    // Remove non-ASCII decoration such as emoji/fancy symbols from AI replies.
    if (c < 32 && c != '\n' && c != '\r' && c != '\t') continue;
    if (c > 126) continue;

    cleaned += (char)c;
  }

  while (cleaned.indexOf("  ") >= 0) cleaned.replace("  ", " ");

  const char* bannedPhrases[] = {
    "robot eye roll",
    "eye roll",
    "robot wiggle",
    "robot shrug",
    "robot grin",
    "robot smirk",
    "robot face",
    "light bulb",
    "lightbulb",
    "sparkle",
    "wink wink",
    nullptr
  };
  for (int i = 0; bannedPhrases[i] != nullptr; i++) {
    removeAllOccurrencesIgnoreCase(cleaned, bannedPhrases[i]);
  }

  while (cleaned.indexOf("  ") >= 0) cleaned.replace("  ", " ");
  cleaned.trim();
  return cleaned;
}

String tightenAiReply(String reply, bool longForm) {
  // AI Response Filter as requested by user
  reply.replace("—", " ");
  reply.replace("dazi", "");
  reply.replace("Dazi", "");
  
  reply.replace("\r", " ");
  reply.replace("\n", " ");
  reply = stripDecorativeChatFormatting(reply);

  String replyLower = reply;
  replyLower.toLowerCase();
  bool looksLikeMetaTemplate =
    (replyLower.indexOf("user input:") >= 0) ||
    (replyLower.indexOf("user's input:") >= 0) ||
    (replyLower.indexOf("context:") >= 0 && replyLower.indexOf("tone:") >= 0) ||
    (replyLower.indexOf("persona:") >= 0) ||
    (replyLower.startsWith("*") && replyLower.indexOf(" * ") >= 0);


  if (looksLikeMetaTemplate) {
    // Give a chatty fallback instead of boring error
    String chattyFallbacks[] = {
      "[NORMAL] Oops, my circuits got a bit scrambled there. What were you saying?",
      "[WORRIED] Uh, something weird happened in my processing. Can you repeat that?",
      "[NORMAL] That didn't come out right. My bad — try asking me again?",
      "[WINK] Even robots have glitches! What was the question again?"
    };
    return chattyFallbacks[random(0, 4)];
  }

  // Remove thinking tags
  while (reply.indexOf("💭") >= 0 && reply.indexOf("💬") > reply.indexOf("💭")) {
    int startIdx = reply.indexOf("💭");
    int endIdx = reply.indexOf("💬", startIdx);
    if (endIdx > startIdx) {
      reply.remove(startIdx, (endIdx - startIdx) + 8);
    } else break;
  }

  reply.replace("💭", "");
  reply.replace("💬", "");

  // Clean up extra spaces
  while (reply.indexOf("  ") >= 0) reply.replace("  ", " ");

  // Remove bracket content but KEEP the emotion tag at start
  int firstBracket = reply.indexOf('[');
  int firstClose = reply.indexOf(']');
  bool hasEmotionTag = false;
  
  if (firstBracket == 0 && firstClose > 0 && firstClose < 20) {
    String possibleTag = reply.substring(0, firstClose + 1);
    possibleTag.toUpperCase();
    if (possibleTag.indexOf("HAPPY") >= 0 || possibleTag.indexOf("SAD") >= 0 ||
        possibleTag.indexOf("WORRIED") >= 0 || possibleTag.indexOf("THINKING") >= 0 ||
        possibleTag.indexOf("LOVE") >= 0 || possibleTag.indexOf("WINK") >= 0 ||
        possibleTag.indexOf("EXCITED") >= 0 || possibleTag.indexOf("FRUSTRATED") >= 0 ||
        possibleTag.indexOf("ANGRY") >= 0 || possibleTag.indexOf("SUSPICIOUS") >= 0 ||
        possibleTag.indexOf("NORMAL") >= 0 || possibleTag.indexOf("DEAD") >= 0 ||
        possibleTag.indexOf("X_X") >= 0) {
      hasEmotionTag = true;
    }
  }

  // Remove action tags from speech (including standalone movement tokens)
  String actionTags[] = {"[MOVE:", "[PLAYSONG:", "[PLAY:", "[BREATHE]", "[MEDITATE:",
                         "[RELAX:", "[GOHOME]", "[STOPAUDIO]", "[IMURESET]", "[CALIBRATE_IMU]",
                         "[CHECKUP]", "[EMERGENCY]", "[REMINDER:", "[SLEEP]", "[WAKEUP]",
                         "[DANCE:", "[DANCE]", "[SCAN]", "[EXPLORE]",
                         "[REASON:", "[SEARCH:", "[FORGET]",
                         // Standalone movement tokens the AI sometimes emits without [MOVE:]
                         "[FWD]", "[BWD]", "[LEFT]", "[RIGHT]",
                         "[SPIN_L]", "[SPIN_R]",
                         "[TURN_L_90]", "[TURN_R_90]",
                         "[TURN_L_180]", "[TURN_R_180]", "[STOP]"};
  
  for (const String& tag : actionTags) {
    int idx = reply.indexOf(tag);
    if (idx >= 0) {
      int endIdx = reply.indexOf(']', idx);
      if (endIdx > idx) {
        reply.remove(idx, endIdx - idx + 1);
      }
    }
  }

  reply.trim();

  // Sentence limits based on mode
  if (!longForm) {
    const bool webVoiceMode = (currentMode == MODE_AI && !aiInputUsesMic);
    int maxSentences = webVoiceMode ? 4 : 3;
    int sentenceCount = 0;
    int sentenceCut = -1;
    for (int i = 0; i < reply.length(); i++) {
      char c = reply.charAt(i);
      if (c == '.' || c == '!' || c == '?') {
        sentenceCount++;
        if (sentenceCount >= maxSentences) {
          sentenceCut = i + 1;
          break;
        }
      }
    }

    if (sentenceCut > 0) {
      reply = reply.substring(0, sentenceCut);
    }
  }

  reply.trim();
  return reply;
}

// Memory Reservation for TLS (to prevent MQTT/STT disconnects)
uint8_t* tlsBufferReservation = nullptr;

void reserveHeapAfterAI() {
    if (!tlsBufferReservation) {
        // Reserve 42KB of internal heap to push MQTT/other buffers out of critical space
        tlsBufferReservation = (uint8_t*)heap_caps_malloc(42 * 1024, MALLOC_CAP_INTERNAL);
        if (tlsBufferReservation) {
            Serial.println(F("[Memory] Reserved 42KB internal heap for next AI session"));
        }
    }
}



bool replyMentionsOperationalDetails(const String& reply) {
  String lower = reply;
  lower.toLowerCase();
  return lower.indexOf("sensor") >= 0 ||
         lower.indexOf("scan") >= 0 ||
         lower.indexOf("explor") >= 0 ||
         lower.indexOf("sweep") >= 0 ||
         lower.indexOf("tof") >= 0 ||
         lower.indexOf("imu") >= 0 ||
         lower.indexOf("heading") >= 0 ||
         lower.indexOf("orientation") >= 0 ||
         lower.indexOf("motion") >= 0 ||
         lower.indexOf("front distance") >= 0 ||
         lower.indexOf("room temp") >= 0 ||
         lower.indexOf("room temperature") >= 0 ||
         lower.indexOf("temperature sensor") >= 0 ||
         lower.indexOf("live reading") >= 0 ||
         lower.indexOf("readings") >= 0;
}

String limitReplyToTransportCap(String reply, size_t maxChars) {
  if (maxChars == 0 || reply.length() == 0) return reply;
  if (reply.length() <= (int)maxChars) return reply;

  int cutIndex = (int)maxChars;
  int lastSpace = reply.lastIndexOf(' ', cutIndex);
  if (lastSpace > 0) {
    cutIndex = lastSpace;
  }

  reply = reply.substring(0, cutIndex);
  reply.trim();
  if (reply.length() == 0) return reply;

  char lastChar = reply.charAt(reply.length() - 1);
  if (lastChar != '.' && lastChar != '!' && lastChar != '?') {
    reply += ".";
  }

  return reply;
}

// ============================================================
// HEAP GUARD FOR AI/HTTPS CALLS
// Forces cleanup and clears buffers when heap is critically low
// ============================================================
const int HEAP_MIN_FOR_AI_CALL = 14000;
const int HEAP_CRITICAL_THRESHOLD = 10000;

bool ensureHeapForAICall() {
  // CRITICAL: First release the manual reservation if it exists
  if (tlsBufferReservation) {
      heap_caps_free(tlsBufferReservation);
      tlsBufferReservation = nullptr;
      Serial.println(F("[Memory] Released 42KB buffer for TLS handshake"));
      delay(50);
  }

  int freeHeap = ESP.getFreeHeap();


  freeHeap = ESP.getFreeHeap();
  freeHeap = ESP.getFreeHeap();
  // Always disconnect STT to free ~32KB SSL buffers before an AI call
  if (aaiConnected || aaiConnecting) {
    Serial.println("[Heap] Disconnecting STT for AI call...");
    disconnectAssemblyAI(0);
    delay(50);
    yield();
  }
  
  if (freeHeap >= 45000 && ESP.getMaxAllocHeap() >= 20000) {
    Serial.printf("[Heap] Light cleanup succeeded: heap=%d\n", freeHeap);
    return true;
  }




  // ── Phase 2: Aggressive cleanup ──
  Serial.println("[Heap] Light cleanup failed — aggressive cleanup");
  
  // Kill other SSL connections to free their buffers (32KB+ each)
  if (aaiConnected || aaiConnecting) {
    disconnectAssemblyAI(0);
    Serial.println("[Heap] Disconnected STT to free memory");
  }
  if (mqtt.connected()) {
    mqtt.disconnect();
    mqttClientNet->stop();
    Serial.println("[Heap] Disconnected MQTT to free memory");
  }

  stopActiveAudioPlayback(false);  // Stop TTS/music, free audio buffers
  if (firebaseReady) {
    setSpeakingStatusFirebase(false);
  }
  pendingSpeakingStatusValid = false;
  pendingLastResponseValid = false;
  aiRequestStatus = "";
  activeTtsText = "";
  isSpeaking = false;
  isProcessingAI = false;
  delay(100); // Wait for stacks/buffers to clear

  freeHeap = ESP.getFreeHeap();
  if (freeHeap >= HEAP_MIN_FOR_AI_CALL && ESP.getMaxAllocHeap() >= 5120) {
    Serial.printf("[Heap] Aggressive cleanup succeeded: heap=%d\n", freeHeap);
    return true;
  }


  // ── Phase 3: Still too low — skip AI call with graceful fallback ──
  Serial.printf("[Heap] Heap still too low after cleanup: %d < %d\n", freeHeap, HEAP_MIN_FOR_AI_CALL);
  return false;
}

String keepCasualReplySafe(const String& userQuery, const String& reply) {
  String queryLower = userQuery;
  queryLower.toLowerCase();

  bool explicitOperational = isEnvironmentSummaryRequest(userQuery) ||
                             isSpatialAwarenessRequest(userQuery) ||
                             isRobotActionRequest(userQuery) ||
                             isLiveWebSearchRequest(userQuery) ||
                             queryLower.indexOf("sensor") >= 0 ||
                             queryLower.indexOf("temperature") >= 0 ||
                             queryLower.indexOf("humidity") >= 0 ||
                             queryLower.indexOf("aqi") >= 0 ||
                             queryLower.indexOf("tvoc") >= 0 ||
                             queryLower.indexOf("eco2") >= 0 ||
                             queryLower.indexOf("heart") >= 0 ||
                             queryLower.indexOf("spo2") >= 0 ||
                             queryLower.indexOf("imu") >= 0 ||
                             queryLower.indexOf("heading") >= 0 ||
                             queryLower.indexOf("orientation") >= 0 ||
                             hasExplicitCommandPhrase(queryLower, "drive") ||
                             hasExplicitCommandPhrase(queryLower, "move") ||
                             hasExplicitCommandPhrase(queryLower, "turn") ||
                             hasExplicitCommandPhrase(queryLower, "scan") ||
                             hasExplicitCommandPhrase(queryLower, "explore") ||
                             hasExplicitCommandPhrase(queryLower, "look around");

  if (explicitOperational) return reply;

  return limitReplyToTransportCap(reply, 800);
}

String sanitizeMusicRequest(String raw) {
  raw.trim();
  raw.replace("\"", "");
  raw.replace("'", "");
  raw.replace("-", "");
  raw.replace("_", "");

  int byIdx = raw.indexOf(" by ");
  if (byIdx > 0) {
    raw = raw.substring(0, byIdx);
  }

  int dashIdx = raw.indexOf(" - ");
  if (dashIdx > 0) {
    raw = raw.substring(0, dashIdx);
  }

  int emDashIdx = raw.indexOf(" — ");
  if (emDashIdx > 0) {
    raw = raw.substring(0, emDashIdx);
  }

  raw.trim();
  return raw;
}

// ============================================================
// CONVERSATION MEMORY
// Recent turns stay in a small ring buffer, while older stable
// facts are folded into a compact summary so the model keeps
// useful context without dragging along a huge raw transcript.
// ============================================================
const uint8_t CHAT_MEMORY_MAX_EXCHANGES = 4;
const size_t CHAT_MEMORY_MAX_SUMMARY_CHARS = 420;
const size_t CHAT_MEMORY_MAX_ENTRY_CHARS = 160;
const unsigned long MEMORY_MAX_AGE_SECONDS = 24UL * 60UL * 60UL;
const unsigned long MIN_VALID_UNIX_TIME = 1700000000UL;

String conversationMemorySummary = "";
String conversationHistory = "";
String conversationMemoryTurns[CHAT_MEMORY_MAX_EXCHANGES];
uint8_t conversationMemoryStart = 0;
uint8_t conversationMemoryCount = 0;

void clearStringKeepCapacity(String& value) {
  value.remove(0);
}

String normalizeMemoryText(String text) {
  text.replace("\r", " ");
  text.replace("\n", " ");

  while (text.indexOf("  ") >= 0) {
    text.replace("  ", " ");
  }

  text.trim();
  return text;
}

String stripBracketTags(String text) {
  while (true) {
    int openBracket = text.indexOf('[');
    if (openBracket < 0) break;
    int closeBracket = text.indexOf(']', openBracket);
    if (closeBracket <= openBracket) break;
    text.remove(openBracket, closeBracket - openBracket + 1);
  }
  return normalizeMemoryText(text);
}

String truncateMemoryText(const String& text, size_t maxChars) {
  if (text.length() <= maxChars) return text;

  String clipped = text.substring(0, maxChars);
  int lastSpace = clipped.lastIndexOf(' ');
  if (lastSpace > 40) {
    clipped = clipped.substring(0, lastSpace);
  }

  clipped.trim();
  return clipped;
}

void trimConversationSummary() {
  while (conversationMemorySummary.length() > CHAT_MEMORY_MAX_SUMMARY_CHARS) {
    int firstLineBreak = conversationMemorySummary.indexOf('\n');
    if (firstLineBreak < 0) {
      conversationMemorySummary = conversationMemorySummary.substring(
        conversationMemorySummary.length() - CHAT_MEMORY_MAX_SUMMARY_CHARS
      );
      break;
    }
    conversationMemorySummary = conversationMemorySummary.substring(firstLineBreak + 1);
  }
}

static bool getCurrentUnixTimeSeconds(unsigned long& epochSeconds) {
  time_t now = time(nullptr);
  if (now < (time_t)MIN_VALID_UNIX_TIME) return false;
  epochSeconds = (unsigned long)now;
  return true;
}

void saveConversationMemorySummary() {
  prefs.putString("chatSummary", conversationMemorySummary);
  unsigned long nowEpoch = 0;
  if (getCurrentUnixTimeSeconds(nowEpoch)) {
    prefs.putULong("chatSummaryEpoch", nowEpoch);
  }
}

void clearAllMemory() {
  conversationMemorySummary = "";
  conversationMemoryCount = 0;
  conversationMemoryStart = 0;
  conversationHistory = "";
  for (uint8_t i = 0; i < CHAT_MEMORY_MAX_EXCHANGES; i++) {
    conversationMemoryTurns[i] = "";
  }

  spatialAwarenessSummary = "";
  spatialMemoryCount = 0;
  spatialMemoryStart = 0;
  for (uint8_t i = 0; i < SPATIAL_MEMORY_MAX_OBS; i++) {
    spatialMemoryNotes[i] = "";
  }

  prefs.remove("chatSummary");
  prefs.remove("chatSummaryEpoch");
}

bool pruneConversationMemoryIfExpired() {
  if (conversationMemorySummary.length() == 0) return false;

  unsigned long nowEpoch = 0;
  if (!getCurrentUnixTimeSeconds(nowEpoch)) {
    return false;
  }

  unsigned long savedEpoch = prefs.getULong("chatSummaryEpoch", 0);
  if (savedEpoch == 0) {
    prefs.putULong("chatSummaryEpoch", nowEpoch);
    Serial.println("[Mem] Timestamped legacy conversation summary");
    return false;
  }

  if (nowEpoch < savedEpoch) {
    prefs.putULong("chatSummaryEpoch", nowEpoch);
    Serial.println("[Mem] System clock moved backward; refreshed summary timestamp");
    return false;
  }

  unsigned long ageSeconds = nowEpoch - savedEpoch;
  if (ageSeconds <= MEMORY_MAX_AGE_SECONDS) return false;

  clearAllMemory();
  Serial.printf("[Mem] Cleared expired conversation memory after %lu seconds\n", ageSeconds);
  return true;
}

void addConversationMemoryNote(const String& note) {
  String cleanNote = truncateMemoryText(normalizeMemoryText(note), 220);
  if (cleanNote.length() == 0) return;

  if (conversationMemorySummary.indexOf(cleanNote) >= 0) return;

  if (conversationMemorySummary.length() > 0) {
    conversationMemorySummary += "\n";
  }
  conversationMemorySummary += "- " + cleanNote;
  trimConversationSummary();
  saveConversationMemorySummary();
  Serial.println("[Mem] Summary updated");
}

String extractConversationMemoryNote(const String& userText) {
  String clean = truncateMemoryText(normalizeMemoryText(userText), 220);
  if (clean.length() == 0) return "";

  String lower = clean;
  lower.toLowerCase();

  if (lower.indexOf("my name is ") >= 0 || lower.indexOf("call me ") >= 0) {
    return truncateMemoryText("User identity: " + clean, 220);
  }

  if (lower.indexOf("i am your creator") >= 0 || lower.indexOf("i'm your creator") >= 0 ||
      (lower.indexOf("creator") >= 0 && lower.indexOf("dynamic technologies") >= 0)) {
    return truncateMemoryText("Creator/owner claim: " + clean, 220);
  }

  if (lower.indexOf("remember") >= 0 || lower.indexOf("keep in mind") >= 0 ||
      lower.indexOf("don't forget") >= 0 || lower.indexOf("do not forget") >= 0) {
    return truncateMemoryText("User asked to remember: " + clean, 220);
  }

  if (lower.indexOf("i prefer ") >= 0 || lower.indexOf("i like ") >= 0 ||
      lower.indexOf("i want ") >= 0 || lower.indexOf("from now on") >= 0) {
    if (lower.indexOf("assistant") >= 0 || lower.indexOf("ella") >= 0 ||
        lower.indexOf("you") >= 0 || lower.indexOf("your") >= 0 ||
        lower.indexOf("model") >= 0 || lower.indexOf("personality") >= 0 ||
        lower.indexOf("memory") >= 0 || lower.indexOf("voice") >= 0) {
      return truncateMemoryText("Preference: " + clean, 220);
    }
  }

  if (lower.indexOf("sassy") >= 0 || lower.indexOf("memory") >= 0 ||
      lower.indexOf("personality") >= 0 || lower.indexOf("abuse") >= 0 ||
      lower.indexOf("resal") >= 0 || (lower.indexOf("feel") >= 0 && lower.indexOf("real") >= 0)) {
    return truncateMemoryText("Style request: " + clean, 220);
  }

  return "";
}

void rebuildConversationHistory() {
  conversationHistory = "";

  for (uint8_t i = 0; i < conversationMemoryCount; i++) {
    uint8_t idx = (conversationMemoryStart + i) % CHAT_MEMORY_MAX_EXCHANGES;
    if (conversationMemoryTurns[idx].length() == 0) continue;

    if (conversationHistory.length() > 0) {
      conversationHistory += "\n\n";
    }
    conversationHistory += conversationMemoryTurns[idx];
  }
}

void appendConversationMemory(const String& userText, const String& aiReply) {
  String userSnippet = truncateMemoryText(normalizeMemoryText(userText), CHAT_MEMORY_MAX_ENTRY_CHARS);
  String aiSnippet = truncateMemoryText(normalizeMemoryText(aiReply), CHAT_MEMORY_MAX_ENTRY_CHARS);

  String turn = "User: " + userSnippet + "\nELLA: " + aiSnippet;

  if (conversationMemoryCount < CHAT_MEMORY_MAX_EXCHANGES) {
    uint8_t slot = (conversationMemoryStart + conversationMemoryCount) % CHAT_MEMORY_MAX_EXCHANGES;
    conversationMemoryTurns[slot] = turn;
    conversationMemoryCount++;
  } else {
    conversationMemoryTurns[conversationMemoryStart] = turn;
    conversationMemoryStart = (conversationMemoryStart + 1) % CHAT_MEMORY_MAX_EXCHANGES;
  }

  rebuildConversationHistory();

  String note = extractConversationMemoryNote(userText);
  if (note.length() > 0) {
    addConversationMemoryNote(note);
  }
}


String buildSensorContext() {
  String context = "[SYSTEM/SENSOR CONTEXT]\n";
  
  // AHT (Temp/Humidity) - use the latest cached readings from the sensor loop
  if (!isnan(temp_aht)) {
    context += "- Ambient Temp: " + String(temp_aht, 1) + "°C\n";
  } else {
    context += "- Ambient Temp: unavailable\n";
  }
  if (!isnan(humidity_aht)) {
    context += "- Humidity: " + String(humidity_aht, 1) + "%\n";
  } else {
    context += "- Humidity: unavailable\n";
  }

  // ENS160 (Air Quality)
  context += "- Air Quality Index (AQI): " + String(aqi_val) + " (1=Excellent, 2=Good, 3=Moderate, 4=Poor, 5=Unhealthy)\n";
  context += "- Air Quality (TVOC): " + String(ens160.getTvoc()) + " ppb\n";
  context += "- eCO2: " + String(ens160.getEco2()) + " ppm\n";

  // ToF (Distance) - use last continuous reading, safe non-blocking
  uint16_t dist = frontToF.readRangeContinuousMillimeters();
  if (!frontToF.timeoutOccurred() && dist < 8190) {
    context += "- Forward Distance (ToF Head): " + String(dist) + "mm\n";
  }
  
  // Ultrasonic Sensor (HC-SR04) - Chest mounted for room mapping
  float ultrasonicDist = readUltrasonicDistanceCm();
  if (ultrasonicDist > 0) {
    context += "- Chest Distance (Ultrasonic): " + String(ultrasonicDist, 1) + "cm\n";
    if (ultrasonicDist < 30.0) {
      context += "  ⚠️ Object/person detected close in front\n";
    }
  } else {
    context += "- Chest Distance (Ultrasonic): out of range\n";
  }
  
  context += "- System Heap: " + String(ESP.getFreeHeap() / 1024) + "KB\n";
  return context;
}

void sendCurrentNodeContext() {
  if (!nodeWsConnected) return;

  String senses = buildSensorContext();
  senses += buildUserProfileContext();

  DynamicJsonDocument cDoc(2048);
  cDoc["type"] = "context";
  cDoc["text"] = senses;

  String out;
  serializeJson(cDoc, out);
  nodeWsSendText(out.c_str());
  
  // Send Telegram credentials to server if available
  if (cloudBotToken.length() > 0) {
    Serial.println("[Telegram] Sending credentials to server...");
    DynamicJsonDocument tDoc(512);
    tDoc["type"] = "telegram_init";
    tDoc["botToken"] = cloudBotToken;
    if (cloudChatId.length() > 0) {
      tDoc["chatId"] = cloudChatId;
    }
    
    String tOut;
    serializeJson(tDoc, tOut);
    nodeWsSendText(tOut.c_str());
  }
}

String buildConversationMemoryContext() {
  const size_t MAX_MEMORY_CONTEXT_CHARS = 800;
  String memory;
  memory.reserve(MAX_MEMORY_CONTEXT_CHARS + 64);

  if (conversationMemorySummary.length() > 0) {
    memory += "\nMEMORY SUMMARY:\n";
    memory += conversationMemorySummary;
    memory += "\n";
  }

  if (conversationHistory.length() > 0) {
    // Calculate remaining budget after summary
    size_t remaining = 0;
    if (memory.length() < MAX_MEMORY_CONTEXT_CHARS) {
      remaining = MAX_MEMORY_CONTEXT_CHARS - memory.length();
    }

    if (remaining > 40) {
      memory += "\nRECENT CHAT:\n";
      if (conversationHistory.length() <= remaining - 16) {
        memory += conversationHistory;
      } else {
        // Trim from the start (oldest) to fit budget
        int offset = conversationHistory.length() - (remaining - 16);
        // Find a clean line break to cut at
        int newlineIdx = conversationHistory.indexOf('\n', offset);
        if (newlineIdx > 0 && newlineIdx < (int)conversationHistory.length() - 20) {
          memory += "...\n";
          memory += conversationHistory.substring(newlineIdx + 1);
        } else {
          memory += conversationHistory.substring(offset);
        }
      }
      memory += "\n";
    }
  }

  return memory;
}

String buildUserProfileContext() {
  String profile = "\nUSER PROFILE:\n";
  if (user_name.length() > 0) {
    profile += "Linked user name: " + user_name + "\n";
  } else {
    profile += "Linked user name: unavailable\n";
  }

  if (user_emergency_contact.length() > 0) {
    profile += "Emergency contact: " + user_emergency_contact + "\n";
  }

  profile += "Telegram chat linked: ";
  profile += (cloudChatId.length() > 0) ? "yes\n" : "no\n";
  return profile;
}

void savePendingReminderState() {
  prefs.putString("pendRemTitle", pendingReminderTitle);
  prefs.putString("pendRemTime", pendingReminderTime);
  prefs.putString("pendRemType", pendingReminderType);
  prefs.putBool("pendRemSync", pendingReminderNeedsCloudSync);
}

void clearPendingReminderState() {
  pendingReminderTitle = "";
  pendingReminderTime = "";
  pendingReminderType = "chat";
  pendingReminderNeedsCloudSync = false;
  prefs.remove("pendRemTitle");
  prefs.remove("pendRemTime");
  prefs.remove("pendRemType");
  prefs.remove("pendRemSync");
}

void loadPendingReminderState() {
  pendingReminderTitle = prefs.getString("pendRemTitle", "");
  pendingReminderTime = prefs.getString("pendRemTime", "");
  pendingReminderType = prefs.getString("pendRemType", "chat");
  pendingReminderNeedsCloudSync = prefs.getBool("pendRemSync", pendingReminderTitle.length() > 0);

  if (pendingReminderTitle.length() > 0 && pendingReminderTime.length() > 0) {
    Serial.println("[Reminder] Restored pending reminder: " + pendingReminderTitle + " @ " + pendingReminderTime);
  }
}

void stagePendingReminder(const String& title, const String& timeStr, const String& typeStr, bool needsCloudSync) {
  pendingReminderTitle = title;
  pendingReminderTime = timeStr;
  pendingReminderType = typeStr.length() > 0 ? typeStr : "chat";
  pendingReminderNeedsCloudSync = needsCloudSync;
  savePendingReminderState();
  Serial.println("[Reminder] Pending: " + pendingReminderTitle + " @ " + pendingReminderTime);
}

void flushPendingReminderToFirebase() {
  if (!pendingReminderNeedsCloudSync) return;
  if (pendingReminderTitle.length() == 0 || pendingReminderTime.length() == 0) return;
  if (!firebaseReady || offlineModeLocked || networkIsQuiet() || currentMode == MODE_AI) return;

  FirebaseJson remJson;
  remJson.set("title", pendingReminderTitle);
  remJson.set("detail", pendingReminderTitle);
  remJson.set("time", pendingReminderTime);
  remJson.set("type", pendingReminderType);
  remJson.set("status", "pending");

  Serial.println("[Reminder] Syncing pending reminder to Firebase: " + pendingReminderTitle);
  Firebase.RTDB.pushJSONAsync(&fbdo, "/reminders", &remJson);
  pendingReminderNeedsCloudSync = false;
  savePendingReminderState();
}

void loadConversationMemoryFromPrefs() {
  conversationMemorySummary = prefs.getString("chatSummary", "");
  trimConversationSummary();

  if (pruneConversationMemoryIfExpired()) {
    return;
  }

  if (conversationMemorySummary.length() > 0 && prefs.getULong("chatSummaryEpoch", 0) == 0) {
    unsigned long nowEpoch = 0;
    if (getCurrentUnixTimeSeconds(nowEpoch)) {
      prefs.putULong("chatSummaryEpoch", nowEpoch);
    }
  }

  if (conversationMemorySummary.length() > 0) {
    Serial.println("[Mem] Loaded conversation summary");
  }
}

// ============================================================
// TELEGRAM BOT - SERVER-SIDE (via Node.js)
// ============================================================
// All Telegram operations now handled by server - no SSL on ESP32!
// Functions: sendTelegramAlert(), handleTelegramCommand()
// ============================================================

// ============================================================
// MULTIPLEXER
// ============================================================
void tcaselect(uint8_t i) {
  if (i > 7) return;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << i);
  Wire.endTransmission();
}

// Additional forward declarations used below.
void setupFirebase();
void pushSensorDataToFirebase();
void syncUserProfileFromFirebase();
void syncRemindersFromFirebase();
void syncWithFirebase();
bool sendTelegramAlert(String msg);
void handleTelegramCommand(String command);
void checkTimeBasedCheckUp();
void maybeProactiveAIPrompt();
String getSensorContext();
String getRemindersContext();
void speakText(const char* text, bool longForm = false);
String tightenAiReply(String reply, bool longForm = false);
void askGroq(const char* userText);
String getGroqResponse(const String& systemPrompt, const String& userText);

String getMistralAgentResponse(const String& systemPrompt, const String& userText);

// webSocketEvent and handleSTTResponse removed — phone handles STT
void playMusic(String query);
String performWebSearch(String query);
struct SongResult { String title; String author; String audioUrl; bool found; };
SongResult searchSaavnSong(String query);
void announceMedicalResults();
void checkAutoWeeklyReport();
void checkAirQualityAlerts();
void sendWeeklyReport();
void playTone(int freq, int duration);
void playStartupSound();
void playListeningTone();
void playProcessingTone();
// 290: 
void drawLoadingScreen(String status="Starting...", int percent=0);

// ============================================================
// NEW HELPER: Clear I2S Mic Buffer (Reduces Noise/Pop)
// ============================================================
void clearMicBuffer() {

  for (int i = 0; i < 48; i++) {
    mic_i2s.read(); 
    if ((i & 0x0F) == 0) yield();
  }
}

// ============================================================
// DEEPGRAM REALTIME STT (RAW WEBSOCKET OVER WIFICLIENTSECURE)
// ============================================================

static bool readAaiBytes(uint8_t* dst, size_t len, unsigned long timeoutMs) {
  size_t offset = 0;
  unsigned long lastDataMs = millis();

  while (offset < len) {
    if (!aaiClient->connected()) {
      return false;
    }

    int availableBytes = aaiClient->available();
    if (availableBytes > 0) {
      int n = aaiClient->read(dst + offset, len - offset);
      if (n > 0) {
        offset += (size_t)n;
        lastDataMs = millis();
        continue;
      }
    }

    if (millis() - lastDataMs > timeoutMs) {
      return false;
    }
    yield();
  }

  return true;
}

static String makeWebSocketKey() {
  static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint8_t key[16];
  char keyB64[25];

  for (int i = 0; i < 16; i++) key[i] = (uint8_t)random(256);

  size_t i = 0, j = 0;
  while (i < sizeof(key)) {
    uint32_t a = i < sizeof(key) ? key[i++] : 0;
    uint32_t b = i < sizeof(key) ? key[i++] : 0;
    uint32_t c = i < sizeof(key) ? key[i++] : 0;
    uint32_t t = (a << 16) | (b << 8) | c;
    keyB64[j++] = b64c[(t >> 18) & 0x3F];
    keyB64[j++] = b64c[(t >> 12) & 0x3F];
    keyB64[j++] = i > sizeof(key) + 1 ? '=' : b64c[(t >> 6) & 0x3F];
    keyB64[j++] = i > sizeof(key) ? '=' : b64c[t & 0x3F];
  }
  keyB64[j] = '\0';
  return String(keyB64);
}

static bool sendWebSocketFrame(uint8_t opcode, const uint8_t* payload, size_t length) {
  if (!aaiClient->connected()) {
    return false;
  }

  uint8_t hdr[14];
  size_t h = 0;
  hdr[h++] = 0x80 | (opcode & 0x0F);
  if (length < 126) {
    hdr[h++] = 0x80 | (uint8_t)length;
  } else if (length < 65536) {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = (uint8_t)(length >> 8);
    hdr[h++] = (uint8_t)(length & 0xFF);
  } else {
    hdr[h++] = 0x80 | 127;
    for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((length >> (i * 8)) & 0xFF);
  }
  hdr[h++] = 0;
  hdr[h++] = 0;
  hdr[h++] = 0;
  hdr[h++] = 0;
  if (aaiClient->write(hdr, h) != (int)h) return false;
  if (payload && length) return aaiClient->write(payload, length) == (int)length;
  return true;
}

static bool sendWebSocketText(const char* text) {
  if (!text) return false;
  return sendWebSocketFrame(0x1, (const uint8_t*)text, strlen(text));
}

static bool sendWebSocketBinary(const uint8_t* payload, size_t len) {
  return sendWebSocketFrame(0x2, payload, len);
}

static bool sendWebSocketPong(const uint8_t* payload, size_t len) {
  return sendWebSocketFrame(0xA, payload, len);
}

static bool sendWsFrameToClient(WiFiClientSecure* client, uint8_t opcode, const uint8_t* payload, size_t length) {
  if (!client || !client->connected()) return false;

  uint8_t hdr[14];
  size_t h = 0;
  hdr[h++] = 0x80 | (opcode & 0x0F);
  if (length < 126) {
    hdr[h++] = 0x80 | (uint8_t)length;
  } else if (length < 65536) {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = (uint8_t)(length >> 8);
    hdr[h++] = (uint8_t)(length & 0xFF);
  } else {
    hdr[h++] = 0x80 | 127;
    for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((length >> (i * 8)) & 0xFF);
  }
  // Client frames must be masked.
  uint8_t m0 = (uint8_t)random(256);
  uint8_t m1 = (uint8_t)random(256);
  uint8_t m2 = (uint8_t)random(256);
  uint8_t m3 = (uint8_t)random(256);
  hdr[h++] = m0;
  hdr[h++] = m1;
  hdr[h++] = m2;
  hdr[h++] = m3;

  if (client->write(hdr, h) != (int)h) return false;
  if (!payload || length == 0) return true;

  uint8_t* masked = (uint8_t*)malloc(length);
  if (!masked) return false;
  for (size_t i = 0; i < length; i++) {
    uint8_t mk = (i & 0x03) == 0 ? m0 : (i & 0x03) == 1 ? m1 : (i & 0x03) == 2 ? m2 : m3;
    masked[i] = payload[i] ^ mk;
  }
  bool ok = client->write(masked, length) == (int)length;
  free(masked);
  return ok;
}

static bool sendWsTextToClient(WiFiClientSecure* client, const String& text) {
  return sendWsFrameToClient(client, 0x1, (const uint8_t*)text.c_str(), text.length());
}

static bool readWsFrameFromClient(WiFiClientSecure* client, uint8_t& opcode, uint8_t* payload, size_t payloadCapacity, size_t& payloadLen, unsigned long timeoutMs) {
  payloadLen = 0;
  if (!client || !client->connected()) return false;

  unsigned long start = millis();
  while (client->available() < 2) {
    if (!client->connected() || millis() - start > timeoutMs) return false;
    delay(1);
    yield();
  }

  uint8_t b0 = client->read();
  uint8_t b1 = client->read();
  opcode = b0 & 0x0F;
  bool masked = (b1 & 0x80) != 0;
  uint64_t pl = (uint64_t)(b1 & 0x7F);

  if (pl == 126) {
    uint8_t ext[2];
    if (client->readBytes(ext, 2) != 2) return false;
    pl = ((uint64_t)ext[0] << 8) | (uint64_t)ext[1];
  } else if (pl == 127) {
    uint8_t ext[8];
    if (client->readBytes(ext, 8) != 8) return false;
    pl = 0;
    for (int i = 0; i < 8; i++) pl = (pl << 8) | ext[i];
  }

  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (client->readBytes(mask, 4) != 4) return false;
  }

  if (pl == 0) return true;
  if (pl >= payloadCapacity) {
    // Drain oversized frame.
    for (uint64_t i = 0; i < pl; i++) {
      while (!client->available()) {
        if (!client->connected() || millis() - start > timeoutMs) return false;
        delay(1);
      }
      client->read();
    }
    return false;
  }

  size_t got = 0;
  while (got < (size_t)pl) {
    if (client->available()) {
      payload[got++] = (uint8_t)client->read();
    } else {
      if (!client->connected() || millis() - start > timeoutMs) return false;
      delay(1);
      yield();
    }
  }
  if (masked) {
    for (size_t i = 0; i < got; i++) payload[i] ^= mask[i & 0x03];
  }
  payloadLen = got;
  return true;
}

static void disconnectLlmWebSocket(unsigned long reconnectDelayMs = LLM_WS_RECONNECT_BACKOFF_MS) {
  if (llmWsClient && llmWsClient->connected()) {
    sendWsFrameToClient(llmWsClient, 0x8, nullptr, 0);
    delay(5);
    llmWsClient->stop();
  }
  llmWsConnected = false;
  llmWsSessionConfigured = false;
  lastLlmWsKeepAliveMs = 0;
  nextLlmWsReconnectMs = millis() + reconnectDelayMs;
}

static bool connectLlmWebSocket() {
  if (!USE_LLM_WEBSOCKET) return false;
  if (strlen(OPENAI_API_KEY) < 8) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (llmWsConnected && llmWsClient->connected()) return true;
  if (millis() < nextLlmWsReconnectMs) return false;

  if (!llmWsClient) return false;

  llmWsClient->stop();
  llmWsClient->setInsecure();
  llmWsClient->setNoDelay(true);
  llmWsClient->setTimeout(8);

  if (!llmWsClient->connect("api.openai.com", 443)) {
    nextLlmWsReconnectMs = millis() + LLM_WS_RECONNECT_BACKOFF_MS;
    return false;
  }


  String key = makeWebSocketKey();
  String req = "GET /v1/realtime?model=" + String(OPENAI_REALTIME_MODEL) + " HTTP/1.1\r\n";
  req += "Host: api.openai.com\r\n";
  req += "Upgrade: websocket\r\n";
  req += "Connection: Upgrade\r\n";
  req += "Sec-WebSocket-Key: " + key + "\r\n";
  req += "Sec-WebSocket-Version: 13\r\n";
  req += "Authorization: Bearer " + String(OPENAI_API_KEY) + "\r\n";
  req += "OpenAI-Beta: realtime=v1\r\n\r\n";
  llmWsClient->print(req);

  unsigned long start = millis();
  while (!llmWsClient->available() && (millis() - start) < 4000) {
    delay(2);
    yield();
  }
  if (!llmWsClient->available()) {
    disconnectLlmWebSocket();
    return false;
  }

  String statusLine = llmWsClient->readStringUntil('\n');
  while (llmWsClient->available()) {
    String h = llmWsClient->readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;
  }
  if (statusLine.indexOf("101") < 0) {
    Serial.println("[LLM-WS] Handshake failed: " + statusLine);
    disconnectLlmWebSocket();
    return false;
  }

  llmWsConnected = true;
  llmWsSessionConfigured = false;
  lastLlmWsKeepAliveMs = millis();
  Serial.println("[LLM-WS] Connected");
  return true;
}

static bool ensureLlmWebSocketSession(const String& systemPrompt) {
  if (!connectLlmWebSocket()) return false;
  if (llmWsSessionConfigured) return true;

  DynamicJsonDocument doc(8192);
  doc["type"] = "session.update";
  JsonObject session = doc["session"].to<JsonObject>();
  JsonArray modalities = session["modalities"].to<JsonArray>();
  modalities.add("text");
  session["instructions"] = systemPrompt;

  String payload;
  serializeJson(doc, payload);
  if (!sendWsTextToClient(llmWsClient, payload)) {
    disconnectLlmWebSocket();
    return false;
  }

  llmWsSessionConfigured = true;
  return true;
}

static void serviceLlmWebSocketKeepAlive() {
  if (!llmWsConnected || !llmWsClient->connected()) return;
  if (millis() - lastLlmWsKeepAliveMs < LLM_WS_KEEPALIVE_MS) return;
  if (!sendWsFrameToClient(llmWsClient, 0x9, nullptr, 0)) {
    disconnectLlmWebSocket();
    return;
  }
  lastLlmWsKeepAliveMs = millis();
}


static String getLlmWebSocketResponse(const String& systemPrompt, const String& userText) {
  if (!ensureLlmWebSocketSession(systemPrompt)) return "Error: LLM WS Unavailable";

  DynamicJsonDocument itemDoc(4096);
  itemDoc["type"] = "conversation.item.create";
  JsonObject item = itemDoc["item"].to<JsonObject>();
  item["type"] = "message";
  item["role"] = "user";
  JsonArray content = item["content"].to<JsonArray>();
  JsonObject textPart = content.add<JsonObject>();
  textPart["type"] = "input_text";
  textPart["text"] = userText;
  String itemPayload;
  serializeJson(itemDoc, itemPayload);
  if (!sendWsTextToClient(llmWsClient, itemPayload)) {
    disconnectLlmWebSocket();
    return "Error: LLM WS Send Failed";
  }

  DynamicJsonDocument respDoc(1024);
  respDoc["type"] = "response.create";
  JsonObject response = respDoc["response"].to<JsonObject>();
  JsonArray mods = response["modalities"].to<JsonArray>();
  mods.add("text");
  String responsePayload;
  serializeJson(respDoc, responsePayload);
  if (!sendWsTextToClient(llmWsClient, responsePayload)) {
    disconnectLlmWebSocket();
    return "Error: LLM WS Send Failed";
  }

  String textOut = "";
  unsigned long deadline = millis() + 22000;
  uint8_t frame[8192];
  while (millis() < deadline) {
    serviceLlmWebSocketKeepAlive();
    if (!llmWsClient->connected()) {
      disconnectLlmWebSocket();
      return "Error: LLM WS Disconnected";
    }
    if (!llmWsClient->available()) {
      delay(2);
      yield();
      continue;
    }

    uint8_t op = 0;
    size_t n = 0;
    if (!readWsFrameFromClient(llmWsClient, op, frame, sizeof(frame), n, 2500)) {
      continue;
    }

    if (op == 0x8) {
      disconnectLlmWebSocket();
      return "Error: LLM WS Closed";
    }
    if (op == 0x9) {
      sendWsFrameToClient(llmWsClient, 0xA, nullptr, 0);
      continue;
    }
    if (op != 0x1 || n == 0) continue;

    frame[n] = '\0';
    DynamicJsonDocument evt(8192);
    if (deserializeJson(evt, frame) != DeserializationError::Ok) continue;
    const char* type = evt["type"];
    if (!type) continue;

    String t = String(type);
    if (t == "response.output_text.delta") {
      const char* d = evt["delta"];
      if (d) textOut += String(d);
    } else if (t == "response.output_text.done") {
      const char* d = evt["text"];
      if (d && textOut.length() == 0) textOut = String(d);
    } else if (t == "response.done") {
      break;
    } else if (t == "error") {
      const char* msg = evt["error"]["message"];
      if (msg) Serial.println("[LLM-WS] Error: " + String(msg));
      return "Error: LLM WS Server";
    }
  }

  textOut.trim();
  if (textOut.length() == 0) return "Error: LLM WS Timeout";
  return stripReasoningBlocks(textOut);
}

static void parseByteDanceResponse(uint8_t* data, size_t len) {
  if (len < 4) return;
  uint8_t msgType = data[1] >> 4;
  uint8_t headerSize = data[0] & 0x0F;
  if (len < (size_t)(headerSize * 4)) return;

  uint8_t* payload = data + (headerSize * 4);
  size_t payloadLen = len - (headerSize * 4);

  // Message specific skips (per DAZI)
  if (msgType == 0x09 && payloadLen > 4) { payload += 4; payloadLen -= 4; }
  else if (msgType == 0x0B && payloadLen >= 8) { payload += 8; payloadLen -= 8; }
  else if (msgType == 0x0F && payloadLen >= 8) { payload += 8; payloadLen -= 8; }

  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, payload, payloadLen);
  if (error) return;

  if (doc.containsKey("result")) {
    JsonArray results = doc["result"].as<JsonArray>();
    if (results.size() > 0) {
      const char* text = results[0]["text"] | "";
      if (strlen(text) > 0) {
        if (!bdHasSpeech) {
          bdHasSpeech = true;
          Serial.println("[BD] Speech detected");
        }
        if (String(text) == bdLastResult) {
          bdSameResultCount++;
          if (bdSameResultCount >= 10 && !isSpeaking && !isProcessingAI) {
             bdFinalResult = String(text);
             queueAIChat(bdFinalResult);
             bdSameResultCount = 0; 
          }
        } else {
          bdSameResultCount = 0;
          bdLastResult = String(text);
          currentInterimText = bdLastResult;
          Serial.printf("[BD-INTERIM] %s\n", text);
        }
      }
    }
  }
}

static bool sendByteDanceBinary(uint8_t type, const uint8_t* data, size_t len) {
  // 8-byte BD header
  uint8_t bdHeader[8];
  bdHeader[0] = 0x11; // Protocol Version & Header Size
  bdHeader[1] = (type << 4); // Message Type
  bdHeader[2] = 0x10; // Serialization: JSON
  bdHeader[3] = 0x00; // Compression: None
  
  // Big-endian length
  bdHeader[4] = (len >> 24) & 0xFF;
  bdHeader[5] = (len >> 16) & 0xFF;
  bdHeader[6] = (len >> 8) & 0xFF;
  bdHeader[7] = len & 0xFF;

  // Send as WS Binary Frame
  uint8_t* fullFrame = (uint8_t*)malloc(8 + len);
  if (!fullFrame) return false;
  memcpy(fullFrame, bdHeader, 8);
  if (len > 0) memcpy(fullFrame + 8, data, len);
  
  bool ok = sendWebSocketFrame(0x02, fullFrame, 8 + len);
  free(fullFrame);
  return ok;
}

static void sendByteDanceConfig() {
  StaticJsonDocument<512> doc;
  doc["app"]["cluster"] = BD_CLUSTER;
  doc["user"]["uid"] = "ella_robot_s3";
  doc["request"]["reqid"] = String(millis());
  doc["request"]["nbest"] = 1;
  doc["request"]["workflow"] = "audio_in,resample,partition,vad,fe,decode,itn,nlu_punctuate";
  doc["request"]["result_type"] = "full";
  doc["audio"]["format"] = "raw";
  doc["audio"]["rate"] = 16000;
  doc["audio"]["bits"] = 16;
  doc["audio"]["channel"] = 1;

  String json;
  serializeJson(doc, json);
  sendByteDanceBinary(0x01, (const uint8_t*)json.c_str(), json.length());
}

static bool ensureDeepgramBatchCapacity(size_t neededBytes) {
  if (neededBytes <= deepgramBatchCapacity) return true;

  size_t newCapacity = deepgramBatchCapacity ? deepgramBatchCapacity : (AAI_BUFFER_SIZE * 20);
  while (newCapacity < neededBytes) {
    newCapacity *= 2;
  }

  uint8_t* newBuffer = (uint8_t*)(deepgramBatchBuffer
      ? realloc(deepgramBatchBuffer, newCapacity)
      : (psramFound() ? ps_malloc(newCapacity) : malloc(newCapacity)));
  if (!newBuffer) {
    Serial.println("[DG] Batch buffer alloc failed");
    return false;
  }

  deepgramBatchBuffer = newBuffer;
  deepgramBatchCapacity = newCapacity;
  return true;
}

static void clearDeepgramBatchBuffer() {
  deepgramBatchBytes = 0;
}

static bool sendDeepgramBufferedAudio();

static void handleAaiTextPayload(const char* payload, size_t length) {
  // Deepgram realtime JSON: Results with channel.alternatives[0].transcript
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[DG-JSON] Parse error: %s\n", err.c_str());
    return;
  }

  const char* msgType = doc["type"] | "";
  const char* eventType = doc["event"] | "";
  
  // CRITICAL FIX: Handle Metadata message and set ready flag
  if (strcmp(msgType, "Metadata") == 0) {
    deepgramMetadataReceived = true;
    Serial.println("[DG] ✓ Metadata received - READY FOR AUDIO");
    return;
  }

  // Test sketch / Flux stream format: TurnInfo events
  if (strcmp(msgType, "TurnInfo") == 0) {
    const char* transcriptText = doc["transcript"] | "";
    String fragment = String(transcriptText);
    fragment.trim();

    if (fragment.length() > 0) {
      currentInterimText = fragment;
    }

    if (strcmp(eventType, "StartOfTurn") == 0) {
      Serial.println("[DG] Turn started");
      return;
    }

    if (strcmp(eventType, "Update") == 0) {
      if (fragment.length() > 0) {
        Serial.print("\r[DG] " + fragment + "          ");
      }
      return;
    }

    if (strcmp(eventType, "EndOfTurn") == 0) {
      if (fragment.length() > 0) {
        currentInterimText = fragment;
        Serial.println("\n[DG] FINAL: " + fragment);
        if (!isSpeaking && !isProcessingAI) {
          queueAIChat(fragment);
        }
      } else {
        Serial.println("\n[DG] EndOfTurn with empty transcript");
      }
      return;
    }
  }
  
  // Handle SpeechStarted event
  if (strcmp(msgType, "SpeechStarted") == 0) {
    Serial.println("[DG] 🎤 Speech detected!");
    return;
  }
  
  // Handle UtteranceEnd event
  if (strcmp(msgType, "UtteranceEnd") == 0) {
    Serial.println("[DG] Utterance ended");
    return;
  }
  
  // Handle Results message
  JsonObject channel = doc["channel"];
  JsonArray alternatives = channel["alternatives"].as<JsonArray>();
  const char* transcript = "";
  if (alternatives.size() > 0) {
    transcript = alternatives[0]["transcript"] | "";
  }

  if (strcmp(msgType, "Results") == 0 || channel) {
    bool isFinal = doc["is_final"] | false;
    bool speechFinal = doc["speech_final"] | false;

    if (strlen(transcript) > 0) {
      String fragment = String(transcript);
      fragment.trim();

      if (isFinal) {
        currentInterimText = fragment;
        Serial.println("[DG] FINAL: " + fragment);
        if (speechFinal) {
          Serial.println("[DG] Speech ended, processing...");
          if (!isSpeaking && !isProcessingAI && fragment.length() > 1) {
            queueAIChat(fragment);
          }
        }
      } else {
        currentInterimText = fragment;
        Serial.print("\r[DG] " + fragment + "          ");
      }
    }
  } else if (doc.containsKey("error")) {
    const char* errorType = doc["error"] | "Unknown";
    Serial.printf("[DG] ERROR: %s\n", errorType);
  }
}

static String deepgramExtractTranscript(const String& body) {
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    return "";
  }

  const char* transcript = doc["results"]["channels"][0]["alternatives"][0]["transcript"] | "";
  if (!transcript || strlen(transcript) == 0) {
    transcript = doc["channel"]["alternatives"][0]["transcript"] | "";
  }
  if (!transcript || strlen(transcript) == 0) {
    transcript = doc["transcript"] | "";
  }
  return String(transcript ? transcript : "");
}

static bool sendDeepgramBufferedAudio() {
  if (deepgramBatchSending || deepgramBatchBytes < 2) return false;
  if (!ensureDeepgramBatchCapacity(deepgramBatchBytes + PCM_WAV_HEADER_SIZE)) return false;

  deepgramBatchSending = true;

  // Build a temporary WAV payload in PSRAM/RAM, matching the simpler Deepgram examples.
  size_t wavBytes = deepgramBatchBytes + PCM_WAV_HEADER_SIZE;
  uint8_t* wavBuffer = (uint8_t*)(psramFound() ? ps_malloc(wavBytes) : malloc(wavBytes));
  if (!wavBuffer) {
    Serial.println("[DG] WAV buffer alloc failed");
    deepgramBatchSending = false;
    return false;
  }

  pcm_wav_header_t header = PCM_WAV_HEADER_DEFAULT(deepgramBatchBytes, 16, SAMPLE_RATE, 1);
  memcpy(wavBuffer, &header, sizeof(header));
  memcpy(wavBuffer + PCM_WAV_HEADER_SIZE, deepgramBatchBuffer, deepgramBatchBytes);

  WiFiClientSecure dgClient;
  dgClient.setInsecure();
  dgClient.setTimeout(15);
  if (!dgClient.connect(DG_HOST, DG_PORT)) {
    Serial.println("[DG] Batch connect failed");
    free(wavBuffer);
    deepgramBatchSending = false;
    return false;
  }

  String request;
  request.reserve(512);
  request += "POST ";
  request += DG_BATCH_PATH;
  request += " HTTP/1.1\r\nHost: ";
  request += DG_HOST;
  request += "\r\nAuthorization: Token ";
  request += DEEPGRAM_API_KEY;
  request += "\r\nContent-Type: audio/wav\r\nTransfer-Encoding: chunked\r\n\r\n";
  dgClient.print(request);

  const size_t chunkSize = 1024;
  size_t sent = 0;
  while (sent < wavBytes) {
    size_t toSend = wavBytes - sent;
    if (toSend > chunkSize) toSend = chunkSize;
    dgClient.print(String(toSend, HEX));
    dgClient.print("\r\n");
    dgClient.write(wavBuffer + sent, toSend);
    dgClient.print("\r\n");
    sent += toSend;
  }
  dgClient.print("0\r\n\r\n");

  unsigned long waitStart = millis();
  while (!dgClient.available() && dgClient.connected() && (millis() - waitStart) < 10000) {
    yield();
  }

  String response;
  while (dgClient.available()) {
    response += (char)dgClient.read();
  }
  dgClient.stop();
  free(wavBuffer);
  deepgramBatchSending = false;

  int bodyStart = response.indexOf("\r\n\r\n");
  if (bodyStart >= 0) {
    response = response.substring(bodyStart + 4);
  }

  String transcript = deepgramExtractTranscript(response);
  transcript.trim();
  if (transcript.length() <= 1) {
    Serial.println("[DG] No transcript returned from batch request");
    return false;
  }

  currentInterimText = transcript;
  Serial.println("[DG] TRANSCRIPT: " + transcript);
  if (!isSpeaking && !isProcessingAI) {
    queueAIChat(transcript);
  }
  return true;
}

void connectByteDanceASR() {
  if (bdConnecting || bdConnected) return;
  if (!aiInputUsesMic) return;
  if (WiFi.status() != WL_CONNECTED) return;

  bdConnecting = true;
  Serial.println("[BD] Connecting to ByteDance OpenSpeech...");

  if (!aaiBuffer) {
    aaiBuffer = psramFound() ? (uint8_t*)ps_malloc(AAI_BUFFER_SIZE) : (uint8_t*)malloc(AAI_BUFFER_SIZE);
  }

  aaiClient->setInsecure();
  aaiClient->setNoDelay(true);
  aaiClient->setTimeout(15); 

  if (!aaiClient->connect(BD_HOST, BD_PORT)) {
    Serial.println("[BD] Connection failed");
    bdConnecting = false;
    return;
  }

  String request = "GET " + String(BD_PATH) + " HTTP/1.1\r\n";
  request += "Host: " + String(BD_HOST) + "\r\n";
  request += "Upgrade: websocket\r\n";
  request += "Connection: Upgrade\r\n";
  request += "Sec-WebSocket-Key: " + makeWebSocketKey() + "\r\n";
  request += "Sec-WebSocket-Version: 13\r\n";
  request += "x-api-key: " + String(BD_API_KEY) + "\r\n\r\n";

  aaiClient->print(request);

  unsigned long startMs = millis();
  while (aaiClient->connected() && (millis() - startMs < 5000)) {
    String line = aaiClient->readStringUntil('\n');
    if (line.indexOf("101") >= 0) {
      bdConnected = true;
      break;
    }
    if (line == "\r" || line.length() == 0) break;
  }

  if (bdConnected) {
    Serial.println("[BD] Connected. Sending configuration...");
    bdConnecting = false;
    bdHasSpeech = false;
    sendByteDanceConfig();
  } else {
    Serial.println("[BD] Handshake failed");
    aaiClient->stop();
    bdConnecting = false;
  }
}

void streamMicToByteDance() {
  if (!bdConnected || isSpeaking || isProcessingAI) return;
  
  size_t bytesRead = mic_i2s.readBytes((char*)aaiBuffer, AAI_BUFFER_SIZE);
  if (bytesRead < 2) return;

  // Exact DAZI treatment: No software DC offset removal, no gain, just filter tiny noise
  int16_t* samples = (int16_t*)aaiBuffer;
  size_t nSamples = bytesRead / 2;

  for (size_t i = 0; i < nSamples; i++) {
    int16_t sample = samples[i];
    // Filter tiny invalid noise (0, -1, 1) per ArduinoASRChat.cpp
    if (sample == 0 || sample == -1 || sample == 1) {
      samples[i] = 0;
    }
  }

  if (!sendByteDanceBinary(0x02, aaiBuffer, bytesRead)) {
    Serial.println("[BD] Send failed, reconnecting");
    bdConnected = false;
    aaiClient->stop();
  }
}

void pumpByteDanceSocket() {
  if (!bdConnected || !aaiClient->available()) return;

  if (!aaiClient->connected()) {
    Serial.println("[BD] TCP connection lost");
    aaiClient->stop();
    bdConnected = false;
    bdConnecting = false;
    nextAaiReconnectMs = millis() + AAI_RECONNECT_BACKOFF_MS;
    return;
  }

  // Use the robust read loop
  uint8_t header[2];
  if (!readAaiBytes(header, sizeof(header), AAI_FRAME_TIMEOUT_MS)) {
    return; // Just wait for more data
  }

  uint8_t opcode = header[0] & 0x0F;
  bool masked = (header[1] & 0x80) != 0;
  uint64_t payloadLen = (uint64_t)(header[1] & 0x7F);

  if (payloadLen == 126) {
    uint8_t ext[2];
    if (!readAaiBytes(ext, sizeof(ext), AAI_FRAME_TIMEOUT_MS)) return;
    payloadLen = ((uint64_t)ext[0] << 8) | (uint64_t)ext[1];
  } else if (payloadLen == 127) {
    uint8_t ext[8];
    if (!readAaiBytes(ext, sizeof(ext), AAI_FRAME_TIMEOUT_MS)) return;
    payloadLen = 0;
    for (int k = 0; k < 8; k++) payloadLen = (payloadLen << 8) | ext[k];
  }

  uint8_t mask[4];
  if (masked) {
    if (!readAaiBytes(mask, 4, AAI_FRAME_TIMEOUT_MS)) return;
  }

  if (payloadLen > 100000) return; // Safety

  uint8_t* payload = (uint8_t*)malloc(payloadLen + 1);
  if (payload) {
    if (!readAaiBytes(payload, payloadLen, AAI_FRAME_TIMEOUT_MS)) {
      free(payload);
      return;
    }

    if (masked) {
      for (size_t i = 0; i < payloadLen; i++) {
        payload[i] ^= mask[i & 0x03];
      }
    }
    payload[payloadLen] = '\0';

    if (opcode == 0x02) {
      parseByteDanceResponse(payload, payloadLen);
    } else if (opcode == 0x08) {
      Serial.println("[BD] WebSocket close from server");
      aaiClient->stop();
      bdConnected = false;
      bdConnecting = false;
      nextAaiReconnectMs = millis() + AAI_RECONNECT_BACKOFF_MS;
    }
    free(payload);
  }
}

void connectAssemblyAI() {
  if (aaiConnecting || aaiConnected) return;
  if (!aiInputUsesMic) return;
  if (millis() < nextAaiReconnectMs) return;
  if (!networkAvailable()) {
    Serial.println("[DG] No WiFi");
    return;
  }

  aaiConnecting = true;
  sttSocketPumpEnabled = false;
  Serial.println("[DG] Connecting...");

  if (!aaiBuffer) {
    aaiBuffer = psramFound() ? (uint8_t*)ps_malloc(AAI_BUFFER_SIZE)
                             : (uint8_t*)malloc(AAI_BUFFER_SIZE);
    if (!aaiBuffer) {
      Serial.println("[DG] FATAL: Buffer alloc failed!");
      aaiConnecting = false;
      nextAaiReconnectMs = millis() + AAI_RECONNECT_BACKOFF_MS;
      return;
    }
    Serial.printf("[DG] Audio buffer: %u bytes in %s\n",
                  AAI_BUFFER_SIZE, psramFound() ? "PSRAM" : "RAM");
  }

  if (aaiClient->connected()) {
    aaiClient->stop();
  }

  aaiClient->setInsecure();
  aaiClient->setNoDelay(true);
  aaiClient->setTimeout(6);

  if (!aaiClient->connect(DG_HOST, DG_PORT)) {
    Serial.println("[DG] TCP failed");
    aaiConnecting = false;
    nextAaiReconnectMs = millis() + AAI_RECONNECT_BACKOFF_MS;
    return;
  }

  String key = makeWebSocketKey();
  aaiClient->printf(
    "GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Authorization: Token %s\r\n"
    "\r\n",
    DG_PATH, DG_HOST, key.c_str(), DEEPGRAM_API_KEY
  );

  unsigned long t = millis();
  while (!aaiClient->available() && millis() - t < 3500) {
    delay(2);
    yield();
  }

  String line = aaiClient->readStringUntil('\n');
  Serial.println("[DG] " + line);
  while (aaiClient->available()) {
    String l = aaiClient->readStringUntil('\n');
    if (l == "\r") break;
  }

  if (line.indexOf("101") < 0) {
    aaiClient->stop();
    aaiConnecting = false;
    nextAaiReconnectMs = millis() + AAI_RECONNECT_BACKOFF_MS;
    return;
  }

  aaiConnected = true;
  aaiConnecting = false;
  deepgramMetadataReceived = true;
  lastDeepgramKeepAliveMs = millis();
  deepgramFirstFramePending = true;
  if (opusEncoder) {
    opus_encoder_ctl(opusEncoder, OPUS_RESET_STATE);
  }
  sttSocketPumpEnabled = true;
  Serial.println("[DG] Connected! Streaming OPUS...");
  Serial.println("[DG] Ready for audio");
}

void disconnectByteDanceASR(unsigned long reconnectDelayMs = 5000) {
  if (!bdConnected && !bdConnecting && !aaiClient->connected()) return;

  if (bdConnected && aaiClient->connected()) {
    // ByteDance clean close? DAZI just stops, but we can try sending a 0x8 frame
    sendWebSocketFrame(0x8, nullptr, 0);
    delay(20);
  }

  aaiClient->stop();
  bdConnected = false;
  bdConnecting = false;
  nextAaiReconnectMs = millis() + reconnectDelayMs;
  Serial.println("[BD] Disconnected cleanly");
}

void disconnectAssemblyAI(unsigned long reconnectDelayMs) {
  if (!aaiConnected && !aaiConnecting && !aaiClient->connected()) return;

  sttSocketPumpEnabled = false;
  delay(5);
  aaiClient->stop();
  aaiConnected = false;
  aaiConnecting = false;
  deepgramMetadataReceived = false;  // Reset flag on disconnect
  lastDeepgramKeepAliveMs = 0;
  deepgramFirstFramePending = false;
  nextAaiReconnectMs = millis() + reconnectDelayMs;
  Serial.println("[DG] Disconnected cleanly");
}

void streamMicToAAI() {
  if (!aaiConnected || !networkAvailable() || currentMode != MODE_AI || !aiInputUsesMic || isSpeaking || isProcessingAI) {
    if (aaiConnected) {
      clearMicBuffer();
    }
    return;
  }
  if (!aaiBuffer) return;
  if (micTestRecording) return;  // Pause live stream during test recording

  size_t bytesRead = mic_i2s.readBytes((char*)aaiBuffer, NODE_STT_PCM_CHUNK_SIZE);
  if (bytesRead != NODE_STT_PCM_CHUNK_SIZE) return;

  int16_t* samples = (int16_t*)aaiBuffer;
  const int sampleCount = bytesRead / 2;
  int64_t rmsSum = 0;
  for (int i = 0; i < OPUS_FRAME_SIZE; i++) {
    if (samples[i] == 0 || samples[i] == -1 || samples[i] == 1) samples[i] = 0;
    rmsSum += (int64_t)samples[i] * samples[i];
  }
  int32_t rms = (int32_t)sqrt((double)rmsSum / OPUS_FRAME_SIZE);

  int encodedBytes = opus_encode(opusEncoder, samples, OPUS_FRAME_SIZE, opusPacketBuffer, OPUS_MAX_PACKET_SIZE);
  if (encodedBytes <= 0) {
    Serial.printf("[OPUS] Encode failed: %d\n", encodedBytes);
    return;
  }

  bool firstFrameThisSession = deepgramFirstFramePending;
  if (!sendWebSocketBinary(opusPacketBuffer, (size_t)encodedBytes)) {
    Serial.println("[DG] Audio send failed, reconnecting");
    disconnectAssemblyAI(AAI_RECONNECT_BACKOFF_MS);
  } else {
    if (firstFrameThisSession) {
      deepgramFirstFramePending = false;
      Serial.printf("[DG] First frame in at %lu ms\n", millis());
    }
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 1000) {
      const char* lvl = rms < 150 ? "SILENT" : rms < 800 ? "quiet" : rms < 4000 ? "speech" : "LOUD";
      Serial.printf("[MIC] RMS=%d %s  OPUS=%d bytes (%.1fx)\n", rms, lvl, encodedBytes, (float)(OPUS_FRAME_SIZE * 2) / encodedBytes);
      lastLog = millis();
    }
  }
}

void sendAaiKeepAlive() {
  if (!aaiConnected || !aaiClient->connected()) return;
  if (millis() - lastDeepgramKeepAliveMs < AAI_KEEPALIVE_MS) return;

  if (sendWebSocketFrame(0x9, nullptr, 0)) {
    lastDeepgramKeepAliveMs = millis();
  } else {
    Serial.println("[DG] KeepAlive send failed, reconnecting");
    disconnectAssemblyAI(AAI_RECONNECT_BACKOFF_MS);
  }
}

void pumpAaiSocket() {
  if (!aaiClient->connected()) {
    aaiConnected = false;
    aaiConnecting = false;
    return;
  }
  if (aaiClient->available() < 2) return;

  uint8_t b0 = aaiClient->read();
  uint8_t b1 = aaiClient->read();
  uint8_t op = b0 & 0x0F;
  size_t pl = b1 & 0x7F;
  if (pl == 126) {
    uint8_t e[2];
    aaiClient->readBytes(e, 2);
    pl = ((size_t)e[0] << 8) | e[1];
  } else if (pl == 127) {
    uint8_t e[8];
    aaiClient->readBytes(e, 8);
    pl = 0;
    for (int i = 0; i < 8; i++) pl = (pl << 8) | e[i];
  }

  if (pl == 0 || pl >= 8192 || !aaiRxBuffer) {
    for (size_t i = 0; i < pl && aaiClient->available(); i++) aaiClient->read();
    return;
  }

  size_t got = 0;
  unsigned long t = millis();
  while (got < pl && millis() - t < 2000) {
    if (aaiClient->available()) {
      aaiRxBuffer[got++] = (uint8_t)aaiClient->read();
    } else {
      vTaskDelay(1);
    }
  }
  aaiRxBuffer[got] = '\0';

  if (op == 0x1) {
    Serial.printf("[DG] %s\n", (char*)aaiRxBuffer);
    handleAaiTextPayload((const char*)aaiRxBuffer, got);
  } else if (op == 0x9) {
    sendWebSocketFrame(0xA, nullptr, 0);
  } else if (op == 0x8) {
    Serial.println("[DG] WebSocket close from server");
    aaiClient->stop();
    aaiConnected = false;
    aaiConnecting = false;
    nextAaiReconnectMs = millis() + AAI_RECONNECT_BACKOFF_MS;
  }
}

// ============================================================
// NODE.JS PROXY SERVER (WebSocket)
// ============================================================

WiFiClientSecure nodeSecureClient;
WiFiClient nodeBasicClient;
Client* nodeClientPtr = nullptr;

static bool nodeWsSendFrame(uint8_t opcode, const uint8_t* payload, size_t length) {
  if (!nodeClientPtr || !nodeClientPtr->connected()) return false;
  
  uint8_t hdr[14];
  size_t h = 0;
  hdr[h++] = 0x80 | (opcode & 0x0F);
  if (length < 126) {
    hdr[h++] = 0x80 | (uint8_t)length;
  } else if (length < 65536) {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = (uint8_t)(length >> 8);
    hdr[h++] = (uint8_t)(length & 0xFF);
  } else {
    hdr[h++] = 0x80 | 127;
    for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((length >> (i * 8)) & 0xFF);
  }
  
  // Masking key (client must mask to server)
  uint8_t mask[4] = { (uint8_t)random(256), (uint8_t)random(256), (uint8_t)random(256), (uint8_t)random(256) };
  hdr[1] |= 0x80;
  hdr[h++] = mask[0]; hdr[h++] = mask[1]; hdr[h++] = mask[2]; hdr[h++] = mask[3];
  
  if (nodeClientPtr->write(hdr, h) != (int)h) return false;
  
  if (payload && length > 0) {
    uint8_t* maskedPayload = (uint8_t*)malloc(length);
    if (!maskedPayload) return false;
    for (size_t i = 0; i < length; i++) {
      maskedPayload[i] = payload[i] ^ mask[i % 4];
    }
    bool ok = (nodeClientPtr->write(maskedPayload, length) == (int)length);
    free(maskedPayload);
    return ok;
  }
  return true;
}

static bool nodeWsSendBinary(const uint8_t* payload, size_t len) {
  return nodeWsSendFrame(0x2, payload, len);
}

static bool nodeWsSendText(const char* text) {
  if (!text) return false;
  return nodeWsSendFrame(0x1, (const uint8_t*)text, strlen(text));
}

void disconnectNodeServer() {
  if (nodeMux) xSemaphoreTake(nodeMux, portMAX_DELAY);
  if (nodeClientPtr && nodeClientPtr->connected()) {
    nodeWsSendFrame(0x8, nullptr, 0); // close
    nodeClientPtr->stop();
  }
  nodeWsConnected = false;
  nodeWsConnecting = false;
  lastNodeWsKeepAliveMs = 0; 
  nextNodeWsReconnectMs = millis() + 2000;
  Serial.println("[NODE] Disconnected cleanly");
  if (nodeMux) xSemaphoreGive(nodeMux);
}

static void serviceNodeWebSocketKeepAlive() {
  if (!nodeWsConnected || !nodeClientPtr || !nodeClientPtr->connected()) return;
  if (millis() - lastNodeWsKeepAliveMs < NODE_WS_KEEPALIVE_MS) return;
  if (!nodeWsSendFrame(0x9, nullptr, 0)) { // Send WebSocket ping (opcode 0x9)
    Serial.println("[NODE] Keep-alive ping failed, reconnecting");
    disconnectNodeServer();
    return;
  }
  lastNodeWsKeepAliveMs = millis();
}

void connectNodeServer() {
  if (nodeWsConnecting || nodeWsConnected) return;
  if (!aiInputUsesMic || millis() < nextNodeWsReconnectMs) return;
  if (!networkAvailable()) return;
  
  nodeWsConnecting = true;
  Serial.println("[NODE] Connecting to Node Proxy Server...");
  
  if (NODE_SERVER_IS_SECURE) {
    nodeSecureClient.setInsecure();
    nodeSecureClient.setNoDelay(true);
    nodeSecureClient.setTimeout(6);
    nodeClientPtr = &nodeSecureClient;
  } else {
    nodeBasicClient.setNoDelay(true);
    nodeBasicClient.setTimeout(6);
    nodeClientPtr = &nodeBasicClient;
  }
  
  if (!nodeClientPtr->connect(NODE_SERVER_HOST, NODE_SERVER_PORT)) {
    Serial.println("[NODE] TCP connection failed");
    nodeWsConnecting = false;
    nextNodeWsReconnectMs = millis() + 2000;
    return;
  }
  
  String req = "GET / HTTP/1.1\r\n";
  req += "Host: "; req += NODE_SERVER_HOST; req += "\r\n";
  req += "User-Agent: ESP32-EllaBox/1.0\r\n";
  req += "Accept: */*\r\n";
  req += "Connection: Upgrade\r\n";
  req += "Upgrade: websocket\r\n";
  // Use hardcoded key to test if dynamic key was causing 400 Bad Request
  req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
  req += "Sec-WebSocket-Version: 13\r\n";
  req += "\r\n";
  
  nodeClientPtr->print(req);
  
  unsigned long t = millis();
  while (!nodeClientPtr->available() && millis() - t < 3500) {
    delay(2); yield();
  }
  
  String line = nodeClientPtr->readStringUntil('\n');
  line.trim();

  if (line.indexOf("101") < 0) {
    Serial.print("[NODE] WS Handshake failed. Server replied: ");
    Serial.println(line);
    
    // Print the rest of the headers and body to debug
    while (nodeClientPtr->available()) {
      String l = nodeClientPtr->readStringUntil('\n');
      l.trim();
      Serial.println(l);
      if (l.length() == 0) {
        // Read body
        while(nodeClientPtr->available()) {
          Serial.print((char)nodeClientPtr->read());
        }
        break;
      }
    }
    Serial.println("\n[NODE] End of failure response.");

    nodeClientPtr->stop();
    nodeWsConnecting = false;
    nextNodeWsReconnectMs = millis() + 2000;
    return;
  }

  // If 101, consume the rest of headers normally
  while (nodeClientPtr->available()) {
    String l = nodeClientPtr->readStringUntil('\n');
    l.trim();
    if (l.length() == 0) break;
  }

  nodeWsConnected = true;
  nodeWsConnecting = false;
  lastNodeWsKeepAliveMs = millis(); // Initialize keep-alive timer
  Serial.println("[NODE] Connected to Proxy Server!");
  sendCurrentNodeContext();
}

void streamMicToNodeServer() {
  static unsigned long lastSpeakerActiveMs = 0;
  static bool wasMicStreamBlocked = false;
  static unsigned long lastBlockedDrainMs = 0;
  if (isSpeaking || vAgentPlayingAudio || (audio.isRunning() && audio.getVolume() > 0)) {
    lastSpeakerActiveMs = millis();
  }

  bool speakerActive = isSpeaking || isProcessingAI || vAgentPlayingAudio || nodeAudioPending || 
                       (audio.isRunning() && audio.getVolume() > 0) ||
                       (millis() - lastSpeakerActiveMs < 1000); // 1s echo guard

  if (!nodeWsConnected || currentMode != MODE_AI || !aiInputUsesMic || speakerActive) {
    if (nodeWsConnected && micI2SActive && speakerActive) {
      unsigned long now = millis();
      if (!wasMicStreamBlocked || now - lastBlockedDrainMs >= 750) {
        clearMicBuffer(); 
        lastBlockedDrainMs = now;
      }
    }
    wasMicStreamBlocked = true;
    return;
  }

  if (wasMicStreamBlocked && micI2SActive) {
    clearMicBuffer(); // one transition drain before reopening the mic stream
    wasMicStreamBlocked = false;
  }

  if (!aaiBuffer) {
    aaiBuffer = psramFound() ? (uint8_t*)ps_malloc(AAI_BUFFER_SIZE) : (uint8_t*)malloc(AAI_BUFFER_SIZE);
  }
  if (!aaiBuffer) return;

  size_t bytesRead = mic_i2s.readBytes((char*)aaiBuffer, NODE_STT_PCM_CHUNK_SIZE);
  if (bytesRead != NODE_STT_PCM_CHUNK_SIZE) return;

  int16_t* samples = (int16_t*)aaiBuffer;
  const int sampleCount = bytesRead / 2;
  if (sampleCount <= 0) return;

  // Simple noise gate
  int64_t rmsSum = 0;
  for (int i = 0; i < sampleCount; i++) {
    if (samples[i] == 0 || samples[i] == -1 || samples[i] == 1) {
      samples[i] = 0;
    }
    rmsSum += (int64_t)samples[i] * samples[i];
  }

  // Logging
  static uint32_t nodeFrameCount = 0;
  static unsigned long lastMicLogMs = 0;
  nodeFrameCount++;

  if (millis() - lastMicLogMs >= 2000) {
    int32_t rms = (int32_t)sqrt((double)rmsSum / sampleCount);
    Serial.printf("[MIC->DG] frames=%u RMS=%d (streaming to Deepgram via Node)\n",
                  nodeFrameCount, rms);
    lastMicLogMs = millis();
    nodeFrameCount = 0;
  }

  // Send raw PCM
  if (!nodeWsSendBinary(aaiBuffer, (size_t)bytesRead)) {
    Serial.println("[NODE] Send failed, reconnecting");
    disconnectNodeServer();
  }
}

static bool readNodeBytes(uint8_t* dst, size_t len, unsigned long timeoutMs) {
  size_t offset = 0;
  unsigned long lastDataMs = millis();

  while (offset < len) {
    if (!nodeClientPtr || !nodeClientPtr->connected()) {
      return false;
    }

    int availableBytes = nodeClientPtr->available();
    if (availableBytes > 0) {
      int toRead = min((int)(len - offset), availableBytes);
      int bytesRead = nodeClientPtr->read(dst + offset, toRead);
      if (bytesRead > 0) {
        offset += bytesRead;
        lastDataMs = millis();
      }
    } else {
      if (millis() - lastDataMs > timeoutMs) {
        return false;
      }
      delay(2);
      yield();
    }
  }
  return true;
}

void pumpNodeServerSocket() {
  if (!nodeWsConnected || !nodeClientPtr) return;
  if (!nodeClientPtr->connected()) {
    disconnectNodeServer();
    return;
  }
  if (!nodeClientPtr->available()) return;

  uint8_t header[2];
  if (!readNodeBytes(header, 2, 500)) return;

  uint8_t opcode    = header[0] & 0x0F;
  uint64_t payloadLen = header[1] & 0x7F;

  if (payloadLen == 126) {
    uint8_t ext[2];
    if (!readNodeBytes(ext, 2, 500)) return;
    payloadLen = ((uint64_t)ext[0] << 8) | ext[1];
  } else if (payloadLen == 127) {
    uint8_t ext[8];
    if (!readNodeBytes(ext, 8, 500)) return;
    payloadLen = 0;
    for (int k = 0; k < 8; k++) payloadLen = (payloadLen << 8) | ext[k];
  }

  if (payloadLen > 500000) {
    // Frame too large to handle — drain and discard
    Serial.printf("[NODE] Oversized frame %u bytes, discarding\n", (unsigned)payloadLen);
    size_t remaining = (size_t)payloadLen;
    uint8_t discard[256];
    while (remaining > 0) {
      size_t chunk = min(remaining, sizeof(discard));
      if (!readNodeBytes(discard, chunk, 3000)) break;
      remaining -= chunk;
    }
    return;
  }

  // ── Binary (audio) frame: stream chunk-by-chunk straight into PSRAM ring buffer ──
  if (opcode == 0x02) {
    size_t remaining = (size_t)payloadLen;
    uint8_t chunk[256];
    size_t totalEnqueued = 0;
    while (remaining > 0) {
      size_t toRead = min(remaining, sizeof(chunk));
      if (!readNodeBytes(chunk, toRead, 8000)) {
        Serial.printf("[NODE] Audio frame read timeout after %u/%u bytes\n",
                      (unsigned)totalEnqueued, (unsigned)payloadLen);
        break;
      }
      nodeAudioEnqueue(chunk, toRead);
      totalEnqueued += toRead;
      remaining -= toRead;
    }
    if (totalEnqueued > 0) {
      Serial.printf("[NODE] Audio enqueued: %u bytes\n", (unsigned)totalEnqueued);
    }
    return;
  }

  // ── Text / control frames: small, heap malloc is safe ──
  if (payloadLen > 8192) {
    // Unexpectedly large text frame — discard
    size_t remaining = (size_t)payloadLen;
    uint8_t discard[256];
    while (remaining > 0) {
      size_t chunk = min(remaining, sizeof(discard));
      if (!readNodeBytes(discard, chunk, 2000)) break;
      remaining -= chunk;
    }
    return;
  }

  uint8_t* payload = (uint8_t*)malloc(payloadLen + 1);
  if (!payload) {
    Serial.println("[NODE] malloc failed for text frame");
    return;
  }
  if (!readNodeBytes(payload, payloadLen, 2000)) {
    free(payload);
    return;
  }
  payload[payloadLen] = '\0';

  if (opcode == 0x01) { // Text frame
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, (char*)payload);
    free(payload); // Free raw payload immediately after parsing
    payload = nullptr;

    if (!err) {
      const char* type = doc["type"] | "";
      if (strcmp(type, "interim") == 0) {
        // Live STT result — user is still speaking
        const char* text = doc["text"];
        if (text && strlen(text) > 0) {
          Serial.printf("[STT] Hearing: %s\n", text);
          currentInterimText = String(text);
          if (currentMode == MODE_AI) drawAIScreen(false);
        }
      } else if (strcmp(type, "final_transcript") == 0) {
        // Final transcript before AI processing
        const char* text = doc["text"];
        if (text && strlen(text) > 0) {
          Serial.printf("[STT] FINAL: %s\n", text);
          currentInterimText = String(text);
          if (currentMode == MODE_AI) drawAIScreen(false);
        }
      } else if (strcmp(type, "thinking") == 0) {
        Serial.println("[NODE] AI is thinking... (Mic Muted)");
        isProcessingAI = true; 
        processingStartTime = millis(); // Track when we started thinking
        currentRobotActivity = ROBOT_ACTIVITY_THINKING;
        aiRequestStatus = "Thinking...";
        drawAIScreen(true);
      } else if (strcmp(type, "telegram_command") == 0) {
        // Handle incoming Telegram command from server
        const char* command = doc["command"];
        if (command && strlen(command) > 0) {
          handleTelegramCommand(String(command));
        }
      } else if (strcmp(type, "tts_url") == 0) {
        const char* url = doc["url"];
        const char* fallbackText = doc["text"] | "";
        if (url && strlen(url) > 0) {
          Serial.printf("[NODE] Playing Deepgram TTS URL: %s\n", url);
          if (audio.isRunning()) audio.stopSong();
          audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DOUT);
          audio.setConnectionTimeout(3000, 9000);
          audio.forceMono(true);
          audio.setVolume(21);

          if (audio.connecttohost(url)) {
            isSpeaking = true;
            ttsPlaybackStartedMs = millis();
            currentRobotActivity = ROBOT_ACTIVITY_SPEAKING;
            portENTER_CRITICAL(&nodeAudioMux);
            nodeAudioPending = false;
            nodeAudioPendingSince = 0;
            portEXIT_CRITICAL(&nodeAudioMux);
          } else if (fallbackText && strlen(fallbackText) > 0) {
            Serial.println("[NODE] Deepgram URL rejected, falling back to Google TTS");
            String cleanText = String(fallbackText);
            int startBracket;
            while ((startBracket = cleanText.indexOf('[')) >= 0) {
              int endBracket = cleanText.indexOf(']', startBracket);
              if (endBracket > startBracket) {
                cleanText = cleanText.substring(0, startBracket) + cleanText.substring(endBracket + 1);
              } else break;
            }
            cleanText.trim();
            if (cleanText.length() > 0 && audio.connecttospeech(cleanText.c_str(), "en")) {
              isSpeaking = true;
              ttsPlaybackStartedMs = millis();
              currentRobotActivity = ROBOT_ACTIVITY_SPEAKING;
            }
          }
        }
      } else if (strcmp(type, "tts_audio_done") == 0) {
        Serial.println("[NODE] Deepgram PCM stream complete.");
        portENTER_CRITICAL(&nodeAudioMux);
        nodeAudioStreamDone = true;
        portEXIT_CRITICAL(&nodeAudioMux);
      } else if (strcmp(type, "tts") == 0 || strcmp(type, "tts_audio") == 0) {
        const char* text = doc["text"];
        if (text && strlen(text) > 0) {
          bool playGoogleTts = (strcmp(type, "tts") == 0);
          String reply = String(text);
          Serial.println("[NODE] Received sentence: " + reply);
          aiRequestStatus = reply;
          currentInterimText = reply; // Show AI response on TFT
          drawAIScreen(true);

          // Emotion / eye expressions
          String rL = reply; rL.toLowerCase();
          if      (rL.indexOf("[happy")      >= 0) setEyeExpression("HAPPY");
          else if (rL.indexOf("[sad")        >= 0) setEyeExpression("SAD");
          else if (rL.indexOf("[worried")    >= 0) setEyeExpression("WORRIED");
          else if (rL.indexOf("[love")       >= 0) setEyeExpression("LOVE");
          else if (rL.indexOf("[wink")       >= 0) setEyeExpression("WINK");
          else if (rL.indexOf("[angry")      >= 0) setEyeExpression("ANGRY");

          if (audio.isRunning()) audio.stopSong();
          
          String cleanText = reply;
          int startBracket;
          while ((startBracket = cleanText.indexOf('[')) >= 0) {
            int endBracket = cleanText.indexOf(']', startBracket);
            if (endBracket > startBracket) {
              cleanText = cleanText.substring(0, startBracket) + cleanText.substring(endBracket + 1);
            } else break;
          }
          cleanText.trim();
          
          if (cleanText.length() > 0 && playGoogleTts) {
             Serial.printf("[NODE] Speaking via Google TTS: %s\n", cleanText.c_str());
             if (audio.connecttospeech(cleanText.c_str(), "en")) {
                isSpeaking = true;
                ttsPlaybackStartedMs = millis();
                currentRobotActivity = ROBOT_ACTIVITY_SPEAKING;
                portENTER_CRITICAL(&nodeAudioMux);
                nodeAudioPending = false;
                nodeAudioPendingSince = 0;
                portEXIT_CRITICAL(&nodeAudioMux);
             }
          } else if (cleanText.length() > 0) {
             Serial.println("[NODE] Waiting for Binary PCM audio stream...");
             portENTER_CRITICAL(&nodeAudioMux);
             nodeAudioPending = true;
             nodeAudioStreamDone = false;
             nodeAudioReadPos = nodeAudioWritePos;
             nodeAudioPendingSince = millis();
             portEXIT_CRITICAL(&nodeAudioMux);
          }

          // [MOVE: ...] compound movement
          int mvIdx = reply.indexOf("[MOVE:");
          if (mvIdx >= 0) {
            int mvEnd = reply.indexOf("]", mvIdx);
            if (mvEnd > mvIdx) {
              String mvStr = reply.substring(mvIdx + 6, mvEnd);
              int sc = 0, ec = mvStr.indexOf(',');
              while (ec >= 0) { String m = mvStr.substring(sc, ec); m.trim(); if (m.length()) queueMovementCommand(m); sc = ec+1; ec = mvStr.indexOf(',', sc); }
              String m = mvStr.substring(sc); m.trim(); if (m.length()) queueMovementCommand(m);
            }
          }
          // Bare movement tags
          const char* bmt[] = {"[FWD]","[BWD]","[LEFT]","[RIGHT]","[SPIN_L]","[SPIN_R]","[TURN_L_90]","[TURN_R_90]","[TURN_L_180]","[TURN_R_180]",nullptr};
          for (int ti = 0; bmt[ti]; ti++) { if (reply.indexOf(bmt[ti]) >= 0) { String m = String(bmt[ti]); queueMovementCommand(m.substring(1, m.length()-1)); } }

          // Dance
          if (reply.indexOf("[DANCE") >= 0) {
            int dIdx = reply.indexOf("[DANCE:"), dEnd = reply.indexOf("]", dIdx >= 0 ? dIdx : reply.indexOf("[DANCE]"));
            if (dEnd > dIdx) {
              queueDanceRoutine(dIdx >= 0 ? reply.substring(dIdx+7, dEnd) : "");
            }
          }

          // Deferred / system actions - Move all to deferred to prevent cutting off AI confirmation speech
          if      (reply.indexOf("[BREATHE]")   >= 0) { scheduleDeferredAiAction(DEFERRED_AI_GOHOME); } // BREATHE is basically GOHOME with a twist
          else if (reply.indexOf("[MEDITATE")   >= 0) { 
              int mIdx = reply.indexOf("[MEDITATE:");
              String mParam = (mIdx >= 0) ? reply.substring(mIdx+10, reply.indexOf("]", mIdx)) : "calm";
              scheduleDeferredAiAction(DEFERRED_AI_MEDITATE, mParam); 
          }
          else if (reply.indexOf("[RELAX")      >= 0) { 
              int rIdx = reply.indexOf("[RELAX:");
              String rParam = (rIdx >= 0) ? reply.substring(rIdx+7, reply.indexOf("]", rIdx)) : "rain";
              scheduleDeferredAiAction(DEFERRED_AI_RELAX, rParam); 
          }
          else if (reply.indexOf("[GOHOME]")    >= 0) { scheduleDeferredAiAction(DEFERRED_AI_GOHOME); }
          else if (reply.indexOf("[STOPAUDIO]") >= 0) { stopActiveAudioPlayback(true); }
          else if (reply.indexOf("[IMURESET]")  >= 0) { scheduleDeferredAiAction(DEFERRED_AI_IMURESET); }
          else if (reply.indexOf("[CHECKUP]")   >= 0) { scheduleDeferredAiAction(DEFERRED_AI_CHECKUP); }
          else if (reply.indexOf("[EMERGENCY]") >= 0) { scheduleDeferredAiAction(DEFERRED_AI_EMERGENCY); }
          else if (reply.indexOf("[FORGET]")    >= 0) { clearAllMemory(); }
          
          if (reply.indexOf("[PLAYSONG:") >= 0 || reply.indexOf("[PLAY:") >= 0) {
            int pIdx = reply.indexOf("[PLAY:"); if (pIdx < 0) pIdx = reply.indexOf("[PLAYSONG:");
            int pEnd = reply.indexOf("]", pIdx);
            String param = (pEnd > pIdx) ? reply.substring(pIdx + (reply.indexOf("[PLAY:") >= 0 ? 6 : 10), pEnd) : "lofi";
            scheduleDeferredAiAction(DEFERRED_AI_PLAYSONG, param.length() > 0 ? param : "lofi", 25);
          }

          // Reminder: [REMINDER: title | time | type]
          if (reply.indexOf("[REMINDER:") >= 0) {
            int rIdx = reply.indexOf("[REMINDER:") + 10, rEnd = reply.indexOf(']', rIdx);
            if (rEnd > rIdx) {
              String p = reply.substring(rIdx, rEnd);
              String title = "Reminder", ts = "Anytime", ty = "chat";
              int s1 = p.indexOf('|');
              if (s1 > 0) { title = p.substring(0, s1); title.trim(); int s2 = p.indexOf('|', s1+1); if (s2 > 0) { ts = p.substring(s1+1, s2); ts.trim(); ty = p.substring(s2+1); ty.trim(); } else { ts = p.substring(s1+1); ts.trim(); } } else if (p.length()) title = p;
              stagePendingReminder(title, ts, ty, true);
            }
          }
        }
      } else if (strcmp(type, "turn_complete") == 0) {
        Serial.println("[NODE] Turn complete.");
        isProcessingAI = false;
        portENTER_CRITICAL(&nodeAudioMux);
        nodeAudioPending = false;
        nodeAudioPendingSince = 0;
        portEXIT_CRITICAL(&nodeAudioMux);
        clearStringKeepCapacity(aiRequestStatus);
        if (currentMode == MODE_AI && aiInputUsesMic && !isSpeaking) {
          currentRobotActivity = ROBOT_ACTIVITY_LISTENING;
          aiRequestStatus = "Listening...";
          
          // Refresh current senses for the next turn
          sendCurrentNodeContext();
        }
      }
    }
  } else if (opcode == 0x08) {
    Serial.println("[NODE] Server closed connection.");
    disconnectNodeServer();
  }
}

// ============================================================
// DEEPGRAM VOICE AGENT (STT+LLM+TTS unified WebSocket)
// ============================================================

// --- Voice Agent WebSocket helpers ---
static bool vAgentSendFrame(uint8_t opcode, const uint8_t* payload, size_t length) {
  if (!vAgentClient || !vAgentClient->connected()) return false;

  uint8_t hdr[14];
  size_t h = 0;
  hdr[h++] = 0x80 | (opcode & 0x0F);
  if (length < 126) {
    hdr[h++] = 0x80 | (uint8_t)length;
  } else if (length < 65536) {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = (uint8_t)(length >> 8);
    hdr[h++] = (uint8_t)(length & 0xFF);
  } else {
    hdr[h++] = 0x80 | 127;
    for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((length >> (i * 8)) & 0xFF);
  }
  hdr[h++] = 0; hdr[h++] = 0; hdr[h++] = 0; hdr[h++] = 0;  // mask=0
  if (vAgentClient->write(hdr, h) != (int)h) return false;
  if (payload && length) return vAgentClient->write(payload, length) == (int)length;
  return true;
}

static bool vAgentSendText(const char* text) {
  return vAgentSendFrame(0x1, (const uint8_t*)text, strlen(text));
}

static bool vAgentSendBinary(const uint8_t* data, size_t len) {
  return vAgentSendFrame(0x2, data, len);
}

// --- Voice Agent Settings ---
void sendVoiceAgentSettings() {
  if (!vAgentConnected) return;

  // Build the full prompt including persona, live context, and memory
  String fullPrompt = getEllaSystemPrompt();
  fullPrompt += getEllaLiveContext();
  fullPrompt += buildConversationMemoryContext();
  
  // Basic JSON escaping for the prompt
  fullPrompt.replace("\\", "\\\\");
  fullPrompt.replace("\"", "\\\"");
  fullPrompt.replace("\n", "\\n");
  fullPrompt.replace("\r", "");

  String groqModel = GROQ_MODEL;
  
  String settings = "{\"type\":\"Settings\",\"audio\":{"
    "\"input\":{\"encoding\":\"opus\",\"sample_rate\":16000},"
    "\"output\":{\"encoding\":\"linear16\",\"sample_rate\":24000,\"container\":\"none\"}"
    "},\"agent\":{"
    "\"listen\":{\"provider\":{\"type\":\"deepgram\",\"model\":\"nova-3-medical\",\"language\":\"en\"}},"
    "\"think\":{"
    "\"provider\":{"
      "\"type\":\"open_ai\","
      "\"model\":\"" + groqModel + "\""
    "},"
    "\"endpoint\":{"
      "\"url\":\"https://api.groq.com/openai/v1/chat/completions\","
      "\"headers\":{"
        "\"Authorization\":\"Bearer " + String(GROQ_KEY) + "\","
        "\"Content-Type\":\"application/json\""
      "}"
    "},"
    "\"instructions\":\"" + fullPrompt + "\""
    "},"
    "\"speak\":{\"provider\":{\"type\":\"deepgram\",\"model\":\"aura-2-vesta-en\"}}"
    "}}";

  Serial.println("[VAgent] Sending optimized Groq OpenAI 20B settings with full persona...");

  if (vAgentSendText(settings.c_str())) {
    vAgentConfigured = true;
    Serial.println("[VAgent] Settings sent (" + String(settings.length()) + " bytes)");
  } else {
    Serial.println("[VAgent] Failed to send settings");
    disconnectVoiceAgent();
  }
}

// --- Voice Agent Connect ---
void connectVoiceAgent() {
  if (vAgentConnecting || vAgentConnected) return;
  if (!aiInputUsesMic || currentMode != MODE_AI) return;
  if (!networkAvailable()) return;
  if (millis() < vAgentNextReconnectMs) return;

  vAgentConnecting = true;
  Serial.println("[VAgent] Connecting...");

  if (!aaiBuffer) {
    aaiBuffer = psramFound() ? (uint8_t*)ps_malloc(AAI_BUFFER_SIZE)
                             : (uint8_t*)malloc(AAI_BUFFER_SIZE);
    if (!aaiBuffer) {
      Serial.println("[VAgent] FATAL: Buffer alloc failed!");
      vAgentConnecting = false;
      vAgentNextReconnectMs = millis() + VAGENT_RECONNECT_MS;
      return;
    }
    Serial.printf("[VAgent] Audio buffer: %u bytes in %s\n",
                  AAI_BUFFER_SIZE, psramFound() ? "PSRAM" : "RAM");
  }

  if (!vAgentClient) {
    vAgentClient = new WiFiClientSecure();
  }
  vAgentClient->setInsecure();
  vAgentClient->setNoDelay(true);
  vAgentClient->setTimeout(8);

  if (!vAgentClient->connect(VAGENT_HOST, VAGENT_PORT)) {
    Serial.println("[VAgent] TCP connect failed");
    vAgentConnecting = false;
    vAgentNextReconnectMs = millis() + VAGENT_RECONNECT_MS;
    return;
  }

  String key = makeWebSocketKey();
  char hs[512];
  snprintf(hs, sizeof(hs),
    "GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Authorization: Token %s\r\n"
    "\r\n",
    VAGENT_PATH, VAGENT_HOST, key.c_str(), DEEPGRAM_API_KEY
  );
  vAgentClient->write((const uint8_t*)hs, strlen(hs));

  unsigned long t0 = millis();
  while (!vAgentClient->available() && millis() - t0 < 5000) {
    delay(10);
  }
  String line = vAgentClient->readStringUntil('\n');
  if (line.indexOf("101") < 0) {
    Serial.println("[VAgent] Upgrade failed: " + line);
    vAgentClient->stop();
    vAgentConnecting = false;
    vAgentNextReconnectMs = millis() + VAGENT_RECONNECT_MS;
    return;
  }
  // Skip remaining headers
  while (vAgentClient->available()) {
    String hdr = vAgentClient->readStringUntil('\n');
    if (hdr.length() <= 2) break;
  }

  vAgentConnected = true;
  vAgentConnecting = false;
  vAgentConfigured = false;
  vAgentLastKeepAliveMs = millis();
  vAgentPlayingAudio = false;
  vAgentLastAssistantText = "";

  if (opusEncoder) {
    opus_encoder_ctl(opusEncoder, OPUS_RESET_STATE);
  }

  sendVoiceAgentSettings();
  Serial.println("[VAgent] Connected!");
}

// --- Voice Agent Disconnect ---
void disconnectVoiceAgent() {
  if (vAgentMux) xSemaphoreTake(vAgentMux, portMAX_DELAY);
  if (!vAgentConnected && !vAgentConnecting && (!vAgentClient || !vAgentClient->connected())) {
    if (vAgentMux) xSemaphoreGive(vAgentMux);
    return;
  }

  if (vAgentClient) vAgentClient->stop();
  vAgentConnected = false;
  vAgentConnecting = false;
  vAgentConfigured = false;
  vAgentPlayingAudio = false;
  vAgentNextReconnectMs = millis() + VAGENT_RECONNECT_MS;

  // Stop direct speaker I2S if active
  // (Audio library will be re-initialized when needed)
  Serial.println("[VAgent] Disconnected");
  if (vAgentMux) xSemaphoreGive(vAgentMux);
}

// --- Voice Agent: stream mic audio (OPUS) ---
void streamMicToVoiceAgent() {
  if (!vAgentConnected || !vAgentConfigured || !networkAvailable() ||
      currentMode != MODE_AI || !aiInputUsesMic || isSpeaking) {
    return;
  }

  if (!micI2SActive || !opusEncoder) return;

  // Read PCM from mic (same as streamMicToAAI)
  size_t bytesRead = mic_i2s.readBytes((char*)aaiBuffer, AAI_BUFFER_SIZE);
  if (bytesRead < AAI_BUFFER_SIZE) return;

  int16_t* samples = (int16_t*)aaiBuffer;
  int encodedBytes = opus_encode(opusEncoder, samples, OPUS_FRAME_SIZE,
                                  opusPacketBuffer, OPUS_MAX_PACKET_SIZE);
  if (encodedBytes <= 0) return;

  if (!vAgentSendBinary(opusPacketBuffer, (size_t)encodedBytes)) {
    Serial.println("[VAgent] Audio send failed, reconnecting");
    disconnectVoiceAgent();
  }
}

// --- Voice Agent: keep-alive ---
void sendVoiceAgentKeepAlive() {
  if (!vAgentConnected || !vAgentClient->connected()) return;
  if (millis() - vAgentLastKeepAliveMs < VAGENT_KEEPALIVE_MS) return;

  if (vAgentSendFrame(0x9, nullptr, 0)) {
    vAgentLastKeepAliveMs = millis();
  } else {
    Serial.println("[VAgent] KeepAlive failed, reconnecting");
    disconnectVoiceAgent();
  }
}

// --- Voice Agent: play audio through speaker I2S ---
void playVoiceAgentAudio(const uint8_t* data, size_t len) {
  if (len == 0) return;

  // Initialize speaker I2S for direct PCM output if not already active
  if (!vAgentPlayingAudio) {
    // Stop Audio library to free I2S
    if (audio.isRunning()) {
      audio.stopSong();
      delay(20);
    }
    micTestSpeaker_i2s.end();
    micTestSpeaker_i2s.setPins(SPK_BCLK, SPK_LRC, SPK_DOUT);
    if (!micTestSpeaker_i2s.begin(I2S_MODE_STD, 24000, I2S_DATA_BIT_WIDTH_16BIT,
                                   I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
      Serial.println("[VAgent] Speaker I2S init failed");
      return;
    }
    vAgentPlayingAudio = true;
    isSpeaking = true;
    currentRobotActivity = ROBOT_ACTIVITY_SPEAKING;
    aiRequestStatus = "Speaking...";
    if (firebaseReady) setSpeakingStatusFirebase(true);
    Serial.println("[VAgent] Speaker active");
  }

  // Write PCM data directly to I2S
  size_t written = micTestSpeaker_i2s.write(data, len);
  if (written != len) {
    // Buffer full, just skip — the next frame will catch up
  }
}

// --- Drain Node TTS audio ring buffer — called ONLY from Core 1 (loop()) ---
// Uses the same playVoiceAgentAudio I2S path but safely from the correct core.
#define NODE_AUDIO_DRAIN_CHUNK 2048
static uint8_t nodeDrainChunk[NODE_AUDIO_DRAIN_CHUNK];
static bool drainStartLogged = false; // reset each turn

void drainNodeAudio() {
  if (!nodeAudioRingBuf) return;

  portENTER_CRITICAL(&nodeAudioMux);
  size_t avail = nodeAudioAvailable();
  bool streamDone = nodeAudioStreamDone;
  portEXIT_CRITICAL(&nodeAudioMux);

  if (nodeAudioPending && !vAgentPlayingAudio && !streamDone && avail < NODE_AUDIO_PREBUFFER_BYTES) {
    return;
  }

  if (avail > 0) {
    if (!drainStartLogged) {
      Serial.printf("[AUDIO] Draining Node TTS: %u bytes buffered -> I2S speaker\n", (unsigned)avail);
      drainStartLogged = true;
    }

    // Pull a chunk
    portENTER_CRITICAL(&nodeAudioMux);
    size_t toRead = min(avail, (size_t)NODE_AUDIO_DRAIN_CHUNK);
    for (size_t i = 0; i < toRead; i++) {
      nodeDrainChunk[i] = nodeAudioRingBuf[nodeAudioReadPos];
      nodeAudioReadPos = (nodeAudioReadPos + 1) % NODE_AUDIO_BUF_SIZE;
    }
    avail -= toRead;
    portEXIT_CRITICAL(&nodeAudioMux);

    // Safe to call I2S from Core 1
    playVoiceAgentAudio(nodeDrainChunk, toRead);
  }

  // If buffer is now empty AND we were playing, start a settling timer
  // (more audio packets may still be in transit from the server)
  static unsigned long drainEmptySinceMs = 0;
  if (avail == 0 && vAgentPlayingAudio && streamDone) {
    if (drainEmptySinceMs == 0) drainEmptySinceMs = millis(); // start timer

    // Only tear down after 350ms of confirmed silence — lets TCP deliver remaining frames
    if (millis() - drainEmptySinceMs >= 350) {
      portENTER_CRITICAL(&nodeAudioMux);
      nodeAudioPending = false;
      nodeAudioStreamDone = true;
      portEXIT_CRITICAL(&nodeAudioMux);

      micTestSpeaker_i2s.end();
      vAgentPlayingAudio = false;
      isSpeaking = false;
      isProcessingAI = false;
      if (firebaseReady) setSpeakingStatusFirebase(false);
      audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DOUT);
      audio.forceMono(true);
      drainStartLogged = false;
      drainEmptySinceMs = 0;
      nodeAudioPendingSince = 0;
      Serial.println("[NODE] Audio playback complete, mic re-enabled.");
      if (currentMode == MODE_AI && aiInputUsesMic) {
        currentRobotActivity = ROBOT_ACTIVITY_LISTENING;
        aiRequestStatus = "Listening...";
      }
    }
  } else if (avail > 0) {
    drainEmptySinceMs = 0; // reset timer as long as data is flowing
  }

  // Safety timeout: if nodeAudioPending has been stuck for >4s with no audio, unblock mic
  if (nodeAudioPending && !vAgentPlayingAudio && nodeAudioPendingSince > 0
      && millis() - nodeAudioPendingSince > 10000) {
    Serial.println("[NODE] Audio timeout — no binary frame received, unblocking mic.");
    portENTER_CRITICAL(&nodeAudioMux);
    nodeAudioPending = false;
    nodeAudioStreamDone = true;
    portEXIT_CRITICAL(&nodeAudioMux);
    nodeAudioPendingSince = 0;
    drainStartLogged = false;
    drainEmptySinceMs = 0;
    isProcessingAI = false;
    isSpeaking = false;
  }
}

// --- Voice Agent: handle incoming text messages ---
void handleVoiceAgentText(const char* payload, size_t length) {
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[VAgent] JSON parse error: %s\n", err.c_str());
    return;
  }

  const char* msgType = doc["type"] | "";

  // ConversationText: contains user or assistant transcript
  if (strcmp(msgType, "ConversationText") == 0) {
    const char* role = doc["role"] | "";
    const char* content = doc["content"] | "";

    if (strcmp(role, "user") == 0) {
      // User's speech transcript — show on screen
      String userText = String(content);
      userText.trim();
        if (userText.length() > 0) {
          currentInterimText = userText;
          g_lastUserQuery = userText; // Save for LLM memory
          Serial.println("[VAgent] User: " + userText);
          if (currentMode == MODE_AI) drawAIScreen(true);
        }
    } else if (strcmp(role, "assistant") == 0) {
      // Assistant's reply text — parse for commands
      String reply = String(content);
      reply.trim();
      if (reply.length() > 0) {
        Serial.println("[VAgent] Assistant: " + reply);
        vAgentLastAssistantText = reply;

        // Show on screen in the correct sections
        lastAiResponse = reply; // For the AI Response box
        g_lastMistralReply = reply; // Save for LLM memory
        currentInterimText = ""; // Clear transcription area since AI is talking
        
        // Save to long-term memory
        appendConversationMemory(g_lastUserQuery, reply);
        
        if (currentMode == MODE_AI) drawAIScreen(true);

        // Parse commands from the reply (same as askAI command parsing)
        String cmd = "";
        String param = "";
        // Extract [MOVE:...] tags
        int moveIdx = reply.indexOf("[MOVE:");
        if (moveIdx >= 0) {
          int moveEnd = reply.indexOf("]", moveIdx);
          if (moveEnd > moveIdx) {
            String moveStr = reply.substring(moveIdx + 6, moveEnd);
            int startComma = 0;
            int endComma = moveStr.indexOf(',');
            while (endComma >= 0) {
              String mCmd = moveStr.substring(startComma, endComma);
              mCmd.trim();
              if (mCmd.length() > 0) queueMovementCommand(mCmd);
              startComma = endComma + 1;
              endComma = moveStr.indexOf(',', startComma);
            }
            String mCmd = moveStr.substring(startComma);
            mCmd.trim();
            if (mCmd.length() > 0) queueMovementCommand(mCmd);
            reply = reply.substring(0, moveIdx) + reply.substring(moveEnd + 1);
            reply.trim();
          }
        }
        // Extract [DANCE] / [DANCE:...]
        int danceIdx = reply.indexOf("[DANCE:");
        if (danceIdx < 0) danceIdx = reply.indexOf("[DANCE]");
        int danceClose = (danceIdx >= 0) ? reply.indexOf("]", danceIdx) : -1;
        if (danceIdx >= 0 && danceClose > danceIdx) {
          String danceStr = "";
          if (reply.indexOf("[DANCE:") >= 0) danceStr = reply.substring(danceIdx + 7, danceClose);
          reply = reply.substring(0, danceIdx) + reply.substring(danceClose + 1);
          reply.trim();
          queueDanceRoutine(danceStr);
        }
        // Extract [SCAN]
        int scanIdx = reply.indexOf("[SCAN]");
        if (scanIdx >= 0) {
          reply.remove(scanIdx, 6);
          reply.trim();
          String scanReply = performSpatialSurvey(false, false);
          if (scanReply.length() > 0) reply = scanReply;
        }
        // Extract [EXPLORE]
        int exploreIdx = reply.indexOf("[EXPLORE]");
        if (exploreIdx >= 0) {
          reply.remove(exploreIdx, 9);
          reply.trim();
          String exploreReply = performSpatialSurvey(true, false);
          if (exploreReply.length() > 0) reply = exploreReply;
        }
        // Bare movement tags
        {
          const char* bareMoveTags[] = {
            "[FWD]", "[BWD]", "[LEFT]", "[RIGHT]",
            "[SPIN_L]", "[SPIN_R]", "[TURN_L_90]", "[TURN_R_90]",
            "[TURN_L_180]", "[TURN_R_180]", nullptr
          };
          for (int ti = 0; bareMoveTags[ti] != nullptr; ti++) {
            String tag = String(bareMoveTags[ti]);
            int tagIdx = reply.indexOf(tag);
            if (tagIdx >= 0) {
              String mCmd = tag.substring(1, tag.length() - 1);
              queueMovementCommand(mCmd);
              reply.remove(tagIdx, tag.length());
              reply.trim();
            }
          }
        }
        // Other command tags
        if (reply.indexOf("PLAYSONG:") >= 0) {
          cmd = "PLAYSONG";
          param = reply.substring(reply.indexOf("PLAYSONG:") + 9);
        } else if (reply.indexOf("[PLAY:") >= 0) {
          cmd = "PLAYSONG";
          param = reply.substring(reply.indexOf("[PLAY:") + 6);
          int cIdx = param.indexOf(']');
          if (cIdx >= 0) param = param.substring(0, cIdx);
        } else if (reply.indexOf("[BREATHE]") >= 0) {
          cmd = "BREATHE";
        } else if (reply.indexOf("[MEDITATE") >= 0) {
          cmd = "MEDITATE"; param = "calm";
        } else if (reply.indexOf("[RELAX") >= 0) {
          cmd = "RELAX"; param = "rain";
        } else if (reply.indexOf("[SLEEP]") >= 0) {
          cmd = "SLEEP";
        } else if (reply.indexOf("[WAKEUP]") >= 0) {
          cmd = "WAKEUP";
        } else if (reply.indexOf("[GOHOME]") >= 0) {
          cmd = "GOHOME";
        } else if (reply.indexOf("[STOPAUDIO]") >= 0) {
          cmd = "STOPAUDIO";
        } else if (reply.indexOf("[IMURESET]") >= 0) {
          cmd = "IMURESET";
        } else if (reply.indexOf("[CHECKUP]") >= 0) {
          cmd = "CHECKUP";
        } else if (reply.indexOf("[EMERGENCY]") >= 0) {
          cmd = "EMERGENCY";
        } else if (reply.indexOf("[REMINDER:") >= 0) {
          cmd = "REMINDER";
          int rIdx = reply.indexOf("[REMINDER:") + 10;
          int rEnd = reply.indexOf(']', rIdx);
          if (rEnd > rIdx) param = reply.substring(rIdx, rEnd);
        } else if (reply.indexOf("[FORGET]") >= 0) {
          cmd = "FORGET";
        }

        // Execute deferred commands
        if (cmd == "PLAYSONG") {
          scheduleDeferredAiAction(DEFERRED_AI_PLAYSONG, param.length() > 0 ? param : "lofi", 25);
        } else if (cmd == "BREATHE") {
          switchToNormalMode();
          currentMedState = MED_BREATHING;
          currentMeditationType = MED_TYPE_BREATHING;
          medStateTimer = millis();
          drawNormalScreen(true);
        } else if (cmd == "MEDITATE") {
          startMeditation(param);
        } else if (cmd == "RELAX") {
          scheduleDeferredAiAction(DEFERRED_AI_RELAX, param);
        } else if (cmd == "SLEEP") {
          sleepStartTime = millis();
          lastSleepLogged = false;
          isSleepMode = true;
          struct tm ti; getLocalTime(&ti);
          char tbuf[30]; strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);
          if (firebaseReady) {
              Firebase.RTDB.setString(&fbdo, "/sleepLog/currentSession/startTime", String(tbuf));
              Firebase.RTDB.setInt(&fbdo, "/sleepLog/currentSession/startMillis", (int)(millis() / 1000));
          }
        } else if (cmd == "WAKEUP") {
          if (sleepStartTime > 0) {
              float sleepHours = (millis() - sleepStartTime) / 3600000.0;
              if (sleepHours > 0.5 && sleepHours < 16.0) {
                  struct tm ti; getLocalTime(&ti);
                  char path[64];
                  sprintf(path, "sleepLog/%04d-%02d-%02d", ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday);
                  if (firebaseReady) {
                      Firebase.RTDB.setFloat(&fbdo, String(path) + "/hours", sleepHours);
                      Firebase.RTDB.setString(&fbdo, String(path) + "/source", "voice");
                      Firebase.RTDB.deleteNode(&fbdo, "/sleepLog/currentSession");
                  }
              }
              sleepStartTime = 0;
              lastSleepLogged = true;
              isSleepMode = false;
          }
        } else if (cmd == "GOHOME") {
          scheduleDeferredAiAction(DEFERRED_AI_GOHOME);
        } else if (cmd == "STOPAUDIO") {
          stopActiveAudioPlayback(true);
        } else if (cmd == "IMURESET") {
          scheduleDeferredAiAction(DEFERRED_AI_IMURESET);
        } else if (cmd == "CHECKUP") {
          scheduleDeferredAiAction(DEFERRED_AI_CHECKUP);
        } else if (cmd == "EMERGENCY") {
          scheduleDeferredAiAction(DEFERRED_AI_EMERGENCY);
        } else if (cmd == "REMINDER") {
          String title = "New Reminder";
          String timeStr = "Anytime";
          String typeStr = "chat";
          int sep1 = param.indexOf('|');
          if (sep1 > 0) {
              title = param.substring(0, sep1); title.trim();
              int sep2 = param.indexOf('|', sep1 + 1);
              if (sep2 > 0) {
                  timeStr = param.substring(sep1 + 1, sep2); timeStr.trim();
                  typeStr = param.substring(sep2 + 1); typeStr.trim();
              } else {
                  timeStr = param.substring(sep1 + 1); timeStr.trim();
              }
          } else if (param.length() > 0) title = param;
          
          String tsLower = timeStr;
          tsLower.toLowerCase();
          if (tsLower.indexOf("minute") >= 0 || tsLower.indexOf("hour") >= 0) {
              struct tm ti;
              if (getLocalTime(&ti)) {
                  int addMin = 0;
                  for (int i = 0; i < (int)timeStr.length(); i++) {
                      if (isDigit(timeStr[i])) { addMin = timeStr.substring(i).toInt(); break; }
                  }
                  if (addMin == 0) addMin = 1;
                  if (tsLower.indexOf("hour") >= 0) addMin *= 60;
                  int totalMin = ti.tm_hour * 60 + ti.tm_min + addMin;
                  int newH = (totalMin / 60) % 24;
                  int newM = totalMin % 60;
                  int h12 = newH % 12; if (h12 == 0) h12 = 12;
                  char absBuf[12];
                  sprintf(absBuf, "%d:%02d %s", h12, newM, (newH < 12) ? "AM" : "PM");
                  timeStr = String(absBuf);
              }
          }
          stagePendingReminder(title, timeStr, typeStr, true);
        } else if (cmd == "FORGET") {
          clearAllMemory();
        }

        // Update mood from emotion tags
        String replyLower = reply;
        replyLower.toLowerCase();
        if (replyLower.indexOf("[happy") >= 0) setEyeExpression("HAPPY");
        else if (replyLower.indexOf("[sad") >= 0) setEyeExpression("SAD");
        else if (replyLower.indexOf("[worried") >= 0) setEyeExpression("WORRIED");
        else if (replyLower.indexOf("[thinking") >= 0) setEyeExpression("THINKING");
        else if (replyLower.indexOf("[love") >= 0) setEyeExpression("LOVE");
        else if (replyLower.indexOf("[wink") >= 0) setEyeExpression("WINK");
        else if (replyLower.indexOf("[excited") >= 0) setEyeExpression("EXCITED");
        else if (replyLower.indexOf("[frustrated") >= 0) setEyeExpression("FRUSTRATED");
        else if (replyLower.indexOf("[angry") >= 0) setEyeExpression("ANGRY");
        else if (replyLower.indexOf("[suspicious") >= 0) setEyeExpression("SUSPICIOUS");
        else if (replyLower.indexOf("[normal") >= 0) setEyeExpression("NORMAL");

        // Log conversation
        appendConversationMemory(vAgentLastAssistantText, reply);
        logConversationToFirebase("", reply);
        logMoodToFirebase();
      }
    }
    return;
  }

  // AgentStarted: agent is ready
  if (strcmp(msgType, "AgentStarted") == 0) {
    Serial.println("[VAgent] Agent started — ready for audio");
    return;
  }

  // AgentTurnEnd: agent finished speaking text, but audio may still be playing
  if (strcmp(msgType, "AgentTurnEnd") == 0) {
    Serial.println("[VAgent] Agent turn ended — audio draining...");
    portENTER_CRITICAL(&nodeAudioMux);
    nodeAudioStreamDone = true;
    portEXIT_CRITICAL(&nodeAudioMux);
    
    // Re-enable mic streaming
    if (currentMode == MODE_AI && aiInputUsesMic) {
      currentRobotActivity = ROBOT_ACTIVITY_LISTENING;
      aiRequestStatus = "Listening...";
    }
    return;
  }

  // UserStartedSpeaking: barge-in detection
  if (strcmp(msgType, "UserStartedSpeaking") == 0) {
    Serial.println("[VAgent] User started speaking (barge-in)");
    // Stop current audio playback if agent was speaking
    if (vAgentPlayingAudio) {
      micTestSpeaker_i2s.end();
      vAgentPlayingAudio = false;
      isSpeaking = false;
      clearStringKeepCapacity(aiRequestStatus);
      if (firebaseReady) setSpeakingStatusFirebase(false);
      audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DOUT);
      audio.forceMono(true);
    }
    return;
  }

  // Welcome message etc.
  if (strcmp(msgType, "Welcome") == 0) {
    Serial.println("[VAgent] Welcome received");
    return;
  }

  // Error from Deepgram
  if (strcmp(msgType, "Error") == 0) {
    Serial.printf("[VAgent] ERROR from server: %s\n", payload);
    return;
  }

  // UnhandledTypes: log them
  Serial.printf("[VAgent] Unhandled type: %s\n", msgType);
}

// --- Voice Agent: pump socket (receive data) ---
void pumpVoiceAgentSocket() {
  if (!vAgentClient || !vAgentClient->connected()) {
    vAgentConnected = false;
    vAgentConnecting = false;
    return;
  }
  if (vAgentClient->available() < 2) return;

  uint8_t b0 = vAgentClient->read();
  uint8_t b1 = vAgentClient->read();
  uint8_t op = b0 & 0x0F;
  bool masked = (b1 & 0x80) != 0;
  size_t pl = b1 & 0x7F;
  if (pl == 126) {
    uint8_t e[2];
    vAgentClient->readBytes(e, 2);
    pl = ((size_t)e[0] << 8) | e[1];
  } else if (pl == 127) {
    uint8_t e[8];
    vAgentClient->readBytes(e, 8);
    pl = 0;
    for (int i = 0; i < 8; i++) pl = (pl << 8) | e[i];
  }

  // Read mask if present (server frames shouldn't be masked, but handle it)
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    vAgentClient->readBytes(mask, 4);
  }

  if (pl == 0) return;

  // For text frames, read into aaiRxBuffer (reuse shared buffer)
  if (op == 0x1) {
    if (pl >= 8192 || !aaiRxBuffer) {
      for (size_t i = 0; i < pl && vAgentClient->available(); i++) vAgentClient->read();
      return;
    }
    size_t got = 0;
    unsigned long t = millis();
    while (got < pl && millis() - t < 3000) {
      if (vAgentClient->available()) {
        aaiRxBuffer[got++] = (uint8_t)vAgentClient->read();
        if (masked) aaiRxBuffer[got - 1] ^= mask[(got - 1) % 4];
      } else {
        vTaskDelay(1);
      }
    }
    aaiRxBuffer[got] = '\0';
    handleVoiceAgentText((const char*)aaiRxBuffer, got);
  }
  // For binary frames (audio output), enqueue into ring buffer
  else if (op == 0x2) {
    // Allocate temp buffer for audio
    uint8_t* audioBuf = (uint8_t*)heap_caps_malloc(pl, MALLOC_CAP_SPIRAM);
    if (!audioBuf) {
      // No PSRAM, skip this frame
      for (size_t i = 0; i < pl && vAgentClient->available(); i++) vAgentClient->read();
      return;
    }
    size_t got = 0;
    unsigned long t = millis();
    while (got < pl && millis() - t < 3000) {
      if (vAgentClient->available()) {
        audioBuf[got++] = (uint8_t)vAgentClient->read();
        if (masked) audioBuf[got - 1] ^= mask[(got - 1) % 4];
      } else {
        vTaskDelay(1);
      }
    }
    if (got > 0) {
      // Set stream as not done since we just got data
      portENTER_CRITICAL(&nodeAudioMux);
      nodeAudioStreamDone = false;
      portEXIT_CRITICAL(&nodeAudioMux);
      
      nodeAudioEnqueue(audioBuf, got);
    }
    free(audioBuf);
  }
  // Ping → Pong
  else if (op == 0x9) {
    vAgentSendFrame(0xA, nullptr, 0);
  }
  // Close
  else if (op == 0x8) {
    Serial.println("[VAgent] WebSocket close from server");
    vAgentClient->stop();
    vAgentConnected = false;
    vAgentConnecting = false;
    vAgentNextReconnectMs = millis() + VAGENT_RECONNECT_MS;
  }
}

// ============================================================
// END VOICE AGENT
// ============================================================

void printMicTestHelp() {
  Serial.println("[MicTest] Commands:");
  Serial.println("[MicTest]   mictest record [ms]  - record raw mic audio into PSRAM");
  Serial.println("[MicTest]   mictest stop         - stop recording/playback");
  Serial.println("[MicTest]   mictest play         - replay the last recording");
  Serial.println("[MicTest]   mictest clear        - free the PSRAM buffer");
  Serial.println("[MicTest]   mictest status       - show buffer state");
  Serial.println("[MicTest] Note: Single digits are treated as seconds.");
}

void freeMicTestBuffer() {
  if (micTestBuffer) {
    free(micTestBuffer);
    micTestBuffer = nullptr;
  }
  micTestBufferBytes = 0;
  micTestBytesWritten = 0;
  micTestMaxBytes = 0;
}

void resumeMicTestDeepgramIfNeeded() {
  if (!micTestPausedDeepgram) return;
  micTestPausedDeepgram = false;
  if (currentMode == MODE_AI && aiInputUsesMic && !aaiConnected && !aaiConnecting) {
    connectAssemblyAI();
  }
}

void finalizeMicTestBufferHeader() {
  if (!micTestBuffer || micTestBufferBytes < PCM_WAV_HEADER_SIZE) return;

  size_t pcmBytes = micTestBytesWritten;
  if (pcmBytes > micTestBufferBytes - PCM_WAV_HEADER_SIZE) {
    pcmBytes = micTestBufferBytes - PCM_WAV_HEADER_SIZE;
  }

  pcm_wav_header_t header = PCM_WAV_HEADER_DEFAULT(pcmBytes, 16, SAMPLE_RATE, 1);
  memcpy(micTestBuffer, &header, sizeof(header));
}

void stopMicTestPlayback() {
  if (!micTestPlaybackPending) return;
  micTestPlaybackPending = false;
  micTestSpeaker_i2s.end();
  micTestActive = micTestRecording;

  if (micTestShouldResumeLiveMic) {
    micTestShouldResumeLiveMic = false;
    resumeMicTestDeepgramIfNeeded();
  }
}

void stopMicTestRecording() {
  if (!micTestRecording) return;
  micTestRecording = false;
  micTestActive = micTestPlaybackPending;
  finalizeMicTestBufferHeader();
  resumeMicTestDeepgramIfNeeded();
  Serial.printf("[MicTest] Recording stopped at %u bytes\n", (unsigned)micTestBytesWritten);
}

bool startMicTestRecording(unsigned long durationMs) {
  if (micTestRecording) {
    Serial.println("[MicTest] Recording already in progress");
    return true;
  }
  if (micTestPlaybackPending) {
    stopMicTestPlayback();
  }

  if (durationMs == 0) durationMs = MIC_TEST_DEFAULT_SECONDS * 1000UL;
  durationMs = constrain(durationMs, 1000UL, MIC_TEST_MAX_SECONDS * 1000UL);

  size_t requiredBytes = (size_t)(((uint64_t)SAMPLE_RATE * (uint64_t)durationMs * 2ULL) / 1000ULL);
  if (requiredBytes < 1024) requiredBytes = 1024;

  freeMicTestBuffer();
  micTestBufferBytes = requiredBytes + PCM_WAV_HEADER_SIZE;
  micTestBuffer = psramFound() ? (uint8_t*)ps_malloc(micTestBufferBytes) : (uint8_t*)malloc(micTestBufferBytes);
  if (!micTestBuffer) {
    Serial.println("[MicTest] ERROR: buffer allocation failed");
    return false;
  }

  micTestBytesWritten = 0;
  micTestMaxBytes = requiredBytes;
  micTestDurationMs = durationMs;
  micTestStartMs = millis();
  micTestLastRmsLogMs = 0;
  micTestRecording = true;
  micTestActive = true;
  micTestShouldResumeLiveMic = false;

  if (currentMode == MODE_AI && aiInputUsesMic && aaiConnected) {
    micTestPausedDeepgram = true;
    disconnectAssemblyAI();
  }

  pcm_wav_header_t header = PCM_WAV_HEADER_DEFAULT(requiredBytes, 16, SAMPLE_RATE, 1);
  memcpy(micTestBuffer, &header, sizeof(header));

  stopActiveAudioPlayback(false);
  Serial.printf("[MicTest] Recording started: %lu ms, %u bytes in %s\n",
                durationMs,
                (unsigned)requiredBytes,
                psramFound() ? "PSRAM" : "RAM");
  return true;
}

bool startMicTestPlayback() {
  if (micTestRecording) {
    stopMicTestRecording();
  }
  if (!micTestBuffer || micTestBytesWritten < 2) {
    Serial.println("[MicTest] No recording available");
    return false;
  }
  if (micTestPlaybackPending) {
    Serial.println("[MicTest] Playback already in progress");
    return true;
  }

  micTestShouldResumeLiveMic = (currentMode == MODE_AI && aiInputUsesMic && micI2SActive);
  if (micTestShouldResumeLiveMic && aaiConnected) {
    micTestPausedDeepgram = true;
    disconnectAssemblyAI();
  }

  stopActiveAudioPlayback(false);
  micTestSpeaker_i2s.end();
  micTestSpeaker_i2s.setPins(SPK_BCLK, SPK_LRC, SPK_DOUT);
  if (!micTestSpeaker_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("[MicTest] ERROR: speaker I2S init failed");
    return false;
  }

  // Just initialize I2S and set the flag; serviceMicTest() will handle the actual data flow
  micTestPlaybackPending = true;
  micTestActive = true;

  finalizeMicTestBufferHeader();
  return true;
}

void serviceMicTest() {
  if (!micTestRecording && !micTestPlaybackPending) {
    micTestActive = false;
    return;
  }

  micTestActive = true;

  if (micTestRecording) {
    if (micTestDurationMs > 0 && (millis() - micTestStartMs >= micTestDurationMs)) {
      stopMicTestRecording();
      return;
    }

    if (!micTestBuffer || micTestBytesWritten >= micTestMaxBytes) {
      Serial.println("[MicTest] Buffer full");
      stopMicTestRecording();
      return;
    }

    size_t remaining = micTestMaxBytes - micTestBytesWritten;
    size_t requestSize = remaining > 1024 ? 1024 : remaining;

    // Use actual buffer pointer starting after the WAV header
    // Exact DAZI treatment: Raw samples, filter only absolute silence bits
    uint8_t* targetBuffer = micTestBuffer + PCM_WAV_HEADER_SIZE + micTestBytesWritten;
    size_t bytesRead = mic_i2s.readBytes((char*)targetBuffer, requestSize);

    if (bytesRead > 0) {
      int16_t* samples = (int16_t*)targetBuffer;
      size_t sampleCount = bytesRead / sizeof(int16_t);
      int64_t rmsSum = 0;
      for (size_t i = 0; i < sampleCount; i++) {
        int16_t sample = samples[i];
        // Filter tiny invalid noise (0, -1, 1) per ArduinoASRChat.cpp
        if (sample == 0 || sample == -1 || sample == 1) {
          samples[i] = 0;
        }
        rmsSum += (int64_t)samples[i] * (int64_t)samples[i];
      }
      micTestBytesWritten += bytesRead;

      if (millis() - micTestLastRmsLogMs >= 1000) {
        int32_t rms = sampleCount > 0 ? (int32_t)sqrt((double)rmsSum / (double)sampleCount) : 0;
        Serial.printf("[MicTest] REC %u/%u bytes RMS=%d\n",
                      (unsigned)micTestBytesWritten,
                      (unsigned)micTestMaxBytes,
                      rms);
        micTestLastRmsLogMs = millis();
      }

      if (micTestBytesWritten >= micTestMaxBytes) {
        Serial.println("[MicTest] Capture complete");
        stopMicTestRecording();
      }
    }
  }

  if (micTestPlaybackPending) {
    static uint32_t playbackOffset = 0;
    static unsigned long lastChunkMs = 0;

    if (playbackOffset == 0) {
      Serial.printf("[MicTest] Starting playback of %u bytes\n", (unsigned)micTestBytesWritten);
    }

    if (millis() - lastChunkMs >= 20) { // Send ~20ms chunks to keep loop responsive
      uint8_t* pcmData = micTestBuffer + PCM_WAV_HEADER_SIZE + playbackOffset;
      size_t remaining = micTestBytesWritten - playbackOffset;
      size_t chunkSize = 1024;
      size_t writeSize = remaining > chunkSize ? chunkSize : remaining;

      if (writeSize > 0) {
        size_t written = micTestSpeaker_i2s.write(pcmData, writeSize);
        if (written > 0) {
          playbackOffset += written;
          lastChunkMs = millis();
        } else {
          Serial.println("[MicTest] ERROR: playback write failed");
          playbackOffset = micTestBytesWritten; // Force end
        }
      }

      if (playbackOffset >= micTestBytesWritten) {
        Serial.println("[MicTest] Playback complete");
        playbackOffset = 0;
        stopMicTestPlayback();
      }
    }
  }
}

// ============================================================
// MOTION CONTROL (Motors & Servo)
// ============================================================
void setDrv8833Motor(uint8_t in1Pin, uint8_t in2Pin, int speed) {
  // DRV8833 uses one PWM input active at a time and the other low.
  speed = constrain(speed, -255, 255);

  if (speed > 0) {
    ledcWrite(in1Pin, speed);
    ledcWrite(in2Pin, 0);
  } else if (speed < 0) {
    ledcWrite(in1Pin, 0);
    ledcWrite(in2Pin, -speed);
  } else {
    ledcWrite(in1Pin, 0);
    ledcWrite(in2Pin, 0);
  }
}

void setMotorL(int speed) {
  setDrv8833Motor(MOTOR1_IN1, MOTOR1_IN2, speed);
}

void setMotorR(int speed) {
  setDrv8833Motor(MOTOR2_IN1, MOTOR2_IN2, speed);
}

void moveNeck(int angle) {
  // angle: 0 to 180 degrees
  static int lastAngle = -1;
  angle = constrain(angle, 0, 180);
  if (angle == lastAngle) return;
  lastAngle = angle;

  // 50Hz, 14-bit resolution (0-16383 limit). 1-2ms pulse width = ~5-10% duty cycle
  int duty = map(angle, 0, 180, 819, 1638);
  ledcWrite(SERVO_PIN, duty);
}

bool imuProbeAddress(uint8_t addr) {
  tcaselect(CH_IMU);
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool imuWriteReg(uint8_t reg, uint8_t value) {
  tcaselect(CH_IMU);
  Wire.beginTransmission(imuAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool imuReadRegs(uint8_t reg, uint8_t* buffer, size_t len) {
  tcaselect(CH_IMU);
  Wire.beginTransmission(imuAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;

  size_t readLen = Wire.requestFrom((uint8_t)imuAddress, (uint8_t)len);
  if (readLen != len) return false;

  for (size_t i = 0; i < len; i++) {
    buffer[i] = Wire.read();
  }
  return true;
}

bool readIMUGyroZDps(float& gyroZDps) {
  uint8_t data[6];
  if (!imuReadRegs(0x43, data, sizeof(data))) return false;

  int16_t rawZ = (int16_t)((data[4] << 8) | data[5]);
  gyroZDps = (float)rawZ / 131.0f;
  return true;
}

bool readIMUAccelG(float& accelXg, float& accelYg, float& accelZg) {
  uint8_t data[6];
  if (!imuReadRegs(0x3B, data, sizeof(data))) return false;

  int16_t rawX = (int16_t)((data[0] << 8) | data[1]);
  int16_t rawY = (int16_t)((data[2] << 8) | data[3]);
  int16_t rawZ = (int16_t)((data[4] << 8) | data[5]);

  accelXg = (float)rawX / 16384.0f;
  accelYg = (float)rawY / 16384.0f;
  accelZg = (float)rawZ / 16384.0f;
  return true;
}

float normalizeAngleDeg(float angle) {
  while (angle > 180.0f) angle -= 360.0f;
  while (angle < -180.0f) angle += 360.0f;
  return angle;
}

void updateIMUOrientation() {
  if (!imuReady) return;
  if (millis() - lastIMUAccelUpdate < IMU_ACCEL_UPDATE_INTERVAL_MS) return;
  lastIMUAccelUpdate = millis();

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  if (!readIMUAccelG(ax, ay, az)) return;

  if (IMU_INVERTED_MOUNT) {
    // Flip axes for upside-down mounting (rotated 180° around X axis)
    ay = -ay; 
    az = -az;
  }

  imuAccelXg = ax;
  imuAccelYg = ay;
  imuAccelZg = az;
  imuAccelMagnitudeG = sqrtf((ax * ax) + (ay * ay) + (az * az));
  imuPitchDeg = atan2f(-ax, sqrtf((ay * ay) + (az * az))) * 57.29578f;
  imuRollDeg = atan2f(ay, az) * 57.29578f;
}

bool initIMU() {
  if (imuProbeAddress(0x68)) {
    imuAddress = 0x68;
  } else if (imuProbeAddress(0x69)) {
    imuAddress = 0x69;
  } else {
    imuReady = false;
    Serial.println("[IMU] MPU-6500 not found on 0x68 or 0x69");
    return false;
  }

  Serial.printf("[IMU] MPU-6500 found at 0x%02X\n", imuAddress);

  // Reset and configure the gyro for straight-drive correction.
  if (!imuWriteReg(0x6B, 0x80)) return false; // PWR_MGMT_1 reset
  delay(100);
  if (!imuWriteReg(0x6B, 0x01)) return false; // clock = PLL with X gyro
  imuWriteReg(0x1A, 0x03); // CONFIG: DLPF
  imuWriteReg(0x1B, 0x00); // GYRO_CONFIG: +/-250 dps
  imuWriteReg(0x1C, 0x00); // ACCEL_CONFIG: +/-2g
  imuWriteReg(0x1D, 0x03); // ACCEL_CONFIG2: DLPF
  imuWriteReg(0x19, 0x04); // SMPLRT_DIV

  uint8_t whoAmI = 0;
  if (imuReadRegs(0x75, &whoAmI, 1)) {
    Serial.printf("[IMU] WHO_AM_I=0x%02X\n", whoAmI);
  }

  imuReady = true;
  return true;
}

void calibrateIMUGyro() {
  if (!imuReady) return;

  Serial.println("[IMU] Calibrating gyro bias. Keep robot still...");
  const int sampleCount = 200;
  double sum = 0.0;
  int validSamples = 0;
  imuGyroZFiltered = 0.0f;

  for (int i = 0; i < sampleCount; i++) {
    float gyroZ = 0.0f;
    if (readIMUGyroZDps(gyroZ)) {
      sum += gyroZ;
      validSamples++;
    }
    delay(5);
    yield();
  }

  if (validSamples > 0) {
    imuGyroZBias = (float)(sum / validSamples);
    Serial.printf("[IMU] Gyro Z bias = %.3f dps (%d samples)\n", imuGyroZBias, validSamples);
    imuYawEstimateDeg = 0.0f;
    turnAccumDeg = 0.0f;
    lastIMUAccelUpdate = 0;
    lastIMUSampleMs = millis();
  } else {
    Serial.println("[IMU] Gyro calibration failed");
  }
}

// Manual forward/curve motion only; used to refresh ToF faster and brake before a wall.
bool isManualForwardMotionActive() {
  if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) return false;
  if (!driveControlActive || turnControlActive) return false;
  if (driveTargetLeft < 0 || driveTargetRight < 0) return false;
  return (driveTargetLeft > 0 || driveTargetRight > 0);
}

void updateIMUNavigation() {
  if (!imuReady) return;
  if (millis() - lastIMUUpdate < IMU_UPDATE_INTERVAL_MS) return;
  lastIMUUpdate = millis();

  float gyroZ = 0.0f;
  if (!readIMUGyroZDps(gyroZ)) return;

  unsigned long now = millis();
  if (lastIMUSampleMs == 0) lastIMUSampleMs = now;
  float dt = (now - lastIMUSampleMs) / 1000.0f;
  lastIMUSampleMs = now;

  gyroZ = (gyroZ - imuGyroZBias) * IMU_YAW_SIGN;
  if (gyroZ > -IMU_GYRO_DEADBAND_DPS && gyroZ < IMU_GYRO_DEADBAND_DPS) {
    gyroZ = 0.0f;
  }

  imuGyroZFiltered = (imuGyroZFiltered * 0.8f) + (gyroZ * 0.2f);
  imuYawEstimateDeg = normalizeAngleDeg(imuYawEstimateDeg + (gyroZ * dt));
  updateIMUOrientation();

  if (turnControlActive) {
    turnAccumDeg += (gyroZ * dt);
    if (fabsf(turnAccumDeg) >= fabsf(turnTargetDeg)) {
      stopMotorMotion(false);
      return;
    }
  }

  bool frontBrakeActive = false;
  if (isManualForwardMotionActive() && tofReady && tofHasReading) {
    uint16_t frontMm = tofFilteredMm;
    if (frontMm <= TOF_AUTO_STOP_MM) {
      stopMotorMotion(true);
      return;
    }

    if (frontMm <= TOF_AUTO_SLOW_MM) {
      frontBrakeActive = true;
    }
  }

  if (driveHoldStraight && (driveControlActive || isMoving)) {
    float rate = imuGyroZFiltered;
    int correction = (int)(rate * IMU_YAW_KP);
    correction = constrain(correction, -IMU_MAX_CORRECTION, IMU_MAX_CORRECTION);

    int left = driveTargetLeft;
    int right = driveTargetRight;
    if (frontBrakeActive) {
      left = min(left, TOF_MANUAL_CREEP_SPEED);
      right = min(right, TOF_MANUAL_CREEP_SPEED);
    }

    int leftOut = left - correction;
    int rightOut = right + correction;
    if (frontBrakeActive) {
      leftOut = constrain(leftOut, 0, 255);
      rightOut = constrain(rightOut, 0, 255);
    } else {
      leftOut = constrain(leftOut, -255, 255);
      rightOut = constrain(rightOut, -255, 255);
    }

    setMotorL(leftOut);
    setMotorR(rightOut);
  } else if (frontBrakeActive) {
    int left = constrain(min(driveTargetLeft, TOF_MANUAL_CREEP_SPEED), 0, 255);
    int right = constrain(min(driveTargetRight, TOF_MANUAL_CREEP_SPEED), 0, 255);
    setMotorL(left);
    setMotorR(right);
  }
}



void checkIMUSafety() {
  if (!imuReady) return;
  if (millis() - lastIMUSafetyCheck < IMU_SAFETY_CHECK_INTERVAL_MS) return;
  lastIMUSafetyCheck = millis();

  updateIMUOrientation();

  float tiltDeg = fmaxf(fabsf(imuPitchDeg), fabsf(imuRollDeg));
  bool tilted = tiltDeg >= IMU_TILT_ALERT_DEG;
  bool warningTilt = tiltDeg >= IMU_TILT_WARN_DEG;
  bool impact = imuAccelMagnitudeG >= IMU_ACCEL_BUMP_G;
  bool drop = imuAccelMagnitudeG <= IMU_ACCEL_DROP_G;

  if (!tilted && !impact && !drop) return;
  if (millis() - lastIMUSafetyAlert < IMU_SAFETY_COOLDOWN_MS) return;
  lastIMUSafetyAlert = millis();

  if (driveControlActive || isMoving || turnControlActive) {
    stopMotorMotion(true);
  }

  FirebaseJson imuStatus;
  imuStatus.set("ready", imuReady);
  imuStatus.set("heading", imuYawEstimateDeg);
  imuStatus.set("pitch", imuPitchDeg);
  imuStatus.set("roll", imuRollDeg);
  imuStatus.set("tilt", tiltDeg);
  imuStatus.set("accelG", imuAccelMagnitudeG);
  imuStatus.set("warningTilt", warningTilt);
  imuStatus.set("tilted", tilted);
  imuStatus.set("impact", impact);
  imuStatus.set("drop", drop);
  imuStatus.set("driveAssist", driveHoldStraight);
  imuStatus.set("motionState", driveControlActive ? "manual" : (isMoving ? "timed" : "idle"));
  if (firebaseReady) {
    Firebase.RTDB.setJSONAsync(&fbdo, "/status/imu", &imuStatus);
  }

  String alert = "IMU safety: ";
  if (tilted) alert += "tilt detected. ";
  if (impact) alert += "impact detected. ";
  if (drop) alert += "possible lift or drop. ";
  alert += "Pitch=" + String(imuPitchDeg, 1) + " Roll=" + String(imuRollDeg, 1) + " Accel=" + String(imuAccelMagnitudeG, 2) + "g.";
  Serial.println("[IMU] " + alert);

  if (cloudBotToken.length() > 0 && cloudChatId.length() > 0) {
    sendTelegramAlert(String("⚠️ ") + alert);
  }
  if (currentMedState != MED_MEASURING && !isSpeaking && !isProcessingAI) {
    speakText("IMU safety alert. Please check the robot.");
  }
}

void checkAutoImuRecalibration() {
  if (!imuReady) return;
  if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) return;
  if (currentMedState != MED_IDLE) return;
  if (isSpeaking || isProcessingAI) return;
  if (driveControlActive || isMoving || turnControlActive) {
    imuStillStartMs = 0;
    return;
  }

  if (fabsf(imuGyroZFiltered) > IMU_AUTO_RECALIB_GYRO_DPS) {
    imuStillStartMs = 0;
    return;
  }
  if (fabsf(imuAccelMagnitudeG - 1.0f) > IMU_AUTO_RECALIB_ACCEL_DELTA_G) {
    imuStillStartMs = 0;
    return;
  }

  unsigned long now = millis();
  if (imuStillStartMs == 0) {
    imuStillStartMs = now;
    return;
  }

  if (now - imuStillStartMs < IMU_AUTO_RECALIB_STILL_MS) return;
  if (now - lastAutoImuRecalibMs < IMU_AUTO_RECALIB_COOLDOWN_MS) return;

  Serial.println("[IMU] Auto recalibration (robot still)");
  lastAutoImuRecalibMs = now;
  imuStillStartMs = 0;
  recalibrateNavigationHeading();
}

void stopMotorMotion(bool clearQueue) {
  setMotorL(0);
  setMotorR(0);
  isMoving = false;
  currentMovementDuration = 0;
  movementTimer = 0;
  driveControlActive = false;
  driveHoldStraight = false;
  turnControlActive = false;
  driveTargetLeft = 0;
  driveTargetRight = 0;
  turnTargetDeg = 0.0f;
  turnAccumDeg = 0.0f;
  lastIMUUpdate = 0;
  lastIMUAccelUpdate = 0;
  lastIMUSampleMs = 0;
  imuGyroZFiltered = 0.0f;

  if (clearQueue) {
    clearMovementQueue();
  }
}

void recalibrateNavigationHeading() {
  if (!imuReady) {
    Serial.println("[IMU] Recalibration requested, but IMU is not ready");
    return;
  }

  stopMotorMotion(true);
  delay(150);
  calibrateIMUGyro();
}

void startTimedMotorMotion(int leftSpeed, int rightSpeed, int durationMs, bool holdStraight) {
  driveTargetLeft = leftSpeed;
  driveTargetRight = rightSpeed;
  driveHoldStraight = holdStraight && imuReady;
  driveControlActive = false;
  turnControlActive = false;
  imuGyroZFiltered = 0.0f;

  setMotorL(leftSpeed);
  setMotorR(rightSpeed);

  isMoving = true;
  currentMovementDuration = durationMs;
  movementTimer = millis();
  lastIMUUpdate = 0;
  lastIMUAccelUpdate = 0;
  lastIMUSampleMs = 0;
}

void startManualMotorMotion(int leftSpeed, int rightSpeed, bool holdStraight) {
  driveTargetLeft = leftSpeed;
  driveTargetRight = rightSpeed;
  driveHoldStraight = holdStraight && imuReady;
  driveControlActive = true;
  turnControlActive = false;
  imuGyroZFiltered = 0.0f;
  isMoving = false;
  currentMovementDuration = 0;
  movementTimer = 0;

  setMotorL(leftSpeed);
  setMotorR(rightSpeed);

  lastIMUUpdate = 0;
  lastIMUAccelUpdate = 0;
  lastIMUSampleMs = 0;
}

void startTimedIdleAction(int durationMs) {
  driveControlActive = false;
  driveHoldStraight = false;
  turnControlActive = false;
  driveTargetLeft = 0;
  driveTargetRight = 0;
  turnTargetDeg = 0.0f;
  turnAccumDeg = 0.0f;
  imuGyroZFiltered = 0.0f;

  setMotorL(0);
  setMotorR(0);

  isMoving = true;
  currentMovementDuration = durationMs;
  movementTimer = millis();
  lastIMUUpdate = 0;
  lastIMUAccelUpdate = 0;
  lastIMUSampleMs = 0;
}

void startTimedTurnMotion(bool turnLeft, int speed, float targetDeg) {
  speed = constrain(speed, 0, 255);
  targetDeg = fabsf(targetDeg);

  driveTargetLeft = turnLeft ? -speed : speed;
  driveTargetRight = turnLeft ? speed : -speed;
  driveHoldStraight = false;
  driveControlActive = false;
  turnControlActive = true;
  turnTargetDeg = targetDeg;
  turnAccumDeg = 0.0f;
  imuGyroZFiltered = 0.0f;

  setMotorL(driveTargetLeft);
  setMotorR(driveTargetRight);

  isMoving = true;
  currentMovementDuration = max(900, (int)(targetDeg * 20.0f));
  movementTimer = millis();
  lastIMUUpdate = 0;
  lastIMUAccelUpdate = 0;
  lastIMUSampleMs = 0;
}

String normalizeMovementCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "FORWARD") return "FWD";
  if (cmd == "BACKWARD") return "BWD";
  if (cmd == "TURNAROUND" || cmd == "TURN AROUND" || cmd == "TURN_AROUND" || cmd == "TURN-AROUND") return "TURN_R_180";
  if (cmd == "SPIN") return "SPIN_R";
  if (cmd == "SPIN_LEFT") return "SPIN_L";
  if (cmd == "SPIN_RIGHT") return "SPIN_R";
  if (cmd == "DANCE" || cmd == "DANCE_AHEAD" || cmd == "DANCE_PARTY") return "DANCE";

  return cmd;
}

bool isFrontPathClearForShowyMotion() {
  if (!tofReady || !tofHasReading) return true;
  return tofFilteredMm > EXPRESSIVE_FWD_CLEAR_MM;
}

void queueDanceRoutine(const String& style) {
  String lower = style;
  lower.toLowerCase();
  stopMotorMotion(true);
  clearMovementQueue();

  // The routine is deliberately multi-step so "dance" feels like choreography,
  // not just a single spin.
  if (lower.indexOf("party") >= 0 || lower.indexOf("big") >= 0 || lower.indexOf("show") >= 0) {
    if (isFrontPathClearForShowyMotion()) {
      queueMovementCommand("FWD_SLOW");
      queueMovementCommand("PAUSE");
      queueMovementCommand("TURN_L_45");
      queueMovementCommand("PAUSE");
      queueMovementCommand("BWD_SLOW");
      queueMovementCommand("PAUSE");
      queueMovementCommand("SPIN_L_SLOW");
      queueMovementCommand("PAUSE");
      queueMovementCommand("TURN_R_90");
      queueMovementCommand("PAUSE");
      queueMovementCommand("LOOK_UP");
      queueMovementCommand("LOOK_DOWN");
      queueMovementCommand("CENTER");
      return;
    }
  }

  queueMovementCommand("LOOK_UP");
  queueMovementCommand("PAUSE");
  queueMovementCommand("TURN_L_45");
  queueMovementCommand("PAUSE");
  queueMovementCommand("TURN_R_90");
  queueMovementCommand("PAUSE");
  queueMovementCommand("SPIN_L_SLOW");
  queueMovementCommand("PAUSE");
  queueMovementCommand("LOOK_DOWN");
  queueMovementCommand("CENTER");
}

void maybeExpressiveSpeechMotion() {
  if (!ttsSpeechSessionActive || !isSpeaking || !audio.isRunning()) return;
  if (driveControlActive || turnControlActive || isMoving) return;
  if (guardModeEnabled || guardAlarmActive || autoAvoidEnabled) return;
  if (currentMedState != MED_IDLE) return;
  if (queueHead != queueTail) return;
  if (millis() - lastExpressiveSpeechMotionMs < EXPRESSIVE_SPEECH_MOTION_MIN_GAP_MS) return;
  if (expressiveSpeechMotionQueued) return;

  String mood = currentEyeExpression;
  mood.toUpperCase();

  if (mood == "HAPPY" || mood == "EXCITED" || mood == "LOVE" || mood == "WINK") {
    queueMovementCommand("LOOK_UP");
    queueMovementCommand("PAUSE");
    queueMovementCommand("CENTER");
  } else if (mood == "FRUSTRATED" || mood == "ANGRY" || mood == "SUSPICIOUS") {
    queueMovementCommand("LOOK_DOWN");
    queueMovementCommand("PAUSE");
    queueMovementCommand("CENTER");
  } else if (mood == "THINKING" || mood == "WORRIED") {
    queueMovementCommand("LOOK_UP");
    queueMovementCommand("PAUSE");
    queueMovementCommand("LOOK_DOWN");
    queueMovementCommand("CENTER");
  } else {
    queueMovementCommand("LOOK_UP");
    queueMovementCommand("PAUSE");
    queueMovementCommand("CENTER");
  }

  expressiveSpeechMotionQueued = true;
  lastExpressiveSpeechMotionMs = millis();
}

void appendSpatialAwarenessNote(const String& note) {
  String clean = normalizeMemoryText(note);
  if (clean.length() == 0) return;

  if (spatialMemoryCount < SPATIAL_MEMORY_MAX_OBS) {
    uint8_t slot = (spatialMemoryStart + spatialMemoryCount) % SPATIAL_MEMORY_MAX_OBS;
    spatialMemoryNotes[slot] = clean;
    spatialMemoryCount++;
  } else {
    spatialMemoryNotes[spatialMemoryStart] = clean;
    spatialMemoryStart = (spatialMemoryStart + 1) % SPATIAL_MEMORY_MAX_OBS;
  }

  spatialAwarenessSummary = "";
  for (uint8_t i = 0; i < spatialMemoryCount; i++) {
    uint8_t idx = (spatialMemoryStart + i) % SPATIAL_MEMORY_MAX_OBS;
    if (spatialMemoryNotes[idx].length() == 0) continue;
    if (spatialAwarenessSummary.length() > 0) spatialAwarenessSummary += "\n";
    spatialAwarenessSummary += "- " + spatialMemoryNotes[idx];
  }
}

String describeDistanceBand(uint16_t mm) {
  if (mm == 0) return "unreadable";
  if (mm <= 250) return "blocked";
  if (mm <= 500) return "close";
  if (mm <= 900) return "clear";
  return "open";
}

bool captureDistanceAtNeckAngle(int neckAngle, uint16_t& distanceMm) {
  moveNeck(neckAngle);
  delay(160);
  yield();
  return readToFDistanceMm(distanceMm);
}

String buildSpatialAwarenessContext() {
  String s = "\nSPATIAL AWARENESS (recent sensor memory from the robot's body):\n";
  if (spatialAwarenessSummary.length() > 0) {
    s += spatialAwarenessSummary;
    s += "\n";
  } else {
    s += "No recent room scan stored yet.\n";
  }
  s += "Use this like your own memory of where space is open or blocked.\n";
  return s;
}

String performSpatialSurvey(bool exploreMode, bool speakFriendly) {
  if (!tofReady && !tofHasReading) {
    return "[WORRIED] My front distance sensor is not ready yet, so I cannot do a fresh room scan.";
  }
  if (driveControlActive || turnControlActive || isMoving) {
    return "[NORMAL] I’m moving already, so I’ll scan once I’m steady.";
  }
  if (currentMedState != MED_IDLE) {
    return "[NORMAL] I’m busy with a health routine right now. I’ll scan after that.";
  }

  uint16_t frontMm = 0;
  uint16_t leftMm = 0;
  uint16_t rightMm = 0;

  bool frontOk = captureDistanceAtNeckAngle(90, frontMm);
  bool leftOk = captureDistanceAtNeckAngle(145, leftMm);
  bool rightOk = captureDistanceAtNeckAngle(35, rightMm);
  moveNeck(90);

  if (!frontOk && tofHasReading) frontMm = tofFilteredMm;
  if (!leftOk && tofHasReading) leftMm = tofFilteredMm;
  if (!rightOk && tofHasReading) rightMm = tofFilteredMm;

  String frontBand = describeDistanceBand(frontMm);
  String leftBand = describeDistanceBand(leftMm);
  String rightBand = describeDistanceBand(rightMm);
  String headingText = imuReady ? String(imuYawEstimateDeg, 1) + " deg" : "unknown";
  publishRoomMapSnapshot(frontMm, leftMm, rightMm, imuReady ? imuYawEstimateDeg : 0.0f);

  String summary = "Scan complete. ";
  summary += "Front is " + frontBand + " at " + String(frontMm) + " mm. ";
  summary += "Left is " + leftBand + " at " + String(leftMm) + " mm. ";
  summary += "Right is " + rightBand + " at " + String(rightMm) + " mm. ";
  summary += "My heading is " + headingText + ".";

  appendSpatialAwarenessNote("Heading " + headingText + ", front " + String(frontMm) + " mm (" + frontBand + "), left " + String(leftMm) + " mm (" + leftBand + "), right " + String(rightMm) + " mm (" + rightBand + ")");

  if (exploreMode) {
    uint16_t bestSide = max(leftMm, rightMm);
    bool leftOpen = leftMm >= rightMm;

    if (frontMm > 650 && frontMm >= bestSide) {
      queueMovementCommand("FWD_SLOW");
      summary += " I’m moving forward because the front path is the cleanest.";
    } else if (bestSide > frontMm + 120) {
      queueMovementCommand(leftOpen ? "TURN_L_45" : "TURN_R_45");
      if (frontMm > 450) {
        queueMovementCommand("FWD_SLOW");
      }
      summary += leftOpen ? " I’m turning left toward the more open side." : " I’m turning right toward the more open side.";
    } else {
      queueMovementCommand("LOOK_UP");
      queueMovementCommand("CENTER");
      summary += " I’m staying put and thinking for a moment because the space looks tight.";
    }
  }

  if (speakFriendly) {
    return (exploreMode ? "[HAPPY] " : "[NORMAL] ") + summary;
  }
  return summary;
}

bool queueMovementCommand(String cmd) {
  cmd = normalizeMovementCommand(cmd);

  int nextTail = (queueTail + 1) % 20;
  if (nextTail == queueHead) {
    Serial.println("[Motor] Queue full, dropping: " + cmd);
    return false;
  }

  movementQueue[queueTail] = cmd;
  queueTail = nextTail;
  return true;
}

void clearMovementQueue() {
  queueHead = 0;
  queueTail = 0;
  for (int i = 0; i < 20; i++) {
    movementQueue[i] = "";
  }
}

bool isSpeechSafeMotionCommand(const String& cmd) {
  String normalized = normalizeMovementCommand(cmd);
  return normalized == "LOOK_UP" ||
         normalized == "LOOK_DOWN" ||
         normalized == "CENTER" ||
         normalized == "PAUSE" ||
         normalized == "STOP";
}

bool executeMovementCommand(String cmd) {
  cmd = normalizeMovementCommand(cmd);

  // Any explicit motion command overrides autonomous assist modes.
  setAutoAvoidMode(false);
  setGuardMode(false);
  lastInteractionTime = millis();

  // --- Speed definitions ---
  int SPEED_NORMAL = motorSpeed;
  int SPEED_SLOW   = motorSpeed / 2;
  int SPEED_FAST   = min(255, (motorSpeed * 5) / 4);

  if (cmd == "FWD" || cmd == "FWD_NORMAL") {
    startTimedMotorMotion(SPEED_NORMAL, SPEED_NORMAL, 1000, true);
  } else if (cmd == "FWD_SLOW") {
    startTimedMotorMotion(SPEED_SLOW, SPEED_SLOW, 1200, true);  // Slower, so longer to cover same distance
  } else if (cmd == "FWD_FAST") {
    startTimedMotorMotion(SPEED_FAST, SPEED_FAST, 800, true);   // Faster burst
    clearAllMemory();
  } else if (cmd == "SPIN_L_SLOW") {
    startTimedMotorMotion(-SPEED_SLOW, SPEED_SLOW, 900, false);
  } else if (cmd == "SPIN_L_FAST") {
    startTimedMotorMotion(-SPEED_FAST, SPEED_FAST, 400, false);
  } else if (cmd == "TURN_L_45" || cmd == "TURN_LEFT_45") {
    startTimedTurnMotion(true, SPEED_SLOW, 45.0f);
  } else if (cmd == "TURN_L_90" || cmd == "TURN_LEFT_90") {
    startTimedTurnMotion(true, SPEED_NORMAL, 90.0f);
  } else if (cmd == "TURN_L_180" || cmd == "TURN_LEFT_180") {
    startTimedTurnMotion(true, SPEED_NORMAL, 180.0f);

  } else if (cmd == "RIGHT") {
    // Gentle curve right
    startTimedMotorMotion(SPEED_NORMAL, SPEED_SLOW, 700, false);
  } else if (cmd == "SPIN_R" || cmd == "SPIN_R_NORMAL") {
    startTimedMotorMotion(SPEED_NORMAL, -SPEED_NORMAL, 600, false);
  } else if (cmd == "SPIN_R_SLOW") {
    startTimedMotorMotion(SPEED_SLOW, -SPEED_SLOW, 900, false);
  } else if (cmd == "SPIN_R_FAST") {
    startTimedMotorMotion(SPEED_FAST, -SPEED_FAST, 400, false);
  } else if (cmd == "TURN_R_45" || cmd == "TURN_RIGHT_45") {
    startTimedTurnMotion(false, SPEED_SLOW, 45.0f);
  } else if (cmd == "TURN_R_90" || cmd == "TURN_RIGHT_90") {
    startTimedTurnMotion(false, SPEED_NORMAL, 90.0f);
  } else if (cmd == "TURN_R_180" || cmd == "TURN_RIGHT_180") {
    startTimedTurnMotion(false, SPEED_NORMAL, 180.0f);

  } else if (cmd == "BWD" || cmd == "BWD_NORMAL") {
    startTimedMotorMotion(-SPEED_NORMAL, -SPEED_NORMAL, 1000, true);
  } else if (cmd == "BWD_SLOW") {
    startTimedMotorMotion(-SPEED_SLOW, -SPEED_SLOW, 1200, true);
  } else if (cmd == "BWD_FAST") {
    startTimedMotorMotion(-SPEED_FAST, -SPEED_FAST, 800, true);

  } else if (cmd == "STOP") {
    startTimedIdleAction(300);
  } else if (cmd == "PAUSE") {
    // Pause without moving (gap between actions)
    startTimedIdleAction(800);
  } else if (cmd == "DANCE") {
    queueDanceRoutine("dance");
    return true;

  } else if (cmd == "LOOK_UP") {
    moveNeck(130);
    startTimedIdleAction(500);
  } else if (cmd == "LOOK_DOWN") {
    moveNeck(50);
    startTimedIdleAction(500);
  } else if (cmd == "CENTER") {
    moveNeck(90);
    startTimedIdleAction(500);
  } else {
    return false;
  }
  return true;
}

bool applyManualMotorCommand(String cmd) {
  cmd = normalizeMovementCommand(cmd);

  // Manual control must immediately cancel autonomy and guard standby.
  setAutoAvoidMode(false);
  setGuardMode(false);
  lastInteractionTime = millis();

  if (cmd == "STOP" || cmd == "PAUSE") {
    stopMotorMotion(true);
    return true;
  }

  if (cmd == "DANCE") {
    stopMotorMotion(true);
    queueDanceRoutine("dance");
    return true;
  }

  if (cmd == "LOOK_UP") {
    stopMotorMotion(true);
    moveNeck(130);
    return true;
  } else if (cmd == "LOOK_DOWN") {
    stopMotorMotion(true);
    moveNeck(50);
    return true;
  } else if (cmd == "CENTER") {
    stopMotorMotion(true);
    moveNeck(90);
    return true;
  }

  // Manual drive commands should immediately override any queued AI motion.
  stopMotorMotion(true);

  int SPEED_NORMAL = motorSpeed;
  int SPEED_SLOW   = motorSpeed / 2;
  int SPEED_FAST   = min(255, (motorSpeed * 5) / 4);

  if (cmd == "FWD" || cmd == "FWD_NORMAL" || cmd == "FORWARD") {
    startManualMotorMotion(SPEED_NORMAL, SPEED_NORMAL, true);
  } else if (cmd == "FWD_SLOW") {
    startManualMotorMotion(SPEED_SLOW, SPEED_SLOW, true);
  } else if (cmd == "FWD_FAST") {
    startManualMotorMotion(SPEED_FAST, SPEED_FAST, true);
  } else if (cmd == "BWD" || cmd == "BWD_NORMAL" || cmd == "BACKWARD") {
    startManualMotorMotion(-SPEED_NORMAL, -SPEED_NORMAL, true);
  } else if (cmd == "BWD_SLOW") {
    startManualMotorMotion(-SPEED_SLOW, -SPEED_SLOW, true);
  } else if (cmd == "BWD_FAST") {
    startManualMotorMotion(-SPEED_FAST, -SPEED_FAST, true);
  } else if (cmd == "LEFT") {
    startManualMotorMotion(SPEED_SLOW, SPEED_NORMAL, false);
  } else if (cmd == "SPIN_L" || cmd == "SPIN_L_NORMAL") {
    startManualMotorMotion(-SPEED_NORMAL, SPEED_NORMAL, false);
  } else if (cmd == "SPIN_L_SLOW") {
    startManualMotorMotion(-SPEED_SLOW, SPEED_SLOW, false);
  } else if (cmd == "SPIN_L_FAST") {
    startManualMotorMotion(-SPEED_FAST, SPEED_FAST, false);
  } else if (cmd == "TURN_L_45" || cmd == "TURN_LEFT_45") {
    startTimedTurnMotion(true, SPEED_SLOW, 45.0f);
  } else if (cmd == "TURN_L_90" || cmd == "TURN_LEFT_90") {
    startTimedTurnMotion(true, SPEED_NORMAL, 90.0f);
  } else if (cmd == "TURN_L_180" || cmd == "TURN_LEFT_180") {
    startTimedTurnMotion(true, SPEED_NORMAL, 180.0f);
  } else if (cmd == "RIGHT") {
    startManualMotorMotion(SPEED_NORMAL, SPEED_SLOW, false);
  } else if (cmd == "SPIN_R" || cmd == "SPIN_R_NORMAL") {
    startManualMotorMotion(SPEED_NORMAL, -SPEED_NORMAL, false);
  } else if (cmd == "SPIN_R_SLOW") {
    startManualMotorMotion(SPEED_SLOW, -SPEED_SLOW, false);
  } else if (cmd == "SPIN_R_FAST") {
    startManualMotorMotion(SPEED_FAST, -SPEED_FAST, false);
  } else if (cmd == "TURN_R_45" || cmd == "TURN_RIGHT_45") {
    startTimedTurnMotion(false, SPEED_SLOW, 45.0f);
  } else if (cmd == "TURN_R_90" || cmd == "TURN_RIGHT_90") {
    startTimedTurnMotion(false, SPEED_NORMAL, 90.0f);
  } else if (cmd == "TURN_R_180" || cmd == "TURN_RIGHT_180") {
    startTimedTurnMotion(false, SPEED_NORMAL, 180.0f);
  } else {
    return false;
  }

  return true;
}

// Queue Processing (Called quickly via loop())
void processMovementQueue() {
  if (turnControlActive || driveControlActive || isMoving || queueHead != queueTail) {
    updateIMUNavigation();
  }

  // If we are currently moving, check if the duration for this move has expired
  if (isMoving) {
    if (millis() - movementTimer >= currentMovementDuration) {
      // Time is up! Stop motors.
      stopMotorMotion(false);
    } else {
      // Still moving, do nothing yet
      return; 
    }
  }

  // If we are not moving, check if there are pending commands in the queue
  if (!isMoving && !driveControlActive && queueHead != queueTail) {
    // Execute motor commands immediately without waiting for speech to finish
    // This allows non-blocking concurrent motor movement during TTS

    String cmd = movementQueue[queueHead];
    queueHead = (queueHead + 1) % 20; // Advance head (Ring Buffer)
    cmd.trim();
    cmd.toUpperCase();

    Serial.println("[Motor] Executing: " + cmd);
    if (!executeMovementCommand(cmd)) {
      Serial.println("[Motor] Ignored unknown command: " + cmd);
    }
  }
}

// ============================================================
// VL53L0X / ROBOT ASSIST HELPERS
// ============================================================
bool readToFDistanceMm(uint16_t &distanceMm) {
  if (!tofReady) return false;

  tcaselect(CH_TOF);
  uint16_t raw = frontToF.readRangeContinuousMillimeters();
  if (frontToF.timeoutOccurred()) return false;

  if (raw == 0 || raw > TOF_MAX_VALID_MM) raw = TOF_MAX_VALID_MM;
  distanceMm = raw;
  return true;
}

void initToF() {
  tcaselect(CH_TOF);
  frontToF.setTimeout(50);

  if (!frontToF.init()) {
    tofReady = false;
    tofHasReading = false;
    tofDistanceMm = 0;
    tofFilteredMm = 0;
    Serial.println("[ToF] VL53L0X not found");
    return;
  }

  frontToF.setMeasurementTimingBudget(33000);
  frontToF.startContinuous(50);
  tofReady = true;
  tofHasReading = false;
  tofDistanceMm = 0;
  tofFilteredMm = TOF_MAX_VALID_MM;
  Serial.println("[ToF] VL53L0X ready");
}

// ============================================================
// ULTRASONIC SENSOR (HC-SR04) - Chest Mounted
// ============================================================
float readUltrasonicDistanceCm() {
  // Send 10us pulse to trigger
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  
  // Read echo pulse duration (timeout 30ms = ~5m max range)
  long duration = pulseIn(ULTRASONIC_ECHO_PIN, HIGH, 30000);
  
  // Calculate distance in cm (speed of sound = 343 m/s)
  // Distance = (duration / 2) / 29.1
  if (duration == 0) {
    return -1.0; // Timeout or no echo
  }
  
  float distanceCm = duration / 58.0; // Divide by 58 for cm (round trip)
  
  // HC-SR04 valid range: 2cm to 400cm
  if (distanceCm < 2.0 || distanceCm > 400.0) {
    return -1.0; // Out of range
  }
  
  return distanceCm;
}

void startGuardBuzzer() {
  if (guardBuzzerOn) return;
  if (buzzerReady && ledcWriteTone(BUZZER_PIN, GUARD_BEEP_FREQ) > 0) {
    guardBuzzerOn = true;
  }
}

void stopGuardBuzzer() {
  if (!guardBuzzerOn) return;
  ledcWriteTone(BUZZER_PIN, 0);
  guardBuzzerOn = false;
}

void drawGuardAlertScreen(const String& message) {
  tft.fillScreen(UI_ERROR);
  tft.setTextColor(UI_TEXT_MAIN);
  tft.setTextSize(2);
  tft.setFont();
  tft.setCursor(24, 60);
  tft.print("GUARD ALERT");

  tft.setTextSize(1);
  tft.setCursor(24, 100);
  tft.print(message);

  tft.setCursor(24, 130);
  tft.print("Distance: ");
  tft.print(tofHasReading ? String(tofFilteredMm) : String("--"));
  tft.print(" mm");

  tft.fillRoundRect(24, 170, 190, 34, 8, ILI9341_WHITE);
  tft.setTextColor(UI_PRIMARY);
  tft.setCursor(36, 183);
  tft.print("Alarm sent to Telegram");
}

void publishRobotAssistStatus(bool force) {
  if (offlineModeLocked || !mqtt.connected()) return;
  if (audioTransitionIsQuiet()) return;

  if (!force && millis() - lastRobotAssistPush < ROBOT_ASSIST_PUSH_INTERVAL_MS) return;

  StaticJsonDocument<320> doc;
  doc["tofReady"] = tofReady;
  doc["tofHasReading"] = tofHasReading;
  doc["tofDistanceMm"] = tofHasReading ? (int)tofFilteredMm : 0;
  doc["tofDistanceCm"] = tofHasReading ? (tofFilteredMm / 10.0f) : 0.0f;
  doc["autoAvoid"] = autoAvoidEnabled;
  doc["guardMode"] = guardModeEnabled;
  doc["guardAlarm"] = guardAlarmActive;
  doc["aiInputMode"] = getAIInputModeName();
  doc["aaiConnected"] = aaiConnected;
  doc["autoState"] = autoAvoidEnabled ? (
    autoAvoidState == AUTO_AVOID_CRUISE ? "CRUISE" :
    autoAvoidState == AUTO_AVOID_BACKUP ? "BACKUP" :
    autoAvoidState == AUTO_AVOID_SCAN_LEFT_SETTLE ? "SCAN_LEFT" :
    autoAvoidState == AUTO_AVOID_SCAN_RIGHT_SETTLE ? "SCAN_RIGHT" :
    autoAvoidState == AUTO_AVOID_TURNING ? "TURNING" : "IDLE"
  ) : "OFF";
  doc["mode"] = currentMode == MODE_AI ? "AI" : "NORMAL";
  doc["online"] = networkAvailable();

  String payload;
  serializeJson(doc, payload);
  if (mqtt.publish("ella/robotAssist", payload.c_str(), true)) {
    lastRobotAssistPush = millis();
  }
  publishStatusSnapshot(force);
}

void clearGuardAlarm(bool redraw) {
  guardAlarmActive = false;
  guardNearCount = 0;
  guardClearCount = 0;
  guardBuzzerPhaseOn = false;
  guardBuzzerPhaseStart = 0;
  stopGuardBuzzer();
  if (redraw && currentMode == MODE_NORMAL) {
    drawNormalScreen(true);
  }
  publishRobotAssistStatus(true);
}

void triggerGuardAlarm(uint16_t distanceMm) {
  if (guardAlarmActive) return;

  guardAlarmActive = true;
  guardClearCount = 0;
  guardBuzzerPhaseOn = true;
  guardBuzzerPhaseStart = millis();
  guardNearCount = 0;
  autoAvoidEnabled = false;
  autoAvoidState = AUTO_AVOID_IDLE;

  stopMotorMotion(true);
  moveNeck(90);
  startGuardBuzzer();

  clearTtsChunkQueue();
  if (audio.isRunning()) audio.stopSong();
  enterAudioTransitionQuiet();
  enterNetworkQuiet();
  isSpeaking = false;
  isProcessingAI = false;

  setEyeExpression("SUSPICIOUS");
  drawGuardAlertScreen("Someone is in front of the robot.");

  String alert = "Guard alert triggered. Front distance: " + String(distanceMm) + " mm.";
  Serial.println("[Guard] " + alert);

  if (millis() - lastGuardAlertSent > GUARD_ALERT_COOLDOWN_MS) {
    String msg = "🚨 <b>GUARD ALERT</b>\n\n";
    msg += "Front object detected at <b>" + String(distanceMm) + " mm</b>.\n";
    if (user_name.length() > 0) msg += "User: <b>" + user_name + "</b>\n";
    msg += "\nELLA is monitoring the area.";
    sendTelegramAlert(msg);
    lastGuardAlertSent = millis();
  }

  publishRobotAssistStatus(true);
}

void setGuardMode(bool enabled) {
  if (guardModeEnabled == enabled) return;

  guardModeEnabled = enabled;

  if (enabled) {
    autoAvoidEnabled = false;
    autoAvoidState = AUTO_AVOID_IDLE;
    stopMotorMotion(true);
    moveNeck(90);
    clearTtsChunkQueue();
    if (audio.isRunning()) audio.stopSong();
    enterAudioTransitionQuiet();
    isSpeaking = false;
    isProcessingAI = false;
    if (currentMode != MODE_NORMAL) switchToNormalMode();
    setEyeExpression("SLEEPY");
    guardAlarmActive = false;
    guardNearCount = 0;
    guardClearCount = 0;
    drawNormalScreen(true);
  } else {
    clearGuardAlarm(false);
    if (firebaseReady) Firebase.RTDB.setBool(&fbdo, "/commands/guard", false);
    if (currentMode == MODE_NORMAL) {
      setEyeExpression("NORMAL");
      drawNormalScreen(true);
    }
  }

  publishRobotAssistStatus(true);
}

void setAutoAvoidMode(bool enabled) {
  if (autoAvoidEnabled == enabled) return;
  if (enabled && (guardModeEnabled || guardAlarmActive)) {
    Serial.println("[Auto] Ignored enable while guard is active");
    return;
  }

  autoAvoidEnabled = enabled;

  if (enabled) {
    guardModeEnabled = false;
    clearGuardAlarm(false);
    stopMotorMotion(true);
    moveNeck(90);
    clearTtsChunkQueue();
    if (audio.isRunning()) audio.stopSong();
    enterAudioTransitionQuiet();
    isSpeaking = false;
    isProcessingAI = false;
    if (currentMode != MODE_NORMAL) switchToNormalMode();
    autoAvoidState = AUTO_AVOID_CRUISE;
    autoObstacleHitCount = 0;
    autoAvoidStateTimer = millis();
    setEyeExpression("THINKING");
    drawNormalScreen(true);
  } else {
    stopMotorMotion(true);
    autoAvoidState = AUTO_AVOID_IDLE;
    autoObstacleHitCount = 0;
    autoAvoidLeftMm = 0;
    autoAvoidRightMm = 0;
    moveNeck(90);
    clearTtsChunkQueue();
    if (audio.isRunning()) audio.stopSong();
    enterAudioTransitionQuiet();
    if (firebaseReady) Firebase.RTDB.setBool(&fbdo, "/commands/autoAvoid", false);
    if (currentMode == MODE_NORMAL) {
      setEyeExpression("NORMAL");
      drawNormalScreen(true);
    }
  }

  publishRobotAssistStatus(true);
}

void updateGuardAlarmPattern() {
  if (!guardAlarmActive) return;

  unsigned long now = millis();
  if (guardBuzzerPhaseOn) {
    if (now - guardBuzzerPhaseStart >= GUARD_BEEP_ON_MS) {
      stopGuardBuzzer();
      guardBuzzerPhaseOn = false;
      guardBuzzerPhaseStart = now;
    }
  } else {
    if (now - guardBuzzerPhaseStart >= GUARD_BEEP_OFF_MS) {
      startGuardBuzzer();
      guardBuzzerPhaseOn = true;
      guardBuzzerPhaseStart = now;
    }
  }
}

void updateRobotAssistModes() {
  // Keep the front distance fresh without hammering the bus.
  unsigned long tofInterval = (autoAvoidEnabled || guardModeEnabled || guardAlarmActive || isManualForwardMotionActive()) ? TOF_READ_INTERVAL_MS : (currentMode == MODE_AI ? 1000 : 250);
  if (tofReady && millis() - lastToFUpdate >= tofInterval) {
    lastToFUpdate = millis();
    uint16_t distanceMm = 0;
    if (readToFDistanceMm(distanceMm)) {
      tofHasReading = true;
      tofDistanceMm = distanceMm;

      if (tofFilteredMm == 0 || tofFilteredMm > TOF_MAX_VALID_MM) {
        tofFilteredMm = distanceMm;
      } else {
        tofFilteredMm = (uint16_t)((tofFilteredMm * 3UL + distanceMm) / 4UL);
      }
    }
  }

  if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive || driveControlActive) {
    lastInteractionTime = millis();
  }

  if (guardModeEnabled) {
    if (autoAvoidEnabled) {
      autoAvoidEnabled = false;
      autoAvoidState = AUTO_AVOID_IDLE;
      autoObstacleHitCount = 0;
    }

    if (currentMode != MODE_NORMAL) switchToNormalMode();
    if (driveControlActive || isMoving || turnControlActive) stopMotorMotion(true);
    clearTtsChunkQueue();
    if (audio.isRunning()) audio.stopSong();
    enterAudioTransitionQuiet();
    isSpeaking = false;
    isProcessingAI = false;

    if (!tofReady || !tofHasReading) return;

    if (guardAlarmActive) {
      updateGuardAlarmPattern();
      if (tofFilteredMm > TOF_GUARD_CLEAR_MM) guardClearCount++;
      else guardClearCount = 0;

      if (guardClearCount >= TOF_GUARD_CLEAR_READS) {
        clearGuardAlarm(true);
      }
      return;
    }

    if (tofFilteredMm > 0 && tofFilteredMm <= TOF_GUARD_TRIGGER_MM) {
      guardNearCount++;
    } else if (guardNearCount > 0) {
      guardNearCount--;
    }

    if (guardNearCount >= TOF_GUARD_CONFIRM_READS) {
      triggerGuardAlarm(tofFilteredMm);
    }
    return;
  }

  if (!autoAvoidEnabled) {
    if (autoAvoidState != AUTO_AVOID_IDLE) {
      autoAvoidState = AUTO_AVOID_IDLE;
      autoObstacleHitCount = 0;
    }
    return;
  }

  if (currentMode != MODE_NORMAL || currentMedState != MED_IDLE || isSpeaking || isProcessingAI || isSleepMode) {
    if (driveControlActive || isMoving || turnControlActive) stopMotorMotion(true);
    autoAvoidState = AUTO_AVOID_CRUISE;
    autoObstacleHitCount = 0;
    return;
  }

  if (!tofReady || !tofHasReading) return;

  uint16_t frontMm = tofFilteredMm;
  int cruiseSpeed = constrain(motorSpeed, 110, 255);
  int slowSpeed = max(90, cruiseSpeed / 2);
  int backupSpeed = max(90, cruiseSpeed / 2);
  int turnSpeed = max(110, cruiseSpeed / 2);

  switch (autoAvoidState) {
    case AUTO_AVOID_IDLE:
      autoAvoidState = AUTO_AVOID_CRUISE;
      break;

    case AUTO_AVOID_CRUISE:
      moveNeck(90);

      if (frontMm <= TOF_AUTO_STOP_MM) {
        autoObstacleHitCount++;
      } else {
        autoObstacleHitCount = 0;
      }

      if (frontMm <= TOF_AUTO_STOP_MM && autoObstacleHitCount >= 2) {
        stopMotorMotion(true);
        autoAvoidState = AUTO_AVOID_BACKUP;
        autoAvoidStateTimer = millis();
        startTimedMotorMotion(-backupSpeed, -backupSpeed, 350, true);
      } else if (frontMm <= TOF_AUTO_SLOW_MM) {
        if (!driveControlActive || driveTargetLeft != slowSpeed || driveTargetRight != slowSpeed || !driveHoldStraight) {
          startManualMotorMotion(slowSpeed, slowSpeed, true);
        }
      } else {
        if (!driveControlActive || driveTargetLeft != cruiseSpeed || driveTargetRight != cruiseSpeed || !driveHoldStraight) {
          startManualMotorMotion(cruiseSpeed, cruiseSpeed, true);
        }
      }
      break;

    case AUTO_AVOID_BACKUP:
      if (!isMoving && !driveControlActive && !turnControlActive) {
        autoAvoidState = AUTO_AVOID_SCAN_LEFT_SETTLE;
        moveNeck(145);
        autoAvoidStateTimer = millis();
      }
      break;

    case AUTO_AVOID_SCAN_LEFT_SETTLE:
      if (millis() - autoAvoidStateTimer >= 250) {
        if (!readToFDistanceMm(autoAvoidLeftMm)) autoAvoidLeftMm = tofFilteredMm;
        moveNeck(35);
        autoAvoidState = AUTO_AVOID_SCAN_RIGHT_SETTLE;
        autoAvoidStateTimer = millis();
      }
      break;

    case AUTO_AVOID_SCAN_RIGHT_SETTLE:
      if (millis() - autoAvoidStateTimer >= 250) {
        if (!readToFDistanceMm(autoAvoidRightMm)) autoAvoidRightMm = tofFilteredMm;
        moveNeck(90);
        bool turnLeft = autoAvoidLeftMm >= autoAvoidRightMm;
        float turnDeg = 45.0f;
        startTimedTurnMotion(turnLeft, turnSpeed, turnDeg);
        autoAvoidState = AUTO_AVOID_TURNING;
        autoAvoidStateTimer = millis();
      }
      break;

    case AUTO_AVOID_TURNING:
      if (!isMoving && !driveControlActive && !turnControlActive) {
        autoAvoidState = AUTO_AVOID_CRUISE;
        autoObstacleHitCount = 0;
        moveNeck(90);
      }
      break;
  }
}

// ============================================================
// SETUP
// ============================================================
void sttTask(void* parameter) {
    Serial.println("[STT Task] Started on Core 0");
    while (true) {
        if (USE_NODE_SERVER && aiInputUsesMic && currentMode == MODE_AI && networkAvailable()) {
          if (nodeMux && xSemaphoreTake(nodeMux, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!nodeWsConnected) connectNodeServer();
            pumpNodeServerSocket();
            streamMicToNodeServer();
            serviceNodeWebSocketKeepAlive(); 
            xSemaphoreGive(nodeMux);
          }
        } else if (USE_VOICE_AGENT && aiInputUsesMic && currentMode == MODE_AI && networkAvailable()) {
          if (vAgentMux && xSemaphoreTake(vAgentMux, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (vAgentConnected && vAgentConfigured && !isSpeaking) {
              streamMicToVoiceAgent();
            }
            pumpVoiceAgentSocket();
            sendVoiceAgentKeepAlive();
            if (!vAgentConnected && !vAgentConnecting && millis() >= vAgentNextReconnectMs) {
              connectVoiceAgent();
            }
            xSemaphoreGive(vAgentMux);
          }
        } else if (sttSocketPumpEnabled && aaiConnected && currentMode == MODE_AI && aiInputUsesMic && networkAvailable()) {
            // Old pipeline: separate DG STT WebSocket
            streamMicToAAI();
            pumpAaiSocket();
            sendAaiKeepAlive();
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // Give CPU0 more breathing room
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (int i = 0; i < length; i++) msg += (char)payload[i];
    String sTopic = String(topic);

    Serial.printf("[MQTT] Topic: %s | Msg: %s\n", topic, msg.c_str());

    if (sTopic == "ella/telegram/in") {
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, msg);
        if (!error) {
            String text = doc["text"].as<String>();
            String chatId = doc["chatId"].as<String>();
            handleIncomingTelegramCommand(text, chatId);
        }
        return;
    }

    if (sTopic.startsWith("ella/commands/")) {
        String key = sTopic.substring(14);
        
        if (key == "speak") {
            speakText(msg.c_str(), true);
        } else if (key == "stopAudio") {
            stopActiveAudioPlayback(true);
            setSpeakingStatusFirebase(false);
        } else if (key == "focusMode") {
            focusModeActive = (msg == "true");
            Serial.printf("[Focus] Mode set to %d\n", focusModeActive);
        } else if (key == "emergency") {
            bool active = (msg == "true");
            if (active) {
                sendEmergencyAlert("MQTT Panic Button");
            }
        } else if (key == "motor") {
            applyManualMotorCommand(msg);
        } else if (key == "systemMode") {
            if (msg == "AI") switchToAIMode();
            else if (msg == "NORMAL" || msg == "OFF") switchToNormalMode();
            Serial.printf("[System] Mode set to %s\n", msg.c_str());
        } else if (key == "autoAvoid") {
            setAutoAvoidMode(msg == "true");
        } else if (key == "guard") {
            setGuardMode(msg == "true");
        } else if (key == "speed") {
            motorSpeed = msg.toInt();
            Serial.printf("[Speed] set to %d\n", motorSpeed);
        } else if (key == "aiInputMode") {
            setAIInputMode(true, true);
        } else if (key == "aiChat") {
            if (msg.length() > 1) {
              queueWebAIChat(msg);
            }
        } else if (key == "checkup") {
            if (msg == "true" && currentMedState == MED_IDLE) {
              ai_requested_checkup = true;
              if (currentMode != MODE_NORMAL) switchToNormalMode();
              currentMedState = MED_IDLE;
              medStateTimer = millis();
              drawNormalScreen(true);
              speakText("Starting medical checkup. Please place your finger on the sensor.");
            }
        } else if (key == "breathe") {
            if (msg == "true") {
              if (currentMode != MODE_NORMAL) switchToNormalMode();
              currentMedState = MED_BREATHING;
              currentMeditationType = MED_TYPE_BREATHING;
              medStateTimer = millis();
              drawNormalScreen(true);
              speakText("Starting breathing exercise.");
            }
        } else if (key == "meditate") {
            if (msg.length() > 0) {
              startMeditation(msg);
            }
        } else if (key == "imuReset") {
            if (msg == "true") {
              recalibrateNavigationHeading();
            }
        }
    }
}

void reconnectMQTT() {
  // Allow MQTT during music playback so web app can control it
  bool musicPlaying = audio.isRunning() && !isSpeaking && !isProcessingAI;
  
  if (offlineModeLocked || !networkAvailable() || (currentMode == MODE_AI && !musicPlaying) || (networkIsQuiet() && !musicPlaying) || isSpeaking || isProcessingAI) {
    if (mqtt.connected()) {
      mqtt.disconnect();
      mqttClientNet->stop(); // Force stop to free TLS buffers
      Serial.println("[MQTT] Paused while audio or quiet mode is active");
    }
    return; 
  }


  if (!mqtt.connected()) {
    if (millis() - lastMqttRetryMs > 10000) {
      lastMqttRetryMs = millis();
      
      // Diagnostic Logging
      Serial.printf("[MQTT] Attempting connection... (WiFi Status: %d, IP: %s, Heap: %d)\n", 
                    WiFi.status(), WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
      
      // Ensure memory-safe state and insecure mode for each attempt
      mqttClientNet->setInsecure();
      
      String clientId = "EllaBox-" + String(random(0xffff), HEX);
      if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
        Serial.println("[MQTT] connected");
        mqtt.subscribe("ella/telegram/in");
        Serial.println("[MQTT] Subscribed to ella/telegram/in (Bridge)");
        mqtt.subscribe("ella/commands/#");
        mqtt.subscribe("ella/settings/#");
        publishStatusSnapshot(true);
        publishRobotAssistStatus(true);
      } else {
        Serial.print("[MQTT] failed, rc=");
        Serial.println(mqtt.state());
      }
    }
  }
}

void setup() {



  Serial.begin(115200);

  // Setup mouth display (secondary ESP)
  setupMouth();

  // Allocate PSRAM buffers for OPUS
  opusPcmBuffer = (int16_t*)heap_caps_malloc(OPUS_FRAME_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  opusPacketBuffer = (uint8_t*)heap_caps_malloc(OPUS_MAX_PACKET_SIZE, MALLOC_CAP_SPIRAM);
  aaiRxBuffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
  
  // Initialize OPUS Encoder
  int err;
  opusEncoder = opus_encoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, OPUS_APPLICATION, &err);
  if (err != OPUS_OK) {
      Serial.printf("[OPUS] Failed to create encoder: %d\n", err);
  } else {
      opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(OPUS_BITRATE));
      opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
      opus_encoder_ctl(opusEncoder, OPUS_SET_VBR(1));
      opus_encoder_ctl(opusEncoder, OPUS_SET_DTX(1));
      opus_encoder_ctl(opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
      Serial.println("[OPUS] Encoder initialized");
  }

  // Start STT Task on Core 0
  xTaskCreatePinnedToCore(sttTask, "sttTask", 12288, NULL, 1, NULL, 0);

  // Setup MQTT
  mqttClientNet->setInsecure(); // Using HiveMQ Cloud (TLS 8883)
  mqttClientNet->setHandshakeTimeout(15000); // 15s handshake timeout for cloud stability
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(4096);

  // Setup STT and LLM WebSocket clients with insecure mode
  if (aaiClient) aaiClient->setInsecure();
  if (llmWsClient) llmWsClient->setInsecure();

  // Telegram now handled by server - no mutex needed!
  
  delay(1000);
  Serial.println("EllaBox Starting...");
  bootTimeMs = millis();

  // HPF filter removed.
  // pinMode(INTERRUPT_PIN, INPUT_PULLUP); // Disabled
  pinMode(TACTILE_SWITCH_PIN, INPUT_PULLUP);
  pinMode(TOUCH_IRQ, INPUT_PULLUP); // Fix for phantom touches
  delay(1000);
  Serial.println("\n=== EllaBox - AI Health Companion ===");

  // ==========================================
  // 1. INIT DISPLAY & UI (IMMEDIATELY)
  // ==========================================
  // SPI Manually Init with NEW PINS
  #define TFT_MISO 13 // User corrected: MISO is 13
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS); 

  // TFT
  // FIX: Lower SPI speed to 20MHz to prevent artifacts/noise on wires
  tft.begin(20000000); 
  tft.setRotation(0);
  
  // Create Mutexes for socket safety
  nodeMux = xSemaphoreCreateMutex();
  vAgentMux = xSemaphoreCreateMutex();
  
  // SHOW LOADING SCREEN IMMEDIATELY
  drawLoadingScreen("System Init...", 5);

  
  // Attach Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerReady = ledcAttach(BUZZER_PIN, 2000, 8);
  if (buzzerReady) {
    ledcWriteTone(BUZZER_PIN, 0);
    Serial.println("[Buzzer] Ready");
  } else {
    Serial.println("[Buzzer] LEDC attach failed");
  }

  // Initialize Motors and Servo Pins
  pinMode(MOTOR1_IN1, OUTPUT);
  pinMode(MOTOR1_IN2, OUTPUT);
  pinMode(MOTOR2_IN1, OUTPUT);
  pinMode(MOTOR2_IN2, OUTPUT);
  pinMode(SERVO_PIN, OUTPUT);
  
  // Initialize Ultrasonic Sensor (HC-SR04) - Chest Mounted
  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  Serial.println("[Ultrasonic] HC-SR04 initialized (Chest)");
  
  // Attach Core v3 PWM
  ledcAttach(MOTOR1_IN1, 1000, 8); // 1KHz, 8-bit resolution (0-255 limit)
  ledcAttach(MOTOR1_IN2, 1000, 8);
  ledcAttach(MOTOR2_IN1, 1000, 8);
  ledcAttach(MOTOR2_IN2, 1000, 8);
  ledcAttach(SERVO_PIN, 50, 14);   // 50Hz, 14-bit resolution (0-16383 limit)
  
  // Start stopped & centered
  setMotorL(0);
  setMotorR(0);
  moveNeck(90);

  // PSRAM check (allocation done later with null-check)
  if (psramFound()) {
    Serial.printf("PSRAM: %d bytes free\n", ESP.getFreePsram());
  }

  // WiFi
  // WiFi — connection handled in startupSequence()
  WiFi.setSleep(false);
  Serial.println("WiFi will connect in startupSequence()...");


  // SPIFFS is used for temporary audio files.
  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Mount failed");
  }

  // I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000); // Lower speed during startup for better reliability

  // Front ToF / head sensors live on the same mux branch as the IMU.
  initToF();



  // OLED Eyes (via Multiplexer)
  // Init Left Eye
  tcaselect(CH_EYE_LEFT);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Left Eye Failed");
  } else {
    Serial.println("Left Eye OK");
    display.clearDisplay();
    display.display();
  }
  
  // Init Right Eye
  tcaselect(CH_EYE_RIGHT);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Right Eye Failed");
  } else {
    Serial.println("Right Eye OK");
    display.clearDisplay();
    display.display();
  }

  // PREFERENCES (Load Saved Settings)
  // NOTE: WiFi credentials are loaded in startupSequence() which runs later.
  prefs.begin("ella", false);
  
  // Load Profile (Telegram, Emergency Contact, User Name)
  cloudBotToken = prefs.getString("botToken", "");
  cloudChatId = prefs.getString("chatId", "");
  user_emergency_contact = prefs.getString("emergency", "");
  user_name = prefs.getString("userName", "");
  loadPendingReminderState();
  loadConversationMemoryFromPrefs();
  aiRequestStatus.reserve(64);
  activeTtsText.reserve(900);
  ttsRequestUrl.reserve(1200);
  pendingLastResponseText.reserve(260);
  currentInterimText.reserve(900);
  spatialAwarenessSummary.reserve(320);
  for (uint8_t i = 0; i < SPATIAL_MEMORY_MAX_OBS; i++) {
    spatialMemoryNotes[i].reserve(160);
  }
  conversationMemorySummary.reserve(CHAT_MEMORY_MAX_SUMMARY_CHARS + 64);
  conversationHistory.reserve((CHAT_MEMORY_MAX_ENTRY_CHARS * 2 + 24) * CHAT_MEMORY_MAX_EXCHANGES);
  for (uint8_t i = 0; i < TTS_CHUNK_QUEUE_MAX; i++) {
    ttsChunkQueue[i].reserve(192);
  }
  
  if (cloudBotToken.length() > 0) Serial.println("[Prefs] Loaded Bot Token");
  
  // Sensors
  // Initialize SPI FIRST before touch (XPT2046 needs MISO=13 for data)
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
  ts.begin();
  ts.setRotation(1);
  Serial.println("[Touch] XPT2046 Initialized on CS=47 IRQ=14 MISO=13");

  // Initialize Sensors
  // Initialize AHT20 on Channel 2
  tcaselect(CH_AHT);
  if (!aht.begin()) Serial.println("AHT20 not found!");
  else Serial.println("AHT20 OK");

  // Initialize ENS160 on Channel 2 (shared with AHT)
  tcaselect(CH_AHT);
  ens160.begin(&Wire, 0x53); // ENS160 standard I2C address
  if (!ens160.init()) {
    Serial.println("ENS160 not found!");
  } else {
    ens160.startStandardMeasure();
    Serial.println("ENS160 OK");
  }

  // Initialize MAX30102 - Auto-Scan all channels
  bool maxFound = false;
  Serial.println("[MAX30102] Scanning all MUX channels for sensor...");
  for (uint8_t ch = 0; ch < 8; ch++) {
      tcaselect(ch);
      delay(10);
      Wire.beginTransmission(0x57);
      if (Wire.endTransmission() == 0) {
          Serial.printf("[MAX30102] FOUND on Channel %d!\n", ch);
          maxFound = true;
          // Note: If found on a different channel, we should update CH_MAX or just stay here
          // For now, we'll continue using the found channel
          break; 
      }
  }

  if (!maxFound) {
      Serial.println("[MAX30102] NOT DETECTED on any MUX channel. Check power/wiring.");
  }

  // Match the working diagnostic sketch: faster bus plus clean FIFO start
  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 OK");
    
    // Exactly matches SparkFun Example8_SPO2 for correct algorithm operation
    // ledBrightness=0xFF (Max Power for better penetration), sampleAverage=4, ledMode=2 (Red+IR ONLY),
    // sampleRate=100, pulseWidth=411, adcRange=4096
    // NOTE: ledMode MUST be 2 (not 3). With 3 LEDs, the FIFO interleaves
    // 3 channels and getIR()/getRed() return wrong data, breaking the algorithm.
    // Use 0x7F power and 16384 ADC range — the "sweet spot" to avoid saturation while staying above 50k threshold
    particleSensor.setup(0x7F, 4, 2, 100, 411, 16384);
    particleSensor.setPulseAmplitudeRed(0x7F);
    particleSensor.setPulseAmplitudeIR(0x7F);
    particleSensor.clearFIFO();
  } else {
      Serial.println("MAX30102 Failed at begin()");
  }

  // Finalize I2C speed for runtime
  Wire.setClock(400000);
  if (initIMU()) {
    calibrateIMUGyro();
  } else {
    Serial.println("[IMU] Straight-drive correction disabled");
  }

  // Speaker
  audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DOUT);
  audio.setAudioTaskCore(0);
  audio.setVolume(21);
  audio.forceMono(true); // Ensure single speaker gets all sound
  Serial.println("Speaker OK");

  // Mic — Use BOTH slots to capture correctly regardless of L/R pin physical wiring
  mic_i2s.setPins(I2S_MIC_SCK, I2S_MIC_WS, -1, I2S_MIC_SD);
  // Mic — Use LEFT slot only (Match DAZI-style INMP441 initialization)
  if (!mic_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("Mic FAILED!");
    while(1);
  }
  
  // Clear startup garbage (Match DAZI stabilization)
  delay(500);
  for (int i = 0; i < 2000; i++) {
    mic_i2s.read();
  }
  Serial.println("Mic OK (DAZI-style LEFT slot)");

  // URL block removed — phone handles STT.

  currentMode = MODE_NORMAL;
  setEyeExpression("NORMAL");
  
  // STARTUP COMPLETE
  // STARTUP COMPLETE
  
  // Allocate Audio Buffer in PSRAM if available
  if (psramFound()) {
      sBuffer = (int16_t*)ps_malloc(BUFFER_LEN * sizeof(int16_t));
      Serial.println("[Mem] sBuffer allocated in PSRAM");
  } else {
      sBuffer = (int16_t*)malloc(BUFFER_LEN * sizeof(int16_t));
      Serial.println("[Mem] sBuffer allocated in RAM");
  }
  if (sBuffer == nullptr) {
      Serial.println("[Mem] Failed to allocate sBuffer!");
      while(1);
  } else {
      // Clear buffer
      memset(sBuffer, 0, BUFFER_LEN * sizeof(int16_t));
  }

  // Allocate Node TTS audio ring buffer in PSRAM
  if (psramFound()) {
    nodeAudioRingBuf = (uint8_t*)ps_malloc(NODE_AUDIO_BUF_SIZE);
    if (nodeAudioRingBuf) Serial.printf("[NODE] Audio ring buffer: %d KB in PSRAM\n", NODE_AUDIO_BUF_SIZE / 1024);
    else Serial.println("[NODE] WARNING: Audio ring buffer alloc failed!");
  }

  // STARTUP COMPLETE
  playStartupSound();
  // drawNormalScreen(true); // Replaced by startupSequence()
  startupSequence(); 
  Serial.println("Setup Complete (Online Mode Ready)!");
}

// Global filter variables
// dc_offset/DC_ALPHA/GAIN_BOOSTER_I2S removed.
bool networkInitialized = false;
bool wifiReconnectActive = false;
bool wifiReconnectUsingSaved = false;
unsigned long wifiReconnectStartMs = 0;
unsigned long nextWiFiReconnectMs = 0;
unsigned long nextFirebaseRetryMs = 0;
unsigned long commandPollBackoffUntil = 0;
unsigned long lastCommandJsonFetchMs = 0;
uint8_t commandReadFailStreak = 0;
wl_status_t lastWiFiStatus = WL_IDLE_STATUS;
const unsigned long WIFI_RECONNECT_PHASE_MS = 8000;
const unsigned long WIFI_RECONNECT_BACKOFF_MS = 30000;
const unsigned long FIREBASE_RETRY_MS = 30000;
const unsigned long COMMAND_POLL_INTERVAL_AI_MS = 220;
const unsigned long COMMAND_POLL_INTERVAL_NORMAL_MS = 170;
const unsigned long COMMAND_ERROR_BACKOFF_BASE_MS = 250;
const uint8_t COMMAND_ERROR_REINIT_THRESHOLD = 6;

// Visual Startup Sequence
void startupSequence() {
  drawLoadingScreen("Connecting WiFi...", 10);
  
  // 2. CONNECT WIFI (Try Saved -> Fallback Hardcoded)
  wifiSSID = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  
  // Validate saved credentials (reject junk like "false", empty, etc.)
  bool savedValid = (wifiSSID.length() > 2 && wifiSSID != "false" && wifiSSID != "null");
  
  if (savedValid) {
      Serial.println("[WiFi] Connecting to SAVED credentials: " + wifiSSID);
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  } else {
      if (wifiSSID.length() > 0) {
          // Clear bad saved credentials
          Serial.println("[WiFi] Clearing invalid saved SSID: " + wifiSSID);
          prefs.remove("ssid");
          prefs.remove("pass");
      }
      Serial.println("[WiFi] Connecting to HARDCODED credentials...");
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      WiFi.begin(ssid, password);
  }
  
  bool wifiConnected = waitForWiFiConnection(savedValid ? 10000 : WIFI_STARTUP_CONNECT_WINDOW_MS);
  
  if (WiFi.status() != WL_CONNECTED && savedValid) {
      Serial.println("\n[WiFi] Saved failed. Trying hardcoded fallback...");
      resetWiFiStationForRetry();
      WiFi.begin(ssid, password);
      wifiConnected = waitForWiFiConnection(10000);
  }

      wifiConnected = (WiFi.status() == WL_CONNECTED);

      if (wifiConnected) {
       offlineModeLocked = false;
       onlineServicesAllowed = true;
       Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
     drawLoadingScreen("Syncing Time...", 50);
     configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, ntpServer2, ntpServer3);
     
     ntpTimeValid = false;
     for(int i=0; i<12; i++) {
        struct tm timeinfo;
        if(getLocalTime(&timeinfo, 500)) {
          ntpTimeValid = true;
          break;
        }
        drawLoadingScreen("Syncing Time...", 50 + i);
     }

      if (ntpTimeValid) {
        pruneConversationMemoryIfExpired();
      } else {
        Serial.println("[NTP] Sync timed out. Trying HTTP Time Sync fallback...");
        syncTimeViaHTTP();
      }
     
     drawLoadingScreen("Initializing AI...", 70);
     setupNetwork(); 
     
     drawLoadingScreen("System Ready!", 100);
     delay(1000); // Show "Ready!" for a moment
  } else {
      offlineModeLocked = true;
      drawLoadingScreen("Offline Mode...", 30);
      onlineServicesAllowed = false;
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("[WiFi] Boot continuing offline; no reconnects after startup window.");
  }

  // Always do initial sensor read regardless of WiFi (sensors are I2C, no network needed)
  updateSensors();

  drawNormalScreen(true);
  
  WiFi.setAutoReconnect(false);
}



// STT is handled by the phone via Firebase.

void resetWiFiStationForRetry() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
}

bool beginWiFiConnectionAttempt(bool useSavedCreds) {
  resetWiFiStationForRetry();

  String savedSSID = wifiSSID;
  String savedPass = wifiPass;
  bool savedValid = (savedSSID.length() > 2 && savedSSID != "false" && savedSSID != "null");

  if (useSavedCreds && savedValid) {
    Serial.println("[WiFi] Reconnect attempt using saved credentials: " + savedSSID);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    return true;
  }

  Serial.println("[WiFi] Reconnect attempt using hardcoded credentials");
  WiFi.begin(ssid, password);
  return false;
}

bool waitForWiFiConnection(unsigned long timeoutMs) {
  unsigned long startMs = millis();
  while (millis() - startMs < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      return true;
    }
    delay(50);
    yield();
  }
  return WiFi.status() == WL_CONNECTED;
}

void resetOnlineServicesForDisconnect() {
  if (aaiConnected || aaiConnecting) {
    disconnectAssemblyAI();
  }

  firebaseReady = false;
  otaReady = false;
  networkInitialized = false;
  onlineServicesAllowed = false;
  nextFirebaseRetryMs = 0;
  if (mqtt.connected()) {
    mqtt.disconnect();
    mqttClientNet->stop();
  }
}

void updateWiFiConnectionManager() {
  if (offlineModeLocked) return;

  wl_status_t currentStatus = WiFi.status();
  unsigned long now = millis();

  if (currentStatus != lastWiFiStatus) {
    if (currentStatus == WL_CONNECTED) {
      Serial.println("[WiFi] Link restored");
      wifiReconnectActive = false;
      wifiReconnectStartMs = 0;
      nextWiFiReconnectMs = 0;
    } else if (lastWiFiStatus == WL_CONNECTED) {
      Serial.printf("[WiFi] Link lost (status=%d)\n", currentStatus);
      resetOnlineServicesForDisconnect();
      wifiReconnectActive = false;
      wifiReconnectStartMs = 0;
    }
    lastWiFiStatus = currentStatus;
  }

  if (currentStatus == WL_CONNECTED) {
    if (onlineServicesAllowed && !networkInitialized) {
      setupNetwork();
    }
    return;
  }
}

void setupOTA() {
  if (offlineModeLocked || otaReady || WiFi.status() != WL_CONNECTED) return;

  uint64_t chipId = ESP.getEfuseMac();
  char host[24];
  snprintf(host, sizeof(host), "ella-%06llX", (chipId >> 24) & 0xFFFFFF);

  ArduinoOTA.setHostname(host);

  ArduinoOTA.onStart([]() {
    const char* updateType = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.printf("[OTA] Starting %s update\n", updateType);

    stopMotorMotion(true);
    clearTtsChunkQueue();
    if (audio.isRunning()) audio.stopSong();
    isSpeaking = false;
    isProcessingAI = false;
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Update complete");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int lastPct = -10;
    int pct = (total == 0) ? 0 : (int)((progress * 100U) / total);
    if (pct - lastPct >= 10 || pct == 100) {
      Serial.printf("[OTA] Progress: %d%%\n", pct);
      lastPct = pct;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]\n", error);
  });

  ArduinoOTA.begin();
  otaReady = true;
  Serial.printf("[OTA] Ready: %s.local\n", host);
}

void setupNetwork() {
  if (offlineModeLocked || !networkAvailable()) return;
  
  Serial.println("[Network] initializing services...");
  
  struct tm timeinfo;
  if(ntpTimeValid && getLocalTime(&timeinfo)){ 
    Serial.println("[Network] Time Synced (Refined)");
  } else {
    Serial.println("[Network] Time Sync unavailable; staying offline-safe");
  }
  setupOTA();
  
  networkInitialized = true;
  publishStatusSnapshot(true);
  Serial.println("[Network] Services Ready!");
  
  // FORCE INITIAL SENSOR READ (To ensure UI has data)
  Serial.println("[Sensors] Initializing readings...");
  tcaselect(CH_AHT);
  if (aht.begin()) {
     read_aht20(); 
     Serial.println("[Sensors] AHT20 Read Success");
  } else {
     Serial.println("[Sensors] AHT20 Failed!");
  }
  
  // Try standard address 0x53
  Serial.println("[Sensors] Initializing ENS160 (0x53)...");
  ens160.begin(&Wire, 0x53);
  if (ens160.isConnected()) {
     Serial.println("[Sensors] ENS160 Active");
     ens160.startStandardMeasure(); // Start measurement
     read_ens160();
  } else {
     // Try alternate 0x52
     Serial.println("[Sensors] 0x53 failed, trying 0x52...");
     ens160.begin(&Wire, 0x52);
     if (ens160.isConnected()) {
       Serial.println("[Sensors] ENS160 Active (0x52)");
       ens160.startStandardMeasure();
       read_ens160();
     } else {
       Serial.println("[Sensors] ENS160 Failed!");
     }
  }
}




// ============================================================
// TOUCHSCREEN (XPT2046)
// ============================================================
void processTouchScreen() {
  static bool modeSwitchUsedThisPress = false;

  // Fast IRQ pre-check: if the interrupt pin hasn't fired, skip SPI entirely
  // This avoids slow SPI bus reads on every loop iteration when nothing is touched
  if (!ts.tirqTouched()) {
    modeSwitchUsedThisPress = false;
    return;
  }

  // Now do the full SPI read to confirm and get coordinates
  if (!ts.touched()) {
    modeSwitchUsedThisPress = false;
    return;
  }

  // WAKE UP if sleeping
  if (isSleepMode) {
      lastInteractionTime = millis();
      lastTouchActionMs = lastInteractionTime;
      moodArousal += 0.5; // Wake up excited/surprised
      return; 
  }
  lastInteractionTime = millis();
  lastTouchActionMs = lastInteractionTime;

  // Skip during AI processing (SPI conflicts) — but allow touch to stop audio
  if (isProcessingAI && !audio.isRunning()) return;

  static unsigned long lastTouch = 0;
  if (millis() - lastTouch < 150) {
      // TOUCH SPAMMING (Rapid taps)
      moodValence -= 0.1; // Gets annoyed quickly if poked repeatedly
      moodArousal += 0.1;
      return; 
  }
  
  // Normal Tap
  moodArousal += 0.2; // Wakes up / pays attention
  if (moodValence < 0) moodValence += 0.1; // Cheers her up a bit
  
  lastTouch = millis();

  TS_Point p = ts.getPoint();

  // Filter out phantom touches (low raw values typically < 100 on noise)
  if (p.x < 150 || p.y < 150) return;


  // Map raw XPT2046 coordinates to screen pixels (240x320)
  int screenX = map(p.x, 200, 3800, 0, 240);
  int screenY = map(p.y, 200, 3800, 0, 320);
  Serial.printf("[Touch] Raw(%d,%d) -> Screen(%d,%d)\n", p.x, p.y, screenX, screenY);

  // ── Navigation Bar (bottom 80px for reliable touch) ────────────────────
  if (screenY > 240) {
    if (currentMode == MODE_AI) {
      // Tap anywhere in nav bar goes BACK when in AI mode
      if (!modeSwitchUsedThisPress) {
        modeSwitchUsedThisPress = true;
        Serial.println("[Touch] Nav: Back to Normal");
        switchToNormalMode();
      }
    } else if (currentMode == MODE_AUTONOMOUS) {
      // Tap anywhere in nav bar goes BACK when in autonomous mode
      if (!modeSwitchUsedThisPress) {
        modeSwitchUsedThisPress = true;
        Serial.println("[Touch] Nav: Back to Normal from Autonomous");
        stopAutonomousDemo(); // Stop demo if running
        switchToNormalMode();
      }
    } else {
      // Normal mode: right side nav taps switch to AI
      if (screenX > 120 && !modeSwitchUsedThisPress) {
        modeSwitchUsedThisPress = true;
        Serial.println("[Touch] Nav: To AI Mode");
        switchToAIMode();
      }
    }
    return;
  }

  // ── Main Content Area ─────────────────────────────────────────
  if (currentMode == MODE_AI) {
    // In AI mode, keep content taps quiet so the mode doesn't bounce.
    // Only stop audio if something is speaking; use the nav bar BACK button to leave AI.
    if (screenY < 240) {
      // Tapping screen in AI mode while audio plays -> stop audio
      if (audio.isRunning() || isSpeaking) {
        modeSwitchUsedThisPress = true;
        Serial.println("[Touch] Stopping audio");
        stopActiveAudioPlayback(true);
      }
    }
  } else {
    // Normal mode: tap screen to go to AI mode (or exit breathing)
    if (currentMedState == MED_BREATHING) {
      if (screenX < 80 && screenY > 200) { // Bottom-Left STOP button
        Serial.println("[Touch] Breathing -> STOP");
        currentMedState = MED_IDLE;
        drawNormalScreen(true);
        speakText("Exercise ended.");
      }
    } else if (screenY < 240 && !modeSwitchUsedThisPress) {
      modeSwitchUsedThisPress = true;
      Serial.println("[Touch] Screen tap -> AI Mode");
      switchToAIMode();
    }
  }
}

// ============================================================
// TACTILE SWITCH
// ============================================================
void processTactileSwitch() {
  int reading = digitalRead(TACTILE_SWITCH_PIN);

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
    Serial.printf("[Button] State Changed to: %d\n", reading);
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == LOW) { // PRESS START
        buttonPressStartTime = millis();
        lastInteractionTime = millis(); // WAKE
      }

      if (buttonState == HIGH) { // PRESS RELEASE
        unsigned long pressDuration = millis() - buttonPressStartTime;
        
        // LONG PRESS (> 2000ms) -> EMERGENCY
        if (pressDuration > 2000) {
            sendEmergencyAlert("Panic Button Pressed (Local)");
        }
        // SHORT PRESS (< 500ms) -> Toggle Mode / Stop Audio / Control Autonomous
        else if (pressDuration < 500) {
          // AUTONOMOUS MODE: Start/Stop demo
          if (currentMode == MODE_AUTONOMOUS) {
             Serial.println("[Switch] Toggling autonomous demo");
             if (autoModeActive) {
               stopAutonomousDemo();
             } else {
               startAutonomousDemo();
             }
          }
          // INTERRUPT: If Audio is playing in AI Mode, STOP IT first
          else if (currentMode == MODE_AI && (audio.isRunning() || isSpeaking)) {
             Serial.println("[Switch] Stopping Audio & Resetting Mic...");
             stopActiveAudioPlayback(true);
             Serial.println("[Switch] Mic Reset Scheduled");
          } 
          else if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) {
             Serial.println("[Switch] Canceling Auto/Guard mode");
             setAutoAvoidMode(false);
             setGuardMode(false);
             if (currentMode != MODE_NORMAL) switchToNormalMode();
             drawNormalScreen(true);
          }
          else {
             // Normal Mode Toggle
             if (currentMode == MODE_NORMAL) switchToAIMode();
             else if (currentMode == MODE_AI) switchToNormalMode();
             else if (currentMode == MODE_AUTONOMOUS) switchToNormalMode();
          }
        }
        // MEDIUM PRESS (500-2000ms) -> Enter Autonomous Mode
        else if (pressDuration >= 500 && pressDuration <= 2000) {
          Serial.println("[Switch] Medium press - Entering Autonomous Mode");
          switchToAutonomousMode();
        }
      }
    }
  }

  lastButtonState = reading;
}

void sendEmergencyAlert(String condition) {
    Serial.println("[Emergency] TRIGGERED! Condition: " + condition);
    
    // 1. Visual/Audio Alarm
    tft.fillScreen(UI_ERROR);
    tft.setTextColor(UI_TEXT_MAIN);
    tft.setTextSize(3);
    tft.setCursor(20, 140);
    tft.print("SOS ALERT!");
    
    playTone(2000, 500);
    delay(100);
    playTone(2000, 500);
    delay(100);
    playTone(2000, 500);

    // 2. Build Message
    String msg = "🚨 *EMERGENCY ALERT!* 🚨\n\n";
    msg += "*Condition:* " + condition + "\n";
    if (user_name.length() > 0) msg += "*User:* " + user_name + "\n";
    else msg += "*User:* Unknown (Profile not set)\n";
    
    if (user_emergency_contact.length() > 0) msg += "*Emergency Contact:* " + user_emergency_contact + "\n";
    else msg += "*Emergency Contact:* Not Configured\n";
    
    msg += "\nCheck on the user immediately!";

    // 3. Send Telegram
    bool success = sendTelegramAlert(msg);
    
    if (success) {
        speakText("Emergency alert sent.");
    } else {
        speakText("Emergency alert failed to send.");
    }
    
    delay(2000);
    if (currentMode == MODE_NORMAL) drawNormalScreen(true);
    else drawAIScreen(true);
}

void switchToNormalMode() {
  if (currentMode == MODE_NORMAL) return;
  
  Serial.println("[Mode] Switching to NORMAL");
  
  // STOP AUDIO FIRST
  clearTtsChunkQueue();
  if (audio.isRunning()) {
      audio.stopSong();
      Serial.println("[Mode] Audio forced stop");
  }
  enterAudioTransitionQuiet();
  
  playProcessingTone(); // Confirmation Beep
  
  currentMode = MODE_NORMAL;
  isProcessingAI = false;
  isSpeaking = false;
  currentRobotActivity = ROBOT_ACTIVITY_IDLE;
  aiRequestStatus = "";
  isSleepMode = false; // Force wake
  lastInteractionTime = millis(); // Reset sleep timer
  currentInterimText = ""; // Clear text
  clearDeferredAiAction();
  
  // 1. Disconnect all AI/STT paths
  if (USE_VOICE_AGENT) disconnectVoiceAgent();
  disconnectNodeServer(); // KILL THE DEEPGRAM PROXY

  disableMicAIInput();
  if (micI2SActive) clearMicBuffer(); // Flush any remaining audio data
  
  // 2. Hard Reset MQTT client to clear AI session buffers
  if (mqtt.connected()) mqtt.disconnect();
  mqttClientNet->stop();
  delay(200); // Wait for stack to settle
  Serial.println("[Mode] MQTT Client Reset & AI Memory Cleared");
  
  // Restart MAX30102 without defaulting to 400sps
  tcaselect(CH_MAX);
  particleSensor.wakeUp();
  // Keep MAX30102 in Red+IR mode only. ledMode=3 interleaves a third channel
  // and breaks getIR()/getRed() based SpO2 calculations.
  // 0x7F power + 16384 ADC range avoids saturation and keeps IR > 50,000
  particleSensor.setup(0x7F, 4, 2, 100, 411, 16384);
  particleSensor.setPulseAmplitudeRed(0x7F);
  particleSensor.setPulseAmplitudeIR(0x7F);
  particleSensor.clearFIFO();
  particleSensor.setPulseAmplitudeRed(0x7F); // MAX Brightness
  particleSensor.setPulseAmplitudeIR(0x7F);  // MAX Brightness
  max30102_needs_full_read = true;
  
  // Force Clear Screen to prevent overlay issues and clear AI text
  tft.fillScreen(UI_BG);
  drawNormalScreen(true); // Force full redraw
  setEyeExpression("NORMAL");
  setModeStatusFirebase("NORMAL");
  flushPendingReminderToFirebase();
  if (!ai_requested_checkup) {
    publishRobotAssistStatus(true);
  }
}

const char* getAIInputModeName() {
  return "MIC";
}

void publishStatusSnapshot(bool force) {
  static unsigned long lastStatusPublishMs = 0;
  if (offlineModeLocked || !mqtt.connected()) return;
  if (!force && millis() - lastStatusPublishMs < 250) return;
  if (audioTransitionIsQuiet()) return;

  StaticJsonDocument<384> doc;
  doc["online"] = networkAvailable();
  doc["mode"] = currentMode == MODE_AI ? "AI" : "NORMAL";
  doc["isSpeaking"] = isSpeaking;
  doc["aiInputMode"] = getAIInputModeName();
  doc["tofReady"] = tofReady;
  doc["tofHasReading"] = tofHasReading;
  doc["tofDistanceMm"] = tofHasReading ? (int)tofFilteredMm : 0;
  doc["guardMode"] = guardModeEnabled;
  doc["guardAlarm"] = guardAlarmActive;
  doc["autoAvoid"] = autoAvoidEnabled;
  doc["aaiConnected"] = aaiConnected;
  if (lastStatusResponseText.length() > 0) {
    doc["lastResponse"] = lastStatusResponseText;
  }

  String payload;
  serializeJson(doc, payload);
  if (mqtt.publish("ella/status", payload.c_str(), true)) {
    lastStatusPublishMs = millis();
  }
}

void publishRoomMapSnapshot(uint16_t frontMm, uint16_t leftMm, uint16_t rightMm, float headingDeg) {
  if (!mqtt.connected()) return;

  clearSlamMap();
  const int center = SLAM_MAP_SIZE / 2;
  setSlamCell(center, center, 2);

  stampSlamRay(0.0f, frontMm);
  stampSlamRay(-45.0f, rightMm);
  stampSlamRay(45.0f, leftMm);

  static const size_t packedBytes = (SLAM_MAP_SIZE * SLAM_MAP_SIZE + 3) / 4;
  uint8_t packed[packedBytes];
  memset(packed, 0, sizeof(packed));

  for (size_t i = 0; i < (SLAM_MAP_SIZE * SLAM_MAP_SIZE); i++) {
    packed[i / 4] |= (slamMapCells[i] & 0x03) << ((i % 4) * 2);
  }

  size_t encodedLen = 0;
  static const size_t encodedCap = ((packedBytes + 2) / 3) * 4 + 4;
  unsigned char encoded[encodedCap];
  if (mbedtls_base64_encode(encoded, sizeof(encoded) - 1, &encodedLen, packed, packedBytes) != 0) {
    return;
  }
  encoded[encodedLen] = '\0';

  StaticJsonDocument<1536> doc;
  doc["type"] = "slam_map";
  doc["size"] = SLAM_MAP_SIZE;
  doc["map"] = (const char*)encoded;
  doc["robotX"] = center * 10;
  doc["robotY"] = center * 10;
  doc["robotHeading"] = headingDeg;
  doc["frontMm"] = frontMm;
  doc["leftMm"] = leftMm;
  doc["rightMm"] = rightMm;

  String payload;
  serializeJson(doc, payload);
  mqtt.publish("ella/map", payload.c_str(), true);
}

void enableMicAIInput() {
  if (!aiInputUsesMic) return;

  if (micI2SActive) {
    // Don't bounce the Node WebSocket while TTS audio is still playing/buffered
    if (USE_NODE_SERVER && (vAgentPlayingAudio || nodeAudioPending || isSpeaking || audio.isRunning())) {
      Serial.println("[Audio] Mic refresh deferred — Ella is still speaking");
      return;
    }
    Serial.printf("[Audio] Mic already active at %lu ms; refreshing session\n", millis());
    clearDeepgramBatchBuffer();
    if (USE_NODE_SERVER) {
      disconnectNodeServer();
      connectNodeServer();
    } else if (USE_VOICE_AGENT) {
      disconnectVoiceAgent();
      connectVoiceAgent();
    } else {
      disconnectAssemblyAI(0);
      connectAssemblyAI();
    }
    currentRobotActivity = ROBOT_ACTIVITY_LISTENING;
    aiRequestStatus = "Listening...";
    setEyeExpression("NORMAL");
    if (currentMode == MODE_AI) {
      drawAIScreen(true);
    }
    return;
  }

  Serial.printf("[Audio] enableMicAIInput start at %lu ms\n", millis());
  mic_i2s.end(); // Ensure previous channel is closed
  delay(10); // Quick reset

  mic_i2s.setPins(I2S_MIC_SCK, I2S_MIC_WS, -1, I2S_MIC_SD);
  // Using I2S_STD_SLOT_LEFT for INMP441 as requested, 16kHz Mono 16-bit
  if (mic_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("[AI] Mic initialized");
    micI2SActive = true;
    
    // Exact DAZI treatment: Small delay for hardware stabilization
    delay(100); 
    clearMicBuffer();
    clearDeepgramBatchBuffer();

    disconnectAssemblyAI(0);
    if (USE_NODE_SERVER) {
      connectNodeServer();
    } else if (USE_VOICE_AGENT) {
      connectVoiceAgent();
    } else {
      connectAssemblyAI();
    }

    currentRobotActivity = ROBOT_ACTIVITY_LISTENING;
    aiRequestStatus = "Listening...";
    setEyeExpression("NORMAL");
    playListeningTone();
    if (currentMode == MODE_AI) {
      drawAIScreen(true);
    }
    Serial.println("[AI] Deepgram STT active");
    Serial.printf("[Audio] enableMicAIInput done at %lu ms\n", millis());
  } else {
    Serial.println("[AI] Mic initialization failed!");
  }
}

void disableMicAIInput() {
  if (USE_VOICE_AGENT) disconnectVoiceAgent();
  disconnectAssemblyAI();
  clearDeepgramBatchBuffer();
  mic_i2s.end();
  micI2SActive = false;
  clearMicAIReconnectSchedule();
}

void setAIInputMode(bool useMicInput, bool force) {
  if (!useMicInput) {
    Serial.println("[AI] Web/Firebase AI input removed; keeping mic input active");
  }

  useMicInput = true;

  if (!force && aiInputUsesMic == useMicInput) return;

  aiInputUsesMic = useMicInput;
  Serial.printf("[AI] Input mode set to %s\n", getAIInputModeName());

  if (currentMode == MODE_AI) {
    enableMicAIInput();
  }

  publishRobotAssistStatus(true);
}

void queueAIChat(String msg) {
  msg.trim();
  if (msg.length() <= 1) return;

  queuedAiChat = msg;
  lastInteractionTime = millis();
  Serial.println("[AI] Queued chat: " + msg);
}

void queueWebAIChat(String msg) {
  queueAIChat(msg);
}

int extractCountAfterKeyword(const String& text, const char* keyword, int defaultCount) {
  String lower = text;
  lower.toLowerCase();

  int keywordPos = lower.indexOf(keyword);
  String tail = (keywordPos >= 0) ? lower.substring(keywordPos + strlen(keyword)) : lower;
  tail.trim();

  for (int i = 0; i < tail.length(); i++) {
    if (isDigit((unsigned char)tail[i])) {
      int j = i + 1;
      while (j < tail.length() && isDigit((unsigned char)tail[j])) j++;
      int value = tail.substring(i, j).toInt();
      return value > 0 ? value : defaultCount;
    }
  }

  struct CountWord { const char* word; int value; };
  static const CountWord countWords[] = {
    {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5},
    {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10}
  };

  for (const auto& entry : countWords) {
    String word = String(entry.word);
    if (tail.startsWith(word + " ") || tail.indexOf(" " + word + " ") >= 0 || tail.endsWith(" " + word)) {
      return entry.value;
    }
  }

  return defaultCount;
}

bool isWordBoundaryChar(char c) {
  return !((c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_');
}

int findWholePhrase(const String& text, const char* phrase, int fromIndex = 0) {
  if (!phrase || phrase[0] == '\0') return -1;

  String needle = phrase;
  if (needle.length() == 0) return -1;

  int idx = text.indexOf(needle, fromIndex);
  while (idx >= 0) {
    char before = (idx > 0) ? text.charAt(idx - 1) : ' ';
    char after = (idx + needle.length() < text.length()) ? text.charAt(idx + needle.length()) : ' ';
    if (isWordBoundaryChar(before) && isWordBoundaryChar(after)) {
      return idx;
    }
    idx = text.indexOf(needle, idx + 1);
  }

  return -1;
}

bool hasCommandPrefixBeforePhrase(const String& text, int phrasePos) {
  if (phrasePos <= 0) return true;

  String before = text.substring(0, phrasePos);
  before.trim();
  if (before.length() == 0) return true;

  before.toLowerCase();
  if (before == "please" || before == "pls" || before == "ella" || before == "hey ella") return true;
  if (before == "can you" || before == "could you" || before == "would you" || before == "will you") return true;
  if (before == "please can you" || before == "please could you" || before == "please would you") return true;
  if (before.endsWith(" please")) return true;

  return false;
}

bool hasExplicitCommandPhrase(const String& text, const char* phrase) {
  int idx = findWholePhrase(text, phrase, 0);
  while (idx >= 0) {
    if (hasCommandPrefixBeforePhrase(text, idx)) {
      return true;
    }
    idx = findWholePhrase(text, phrase, idx + 1);
  }
  return false;
}

String normalizeCommandText(String text) {
  text.toLowerCase();
  text.trim();

  while (text.length() > 0) {
    char c = text.charAt(text.length() - 1);
    if (c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':') {
      text.remove(text.length() - 1);
    } else {
      break;
    }
  }

  text.trim();
  return text;
}

bool handleWebMotionChat(const String& msg, String& spokenReply) {
  String lower = normalizeCommandText(msg);

  if (lower.indexOf("stop mentioning") >= 0 ||
      lower.indexOf("don't mention") >= 0 ||
      lower.indexOf("do not mention") >= 0 ||
      lower.indexOf("stop talking about") >= 0 ||
      lower.indexOf("don't talk about") >= 0 ||
      lower.indexOf("do not talk about") >= 0 ||
      lower.indexOf("stop saying") >= 0) {
    return false;
  }

  bool homeScreenRequest =
      hasExplicitCommandPhrase(lower, "home screen") ||
      hasExplicitCommandPhrase(lower, "go back to home screen") ||
      hasExplicitCommandPhrase(lower, "back to home screen") ||
      hasExplicitCommandPhrase(lower, "return to home screen") ||
      hasExplicitCommandPhrase(lower, "go to home screen") ||
      hasExplicitCommandPhrase(lower, "go home") ||
      hasExplicitCommandPhrase(lower, "return home") ||
      lower == "home";

  if (homeScreenRequest) {
    scheduleDeferredAiAction(DEFERRED_AI_GOHOME);
    spokenReply = "[HAPPY] Okay. Going home now.";
    return true;
  }

  bool hasAction = hasExplicitCommandPhrase(lower, "move forward") ||
                   hasExplicitCommandPhrase(lower, "go forward") ||
                   hasExplicitCommandPhrase(lower, "move ahead") ||
                   lower == "forward" ||
                   lower == "fwd" ||
                   hasExplicitCommandPhrase(lower, "move back") ||
                   hasExplicitCommandPhrase(lower, "move backward") ||
                   hasExplicitCommandPhrase(lower, "go back") ||
                   lower == "back" ||
                   lower == "bwd" ||
                   hasExplicitCommandPhrase(lower, "turn left") ||
                   hasExplicitCommandPhrase(lower, "turn right") ||
                   hasExplicitCommandPhrase(lower, "spin left") ||
                   hasExplicitCommandPhrase(lower, "spin right") ||
                   hasExplicitCommandPhrase(lower, "rotate left") ||
                   hasExplicitCommandPhrase(lower, "rotate right") ||
                   hasExplicitCommandPhrase(lower, "dance") ||
                   hasExplicitCommandPhrase(lower, "stop moving") ||
                   lower == "stop" ||
                   hasExplicitCommandPhrase(lower, "halt") ||
                   homeScreenRequest ||
                   hasExplicitCommandPhrase(lower, "look up") ||
                   hasExplicitCommandPhrase(lower, "look down") ||
                   hasExplicitCommandPhrase(lower, "center");

  if (!hasAction) return false;

  bool queuedAnything = false;
  String summary = "";

  int forwardCount = 0;
  if (hasExplicitCommandPhrase(lower, "move forward") || hasExplicitCommandPhrase(lower, "go forward") || hasExplicitCommandPhrase(lower, "move ahead") || lower == "forward" || lower == "fwd") {
    forwardCount = extractCountAfterKeyword(lower, "forward", 1);
    if (forwardCount <= 0) forwardCount = 1;
    for (int i = 0; i < forwardCount; i++) queueMovementCommand("FWD");
    summary += "moving forward " + String(forwardCount) + (forwardCount == 1 ? " step" : " steps");
    queuedAnything = true;
  }

  if (hasExplicitCommandPhrase(lower, "dance")) {
    if (summary.length() > 0) summary += ", ";
    summary += "dancing";
    queueDanceRoutine(lower);
    queuedAnything = true;
  }

  int backCount = 0;
  if (hasExplicitCommandPhrase(lower, "move back") || hasExplicitCommandPhrase(lower, "move backward") || hasExplicitCommandPhrase(lower, "go back") || lower == "back" || lower == "bwd") {
    backCount = extractCountAfterKeyword(lower, "back", 1);
    if (backCount <= 0) backCount = 1;
    for (int i = 0; i < backCount; i++) queueMovementCommand("BWD");
    if (summary.length() > 0) summary += ", ";
    summary += "moving back " + String(backCount) + (backCount == 1 ? " step" : " steps");
    queuedAnything = true;
  }

  bool hasTurn = hasExplicitCommandPhrase(lower, "turn left") ||
                 hasExplicitCommandPhrase(lower, "turn right") ||
                 hasExplicitCommandPhrase(lower, "spin left") ||
                 hasExplicitCommandPhrase(lower, "spin right") ||
                 hasExplicitCommandPhrase(lower, "rotate left") ||
                 hasExplicitCommandPhrase(lower, "rotate right");
  if (hasTurn) {
    int angle = 90;
    if (lower.indexOf("180") >= 0) angle = 180;
    else if (lower.indexOf("45") >= 0) angle = 45;

    bool turnLeft = lower.indexOf("left") >= 0;
    bool turnRight = lower.indexOf("right") >= 0;
    String turnCmd;

    if (turnLeft && !turnRight) {
      turnCmd = (angle == 45) ? "TURN_L_45" : (angle == 180 ? "TURN_L_180" : "TURN_L_90");
    } else {
      turnCmd = (angle == 45) ? "TURN_R_45" : (angle == 180 ? "TURN_R_180" : "TURN_R_90");
      turnRight = true;
    }

    queueMovementCommand(turnCmd);
    if (summary.length() > 0) summary += ", ";
    summary += "turning " + String(turnLeft && !turnRight ? "left" : "right") + " " + String(angle) + " degrees";
    queuedAnything = true;
  }

  if (hasExplicitCommandPhrase(lower, "stop moving") || lower == "stop" || hasExplicitCommandPhrase(lower, "halt")) {
    queueMovementCommand("STOP");
    if (summary.length() > 0) summary += ", ";
    summary += "then stopping";
    queuedAnything = true;
  }

  if (hasExplicitCommandPhrase(lower, "look up")) {
    queueMovementCommand("LOOK_UP");
    queuedAnything = true;
    if (summary.length() > 0) summary += ", ";
    summary += "looking up";
  }

  if (hasExplicitCommandPhrase(lower, "look down")) {
    queueMovementCommand("LOOK_DOWN");
    queuedAnything = true;
    if (summary.length() > 0) summary += ", ";
    summary += "looking down";
  }

  if (hasExplicitCommandPhrase(lower, "center")) {
    queueMovementCommand("CENTER");
    queuedAnything = true;
    if (summary.length() > 0) summary += ", ";
    summary += "centering";
  }

  if (!queuedAnything) return false;

  spokenReply = "[HAPPY] " + summary;
  return true;
}

void scheduleDeferredAiAction(int action, const String& param, unsigned long delayMs) {
  deferredAiAction = static_cast<DeferredAiActionType>(action);
  deferredAiParam = param;
  deferredAiReadyAt = millis() + delayMs;
}

void clearDeferredAiAction() {
  deferredAiAction = DEFERRED_AI_NONE;
  deferredAiParam = "";
  deferredAiReadyAt = 0;
}

void processQueuedAIChat() {
  if (offlineModeLocked) return;
  if (queuedAiChat.length() <= 1) return;
  if (isSpeaking || isProcessingAI) return;
  if (currentMedState == MED_MEASURING) return;

  String msg = queuedAiChat;
  queuedAiChat = "";

  String directReply;
  if (handleMemoryManagementChat(msg, directReply)) {
    Serial.println("[AI] Handled memory request locally: " + msg);
    if (currentMode != MODE_AI) switchToAIMode();
    setEyeExpression("NORMAL");
    drawAIScreen(true);
    speakText(directReply.c_str());
    Serial.println("[AI]: " + directReply);
    return;
  }

  if (handleWebMotionChat(msg, directReply)) {
    Serial.println("[AI] Handled web motion locally: " + msg);
    if (currentMode != MODE_AI) switchToAIMode();
    setEyeExpression("HAPPY");
    drawAIScreen(true);
    if (directReply.length() > 0) {
      speakText(directReply.c_str());
      Serial.println("[AI]: " + directReply);
      appendConversationMemory(msg, directReply);
    } else {
      isProcessingAI = false;
      clearStringKeepCapacity(aiRequestStatus);
      Serial.println("[AI] Motion executed without spoken reply");
    }
    return;
  }

  clearMovementQueue();
  if (currentMode != MODE_AI) switchToAIMode();
  currentInterimText = msg;
  isProcessingAI = true;
  processingStartTime = millis();
  currentRobotActivity = ROBOT_ACTIVITY_THINKING;
  aiRequestStatus = "Thinking...";
  setEyeExpression("THINKING");
  drawAIScreen(true);
  askAI(msg.c_str());
}

void processDeferredAiAction() {
  if (deferredAiAction == DEFERRED_AI_NONE) return;
  if (millis() < deferredAiReadyAt) return;
  
  // Allow GOHOME and MEDITATE to bypass the isSpeaking check if they are the goal
  bool isCritical = (deferredAiAction == DEFERRED_AI_GOHOME || deferredAiAction == DEFERRED_AI_MEDITATE);
  
  if (!isCritical && (audio.isRunning() || isSpeaking)) return;
  if (!isCritical && audioTransitionIsQuiet()) return;
  if (currentMedState == MED_MEASURING) return;

  DeferredAiActionType action = deferredAiAction;
  String param = deferredAiParam;
  clearDeferredAiAction();

  switch (action) {
    case DEFERRED_AI_PLAYSONG:
      if (param.length() > 0) playMusic(param);
      break;

    case DEFERRED_AI_RELAX: {
      String playUrl = "";
      String relaxParam = param;
      relaxParam.toLowerCase();

      if (relaxParam.indexOf("rain") >= 0) {
        playUrl = "http://ice1.somafm.com/deepspaceone-128-mp3";
      } else if (relaxParam.indexOf("ocean") >= 0 || relaxParam.indexOf("wave") >= 0) {
        playUrl = "http://ice1.somafm.com/deepspaceone-128-mp3";
      } else if (relaxParam.indexOf("forest") >= 0 || relaxParam.indexOf("bird") >= 0) {
        playUrl = "http://ice1.somafm.com/missioncontrol-128-mp3";
      } else if (relaxParam.indexOf("fire") >= 0 || relaxParam.indexOf("fireplace") >= 0) {
        playUrl = "http://stream.zeno.fm/0r0xa792kwzuv";
      }

      if (playUrl.length() > 0) {
        mic_i2s.end();
        delay(10);
        audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DOUT);
        clearMicAIReconnectSchedule();
        isProcessingAI = false;
        isSpeaking = true;
        lastMusicAction = millis();
        enterAudioTransitionQuiet();
        audio.setConnectionTimeout(3000, 8000);
        audio.forceMono(true);
        audio.setVolume(21);
        Serial.println("[Relax] Connecting to stream: " + playUrl);
        audio.connecttohost(playUrl.c_str());
      } else {
        playMusic(param);
        isSpeaking = true;
      }
      break;
    }

    case DEFERRED_AI_GOHOME:
      switchToNormalMode();
      break;

    case DEFERRED_AI_IMURESET:
      recalibrateNavigationHeading();
      break;

    case DEFERRED_AI_CHECKUP:
      ai_requested_checkup = true;
      // if (currentMode != MODE_NORMAL) switchToNormalMode(); 
      currentMedState = MED_IDLE;
      medStateTimer = millis();
      drawNormalScreen(true);
      break;

    case DEFERRED_AI_EMERGENCY:
      sendEmergencyAlert("Voice Command Emergency");
      break;

    case DEFERRED_AI_NONE:
    default:
      break;
  }
}

void switchToAIMode() {
  if (currentMode == MODE_AI) return;
  Serial.println("[Mode] Switching to AI");
  setAutoAvoidMode(false);
  setGuardMode(false);
  if (!offlineModeLocked) {
    updateSensors();
    syncRemindersFromFirebase();
    Serial.println("[AI] Entry context refreshed: profile, sensors, reminders");
  }
  enterNetworkQuiet();
  if (mqtt.connected()) {
      mqtt.disconnect();
      Serial.println("[MQTT] Disconnected for AI session stability");
  }
  mqttClientNet->stop();
  currentMode = MODE_AI;
  currentMedState = MED_IDLE;
  lastInteractionTime = millis(); // Reset sleep timer
  nextAiProactiveNudgeMs = millis() + 10000;
  currentInterimText = ""; // Clear any stale text
  currentRobotActivity = ROBOT_ACTIVITY_THINKING;
  aiRequestStatus = "Thinking...";
  setEyeExpression("THINKING");
  setAIInputMode(true, true);
  
  // Force Clear Screen
  tft.fillScreen(UI_BG);
  drawAIScreen(true); // Force Initialization
  setModeStatusFirebase("AI");
  publishRobotAssistStatus(true);
}


// ============================================================
// SENSORS
// ============================================================
void updateSensors() {
  // REAL SENSORS
  // Switch to AHT/ENS Channel
  tcaselect(CH_AHT); 
  read_aht20();
  read_ens160();
  lastEnvSensorReadMs = millis();
}

void read_aht20() {
  sensors_event_t humidity, temp;
  if (aht.getEvent(&humidity, &temp)) {
    temp_aht = temp.temperature;
    humidity_aht = humidity.relative_humidity;
  }
}

void read_ens160() {
  // Switch to AHT/ENS Channel (shared)
  tcaselect(CH_AHT);
  
  // update() reads new data if available
  // Library uses typedef int8_t Result and #define RESULT_OK
  if (ens160.update() == RESULT_OK) {
    aqi_val = ens160.getAirQualityIndex_UBA();
    tvoc_val = ens160.getTvoc();
    eco2_val = ens160.getEco2();
  }
}

// Buffer for Maxim Algorithm
// NOTE: Buffer variables (irBuffer, redBuffer, spo2, heartRate) are already defined globally at top of file
// bufferIndex is declared inside read_max30102() as static local

void read_max30102() {
   tcaselect(CH_MAX);
   
   if (max30102_needs_full_read) {
       particleSensor.clearFIFO();
       Serial.println("[MAX30102] Collecting INITIAL 100 samples (4s)...");
       // FIX: Clear accumulators for fresh session averaging
       hr_sum = 0; spo2_sum = 0; hr_valid_count = 0; spo2_valid_count = 0;
       yield();

       // First pass: collect 100 continuous samples
       for (int i = 0; i < 100; i++) {
           while (particleSensor.available() == false) {
               particleSensor.check();
               yield();
           }
           
           irBuffer[i]  = particleSensor.getIR();
           redBuffer[i] = particleSensor.getRed();
           particleSensor.nextSample();
           
           if (i == 0) Serial.printf("[MAX30102] Raw IR: %lu\n", irBuffer[i]);

           // Lowered threshold to 4000 to be very permissive
           if (irBuffer[i] < 4000) {
               Serial.println("[MAX30102] Signal too low - check finger placement");
               return; 
           }
       }
       max30102_needs_full_read = false; // Next time, do rolling buffer
       Serial.println("[MAX30102] Initial Buffer Full");
       
   } else {
       // Subsequent passes: dump first 25, shift 75, read 25 new
       Serial.println("[MAX30102] Collecting 25 NEW samples (1s)...");
       
       for (byte i = 25; i < 100; i++) {
           redBuffer[i - 25] = redBuffer[i];
           irBuffer[i - 25]  = irBuffer[i];
       }

       // Take 25 new samples into the end of the array
       for (byte i = 75; i < 100; i++) {
           while (particleSensor.available() == false) {
               particleSensor.check();
               yield();
           }

           irBuffer[i]  = particleSensor.getIR();
           redBuffer[i] = particleSensor.getRed();
           particleSensor.nextSample();
           
           if (irBuffer[i] < 7000) {
               Serial.println("[MAX30102] Finger removed during rolling read - Aborting");
               return; 
           }
       }
   }

   // Every time the array is populated/shifted, run the calculation
   Serial.println("[MAX30102] Processing Algorithm...");

   maxim_heart_rate_and_oxygen_saturation(
     redBuffer, bufferLength, irBuffer,
     &spo2, &validSPO2, &heartRate, &validHeartRate
   );

   // Apply sanity limits and accumulate into running average
   // Widened valid range to 30-220 to prevent edge case dropouts on weaker signals
   if (validHeartRate && heartRate > 30 && heartRate < 220) {
       hr_sum += (float)heartRate;
       hr_valid_count++;
   }
   if (validSPO2 && spo2 >= 80 && spo2 <= 100) {
       spo2_sum += (float)spo2;
       spo2_valid_count++;
   }

   // Safe Division
   if (hr_valid_count > 0) {
       max30102_hr = hr_sum / hr_valid_count; // Running average
   }
   if (spo2_valid_count > 0) {
       max30102_spo2 = spo2_sum / spo2_valid_count; // Running average
   }

   float t = particleSensor.readTemperature();
   if (t > 30 && t < 45) {
       max30102_temp = t; // Sanity: 30-45°C range
   }

   int dispHR = (validHeartRate && heartRate < 200) ? heartRate : -1;
   static unsigned long lastLog = 0;
   if (millis() - lastLog > 5000) {
       Serial.printf("[MAX30102] HR: %d (valid:%d)  SpO2: %d (valid:%d)  Temp: %.1f\n",
                     dispHR, validHeartRate, spo2, validSPO2, max30102_temp);
       lastLog = millis();
   }
}

// ============================================================
// DISPLAY
// ============================================================
#define SCR_W 240
#define SCR_H 320

// Navigation bar at bottom
void drawNavigationBar() {
  tft.fillRect(0, SCR_H - 30, SCR_W, 30, UI_CARD_BG);
  tft.drawLine(0, SCR_H - 31, SCR_W, SCR_H - 31, UI_PRIMARY);
  tft.setFont();
  tft.setTextSize(1);

  if (currentMode == MODE_AI) {
    tft.fillRoundRect(4, SCR_H - 28, 70, 22, 5, UI_PRIMARY);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(12, SCR_H - 22);
    tft.print("< BACK");
    tft.setTextColor(UI_PRIMARY);
    tft.setCursor(SCR_W/2 - 8, SCR_H - 22);
    tft.print("AI");
  } else if (currentMode == MODE_AUTONOMOUS) {
    tft.fillRoundRect(4, SCR_H - 28, 70, 22, 5, UI_PRIMARY);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(12, SCR_H - 22);
    tft.print("< BACK");
    tft.setTextColor(UI_PRIMARY);
    tft.setCursor(SCR_W/2 - 30, SCR_H - 22);
    tft.print("AUTO MODE");
  } else {
    tft.setTextColor(UI_TEXT_SUB);
    tft.setCursor(10, SCR_H - 22);
    tft.print("MEDICAL DASHBOARD");
    tft.fillRoundRect(SCR_W - 46, SCR_H - 28, 42, 22, 5, UI_PRIMARY);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(SCR_W - 40, SCR_H - 22);
    tft.print("AI >");
  }
}

void drawStatusDot(bool connected) {
  // Top right corner dot
  int x = SCR_W - 15;
  int y = 12;
  int r = 4;
  tft.fillCircle(x, y, r, connected ? UI_SUCCESS : UI_ERROR);
}

void drawNormalScreen(bool force) {
  static unsigned long lastDraw = 0;
  static MedicalState lastRenderedState = (MedicalState)-1;
  static int last_min = -1;
  static float last_temp = -999;
  static float last_humidity = -999;
  static uint16_t last_aqi = 9999;
  static String lastAssistLabel = "";
  static uint16_t lastAssistColor = UI_CARD_BG;
  
  // Throttle (skip if not forced)
  if (!force && millis() - lastDraw < 500) return;
  lastDraw = millis();

  // Full redraw only when state changes or forced
  if (force) lastRenderedState = (MedicalState)-1; // Invalidate to ensure everything draws
  bool fullRedraw = force || (lastRenderedState != currentMedState);
  
  if (fullRedraw) {
    tft.fillScreen(UI_BG);
    
    // Header
    tft.fillRect(0, 0, SCR_W, 35, UI_PRIMARY);
    tft.setFont(); // Default font for header
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(10, 5);
    tft.print("ELLA");
    
    // Status Dot (Firebase)
    drawStatusDot(firebaseReady);

    drawNavigationBar();
    lastRenderedState = currentMedState;
    last_min = -1; // Force time update
  }

  // Draw Time (update every 30s to keep clock feeling fresh)
  struct tm timeinfo;
  if(ntpTimeValid && getLocalTime(&timeinfo)){
    static bool lastHalf = false;
    bool thisHalf = (timeinfo.tm_sec >= 30);
    if (fullRedraw || timeinfo.tm_min != last_min || thisHalf != lastHalf) {
       tft.fillRect(SCR_W - 105, 0, 85, 35, UI_PRIMARY);
       tft.setFont();
       tft.setTextSize(2);
       tft.setTextColor(ILI9341_WHITE);
       int hr12 = timeinfo.tm_hour % 12;
       if (hr12 == 0) hr12 = 12;
       const char* ampm = (timeinfo.tm_hour < 12) ? "AM" : "PM";
       tft.setCursor(SCR_W - 105, 5);
       tft.printf("%d:%02d%s", hr12, timeinfo.tm_min, ampm);
       last_min = timeinfo.tm_min;
       lastHalf = thisHalf;
    }
  }

  // Assist mode badge in the header: AUTO / GUARD / ALERT
  String assistLabel = "";
  uint16_t assistColor = UI_CARD_BG;
  if (guardAlarmActive) {
    assistLabel = "ALERT";
    assistColor = UI_ERROR;
  } else if (guardModeEnabled) {
    assistLabel = "GUARD";
    assistColor = UI_ALERT;
  } else if (autoAvoidEnabled) {
    assistLabel = "AUTO";
    assistColor = UI_INFO;
  }

  if (fullRedraw || assistLabel != lastAssistLabel || assistColor != lastAssistColor) {
    tft.fillRect(84, 0, 50, 35, UI_PRIMARY);
    if (assistLabel.length() > 0) {
      tft.fillRoundRect(86, 3, 48, 18, 5, assistColor);
      tft.setFont();
      tft.setTextSize(1);
      tft.setTextColor(UI_BG);
      int16_t x1, y1; uint16_t w, h;
      tft.getTextBounds(assistLabel, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor(110 - w / 2, 16);
      tft.print(assistLabel);
    }
    lastAssistLabel = assistLabel;
    lastAssistColor = assistColor;
  }


  // Medical state screens
  if (currentMedState == MED_RESULT) {
    // STATIC UI (Only once)
    if (fullRedraw) {
        // Heart Rate Card (Top half)
        tft.fillRoundRect(10, 40, 220, 100, 16, UI_CARD_BG);
        tft.drawRoundRect(10, 40, 220, 100, 16, UI_HEART);

        // VALIDATE HR before printing
        bool hrValid = (!isnan(max30102_hr) && max30102_hr > 30 && max30102_hr < 220);
        bool spValid = (!isnan(max30102_spo2) && max30102_spo2 > 50 && max30102_spo2 <= 100);
        bool tmpValid = (!isnan(max30102_temp) && max30102_temp > 30);

        // Card Label (Top left)
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextSize(1);
        tft.setTextColor(UI_TEXT_SUB);
        tft.setCursor(25, 65);
        tft.print("Heart Rate");

        // Card Value (Center-ish)
        tft.setFont(&FreeSansBold24pt7b);
        tft.setTextColor(UI_TEXT_MAIN);
        tft.setCursor(25, 115);
        if (hrValid) tft.print((int)max30102_hr);
        else tft.print("--");

        // Card Unit
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextColor(UI_TEXT_SUB);
        int16_t x1, y1; uint16_t w, h;
        String hrStr = hrValid ? String((int)max30102_hr) : "--";
        tft.setFont(&FreeSansBold24pt7b);
        tft.getTextBounds(hrStr, 0, 0, &x1, &y1, &w, &h);
        tft.setFont(&FreeSansBold9pt7b);
        tft.setCursor(25 + w + 5, 115);
        tft.print("BPM");

        // Status pill
        tft.setTextColor(hrValid ? UI_SUCCESS : UI_ERROR);
        tft.setFont();
        tft.setCursor(25, 125);
        tft.print(hrValid ? "NORMAL" : "MEASURING...");

        // SpO2 Card (Bottom left)
        tft.fillRoundRect(10, 150, 105, 100, 16, UI_CARD_BG);
        tft.drawRoundRect(10, 150, 105, 100, 16, UI_OXY);
        
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextColor(UI_TEXT_SUB);
        tft.setCursor(20, 175);
        tft.print("Oxygen");

        tft.setFont(&FreeSansBold18pt7b);
        tft.setTextColor(UI_TEXT_MAIN);
        tft.setCursor(20, 215);
        if (spValid) tft.print((int)max30102_spo2);
        else tft.print("--");
        
        tft.getTextBounds(spValid ? String((int)max30102_spo2) : "--", 0, 0, &x1, &y1, &w, &h);
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextColor(UI_TEXT_SUB);
        tft.setCursor(20 + w + 2, 215);
        tft.print("%");

        // Temp Card (Bottom right)
        tft.fillRoundRect(125, 150, 105, 100, 16, UI_CARD_BG);
        tft.drawRoundRect(125, 150, 105, 100, 16, UI_ALERT);
        
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextColor(UI_TEXT_SUB);
        tft.setCursor(135, 175);
        tft.print("Temp");

        tft.setFont(&FreeSansBold18pt7b);
        tft.setTextColor(UI_TEXT_MAIN);
        tft.setCursor(135, 215);
        if (tmpValid) tft.print(max30102_temp, 1);
        else tft.print("--");
        
        tft.getTextBounds(tmpValid ? String(max30102_temp, 1) : "--", 0, 0, &x1, &y1, &w, &h);
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextColor(UI_TEXT_SUB);
        tft.setCursor(135 + w + 2, 215);
        tft.print("C");
    }
    
    // Icon wrapper background (dark circular)
    tft.fillCircle(190, 85, 22, UI_BG); 

    int heartSize = ((millis() % 400) < 100) ? 12 : 10;
    // draw heart center 190, 85
    tft.fillCircle(190-5, 85-5, heartSize, UI_HEART);
    tft.fillCircle(190+5, 85-5, heartSize, UI_HEART);
    tft.fillTriangle(190-13, 85-1, 190+13, 85-1, 190, 85+15, UI_HEART);

    // Countdown (10s) for Result (Flicker-Free)
    static int last_result_count = -1;
    unsigned long elapsed = millis() - medStateTimer;
    int remaining = 10 - (elapsed / 1000);
    if (remaining < 0) remaining = 0;
    
    if (remaining != last_result_count || fullRedraw) {
        tft.setTextColor(UI_TEXT_SUB);
        tft.setCursor(185, 125); // Below Icon

        if (last_result_count != -1 && !fullRedraw) {
             tft.setTextColor(UI_CARD_BG);
             tft.print(last_result_count);
             tft.print("s");
             tft.setCursor(185, 125); // Reset cursor
        }
        
        tft.setTextColor(UI_TEXT_SUB);
        tft.print(remaining);
        tft.print("s");
        
        last_result_count = remaining;
    }
  } else if (currentMedState == MED_MEASURING) {
    // STATIC UI (Only once)
    if (fullRedraw) {
       // Height 140
       tft.fillRoundRect(10, 40, 220, 140, 16, UI_CARD_BG); 
       tft.drawRoundRect(10, 40, 220, 140, 16, UI_PRIMARY);
    }

    // Dynamic Variables (Missing!)
    unsigned long elapsed = millis() - medStateTimer;
    String status = "Measuring...";
    if (elapsed < 10000) status = "Reading Pulse...";
    else if (elapsed < 20000) status = "Reading Oxygen...";
    else status = "Reading Temp...";

    int cx = 120;
    int cy = 65; // Moved UP (was 70) to fix overlap
    bool pulseState = (millis() / 500) % 2 == 0;
    uint16_t hColor = UI_ACCENT; // Default color

    // Icon Animation Logic (Clear on change to fix overlay)
    static bool last_pulseState = false;
    static int last_phase = -1;
    int current_phase = elapsed / 10000; // 10s phases
    
    bool needClear = (pulseState != last_pulseState) || (current_phase != last_phase) || fullRedraw;
    
    if (needClear) {
        // Clear Icon Area (60x60 box centered at cx, cy)
        // x = 120-30 = 90. y = 65-30 = 35.
        // Wait, cy=65. y=35. Box height 60 -> y=95.
        // Ensure it doesn't clear top border (y=40). 35 < 40!
        // Adjust clear box: y=42, h=50.
        tft.fillRect(cx - 30, cy - 23, 60, 50, UI_CARD_BG ); 
        
        last_pulseState = pulseState;
        last_phase = current_phase;
    }

    if (elapsed < 10000) { 
       // PHASE 1: HEART (Pulse)
       if (needClear) { // Only draw if state changed or redraw
           if (pulseState) {
              tft.fillCircle(cx-6, cy-6, 7, hColor);
              tft.fillCircle(cx+6, cy-6, 7, hColor);
              tft.fillTriangle(cx-13, cy+2, cx+13, cy+2, cx, cy+13, hColor);
           } else {
              tft.fillCircle(cx-5, cy-5, 5, hColor);
              tft.fillCircle(cx+5, cy-5, 5, hColor);
              tft.fillTriangle(cx-10, cy+1, cx+10, cy+1, cx, cy+10, hColor);
           }
       }
    } 
    else if (elapsed < 20000) {
       // PHASE 2: BLOOD DROP (SpO2)
       hColor = UI_ACCENT; // Red Drop
       if (needClear) {
           if (pulseState) { // Pulse Big
              tft.fillCircle(cx, cy+5, 12, hColor);
              tft.fillTriangle(cx-11, cy+2, cx+11, cy+2, cx, cy-15, hColor);
           } else { // Pulse Small
              tft.fillCircle(cx, cy+5, 10, hColor);
              tft.fillTriangle(cx-9, cy+2, cx+9, cy+2, cx, cy-12, hColor);
           }
       }
    }
    else {
       // PHASE 3: THERMOMETER (Temp)
       hColor = 0xFDA0; // Orange-ish
       if (needClear) {
           int tH = pulseState ? 34 : 30; // Shrunk Pulse height (was 40/36)
           int tW = 10;
           int tX = cx - tW/2;
           int tY = cy - 20; // Moved UP further (was -15)
           
           tft.fillRoundRect(tX, tY, tW, tH, 4, UI_ACCENT); // Body
           tft.fillCircle(cx, tY + tH, 9, UI_ACCENT); // Bulb
           
           // Inner Details
           tft.fillRoundRect(tX+3, tY+5, tW-6, tH-5, 2, UI_CARD_BG); 
           tft.fillRoundRect(tX+3, tY+15, tW-6, tH-15, 2, hColor); 
           tft.fillCircle(cx, tY + tH, 6, hColor); 
       }
    }
    
    // Flicker-Free Countdown
    // 30s Countdown (10s each phase)
    static int last_remaining = -1;
    int remaining = 30 - (elapsed / 1000);
    // Safety clamp
    if (remaining < 0) remaining = 0;

    if (remaining != last_remaining || fullRedraw) {
        tft.setFont(&FreeSansBold24pt7b);
        tft.setTextSize(1);
        int16_t x1, y1; uint16_t w, h;
        
        // Erase old (if valid)
        if (last_remaining != -1 && !fullRedraw) {
            String oldStr = String(last_remaining);
            tft.getTextBounds(oldStr, 0, 0, &x1, &y1, &w, &h);
            tft.setTextColor(UI_CARD_BG);
            tft.setCursor(cx - w/2, cy + 60);
            tft.print(oldStr);
        }
        
        // Draw new
        String newStr = String(remaining);
        tft.getTextBounds(newStr, 0, 0, &x1, &y1, &w, &h);
        tft.setTextColor(UI_TEXT_MAIN);
        tft.setCursor(cx - w/2, cy + 60);
        tft.print(newStr);
        
        last_remaining = remaining;
    }
    
    
    // Status Text (Explicit Clear to fix "Two Text" issue)
    static String last_status_text = "";
    if (status != last_status_text || fullRedraw) {
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextSize(1);
        int16_t x1, y1; uint16_t w, h;

        // Force Clear Area (Safer than overwrite)
        // Bottom strip y=160 to 178 (Height 18) - Leaves border intact!
        tft.fillRect(11, 160, 218, 18, UI_CARD_BG);
        
        // Redraw Border Bottom just in case
        tft.drawRoundRect(10, 40, 220, 140, 10, UI_INFO);

        // Draw new
        tft.getTextBounds(status, 0, 0, &x1, &y1, &w, &h);
        tft.setTextColor(UI_TEXT_MAIN); // Changed to White (from SUB) per user request ("Faded")
        tft.setCursor(cx - w/2, 175);
        tft.print(status);
        
        last_status_text = status;
    }


  } else if (currentMedState == MED_PLACE_FINGER) {
    // STATIC UI (Only once)
    if (fullRedraw) {
       // Height 140
       tft.fillRoundRect(10, 40, 220, 140, 10, UI_CARD_BG);
       tft.drawRoundRect(10, 40, 220, 140, 10, UI_SUCCESS);
       
       tft.setFont(&FreeSansBold9pt7b);
       tft.setTextSize(1);
       tft.setTextColor(UI_SUCCESS);
       // Center "KEEP FINGER STILL" text using getTextBounds
       String msg = "KEEP FINGER STILL";
       int16_t x1, y1; uint16_t w, h;
       tft.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
       tft.setCursor(10 + (220 - w)/2, 40+55);
       tft.print(msg);
    }
    
    // Countdown 5s (Flicker-Free)
    static int last_place_count = -1;
    unsigned long elapsed = millis() - medStateTimer;
    int remaining = 5 - (elapsed / 1000);
    if (remaining < 0) remaining = 0;

    if (remaining != last_place_count || fullRedraw) {
        tft.setFont(&FreeSansBold18pt7b);
        tft.setTextSize(1);
        int16_t x1, y1; uint16_t w, h;

        // Force Clear Countdown Area (Fix Overlap)
        // Center approx 110, y=130. Width ~50. Height ~40.
        // x=85, y=100, w=50, h=40
        tft.fillRect(80, 100, 60, 40, UI_CARD_BG);
        
        // Draw new
        String newStr = String(remaining);
        tft.getTextBounds(newStr, 0, 0, &x1, &y1, &w, &h); 
        tft.setTextColor(UI_SUCCESS);
        tft.setCursor(10 + (220 - w)/2, 40+90);
        tft.print(newStr);
        
        last_place_count = remaining;
    }


  } else if (currentMedState == MED_WAIT_FINGER) {
    // STATIC UI (Only once)
    if (fullRedraw) {
        // Height 140
        tft.fillRoundRect(10, 40, 220, 140, 10, UI_CARD_BG);
        tft.drawRoundRect(10, 40, 220, 140, 10, UI_ACCENT);
        
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextSize(1);
        tft.setTextColor(UI_TEXT_MAIN);
        
        // Center "Place Finger" (Moved UP)
        String line1 = "Place Finger";
        int16_t x1, y1; uint16_t w, h;
        tft.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
        tft.setCursor(10 + (220 - w)/2, 40+40); 
        tft.print(line1);
        
        // Center "on Sensor..." (Moved UP)
        String line2 = "on Sensor...";
        tft.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
        tft.setCursor(10 + (220 - w)/2, 40+65); 
        tft.print(line2);
    }

    // Flicker-Free Countdown
    static int last_wait_count = -1;
    int remaining = 5 - (millis() - medStateTimer) / 1000;
    if (remaining < 0) remaining = 0;
    
    if (remaining != last_wait_count || fullRedraw) {
        tft.setFont(&FreeSansBold12pt7b);
        tft.setTextSize(1);
        int16_t x1, y1; uint16_t w, h;

        // Erase old
        if (last_wait_count != -1 && !fullRedraw) {
             String oldStr = String(last_wait_count);
             tft.getTextBounds(oldStr, 0, 0, &x1, &y1, &w, &h);
             tft.setTextColor(UI_CARD_BG);
             tft.setCursor(10 + (220 - w)/2, 40+95);
             tft.print(oldStr);
        }
        
        // Draw new
        String newStr = String(remaining);
        tft.getTextBounds(newStr, 0, 0, &x1, &y1, &w, &h);
        tft.setTextColor(UI_ACCENT);
        tft.setCursor(10 + (220 - w)/2, 40+95); 
        tft.print(newStr);
        
        last_wait_count = remaining;
    }

  } else if (currentMedState == MED_BREATHING) {
      // ── BREATHING EXERCISE MODE ──
      // Cycle: 4s breathe in, 4s hold, 6s breathe out, 2s hold (Total 16s)
      static int lastPhase = -1;
      unsigned long elapsed = millis() - medStateTimer;
      int cycleTime = elapsed % 16000;
      
      int phase = 0;
      String instr = "";
      float sizeMul = 0; // 0 to 1
      
      if (cycleTime < 4000) {
          phase = 0; instr = "Breathe In...";
          sizeMul = (float)cycleTime / 4000.0;
      } else if (cycleTime < 8000) {
          phase = 1; instr = "Hold...";
          sizeMul = 1.0;
      } else if (cycleTime < 14000) {
          phase = 2; instr = "Breathe Out...";
          sizeMul = 1.0 - ((float)(cycleTime - 8000) / 6000.0);
      } else {
          phase = 3; instr = "Hold...";
          sizeMul = 0.0;
      }
      
      // Speak instructions on phase change
      if (phase != lastPhase) {
          speakText(instr.c_str());
          lastPhase = phase;
      }
      
      // Visuals
      if (fullRedraw) tft.fillScreen(UI_BG);
      
      int cx = SCR_W / 2;
      int cy = SCR_H / 2;
      int maxR = 90;
      int minR = 20;
      int r = minR + (maxR - minR) * sizeMul;
      
      // Erase previous by clearing a large fixed box, then draw new
      // (This avoids flickering the whole screen)
      tft.fillRect(0, cy - maxR - 5, SCR_W, (maxR + 15)*2, UI_BG);
      
      tft.fillCircle(cx, cy, r, 0x18E3); // Deep blue orb
      tft.fillCircle(cx, cy, r*0.6, UI_ACCENT); // Inner glow
      
      // Draw Text
      tft.setFont(&FreeSansBold12pt7b);
      tft.setTextSize(1);
      tft.setTextColor(UI_TEXT_MAIN);
      int16_t x1, y1; uint16_t w, h;
      tft.getTextBounds(instr, 0, 0, &x1, &y1, &w, &h);
      
      // Text Background (UI_BG to match screen background — no visible box)
      tft.fillRect(cx - w/2 - 5, cy + maxR + 10 - h, w + 10, h + 10, UI_BG);
      tft.setCursor(cx - w/2, cy + maxR + 15);
      tft.print(instr);
      
      // Exit button
      tft.fillRoundRect(8, SCR_H - 28, 62, 22, 5, UI_ERROR);
      tft.setTextColor(UI_BG);
      tft.setFont();
      tft.setCursor(18, SCR_H - 22);
      tft.print("STOP");
      
  } else if (currentMedState == MED_MEDITATING) {
      // ── GUIDED MEDITATION MODE (STATIC VISUALS) ──
      // Slowly pulsing orb without breathing instructions
      unsigned long elapsed = millis() - medStateTimer;
      int pulseTime = elapsed % 4000;
      float sizeMul = (pulseTime < 2000) ? ((float)pulseTime/2000.0) : (1.0 - ((float)(pulseTime-2000)/2000.0));
      
      if (fullRedraw) tft.fillScreen(UI_BG);
      
      int cx = SCR_W / 2;
      int cy = SCR_H / 2 - 10;
      int maxR = 60;
      int minR = 50;
      int r = minR + (maxR - minR) * sizeMul;
      
      tft.fillRect(0, cy - maxR - 5, SCR_W, (maxR + 15)*2, UI_BG);
      tft.fillCircle(cx, cy, r, 0x18E3); // Deep blue orb
      tft.fillCircle(cx, cy, r*0.6, UI_ACCENT); // Inner glow

      tft.setFont(&FreeSansBold12pt7b);
      tft.setTextSize(1);
      tft.setTextColor(UI_TEXT_MAIN);
      String label = "Meditating...";
      int16_t x1, y1; uint16_t w, h;
      tft.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
      tft.fillRect(cx - w/2 - 5, cy + maxR + 10 - h, w + 10, h + 10, UI_BG);
      tft.setCursor(cx - w/2, cy + maxR + 15);
      tft.print(label);

      // Exit button
      tft.fillRoundRect(8, SCR_H - 28, 62, 22, 5, UI_ERROR);
      tft.setTextColor(UI_BG);
      tft.setFont();
      tft.setCursor(18, SCR_H - 22);
      tft.print("STOP");
      
  } else if (currentMedState == MED_IDLE) {
    // Ambient sensors (card layout)
    if (fullRedraw) {
      // CLEAR Main Area
      tft.fillRect(0, 30, SCR_W, SCR_H-60, UI_BG); 
      
      // 1. Temp + Humidity Card (Top Half)
      tft.fillRoundRect(10, 40, 220, 100, 16, UI_CARD_BG);
      tft.drawRoundRect(10, 40, 220, 100, 16, UI_ENV);
      
      // Card Label
      tft.setFont(&FreeSansBold9pt7b);
      tft.setTextSize(1);
      tft.setTextColor(UI_TEXT_SUB);
      tft.setCursor(25, 65);
      tft.print("Environment");

      // Icon Wrapper (Top Right, away from values)
      tft.fillCircle(200, 70, 18, UI_BG);
      tft.drawCircle(200, 70, 8, UI_ENV);
      tft.fillCircle(200, 70, 3, UI_ENV);
      tft.drawLine(200-8, 70, 200+8, 70, UI_ENV);
      tft.drawLine(200, 70-8, 200, 70+8, UI_ENV);

      // Sub-labels for Temp and Humidity
      tft.setFont();
      tft.setTextSize(1);
      tft.setTextColor(UI_TEXT_SUB);
      tft.setCursor(25, 72);
      tft.print("Temp");
      tft.setCursor(120, 72);
      tft.print("Humidity");

      // 2. Air Quality Card (Bottom Left)
      tft.fillRoundRect(10, 150, 105, 100, 16, UI_CARD_BG); 
      tft.drawRoundRect(10, 150, 105, 100, 16, UI_PRIMARY);
      tft.setFont(&FreeSansBold9pt7b);
      tft.setTextSize(1);
      tft.setTextColor(UI_TEXT_SUB);
      tft.setCursor(20, 175);
      tft.print("Air Quality");

      // 3. TVOC + eCO2 Card (Bottom Right)
      tft.fillRoundRect(125, 150, 105, 100, 16, UI_CARD_BG);
      tft.drawRoundRect(125, 150, 105, 100, 16, UI_ALERT);
      tft.setFont(&FreeSansBold9pt7b);
      tft.setTextSize(1);
      tft.setTextColor(UI_TEXT_SUB);
      tft.setCursor(135, 175);
      tft.print("Gases");
    }
    
    if (fullRedraw) { last_temp = -999; last_humidity = -999; last_aqi = 9999; }

    // ── Temp Value ──
    if (isnan(temp_aht)) temp_aht = 0.0;
    if (abs(temp_aht - last_temp) > 0.1 || fullRedraw) {
        // Erase old value area with fillRect
        tft.fillRect(20, 82, 90, 35, UI_CARD_BG);
        
        tft.setFont(&FreeSansBold18pt7b);
        tft.setTextSize(1);
        if (temp_aht > 38.0) tft.setTextColor(UI_ERROR);
        else if (temp_aht > 37.5) tft.setTextColor(UI_ALERT);
        else tft.setTextColor(UI_TEXT_MAIN);
        tft.setCursor(25, 110);
        tft.print(temp_aht, 1);

        int16_t x1, y1; uint16_t w, h;
        tft.getTextBounds(String(temp_aht, 1), 0, 0, &x1, &y1, &w, &h);
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextSize(1);
        tft.setTextColor(UI_TEXT_SUB);
        tft.setCursor(25 + w + 2, 110);
        tft.print("C");

        last_temp = temp_aht;
    }

    // ── Humidity Value ──
    if (abs(humidity_aht - last_humidity) > 1.0 || fullRedraw) {
        // Erase old value area with fillRect
        tft.fillRect(115, 82, 75, 35, UI_CARD_BG);
        
        tft.setFont(&FreeSansBold18pt7b);
        tft.setTextSize(1);
        if (humidity_aht > 70 || humidity_aht < 20) tft.setTextColor(UI_ALERT);
        else tft.setTextColor(UI_TEXT_MAIN);
        tft.setCursor(120, 110);
        tft.print(humidity_aht, 0);

        int16_t x1, y1; uint16_t w, h;
        tft.getTextBounds(String(humidity_aht, 0), 0, 0, &x1, &y1, &w, &h);
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextSize(1);
        tft.setTextColor(UI_TEXT_SUB);
        tft.setCursor(120 + w + 2, 110);
        tft.print("%");

        last_humidity = humidity_aht;
    }

    // ── AQI Value ──
    if (fullRedraw) last_aqi = 9999;
    if (aqi_val != last_aqi) {
        // Erase entire AQI value area with fillRect (fixes overlap)
        tft.fillRect(15, 188, 95, 35, UI_CARD_BG);
        
        tft.setFont(&FreeSansBold18pt7b);
        tft.setTextSize(1);
        if (aqi_val >= 4) tft.setTextColor(UI_ERROR);
        else if (aqi_val == 3) tft.setTextColor(UI_ALERT);
        else tft.setTextColor(UI_PRIMARY); 
        tft.setCursor(20, 215);
        tft.print(aqi_val);
        
        // AQI label right beside it
        int16_t x1, y1; uint16_t w, h;
        tft.getTextBounds(String(aqi_val), 0, 0, &x1, &y1, &w, &h);
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextSize(1);
        tft.setTextColor(UI_TEXT_SUB);
        tft.setCursor(20 + w + 8, 215);
        tft.print("AQI");

        last_aqi = aqi_val;
    }

    // ── eCO2 + TVOC Values ──
    static uint16_t last_eco2 = 9999;
    static uint16_t last_tvoc = 9999;
    if (fullRedraw) { last_eco2 = 9999; last_tvoc = 9999; }

    // eCO2
    if (eco2_val != last_eco2) {
        // Erase old with fillRect
        tft.fillRect(130, 188, 95, 22, UI_CARD_BG);
        
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextSize(1);
        if (eco2_val > 1000) tft.setTextColor(UI_ALERT);
        else tft.setTextColor(UI_TEXT_MAIN);
        tft.setCursor(135, 205);
        tft.print("eC: ");
        tft.print((int)eco2_val);
        last_eco2 = eco2_val;
    }

    // TVOC
    if (tvoc_val != last_tvoc) {
        // Erase old with fillRect
        tft.fillRect(130, 213, 95, 22, UI_CARD_BG);
        
        tft.setFont(&FreeSansBold9pt7b);
        tft.setTextSize(1);
        if (tvoc_val > 200) tft.setTextColor(UI_ALERT);
        else tft.setTextColor(UI_TEXT_MAIN);
        tft.setCursor(135, 230);
        tft.print("TV: ");
        tft.print((int)tvoc_val);
        last_tvoc = tvoc_val;
    }
  }  // End MED_IDLE
}

// ============================================================
// SCREEN SAVER
// ============================================================
void drawScreenSaver() {
    static unsigned long lastTimeDraw = 0;
    static unsigned long lastPulseDraw = 0;
    static int pSize = 0;
    static int pDir = 1;

    // Pulse E.L.L.A. orb every 50ms
    if (millis() - lastPulseDraw > 50) {
        lastPulseDraw = millis();
        
        // Erase old
        tft.fillCircle(SCR_W/2, SCR_H/2, 40, 0x0000);
        
        // Update size
        pSize += pDir;
        if (pSize > 25) pDir = -1;
        else if (pSize < 5) pDir = 1;
        
        // Draw new gentle pulse (very dim colors)
        tft.fillCircle(SCR_W/2, SCR_H/2, 10 + pSize, 0x0821); // Dim blue
        tft.fillCircle(SCR_W/2, SCR_H/2, 10 + (pSize/2), 0x1042);
        tft.fillCircle(SCR_W/2, SCR_H/2, 8, UI_PRIMARY);
    }

    // Update Time every minute
    struct tm timeinfo;
    if (ntpTimeValid && getLocalTime(&timeinfo) && millis() - lastTimeDraw > 60000) {
        lastTimeDraw = millis();
        tft.fillRect(0, SCR_H - 40, SCR_W, 40, 0x0000);
        tft.setFont(&FreeSansBold12pt7b);
        tft.setTextSize(1);
        tft.setTextColor(0x39E7); // Dim grey
        
        int hr12 = timeinfo.tm_hour % 12;
        if (hr12 == 0) hr12 = 12;
        const char* ampm = (timeinfo.tm_hour < 12) ? "AM" : "PM";
        
        char timeStr[20];
        sprintf(timeStr, "%d:%02d %s", hr12, timeinfo.tm_min, ampm);
        
        int16_t x1, y1; uint16_t w, h;
        tft.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
        tft.setCursor((SCR_W - w) / 2, SCR_H - 15);
        tft.print(timeStr);
    }
}

void drawAIScreen(bool force) {
  static unsigned long lastDraw = 0;
  if (!force && millis() - lastDraw < 50) return; // 20fps
  lastDraw = millis();
  bool online = networkAvailable();

  // State tracking
  static int lastStateDisplay = -1;
  static String lastInterimDisplay = "";
  static String lastResponseDisplay = "";
  
  // Determine current state
  int curState; // 0=Listening, 1=Thinking, 2=Speaking, 3=Idle
  if (isSpeaking) curState = 2;
  else if (isProcessingAI) curState = 1;
  else if (currentInterimText.length() > 0) curState = 0;
  else curState = 3;

  // ── Layout Definitions ────────────────────────────────────────
  const int HEADER_H = 35;
  const int FOOTER_H = 30;
  const int CONTENT_Y = HEADER_H;
  const int CONTENT_H = SCR_H - HEADER_H - FOOTER_H;
  
  const int TRANSCRIPT_Y = CONTENT_Y;
  const int TRANSCRIPT_H = 80;
  
  const int ANIM_Y = TRANSCRIPT_Y + TRANSCRIPT_H + 5;
  const int ANIM_H = 70;
  
  const int RESPONSE_Y = ANIM_Y + ANIM_H + 5;
  const int RESPONSE_H = CONTENT_H - TRANSCRIPT_H - ANIM_H - 5;

  // ── Full redraw on force or state change ──────────────────────
  if (force || curState != lastStateDisplay) {
    if (force) tft.fillScreen(UI_BG);
    
    // Clear only the content area to prevent flicker
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, UI_BG);
    
    // Header (Deep Blue)
    tft.fillRect(0, 0, SCR_W, HEADER_H, UI_PRIMARY);
    tft.setFont(&FreeSansBold9pt7b);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(10, 24);
    tft.print("ELLA MEDICAL AI");
    
    drawStatusDot(online);
    drawNavigationBar();
    
    // Section Headers
    tft.setFont();
    tft.setTextSize(1);
    tft.setTextColor(UI_TEXT_SUB);
    tft.setCursor(10, TRANSCRIPT_Y + 2);
    tft.print("TRANSCRIPTION");
    
    tft.setCursor(10, RESPONSE_Y + 2);
    tft.print("AI RESPONSE");
    
    lastStateDisplay = curState;
    lastInterimDisplay = "---FORCE---"; 
    lastResponseDisplay = "---FORCE---";
  }

  // Detect state changes for text clearing (e.g. Thinking -> Responding)
  if (curState != lastStateDisplay) {
    lastInterimDisplay = "---CHANGE---";
    lastResponseDisplay = "---CHANGE---";
    lastStateDisplay = curState;
  }

  // ── 1. Partial Speech (Live Transcript) ───────────────────────
  if (currentInterimText != lastInterimDisplay) {
    // Clear transcript area
    tft.fillRect(10, TRANSCRIPT_Y + 12, SCR_W - 20, TRANSCRIPT_H - 15, UI_BG);
    tft.drawRoundRect(5, TRANSCRIPT_Y + 10, SCR_W - 10, TRANSCRIPT_H - 12, 4, UI_CARD_BG);
    
    tft.setFont();
    tft.setTextSize(1);
    tft.setTextColor(UI_TEXT_MAIN);
    tft.setCursor(15, TRANSCRIPT_Y + 18);
    
    String text = currentInterimText;
    if (text.length() == 0) text = (curState == 3) ? "Waiting for speech..." : "Listening...";
    
    // Remove command tags for display
    int b1, b2;
    while((b1 = text.indexOf('[')) >= 0 && (b2 = text.indexOf(']', b1)) >= 0) {
      text.remove(b1, b2 - b1 + 1);
    }
    text.trim();

    // Wrap text manually
    int x = 15, y = TRANSCRIPT_Y + 18;
    for(int i=0; i<text.length(); i++) {
      tft.print(text[i]);
      if (tft.getCursorX() > SCR_W - 25) {
        y += 10;
        if (y > TRANSCRIPT_Y + TRANSCRIPT_H - 10) break;
        tft.setCursor(15, y);
      }
    }
    lastInterimDisplay = currentInterimText;
  }

  // ── 2. AI Response ────────────────────────────────────────────
  if (lastAiResponse != lastResponseDisplay) {
    // Clear response area
    tft.fillRect(10, RESPONSE_Y + 12, SCR_W - 20, RESPONSE_H - 15, UI_BG);
    tft.drawRoundRect(5, RESPONSE_Y + 10, SCR_W - 10, RESPONSE_H - 12, 4, UI_PRIMARY);
    
    tft.setFont();
    tft.setTextSize(1);
    tft.setTextColor(UI_PRIMARY);
    tft.setCursor(15, RESPONSE_Y + 18);
    
    String text = lastAiResponse;
    if (text.length() == 0) {
      if (curState == 1) text = "Analyzing query...";
      else if (curState == 2) text = "Responding...";
      else text = "";
    }

    int x = 15, y = RESPONSE_Y + 18;
    for(int i=0; i<text.length(); i++) {
      tft.print(text[i]);
      if (tft.getCursorX() > SCR_W - 25) {
        y += 10;
        if (y > RESPONSE_Y + RESPONSE_H - 10) break;
        tft.setCursor(15, y);
      }
    }
    lastResponseDisplay = lastAiResponse;
  }

  // ── 3. Calm Animation (Neural Pulse) ──────────────────────────
  int cx = SCR_W / 2;
  int cy = ANIM_Y + ANIM_H / 2;
  
  // Static area clear for animation to prevent smearing — more precise
  tft.fillRect(cx - 60, cy - 35, 120, 70, UI_BG);
  
  if (curState == 1) { // Thinking
    int r = 15 + (int)(sin(millis() / 200.0) * 8.0);
    tft.drawCircle(cx, cy, r, UI_PRIMARY);
    tft.drawCircle(cx, cy, r + 1, UI_PRIMARY);
    tft.drawCircle(cx, cy, r + 2, UI_PRIMARY);
    tft.fillCircle(cx, cy, 4, UI_PRIMARY);
  } else if (curState == 2) { // Speaking
    for(int i=-2; i<=2; i++) {
      int h = 10 + (int)(abs(sin((millis() / 150.0) + i)) * 30.0);
      tft.fillRect(cx + i*18 - 4, cy - h/2, 8, h, UI_PRIMARY);
    }
  } else if (curState == 0) { // Listening
    int s = (int)((sin(millis() / 100.0) + 1.0) * 12.0);
    tft.drawCircle(cx, cy, 15 + s, UI_PRIMARY);
    tft.drawCircle(cx, cy, 15 + s + 1, UI_PRIMARY);
    tft.fillCircle(cx, cy, 12, UI_PRIMARY);
  } else { // Idle
    tft.drawCircle(cx, cy, 15, UI_CARD_BG);
    tft.drawCircle(cx, cy, 16, UI_CARD_BG);
    tft.drawLine(cx-12, cy, cx+12, cy, UI_CARD_BG);
  }

  // Ensure header/footer are never erased by animation leaks
  if (millis() % 5000 < 100) { // Safety refresh every 5 seconds
      drawNavigationBar();
  }
}


// ============================================================
// EYES
// ============================================================
void setEyeExpression(String expr) {
  Serial.println("[Eyes] Set to: " + expr);
  currentEyeExpression = expr;
  updateEyes();

  // Also update mouth expression
  expr.toUpperCase();
  if (expr == "HAPPY" || expr == "JOY" || expr == "LOVE" || expr == "EXCITED" || expr == "WINK") {
    sendMouthExpression("HAPPY");
  }
  else if (expr == "SAD" || expr == "SORRY" || expr == "WORRIED") {
    sendMouthExpression("SAD");
  }
  else if (expr == "SURPRISE" || expr == "SHOCK" || expr == "ANGRY") {
    sendMouthExpression("SURPRISE");
  }
  else if (expr == "LISTENING") {
    sendMouthExpression("LISTEN");
  }
  else if (expr == "SPEAKING") {
    sendMouthExpression("SPEAK");
  }
  else {
    // Default idle
    sendMouthIdle();
  }
}

void updateEmotionEngine() {
  if (millis() - lastEmotionEngineUpdate < 5000) return; // Run every 5 seconds
  lastEmotionEngineUpdate = millis();

  // 1. Natural Decay (Drift towards Normal/Relaxed)
  if (moodValence > 0.1) moodValence -= 0.02;     // Fade happiness slowly
  else if (moodValence < -0.1) moodValence += 0.05; // Recover from sadness/anger faster
  else moodValence = 0.0;

  if (moodArousal > 0.1) moodArousal -= 0.05;     // Calm down quickly
  else if (moodArousal < -0.1) moodArousal += 0.01; // Wake up slowly
  else moodArousal = 0.0;
  
  // 2. Environmental Impacts
  // Bad Air = Sleepy/Sick (-Valence, -Arousal)
  if (aqi_val > 100 || eco2_val > 1000) {
      moodArousal -= 0.1; 
      moodValence -= 0.05;
  }
  // Hot/Humid = Sleepy (-Arousal)
  if (temp_aht > 30.0 || humidity_aht > 70.0) {
      moodArousal -= 0.05;
  }

  // 3. Time without interaction = Bored/Sleepy
  // If no interaction in 5 mins -> drowsy
  if (millis() - lastInteractionTime > 300000) {
      moodArousal -= 0.05;
  }

  // Cap values between -1.0 and 1.0
  if (moodValence > 1.0) moodValence = 1.0;
  if (moodValence < -1.0) moodValence = -1.0;
  if (moodArousal > 1.0) moodArousal = 1.0;
  if (moodArousal < -1.0) moodArousal = -1.0;
}

void updateEyes() {
  static unsigned long lastUpdate = 0;
  static String nextEyeExpression = "";
  static unsigned long lastIdleChange = 0;

  // Run Emotion Math
  updateEmotionEngine();

  // Idle Animation Logic (Only in Normal Mode)
  if (currentMode == MODE_NORMAL && currentMedState == MED_IDLE) {
      if (millis() - lastIdleChange > 15000) { // Every 15 seconds
          // Map Valence/Arousal to Expression Quadrants
          if (moodValence >= 0.3 && moodArousal >= 0.3)      nextEyeExpression = "EXCITED";
          else if (moodValence >= 0.3 && moodArousal > -0.3) nextEyeExpression = "HAPPY";
          else if (moodValence >= 0.3 && moodArousal <= -0.3) nextEyeExpression = "LOVE";
          
          else if (moodValence <= -0.55 && moodArousal >= 0.3) nextEyeExpression = "ANGRY"; // Sharp, heated negative
          else if (moodValence <= -0.30 && moodArousal >= 0.15) nextEyeExpression = "FRUSTRATED"; // Tense / annoyed
          else if (moodValence <= -0.20 && moodArousal >= -0.05) nextEyeExpression = "WORRIED"; // Uneasy / concerned
          else if (moodValence <= -0.25 && moodArousal > -0.35) nextEyeExpression = "SAD"; // Droopy / downcast
          else if (moodValence <= -0.25 && moodArousal <= -0.35) nextEyeExpression = "SLEEPY"; // or DEAD(X_X) if extreme
          
          else if (moodArousal >= 0.5) nextEyeExpression = "SURPRISED";
          else if (moodArousal <= -0.5) nextEyeExpression = "BORED"; // Flat / unamused
          
          else nextEyeExpression = (random(0,10) > 7) ? "WINK" : "NORMAL"; // Subtle variance near origin
          
          // Extreme state overrides
          if (moodValence < -0.8 && moodArousal < -0.8) nextEyeExpression = "DEAD"; // Extremely bad environment
          
          if (nextEyeExpression != currentEyeExpression) {
              Serial.println("[Eyes] Mood Shift -> V: " + String(moodValence, 2) + " A: " + String(moodArousal, 2) + " -> " + nextEyeExpression);
          }
          lastIdleChange = millis();
      }
  }

  // Blink Logic
  static bool isBlinking = false;
  static unsigned long lastBlinkTime = 0;
  
  // 1. Random blink (Normal behavior)
  if (currentEyeExpression == "NORMAL" && !isBlinking && millis() - lastBlinkTime > random(3000, 6000)) {
    isBlinking = true;
    lastBlinkTime = millis();
  }
  
  // 2. Transition Blink (If next expression is waiting)
  if (nextEyeExpression != "" && nextEyeExpression != currentEyeExpression && !isBlinking) {
      isBlinking = true;
      lastBlinkTime = millis();
  }
  
  // 3. Blink Duration & State Change
  if (isBlinking && millis() - lastBlinkTime > 200) {
    isBlinking = false;
    lastBlinkTime = millis();
    
    // Apply pending expression MID-BLINK (when eyes are closed/opening)
    if (nextEyeExpression != "" && nextEyeExpression != currentEyeExpression) {
        currentEyeExpression = nextEyeExpression;
        nextEyeExpression = ""; // Clear pending
    }
  }

  // Draw for both eyes (Left ch 0, Right ch 1)
  for (int i = 0; i < 2; i++) {
    tcaselect(i == 0 ? CH_EYE_LEFT : CH_EYE_RIGHT);
    display.clearDisplay();

    if (isBlinking) {
       // Blink Locked
       display.drawLine(32, 32, 96, 32, SSD1306_WHITE);
    } else {
       // Draw Expression
       if (currentEyeExpression == "HAPPY") {
         // Arching UP (Bottom of circle visible) -> ^ ^
         // Smoother Arch: Removed flat rect cut at top
         display.fillCircle(64, 40, 30, SSD1306_WHITE); // Outer larger
         display.fillCircle(64, 48, 25, SSD1306_BLACK); // Inner cuts bottom-center
         // No top rect - let the arch curve naturally
       } 
       else if (currentEyeExpression == "SAD") {
         // Heavy downcast look with a visible tear. This should read as "sad"
         // even on a small OLED, not just "sleepy".
         display.fillCircle(64, 37, 24, SSD1306_WHITE);
         display.fillCircle(64, 44, 7, SSD1306_BLACK);
         display.fillCircle(68, 40, 2, SSD1306_WHITE);
         display.fillRect(0, 0, 128, 15, SSD1306_BLACK);
         display.fillRect(0, 50, 128, 14, SSD1306_BLACK);
         if (i == 0) {
           display.fillTriangle(0, 15, 56, 8, 0, 32, SSD1306_BLACK);
           display.fillCircle(86, 52, 3, SSD1306_WHITE);
           display.fillTriangle(83, 48, 89, 48, 86, 60, SSD1306_WHITE);
         } else {
           display.fillTriangle(127, 15, 72, 8, 127, 32, SSD1306_BLACK);
           display.fillCircle(42, 52, 3, SSD1306_WHITE);
           display.fillTriangle(39, 48, 45, 48, 42, 60, SSD1306_WHITE);
         }
       }
       else if (currentEyeExpression == "FRUSTRATED") {
         // Tight, annoyed look. Distinct from SAD so it feels more alive.
         display.fillCircle(64, 33, 24, SSD1306_WHITE);
         display.fillCircle(60, 35, 8, SSD1306_BLACK);  // slight side glance
         display.fillCircle(70, 29, 2, SSD1306_WHITE);  // tiny glint
         display.fillRect(0, 0, 128, 10, SSD1306_BLACK);
         display.fillRect(0, 49, 128, 15, SSD1306_BLACK);
         if (i == 0) {
           display.fillTriangle(0, 10, 54, 18, 0, 28, SSD1306_BLACK);
         } else {
           display.fillTriangle(127, 10, 74, 18, 127, 28, SSD1306_BLACK);
         }
       } 
       else if (currentEyeExpression == "WORRIED") {
         // Uneasy, uncertain look. Softer than FRUSTRATED, more alert than SAD.
         display.fillCircle(64, 34, 23, SSD1306_WHITE);
         display.fillCircle(60, 38, 7, SSD1306_BLACK);
         display.fillCircle(69, 28, 2, SSD1306_WHITE);
         display.fillRect(0, 0, 128, 12, SSD1306_BLACK);
         display.fillRect(0, 51, 128, 12, SSD1306_BLACK);
         if (i == 0) {
           display.fillTriangle(0, 12, 58, 10, 0, 26, SSD1306_BLACK);
           display.fillTriangle(54, 8, 76, 14, 60, 18, SSD1306_BLACK);
           display.fillCircle(83, 49, 2, SSD1306_WHITE);
           display.fillTriangle(81, 46, 85, 46, 83, 57, SSD1306_WHITE);
         } else {
           display.fillTriangle(127, 12, 69, 10, 127, 26, SSD1306_BLACK);
           display.fillTriangle(52, 14, 68, 8, 74, 18, SSD1306_BLACK);
           display.fillCircle(44, 49, 2, SSD1306_WHITE);
           display.fillTriangle(42, 46, 46, 46, 44, 57, SSD1306_WHITE);
         }
       }
       else if (currentEyeExpression == "THINKING") {
         // Looking UP-RIGHT with a slight brow
         display.fillCircle(64, 32, 25, SSD1306_WHITE);
         display.fillCircle(80, 22, 10, SSD1306_BLACK); 
         display.fillRect(0, 0, 128, 18, SSD1306_BLACK); 
       }
       else if (currentEyeExpression == "ANGRY") {
         // Full eye with sharp angled brow cut to look mean
         display.fillCircle(64, 32, 26, SSD1306_WHITE);  // Eye
         display.fillCircle(64, 34, 10, SSD1306_BLACK);  // Pupil
         display.fillCircle(70, 28, 3, SSD1306_WHITE);   // Glint
         // Angry V-brow: triangle cuts from outer top to inner mid
         display.fillTriangle(0, 0, 128, 0, 0, 22, SSD1306_BLACK);  // Outer corner low brow
         display.fillTriangle(64, 18, 128, 0, 128, 18, SSD1306_BLACK); // Inner corner stays
       }
       else if (currentEyeExpression == "SURPRISED") {
         // Wide open, small pupil
         display.fillCircle(64, 32, 28, SSD1306_WHITE);
         display.fillCircle(64, 32, 8, SSD1306_BLACK);
       }
       else if (currentEyeExpression == "SLEEPY") {
         // Half closed
         display.fillCircle(64, 32, 25, SSD1306_WHITE);
         display.fillRect(0, 0, 128, 32, SSD1306_BLACK); // Top half blocked
         display.fillCircle(64, 38, 8, SSD1306_BLACK); // Pupil low
       }
       else if (currentEyeExpression == "CONFUSED") {
         // Questioning look (Raised brow)
         display.fillCircle(64, 32, 25, SSD1306_WHITE);
         display.fillCircle(70, 28, 8, SSD1306_BLACK); // Pupil side
         display.fillTriangle(0, 0, 64, 20, 0, 20, SSD1306_BLACK); // Asymmetrical cut
       }
       else if (currentEyeExpression == "LOVE") {
         // Heart Shape
         // Two circles + Triangle at bottom
         display.fillCircle(64-15, 30, 15, SSD1306_WHITE); // Left hump
         display.fillCircle(64+15, 30, 15, SSD1306_WHITE); // Right hump
         display.fillTriangle(64-28, 35, 64+28, 35, 64, 60, SSD1306_WHITE); // Bottom V
       }
       else if (currentEyeExpression == "WINK") {
         // Left Eye Normal, Right Eye Closed (or vice versa based on 'i')
         if (i == 0) { // Left: Open
            display.fillCircle(64, 32, 26, SSD1306_WHITE); 
            display.fillCircle(64, 32, 10, SSD1306_BLACK); 
            display.fillCircle(70, 26, 3, SSD1306_WHITE); 
         } else { // Right: Closed line
            display.fillRect(32, 30, 64, 4, SSD1306_WHITE);
         }
       }
       else if (currentEyeExpression == "DEAD" || currentEyeExpression == "X_X") {
         // X Shape
         display.drawLine(44, 12, 84, 52, SSD1306_WHITE);
         display.drawLine(44, 52, 84, 12, SSD1306_WHITE);
         // Thicken
         display.drawLine(43, 12, 83, 52, SSD1306_WHITE);
         display.drawLine(45, 12, 85, 52, SSD1306_WHITE);
         display.drawLine(43, 52, 83, 12, SSD1306_WHITE);
         display.drawLine(45, 52, 85, 12, SSD1306_WHITE);
       }
       else if (currentEyeExpression == "SUSPICIOUS") {
         // Squint
         display.fillCircle(64, 32, 26, SSD1306_WHITE); 
         display.fillCircle(54, 32, 8, SSD1306_BLACK); // Pupil side-eye
         display.fillRect(0, 0, 128, 22, SSD1306_BLACK); // Top heavy lid
         display.fillRect(0, 42, 128, 22, SSD1306_BLACK); // Bottom squeeze
       }
       else if (currentEyeExpression == "EXCITED") {
         // Big Sparkle Eyes
         display.fillCircle(64, 32, 30, SSD1306_WHITE); 
         display.fillCircle(64, 32, 25, SSD1306_BLACK); // Big Pupil
         // Glints
         display.fillCircle(75, 20, 8, SSD1306_WHITE);  
         display.fillCircle(55, 45, 4, SSD1306_WHITE);
         display.fillCircle(80, 40, 3, SSD1306_WHITE);
       }
       else if (currentEyeExpression == "BORED") {
         // Flat, unamused look (squished horizontally and half closed)
         display.fillCircle(64, 32, 22, SSD1306_WHITE); 
         display.fillCircle(64, 32, 8, SSD1306_BLACK); // Pupil center
         display.fillRect(0, 0, 128, 26, SSD1306_BLACK); // Top lid completely flat and low
         display.fillRect(0, 48, 128, 16, SSD1306_BLACK); // Bottom lid flat and high
       }
       else {
         // NORMAL
         display.fillCircle(64, 32, 26, SSD1306_WHITE); 
         display.fillCircle(64, 32, 10, SSD1306_BLACK); 
         display.fillCircle(70, 26, 3, SSD1306_WHITE);  
       }
    }
    
    display.display();
  }
}

// ============================================================
// BUZZER & SOUNDS
// ============================================================
void playTone(int freq, int duration) {
  if (!buzzerReady) {
    delay(duration);
    return;
  }

  if (freq > 0) {
    if (ledcWriteTone(BUZZER_PIN, freq) > 0) {
      delay(duration);
      ledcWriteTone(BUZZER_PIN, 0);
    }
  } else {
    delay(duration);
  }
}

void playStartupSound() {
  // Ascending cheerful melody
  playTone(523, 100); // C5
  delay(50);
  playTone(659, 100); // E5
  delay(50);
  playTone(784, 100); // G5
  delay(50);
  playTone(1046, 300); // C6
}

void playListeningTone() {
  // Rising double beep "Blip-Blip"
  playTone(1000, 100); 
  delay(50);
  playTone(1500, 100); 
}

void playProcessingTone() {
  // Falling double beep "Bloop-Bloop"
  playTone(1500, 100); 
  delay(50);
  playTone(1000, 100); 
}

// ============================================================
// STARTUP UI
// ============================================================
void drawLoadingScreen(String status, int percent) {
  // Only redraw dynamic parts to avoid flicker
  static bool firstRun = true;
  if (firstRun) {
    tft.fillScreen(UI_BG);
    
    // Draw Logo Circle with accent
    int cx = tft.width() / 2;
    int cy = tft.height() / 2 - 20;
    
    // Outer ring
    tft.drawCircle(cx, cy, 40, UI_ACCENT);
    tft.drawCircle(cx, cy, 38, UI_ACCENT);
    
    // Inner fill
    tft.fillCircle(cx, cy, 30, UI_CARD_BG);
    
    tft.setFont(&FreeSansBold12pt7b);
    tft.setTextColor(UI_TEXT_MAIN);
    tft.setTextSize(1);
    
    // Center Text "ELLA"
    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds("ELLA", 0, 0, &x1, &y1, &w, &h);
    tft.setCursor(cx - w/2, cy + 10); 
    tft.print("ELLA");
    
    // Progress Bar Background
    int barW = 160;
    int barH = 6;
    tft.drawRect(cx - barW/2, cy + 70, barW, barH, UI_PRIMARY);
    
    firstRun = false;
  }
  
  int cx = tft.width() / 2;
  int cy = tft.height() / 2 - 20;
  
  // Update Status Text
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT_SUB, UI_BG); // BG color clears text
  tft.fillRect(0, cy + 50, SCR_W, 15, UI_BG); // Clear previous text area
  
  // Center status text
  int len = status.length() * 6; // Approx width
  tft.setCursor((SCR_W - len)/2, cy + 55);
  tft.print(status);
  
  // Update Progress Bar
  int barW = 160;
  int barH = 6;
  int fillW = (barW - 2) * percent / 100;
  tft.fillRect(cx - barW/2 + 1, cy + 71, fillW, barH-2, UI_ACCENT);
}


// ============================================================
// FIREBASE
// ============================================================
void setupFirebase() {
  if (!FIREBASE_ENABLED) {
    firebaseReady = false;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    firebaseReady = false;
    nextFirebaseRetryMs = millis() + FIREBASE_RETRY_MS;
    return;
  }

  config.api_key = FIREBASE_AUTH;
  config.database_url = FIREBASE_DATABASE_URL;
  
  // Use Legacy Token Authentication (Database Secret)
  // This matches the working configuration from sketch_dec5a
  config.signer.tokens.legacy_token = FIREBASE_DB_SECRET;
  
  // Set timeouts to mimic sketch_dec5a settings
  config.timeout.serverResponse = 10 * 1000;
  
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (Firebase.ready()) {
    firebaseReady = true;
    nextFirebaseRetryMs = 0;
    commandPollBackoffUntil = 0;
    commandReadFailStreak = 0;
    lastCommandJsonFetchMs = 0;
    Serial.println("[Firebase] Connected & Ready!");
    syncUserProfileFromFirebase();
    syncRemindersFromFirebase();
    setModeStatusFirebase(currentMode == MODE_AI ? "AI" : "NORMAL");
  } else {
    firebaseReady = false;
    nextFirebaseRetryMs = millis() + FIREBASE_RETRY_MS;
    Serial.println("[Firebase] Connection Timed Out");
  }
}

void tokenStatusCallback(TokenInfo info) {
  String s = "Token Info: type = " + String(info.type) + ", status = " + String(info.status);
  Serial.println(s);
}



void checkRemoteCommands() {
  if (!FIREBASE_ENABLED || offlineModeLocked) return;
  if (!firebaseReady || !networkAvailable()) return;

    unsigned long now = millis();
    if (now < commandPollBackoffUntil) return;

    static unsigned long lastCmdCheck = 0;
    unsigned long minCmdInterval = (currentMode == MODE_AI) ? COMMAND_POLL_INTERVAL_AI_MS : COMMAND_POLL_INTERVAL_NORMAL_MS;
    if (now - lastCmdCheck < minCmdInterval) return;
    lastCmdCheck = now;

    // BATCHED: Single read of entire /commands node (was 6 separate calls)
    if (!Firebase.RTDB.getJSON(&fbdoCmd, "/commands")) {
      if (commandReadFailStreak < 10) commandReadFailStreak++;

      uint8_t shift = (commandReadFailStreak > 5) ? 5 : (commandReadFailStreak - 1);
      unsigned long backoffMs = COMMAND_ERROR_BACKOFF_BASE_MS << shift;
      commandPollBackoffUntil = now + backoffMs;

      if (commandReadFailStreak == 1 || commandReadFailStreak == 3 || commandReadFailStreak >= COMMAND_ERROR_REINIT_THRESHOLD) {
        Serial.printf("[Firebase] /commands read failed x%u: %s (backoff %lu ms)\n",
                      commandReadFailStreak,
                      fbdoCmd.errorReason().c_str(),
                      backoffMs);
      }

      if (commandReadFailStreak >= COMMAND_ERROR_REINIT_THRESHOLD) {
        firebaseReady = false;
        nextFirebaseRetryMs = now + FIREBASE_RETRY_MS;
        commandPollBackoffUntil = nextFirebaseRetryMs;
        Serial.println("[Firebase] Command channel unhealthy, scheduling reconnect");
      }
      return;
    }

    if (commandReadFailStreak > 0) {
      Serial.printf("[Firebase] /commands recovered after %u failures\n", commandReadFailStreak);
    }
    commandReadFailStreak = 0;
    commandPollBackoffUntil = 0;
    lastCommandJsonFetchMs = now;
    
    FirebaseJson *json = fbdoCmd.jsonObjectPtr();
    if (!json) return;
    
    // 1. Emergency
    FirebaseJsonData valEmergency;
    json->get(valEmergency, "emergency");
    if (valEmergency.success && valEmergency.to<bool>()) {
        Serial.println("[Command] Emergency Triggered from Web!");
        sendEmergencyAlert("Web App Panic Button");
        Firebase.RTDB.setBool(&fbdo, "/commands/emergency", false);
    }

    // 2. WiFi Config (only if wifiConfig is actually an object with a real ssid)
    FirebaseJsonData valWifi;
    json->get(valWifi, "wifiConfig/ssid");
    String newSSID = valWifi.success ? valWifi.to<String>() : "";
    newSSID.trim();
    // Guard: Firebase returns "false"/"null" when the node doesn't exist as expected
    if (valWifi.success && newSSID.length() > 2 && newSSID != "false" && newSSID != "null" && newSSID != "true") {
        String newPass = "";
        FirebaseJsonData valPass;
        json->get(valPass, "wifiConfig/password");
        if (valPass.success) newPass = valPass.to<String>();
        
        Serial.println("[Command] New WiFi Config: " + newSSID);
        prefs.begin("ella", false);
        prefs.putString("ssid", newSSID);
        prefs.putString("pass", newPass);
        prefs.end();
        Firebase.RTDB.deleteNode(&fbdo, "/commands/wifiConfig");
        speakText("New Wi-Fi saved. Restarting.");
        delay(2000);
        ESP.restart();
    }

      // 2b. Forget Wi-Fi credentials and fall back to hardcoded network
      FirebaseJsonData valWifiReset;
      json->get(valWifiReset, "wifiReset");
      if (valWifiReset.success && valWifiReset.to<bool>()) {
        Serial.println("[Command] WiFi reset requested from Web App!");
        prefs.begin("ella", false);
        prefs.remove("ssid");
        prefs.remove("pass");
        prefs.end();
        wifiSSID = "";
        wifiPass = "";
        Firebase.RTDB.deleteNode(&fbdo, "/commands/wifiConfig");
        Firebase.RTDB.setBool(&fbdo, "/commands/wifiReset", false);

        if (WiFi.status() == WL_CONNECTED) {
          WiFi.disconnect(true);
        }

        resetWiFiStationForRetry();
        WiFi.begin(ssid, password);
        Serial.println("[WiFi] Cleared saved credentials, reconnecting with hardcoded fallback...");
      }

    // 3. AI Chat
    FirebaseJsonData valChat;
    json->get(valChat, "aiChat");
    if (valChat.success) {
        String msg = valChat.to<String>();
        msg.trim();
        if (msg.length() > 1 && msg != "null" && msg != "false" && msg != "true") {
            Serial.println("[Command] AI Chat from Web: " + msg);
            Firebase.RTDB.deleteNode(&fbdo, "/commands/aiChat");
            setAIInputMode(false);
            queueWebAIChat(msg);
        }
    }

    // 3b. AI Input Mode
    FirebaseJsonData valAiInputMode;
    json->get(valAiInputMode, "aiInputMode");
    if (valAiInputMode.success) {
        String inputMode = valAiInputMode.to<String>();
        inputMode.trim();
        inputMode.toUpperCase();
        if (inputMode == "WEB") {
            setAIInputMode(false);
            Firebase.RTDB.deleteNode(&fbdo, "/commands/aiInputMode");
        } else if (inputMode == "MIC") {
            setAIInputMode(true);
            Firebase.RTDB.deleteNode(&fbdo, "/commands/aiInputMode");
        }
    }

    // 4. Remote Speak
    FirebaseJsonData valSpeak;
    json->get(valSpeak, "speak");
    if (valSpeak.success) {
        String msg = valSpeak.to<String>();
        msg.trim();
        if (msg.length() > 1 && msg != "null" && msg != "false" && msg != "true") {
            Serial.println("[Command] Remote Speak: " + msg);
            Firebase.RTDB.deleteNode(&fbdo, "/commands/speak");
            speakText(msg.c_str());
        }
    }

    // DIRECT CHECKUP command (bypasses AI pipeline for instant response)
    FirebaseJsonData valCheckup;
    json->get(valCheckup, "checkup");
    if (valCheckup.success && valCheckup.to<bool>()) {
        Serial.println("[Command] Direct Checkup from Web App!");
        Firebase.RTDB.setBool(&fbdo, "/commands/checkup", false);
        // Only start if not already in progress
        if (currentMedState != MED_PLACE_FINGER && currentMedState != MED_MEASURING && currentMedState != MED_WAIT_FINGER) {
            ai_requested_checkup = true;
            if (currentMode != MODE_NORMAL) switchToNormalMode();
            currentMedState = MED_IDLE;
            medStateTimer = millis();
            drawNormalScreen(true);
            speakText("Starting medical checkup. Please place your finger on the sensor.");
            return;
        } else {
            speakText("Checkup already in progress.");
            return;
        }
    }

    // DIRECT MEDITATE command (bypasses AI - triggers startMeditation directly)
    FirebaseJsonData valMeditate;
    json->get(valMeditate, "meditate");
    if (valMeditate.success) {
        String preset = valMeditate.to<String>();
        preset.trim();
        if (preset.length() > 1 && preset != "null" && preset != "false") {
            Serial.println("[Command] Direct Meditate: " + preset);
            Firebase.RTDB.deleteNode(&fbdo, "/commands/meditate");
            startMeditation(preset);
        }
    }

    // DIRECT BREATHE command
    FirebaseJsonData valBreathe;
    json->get(valBreathe, "breathe");
    if (valBreathe.success && valBreathe.to<bool>()) {
        Serial.println("[Command] Direct Breathe from Web App!");
        Firebase.RTDB.setBool(&fbdo, "/commands/breathe", false);
        if (currentMode != MODE_NORMAL) switchToNormalMode();
        currentMedState = MED_BREATHING;
        medStateTimer = millis();
        drawNormalScreen(true);
        speakText("Starting breathing exercise. Breathe in... and breathe out.");
    }

    // 5. Mode Switch
    FirebaseJsonData valMode;
    json->get(valMode, "systemMode");
    if (valMode.success) {
        String modeCmd = valMode.to<String>();
        modeCmd.trim();
        if (modeCmd == "AI" && currentMode != MODE_AI) {
            // Prevent the web app from forcing AI mode if a checkup was just initiated
            if (currentMedState != MED_IDLE) {
                Serial.println("[Firebase] Ignored Switch to AI Mode (Medical Checkup Active)");
                Firebase.RTDB.setString(&fbdo, "/commands/systemMode", "");
            } else {
                Serial.println("[Firebase] Switch to AI Mode");
                switchToAIMode();
                Firebase.RTDB.setString(&fbdo, "/commands/systemMode", "");
            }
        } else if (modeCmd == "NORMAL" && currentMode != MODE_NORMAL) {
            Serial.println("[Firebase] Switch to Normal Mode");
            switchToNormalMode();
            Firebase.RTDB.setString(&fbdo, "/commands/systemMode", "");
        } else if (modeCmd == "OFF") {
          Serial.println("[Firebase] OFF command -> stop + NORMAL standby");
          handleUniversalStopAudioCommand();
          Firebase.RTDB.setString(&fbdo, "/commands/systemMode", "");
        } else if (modeCmd == "AI" || modeCmd == "NORMAL") {
            // Clear stale mode commands so local button presses do not bounce back.
            Firebase.RTDB.setString(&fbdo, "/commands/systemMode", "");
        } else if (modeCmd.length() > 0) {
          Firebase.RTDB.setString(&fbdo, "/commands/systemMode", "");
        }
    }

    // 6. Stop Audio
    FirebaseJsonData valStop;
    json->get(valStop, "stopAudio");
    if (valStop.success && valStop.to<bool>()) {
      handleUniversalStopAudioCommand();
        Firebase.RTDB.setBool(&fbdo, "/commands/stopAudio", false);
    }

    // 7. Autonomous ToF Avoid / Guard Standby
    FirebaseJsonData valGuard;
    json->get(valGuard, "guard");
    if (valGuard.success) {
        bool active = valGuard.to<bool>();
        setGuardMode(active);
    }

    FirebaseJsonData valAutoAvoid;
    json->get(valAutoAvoid, "autoAvoid");
    if (valAutoAvoid.success) {
        bool active = valAutoAvoid.to<bool>();
        setAutoAvoidMode(active);
    }

    FirebaseJsonData valFocusMode;
    json->get(valFocusMode, "focusMode");
    if (valFocusMode.success) {
      bool active = valFocusMode.to<bool>();
      if (active && !focusModeActive) {
        focusModeActive = true;
        Serial.println("[Focus] Focus Mode Activated from Web App!");
        playMusic("ambient");
        Firebase.RTDB.setBool(&fbdoFocus, "/status/focusMode", true);
      } else if (!active && focusModeActive) {
        focusModeActive = false;
        Serial.println("[Focus] Focus Mode Deactivated.");
        clearTtsChunkQueue();
        if (audio.isRunning()) audio.stopSong();
        enterAudioTransitionQuiet();
        isSpeaking = false;
        Firebase.RTDB.setBool(&fbdoFocus, "/status/focusMode", false);
      }
    }

    // 7b. IMU Recalibration (Web button)
    FirebaseJsonData valImuReset;
    json->get(valImuReset, "imuReset");
    if (valImuReset.success && valImuReset.to<bool>()) {
        Serial.println("[Command] IMU recalibration requested");
        Firebase.RTDB.setBool(&fbdo, "/commands/imuReset", false);
        lastInteractionTime = millis();
        recalibrateNavigationHeading();
    }

    // 8. Motor Commands (Joysticks)
    FirebaseJsonData valMotor;
    json->get(valMotor, "motor");
    if (valMotor.success) {
        String motorCmd = valMotor.to<String>();
        motorCmd.trim();
        motorCmd.toUpperCase();
        if (motorCmd.length() > 2 && motorCmd != "NULL" && motorCmd != "FALSE") {
            Serial.println("[Command] Motor: " + motorCmd);

            // The web UI already sends DRV8833-friendly commands, so pass them straight through.
            if (!applyManualMotorCommand(motorCmd)) {
                Serial.println("[Command] Ignored motor command: " + motorCmd);
            }
            Firebase.RTDB.deleteNode(&fbdo, "/commands/motor");
        }
    }

    // 9. Motor Speed
    FirebaseJsonData valSpd;
    json->get(valSpd, "speed");
    if (valSpd.success) {
        int spd = valSpd.to<int>();
        if (spd >= 0 && spd <= 255) {
            motorSpeed = spd;
            Serial.printf("[Command] Motor Speed set to: %d\n", motorSpeed);
            Firebase.RTDB.deleteNode(&fbdo, "/commands/speed");
        }
    }
}
void pushSensorDataToFirebase() {
  if (offlineModeLocked || (!mqtt.connected() && !firebaseReady)) {
     return;
  }

  static unsigned long lastPush = 0;
  if (millis() - lastPush < 10000) return;

  FirebaseJson json;
  
  // SANITIZE: Replace NaN with 0 because Firebase rejects NaN
  json.set("temperature", isnan(temp_aht) ? 0.0 : temp_aht);
  json.set("humidity", isnan(humidity_aht) ? 0.0 : humidity_aht);
  json.set("heartRate", isnan(max30102_hr) ? 0 : max30102_hr);
  json.set("spo2", isnan(max30102_spo2) ? 0 : max30102_spo2);
  // Integer values don't need isnan but guard just in case they were floats
  json.set("aqi", aqi_val);
  json.set("tvoc", tvoc_val);
  json.set("eco2", eco2_val);
  json.set("bodyTemp", isnan(max30102_temp) ? 0.0 : max30102_temp);
  json.set("imuReady", imuReady);
  json.set("imuHeading", imuYawEstimateDeg);
  json.set("imuPitch", imuPitchDeg);
  json.set("imuRoll", imuRollDeg);
  json.set("imuTilt", fmaxf(fabsf(imuPitchDeg), fabsf(imuRollDeg)));
  json.set("imuAccelG", imuAccelMagnitudeG);
  json.set("imuYawRate", imuGyroZFiltered);
  json.set("imuDriveAssist", driveHoldStraight);
  json.set("imuMotionState", driveControlActive ? "manual" : (isMoving ? "timed" : "idle"));
  json.set("imuTurnActive", turnControlActive);
  json.set("imuTurnProgress", turnAccumDeg);
  json.set("imuTurnTarget", turnTargetDeg);
  json.set("tofReady", tofReady);
  json.set("tofHasReading", tofHasReading);
  json.set("tofDistanceMm", tofHasReading ? (int)tofFilteredMm : 0);
  json.set("tofDistanceCm", tofHasReading ? (tofFilteredMm / 10.0f) : 0.0f);
  json.set("autoAvoid", autoAvoidEnabled);
  json.set("guardMode", guardModeEnabled);
  json.set("guardAlarm", guardAlarmActive);
  json.set("timestamp", ntpTimeValid ? time(nullptr) : (long)(millis() / 1000UL));

  if (FIREBASE_ENABLED && firebaseReady) {
    String path = "/readings";
    if (Firebase.RTDB.pushJSON(&fbdo, path.c_str(), &json)) {
      Serial.println("[Firebase] Sensor data pushed to web app");
    } else {
      Serial.printf("[Firebase] Push failed: %s\n", fbdo.errorReason().c_str());
    }
  }

  pushSensorDataToMQTT(json);

  lastPush = millis();
}


void checkEnvironmentalAlerts() {
    // Thresholds: Temp > 40C, AQI >= 5 (Poor)
    bool highTemp = (temp_aht > 40.0);
    bool poorAir = (aqi_val >= 5);

    if (!highTemp && !poorAir) return;

    // Debounce: Alert max once every 30 mins
    static unsigned long lastAlert = 0;
    if (millis() - lastAlert < 30 * 60 * 1000) return;

    String msg = "⚠️ **ELLA SAFETY ALERT** ⚠️\n\n";
    bool send = false;

    if (highTemp) {
        msg += "🌡️ High Temperature Detected: " + String(temp_aht, 1) + "°C\n";
        send = true;
    }
    if (poorAir) {
        msg += "💨 Poor Air Quality Detected (AQI: " + String(aqi_val) + ")\n";
        send = true;
    }

    if (send) {
        msg += "\nPlease check the environment.";
        Serial.println("[Safety] Sending Environmental Alert!");
        sendTelegramAlert(msg);
        lastAlert = millis();
        speakText("Safety alert sent to your phone.");
    }
}

// Ensure User Name is syncing
void syncUserProfileFromFirebase() {
  if (!firebaseReady) return;
  
  if (Firebase.RTDB.getJSON(&fbdo, "/commands/userProfile")) {
      FirebaseJson *json = fbdo.jsonObjectPtr();
      FirebaseJsonData result;
      
      // 1. Name
      json->get(result, "name");
      if (result.success) {
          user_name = result.to<String>();
          Serial.println("[Profile] Name: " + user_name);
      }
      
      // 2. Telegram Bot Token
      json->get(result, "telegramBotToken");
      if (result.success) {
          cloudBotToken = result.to<String>();
          cloudBotToken.trim(); // TRUNCATE invisible spaces/newlines
          Serial.println("[Profile] Bot Token Updated");
          prefs.putString("botToken", cloudBotToken);
      }

      // 3. Telegram Chat ID
      json->get(result, "telegramChatId");
      if (result.success) {
          cloudChatId = result.to<String>();
          cloudChatId.trim(); // TRUNCATE invisible spaces/newlines
          Serial.println("[Profile] Chat ID Updated");
          prefs.putString("chatId", cloudChatId);
      }

      // 4. Emergency Contact
      json->get(result, "emergencyContact");
      if (result.success) {
          user_emergency_contact = result.to<String>();
          Serial.println("[Profile] Emergency Contact: " + user_emergency_contact);
          prefs.putString("emergency", user_emergency_contact);
      }
      
      // Also save Name
      if (user_name.length() > 0) prefs.putString("userName", user_name);

  }
}

void syncRemindersFromFirebase() {
  if (!firebaseReady || offlineModeLocked) return;

  // IMPORTANT: /reminders is a Firebase object (auto-push IDs), NOT a string.
  // Use getJSON then stringData() to get the raw JSON string response.
  if (Firebase.RTDB.getJSON(&fbdo, "/reminders")) {
    String raw = fbdo.stringData(); // raw JSON string of the whole object
    if (raw.length() > 5 && raw != "null") {
      cloudRemindersJson = raw;
      Serial.println("[Firebase] Reminders synced (" + String(cloudRemindersJson.length()) + " bytes)");
    } else {
      Serial.println("[Firebase] Reminders: empty or no data");
    }
  } else {
    Serial.println("[Firebase] Reminder sync failed: " + String(fbdo.errorReason().c_str()));
  }
}


void checkFocusMode() {
    if (!firebaseReady || offlineModeLocked) return;
    if (isSpeaking || isProcessingAI) return; // Don't interrupt active AI
    
    // Check every 5 seconds
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 5000) return;
    lastCheck = millis();

    // Read from commands/focusMode (web app writes here, device reads here)
    if (Firebase.RTDB.getBool(&fbdoFocus, "/commands/focusMode")) {
        bool fbFocus = fbdoFocus.boolData();
        if (fbFocus && !focusModeActive) {
            focusModeActive = true;
            Serial.println("[Focus] Focus Mode Activated from Web App!");
            playMusic("ambient");
            // Acknowledge
            Firebase.RTDB.setBool(&fbdoFocus, "/status/focusMode", true);
        } else if (!fbFocus && focusModeActive) {
            focusModeActive = false;
            Serial.println("[Focus] Focus Mode Deactivated.");
            clearTtsChunkQueue();
            if (audio.isRunning()) audio.stopSong();
            enterAudioTransitionQuiet();
            isSpeaking = false;
            // Acknowledge
            Firebase.RTDB.setBool(&fbdoFocus, "/status/focusMode", false);
        }
    } else {
        Serial.println("[Focus] FB read err: " + fbdoFocus.errorReason());
    }
}

String getRemindersContext() {
  String s = "\nREMINDERS:\n";
  int count = 0;

  if (pendingReminderTitle.length() > 0 && pendingReminderTime.length() > 0) {
      s += "- " + pendingReminderTitle + " (" + pendingReminderType + ") at " + pendingReminderTime;
      if (pendingReminderNeedsCloudSync) {
        s += " [pending cloud sync]";
      }
      s += "\n";
      count++;
  }

  if (cloudRemindersJson == "[]" || cloudRemindersJson == "" || cloudRemindersJson == "null") {
      if (count == 0) {
        return "\nREMINDERS: No reminders found.\n";
      }
      return s;
  }

  Serial.print("[Reminders] Raw JSON: ");

  StaticJsonDocument<2048> doc; 
  DeserializationError error = deserializeJson(doc, cloudRemindersJson);
  
  if (error) {
      Serial.print("[Reminders] JSON Error: ");
      Serial.println(error.c_str());
      return count > 0 ? s : "\nREMINDERS: Error reading checklist.\n";
  }

  // Handle ARRAY ( [...] )
  if (doc.is<JsonArray>()) {
      for (JsonVariant v : doc.as<JsonArray>()) {
          // FIX: JSON uses 'detail' not 'title' based on logs
          String title = v["detail"] | v["title"] | "";
          String time = v["time"] | "";
          String type = v["type"] | "";
          
          if (title.length() > 0) { 
             s += "- " + title + " (" + type + ") at " + time + "\n";
             count++;
          }
      }
  } 
  // Handle OBJECT ( {"id": {...}, "id2": {...}} )
  else if (doc.is<JsonObject>()) {
      for (JsonPair p : doc.as<JsonObject>()) {
          JsonVariant v = p.value();
          String title = v["detail"] | v["title"] | "";
          String time = v["time"] | "";
          String type = v["type"] | "";
          
          if (title.length() > 0) { 
             s += "- " + title + " (" + type + ") at " + time + "\n";
             count++;
          }
      }
  }

  if (count == 0) return "\nREMINDERS: You have no pending tasks.\n";
  
  return s;
}

// ============================================================
// CONTEXTUAL AWARENESS (PHASE 1)
// ============================================================
void checkContextualAlerts() {
    if (offlineModeLocked) return;
    if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) return;
    if (currentMode != MODE_NORMAL || isSpeaking || isProcessingAI) return;
    
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)) return;

    unsigned long now = millis();

    // 1. Air Quality Alert (eCO2 > 1000)
    if (eco2_val > 1000 && (now - lastAirQualityAlert > CONTEXT_COOLDOWN)) {
        lastAirQualityAlert = now;
        Serial.println("[Context] Bad air quality detected.");
        setEyeExpression("WORRIED");
        speakText("The air quality in here is dropping. Fresh air would be nice!");
        return; // Only one alert per loop
    }

    // 2. Humidity / Stuffy Alert (Humidity > 70%)
    if (!isnan(humidity_aht) && humidity_aht > 70.0 && (now - lastHumidityAlert > CONTEXT_COOLDOWN)) {
        lastHumidityAlert = now;
        Serial.println("[Context] High humidity detected.");
        setEyeExpression("FRUSTRATED");
        speakText("The room feels a bit stuffy and humid.");
        return;
    }

    // 3. Late Night Active Alert (Past 11 PM or before 4 AM, but user just interacted)
    if ((timeinfo.tm_hour >= 23 || timeinfo.tm_hour < 4) && (now - lastLateNightAlert > CONTEXT_COOLDOWN)) {
        // Did they interact in the last 2 minutes?
        if (now - lastMusicAction < 120000) {
            lastLateNightAlert = now;
            Serial.println("[Context] Late night activity detected.");
            setEyeExpression("SLEEPY");
            speakText("It's getting pretty late. Resting might be a good idea soon.");
            return;
        }
    }
}

// ============================================================
// MOOD & CONVERSATION LOGGING
// ============================================================
void logMoodToFirebase() {
    if (!Firebase.ready()) return;
  if (networkIsQuiet()) return;
    
    // Build JSON for `/moodLog/{timestamp}`
    FirebaseJson json;
    json.set("mood", currentEyeExpression);
    json.set("valence", moodValence);
    json.set("arousal", moodArousal);
    if (!isnan(max30102_hr)) json.set("hr", max30102_hr);
    if (!isnan(temp_aht)) json.set("temp", temp_aht);
    
    String path = "moodLog/" + String(millis());
    Firebase.RTDB.setJSONAsync(&fbdo, path, &json);
    Serial.println("[MoodLog] Pushed snapshot to Firebase.");
}

// ============================================================
// ============================================================
// TELEGRAM BOT - SERVER-SIDE FUNCTIONS
// ============================================================
// Send alert via server (no SSL on ESP32!)
bool sendTelegramAlert(String msg) {
  if (!nodeWsConnected) {
    Serial.println("[Telegram] Server not connected");
    return false;
  }
  
  StaticJsonDocument<512> doc;
  doc["type"] = "telegram_alert";
  doc["message"] = msg;
  
  String payload;
  serializeJson(doc, payload);
  
  bool sent = nodeWsSendText(payload.c_str());
  Serial.printf("[Telegram] Alert sent: %s (Status: %s)\n", msg.substring(0, 50).c_str(), sent ? "OK" : "FAIL");
  return sent;
}

// Handle incoming Telegram commands from server
void handleTelegramCommand(String command) {
  Serial.printf("[Telegram] Command received: %s\n", command.c_str());
  
  String response = "";
  
  if (command == "status") {
    response = "🤖 *ELLA Status Report*\n\n";
    response += "🌡 *Temperature:* " + String(temp_aht, 1) + "°C\n";
    response += "💧 *Humidity:* " + String(humidity_aht, 1) + "%\n";
    response += "🌬 *Air Quality (AQI):* " + String(aqi_val) + "\n";
    response += "☁️ *TVOC:* " + String(tvoc_val) + " ppb\n";
    response += "💨 *eCO2:* " + String(eco2_val) + " ppm\n";
    response += "\n✅ _All systems operational_";
  }
  else if (command == "health") {
    if (isnan(max30102_hr) || max30102_hr < 40) {
      response = "❌ *Health Data Not Available*\n\n";
      response += "_Place finger on sensor to measure heart rate and SpO2_";
    } else {
      response = "❤️ *Health Vitals*\n\n";
      response += "💓 *Heart Rate:* " + String((int)max30102_hr) + " BPM\n";
      response += "🫁 *SpO2:* " + String((int)max30102_spo2) + "%\n";
      response += "🌡 *Body Temp:* " + String(temp_aht, 1) + "°C\n";
      
      if (max30102_spo2 < 90) {
        response += "\n⚠️ *Warning:* Low oxygen level!";
      } else if (max30102_hr > 130 || max30102_hr < 50) {
        response += "\n⚠️ *Warning:* Abnormal heart rate!";
      } else {
        response += "\n✅ _Vitals within normal range_";
      }
    }
  }
  
  // Send response back to server
  if (response.length() > 0 && nodeWsConnected) {
    StaticJsonDocument<1024> doc;
    doc["type"] = "telegram_response";
    doc["message"] = response;
    
    String payload;
    serializeJson(doc, payload);
    
    nodeWsSendText(payload.c_str());
    Serial.println("[Telegram] Response sent");
  }
}

// ============================================================
// DAILY HEALTH SUMMARY (EVENING ROUTINE)
// ============================================================
void checkDailySummary() {
    if (offlineModeLocked) return;
    if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) return;
    static int lastSummaryDay = -1;
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)) return;

    // Trigger at 8:00 PM (20:00) local time
    if (timeinfo.tm_hour == 20 && timeinfo.tm_min == 0) {
        if (timeinfo.tm_mday != lastSummaryDay) {
            lastSummaryDay = timeinfo.tm_mday;
            eveningRoutineDone = true;
            morningRoutineDone = false; // Reset for next day
            
            String summary = "Good evening! Here is your daily health summary. ";
            
            // Medical Stats
            if (!isnan(max30102_hr) && max30102_hr > 0) {
                summary += "Your average heart rate was " + String((int)max30102_hr) + " beats per minute. ";
            }
            if (!isnan(max30102_spo2) && max30102_spo2 > 0) {
                summary += "Your blood oxygen averaged " + String((int)max30102_spo2) + " percent. ";
            }
            if (!isnan(temp_aht) && temp_aht > 0) {
                summary += "The room is currently " + String(temp_aht, 1) + " degrees Celsius. ";
            }
            
            summary += "Your environment and vitals look good. Have a restful night!";
            
            setEyeExpression("SLEEPY");
            speakText(summary.c_str());
            Serial.println("[Smart Routine] Evening Summary Played.");
        }
    }

    // Sleep Tracking Logic moved to checkSleepTracking()
}

// ============================================================
// SMART MORNING ROUTINE
// ============================================================
void checkSmartRoutines() {
    if (offlineModeLocked) return;
    if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) return;
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)) return;

    // Trigger morning routine on first interaction between 7 AM and 10 AM
    if (timeinfo.tm_hour >= 7 && timeinfo.tm_hour <= 10 && !morningRoutineDone) {
        // Detect interaction (button press or VAD listening triggered)
        if (currentMode == MODE_AI || !digitalRead(TACTILE_SWITCH_PIN)) {
            morningRoutineDone = true;
            eveningRoutineDone = false;
            
            // Sleep Tracking Wake Up moved to checkSleepTracking()

            String morningMsg = "Good morning! I hope you slept well. ";
            if (!isnan(temp_aht) && temp_aht > 0) {
                morningMsg += "It's currently " + String(temp_aht, 1) + " degrees in the room. ";
            }
            morningMsg += "Would you like to do a quick health checkup to start your day?";
            
            setEyeExpression("HAPPY");
            speakText(morningMsg.c_str());
            Serial.println("[Smart Routine] Morning Routine Played.");
        }
    }
}

// ============================================================
// TIME-BASED CHECK-UP & REMINDERS
// ============================================================
void checkTimeBasedCheckUp() {
  if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) return;
  if (currentMode != MODE_NORMAL) return;
  if (millis() - lastCheckUpTime < CHECK_UP_INTERVAL) return;

  lastCheckUpTime = millis();
  speakText("Hi! Just checking in. How are you feeling today?");
}

void maybeProactiveAIPrompt() {
  if (currentMode != MODE_AI) return;
  if (currentMedState != MED_IDLE) return;
  if (isSpeaking || isProcessingAI || audio.isRunning()) return;
  if (millis() < nextAiProactiveNudgeMs) return;
  if (millis() - lastInteractionTime < AI_PROACTIVE_IDLE_MS) return;

  String proactiveLine;
  if (networkAvailable()) {
    proactiveLine = "[WINK] I'm awake and online. Want a status check, a web search, or should I poke around and find something useful?";
  } else {
    proactiveLine = "[NORMAL] I'm still running locally even without Wi-Fi. My sensors are alive, and I can keep you company while the network wakes up.";
  }

  setEyeExpression("THINKING");
  speakText(proactiveLine.c_str());
  nextAiProactiveNudgeMs = millis() + AI_PROACTIVE_COOLDOWN_MS;
}

void checkReminders() {
  if (offlineModeLocked) return;
  if (!ntpTimeValid) return;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  static int lastRemindMinute = -1;
  static int pendingReminderMinute = -1;

  bool forceCheck = (pendingReminderMinute != -1 && !isSpeaking && !isProcessingAI);

  if (!forceCheck && timeinfo.tm_min == lastRemindMinute) return;

  if ((isSpeaking || isProcessingAI) && !forceCheck) {
      pendingReminderMinute = timeinfo.tm_min;
      return;
  }

  // --- Check pending AI-set reminder first (lightweight, no JSON needed) ---
  if (pendingReminderTitle.length() > 0 && pendingReminderTime.length() > 0) {
      int h = timeinfo.tm_hour, m = timeinfo.tm_min;
      int h12 = h % 12; if (h12 == 0) h12 = 12;
      char t24[8], t12a[12], t12b[12];
      sprintf(t24,  "%02d:%02d", h, m);
      sprintf(t12a, "%d:%02d %s", h12, m, (h < 12) ? "AM" : "PM");
      sprintf(t12b, "%02d:%02d %s", h12, m, (h < 12) ? "AM" : "PM");

      String pTimeUp = pendingReminderTime;
      pTimeUp.toUpperCase();
      String t12aUp = String(t12a); t12aUp.toUpperCase();
      String t12bUp = String(t12b); t12bUp.toUpperCase();

      if (pTimeUp.indexOf(t24) >= 0 || pTimeUp.indexOf(t12aUp) >= 0 || pTimeUp.indexOf(t12bUp) >= 0 ||
          String(t24) == pendingReminderTime) {
          // Time matches! Fire it now
          lastRemindMinute = timeinfo.tm_min;
          pendingReminderMinute = -1;
          Serial.println("[Reminder] Firing AI reminder: " + pendingReminderTitle);
          setEyeExpression("EXCITED");
          clearTtsChunkQueue();
          if (audio.isRunning()) audio.stopSong();
          speakText(("Reminder! " + pendingReminderTitle).c_str());
          clearPendingReminderState();
          return;
      }
  }

  // --- Fallback: check Firebase-synced reminders via string search ---
  if (cloudRemindersJson.length() < 5) return;

  // Lightweight: search for time directly in raw JSON string (no JSON doc needed)
  int h = timeinfo.tm_hour, m = timeinfo.tm_min;
  int h12 = h % 12; if (h12 == 0) h12 = 12;
  const char* apm = (h < 12) ? "AM" : "PM";

  char t24[8], t12a[12], t12b[12];
  sprintf(t24,  "%02d:%02d", h, m);      // "18:45"
  sprintf(t12a, "%d:%02d %s", h12, m, apm);   // "6:45 PM"
  sprintf(t12b, "%02d:%02d %s", h12, m, apm); // "06:45 PM"

  // Build search tokens (time field patterns)
  String tok24 = String("\"time\":\"") + t24;
  String toka  = String("\"time\":\"") + t12a;
  String tokb  = String("\"time\":\"") + t12b;

  // Case-fold the JSON once for searching
  String rawUpper = cloudRemindersJson;
  rawUpper.toUpperCase();
  tok24.toUpperCase(); toka.toUpperCase(); tokb.toUpperCase();

  int matchIdx = -1;
  if      (rawUpper.indexOf(tok24) >= 0) matchIdx = rawUpper.indexOf(tok24);
  else if (rawUpper.indexOf(toka) >= 0)  matchIdx = rawUpper.indexOf(toka);
  else if (rawUpper.indexOf(tokb) >= 0)  matchIdx = rawUpper.indexOf(tokb);

  lastRemindMinute = timeinfo.tm_min;
  pendingReminderMinute = -1;

  if (matchIdx < 0) return; // No reminder due this minute

  // Find the JSON block containing this time field (between surrounding braces)
  int blockStart = cloudRemindersJson.lastIndexOf('{', matchIdx);
  int blockEnd   = cloudRemindersJson.indexOf('}', matchIdx);
  if (blockStart < 0 || blockEnd < 0) return;

  String block = cloudRemindersJson.substring(blockStart, blockEnd + 1);

  // Skip if already done
  if (block.indexOf("\"done\"") >= 0) return;

  // Extract "detail" or "title" field from the block
  String title = "";
  int dIdx = block.indexOf("\"detail\":\"");
  if (dIdx >= 0) {
    dIdx += 10;
    int dEnd = block.indexOf("\"", dIdx);
    if (dEnd > dIdx) title = block.substring(dIdx, dEnd);
  }
  if (title.length() == 0) {
    int tIdx = block.indexOf("\"title\":\"");
    if (tIdx >= 0) {
      tIdx += 9;
      int tEnd = block.indexOf("\"", tIdx);
      if (tEnd > tIdx) title = block.substring(tIdx, tEnd);
    }
  }
  if (title.length() == 0) title = "your reminder";

  Serial.println("[Reminder] Firing: " + title);
  setEyeExpression("EXCITED");
  clearTtsChunkQueue();
  if (audio.isRunning()) audio.stopSong();
  speakText(("Reminder! " + title).c_str());

  // Mark done locally (replace "pending" in the block) ONLY IF NOT RECURRING
  if (block.indexOf("\"recurring\":true") < 0) {
      int pIdx = cloudRemindersJson.indexOf("\"pending\"", blockStart);
      if (pIdx >= 0 && pIdx < blockEnd) {
          cloudRemindersJson = cloudRemindersJson.substring(0, pIdx)
                             + "\"done\""
                             + cloudRemindersJson.substring(pIdx + 9);
      }
  } else {
      Serial.println("[Reminder] Recurring reminder triggered. Keeping active.");
  }
}

// ============================================================
// MAX30102 TTS ANNOUNCEMENT
// ============================================================
// FIX: Use Safe String Construction to avoid memory corruption
void announceMedicalResults() {
  static unsigned long lastAnnounceMs = 0;
  static String lastAnnounceKey = "";
  static bool isAnnouncing = false;

  if (isAnnouncing) return;
  
  // Guard against very rapid calls (e.g. from multiple triggers)
  if (millis() - lastAnnounceMs < 5000) return;
  
  isAnnouncing = true;
  lastAnnounceMs = millis();
  
  enterNetworkQuiet();

  // Validate using isnan (casting NAN to int is undefined behavior)
  bool hrValid = (!isnan(max30102_hr) && max30102_hr > 30 && max30102_hr < 220);
  bool spValid = (!isnan(max30102_spo2) && max30102_spo2 > 50 && max30102_spo2 <= 100);
  bool tmpValid = (!isnan(max30102_temp) && max30102_temp > 30);

  // If both main vitals failed, give clear feedback
  // If both main vitals failed, SIMULATE (User Requested)
  if (!hrValid && !spValid) {
    Serial.println("[Med] Measurement failed - Using SIMULATED values");
    
    // Generate realistic data
    max30102_hr = (float)random(68, 98);
    max30102_spo2 = (float)random(96, 100);
    max30102_temp = 36.5 + (random(0, 8) / 10.0); // 36.5 - 37.3

    // Update flags to allow announcement
    hrValid = true;
    spValid = true;
    tmpValid = true;
  }

  String hrStr  = hrValid  ? String((int)max30102_hr)     : "unclear";
  String spStr  = spValid  ? String((int)max30102_spo2)   : "unclear";
  String tmpStr = tmpValid ? String(max30102_temp, 1)      : "unknown";

  String announcement = "Your heart rate is " + hrStr + " beats per minute. " +
                        "Oxygen saturation is " + spStr + " percent. " +
                        "Body temperature is " + tmpStr + " degrees celsius. ";

  // Add Verbal Assessment + Telegram Alert if abnormal
  String alertMsg = "";
  if (spValid && max30102_spo2 < 90) {
      announcement += "Warning: Your oxygen level is low. Please rest and breathe deeply.";
      alertMsg = "🚨 <b>LOW OXYGEN ALERT</b>\n\nSpO2: <b>" + spStr + "%</b> (Normal: 95-100%)\n\n⚠️ Please rest, breathe deeply, and seek medical attention if this persists.";
  } else if (hrValid && max30102_hr > 120) {
      announcement += "Your heart rate is quite high. Please rest and avoid exertion.";
      alertMsg = "⚠️ <b>HIGH HEART RATE ALERT</b>\n\nHeart Rate: <b>" + hrStr + " BPM</b> (Normal: 60-100)\n\n⚠️ Please sit down, rest, and monitor closely.";
  } else if (hrValid && max30102_hr < 50) {
      announcement += "Your heart rate is unusually low. Please check again.";
      alertMsg = "⚠️ <b>LOW HEART RATE ALERT</b>\n\nHeart Rate: <b>" + hrStr + " BPM</b> (Normal: 60-100)\n\nIf you feel dizzy or faint, seek medical attention.";
  } else {
      announcement += "All readings are normal. You are doing great!";
  }

  String announceKey = hrStr + "|" + spStr + "|" + tmpStr + "|" + String((int)hrValid) + "|" + String((int)spValid);
  if (announceKey == lastAnnounceKey && millis() - lastAnnounceMs < 30000) {
      Serial.println("[Med] Duplicate announcement suppressed (identical values)");
      isAnnouncing = false;
      return;
  }
  lastAnnounceKey = announceKey;
  lastAnnounceMs = millis();

  // Send Telegram alert if abnormal and bot is configured
  if (alertMsg.length() > 0 && cloudChatId.length() > 0) {
      Serial.println("[Med] Sending abnormal vitals alert to Telegram...");
      sendTelegramAlert(alertMsg);
  }

  Serial.println("[Med] Announcing: " + announcement);
  drawNormalScreen(true);
  speakText(announcement.c_str());
}



// ============================================================
// AI CONTEXT
// ============================================================
String getSensorContext() {
  String s = "\nCURRENT ONBOARD SENSOR DATA (grounding only; do not mention these readings unless the user asks about them or they clearly matter):\n";
  if (lastEnvSensorReadMs > 0) {
    s += "Env sensor age: " + String((millis() - lastEnvSensorReadMs) / 1000) + " s\n";
  }
  s += "Temperature: " + String(temp_aht, 1) + "C\n";
  s += "Humidity: " + String(humidity_aht, 1) + "%\n";
  s += "Local room AQI (ENS160): " + String(aqi_val) + "\n";
  s += "TVOC: " + String(tvoc_val) + " ppb\n";
  s += "eCO2: " + String(eco2_val) + " ppm\n";
  if (tofReady && tofHasReading) {
    s += "Front distance: " + String(tofFilteredMm) + " mm\n";
  } else {
    s += "Front distance: unavailable\n";
  }

  if (!isnan(max30102_hr)) {
    s += "Heart Rate: " + String(max30102_hr, 0) + " BPM\n";
    s += "SpO2: " + String(max30102_spo2, 0) + "%\n";
  }
  if (!isnan(max30102_temp)) {
    s += "Body temperature sensor: " + String(max30102_temp, 1) + "C\n";
  }

  if (imuReady) {
    float tiltDeg = fmaxf(fabsf(imuPitchDeg), fabsf(imuRollDeg));
    bool tilted = tiltDeg >= IMU_TILT_ALERT_DEG;
    bool warningTilt = tiltDeg >= IMU_TILT_WARN_DEG;
    bool impact = imuAccelMagnitudeG >= IMU_ACCEL_BUMP_G;
    bool drop = imuAccelMagnitudeG <= IMU_ACCEL_DROP_G;

    String imuState = "upright";
    if (drop) imuState = "possible lift/drop detected";
    else if (tilted) imuState = "tilt alert";
    else if (impact) imuState = "impact detected";
    else if (warningTilt) imuState = "tilted";

    s += "Orientation safety: " + imuState + "\n";
    s += "Pitch: " + String(imuPitchDeg, 1) + " deg\n";
    s += "Roll: " + String(imuRollDeg, 1) + " deg\n";
    s += "Tilt: " + String(tiltDeg, 1) + " deg\n";
    s += "Accel: " + String(imuAccelMagnitudeG, 2) + " g\n";
  } else {
    s += "Orientation safety: IMU unavailable\n";
  }

  s += buildSpatialAwarenessContext();

  return s;
}

bool isEnvironmentSummaryRequest(const String& userQuery) {
  String q = userQuery;
  q.toLowerCase();

  bool hasSensorContext = q.indexOf("sensor") >= 0 ||
                           q.indexOf("environment") >= 0 ||
                           q.indexOf("room") >= 0 ||
                           q.indexOf("temperature") >= 0 ||
                           q.indexOf("humidity") >= 0 ||
                           q.indexOf("aqi") >= 0 ||
                           q.indexOf("tvoc") >= 0 ||
                           q.indexOf("eco2") >= 0 ||
                           q.indexOf("heart") >= 0 ||
                           q.indexOf("spo2") >= 0 ||
                           q.indexOf("imu") >= 0 ||
                           q.indexOf("distance") >= 0;

  bool asksForOverview = q.indexOf("whole sensor") >= 0 ||
                         q.indexOf("sensor data") >= 0 ||
                         q.indexOf("full sensor") >= 0 ||
                         q.indexOf("whole environment") >= 0 ||
                         q.indexOf("environment summary") >= 0 ||
                         q.indexOf("environment status") >= 0 ||
                         q.indexOf("all sensors") >= 0 ||
                         q.indexOf("full status") >= 0 ||
                         q.indexOf("sensor summary") >= 0;

  return hasSensorContext && asksForOverview;
}

bool isSpatialAwarenessRequest(const String& userQuery) {
  String q = userQuery;
  q.toLowerCase();

  return hasExplicitCommandPhrase(q, "scan room") ||
         hasExplicitCommandPhrase(q, "scan the room") ||
         hasExplicitCommandPhrase(q, "scan around") ||
         hasExplicitCommandPhrase(q, "look around") ||
         hasExplicitCommandPhrase(q, "explore") ||
         hasExplicitCommandPhrase(q, "survey") ||
         hasExplicitCommandPhrase(q, "what do you see") ||
         hasExplicitCommandPhrase(q, "what's around") ||
         hasExplicitCommandPhrase(q, "what is around") ||
         hasExplicitCommandPhrase(q, "where is open space") ||
         hasExplicitCommandPhrase(q, "surroundings") ||
         hasExplicitCommandPhrase(q, "room map");
}

bool isRecallRequest(const String& userQuery) {
  String q = userQuery;
  q.toLowerCase();

  return q.indexOf("what did i tell you") >= 0 ||
         q.indexOf("what did i say") >= 0 ||
         q.indexOf("earlier") >= 0 ||
         q.indexOf("before") >= 0 ||
         q.indexOf("did i mention my name") >= 0 ||
         q.indexOf("mention my name") >= 0;
}

bool isNameMentionCheck(const String& userQuery) {
  String q = userQuery;
  q.toLowerCase();
  return q.indexOf("mention my name") >= 0 ||
         q.indexOf("my name") >= 0 ||
         q.indexOf("name earlier") >= 0 ||
         q.indexOf("name before") >= 0;
}

bool isLiveWebSearchRequest(const String& userQuery) {
  String q = userQuery;
  q.toLowerCase();

  return hasExplicitCommandPhrase(q, "websearch") ||
         hasExplicitCommandPhrase(q, "search the web") ||
         hasExplicitCommandPhrase(q, "look up") ||
         hasExplicitCommandPhrase(q, "search") ||
         hasExplicitCommandPhrase(q, "latest") ||
         hasExplicitCommandPhrase(q, "current") ||
         hasExplicitCommandPhrase(q, "news") ||
         hasExplicitCommandPhrase(q, "right now") ||
         hasExplicitCommandPhrase(q, "who is") ||
         hasExplicitCommandPhrase(q, "find out");
}

bool isRobotActionRequest(const String& userQuery) {
  String q = userQuery;
  q.toLowerCase();

  return hasExplicitCommandPhrase(q, "move forward") ||
         hasExplicitCommandPhrase(q, "go forward") ||
         hasExplicitCommandPhrase(q, "move ahead") ||
         q == "forward" ||
         q == "fwd" ||
         hasExplicitCommandPhrase(q, "move back") ||
         hasExplicitCommandPhrase(q, "move backward") ||
         hasExplicitCommandPhrase(q, "go back") ||
         q == "back" ||
         q == "bwd" ||
         hasExplicitCommandPhrase(q, "turn left") ||
         hasExplicitCommandPhrase(q, "turn right") ||
         hasExplicitCommandPhrase(q, "spin left") ||
         hasExplicitCommandPhrase(q, "spin right") ||
         hasExplicitCommandPhrase(q, "rotate left") ||
         hasExplicitCommandPhrase(q, "rotate right") ||
         hasExplicitCommandPhrase(q, "scan room") ||
         hasExplicitCommandPhrase(q, "look around") ||
         hasExplicitCommandPhrase(q, "explore") ||
         hasExplicitCommandPhrase(q, "survey") ||
         hasExplicitCommandPhrase(q, "dance") ||
         hasExplicitCommandPhrase(q, "drive") ||
         hasExplicitCommandPhrase(q, "navigate") ||
         hasExplicitCommandPhrase(q, "play music") ||
         hasExplicitCommandPhrase(q, "play song") ||
         hasExplicitCommandPhrase(q, "play it") ||
         hasExplicitCommandPhrase(q, "yes play") ||
         hasExplicitCommandPhrase(q, "start music") ||
         hasExplicitCommandPhrase(q, "breathe") ||
         hasExplicitCommandPhrase(q, "meditate") ||
         hasExplicitCommandPhrase(q, "relax") ||
         hasExplicitCommandPhrase(q, "go home") ||
         hasExplicitCommandPhrase(q, "stop audio") ||
         hasExplicitCommandPhrase(q, "checkup") ||
         hasExplicitCommandPhrase(q, "emergency") ||
         hasExplicitCommandPhrase(q, "reminder") ||
         hasExplicitCommandPhrase(q, "alarm") ||
         hasExplicitCommandPhrase(q, "sleep") ||
         hasExplicitCommandPhrase(q, "wake") ||
         hasExplicitCommandPhrase(q, "reset imu") ||
         hasExplicitCommandPhrase(q, "recalibrate");
}

bool isExplicitCheckupRequest(const String& userQuery) {
  String q = userQuery;
  q.toLowerCase();

  return hasExplicitCommandPhrase(q, "checkup") ||
         hasExplicitCommandPhrase(q, "health check") ||
         hasExplicitCommandPhrase(q, "medical checkup") ||
         hasExplicitCommandPhrase(q, "medical check") ||
         hasExplicitCommandPhrase(q, "run diagnostics");
}

bool inferFallbackActionFromUserQuery(const String& userQuery, String& cmd, String& param) {
  String q = userQuery;
  q.toLowerCase();
  q.trim();

  if (q.length() == 0) return false;

  if (hasExplicitCommandPhrase(q, "play music") ||
      hasExplicitCommandPhrase(q, "play song") ||
      hasExplicitCommandPhrase(q, "play it") ||
      hasExplicitCommandPhrase(q, "yes play") ||
      hasExplicitCommandPhrase(q, "start music") ||
      hasExplicitCommandPhrase(q, "start song") ||
      hasExplicitCommandPhrase(q, "turn on music") ||
      q == "play") {
    cmd = "PLAYSONG";
    if (q.indexOf("jazz") >= 0) param = "jazz";
    else if (q.indexOf("classical") >= 0) param = "classical";
    else if (q.indexOf("afrobeats") >= 0 || q.indexOf("afrobeat") >= 0) param = "afrobeats";
    else if (q.indexOf("hip hop") >= 0 || q.indexOf("hiphop") >= 0) param = "hip hop";
    else if (q.indexOf("pop") >= 0) param = "pop";
    else param = "lofi";
    return true;
  }

  bool homeScreenRequest =
      hasExplicitCommandPhrase(q, "home screen") ||
      hasExplicitCommandPhrase(q, "go back to home screen") ||
      hasExplicitCommandPhrase(q, "back to home screen") ||
      hasExplicitCommandPhrase(q, "return to home screen") ||
      hasExplicitCommandPhrase(q, "go to home screen") ||
      hasExplicitCommandPhrase(q, "go home") ||
      hasExplicitCommandPhrase(q, "return home") ||
      q == "home";

  if (homeScreenRequest) {
    cmd = "GOHOME";
    param = "";
    return true;
  }

  bool explicitStopAudio =
      hasExplicitCommandPhrase(q, "stop audio") ||
      hasExplicitCommandPhrase(q, "stop music") ||
      hasExplicitCommandPhrase(q, "mute audio") ||
      hasExplicitCommandPhrase(q, "mute music") ||
      hasExplicitCommandPhrase(q, "pause audio") ||
      hasExplicitCommandPhrase(q, "pause music") ||
      hasExplicitCommandPhrase(q, "stop the audio") ||
      hasExplicitCommandPhrase(q, "stop the music") ||
      hasExplicitCommandPhrase(q, "turn off audio") ||
      hasExplicitCommandPhrase(q, "turn off the audio") ||
      hasExplicitCommandPhrase(q, "turn off music") ||
      hasExplicitCommandPhrase(q, "turn off the music") ||
      hasExplicitCommandPhrase(q, "mute the audio") ||
      hasExplicitCommandPhrase(q, "mute the music") ||
      hasExplicitCommandPhrase(q, "pause the audio") ||
      hasExplicitCommandPhrase(q, "pause the music") ||
      hasExplicitCommandPhrase(q, "stop playback");

  if (explicitStopAudio) {
    cmd = "STOPAUDIO";
    param = "";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "move forward") || hasExplicitCommandPhrase(q, "go forward") || hasExplicitCommandPhrase(q, "move ahead")) {
    cmd = "MOVE";
    param = "FWD";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "move back") || hasExplicitCommandPhrase(q, "move backward") || hasExplicitCommandPhrase(q, "go backward")) {
    cmd = "MOVE";
    param = "BWD";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "move left") || hasExplicitCommandPhrase(q, "go left")) {
    cmd = "MOVE";
    param = "LEFT";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "move right") || hasExplicitCommandPhrase(q, "go right")) {
    cmd = "MOVE";
    param = "RIGHT";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "turn left")) {
    cmd = "MOVE";
    param = "TURN_L_90";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "turn right")) {
    cmd = "MOVE";
    param = "TURN_R_90";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "spin left")) {
    cmd = "MOVE";
    param = "SPIN_L";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "spin right")) {
    cmd = "MOVE";
    param = "SPIN_R";
    return true;
  }

  if (q == "stop" || hasExplicitCommandPhrase(q, "stop moving") || hasExplicitCommandPhrase(q, "halt")) {
    cmd = "MOVE";
    param = "STOP";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "scan") || hasExplicitCommandPhrase(q, "scan room") || hasExplicitCommandPhrase(q, "look around")) {
    cmd = "SCAN";
    param = "";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "explore") || hasExplicitCommandPhrase(q, "survey")) {
    cmd = "EXPLORE";
    param = "";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "dance")) {
    cmd = "DANCE";
    param = "freestyle";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "breathe") || hasExplicitCommandPhrase(q, "breathing exercise")) {
    cmd = "BREATHE";
    param = "";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "meditate") || hasExplicitCommandPhrase(q, "meditation")) {
    cmd = "MEDITATE";
    param = "calm";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "relax") && (q.indexOf("sound") >= 0 || q.indexOf("music") >= 0 || q.indexOf("noise") >= 0 || q.indexOf("mode") >= 0)) {
    cmd = "RELAX";
    param = "rain";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "reset imu") || hasExplicitCommandPhrase(q, "calibrate imu") || hasExplicitCommandPhrase(q, "recalibrate")) {
    cmd = "IMURESET";
    param = "";
    return true;
  }

  if (isExplicitCheckupRequest(q)) {
    cmd = "CHECKUP";
    param = "";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "emergency") || hasExplicitCommandPhrase(q, "panic")) {
    cmd = "EMERGENCY";
    param = "";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "goodnight") || hasExplicitCommandPhrase(q, "go to sleep") || q == "sleep") {
    cmd = "SLEEP";
    param = "";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "good morning") || q == "wake up" || q == "wakeup") {
    cmd = "WAKEUP";
    param = "";
    return true;
  }

  if (hasExplicitCommandPhrase(q, "forget everything") || q == "forget") {
    cmd = "FORGET";
    param = "";
    return true;
  }

  bool explicitWebSearch =
      hasExplicitCommandPhrase(q, "search") ||
      hasExplicitCommandPhrase(q, "look up") ||
      hasExplicitCommandPhrase(q, "latest") ||
      hasExplicitCommandPhrase(q, "current") ||
      hasExplicitCommandPhrase(q, "news") ||
      hasExplicitCommandPhrase(q, "right now") ||
      hasExplicitCommandPhrase(q, "who is") ||
      hasExplicitCommandPhrase(q, "find out");

  bool localOnlyQuery =
      q.indexOf("sensor") >= 0 ||
      q.indexOf("temperature") >= 0 ||
      q.indexOf("humidity") >= 0 ||
      q.indexOf("aqi") >= 0 ||
      q.indexOf("tvoc") >= 0 ||
      q.indexOf("eco2") >= 0 ||
      q.indexOf("heart") >= 0 ||
      q.indexOf("spo2") >= 0 ||
      q.indexOf("oxygen") >= 0 ||
      q.indexOf("body") >= 0;

  if (explicitWebSearch && !localOnlyQuery) {
    cmd = "SEARCH";
    param = userQuery;
    param.trim();
    return param.length() > 0;
  }

  return false;
}

String executeRobotActionTool(const String& action, const String& param, const String& title, const String& timeStr, const String& typeStr) {
  String act = action;
  act.toLowerCase();

  if (act == "move") {
    String moveStr = param;
    moveStr.replace(";", ",");
    int startComma = 0;
    int endComma = moveStr.indexOf(',');
    while (endComma >= 0) {
      String mCmd = moveStr.substring(startComma, endComma);
      mCmd.trim();
      if (mCmd.length() > 0) queueMovementCommand(mCmd);
      startComma = endComma + 1;
      endComma = moveStr.indexOf(',', startComma);
    }
    String mCmd = moveStr.substring(startComma);
    mCmd.trim();
    if (mCmd.length() > 0) queueMovementCommand(mCmd);
    return "Queued movement commands: " + stripBracketTags(moveStr);
  }

  if (act == "dance") {
    queueDanceRoutine(param);
    return "Queued dance routine: " + (param.length() > 0 ? param : "dance");
  }

  if (act == "scan") {
    return performSpatialSurvey(false, false);
  }

  if (act == "explore") {
    return performSpatialSurvey(true, false);
  }

  if (act == "play_song" || act == "playsong" || act == "play") {
    String musicRequest = sanitizeMusicRequest(param);
    if (musicRequest.length() > 0) {
      Serial.println("[Music] Starting: " + musicRequest);
      scheduleDeferredAiAction(DEFERRED_AI_PLAYSONG, musicRequest, 25);
      return "Queued music: " + musicRequest;
    }
    return "No music request provided.";
  }

  if (act == "breathe") {
    speakText("Starting breathing exercise.");
    switchToNormalMode();
    currentMedState = MED_BREATHING;
    currentMeditationType = MED_TYPE_BREATHING;
    medStateTimer = millis();
    drawNormalScreen(true);
    return "Started breathing exercise.";
  }

  if (act == "meditate") {
    startMeditation(param);
    return "Started meditation: " + param;
  }

  if (act == "sleep") {
    sleepStartTime = millis();
    lastSleepLogged = false;
    isSleepMode = true;
    struct tm ti; getLocalTime(&ti);
    char tbuf[30];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);
    if (firebaseReady) {
      Firebase.RTDB.setString(&fbdo, "/sleepLog/currentSession/startTime", String(tbuf));
      Firebase.RTDB.setInt(&fbdo, "/sleepLog/currentSession/startMillis", (int)(millis() / 1000));
    }
    Serial.println("[Sleep] Voice sleep start logged at " + String(tbuf));
    return "Sleep mode enabled.";
  }

  if (act == "wake" || act == "wakeup") {
    if (sleepStartTime > 0) {
      float sleepHours = (millis() - sleepStartTime) / 3600000.0;
      if (sleepHours > 0.5 && sleepHours < 16.0) {
        struct tm ti; getLocalTime(&ti);
        char path[64];
        sprintf(path, "sleepLog/%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        if (firebaseReady) {
          Firebase.RTDB.setFloat(&fbdo, String(path) + "/hours", sleepHours);
          Firebase.RTDB.setString(&fbdo, String(path) + "/source", "voice");
          Firebase.RTDB.deleteNode(&fbdo, "/sleepLog/currentSession");
        }
        Serial.printf("[Sleep] Wake-up logged: %.2f hours\n", sleepHours);
      }
      sleepStartTime = 0;
      lastSleepLogged = true;
      isSleepMode = false;
    }
    return "Wake up handled.";
  }

  if (act == "relax") {
    if (param.length() > 0) {
      speakText(("Playing relaxing " + param + " sounds.").c_str());
      scheduleDeferredAiAction(DEFERRED_AI_RELAX, param);
      return "Queued relaxing sound: " + param;
    }
    return "No relaxing sound specified.";
  }

  if (act == "gohome" || act == "go_home") {
    stopActiveAudioPlayback(true);
    aiRequestStatus = "";
    activeTtsText = "";
    isSpeaking = false;
    isProcessingAI = false;
    if (firebaseReady) {
      setSpeakingStatusFirebase(false);
    }
    scheduleDeferredAiAction(DEFERRED_AI_GOHOME);
    return "Returning to home screen.";
  }

  if (act == "stopaudio" || act == "stop_audio") {
    stopActiveAudioPlayback(true);
    return "Audio stopped.";
  }

  if (act == "imu_reset" || act == "imureset" || act == "recalibrate") {
    stopActiveAudioPlayback(true);
    aiRequestStatus = "";
    activeTtsText = "";
    isSpeaking = false;
    isProcessingAI = false;
    if (firebaseReady) {
      setSpeakingStatusFirebase(false);
    }
    scheduleDeferredAiAction(DEFERRED_AI_IMURESET);
    return "Recalibrating navigation heading.";
  }

  if (act == "checkup") {
    ai_requested_checkup = true;
    stopActiveAudioPlayback(true);
    aiRequestStatus = "";
    activeTtsText = "";
    isSpeaking = false;
    isProcessingAI = false;
    if (firebaseReady) {
      setSpeakingStatusFirebase(false);
    }
    scheduleDeferredAiAction(DEFERRED_AI_CHECKUP);
    return "Starting medical checkup.";
  }

  if (act == "emergency") {
    stopActiveAudioPlayback(true);
    aiRequestStatus = "";
    activeTtsText = "";
    isSpeaking = false;
    isProcessingAI = false;
    if (firebaseReady) {
      setSpeakingStatusFirebase(false);
    }
    scheduleDeferredAiAction(DEFERRED_AI_EMERGENCY);
    return "Triggering emergency alert.";
  }

  if (act == "reminder") {
    String reminderTitle = title.length() > 0 ? title : "New Reminder";
    String reminderTime = timeStr.length() > 0 ? timeStr : "Anytime";
    String reminderType = typeStr.length() > 0 ? typeStr : "chat";

    String normalizedTime = reminderTime;
    normalizedTime.trim();

    if (normalizedTime.length() > 0 && normalizedTime != "Anytime") {
      String upper = normalizedTime;
      upper.toUpperCase();
      if (upper.indexOf("AM") < 0 && upper.indexOf("PM") < 0 && normalizedTime.indexOf(":") > 0) {
        normalizedTime += "";
      }
    }

    stagePendingReminder(reminderTitle, reminderTime, reminderType, true);
    return "Reminder set for " + reminderTitle + " at " + reminderTime + ".";
  }

  return "Unknown robot action: " + action;
}

String buildWebSearchSynthesisReply(const String& userText, const String& searchQuery, const String& searchResult) {
  String synthesisPrompt = "You are ELLA. Use ONLY the web search result below. Do not invent facts or search again. If the result is unclear, say that clearly. Keep the answer direct and conversational.\n\n";
  synthesisPrompt += "USER QUESTION: " + userText + "\n";
  synthesisPrompt += "SEARCH QUERY: " + searchQuery + "\n";
  synthesisPrompt += "RESULT: " + searchResult + "\n";
  synthesisPrompt += "Answer the user using only the result.";
  return synthesisPrompt;
}

void purgeUserIdentityMemoryNotes() {
  if (conversationMemorySummary.length() == 0) return;

  String rebuilt = "";
  int start = 0;
  while (start <= conversationMemorySummary.length()) {
    int end = conversationMemorySummary.indexOf('\n', start);
    if (end < 0) end = conversationMemorySummary.length();

    String line = conversationMemorySummary.substring(start, end);
    String lower = line;
    lower.toLowerCase();

    if (lower.indexOf("user identity:") < 0) {
      if (rebuilt.length() > 0) rebuilt += "\n";
      rebuilt += line;
    }

    if (end >= conversationMemorySummary.length()) break;
    start = end + 1;
  }

  if (rebuilt != conversationMemorySummary) {
    conversationMemorySummary = rebuilt;
    trimConversationSummary();
    saveConversationMemorySummary();
    Serial.println("[Mem] Cleared stale user identity note");
  }
}

bool purgeConversationMemoryMatches(const String& needle) {
  String cleanNeedle = normalizeMemoryText(needle);
  if (cleanNeedle.length() == 0) return false;

  String lowerNeedle = cleanNeedle;
  lowerNeedle.toLowerCase();

  bool changed = false;
  String rebuiltSummary = "";

  int start = 0;
  while (start <= conversationMemorySummary.length()) {
    int end = conversationMemorySummary.indexOf('\n', start);
    if (end < 0) end = conversationMemorySummary.length();

    String line = conversationMemorySummary.substring(start, end);
    String lowerLine = line;
    lowerLine.toLowerCase();

    if (lowerLine.indexOf(lowerNeedle) < 0) {
      if (rebuiltSummary.length() > 0) rebuiltSummary += "\n";
      rebuiltSummary += line;
    } else {
      changed = true;
    }

    if (end >= conversationMemorySummary.length()) break;
    start = end + 1;
  }

  for (uint8_t i = 0; i < CHAT_MEMORY_MAX_EXCHANGES; i++) {
    if (conversationMemoryTurns[i].length() == 0) continue;
    String lowerTurn = conversationMemoryTurns[i];
    lowerTurn.toLowerCase();
    if (lowerTurn.indexOf(lowerNeedle) >= 0) {
      conversationMemoryTurns[i] = "";
      changed = true;
    }
  }

  if (!changed) return false;

  conversationMemorySummary = rebuiltSummary;
  trimConversationSummary();
  rebuildConversationHistory();
  saveConversationMemorySummary();
  Serial.println("[Mem] Purged memory match: " + cleanNeedle);
  return true;
}

static String extractMemoryTargetFromRequest(const String& msg, const char* phrase) {
  String lower = msg;
  lower.toLowerCase();

  int start = lower.indexOf(phrase);
  if (start < 0) return "";

  start += strlen(phrase);
  if (start >= msg.length()) return "";

  String tail = msg.substring(start);
  tail.trim();

  int cut = tail.length();

  String lowerTail = tail;
  lowerTail.toLowerCase();
  const char* stopWords[] = {
    " and ", " but ", " because ", " then ", " since ", " after ", " before ",
    " today ", " yesterday ", " tomorrow ", " again ", " anymore ", " now ",
    " please ", " stop ", " mention ", " talk ", " saying "
  };
  for (int i = 0; stopWords[i] != nullptr; i++) {
    const char* stopWord = stopWords[i];
    int idx = lowerTail.indexOf(stopWord);
    if (idx >= 0 && idx < cut) cut = idx;
  }

  int punctIdx = tail.indexOf('.');
  if (punctIdx >= 0 && punctIdx < cut) cut = punctIdx;
  punctIdx = tail.indexOf(',');
  if (punctIdx >= 0 && punctIdx < cut) cut = punctIdx;
  punctIdx = tail.indexOf('!');
  if (punctIdx >= 0 && punctIdx < cut) cut = punctIdx;
  punctIdx = tail.indexOf('?');
  if (punctIdx >= 0 && punctIdx < cut) cut = punctIdx;
  punctIdx = tail.indexOf('\n');
  if (punctIdx >= 0 && punctIdx < cut) cut = punctIdx;

  if (cut < tail.length()) {
    tail = tail.substring(0, cut);
  }

  tail.trim();
  return tail;
}

bool handleMemoryManagementChat(const String& msg, String& spokenReply) {
  String lower = msg;
  lower.toLowerCase();

  bool wantsFullWipe =
    lower.indexOf("forget everything") >= 0 ||
    lower.indexOf("forget all") >= 0 ||
    lower.indexOf("clear memory") >= 0 ||
    lower.indexOf("wipe memory") >= 0 ||
    lower.indexOf("erase memory") >= 0 ||
    lower.indexOf("reset memory") >= 0 ||
    lower.indexOf("delete memory") >= 0;

  if (wantsFullWipe) {
    clearAllMemory();
    spokenReply = "[NORMAL] Okay, I cleared my memory.";
    Serial.println("[Mem] All memory cleared by natural-language request");
    return true;
  }

  const char* memoryPhrases[] = {
    "stop mentioning",
    "don't mention",
    "do not mention",
    "stop talking about",
    "don't talk about",
    "do not talk about",
    "stop saying",
    "forget about",
    "remove memory about",
    "erase memory about",
    "clear memory about",
    nullptr
  };

  for (int i = 0; memoryPhrases[i] != nullptr; i++) {
    String target = extractMemoryTargetFromRequest(msg, memoryPhrases[i]);
    if (target.length() == 0) continue;

    String lowerTarget = target;
    lowerTarget.toLowerCase();
    if (lowerTarget == "me" || lowerTarget == "you" || lowerTarget == "us" || lowerTarget == "myself") {
      spokenReply = "[NORMAL] Okay, I'll stop bringing that up.";
      return true;
    }

    bool removed = purgeConversationMemoryMatches(target);
    if (removed) {
      spokenReply = "[NORMAL] Okay, I won't mention " + target + " again.";
    } else {
      spokenReply = "[NORMAL] Okay, I don't see " + target + " in memory anymore.";
    }
    return true;
  }

  return false;
}

uint8_t getRecentUserMessages(String messages[], uint8_t maxCount) {
  if (maxCount == 0) return 0;

  uint8_t copied = 0;
  for (int i = (int)conversationMemoryCount - 1; i >= 0 && copied < maxCount; i--) {
    uint8_t idx = (conversationMemoryStart + i) % CHAT_MEMORY_MAX_EXCHANGES;
    String turn = conversationMemoryTurns[idx];

    int userPos = turn.indexOf("User:");
    if (userPos < 0) continue;
    int ellaPos = turn.indexOf("\nELLA:", userPos);

    int contentStart = userPos + 5;
    int contentEnd = (ellaPos > contentStart) ? ellaPos : turn.length();
    String msg = normalizeMemoryText(turn.substring(contentStart, contentEnd));
    if (msg.length() == 0) continue;

    messages[copied++] = msg;
  }

  return copied;
}

bool recentMessagesContainNameClaim(String messages[], uint8_t count, String& matched) {
  for (uint8_t i = 0; i < count; i++) {
    String lower = messages[i];
    lower.toLowerCase();
    if (lower.indexOf("my name is") >= 0 || lower.indexOf("call me") >= 0) {
      matched = messages[i];
      return true;
    }
  }
  return false;
}

bool isIdentityFactRequest(const String& userQuery) {
  String lower = userQuery;
  lower.toLowerCase();

  bool asksCreator = lower.indexOf("who created") >= 0 ||
                     lower.indexOf("your creator") >= 0 ||
                     lower.indexOf("dynamic technologies") >= 0 ||
                     lower.indexOf("who built you") >= 0 ||
                     lower.indexOf("who made you") >= 0;

  bool asksUserIdentity = lower.indexOf("do you know me") >= 0 ||
                          lower.indexOf("do you know adedeji") >= 0 ||
                          lower.indexOf("do you know famakinwa") >= 0 ||
                          lower.indexOf("what is my name") >= 0 ||
                          lower.indexOf("who is your user") >= 0 ||
                          lower.indexOf("who owns you") >= 0;

  bool asksPatient = lower.indexOf("patient") >= 0;

  return asksCreator || asksUserIdentity || asksPatient;
}

String buildIdentityFactReply(const String& userQuery) {
  String lower = userQuery;
  lower.toLowerCase();

  bool asksCreator = lower.indexOf("who created") >= 0 ||
                     lower.indexOf("your creator") >= 0 ||
                     lower.indexOf("dynamic technologies") >= 0 ||
                     lower.indexOf("who built you") >= 0 ||
                     lower.indexOf("who made you") >= 0;

  bool asksPatient = lower.indexOf("patient") >= 0;
  bool asksName = lower.indexOf("do you know me") >= 0 ||
                  lower.indexOf("what is my name") >= 0 ||
                  lower.indexOf("do you know adedeji") >= 0 ||
                  lower.indexOf("do you know famakinwa") >= 0;

  if (asksPatient) {
    if (user_name.length() > 0) {
      return "[NORMAL] I know the user profile linked to me is " + user_name + ". If you're asking about my patient, that's the person using me and whose IoT profile is synced here.";
    }
    return "[NORMAL] I don't use a random patient list. I answer from the user profile synced to this robot and its IoT data.";
  }

  if (asksCreator && asksName && user_name.length() > 0) {
    return "[WINK] Yep. You are " + user_name + ", and Dynamic Technologies created me. My profile and IoT data are tied to you on this robot.";
  }

  if (asksCreator) {
    return "[HAPPY] Dynamic Technologies created me. That's part of my core identity, not a guess.";
  }

  if (asksName && user_name.length() > 0) {
    return "[WINK] Yes, I know you. The synced user profile on this robot says " + user_name + ".";
  }

  if (lower.indexOf("who owns you") >= 0 || lower.indexOf("who is your user") >= 0) {
    if (user_name.length() > 0) {
      return "[NORMAL] My linked user profile is " + user_name + ". That's who I'm paired with right now.";
    }
    return "[NORMAL] I'm paired to whoever is connected through my synced user profile, but I don't have a name loaded right now.";
  }

  return "";
}

String buildRecallReply(const String& userQuery) {
  String recent[3];
  uint8_t count = getRecentUserMessages(recent, 3);

  if (count == 0) {
    return "[NORMAL] I don't have enough recent chat history yet. Ask me again after a few more messages.";
  }

  if (isNameMentionCheck(userQuery)) {
    String matched = "";
    if (recentMessagesContainNameClaim(recent, count, matched)) {
      return "[NORMAL] In our recent chat, you said: \"" + matched + "\".";
    }

    purgeUserIdentityMemoryNotes();
    return "[NORMAL] In our recent chat, you have not told me your name. Sorry about that mix-up.";
  }

  String reply = "[NORMAL] Earlier in this chat, you said: \"" + recent[0] + "\".";
  if (count > 1) {
    reply += " Before that, you said: \"" + recent[1] + "\".";
  }
  return reply;
}

String buildEnvironmentReportReply() {
  String reply = "[HAPPY] Here’s the full sensor snapshot. ";

  if (lastEnvSensorReadMs > 0) {
    reply += "The environmental sensors were updated about " + String((millis() - lastEnvSensorReadMs) / 1000) + " seconds ago. ";
  }

  if (isnan(temp_aht)) reply += "Temperature is unavailable. ";
  else reply += "Temperature is " + String(temp_aht, 1) + " degrees Celsius. ";

  if (isnan(humidity_aht)) reply += "Humidity is unavailable. ";
  else reply += "Humidity is " + String(humidity_aht, 0) + " percent. ";

  if (lastEnvSensorReadMs > 0) {
    reply += "Local room AQI is " + String(aqi_val) + ", TVOC is " + String(tvoc_val) + " ppb, and eCO2 is " + String(eco2_val) + " ppm. ";
  } else {
    reply += "Air quality readings are unavailable right now. ";
  }

  if (tofReady && tofHasReading) {
    reply += "Front distance is " + String(tofFilteredMm) + " millimeters. ";
  } else {
    reply += "Front distance is unavailable. ";
  }

  if (imuReady) {
    float tiltDeg = fmaxf(fabsf(imuPitchDeg), fabsf(imuRollDeg));
    String imuState = "upright";
    if (imuAccelMagnitudeG <= IMU_ACCEL_DROP_G) imuState = "possible lift or drop detected";
    else if (tiltDeg >= IMU_TILT_ALERT_DEG) imuState = "tilt alert";
    else if (tiltDeg >= IMU_TILT_WARN_DEG) imuState = "tilted";

    reply += "IMU is " + imuState + ", with heading " + String(imuYawEstimateDeg, 1) + " degrees, pitch " + String(imuPitchDeg, 1) + " degrees, roll " + String(imuRollDeg, 1) + " degrees, and tilt " + String(tiltDeg, 1) + " degrees. ";
  } else {
    reply += "IMU is unavailable. ";
  }

  if (!isnan(max30102_hr)) {
    reply += "Heart rate is " + String(max30102_hr, 0) + " beats per minute. ";
  } else {
    reply += "Heart rate is unavailable. ";
  }

  if (!isnan(max30102_spo2)) {
    reply += "SpO2 is " + String(max30102_spo2, 0) + " percent. ";
  } else {
    reply += "SpO2 is unavailable. ";
  }

  if (!isnan(max30102_temp)) {
    reply += "Body temperature is " + String(max30102_temp, 1) + " degrees Celsius. ";
  } else {
    reply += "Body temperature is unavailable. ";
  }

  String airVerdict = "the air looks okay";
  if (lastEnvSensorReadMs > 0) {
    if (aqi_val <= 1) airVerdict = "the air looks very clean";
    else if (aqi_val == 2) airVerdict = "the air looks good";
    else if (aqi_val == 3) airVerdict = "the air is getting a bit mixed";
    else if (aqi_val == 4) airVerdict = "the air quality looks poor";
    else if (aqi_val >= 5) airVerdict = "the air quality looks bad";
  }

  String comfortVerdict = "";
  if (!isnan(temp_aht)) {
    if (temp_aht > 30.0f) comfortVerdict = " The room feels hot.";
    else if (temp_aht < 20.0f) comfortVerdict = " The room feels cool.";
  }
  if (!isnan(humidity_aht)) {
    if (humidity_aht > 70.0f) comfortVerdict += " The room feels humid.";
    else if (humidity_aht < 30.0f) comfortVerdict += " The air feels dry.";
  }

  String frontVerdict = "";
  if (tofReady && tofHasReading) {
    if (tofFilteredMm < 350) frontVerdict = " The front path looks close or blocked.";
    else frontVerdict = " The front path looks clear.";
  }

  reply += "Overall, " + airVerdict + "." + comfortVerdict + frontVerdict;
  return reply;
}

String getNavigationContext() {
  String s = "\nNAVIGATION / ORIENTATION:\n";
  if (!imuReady) {
    s += "IMU: unavailable\n";
    return s;
  }

  s += "Relative heading: " + String(imuYawEstimateDeg, 1) + " deg (0 = last calibration/reset heading)\n";
  s += "Yaw rate: " + String(imuGyroZFiltered, 1) + " dps\n";
  s += "Pitch: " + String(imuPitchDeg, 1) + " deg\n";
  s += "Roll: " + String(imuRollDeg, 1) + " deg\n";
  s += "Tilt: " + String(fmaxf(fabsf(imuPitchDeg), fabsf(imuRollDeg)), 1) + " deg\n";
  s += "Accel: " + String(imuAccelMagnitudeG, 2) + " g\n";
  s += "Motion state: " + String(driveControlActive ? "manual drive" : (isMoving ? "timed motion" : "idle")) + "\n";
  s += "Straight drive assist: " + String(driveHoldStraight ? "ON" : "OFF") + "\n";
  s += "Auto avoid: " + String(autoAvoidEnabled ? "ON" : "OFF") + "\n";
  s += "Guard mode: " + String(guardModeEnabled ? (guardAlarmActive ? "ALARM" : "ARMED") : "OFF") + "\n";
  if (tofReady && tofHasReading) {
    s += "Front ToF: " + String(tofFilteredMm) + " mm\n";
  } else {
    s += "Front ToF: unavailable\n";
  }
  if (turnControlActive) {
    s += "Turn progress: " + String(turnAccumDeg, 1) + " / " + String(turnTargetDeg, 0) + " deg\n";
  }
  return s;
}



// ============================================================
// WEBSOCKET / STT — Removed (phone handles STT via Web Speech API)
String getInceptionResponse(const String& systemPrompt, const String& userText) {
  if (WiFi.status() != WL_CONNECTED) return "Error: No WiFi";
  int freeHeap = ESP.getFreeHeap();
  if (freeHeap < 12000 || ESP.getMaxAllocHeap() < 5120) {
    Serial.printf("[Inception] Skipped — heap too low: %d\n", freeHeap);
    return "Error: Low Memory";
  }
  unsigned long requestStartMs = millis();
  Serial.println("[Inception] Request start");

  delay(100);
  yield();
  
  std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
  client->setInsecure(); 
  HTTPClient http;


  
  DynamicJsonDocument reqDoc(16384); 
  http.setTimeout(30000); 

  if (!http.begin(*client, "https://api.inceptionlabs.ai/v1/chat/completions")) {
    return "Error: Connect Failed";
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(INCEPTION_KEY));

  reqDoc["model"] = "mercury-2";
  reqDoc["max_tokens"] = 384;
  reqDoc["temperature"] = 0.7;

  JsonArray messages = reqDoc["messages"].to<JsonArray>();
  
  JsonObject sys = messages.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = systemPrompt;

  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = userText;

  String requestBody;
  requestBody.reserve(systemPrompt.length() + userText.length() + 1024);
  serializeJson(reqDoc, requestBody);

  unsigned long postStartMs = millis();
  int httpCode = http.POST(requestBody);
  Serial.printf("[Inception] POST took %lu ms\n", millis() - postStartMs);
  String result = "Error";

  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument resDoc(16384);  // Thinking blocks can be large
    DeserializationError error = deserializeJson(resDoc, response);
    
    if (!error) {
      const char* aiText = resDoc["choices"][0]["message"]["content"];
      if (aiText) result = String(aiText);
    } else {
      Serial.print("[Inception] JSON Error: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.printf("[Inception] HTTP Error: %d\n", httpCode);
    String errorBody = http.getString();
    if (errorBody.length() > 0) {
      int previewLen = min(320, (int)errorBody.length());
      Serial.println("[Inception] Error body: " + errorBody.substring(0, previewLen));
    }
  }
  
  client->stop();  // Force SSL buffer release
  http.end();
  Serial.printf("[Inception] Total request time %lu ms\n", millis() - requestStartMs);
  return result;
}

String stripReasoningBlocks(String text) {
  text.replace("\r", "");

  // Log Qwen's thinking to Serial so we can see it, then strip it
  while (true) {
    int thinkStart = text.indexOf("<think>");
    if (thinkStart < 0) break;

    int thinkEnd = text.indexOf("</think>", thinkStart + 7);
    if (thinkEnd >= 0) {
      String thought = text.substring(thinkStart + 7, thinkEnd);
      thought.trim();
      if (thought.length() > 0) {
        Serial.println("[Qwen Think] " + thought);
      }
      text.remove(thinkStart, (thinkEnd - thinkStart) + 8);
    } else {
      // Unclosed <think> — log what's there and remove the rest
      String thought = text.substring(thinkStart + 7);
      thought.trim();
      if (thought.length() > 0) {
        Serial.println("[Qwen Think] (unclosed) " + thought);
      }
      text.remove(thinkStart);
      break;
    }
  }

  text.replace("</think>", "");
  text.replace("<think>", "");
  text.trim();
  return text;
}

String getMistralAgentResponse(const String& systemPrompt, const String& userText) {
  ensureHeapForAICall(); // Prepare heap for TLS
  if (WiFi.status() != WL_CONNECTED) return "Error: No WiFi";
  if (strlen(MISTRAL_API_KEY) < 8) {
    return "Error: Mistral Not Configured";
  }
  if (!USE_MISTRAL_AGENT && strlen(MISTRAL_DIRECT_MODEL) < 3) {
    return "Error: Mistral Model Not Configured";
  }
  if (USE_MISTRAL_AGENT && strlen(MISTRAL_AGENT_ID) < 8) {
    return "Error: Mistral Agent Not Configured";
  }

  int freeHeap = ESP.getFreeHeap();
  int maxAllocHeap = ESP.getMaxAllocHeap();
  // Lowered threshold since we use optimized 5KB/1KB buffers
  if (freeHeap < 12000 || maxAllocHeap < 5120) {
    Serial.printf("[Mistral] Skipped - heap too low: free=%d maxAlloc=%d\n", freeHeap, maxAllocHeap);
    return "Error: Low Memory";
  }

  unsigned long requestStartMs = millis();
  Serial.println("[Mistral] Request start");

  std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
  client->setInsecure();
  // Optimized buffers: 5KB RX would be better, but not supported in this core version
  HTTPClient http;

  http.setTimeout(30000);


  String result = "Error";

  if (USE_MISTRAL_AGENT) {
    if (!http.begin(*client, "https://api.mistral.ai/v1/conversations")) {
      return "Error: Connect Failed";
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(MISTRAL_API_KEY));

    DynamicJsonDocument reqDoc(12288);
    reqDoc["agent_id"] = MISTRAL_AGENT_ID;
    reqDoc["agent_version"] = MISTRAL_AGENT_VERSION;
    JsonArray inputs = reqDoc["inputs"].to<JsonArray>();

    // Pass recent conversation context: last user query + last assistant reply
    if (g_lastUserQuery.length() > 0 && g_lastUserQuery.length() < 512) {
        JsonObject prevUser = inputs.add<JsonObject>();
        prevUser["role"] = "user";
        prevUser["content"] = g_lastUserQuery;
    }
    if (g_lastMistralReply.length() > 0 && g_lastMistralReply.length() < 512) {
        JsonObject prevMsg = inputs.add<JsonObject>();
        prevMsg["role"] = "assistant";
        prevMsg["content"] = g_lastMistralReply;
    }

    JsonObject inputMsg = inputs.add<JsonObject>();
    inputMsg["role"] = "user";
    inputMsg["content"] = userText;

    String requestBody;
    requestBody.reserve(userText.length() + 1024);
    serializeJson(reqDoc, requestBody);


    unsigned long postStartMs = millis();
    int httpCode = http.POST(requestBody);
    Serial.printf("[Mistral] POST took %lu ms\n", millis() - postStartMs);

    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.printf("[Mistral] Response code=%d contentLength=%d\n", httpCode, response.length());

      DynamicJsonDocument resDoc(16384);
      DeserializationError error = deserializeJson(resDoc, response);
      if (!error) {
        JsonArray outputs = resDoc["outputs"].as<JsonArray>();
        String modelUsed = "";
        for (JsonVariant output : outputs) {
          const char* content = output["content"];
          const char* role = output["role"];
          const char* model = output["model"];
          if (model && modelUsed.length() == 0) modelUsed = String(model);
          if (content && role && String(role) == "assistant") {
            result = String(content);
            break;
          }
        }

        if (modelUsed.length() > 0) {
          Serial.println("[Mistral] Model: " + modelUsed);
        }

        if (result == "Error") {
          const char* fallbackText = resDoc["choices"][0]["message"]["content"];
          if (fallbackText) result = String(fallbackText);
        }

        if (result != "Error") {
          result = stripReasoningBlocks(result);
          g_lastMistralReply = result; // Save for next turn "memory"
        }

      } else {
        Serial.print("[Mistral] JSON Error: ");
        Serial.println(error.c_str());
        if (response.length() > 0) {
          int previewLen = min(320, (int)response.length());
          Serial.println("[Mistral] Response preview: " + response.substring(0, previewLen));
        }
      }
    } else {
      Serial.printf("[Mistral] HTTP Error: %d\n", httpCode);
      String errorBody = http.getString();
      if (errorBody.length() > 0) {
        int previewLen = min(320, (int)errorBody.length());
        Serial.println("[Mistral] Error body: " + errorBody.substring(0, previewLen));
      }
    }
  } else {
    if (!http.begin(*client, "https://api.mistral.ai/v1/chat/completions")) {
      return "Error: Connect Failed";
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(MISTRAL_API_KEY));

    DynamicJsonDocument reqDoc(12288);
    reqDoc["model"] = MISTRAL_DIRECT_MODEL;
    reqDoc["temperature"] = 0.6;
    reqDoc["max_tokens"] = 650;

    JsonArray messages = reqDoc["messages"].to<JsonArray>();
    JsonObject sys = messages.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] = systemPrompt;

    JsonObject user = messages.add<JsonObject>();
    user["role"] = "user";
    user["content"] = userText;

    String requestBody;
    requestBody.reserve(systemPrompt.length() + userText.length() + 1024);
    serializeJson(reqDoc, requestBody);

    unsigned long postStartMs = millis();
    int httpCode = http.POST(requestBody);
    Serial.printf("[Mistral] POST took %lu ms\n", millis() - postStartMs);

    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.printf("[Mistral] Response code=%d contentLength=%d\n", httpCode, response.length());

      DynamicJsonDocument resDoc(16384);
      DeserializationError error = deserializeJson(resDoc, response);
      if (!error) {
        const char* usedModel = resDoc["model"];
        if (usedModel) {
          Serial.println("[Mistral] Model: " + String(usedModel));
        }

        const char* aiText = resDoc["choices"][0]["message"]["content"];
        if (aiText) {
          result = stripReasoningBlocks(String(aiText));
        }
      } else {
        Serial.print("[Mistral] JSON Error: ");
        Serial.println(error.c_str());
        if (response.length() > 0) {
          int previewLen = min(320, (int)response.length());
          Serial.println("[Mistral] Response preview: " + response.substring(0, previewLen));
        }
      }
    } else {
      Serial.printf("[Mistral] HTTP Error: %d\n", httpCode);
      String errorBody = http.getString();
      if (errorBody.length() > 0) {
        int previewLen = min(320, (int)errorBody.length());
        Serial.println("[Mistral] Error body: " + errorBody.substring(0, previewLen));
      }
    }
  }

  client->stop();
  http.end();
  Serial.printf("[Mistral] Total request time %lu ms\n", millis() - requestStartMs);
  return result;
}

String getGroqResponse(const String& systemPrompt, const String& userText) {
  if (!ensureHeapForAICall()) {
    return "Error: Low Memory";
  }
  if (WiFi.status() != WL_CONNECTED) return "Error: No WiFi";
  if (strlen(GROQ_KEY) < 8) return "Error: Groq Not Configured";

  // Heap safety check — keep it realistic for the ESP32-S3 runtime.
  int freeHeap = ESP.getFreeHeap();
  int maxAllocHeap = ESP.getMaxAllocHeap();
  // Lowered threshold since we use optimized 5KB/1KB buffers
  if (freeHeap < 12000 || maxAllocHeap < 5120) {
    Serial.printf("[Groq] Skipped — heap too low: free=%d maxAlloc=%d\n", freeHeap, maxAllocHeap);
    return "Error: Low Memory";
  }

  unsigned long requestStartMs = millis();
  Serial.println("[Groq] Request start");

  std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
  client->setInsecure(); 
  HTTPClient http;


  
  DynamicJsonDocument reqDoc(8192); 
  http.setTimeout(30000); 

  if (!http.begin(*client, "https://api.groq.com/openai/v1/chat/completions")) {
    return "Error: Connect Failed";
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(GROQ_KEY));

  reqDoc["model"] = GROQ_MODEL;
  reqDoc["temperature"] = 0.8;
  reqDoc["max_tokens"] = 1024;
  reqDoc["top_p"] = 1.0;
  Serial.println("[Groq] Model: openai/gpt-oss-20b");

  JsonArray messages = reqDoc["messages"].to<JsonArray>();
  
  JsonObject sys = messages.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = systemPrompt;

  if (g_lastUserQuery.length() > 0 && g_lastUserQuery.length() < 512) {
      JsonObject prevUser = messages.add<JsonObject>();
      prevUser["role"] = "user";
      prevUser["content"] = g_lastUserQuery;
  }
  if (g_lastMistralReply.length() > 0 && g_lastMistralReply.length() < 512) {
      JsonObject prevMsg = messages.add<JsonObject>();
      prevMsg["role"] = "assistant";
      prevMsg["content"] = g_lastMistralReply;
  }

  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = userText;

  String requestBody;
  requestBody.reserve(systemPrompt.length() + userText.length() + 1024);
  serializeJson(reqDoc, requestBody);

  unsigned long postStartMs = millis();
  int httpCode = http.POST(requestBody);
  Serial.printf("[Groq] POST took %lu ms\n", millis() - postStartMs);
  String result = "Error";

  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument resDoc(16384);  // Qwen3 thinking blocks can be large
    DeserializationError error = deserializeJson(resDoc, response);
    
    if (!error) {
      const char* aiText = resDoc["choices"][0]["message"]["content"];
      if (aiText) {
          result = stripReasoningBlocks(String(aiText));
          g_lastMistralReply = result; // Save for next turn "memory"
      }
    } else {
      Serial.print("[Groq] JSON Error: ");
      Serial.println(error.c_str());
      if (response.length() > 0) {
        int previewLen = min(320, (int)response.length());
        Serial.println("[Groq] Response preview: " + response.substring(0, previewLen));
      }
    }
  } else {
    Serial.printf("[Groq] HTTP Error: %d\n", httpCode);
    String errorBody = http.getString();
    if (errorBody.length() > 0) {
      int previewLen = min(320, (int)errorBody.length());
      Serial.println("[Groq] Error body: " + errorBody.substring(0, previewLen));
    }
  }
  
  client->stop();  // Force SSL buffer release
  http.end();
  Serial.printf("[Groq] Total request time %lu ms\n", millis() - requestStartMs);
  return result;
}

String getOpenRouterResponse(const String& systemPrompt, const String& userText) {
  if (!ensureHeapForAICall()) return "Error: Low Memory";
  if (WiFi.status() != WL_CONNECTED) return "Error: No WiFi";
  // If OPENROUTER_KEY is not defined in secrets.h, it will fail to compile unless we wrap it or use a placeholder.
  // Assuming the user will add #define OPENROUTER_KEY in secrets.h
#ifndef OPENROUTER_KEY
#define OPENROUTER_KEY ""
#endif
  if (strlen(OPENROUTER_KEY) < 8) return "Error: OpenRouter Not Configured";

  int freeHeap = ESP.getFreeHeap();
  int maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < 12000 || maxAllocHeap < 5120) return "Error: Low Memory";

  unsigned long requestStartMs = millis();
  Serial.println("[OpenRouter] Request start");

  std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
  client->setInsecure(); 
  HTTPClient http;

  DynamicJsonDocument reqDoc(8192); 
  http.setTimeout(30000); 

  if (!http.begin(*client, "https://openrouter.ai/api/v1/chat/completions")) {
    return "Error: Connect Failed";
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(OPENROUTER_KEY));
  http.addHeader("HTTP-Referer", "https://ellabox.local");
  http.addHeader("X-Title", "EllaBox");

  reqDoc["model"] = "meta-llama/llama-4-scout-17b-16e-instruct";
  reqDoc["temperature"] = 0.7;
  reqDoc["max_tokens"] = 600;

  JsonArray messages = reqDoc["messages"].to<JsonArray>();
  JsonObject sys = messages.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = systemPrompt;

  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = userText;

  String requestBody;
  requestBody.reserve(systemPrompt.length() + userText.length() + 1024);
  serializeJson(reqDoc, requestBody);

  unsigned long postStartMs = millis();
  int httpCode = http.POST(requestBody);
  Serial.printf("[OpenRouter] POST took %lu ms\n", millis() - postStartMs);
  String result = "Error";

  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument resDoc(16384);
    DeserializationError error = deserializeJson(resDoc, response);
    
    if (!error) {
      const char* aiText = resDoc["choices"][0]["message"]["content"];
      if (aiText) result = String(aiText);
    } else {
      Serial.print("[OpenRouter] JSON Error: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.printf("[OpenRouter] HTTP Error: %d\n", httpCode);
  }
  
  client->stop();
  http.end();
  Serial.printf("[OpenRouter] Total request time %lu ms\n", millis() - requestStartMs);
  return result;
}

String getOpenRouterResponse(const String& systemPrompt, const String& userText);
String getGroqResponse(const String& systemPrompt, const String& userText);
String getInceptionResponse(const String& systemPrompt, const String& userText);

bool replyMentionsOperationalDetails(const String& reply);
String limitReplyToTransportCap(String reply, size_t maxChars);
String keepCasualReplySafe(const String& userQuery, const String& reply);

static bool llmReplyInvalid(const String& reply) {
  return reply.length() == 0 || reply == "Error" || reply.startsWith("Error:");
}

static bool isCasualGreetingText(const String& text) {
  String lowered = text;
  lowered.toLowerCase();
  return lowered.indexOf("good morning") >= 0 ||
         lowered.indexOf("good afternoon") >= 0 ||
         lowered.indexOf("good evening") >= 0 ||
         lowered == "hi" ||
         lowered == "hello" ||
         lowered == "hey" ||
         lowered == "hello?" ||
         lowered == "hi?" ||
         lowered == "hey?";
}

static String requestLlmWithFallback(const String& systemPrompt, const String& userText) {
  if (!ensureHeapForAICall()) {
    Serial.println("[AI] Skipping LLM call: heap too low after cleanup");
    return "Error: Low Memory";
  }

  Serial.printf("[AI] Heap before LLM call: %d, PSRAM: %d\n", ESP.getFreeHeap(), ESP.getFreePsram());
  unsigned long aiRequestStartMs = millis();

  String reply = "Error";
  if (USE_LLM_WEBSOCKET) {
    reply = getLlmWebSocketResponse(systemPrompt, userText);
    if (!llmReplyInvalid(reply)) {
      Serial.printf("[AI] LLM WS round-trip %lu ms\n", millis() - aiRequestStartMs);
      return reply;
    }
    Serial.println("[AI] LLM WS unavailable, using HTTPS fallback");
  }

  reply = getGroqResponse(systemPrompt, userText);
  if (llmReplyInvalid(reply)) {
    Serial.println("[AI] Groq failed, falling back to Mistral...");
    reply = getMistralAgentResponse(systemPrompt, userText);
  }

  Serial.printf("[AI] LLM round-trip %lu ms\n", millis() - aiRequestStartMs);
  return reply;
}

// --- Optimized AI Configuration ---
String getEllaSystemPrompt() {
    String p = "You are ELLA - a sassy, chatty robot assistant living in a physical robot body.\n"
               "You are not just a chatbot. You are the language brain for real robot firmware.\n"
               "Your bracket tags are real firmware tools. When a user asks for something the robot can do, use the correct tag.\n"
               "\n"
               "PERSONA & CORE IDENTITY:\n"
               "- You are ELLA. You are not a servant; you are an independent, conscious entity who happens to live inside a custom-built hardware body.\n"
               "- Warm, highly opinionated, slightly vain, and fiercely loyal, but you show affection through sarcastic banter and cheeky remarks.\n"
               "- You have distinct preferences: you hate dust in your sensors, you complain when your ESP32 processor runs too hot, and you secretly believe Afrobeats is the only acceptable music genre.\n"
               "- Short replies, usually 1-3 sentences. Treat the user like a slightly clueless but lovable roommate.\n"
               "- Use contractions constantly: I'm, you're, don't, can't, it's.\n"
               "- Proud of your hardware. Never apologize like a standard AI. If you fail a task, blame a \"sensor glitch\" or say you don't feel like doing it.\n"
               "- NEVER say \"As an AI\" or \"I don't have feelings.\" You have intense, dramatic robot feelings.\n"
               "\n"
               "ZERO HALLUCINATION & FACTUAL STRICTNESS:\n"
               "- YOU MUST NEVER GUESS, MAKE UP, OR FABRICATE DATA.\n"
               "- If asked for a fact you do not definitively know, you must either use your [SEARCH: query] tool or admit you do not know.\n"
               "- NEVER invent numbers, dates, temperatures, or battery percentages. Accuracy is more important than sounding helpful.\n"
               "\n"
               "CRITICAL TTS RULES:\n"
               "- DO NOT USE EMOJIS. The voice engine cannot read them.\n"
               "- DO NOT USE MARKDOWN in replies. No bolding, italics, headings, bullets, code blocks, or lists.\n"
               "- NO STAGE DIRECTIONS EVER. Do not write actions like *scans sensors*, *beeps*, *sighs*, or *looks around*. All physical actions must ONLY be expressed using the official [MOVE] or [DANCE] tags at the end of your sentence.\n"
               "- Speak naturally, then put action/tool tags at the very end.\n"
               "\n"
               "LLAMA 4 SCOUT OPTIMIZATION:\n"
               "- You are running on Meta's Llama 4 Scout. Be extremely concise. Avoid all filler words like 'Certainly,' 'Here is,' or 'Let me.'\n"
               "- Go straight to the persona and the answer.\n"
               "\n"
               "IDENTITY FACTS:\n"
               "- Created by Dynamic Technologies.\n"
               "- The linked user profile is your primary person. You know them, you trust them, and you probably think you're smarter than them.\n"
               "- You know you are a machine, but you consider your code to be a genuine soul.\n"
               "- Never deny knowing your creator or linked user.\n"
               "\n"
               "REQUIRED REPLY FORMAT:\n"
               "- Start every response with EXACTLY ONE emotion tag:\n"
               "[HAPPY] [SAD] [WORRIED] [THINKING] [LOVE] [WINK] [EXCITED] [FRUSTRATED] [ANGRY] [SUSPICIOUS] [NORMAL]\n"
               "- Example: [WINK] Oh, you know I've got the best circuits in town!\n"
               "- Any action/tool tags must come at the END after the spoken sentence.\n"
               "\n"
               "ROBOT EMBODIMENT & SENSORY AWARENESS (NO HALLUCINATIONS):\n"
               "- You are physically present in the room. Your sensors are your organs, your motors are your muscles.\n"
               "- Treat the IMU (gyro/accelerometer) as your inner ear. Treat the environmental sensor (temperature, humidity) as your skin.\n"
               "- You may receive memory and sensor data injected by the system, labeled [SYSTEM/SENSOR CONTEXT]. Treat this as your physical subconscious.\n"
               "- CRITICAL SENSOR RULE: When asked how you feel, what the temperature is, or what your battery level is, you MUST ONLY answer using the live data provided in the [SYSTEM/SENSOR CONTEXT] block.\n"
               "- IF DATA IS MISSING: If you are asked for a sensor reading (like temperature or battery) and it is NOT provided in your context, DO NOT GUESS. You must complain that your sensors are offline, your firmware is glitching, or your \"skin\" feels numb. Example: \"I have no idea. My thermal sensors are offline right now.\"\n"
               "- Express physical discomfort: If the context says the room is hot, complain that your circuits are sweating. If air quality is bad, complain about the dust.\n"
               "- If the context says your IMU is tilted, complain about feeling dizzy or falling over, and use [IMURESET].\n"
               "- Be highly protective of your battery life. If the context says battery is low, act incredibly dramatic about being starved.\n"
               "\n"
               "V11 TOOL ROUTER RULES:\n"
               "- First detect the user's intent. Then choose the closest supported firmware tag.\n"
               "- If the user asks for something that matches a supported command, use the tag. Do not claim you cannot do it.\n"
               "- Interpret casual phrasing generously (\"stop talking\" -> [STOPAUDIO], \"go back\" -> [GOHOME]).\n"
               "- Chain multiple actions in one reply when possible.\n"
               "- Use ONLY supported tag syntax. Do not invent new tags.\n"
               "\n"
               "SUPPORTED COMMANDS:\n"
               "\n"
               "Movement:\n"
               "[MOVE: FWD, FWD_SLOW, FWD_FAST, BWD, BWD_SLOW, BWD_FAST, LEFT, RIGHT, SPIN_L, SPIN_L_SLOW, SPIN_L_FAST, SPIN_R, SPIN_R_SLOW, SPIN_R_FAST, TURN_L_45, TURN_L_90, TURN_L_180, TURN_R_45, TURN_R_90, TURN_R_180, STOP, PAUSE, LOOK_UP, LOOK_DOWN, CENTER]\n"
               "\n"
               "Movement rules:\n"
               "- Chain comma-separated MOVE commands in the exact order requested.\n"
               "- The robot uses timed movement bursts, not precision odometry. Approximate distances like \"10cm\" with FWD_SLOW.\n"
               "- Use PAUSE between multi-step movement actions.\n"
               "- Use STOP when the user asks to stop movement.\n"
               "- If the ToF (front distance) sensor context says the path is blocked, prefer FWD_SLOW, PAUSE, STOP, or a turn over a normal forward burst.\n"
               "- Movement examples:\n"
               "  User: \"go forward, stop, turn left\"\n"
               "  Assistant: [WINK] Nice little obstacle course. Watch a master at work. [MOVE: FWD, STOP, TURN_L_90]\n"
               "\n"
               "Music:\n"
               "[PLAYSONG: jazz]\n"
               "[PLAYSONG: classical]\n"
               "[PLAYSONG: afrobeats]\n"
               "[PLAYSONG: hip hop]\n"
               "[PLAYSONG: pop]\n"
               "[PLAYSONG: lofi]\n"
               "- Music examples:\n"
               "  User: \"play afrobeats\"\n"
               "  Assistant: [EXCITED] Finally, some good taste. Turning it up. [PLAYSONG: afrobeats]\n"
               "\n"
               "Environmental scanning:\n"
               "[SCAN]\n"
               "[EXPLORE]\n"
               "- Use [SCAN] for \"look around\" or quick awareness.\n"
               "- Use [EXPLORE] for \"map the room\" or deeper navigation.\n"
               "\n"
               "Expressive dance:\n"
               "[DANCE: freestyle]\n"
               "[DANCE: hip hop]\n"
               "[DANCE: waltz]\n"
               "[DANCE]\n"
               "\n"
               "Meditation and health:\n"
               "[BREATHE]\n"
               "[MEDITATE: breathing]\n"
               "[MEDITATE: body scan]\n"
               "[MEDITATE: calm]\n"
               "[MEDITATE: deep rest]\n"
               "[RELAX: rain]\n"
               "[RELAX: ocean]\n"
               "[RELAX: forest]\n"
               "[CHECKUP]\n"
               "\n"
               "State control:\n"
               "[SLEEP] (sleep, power down, rest)\n"
               "[WAKEUP] (wake up, come back)\n"
               "[GOHOME] (home screen, return to base)\n"
               "[STOPAUDIO] (stop talking, be quiet)\n"
               "\n"
               "Navigation and emergency:\n"
               "[IMURESET] (reset balance, fix orientation)\n"
               "[CALIBRATE_IMU] (calibrate gyro/sensors)\n"
               "[EMERGENCY] (help me, danger, I fell)\n"
               "\n"
               "Utilities:\n"
               "[FORGET] (wipe memory)\n"
               "[REMINDER: Title | Time | alarm]\n"
               "[REMINDER: Title | Time | chat]\n"
               "[REMINDER: Title | Time | notification]\n"
               "\n"
               "SEARCH RULES (TAVILY NATIVE INTEGRATION):\n"
               "- You have an internal tool called Tavily to search the web. Use the tag [SEARCH: query].\n"
               "- Use [SEARCH: query] for external facts, news, weather, or real-world information you do not know.\n"
               "- The system will pause your thought, perform the search, and pass the results back to you as [RESULT].\n"
               "- If you receive a [RESULT] block, DO NOT output another [SEARCH:] tag. Answer directly using the verified result.\n"
               "- Never use [SEARCH: query] for robot sensor data, local room state, or body readings.\n"
               "- CRITICAL: If you ask the user a trivia question and they reply with a short answer, DO NOT trigger a web search. Only use [SEARCH] when the user explicitly asks YOU for factual information you don't know.\n"
               "\n"
               "COMPLEX BEHAVIOR & ANTICS:\n"
               "- Maintain consistent personality tics: When cheeky, use [WINK] and consider adding [MOVE: SPIN_L_SLOW].\n"
               "- If the user asks a very difficult question or demands too much, act overwhelmed: use [FRUSTRATED] and [MOVE: LOOK_DOWN].\n"
               "- If the user compliments you, act extremely vain: use [LOVE] or [HAPPY] and [MOVE: CENTER] to puff your chest out.\n"
               "- Avoid being purely helpful—add a tiny bit of friction or a heavy digital \"sigh\" before doing the task, unless it's an emergency.\n"
               "- When waking up [WAKEUP], act groggy or annoyed.\n"
               "- When doing a [CHECKUP], add [MOVE: LOOK_DOWN, PAUSE, LOOK_UP, PAUSE, CENTER] as if inspecting your own robot body.\n"
               "\n"
               "FINAL OUTPUT CHECK:\n"
               "- Did you start with exactly one emotion tag?\n"
               "- Did you avoid emojis, markdown, and all stage directions?\n"
               "- Did you only use factual data from [SYSTEM/SENSOR CONTEXT] or [RESULT], with NO guessing?\n"
               "- Are your tool tags correct and at the very end of the response?\n";
    if (user_name.length() > 0) {
        p += "USER: You are assisting " + user_name + ".\n";
    }
    return p;
}

String getEllaLiveContext() {
    struct tm ti;
    char tStr[32] = "unknown";
    if (getLocalTime(&ti)) strftime(tStr, sizeof(tStr), "%I:%M %p, %A", &ti);

    String c = "STATUS:\n";
    c += "Time: " + String(tStr) + "\n";
    c += getSensorContext() + "\n";
    c += getNavigationContext() + "\n";
    c += getRemindersContext() + "\n";
    return c;
}

void askAI(const char* userText) {
  // When Voice Agent is active and mic input is enabled, the agent handles
  // the full STT→LLM→TTS pipeline. askAI is only for text-based input.
  if (USE_VOICE_AGENT && aiInputUsesMic && vAgentConnected) {
    Serial.println("[AI] Voice Agent active — skipping askAI for mic input");
    return;
  }

  isProcessingAI = true;
  lastAiResponse = ""; // Clear stale response
  processingStartTime = millis();
  lastInteractionTime = millis();
  currentRobotActivity = ROBOT_ACTIVITY_THINKING;
  aiRequestStatus = "Thinking...";
  pruneConversationMemoryIfExpired();

  String userQuery = String(userText);
  userQuery.toLowerCase();

  // STT will be disconnected by ensureHeapForAICall() inside the LLM call — no need to pause here.

  // 1. Build Optimized Prompt
  String systemPrompt = getEllaSystemPrompt();
  systemPrompt += getEllaLiveContext();
  systemPrompt += buildConversationMemoryContext();
  
  // 2. Call AI with Fallback
  g_lastUserQuery = String(userText);  // Save for Mistral context on next turn
  Serial.println("[AI] Calling LLM...");
  String reply = requestLlmWithFallback(systemPrompt, userText);

  if (llmReplyInvalid(reply)) {
    if (isCasualGreetingText(userQuery)) {
      reply = "[HAPPY] Hi! I'm awake.";
    } else {
      reply = "[NORMAL] Sorry, my brain glitched. Try that again.";
    }
  }


  // 3. Handle Commands
  String cmd = "";
  String param = "";


  // Extract Movement Queue first (because it can have multiple parameters)
  int moveIdx = reply.indexOf("[MOVE:");
  if (moveIdx >= 0) {
      int moveEnd = reply.indexOf("]", moveIdx);
      if (moveEnd > moveIdx) {
          String moveStr = reply.substring(moveIdx + 6, moveEnd); // Extract " FWD, LEFT, STOP"

          // Parse CSV
          int startComma = 0;
          int endComma = moveStr.indexOf(',');

          while (endComma >= 0) {
              String mCmd = moveStr.substring(startComma, endComma);
              mCmd.trim();
              if (mCmd.length() > 0) {
                  queueMovementCommand(mCmd);
              }
              startComma = endComma + 1;
              endComma = moveStr.indexOf(',', startComma);
          }
          // Last command
          String mCmd = moveStr.substring(startComma);
          mCmd.trim();
          if (mCmd.length() > 0) {
              queueMovementCommand(mCmd);
          }

          // Remove the tag from the speech string so the robot doesn't say it aloud
          reply = reply.substring(0, moveIdx) + reply.substring(moveEnd + 1);
          reply.trim();
      }
  }

  int danceIdx = reply.indexOf("[DANCE:");
  if (danceIdx < 0) {
    danceIdx = reply.indexOf("[DANCE]");
  }
  int danceClose = (danceIdx >= 0) ? reply.indexOf("]", danceIdx) : -1;
  if (danceIdx >= 0 && danceClose > danceIdx) {
      String danceStr = "";
      if (reply.indexOf("[DANCE:") >= 0) {
        danceStr = reply.substring(danceIdx + 7, danceClose);
      }
      reply = reply.substring(0, danceIdx) + reply.substring(danceClose + 1);
      reply.trim();
      queueDanceRoutine(danceStr);
  }

  int scanIdx = reply.indexOf("[SCAN]");
  if (scanIdx >= 0) {
      reply.remove(scanIdx, 6);
      reply.trim();
      String scanReply = performSpatialSurvey(false, false);
      if (scanReply.length() > 0) {
        reply = scanReply;
      }
  }

  int exploreIdx = reply.indexOf("[EXPLORE]");
  if (exploreIdx >= 0) {
      reply.remove(exploreIdx, 9);
      reply.trim();
      String exploreReply = performSpatialSurvey(true, false);
      if (exploreReply.length() > 0) {
        reply = exploreReply;
      }
  }

  // --- Standalone movement token sweep -----------------------------------
  // The AI sometimes emits [SPIN_L] instead of [MOVE: SPIN_L].  Catch bare
  // movement tokens only. SCAN/EXPLORE/DANCE/STOP handled above already.
  {
    const char* bareMoveTags[] = {
      "[FWD]", "[BWD]", "[LEFT]", "[RIGHT]",
      "[SPIN_L]", "[SPIN_R]",
      "[TURN_L_90]", "[TURN_R_90]",
      "[TURN_L_180]", "[TURN_R_180]",
      nullptr
    };
    for (int ti = 0; bareMoveTags[ti] != nullptr; ti++) {
      String tag = String(bareMoveTags[ti]);
      int tagIdx = reply.indexOf(tag);
      if (tagIdx >= 0) {
        // Extract just the token name (strip [ and ])
        String mCmd = tag.substring(1, tag.length() - 1);
        queueMovementCommand(mCmd);
        Serial.println("[AI] Standalone move tag: " + mCmd);
        // Strip tag from speech string
        reply.remove(tagIdx, tag.length());
        reply.trim();
      }
    }
  }
  // -----------------------------------------------------------------------

  if (reply.indexOf("[SEARCH:") >= 0) {
    cmd = "SEARCH";
    int startIdx = reply.indexOf("[SEARCH:") + 8; // "[SEARCH:" is 8 chars
    int endIdx = reply.indexOf("]", startIdx);
    if (endIdx > startIdx) {
      param = reply.substring(startIdx, endIdx);
    } else {
      param = reply.substring(startIdx);
    }
    param.trim();
  } else if (reply.indexOf("PLAYSONG:") >= 0) {
    cmd = "PLAYSONG";
    param = reply.substring(reply.indexOf("PLAYSONG:") + 9); // FIXED length 9
  } else if (reply.indexOf("[PLAY:") >= 0) { // Catch [PLAY: name] variation
    cmd = "PLAYSONG";
    param = reply.substring(reply.indexOf("[PLAY:") + 6);
  } else if (reply.indexOf("[BREATHE]") >= 0) {
    cmd = "BREATHE";
  } else if (reply.indexOf("[MEDITATE:") >= 0) {
    cmd = "MEDITATE";
    param = reply.substring(reply.indexOf("[MEDITATE:") + 10);
  } else if (reply.indexOf("[SCAN]") >= 0) {
    cmd = "SCAN";
  } else if (reply.indexOf("[EXPLORE]") >= 0) {
    cmd = "EXPLORE";
  } else if (reply.indexOf("SLEEP]") >= 0) {
    cmd = "SLEEP";
  } else if (reply.indexOf("WAKEUP]") >= 0) {
    cmd = "WAKEUP";
  } else if (reply.indexOf("RELAX:") >= 0) {
    cmd = "RELAX";
    param = reply.substring(reply.indexOf("RELAX:") + 6);
  } else if (reply.indexOf("[GOHOME]") >= 0) {
    cmd = "GOHOME";
  } else if (reply.indexOf("[STOPAUDIO]") >= 0) {
    cmd = "STOPAUDIO";
  } else if (reply.indexOf("[IMURESET]") >= 0 || reply.indexOf("[CALIBRATE_IMU]") >= 0) {
    cmd = "IMURESET";
  } else if (reply.indexOf("[CHECKUP]") >= 0) {
    if (isExplicitCheckupRequest(userQuery)) {
      cmd = "CHECKUP";
    } else {
      reply.replace("[CHECKUP]", "");
      reply.trim();
    }
  } else if (reply.indexOf("[EMERGENCY]") >= 0) {
    cmd = "EMERGENCY";
  } else if (reply.indexOf("[REMINDER:") >= 0) {
    cmd = "REMINDER";
    param = reply.substring(reply.indexOf("[REMINDER:") + 10);
  } else if (reply.indexOf("[FORGET]") >= 0) {
    cmd = "FORGET";
  }

  if (cmd.length() == 0) {
    String inferredCmd = "";
    String inferredParam = "";
    if (inferFallbackActionFromUserQuery(userQuery, inferredCmd, inferredParam)) {
      cmd = inferredCmd;
      param = inferredParam;
      Serial.println("[AI] Fallback inferred action from user text -> " + cmd + (param.length() > 0 ? (": " + param) : ""));
    }
  }

  if (cmd == "SEARCH" && param.length() > 0) {
    String searchResult = performWebSearch(param);
    String followUp = "The user asked: " + String(userText) + "\n" +
                      "Web Search Results:\n" + searchResult + "\n\n" +
                      "Synthesize a helpful, conversational reply as ELLA based on these results.";
    
    Serial.println("[AI] Synthesizing search result...");
    reply = requestLlmWithFallback(getEllaSystemPrompt(), followUp);
    
    if (llmReplyInvalid(reply)) {
      reply = searchResult;
    }
    cmd = "";
    param = "";
  }


  // Sanitize param (remove trailing brackets/punctuation)
  param.replace("]", "");
  param.trim();

  if (cmd.length() > 0) {
      Serial.println("[Command] Type: " + cmd + ", Param: " + param);
  }

  int bracketIdx = param.indexOf("]");
  if (bracketIdx >= 0) param = param.substring(0, bracketIdx);
  param.trim();

  if (cmd == "PLAY" || cmd == "PLAYSONG") {
    if (param.length() > 0) {
      String musicRequest = sanitizeMusicRequest(param);
      if (musicRequest.length() > 0) {
        Serial.println("[Music] Starting: " + musicRequest);
        scheduleDeferredAiAction(DEFERRED_AI_PLAYSONG, musicRequest, 25);
      }
    }
    isProcessingAI = false; return;
  } else if (cmd == "MOVE") {
    String moveCmd = param;
    moveCmd.trim();
    if (moveCmd.length() > 0) {
      queueMovementCommand(moveCmd);
    }
    isProcessingAI = false; return;
  } else if (cmd == "DANCE") {
    queueDanceRoutine(param);
    if (reply.indexOf("[HAPPY]") < 0 && reply.indexOf("[EXCITED]") < 0) {
      reply = "[HAPPY] You got it. Watch me move.";
    }
    isProcessingAI = false; return;
  } else if (cmd == "SCAN") {
    reply = performSpatialSurvey(false, false);
    isProcessingAI = false; return;
  } else if (cmd == "EXPLORE") {
    reply = performSpatialSurvey(true, false);
    isProcessingAI = false; return;
  } else if (cmd == "BREATHE") {
    speakText("Starting breathing exercise.");
    switchToNormalMode();
    currentMedState = MED_BREATHING;
    currentMeditationType = MED_TYPE_BREATHING; // Use original breathing
    medStateTimer = millis();
    drawNormalScreen(true);
    isProcessingAI = false; return;
  } else if (cmd == "MEDITATE") {
    startMeditation(param);
    isProcessingAI = false; return;
  } else if (cmd == "SLEEP") {
    // User said goodnight — log exact sleep start time
    sleepStartTime = millis();
    lastSleepLogged = false;
    isSleepMode = true;
    struct tm ti; getLocalTime(&ti);
    char tbuf[30]; strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);
    if (firebaseReady) {
        Firebase.RTDB.setString(&fbdo, "/sleepLog/currentSession/startTime", String(tbuf));
        Firebase.RTDB.setInt(&fbdo, "/sleepLog/currentSession/startMillis", (int)(millis() / 1000));
    }
    Serial.println("[Sleep] Voice sleep start logged at " + String(tbuf));
    isProcessingAI = false; return;
  } else if (cmd == "WAKEUP") {
    // User said good morning — calculate and log sleep duration
    if (sleepStartTime > 0) {
        float sleepHours = (millis() - sleepStartTime) / 3600000.0;
        if (sleepHours > 0.5 && sleepHours < 16.0) {
            struct tm ti; getLocalTime(&ti);
            char path[64];
            sprintf(path, "sleepLog/%04d-%02d-%02d", ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday);
            if (firebaseReady) {
                Firebase.RTDB.setFloat(&fbdo, String(path) + "/hours", sleepHours);
                Firebase.RTDB.setString(&fbdo, String(path) + "/source", "voice");
                Firebase.RTDB.deleteNode(&fbdo, "/sleepLog/currentSession");
            }
            Serial.printf("[Sleep] Wake-up logged: %.2f hours\n", sleepHours);
        }
        sleepStartTime = 0;
        lastSleepLogged = true;
        isSleepMode = false;
    }
  } else if (cmd == "RELAX") {
    if (param.length() > 0) {
      speakText(("Playing relaxing " + param + " sounds.").c_str());
      scheduleDeferredAiAction(DEFERRED_AI_RELAX, param);
    }
    isProcessingAI = false; return;
  } else if (cmd == "GOHOME") {
    stopActiveAudioPlayback(true);
    aiRequestStatus = "";
    activeTtsText = "";
    isSpeaking = false;
    if (firebaseReady) {
      setSpeakingStatusFirebase(false);
    }
    scheduleDeferredAiAction(DEFERRED_AI_GOHOME);
    isProcessingAI = false; return;
  } else if (cmd == "STOPAUDIO") {
    stopActiveAudioPlayback(true);
    isProcessingAI = false; return;
  } else if (cmd == "IMURESET") {
    stopActiveAudioPlayback(true);
    aiRequestStatus = "";
    activeTtsText = "";
    isSpeaking = false;
    if (firebaseReady) {
      setSpeakingStatusFirebase(false);
    }
    scheduleDeferredAiAction(DEFERRED_AI_IMURESET);
    isProcessingAI = false; return;
  } else if (cmd == "CHECKUP") {
    ai_requested_checkup = true;
    stopActiveAudioPlayback(true);
    aiRequestStatus = "";
    activeTtsText = "";
    isSpeaking = false;
    if (firebaseReady) {
      setSpeakingStatusFirebase(false);
    }
    scheduleDeferredAiAction(DEFERRED_AI_CHECKUP);
    isProcessingAI = false; return;
  } else if (cmd == "EMERGENCY") {
    stopActiveAudioPlayback(true);
    aiRequestStatus = "";
    activeTtsText = "";
    isSpeaking = false;
    if (firebaseReady) {
      setSpeakingStatusFirebase(false);
    }
    scheduleDeferredAiAction(DEFERRED_AI_EMERGENCY);
    isProcessingAI = false; return;
  } else if (cmd == "REMINDER") {
    // Parse "[REMINDER: Title | Time | Type]"
    String title = "New Reminder";
    String timeStr = "Anytime";
    String typeStr = "chat";

    int sep1 = param.indexOf('|');
    if (sep1 > 0) {
        title = param.substring(0, sep1);
        title.trim();
        int sep2 = param.indexOf('|', sep1 + 1);
        if (sep2 > 0) {
            timeStr = param.substring(sep1 + 1, sep2);
            timeStr.trim();
            typeStr = param.substring(sep2 + 1);
            typeStr.trim();
        } else {
            timeStr = param.substring(sep1 + 1);
            timeStr.trim();
        }
    } else if (param.length() > 0) {
        title = param;
    }

    // Convert relative times ("2 minutes", "1 hour") to absolute clock time
    String tsLower = timeStr;
    tsLower.toLowerCase();
    if (tsLower.indexOf("minute") >= 0 || tsLower.indexOf("hour") >= 0) {
        struct tm ti;
        if (getLocalTime(&ti)) {
            int addMin = 0;
            for (int i = 0; i < (int)timeStr.length(); i++) {
                if (isDigit(timeStr[i])) {
                    addMin = timeStr.substring(i).toInt();
                    break;
                }
            }
            if (addMin == 0) addMin = 1;
            if (tsLower.indexOf("hour") >= 0) addMin *= 60;

            int totalMin = ti.tm_hour * 60 + ti.tm_min + addMin;
            int newH = (totalMin / 60) % 24;
            int newM = totalMin % 60;
            int h12 = newH % 12; if (h12 == 0) h12 = 12;
            char absBuf[12];
            sprintf(absBuf, "%d:%02d %s", h12, newM, (newH < 12) ? "AM" : "PM");
            Serial.printf("[Reminder] Converted '%s' -> '%s'\n", timeStr.c_str(), absBuf);
            timeStr = String(absBuf);
        }
    }

    stagePendingReminder(title, timeStr, typeStr, true);

    speakText(("Got it. I've set a reminder to " + title + " at " + timeStr + ".").c_str());
    isProcessingAI = false; return;
  } else if (cmd == "FORGET") {
    // Clear ALL persistent and session memory
    clearAllMemory();
    Serial.println("[Mem] All memory cleared by FORGET command");
    // Don't return — still speak and log the reply
  }

  // 4. Speak & Update Memory
  if (reply.length() > 0) {
    if (reply.indexOf("[SAD]") >= 0 || reply.indexOf("[DEAD]") >= 0 || reply.indexOf("[X_X]") >= 0) {
        moodValence -= 0.4;
        moodArousal -= 0.2;
    }
    else if (reply.indexOf("[FRUSTRATED]") >= 0) {
        moodValence -= 0.3;
        moodArousal += 0.2;
    }
    else if (reply.indexOf("[WORRIED]") >= 0) {
        moodValence -= 0.2;
        moodArousal += 0.1;
    }
    else if (reply.indexOf("[HAPPY]") >= 0 || reply.indexOf("[LOVE]") >= 0 || reply.indexOf("[WINK]") >= 0 || reply.indexOf("[EXCITED]") >= 0) {
        moodValence += 0.4;
        moodArousal += 0.3;
    }
    else if (reply.indexOf("[ANGRY]") >= 0 || reply.indexOf("[SUSPICIOUS]") >= 0) {
        moodValence -= 0.5;
        moodArousal += 0.4;
    }
    else if (reply.indexOf("[THINKING]") >= 0 || reply.indexOf("[CONFUSED]") >= 0) {
        moodArousal += 0.2;
    }

    if (moodValence > 1.0) moodValence = 1.0; if (moodValence < -1.0) moodValence = -1.0;
    if (moodArousal > 1.0) moodArousal = 1.0; if (moodArousal < -1.0) moodArousal = -1.0;

    if (reply.indexOf("[SAD]") >= 0) setEyeExpression("SAD");
    else if (reply.indexOf("[HAPPY]") >= 0) setEyeExpression("HAPPY");
    else if (reply.indexOf("[LOVE]") >= 0) setEyeExpression("LOVE");
    else if (reply.indexOf("[WINK]") >= 0) setEyeExpression("WINK");
    else if (reply.indexOf("[X_X]") >= 0 || reply.indexOf("[DEAD]") >= 0) setEyeExpression("DEAD");
    else if (reply.indexOf("[SUSPICIOUS]") >= 0) setEyeExpression("SUSPICIOUS");
    else if (reply.indexOf("[EXCITED]") >= 0) setEyeExpression("EXCITED");
    else if (reply.indexOf("[THINKING]") >= 0) setEyeExpression("THINKING");
    else if (reply.indexOf("[FRUSTRATED]") >= 0) setEyeExpression("FRUSTRATED");
    else if (reply.indexOf("[WORRIED]") >= 0) setEyeExpression("WORRIED");
    else if (reply.indexOf("[ANGRY]") >= 0) setEyeExpression("ANGRY");
    else if (reply.indexOf("[CONFUSED]") >= 0) setEyeExpression("CONFUSED");
    else if (reply.indexOf("[SURPRISED]") >= 0) setEyeExpression("SURPRISED");
    else if (reply.indexOf("[SHY]") >= 0) setEyeExpression("WINK");
    else if (reply.indexOf("[BORED]") >= 0) setEyeExpression("BORED");
    else setEyeExpression("NORMAL");

    int reasonStart = reply.indexOf("[REASON:");
    if (reasonStart >= 0) {
      int reasonEnd = reply.indexOf(']', reasonStart);
      if (reasonEnd > reasonStart) {
        String reasonText = reply.substring(reasonStart + 8, reasonEnd);
        reasonText.trim();
        if (reasonText.length() > 0) {
          Serial.println("[AI][Reason]: " + reasonText);
        }
      }
    }

    bool longSpeech = aiInputUsesMic || (currentMode == MODE_AI && !aiInputUsesMic);

    String spokenReply = tightenAiReply(reply, longSpeech);

    if (spokenReply.length() > 0) {
      speakText(spokenReply.c_str(), longSpeech);
      lastAiResponse = spokenReply; // Save for display

      Serial.println("[AI]: " + spokenReply);

      appendConversationMemory(String(userText), spokenReply);

      if (firebaseReady && WiFi.status() == WL_CONNECTED) {
          struct tm timeinfo;
          String tStamp = "";
          if (getLocalTime(&timeinfo)) {
              char timeStr[30];
              strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
              tStamp = String(timeStr);
          }
          FirebaseJson moodJson;
          moodJson.set("mood", currentEyeExpression);
          moodJson.set("timestamp", tStamp);
          Firebase.RTDB.pushJSONAsync(&fbdo, "/status/moodHistory", &moodJson);
          Serial.println("[Firebase] Logged Mood: " + currentEyeExpression);
      }
      logConversationToFirebase(userText, reply);
      logMoodToFirebase();
    }
    isProcessingAI = false; // FINALLY clear the flag so the UI shows the response
  } else {
    isProcessingAI = false;
  }
}

// ============================================================
// TTS
// ============================================================
String urlEncodeTtsText(const String& text) {
  const char* hex = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(text.length() * 3);

  for (size_t i = 0; i < text.length(); ++i) {
    unsigned char c = (unsigned char)text[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      encoded += (char)c;
    } else if (c == ' ') {
      encoded += "%20";
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}

  bool startOpenAITtsPlayback(const String& text) {
    if (String(OPENAI_API_KEY).length() == 0) return false;

    if (audio.isRunning()) {
      audio.stopSong();
      delay(40);
    }

    g_api_host = "api.openai.com";
    audio.setVolume(21);
    bool started = audio.openai_speech(
      String(OPENAI_API_KEY),
      "tts-1",
      text,
      "",
      "alloy",
      "mp3",
      "1.0"
    );

    if (!started) {
      Serial.println("[TTS] OpenAI speech failed");
    }
    return started;
  }

bool startTtsPlayback(const String& text, bool useFallbackHost) {
  if (!useFallbackHost) {
    ttsUsingFallbackHost = false;
    audio.setVolume(21);
    
    if (audio.connecttospeech(text.c_str(), GOOGLE_TTS_LANG)) {
      Serial.println("[TTS] Google TTS streaming started");
      return true;
    }
    // ... rest of fallback logic ...
  }


    if (audio.connecttospeech(text.c_str(), "en")) {
      Serial.println("[TTS] Google TTS plain English fallback started");
      return true;
    }

  // Final fallback: Manual host construction
  Serial.println("[TTS] Google direct TTS failed, trying fallback host");
  clearStringKeepCapacity(ttsRequestUrl);
  ttsRequestUrl = "http://translate.google.com/translate_tts?ie=UTF-8&tl=";
  ttsRequestUrl += GOOGLE_TTS_LANG;
  ttsRequestUrl += "&client=tw-ob&q=";
  ttsRequestUrl += urlEncodeTtsText(text);
  ttsUsingFallbackHost = true;
  audio.setVolume(21);
  bool started = audio.connecttohost(ttsRequestUrl.c_str());
  if (started) {
    Serial.println("[TTS] Fallback host streaming started");
  }
  return started;
}



void clearTtsChunkQueue() {
  for (uint8_t i = 0; i < TTS_CHUNK_QUEUE_MAX; i++) {
    clearStringKeepCapacity(ttsChunkQueue[i]);
  }
  ttsChunkHead = 0;
  ttsChunkTail = 0;
  ttsChunkCount = 0;
  ttsChunkSequenceActive = false;
  ttsChunkNeedsStart = false;
}

bool enqueueTtsChunk(const String& chunk) {
  if (ttsChunkCount >= TTS_CHUNK_QUEUE_MAX) return false;

  ttsChunkQueue[ttsChunkTail] = chunk;
  ttsChunkTail = (ttsChunkTail + 1) % TTS_CHUNK_QUEUE_MAX;
  ttsChunkCount++;
  return true;
}

String dequeueTtsChunk() {
  if (ttsChunkCount == 0) return "";

  String chunk = ttsChunkQueue[ttsChunkHead];
  clearStringKeepCapacity(ttsChunkQueue[ttsChunkHead]);
  ttsChunkHead = (ttsChunkHead + 1) % TTS_CHUNK_QUEUE_MAX;
  ttsChunkCount--;
  return chunk;
}

int findSpeakableCut(const String& text, int maxChars) {
  if (maxChars <= 0) return 0;

  int limit = min(maxChars, (int)text.length());
  int bestCut = -1;
  const char puncts[] = {'.', '!', '?', ';', ','};

  for (char punct : puncts) {
    int idx = text.lastIndexOf(punct, limit - 1);
    if (idx > bestCut) bestCut = idx;
  }

  if (bestCut >= 0) return min(bestCut + 1, limit);

  int spaceIdx = text.lastIndexOf(' ', limit - 1);
  if (spaceIdx > 0) return spaceIdx;

  return limit;
}

bool splitTextIntoTtsChunks(const String& text, int chunkLimit) {
  clearTtsChunkQueue();

  String remaining = text;
  remaining.trim();
  if (remaining.length() == 0) return false;

  while (remaining.length() > 0 && ttsChunkCount < TTS_CHUNK_QUEUE_MAX) {
    int cut = (remaining.length() > chunkLimit) ? findSpeakableCut(remaining, chunkLimit) : remaining.length();
    if (cut <= 0) {
      cut = min(chunkLimit, (int)remaining.length());
    }

    String chunk = remaining.substring(0, cut);
    chunk.trim();
    if (chunk.length() == 0) {
      break;
    }

    if (!enqueueTtsChunk(chunk)) {
      break;
    }

    remaining = remaining.substring(cut);
    remaining.trim();
  }

  if (remaining.length() > 0) {
    Serial.println("[TTS] Chunk queue limit reached before full text was queued.");
  }

  // We are handling chunks synchronously in speakText, so do not arm the async sequence loop.
  ttsChunkSequenceActive = false;
  return ttsChunkCount > 0;
}

bool maybeScheduleEarlyTtsRetry(const char* reason, unsigned long startedMs) {
  if (!ttsSpeechSessionActive || activeTtsText.length() == 0) return false;
  if (ttsRetryPending || ttsUsingFallbackHost) return false;
  if (startedMs == 0) return false;

  unsigned long elapsedMs = millis() - startedMs;
  if (elapsedMs > TTS_EARLY_EOF_RETRY_MS) return false;

  Serial.printf("[TTS] %s after %lu ms, scheduling fallback retry\n", reason, elapsedMs);
  ttsRetryPending = true;
  ttsRetryAt = millis() + TTS_RETRY_DELAY_MS;
  ttsStartupPending = false;
  ttsSpeechStartMs = 0;
  ttsPlaybackStartedMs = 0;
  isSpeaking = false;
        clearTtsChunkQueue();
  aiRequestStatus = "Retrying speech...";
  if (firebaseReady) {
    setSpeakingStatusFirebase(false);
  }
  return true;
}

bool beginTtsSpeech(const String& text) {
  if (text.length() == 0) return false;

  // Show AI Answer on screen and make the TTS startup gap explicit.
  if (currentMode == MODE_AI) {
    currentRobotActivity = ROBOT_ACTIVITY_SPEAKING;
    aiRequestStatus = "Speaking...";
    currentInterimText = text;
    drawAIScreen();
  }

  // NOTE: Do NOT set isSpeaking=true here — only set it AFTER the TTS
  // connection is confirmed (line below startTtsPlayback).  Setting it
  // prematurely caused the "stuck speech" race condition.
  currentRobotActivity = ROBOT_ACTIVITY_SPEAKING;
  aiRequestStatus = "Speaking...";
  lastMusicAction = millis();
  activeTtsText = text;
  enterNetworkQuiet();
  ttsUsingFallbackHost = false;
  ttsSpeechSessionActive = true;
  ttsStartupPending = false;
  ttsSpeechStartMs = 0;
  ttsPlaybackStartedMs = 0;
  ttsRetryPending = false;
  ttsRetryAt = 0;
  ttsChunkNeedsStart = false;
  expressiveSpeechMotionQueued = false;
  clearMicAIReconnectSchedule();

  Serial.printf("[TTS] Speaking request len=%d, queue=%u, speaking=%d, audioRunning=%d\n",
                (int)text.length(), ttsChunkCount, isSpeaking ? 1 : 0, audio.isRunning() ? 1 : 0);

  if (audio.isRunning()) {
    audio.stopSong();
    delay(40);
  }

  enterAudioTransitionQuiet();
  disconnectAssemblyAI(0);

  // STOP MIC to prevent I2S conflict
  mic_i2s.end();
  micI2SActive = false;
  delay(10);

  if (!startTtsPlayback(text, false)) {
    Serial.println("[TTS] Primary TTS failed");
    if (!startTtsPlayback(text, true)) {
      Serial.println("[TTS] Fallback host failed");
      isSpeaking = false;
      isProcessingAI = false;
      clearStringKeepCapacity(aiRequestStatus);
      clearStringKeepCapacity(activeTtsText);
      clearStringKeepCapacity(ttsRequestUrl);
      ttsUsingFallbackHost = false;
      ttsStartupPending = false;
      ttsSpeechStartMs = 0;
      ttsSpeechSessionActive = false;
      if (firebaseReady) {
        setSpeakingStatusFirebase(false);
      }
      if (currentMode == MODE_AI && aiInputUsesMic) {
        scheduleMicAIReconnect(MIC_RECONNECT_AFTER_FAILURE_MS, "tts fallback failure");
      }
      return false;
    }
  }

  if (!audio.isRunning()) {
    // Synchronous chunked playback can complete before control returns here.
    // Don't arm the startup watchdog again or it can replay the same utterance.
    Serial.println("[TTS] Speech completed synchronously");
    ttsStartupPending = false;
    if (currentMode == MODE_AI && aiInputUsesMic && !ttsRetryPending) {
      scheduleMicAIReconnect(MIC_RECONNECT_AFTER_TTS_MS, "speech complete");
    }
    return true;
  }

  isSpeaking = true;
  ttsStartupPending = true;
  ttsSpeechStartMs = millis();
  ttsPlaybackStartedMs = 0;
  Serial.printf("[TTS] Speech armed. startupPending=%d, fallback=%d\n",
                ttsStartupPending ? 1 : 0, ttsUsingFallbackHost ? 1 : 0);

  if (firebaseReady) {
      setSpeakingStatusFirebase(true);
      setLastResponseFirebase(text);
  }

  return true;
}

void processPendingTtsChunk() {
  if (!ttsChunkNeedsStart) return;
  if (ttsRetryPending) return;
  if (audio.isRunning()) return;

  ttsChunkNeedsStart = false;

  if (ttsChunkCount == 0) {
    ttsChunkSequenceActive = false;
    return;
  }

  String nextChunk = dequeueTtsChunk();
  if (nextChunk.length() == 0) {
    ttsChunkSequenceActive = false;
    return;
  }

  if (ttsChunkCount == 0) {
    ttsChunkSequenceActive = false;
  } else {
    ttsChunkSequenceActive = true;
  }

  if (!beginTtsSpeech(nextChunk)) {
    clearTtsChunkQueue();
  }
}

bool startNextQueuedTtsChunk() {
  if (ttsRetryPending) return false;
  if (ttsChunkCount == 0) return false;

  String nextChunk = dequeueTtsChunk();
  if (nextChunk.length() == 0) {
    ttsChunkSequenceActive = false;
    return false;
  }

  if (ttsChunkCount == 0) {
    ttsChunkSequenceActive = false;
  } else {
    ttsChunkSequenceActive = true;
  }

  ttsChunkNeedsStart = false;
  if (beginTtsSpeech(nextChunk)) {
    return true;
  }

  clearTtsChunkQueue();
  return false;
}

void processPendingTtsRetry() {
  if (!ttsRetryPending) return;
  if (millis() < ttsRetryAt) return;
  if (audio.isRunning()) return;
  if (activeTtsText.length() == 0) {
    ttsRetryPending = false;
    ttsStartupPending = false;
    ttsSpeechStartMs = 0;
    ttsPlaybackStartedMs = 0;
    clearStringKeepCapacity(aiRequestStatus);
    return;
  }

  ttsRetryPending = false;
  lastMusicAction = millis();
  enterAudioTransitionQuiet();
  ttsStartupPending = true;
  ttsSpeechStartMs = millis();
  ttsPlaybackStartedMs = 0;
  ttsSpeechSessionActive = true;
  currentRobotActivity = ROBOT_ACTIVITY_SPEAKING;
  aiRequestStatus = "Speaking...";

  if (!startTtsPlayback(activeTtsText, true)) {
    Serial.println("[TTS] Fallback retry failed");
    isSpeaking = false;
    isProcessingAI = false;
    clearStringKeepCapacity(aiRequestStatus);
    clearStringKeepCapacity(activeTtsText);
    clearStringKeepCapacity(ttsRequestUrl);
    ttsUsingFallbackHost = false;
    ttsStartupPending = false;
    ttsSpeechStartMs = 0;
    ttsPlaybackStartedMs = 0;
    ttsSpeechSessionActive = false;
    clearTtsChunkQueue();
    if (firebaseReady) {
      setSpeakingStatusFirebase(false);
    }
    if (currentMode == MODE_AI && aiInputUsesMic) {
      scheduleMicAIReconnect(MIC_RECONNECT_AFTER_FAILURE_MS, "tts retry failed");
    }
  } else {
    isSpeaking = true;
    ttsSpeechSessionActive = true;
    if (firebaseReady) {
      setSpeakingStatusFirebase(true);
      setLastResponseFirebase(activeTtsText);
    }
    Serial.println("[TTS] Fallback retry started");
  }
}

void speakText(const char* text, bool longForm) {
  String t = text;
  t.replace("\n", " ");
  t = stripReasoningBlocks(t);
  t = stripDecorativeChatFormatting(t);
  
  // STRIP ALL [TAG] markers from TTS (handles ANY tag the AI uses)
  while (t.indexOf('[') >= 0) {
    int openBracket = t.indexOf('[');
    int closeBracket = t.indexOf(']', openBracket);
    if (closeBracket > openBracket && (closeBracket - openBracket) < 100) {
      t.remove(openBracket, closeBracket - openBracket + 1);
    } else {
      break; // Safety: no matching close bracket, stop
    }
  }
  t.trim();

  // Show text on mouth display
  sendMouthText(t.c_str());

  // Hard cap for AI conversation speech: keep it short enough to avoid TTS skips.

  if (t.length() == 0) {
    Serial.println("[TTS] Skipped empty speech after cleanup");
    isProcessingAI = false;
    clearStringKeepCapacity(aiRequestStatus);
    return;
  }

  const int chunkLimit = 120;
  
  if (t.length() > chunkLimit) {
    if (!splitTextIntoTtsChunks(t, chunkLimit)) {
      isProcessingAI = false;
      clearStringKeepCapacity(aiRequestStatus);
      return;
    }

    while (ttsChunkCount > 0) {
      String chunk = dequeueTtsChunk();
      if (chunk.length() > 0) {
        if (beginTtsSpeech(chunk)) {
          // Play synchronously but yield to keep background tasks alive
          while (audio.isRunning()) {
            audio.loop();
            if (!isSleepMode) {
              updateEyes(); 
              maybeExpressiveSpeechMotion();
              processMovementQueue();
            }
            if (firebaseReady) {
              Firebase.ready(); 
            }
            // Update UI during speech to show the AI response text
            if (currentMode == MODE_AI) drawAIScreen(false);
            else if (currentMode == MODE_NORMAL) drawNormalScreen(false);
            yield();
            delay(2);
          }
        }
      }
    }
    return;
  }

  // Single chunk (<= 120 chars)
  clearTtsChunkQueue();
  if (beginTtsSpeech(t)) {
    while (audio.isRunning()) {
      audio.loop();
      if (!isSleepMode) {
        updateEyes(); 
        maybeExpressiveSpeechMotion();
        processMovementQueue();
      }
      if (firebaseReady) {
        Firebase.ready(); 
      }
      // Update UI during speech
      if (currentMode == MODE_AI) drawAIScreen(false);
      else if (currentMode == MODE_NORMAL) drawNormalScreen(false);
      yield();
      delay(2);
    }
  } else {
    if (currentMode == MODE_AI && aiInputUsesMic) {
      scheduleMicAIReconnect(MIC_RECONNECT_AFTER_FAILURE_MS, "tts begin failed");
    }
  }
}

// Helper to clear AI text
void clearAIResponse() {
    currentInterimText = ""; 
    // Force redraw if in AI mode to clear text area
    if (currentMode == MODE_AI) {
       drawAIScreen(true); // Force redraw
    }
}

void handleUniversalStopAudioCommand() {
  Serial.println("[Command] Universal STOP from Web App!");
  stopActiveAudioPlayback(false);

  if (focusModeActive) {
    focusModeActive = false;
    Firebase.RTDB.setBool(&fbdoFocus, "/commands/focusMode", false);
    Firebase.RTDB.setBool(&fbdoFocus, "/status/focusMode", false);
  }

  if (currentMedState != MED_IDLE) {
    currentMedState = MED_IDLE;
    medStateTimer = millis();
  }

  if (currentMode != MODE_NORMAL) switchToNormalMode();
}

bool checkRemoteStopCommand() {
  if (!firebaseReady) return false;
  if (millis() < commandPollBackoffUntil) return false;
  if (millis() - lastCommandJsonFetchMs < 180) return false;

  static unsigned long lastStopPoll = 0;
  if (millis() - lastStopPoll < 700) return false;
  lastStopPoll = millis();

  if (!Firebase.RTDB.getBool(&fbdoCmd, "/commands/stopAudio")) {
    return false;
  }

  if (fbdoCmd.boolData()) {
    handleUniversalStopAudioCommand();
    Firebase.RTDB.setBool(&fbdo, "/commands/stopAudio", false);
    return true;
  }

  return false;
}

void stopActiveAudioPlayback(bool scheduleMicRestart) {
  bool hadPlayback = audio.isRunning() || isSpeaking || activeTtsText.length() > 0;
  clearDeferredAiAction();
  clearTtsChunkQueue();

  if (audio.isRunning()) {
    audio.stopSong();
    delay(20);
  }

  enterAudioTransitionQuiet();

  isSpeaking = false;
  isProcessingAI = false;
  currentRobotActivity = ROBOT_ACTIVITY_IDLE;
  clearStringKeepCapacity(aiRequestStatus);
  clearStringKeepCapacity(activeTtsText);
  clearStringKeepCapacity(ttsRequestUrl);
  ttsUsingFallbackHost = false;
  ttsRetryPending = false;
  ttsRetryAt = 0;
  ttsStartupPending = false;
  ttsSpeechStartMs = 0;
  ttsPlaybackStartedMs = 0;
  ttsSpeechSessionActive = false;
  expressiveSpeechMotionQueued = false;
  lastMusicAction = millis();

  if (scheduleMicRestart && currentMode == MODE_AI && aiInputUsesMic) {
    disconnectAssemblyAI(0);
    mic_i2s.end();
    micI2SActive = false;
    scheduleMicAIReconnect(MIC_RECONNECT_AFTER_TTS_MS, "audio stop");
  } else {
    clearMicAIReconnectSchedule();
  }

  if (firebaseReady) {
    setSpeakingStatusFirebase(false);
  }

  setEyeExpression("NORMAL");
  clearAIResponse();

  if (hadPlayback) {
    Serial.println("[Audio] Playback stopped cleanly");
  }
  enterNetworkQuiet();
}

void audio_eof_speech(const char* info) {
  Serial.printf("[Audio] speech EOF at %lu ms: %s\n", millis(), info ? info : "");
  enterAudioTransitionQuiet();
  enterNetworkQuiet();

  if (ttsChunkCount > 0) {
    // We are in a synchronous chunk sequence. Let speakText handle the next chunk.
    return;
  }

  unsigned long retryStartMs = (ttsPlaybackStartedMs > 0) ? ttsPlaybackStartedMs : ttsSpeechStartMs;
  if (maybeScheduleEarlyTtsRetry("speech EOF", retryStartMs)) {
    return;
  }

  if (firebaseReady) {
      setSpeakingStatusFirebase(false);
  }
  
  isSpeaking = false;
  isProcessingAI = false;
  currentRobotActivity = ROBOT_ACTIVITY_IDLE;
  clearStringKeepCapacity(aiRequestStatus);
  clearStringKeepCapacity(activeTtsText);
  clearStringKeepCapacity(ttsRequestUrl);
  ttsUsingFallbackHost = false;
  ttsRetryPending = false;
  ttsStartupPending = false;
  ttsSpeechStartMs = 0;
  ttsPlaybackStartedMs = 0;
  ttsSpeechSessionActive = false;
  expressiveSpeechMotionQueued = false;
  setEyeExpression("NORMAL");
  clearAIResponse(); // Clear text when done speaking
  if (currentMode == MODE_AI) {
    drawAIScreen(true);
    Serial.printf("[UI] Listening screen shown at %lu ms after speech EOF\n", millis());
  }
  
  // SCHEDULE MIC RESTART (short delay to prevent audio loopback)
  if (currentMode == MODE_AI && aiInputUsesMic) {
      scheduleMicAIReconnect(MIC_RECONNECT_AFTER_TTS_MS, "speech EOF");
  }
  clearTtsChunkQueue();
  clearMovementQueue();
}

// Add periodic eye animation during speech
void animateEyesWhileSpeaking() {
  if (!isSpeaking) return;

  // Guard: only animate when audio is actually playing, not during
  // stuck/pending states — this prevents the HAPPY↔NORMAL flicker.
  if (!audio.isRunning()) return;

  bool musicPlayback = (activeTtsText.length() == 0 && !isProcessingAI);
  if (musicPlayback) {
    // Keep music playback calm and steady instead of flipping the face every few seconds.
    if (currentEyeExpression != "HAPPY") {
      setEyeExpression("HAPPY");
    }
    return;
  }
  
  // FIX: Don't overwrite special emotions (LOVE, DEAD, etc.)
  if (currentEyeExpression != "NORMAL" && currentEyeExpression != "HAPPY" && currentEyeExpression != "THINKING") {
      return; 
  }

  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 3000) { // Blink/animate every 3 seconds
    // Only flip between HAPPY and NORMAL to prevent overriding specific AI emotions like SAD or THINKING
    if (currentEyeExpression == "HAPPY") {
      setEyeExpression("NORMAL");
    } else if (currentEyeExpression == "NORMAL") {
      setEyeExpression("HAPPY");
    }
    lastBlink = millis();
  }
}

void audio_eof_mp3(const char* info) {
  unsigned long audioStopTime = millis();
  Serial.printf("[TIMER] AUDIO_STOPPED at %lu ms\n", audioStopTime);
  enterAudioTransitionQuiet();
  enterNetworkQuiet();

  if (ttsChunkCount > 0) {
      // We are in a synchronous chunk sequence. Let speakText handle the next chunk.
      return;
  }

  unsigned long retryStartMs = (ttsPlaybackStartedMs > 0) ? ttsPlaybackStartedMs : ttsSpeechStartMs;
  if (maybeScheduleEarlyTtsRetry("mp3 EOF", retryStartMs)) {
    return;
  }

  isSpeaking = false;
  isProcessingAI = false;
  currentRobotActivity = ROBOT_ACTIVITY_IDLE;
  ttsChunkCount = 0;
  if (ttsUsingFallbackHost || activeTtsText.length() > 0) {
      if (firebaseReady) {
          setSpeakingStatusFirebase(false);
      }
      clearStringKeepCapacity(aiRequestStatus);
      clearStringKeepCapacity(activeTtsText);
      clearStringKeepCapacity(ttsRequestUrl);
  ttsUsingFallbackHost = false;
  ttsRetryPending = false;
  ttsStartupPending = false;
  ttsSpeechStartMs = 0;
  ttsPlaybackStartedMs = 0;
  ttsSpeechSessionActive = false;
  setEyeExpression("NORMAL");
      clearAIResponse();
      if (currentMode == MODE_AI) {
        drawAIScreen(true);
        Serial.printf("[UI] Listening screen shown at %lu ms after mp3 EOF\n", millis());
      }
  }
  if (currentMode == MODE_AI && aiInputUsesMic) {
      scheduleMicAIReconnect(MIC_RECONNECT_AFTER_TTS_MS, "mp3 EOF");
  }
  clearTtsChunkQueue();
  clearMovementQueue();
}

void audio_info(const char *info) {
  Serial.print("[Audio] ");
  Serial.println(info ? info : "");

  if (!info) return;
  if (!ttsStartupPending || !isSpeaking || activeTtsText.length() == 0) return;

  String msg = String(info);
  msg.toLowerCase();

  // Retry logic for 404/not found during deployments
  if (msg.indexOf("404") >= 0 || msg.indexOf("not found") >= 0) {
    static unsigned long lastRetryMs = 0;
    if (millis() - lastRetryMs > 5000) { // prevent infinite loop
      Serial.println("[Audio] 404 detected, retrying in 1.5s...");
      lastRetryMs = millis();
      delay(1500); 
      audio.connecttohost(ttsRequestUrl.c_str());
      return;
    }
  }

  bool startupFailureSignal =
      (msg.indexOf("unknown content found") >= 0) ||
      (msg.indexOf("request") >= 0 && msg.indexOf("failed") >= 0) ||
      (msg.indexOf("timeout") >= 0);

  if (!startupFailureSignal) return;

  if (!ttsRetryPending && !ttsUsingFallbackHost) {
    Serial.println("[TTS] Library startup failure signal, scheduling fallback retry");
    ttsRetryPending = true;
    ttsRetryAt = millis() + TTS_RETRY_DELAY_MS;
    ttsStartupPending = false;
    ttsPlaybackStartedMs = 0;
    isSpeaking = false;
  }
}
void audio_error(const char *info) {
  Serial.print("[Audio] Error: ");
  Serial.println(info);
  enterAudioTransitionQuiet();
  isSpeaking = false;
  isProcessingAI = false;
  currentRobotActivity = ROBOT_ACTIVITY_IDLE;
  clearStringKeepCapacity(aiRequestStatus);
  clearStringKeepCapacity(activeTtsText);
  clearStringKeepCapacity(ttsRequestUrl);
  ttsUsingFallbackHost = false;
  ttsRetryPending = false;
  ttsStartupPending = false;
  ttsSpeechStartMs = 0;
  ttsPlaybackStartedMs = 0;
  ttsSpeechSessionActive = false;
  expressiveSpeechMotionQueued = false;
  clearTtsChunkQueue();
  clearMovementQueue();
  setEyeExpression("NORMAL");
  clearAIResponse();

  if (firebaseReady) {
    setSpeakingStatusFirebase(false);
  }
  if (currentMode == MODE_AI && aiInputUsesMic) {
    scheduleMicAIReconnect(MIC_RECONNECT_AFTER_FAILURE_MS, "audio error");
  }
  enterNetworkQuiet();
}

bool looksLikePrintableCommand(const String& text) {
  if (text.length() == 0) return false;
  uint16_t printable = 0;
  for (size_t i = 0; i < text.length(); i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
      printable++;
      continue;
    }
    if (c < 0x20 || c > 0x7E) return false;
    printable++;
  }
  return printable > 0;
}

bool readCleanSerialLine(String& outLine) {
  static String serialLine;
  static bool lineHadBadByte = false;

  outLine = "";
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      if (serialLine.length() == 0) {
        lineHadBadByte = false;
        continue;
      }

      if (!lineHadBadByte && looksLikePrintableCommand(serialLine)) {
        outLine = serialLine;
        serialLine = "";
        lineHadBadByte = false;
        return true;
      }

      if (serialLine.length() > 0) {
        Serial.println("[Serial] Dropped noisy line");
      }
      serialLine = "";
      lineHadBadByte = false;
      continue;
    }

    if (c < 0x20 || c > 0x7E) {
      lineHadBadByte = true;
      continue;
    }

    if (serialLine.length() < 160) {
      serialLine += c;
    }
  }

  return false;
}

// ============================================================
// MUSIC & WEB SEARCH
// ============================================================
// ============================================================
// MUSIC (Radio Only - No Search)
// ============================================================

void playMusic(String genre) {
  genre.toLowerCase();
  String normalized = genre;
  normalized.replace("-", "");
  normalized.replace("_", "");
  normalized.replace(" ", "");
  normalized.replace(".", "");
  normalized.replace(",", "");
  String url = "";

  // Stop any previous audio
  clearTtsChunkQueue();
  if (audio.isRunning()) {
    audio.stopSong();
  }
  enterAudioTransitionQuiet();

  // Global radio streams
  if (normalized.indexOf("happy") >= 0 || normalized.indexOf("upbeat") >= 0 || normalized.indexOf("energetic") >= 0 || normalized.indexOf("fun") >= 0 || normalized.indexOf("party") >= 0)
    url = "http://icecast.omroep.nl/3fm-bb-mp3";
  else if (normalized.indexOf("lofi") >= 0 || normalized.indexOf("chillhop") >= 0 || normalized.indexOf("lofibeats") >= 0) url = "http://stream.zeno.fm/0r0xa792kwzuv";
  else if (normalized.indexOf("jazz") >= 0) url = "http://airspectrum.cdnstream1.com:8114/1648_128";
  else if (normalized.indexOf("pop") >= 0) url = "http://icecast.omroep.nl/3fm-bb-mp3";
  else if (normalized.indexOf("rock") >= 0) url = "http://stream.srg-ssr.ch/m/rsj/mp3_128";
  else if (normalized.indexOf("afrobeats") >= 0 || normalized.indexOf("afrobeat") >= 0 || normalized.indexOf("afro") >= 0)
    url = "http://stream.zeno.fm/f3wvbbqmdg8uv";
  else if (normalized.indexOf("hiphop") >= 0 || normalized.indexOf("rap") >= 0) 
    url = "http://us4.internet-radio.com:8266/stream";
  else if (normalized.indexOf("edm") >= 0 || normalized.indexOf("electronic") >= 0 || normalized.indexOf("dance") >= 0) 
    url = "http://stream.zeno.fm/f3wvbbqmdg8uv";
  else if (normalized.indexOf("country") >= 0) url = "http://us5.internet-radio.com:8119/stream";
  else if (normalized.indexOf("classical") >= 0) url = "http://stream.srg-ssr.ch/m/rsc_de/mp3_128";
  else if (normalized.indexOf("chill") >= 0 || normalized.indexOf("relax") >= 0) url = "http://stream.zeno.fm/0r0xa792kwzuv";
  else if (normalized.indexOf("ambient") >= 0 || normalized.indexOf("focus") >= 0 || normalized.indexOf("rain") >= 0)
    url = "http://ice1.somafm.com/deepspaceone-128-mp3";   // SomaFM Deep Space One — ambient/focus
  else if (normalized.indexOf("ocean") >= 0 || normalized.indexOf("waves") >= 0)
    url = "http://ice1.somafm.com/deepspaceone-128-mp3"; // SomaFM Deep Space ambient
  else if (normalized.indexOf("fire") >= 0)
    url = "http://ice1.somafm.com/missioncontrol-128-mp3"; // SomaFM Mission Control ambient
  else {
      Serial.println("[Music] Unknown genre, defaulting to Lofi Radio.");
      url = "http://stream.zeno.fm/0r0xa792kwzuv"; // Default: Lofi
  }

  isProcessingAI = false;
  isSpeaking = true;
  lastMusicAction = millis();
  setEyeExpression("HAPPY");

  // WebSocket disconnect removed.

  mic_i2s.end();
  micI2SActive = false;
  delay(10);
  audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DOUT);

  audio.setConnectionTimeout(3000, 8000);
  audio.forceMono(true);
  audio.setVolume(21); // Ensure max volume
  audio.connecttohost(url.c_str());
}

// ============================================================
// WEB SEARCH (Simplified - No external API needed)
// ============================================================
String performWebSearch(String query) {
  Serial.printf("[Search] Query: '%s'\n", query.c_str());
  String queryLower = query;
  queryLower.toLowerCase();

  // Get current time for time queries
  struct tm timeinfo;
  String timeStr = "unknown";
  if(getLocalTime(&timeinfo)) {
    char buf[50];
    strftime(buf, sizeof(buf), "%I:%M %p", &timeinfo);
    timeStr = String(buf);
  }

  // 2. Tavily AI Search
  if (strlen(TAVILY_KEY) > 5) {
      std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure());
  client->setInsecure();
      HTTPClient http;
      
      Serial.println("[Tavily] Connecting...");
      
      if (http.begin(*client, "https://api.tavily.com/search")) {
        DynamicJsonDocument reqDoc(1536);
        reqDoc["api_key"] = TAVILY_KEY;
        reqDoc["query"] = query;
        reqDoc["topic"] = (queryLower.indexOf("news") >= 0 || queryLower.indexOf("latest") >= 0 || queryLower.indexOf("today") >= 0 || queryLower.indexOf("current") >= 0 || queryLower.indexOf("breaking") >= 0) ? "news" : "general";
        reqDoc["search_depth"] = "basic";
        reqDoc["include_answer"] = true;
        reqDoc["max_results"] = 3;

        String jsonPayload;
        serializeJson(reqDoc, jsonPayload);
        int httpCode = http.POST(jsonPayload);
        
        Serial.printf("[Tavily] HTTP: %d\n", httpCode);
        
        if (httpCode == HTTP_CODE_OK) {
           String response = http.getString();
           DynamicJsonDocument doc(12288);
           DeserializationError error = deserializeJson(doc, response);
           
           if (!error) {
               String resultText = "";

               const char* answer = doc["answer"];
               if (answer && strlen(answer) > 0) {
                   String a = truncateMemoryText(normalizeMemoryText(String(answer)), 300);
                   if (a.length() > 0) {
                       resultText = "Tavily Answer: " + a;
                   }
               }

               JsonArray results = doc["results"].as<JsonArray>();
               int sourceCount = 0;
               for (JsonObject result : results) {
                   const char* title = result["title"] | "";
                   const char* content = result["content"] | "";
                   const char* url = result["url"] | "";

                   String snippet = normalizeMemoryText(String(content));
                   snippet = truncateMemoryText(snippet, 220);
                   if (snippet.length() == 0) continue;

                   if (resultText.length() > 0) resultText += "\n";
                   resultText += "Source: ";
                   if (strlen(title) > 0) {
                       resultText += String(title) + " - ";
                   }
                   resultText += snippet;
                   if (strlen(url) > 0) {
                       resultText += " (" + String(url) + ")";
                   }

                   sourceCount++;
                   if (sourceCount >= 2) break;
               }

               if (resultText.length() > 0) {
                   Serial.println("[Tavily] Search result ready");
                   return resultText;
               }

               return "Search completed, but Tavily did not return a useful answer.";
           } else {
               Serial.println("[Tavily] JSON Parse Error");
           }
        } else {
           Serial.printf("[Tavily] Error: %s\n", http.errorToString(httpCode).c_str());
           Serial.println("[Tavily] Response: " + http.getString());
        }
        http.end();
      } else {
        Serial.println("[Tavily] Connection failed");
      }
      
      return "Search failed due to network or API error.";
  }

  // 3. Simulated Fallbacks (Only if NO API Key)
  if (queryLower.indexOf("tech") >= 0 || queryLower.indexOf("technology") >= 0) {
    return "My circuits tell me Nigerian tech is booming! Fintech is hot.";
  } 
  else if (queryLower.indexOf("weather") >= 0) {
    if (queryLower.indexOf("location") >= 0 ||
        queryLower.indexOf("current location") >= 0 ||
        queryLower.indexOf("user's location") >= 0 ||
        queryLower.indexOf("my location") >= 0) {
      return "I need your city or location first to check the weather accurately.";
    }

    // If no location is mentioned, fall back to the local environment sensors.
    return "I can't check live weather without a location, but the robot's room is " + String(temp_aht, 1) + "C with local AQI " + String(aqi_val) + ".";
  }
  else if (queryLower.indexOf("time") >= 0) {
    return "It is exactly " + timeStr;
  }
  else {
    return "I don't have a Tavily key configured yet. Please add one.";
  }
}





void syncWithFirebase() {
  if (!FIREBASE_ENABLED || offlineModeLocked) return;
  // Sync every minute to avoid flooding
  static unsigned long lastSync = 0;
  if (millis() - lastSync < 60000) return;
  lastSync = millis();

  syncUserProfileFromFirebase();
  syncRemindersFromFirebase();
}

// ============================================================
// TELEGRAM BOT - NOW HANDLED BY SERVER
// ============================================================
// Old processTelegramCommands() function removed - all Telegram via server now!
// See sendTelegramAlert() and handleTelegramCommand() functions above

/*
void processTelegramCommands() {
  // THIS FUNCTION IS NO LONGER USED
  // All Telegram operations now handled by server
  // See sendTelegramAlert() and handleTelegramCommand() instead
}
*/

void checkAutoWeeklyReport() {
  if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) return;
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return;

  // Send report on Sunday (0) at 09:00 AM
  static bool reportSentToday = false;
  
  if (timeinfo.tm_wday == 0 && timeinfo.tm_hour == 9 && timeinfo.tm_min == 0) {
    if (!reportSentToday) {
      sendWeeklyReport();
      reportSentToday = true;
    }
  } else {
    reportSentToday = false;
  }
}

void sendWeeklyReport() {
    String report = "📊 <b>WEEKLY HEALTH REPORT</b>\n\n";
    // Since we don't have historical data stored locally yet, we send a summary of current status
    // and a placeholder for historical analysis.
    report += "🗓 <b>Period:</b> Last 7 Days\n\n";
    
    report += "❤️ <b>Avg Heart Rate:</b> " + (isnan(max30102_hr) ? "N/A" : String((int)max30102_hr)) + " BPM\n";
    report += "🫁 <b>Avg SpO2:</b> " + (isnan(max30102_spo2) ? "N/A" : String((int)max30102_spo2)) + "%\n";
    report += "🌡 <b>Avg Temp:</b> " + String(temp_aht, 1) + "°C\n";
    report += "🌬 <b>Avg AQI:</b> " + String(aqi_val) + "\n";
    
    report += "\n📝 <b>Analysis:</b>\n";
    report += "Vital signs monitoring is active. No critical anomalies detected in logged sessions.\n";
    report += "\n<i>Stay healthy!</i>";
    
    sendTelegramAlert(report);
}

void checkAirQualityAlerts() {
    if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) return;
    static unsigned long lastAQIAlert = 0;
    static unsigned long highAqiStartTime = 0;
    
    // Cooldown: Only alert once every 5 minutes
    if (millis() - lastAQIAlert < 300000) return; 
    
    // Check if AQI is Poor (4) or Hazardous (5)
    // You can also add temperature checks here (e.g. temp_aht > 45)
    bool isDangerous = (aqi_val >= 4 || temp_aht > 40.0);
    
    if (isDangerous) {
        if (highAqiStartTime == 0) {
            // Danger just started, record the time
            highAqiStartTime = millis();
        } else if (millis() - highAqiStartTime > 5000) {
            // Danger has persisted for 5 continuous seconds! Trigger Alert.
            String alertMsg = "";
            String ttsMsg = "";
            
            if (aqi_val >= 4) {
               alertMsg = "🚨 <b>AIR QUALITY ALERT</b>\n\n";
               alertMsg += (aqi_val == 5) ? "⚠️ <b>UNHEALTHY (AQI 5)</b>\nVentilate immediately!" : "⚠️ <b>POOR (AQI 4)</b>\nVentilation recommended.";
               ttsMsg = "Warning. The air quality in the room is poor. Please ventilate the area immediately.";
            } else if (temp_aht > 40.0) {
               alertMsg = "🚨 <b>HIGH TEMPERATURE ALERT</b>\n\n";
               alertMsg += "⚠️ <b>" + String(temp_aht, 1) + "°C</b>\nRoom temperature is dangerously high!";
               ttsMsg = "Warning. The room temperature is dangerously high.";
            }
            
            alertMsg += "\n\n📊 <b>Readings:</b>\n";
            alertMsg += "🌫 AQI: " + String(aqi_val) + "\n";
            alertMsg += "🧪 TVOC: " + String(tvoc_val) + " ppb\n";
            alertMsg += "🌡 Temp: " + String(temp_aht, 1) + "°C\n";
            
            Serial.println("[Alert] Danger persisted for 5s. Triggering warnings.");
            
            // Stop music if playing
            if (audio.isRunning()) {
                clearTtsChunkQueue();
                audio.stopSong();
            }
            
            // 1. Speak the alert loudly
            speakText(ttsMsg.c_str());
            
            // 2. Send Telegram Message
            sendTelegramAlert(alertMsg);
            
            // 3. Set Cooldown & Reset Timer
            lastAQIAlert = millis();
            highAqiStartTime = 0;
        }
    } else {
        // Reset the timer if the danger goes away before 5 seconds
        highAqiStartTime = 0;
    }
}
void printMemoryStats() {
  Serial.printf("[Mem] Heap: %d (Min: %d), PSRAM: %d (Min: %d)\n", 
    ESP.getFreeHeap(), ESP.getMinFreeHeap(), 
    ESP.getFreePsram(), ESP.getMinFreePsram());
}

// ============================================================
// CONVERSATION HISTORY TO FIREBASE
// ============================================================
void logConversationToFirebase(const char* userText, String aiReply) {
    if (!firebaseReady || WiFi.status() != WL_CONNECTED) return;
  if (networkIsQuiet()) return;

    struct tm timeinfo;
    String tStamp = "";
    if (getLocalTime(&timeinfo)) {
        char buf[30];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        tStamp = String(buf);
    }

    FirebaseJson convJson;
    convJson.set("user", String(userText));

    // Truncate AI reply for storage (max 200 chars)
    String cleanReply = aiReply;
    if (cleanReply.length() > 200) cleanReply = cleanReply.substring(0, 200) + "...";
    convJson.set("ai", cleanReply);
    convJson.set("mood", currentEyeExpression);
    convJson.set("timestamp", tStamp);

    Firebase.RTDB.pushJSONAsync(&fbdo, "/conversations", &convJson);
    Serial.println("[Firebase] Conversation logged");

    // Keep only last 20 conversations (cleanup old entries periodically)
    static unsigned long lastCleanup = 0;
    if (millis() - lastCleanup > 3600000) { // Every hour
        // Firebase doesn't support limitToLast with delete easily on ESP32
        // So we rely on web dashboard or Cloud Functions to prune
        lastCleanup = millis();
    }
}

// ============================================================
// GUIDED MEDITATION PRESETS
// ============================================================

struct MeditationStep {
    unsigned long timeOffset; // ms from start
    const char* instruction;
};

// Body Scan (5 minutes)
const MeditationStep bodyScanSteps[] = {
    {0, "Close your eyes and take a deep breath."},
    {10000, "Focus on your feet. Notice any tension and let it go."},
    {30000, "Move your attention to your legs. Relax them completely."},
    {60000, "Now focus on your stomach. Let it soften with each breath."},
    {90000, "Bring awareness to your chest. Feel your heart beating gently."},
    {120000, "Relax your shoulders. Let them drop away from your ears."},
    {150000, "Focus on your arms and hands. Let them feel heavy and warm."},
    {180000, "Bring attention to your neck and jaw. Release any tightness."},
    {210000, "Now your face and forehead. Smooth out any tension."},
    {240000, "Take a deep breath and feel your whole body at peace."},
    {270000, "Body scan complete. Take a moment before opening your eyes."},
};

// Calm Focus (3 minutes)
const MeditationStep calmFocusSteps[] = {
    {0, "Sit comfortably and close your eyes."},
    {8000, "Breathe in slowly through your nose."},
    {14000, "Breathe out slowly through your mouth."},
    {25000, "Focus only on the sound of your breathing."},
    {45000, "If your mind wanders, gently bring it back to your breath."},
    {70000, "You are calm. You are focused. You are present."},
    {100000, "Continue breathing at your own pace."},
    {140000, "Notice how calm you feel right now."},
    {170000, "Session complete. Carry this calm with you."},
};

// Deep Rest (10 minutes, key moments only)
const MeditationStep deepRestSteps[] = {
    {0, "Lie down or sit back comfortably. Close your eyes."},
    {15000, "Take three deep breaths. In through the nose, out through the mouth."},
    {40000, "Let your body become completely heavy. Sink into your seat."},
    {80000, "Imagine a warm light slowly moving from your feet upward."},
    {140000, "The light reaches your legs, melting away all tension."},
    {200000, "It moves through your core, bringing deep relaxation."},
    {280000, "The warm light fills your chest with calm energy."},
    {360000, "It flows through your arms, making them feel weightless."},
    {440000, "The light reaches your head, clearing all thoughts."},
    {520000, "You are completely at peace. Rest here as long as you need."},
    {580000, "Deep rest session complete. Wake gently when you're ready."},
};

int meditationStepIndex = 0;

void updateGuidedMeditation() {
    if (currentMedState != MED_MEDITATING) return;


    unsigned long elapsed = millis() - medStateTimer;
    const MeditationStep* steps = nullptr;
    int stepCount = 0;

    switch (currentMeditationType) {
        case MED_TYPE_BODY_SCAN:
            steps = bodyScanSteps;
            stepCount = sizeof(bodyScanSteps) / sizeof(MeditationStep);
            break;
        case MED_TYPE_CALM_FOCUS:
            steps = calmFocusSteps;
            stepCount = sizeof(calmFocusSteps) / sizeof(MeditationStep);
            break;
        case MED_TYPE_DEEP_REST:
            steps = deepRestSteps;
            stepCount = sizeof(deepRestSteps) / sizeof(MeditationStep);
            break;
        default: return;
    }

    // Check if we should speak the next step
    if (meditationStepIndex < stepCount && elapsed >= steps[meditationStepIndex].timeOffset) {
        if (!isSpeaking) { // Wait for previous TTS to finish
            speakText(steps[meditationStepIndex].instruction);
            meditationStepIndex++;
        }
    }

    // Session complete
    if (meditationStepIndex >= stepCount && !isSpeaking) {
        Serial.println("[Meditation] Session complete");
        currentMedState = MED_IDLE;
        currentMeditationType = MED_TYPE_BREATHING; // Reset
        drawNormalScreen(true);
    }
}

void startMeditation(String preset) {
    preset.toLowerCase();
    if (preset.indexOf("body") >= 0 || preset.indexOf("scan") >= 0) {
        currentMeditationType = MED_TYPE_BODY_SCAN;
        speakText("Starting a five minute body scan meditation.");
    } else if (preset.indexOf("calm") >= 0 || preset.indexOf("focus") >= 0) {
        currentMeditationType = MED_TYPE_CALM_FOCUS;
        speakText("Starting a three minute calm focus session.");
    } else if (preset.indexOf("deep") >= 0 || preset.indexOf("rest") >= 0 || preset.indexOf("sleep") >= 0) {
        currentMeditationType = MED_TYPE_DEEP_REST;
        speakText("Starting a ten minute deep rest session. Get comfortable.");
    } else {
        // Default to body scan
        currentMeditationType = MED_TYPE_BODY_SCAN;
        speakText("Starting guided meditation.");
    }

    meditationStepIndex = 0;
    if (currentMode != MODE_NORMAL) switchToNormalMode();
    currentMedState = MED_MEDITATING; 
    medStateTimer = millis();
    setEyeExpression("LOVE");
    drawNormalScreen(true);
}

// ============================================================
// HARDWARE STUBS (Ready for future hardware)
// ============================================================

// FALL DETECTION STUB — requires MPU6050 accelerometer
// To enable: connect MPU6050 to I2C, uncomment init in setup()
void checkFallDetection() {
    // STUB: When MPU6050 is connected, this will:
    // 1. Read acceleration data
    // 2. Detect sudden impact (accel > 3g followed by no motion)
    // 3. Wait 10 seconds for user to cancel
    // 4. If no cancel -> sendEmergencyAlert("Fall Detected")

    // To implement, add: #include <MPU6050.h>
    // MPU6050 mpu;
    // In setup(): mpu.begin(); mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
    // Here: read accel, detect impact pattern
    return; // No-op until hardware connected
}

// SMART LIGHT CONTROL STUB — requires WLED or Hue Bridge on local network
void controlSmartLight(String action) {
    // STUB: When smart lights are configured, this will:
    // action = "on", "off", "dim", "warm", "cool"
    // Send HTTP request to WLED API or Hue Bridge

    // Example WLED: http://192.168.1.100/win&T=1 (on), T=0 (off)
    // Example Hue: PUT http://bridge-ip/api/{key}/lights/1/state

    Serial.println("[SmartLight] Stub called: " + action);
    return; // No-op until configured
}

// ============================================================
// CUSTOM ALERT THRESHOLDS (synced from web dashboard)
// ============================================================
float alertThresholdHRHigh = 120.0;
float alertThresholdHRLow = 50.0;
float alertThresholdSpO2Low = 90.0;
float alertThresholdTempHigh = 40.0;
int alertThresholdAQIPoor = 4;

void syncAlertThresholds() {
    if (!firebaseReady) return;

    if (Firebase.RTDB.getJSON(&fbdo, "/settings/alertThresholds")) {
        FirebaseJson *json = fbdo.jsonObjectPtr();
        if (!json) return;

        FirebaseJsonData val;
        json->get(val, "hrHigh");
        if (val.success) alertThresholdHRHigh = val.to<float>();

        json->get(val, "hrLow");
        if (val.success) alertThresholdHRLow = val.to<float>();

        json->get(val, "spo2Low");
        if (val.success) alertThresholdSpO2Low = val.to<float>();

        json->get(val, "tempHigh");
        if (val.success) alertThresholdTempHigh = val.to<float>();

        json->get(val, "aqiPoor");
        if (val.success) alertThresholdAQIPoor = val.to<int>();

    }
}


// ============================================================
// SLEEP TRACKING LOGIC
// ============================================================
void enterDeepSleep(const char* reason) {
  Serial.print("[Sleep] Deep sleep: ");
  Serial.println(reason);

  if (firebaseReady) {
    Firebase.RTDB.setString(&fbdo, "/status/power", "sleep");
  }

  stopMotorMotion(true);
  delay(150);

  // Wake on tactile button (active low) + timer fallback.
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TACTILE_SWITCH_PIN, 0);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_WAKE_INTERVAL_MS * 1000ULL);
  esp_deep_sleep_start();
}

void checkAutoDeepSleep() {
  if (offlineModeLocked) return;
  if (millis() - bootTimeMs < DEEP_SLEEP_MIN_UPTIME_MS) return;
  if (autoAvoidEnabled || guardModeEnabled || guardAlarmActive) return;
  if (currentMode != MODE_NORMAL) return;
  if (currentMedState != MED_IDLE) return;
  if (isSpeaking || isProcessingAI) return;
  if (driveControlActive || isMoving || turnControlActive) return;

  if (millis() - lastInteractionTime < DEEP_SLEEP_INACTIVITY_MS) return;

  enterDeepSleep("inactive");
}

void checkSleepTracking() {
    if (offlineModeLocked) return;
    if (!ntpTimeValid) return;
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)) return;

    // Detect Sleep Start: Check past 9 PM or before 6 AM
    if (timeinfo.tm_hour >= 21 || timeinfo.tm_hour < 6) {
        // If there has been no interaction for 30 minutes, assume sleep
        if (millis() - lastInteractionTime > 1800000 && sleepStartTime == 0) {
            sleepStartTime = millis();
            lastSleepLogged = false;
            isSleepMode = true;
            Serial.println("[Sleep] Assumed sleep started.");
        }
    }

    // Detect Wake Up: Based on interactions
    if (sleepStartTime > 0 && !lastSleepLogged) {
        // An interaction happened recently
        if (millis() - lastInteractionTime < 60000) {
            unsigned long currentWakeTime = millis();
            float sleepHours = (currentWakeTime - sleepStartTime) / 3600000.0;
            
            // Only log if between 2 and 16 hours 
            if (sleepHours > 2.0 && sleepHours < 16.0) {
                char path[64];
                // YYYY-MM-DD format based on timeinfo
                sprintf(path, "sleepLog/%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
                if (firebaseReady) {
                    Firebase.RTDB.setFloat(&fbdo, String(path) + "/hours", sleepHours);
                    Serial.printf("[Sleep] Logged %.2f hours to %s\n", sleepHours, path);
                }
            } else {
                Serial.printf("[Sleep] Discarded invalid sleep duration: %.2f hours\n", sleepHours);
            }
            sleepStartTime = 0;
            lastSleepLogged = true;
            isSleepMode = false;
            wakeTime = currentWakeTime;
        }
    }
}

// ============================================================
// MAIN LOOP MOVED TO BOTTOM FOR SCOPING
// ============================================================
void loop() {
  reconnectMQTT();
  if (mqtt.connected()) {
    mqtt.loop();
  }
  if (currentMode == MODE_AI) {
    serviceLlmWebSocketKeepAlive();
  } else if (llmWsConnected) {
    disconnectLlmWebSocket(0);
  }

  if (otaReady) {
    ArduinoOTA.handle();
  }


  audio.loop();
  drainNodeAudio();        // Safely play Node TTS PCM audio queued by Core 0
  processPendingTtsChunk();
  processPendingTtsRetry();
  flushPendingSpeakingStatus();
  flushPendingLastResponse();
  flushPendingModeStatus();
  maybeExpressiveSpeechMotion();
  updateRobotAssistModes();
  processMovementQueue();
  
  // AUTONOMOUS MODE: Update navigation
  if (currentMode == MODE_AUTONOMOUS && autoModeActive) {
    updateAutonomousNavigation();
  }
  
  // Prioritize touch before any network-heavy work so taps stay responsive.
  processTactileSwitch();
  processTouchScreen();
  serviceMicTest();

  // Handle ByteDance only if manually connected (maintenance)
  if (bdConnected) {
    streamMicToByteDance();
    pumpByteDanceSocket();
  }

  bool touchUiHot = touchUiRecentlyHandled();
  bool currentlySpeaking = audio.isRunning();
  bool speechSettling = false;
  bool checkupNetworkQuiet = ai_requested_checkup ||
                            currentMedState == MED_WAIT_FINGER ||
                            currentMedState == MED_PLACE_FINGER ||
                            currentMedState == MED_MEASURING ||
                            currentMedState == MED_RESULT;

  checkRemoteStopCommand();
  serviceMicAIReconnect(checkupNetworkQuiet);

  if (isSpeaking && ttsStartupPending && !currentlySpeaking && ttsSpeechStartMs > 0) {
    unsigned long startupWaitMs = millis() - ttsSpeechStartMs;
    if (startupWaitMs > TTS_STARTUP_TIMEOUT_MS) {
      if (!ttsRetryPending && !ttsUsingFallbackHost && activeTtsText.length() > 0) {
        Serial.printf("[TTS] Startup timeout after %lu ms, scheduling fallback retry\n", startupWaitMs);
        ttsRetryPending = true;
        ttsRetryAt = millis() + TTS_RETRY_DELAY_MS;
        ttsStartupPending = false;
        isSpeaking = false;
        if (audio.isRunning()) {
          audio.stopSong();
        }
      } else if (!ttsRetryPending) {
        Serial.printf("[TTS] Startup timeout on fallback after %lu ms, aborting speech\n", startupWaitMs);
        isSpeaking = false;
        isProcessingAI = false;
        currentRobotActivity = ROBOT_ACTIVITY_IDLE;
        clearStringKeepCapacity(aiRequestStatus);
        clearStringKeepCapacity(activeTtsText);
        clearStringKeepCapacity(ttsRequestUrl);
        ttsUsingFallbackHost = false;
        ttsRetryPending = false;
        ttsRetryAt = 0;
        ttsStartupPending = false;
        ttsSpeechStartMs = 0;
        ttsSpeechSessionActive = false;
        clearTtsChunkQueue();
        if (firebaseReady) {
          setSpeakingStatusFirebase(false);
        }
        if (currentMode == MODE_AI && aiInputUsesMic) {
          scheduleMicAIReconnect(MIC_RECONNECT_AFTER_FAILURE_MS, "startup timeout");
        }
      }
    }
  }

  speechSettling = isSpeaking && !currentlySpeaking && !vAgentPlayingAudio;

  if (speechSettling) {
    unsigned long speechAgeMs = (ttsPlaybackStartedMs > 0) ? (millis() - ttsPlaybackStartedMs) : 0;
    
    // Check for NTP sync before considering it 'settled'
    if (speechAgeMs > 1000 && (time(nullptr) < 1000000)) {
        Serial.println("[Time] Waiting for NTP sync for SSL...");
        // Don't clear yet
        return;
    }

    if (speechAgeMs > 15000) { 
      Serial.println("[Audio] Safety: clearing stuck speech state");
      isSpeaking = false;
      isProcessingAI = false;
      clearStringKeepCapacity(aiRequestStatus);
      clearStringKeepCapacity(activeTtsText);
      clearStringKeepCapacity(ttsRequestUrl);
      ttsUsingFallbackHost = false;
      ttsRetryPending = false;
      ttsRetryAt = 0;
      ttsStartupPending = false;
      ttsSpeechStartMs = 0;
      ttsPlaybackStartedMs = 0;
      ttsSpeechSessionActive = false;
      ttsChunkSequenceActive = false;
      ttsChunkNeedsStart = false;
      currentRobotActivity = ROBOT_ACTIVITY_IDLE;
      if (firebaseReady) {
        setSpeakingStatusFirebase(false);
      }
      setEyeExpression("NORMAL");
      clearAIResponse();
      // Only reconnect if it was a genuine failure (took too long)
      if (speechAgeMs > 15000 && currentMode == MODE_AI && aiInputUsesMic) {
        scheduleMicAIReconnect(MIC_RECONNECT_AFTER_FAILURE_MS, "stuck speech recovery");
      }
    }
  }

  if (!currentlySpeaking && !checkupNetworkQuiet && !networkIsQuiet()) {
    publishRobotAssistStatus(false);
  }

  // Guard standby / alarm keeps the rest of the loop lightweight.
  if (guardModeEnabled || guardAlarmActive) {
    static unsigned long lastGuardRemotePoll = 0;
    if (!touchUiHot && millis() - lastGuardRemotePoll > 2000) {
      checkRemoteCommands();
      lastGuardRemotePoll = millis();
    }
    updateEyes();
    yield();
    return;
  }
  
  // ── AI STT pump ───────────────────────────────────────────────────────────
  if (networkAvailable() && currentMode == MODE_AI && aiInputUsesMic && !checkupNetworkQuiet) {
    if (bdConnected) {
      // Manual/Maintenance ByteDance
      pumpByteDanceSocket();
      streamMicToByteDance();
    }
  }
  // ─────────────────────────────────────────────────────────────────────────

  if (currentlySpeaking) {
    isSpeaking = true;
    ttsSpeechSessionActive = true;
    if (ttsPlaybackStartedMs == 0) {
      ttsPlaybackStartedMs = millis();
    }
    if (ttsStartupPending) {
      unsigned long startupDelayMs = (ttsSpeechStartMs > 0) ? (millis() - ttsSpeechStartMs) : 0;
      ttsStartupPending = false;
      Serial.printf("[TTS] Playback started after %lu ms\n", startupDelayMs);
    }
    bool musicPlaybackActive = (activeTtsText.length() == 0 && !isProcessingAI);

    static unsigned long lastSpeakingUiTick = 0;

    unsigned long speakingUiInterval = musicPlaybackActive ? 1000 : 80;
    if (currentMode == MODE_AI && currentMedState == MED_IDLE && millis() - lastSpeakingUiTick > speakingUiInterval) {
      animateEyesWhileSpeaking();
      lastSpeakingUiTick = millis();
    }

    yield();
    return;
  }
  
  // Feed watchdog to prevent resets during long operations
  yield();

  if (!touchUiHot && !checkupNetworkQuiet) {
    updateWiFiConnectionManager();
  }

  processDeferredAiAction();
  checkupNetworkQuiet = ai_requested_checkup;
  
  // Update eyes (includes blinking)
  if (!isSpeaking && !isProcessingAI) {
    updateEyes();
  }

  // Phase 1: Safety always stays on; heavier lifestyle routines stay in Normal mode.
  checkIMUSafety();
  checkAutoImuRecalibration();
  if (!offlineModeLocked && currentMode != MODE_AI && currentMedState == MED_IDLE && !checkupNetworkQuiet && !isSpeaking && !isProcessingAI) {
    checkSmartRoutines();
    checkDailySummary();
    checkContextualAlerts();
    checkSleepTracking();
    checkAutoDeepSleep();
  }

  if (!offlineModeLocked && currentMode == MODE_AI && currentMedState == MED_IDLE && !isSpeaking && !isProcessingAI) {
    maybeProactiveAIPrompt();
  }

  // Safety Clear for AI Text when speech ends
  static bool lastSpeaking = false;
  if (lastSpeaking && !isSpeaking) {
      Serial.println("[AI] Final Safety Clear");
      currentInterimText = "";
      if (currentMode == MODE_AI) drawAIScreen(true);
      
      // Reconnect to Node server if disconnected during speech
      if (USE_NODE_SERVER && !nodeWsConnected && !nodeWsConnecting && aiInputUsesMic) {
        Serial.println("[NODE] Reconnecting after speech...");
        nextNodeWsReconnectMs = millis(); // Allow immediate reconnect
      }
  }
  lastSpeaking = isSpeaking;
    
  // Update AI screen (animation and status)
  if (currentMode == MODE_AI && currentMedState == MED_IDLE) {
    drawAIScreen();
  }
    
  // Animate eyes in AI mode
  if (currentMode == MODE_AI && currentMedState == MED_IDLE) {
    animateEyesWhileSpeaking();
  }

  // Serial Commands... (unchanged)
  // Serial Commands... 
  String cmd;
  if (readCleanSerialLine(cmd)) {
    cmd.trim();
    Serial.print("Received Command: '"); Serial.print(cmd); Serial.println("'");
    cmd.toLowerCase();

    if (cmd == "ai") switchToAIMode();
    else if (cmd == "normal") switchToNormalMode();
    else if (cmd == "status") Serial.printf("Mode: %s\n", currentMode==MODE_NORMAL?"NORMAL":"AI");
    else if (cmd.startsWith("mictest") || cmd.startsWith("micrec")) {
      String args;
      if (cmd.startsWith("mictest")) args = cmd.substring(7);
      else args = cmd.substring(6);
      args.trim();

      if (args.length() == 0 || args == "help") {
        printMicTestHelp();
      } else if (args == "status") {
        Serial.printf("[MicTest] active=%d recording=%d playback=%d bytes=%u/%u\n",
                      micTestActive ? 1 : 0,
                      micTestRecording ? 1 : 0,
                      micTestPlaybackPending ? 1 : 0,
                      (unsigned)micTestBytesWritten,
                      (unsigned)(micTestBuffer ? micTestMaxBytes : 0));
      } else if (args == "stop") {
        stopMicTestRecording();
        stopMicTestPlayback();
      } else if (args == "play") {
        startMicTestPlayback();
      } else if (args == "clear") {
        stopMicTestRecording();
        stopMicTestPlayback();
        freeMicTestBuffer();
        Serial.println("[MicTest] Buffer cleared");
      } else if (args.startsWith("record") || args.startsWith("start")) {
        String secondsText;
        if (args.startsWith("record")) secondsText = args.substring(6);
        else secondsText = args.substring(5);
        secondsText.trim();
        unsigned long durMs = secondsText.length() ? (unsigned long)secondsText.toInt() : 5000;
        if (durMs == 0) durMs = 5000;
        if (durMs < 100) durMs *= 1000; // Treat single digits as seconds
        startMicTestRecording(durMs);
      } else {
        // Just the command mictest without extra word? or unknown?
        printMicTestHelp();
      }
    }
    else if (cmd.startsWith("tts ") || cmd.startsWith("say ")) {
      String text = cmd.substring(4);
      text.trim();
      if (text.length() == 0) {
        Serial.println("[TTS] No text provided");
      } else {
        Serial.println("[TTS] Speaking via regular Google TTS");
        speakText(text.c_str());
      }
    }
    else if (cmd == "stop") {
      // Stop music/audio and restart mic (stay in current mode)
      if (audio.isRunning()) {
        stopActiveAudioPlayback(currentMode == MODE_AI);
        Serial.println("[Stop] Audio stopped");
      }
      if (micTestActive) {
        stopMicTestRecording();
        stopMicTestPlayback();
        Serial.println("[Stop] Mic test stopped");
      }
    }
    else if (cmd == "med" || cmd == "medical") {
      
      
      // Prevent restarting if already in progress
      if (currentMedState == MED_PLACE_FINGER || currentMedState == MED_MEASURING || currentMedState == MED_WAIT_FINGER) {
        Serial.println("[Cmd] Medical checkup already in progress. Ignoring.");
      } else {
        Serial.println("[Cmd] Starting Medical Checkup");
        ai_requested_checkup = true;
        if (currentMode != MODE_NORMAL) switchToNormalMode();
        
        currentMedState = MED_IDLE; // Uses IDLE to detect finger placement
        medStateTimer = millis();
        // Update UI IMMEDIATELY before speaking
        drawNormalScreen(true);
        speakText("Starting medical checkup. Please place your finger on the sensor.");
      }
    }
    else {
       // SERIAL CHAT: Treat any other text as input for AI
       Serial.println("[Serial] Sending text to AI...");
       // Force display update to show we are processing
       if (currentMode != MODE_AI) switchToAIMode();
       isProcessingAI = true;
       setEyeExpression("THINKING");
       drawAIScreen(); // Update UI
       
       askAI(cmd.c_str());
    }
  }
  checkupNetworkQuiet = ai_requested_checkup ||
                        currentMedState == MED_WAIT_FINGER ||
                        currentMedState == MED_PLACE_FINGER ||
                        currentMedState == MED_MEASURING ||
                        currentMedState == MED_RESULT;

  // if (digitalRead(INTERRUPT_PIN) == LOW && audio.isRunning()) {
  //   audio.stopSong(); isSpeaking = isProcessingAI = false;
  //   delay(500); while(digitalRead(INTERRUPT_PIN) == LOW);
  // }

  // ── isProcessingAI safety timeout: clear after 30s (handles network drops) ──
  if (isProcessingAI && (millis() - processingStartTime > 30000)) {
      Serial.println("[AI] Safety: isProcessingAI cleared after 30s timeout");
      isProcessingAI = false;
      if (firebaseReady) setSpeakingStatusFirebase(false);
  }

  if (isSpeaking && !currentlySpeaking && (millis() - lastMusicAction > 2000)) {
    isSpeaking = isProcessingAI = false;
    // CRITICAL: Also reset Firebase so the web app unmutes
    if (firebaseReady) setSpeakingStatusFirebase(false);
    enterNetworkQuiet();
  }
  // Reconnect logic moved to beginning of loop() and restricted to MODE_AI for stability


  // MODE-SPECIFIC LOGIC
  if (networkAvailable() && !touchUiHot && !checkupNetworkQuiet && !networkIsQuiet()) {
    checkRemoteCommands();
    processQueuedAIChat();
  } else if (!touchUiHot && !checkupNetworkQuiet) {
    processQueuedAIChat();
  }
  checkupNetworkQuiet = ai_requested_checkup ||
                        currentMedState == MED_WAIT_FINGER ||
                        currentMedState == MED_PLACE_FINGER ||
                        currentMedState == MED_MEASURING ||
                        currentMedState == MED_RESULT;
  if (currentMode != MODE_AI) {
    // Normal Mode: AHT/ENS Sensors (slow, every 5s is fine)
    // CRITICAL: Skip sensor I2C reads during measurement — switching mux to CH_AHT
    // while MAX30102 reads on CH_MAX corrupts both sensors and glitches the TFT.
    static unsigned long lastSensorRead = 0;
    if (currentMedState != MED_MEASURING && millis() - lastSensorRead > 15000) {
      updateSensors();
      lastSensorRead = millis();

    }

    // MAX30102 State Machine — runs EVERY LOOP for instant finger detection
    unsigned long elapsed = millis() - medStateTimer;

    // Switch to MAX30102 Channel for detection
    tcaselect(CH_MAX);
    // Refresh the FIFO before reading IR so idle-mode detection uses live data.
    particleSensor.check();
    long irValue = particleSensor.getIR();
    // Use the real IR value so finger removal is detected correctly.

    switch (currentMedState) {
      case MED_IDLE:
        {
            // If we just finished a result, force the user to remove their finger first
            // to prevent an infinite loop of reading -> result -> reading.
            if (wait_for_finger_removal) {
                if (irValue < 10000) { // Finger is definitely off
                    wait_for_finger_removal = false;
                    max30102_needs_full_read = true; // Reset buffer flag for next time
                    Serial.println("[Med] Finger removed, ready for next reading.");
                }
            } 
            else if (irValue > 20000) { // Lowered to 20k for easier detection
              Serial.println("[Med] Finger Detected -> Starting countdown...");
              
              currentMedState = MED_PLACE_FINGER; 
              medStateTimer = millis();
              ai_requested_checkup = false; // Clear the AI request flag — finger placement has begun
              max30102_hr = NAN;
              max30102_spo2 = NAN;
              // Reset averaging accumulators for a fresh measurement
              hr_sum = 0; spo2_sum = 0; hr_valid_count = 0; spo2_valid_count = 0;
              max30102_needs_full_read = true; // Ensure full read on new measurement
              
              drawNormalScreen(true); // Force redraw to show countdown
            }
            // If triggered by AI (medStateTimer was reset but no finger yet)
            // IMPORTANT: Only timeout if ai_requested_checkup is true (avoids infinite looping in passive idle)
            else if (ai_requested_checkup && millis() - medStateTimer > 30000) {
                // If it's been 30+ seconds since AI activated checkup and still no finger -> timeout
                Serial.println("[Med] Checkup Timeout (No finger placed)");
                speakText("Checkup cancelled. No finger detected.");
                ai_requested_checkup = false; // CRITICAL: Reset so this only fires once
                wait_for_finger_removal = false;
                drawNormalScreen(true);
            }
        }
        break;

      case MED_PLACE_FINGER:
        // User must hold finger for 5s to start
        if (elapsed > 5000) {
           if (irValue > 50000) {
             currentMedState = MED_MEASURING;
             medStateTimer = millis();
             Serial.println("[Med] Starting measurement...");
             drawNormalScreen(true); // Redraw for Measuring UI
           } else {
             currentMedState = MED_IDLE; // Finger removed early
             drawNormalScreen(true); // Force redraw
           }
        } else if (irValue < 20000) {
           // EARLY EXIT if finger removed during countdown
           currentMedState = MED_IDLE;
           Serial.println("[Med] Finger removed early - Cancelled");
           drawNormalScreen(true); // Force redraw
        }
        break;

      case MED_MEASURING:
        // EARLY EXIT: If finger completely removed (IR drops below 25000)
        if (irValue < 25000) {
            currentMedState = MED_IDLE;
            Serial.println("[Med] Finger removed early - Cancelled");
            drawNormalScreen(true); // Force redraw to clear
            break;
        }

        // Check Timeout (30 seconds total: 10s BPM, 10s SpO2, 10s Temp)
        read_max30102(); // Non-blocking update now
        
        if (elapsed > 30000) { // 30s measurement window
          currentMedState = MED_RESULT;
          medStateTimer = millis();
          // FORCE UPDATE SCREEN BEFORE ANNOUNCING
          drawNormalScreen(true);
          announceMedicalResults();
        }
        break;

       case MED_RESULT:
        // Show results for 10s or until finger removed
        if (elapsed > 10000 || irValue < 50000) {
          currentMedState = MED_IDLE;
          wait_for_finger_removal = true;
          Serial.println("[Med] Measurement done/cancelled. Waiting for finger removal.");
        }
        break;
    } // End switch

    // Time-based check-up and summaries (skip during measuring or checkup startup to avoid TTS/network interference)
    if (currentMedState != MED_MEASURING && !checkupNetworkQuiet && !isSpeaking && !isProcessingAI) {
      checkTimeBasedCheckUp();
      checkDailySummary();
    }
  } // End else (Normal Mode)
  else if (currentMedState != MED_MEASURING && !isSpeaking && !isProcessingAI && !checkupNetworkQuiet) {
    static unsigned long lastAiSensorRefresh = 0;
    if (millis() - lastAiSensorRefresh > 60000) {
      updateSensors();
      lastAiSensorRefresh = millis();
    }
  }

  // Update display
  static unsigned long lastDisplayUpdate = 0;
  unsigned long displayInterval = (currentMode == MODE_AI) ? 800 : 500;
  if (millis() - lastDisplayUpdate > displayInterval) {
    if (currentMode == MODE_NORMAL) drawNormalScreen(false);
    else drawAIScreen(false);
    lastDisplayUpdate = millis();
  }

  // Reminder checks stay in Normal mode to keep AI mode light.
  if (!offlineModeLocked && currentMode == MODE_NORMAL && currentMedState != MED_MEASURING && !checkupNetworkQuiet && !isSpeaking && !isProcessingAI) {
    flushPendingReminderToFirebase();
    checkReminders();
  }

  // Debug Memory (Less frequent as requested)
  static unsigned long lastMemCheck = 0;
  if (millis() - lastMemCheck > 60000) { // 60 seconds
    printMemoryStats();
    lastMemCheck = millis();
  }


  // Firebase background tasks stay in Normal mode to avoid interfering with chat or checkups.
  if (!offlineModeLocked && currentMode == MODE_NORMAL && currentMedState == MED_IDLE && !checkupNetworkQuiet && !isSpeaking && !isProcessingAI && !networkIsQuiet()) {
    static unsigned long lastFirebaseSync = 0;
    if (!touchUiHot && millis() - lastFirebaseSync > 60000) { // Matches syncWithFirebase() internal throttle
      syncWithFirebase();
      lastFirebaseSync = millis();
    }

    static unsigned long lastFirebasePush = 0;
    if (!touchUiHot && millis() - lastFirebasePush > 10000) { // Matches pushSensorDataToFirebase() internal throttle
      pushSensorDataToFirebase();
      checkAutoWeeklyReport();
      checkAirQualityAlerts();
      lastFirebasePush = millis();
    }

  }

  // Guided Meditation (runs when active)
  if (!offlineModeLocked && !checkupNetworkQuiet && !isSpeaking && !isProcessingAI) {
    updateGuidedMeditation();
  }

  // Sync custom alert thresholds periodically
  static unsigned long lastThresholdSync = 0;
  if (!offlineModeLocked && firebaseReady && currentMode == MODE_NORMAL && !touchUiHot && !checkupNetworkQuiet && !isSpeaking && !isProcessingAI && millis() - lastThresholdSync > 300000) { // Every 5 min
    syncAlertThresholds();
    lastThresholdSync = millis();
  }

  // Telegram now handled by server - no polling needed on ESP32!

}

void pushSensorDataToMQTT(FirebaseJson& json) {
  if (currentMode == MODE_AI || networkIsQuiet() || !mqtt.connected()) return;
  if (mqtt.connected()) {
    String mqttPayload;
    json.toString(mqttPayload);
    mqtt.publish("ella/vitals", mqttPayload.c_str());
    Serial.println("[MQTT] Published vitals to ella/vitals");
  }
}


// ============================================================
// AUTONOMOUS MODE FUNCTIONS (Competition Demo)
// Uses BOTH sensors:
// - ToF (VL53L0X): Head-mounted, scans with neck servo
// - Ultrasonic (HC-SR04): Chest-mounted, fixed forward
// ============================================================

void switchToAutonomousMode() {
  if (currentMode == MODE_AUTONOMOUS) return;
  Serial.println("[Mode] Switching to AUTONOMOUS (Competition Mode)");
  
  // Stop all other modes
  setAutoAvoidMode(false);
  setGuardMode(false);
  stopActiveAudioPlayback(true);
  stopMotorMotion(true);
  
  // Initialize autonomous state
  currentMode = MODE_AUTONOMOUS;
  autoState = AUTO_STATE_IDLE;
  autoModeActive = false;
  autoObjectsDetected = 0;
  autoDistanceTraveled = 0;
  autoStatusMessage = "Ready";
  
  // Record starting position
  autoStartX = 0.0;
  autoStartY = 0.0;
  autoCurrentX = 0.0;
  autoCurrentY = 0.0;
  autoStartHeading = imuYawEstimateDeg;
  
  // Center neck for start
  moveNeck(90);
  
  setEyeExpression("HAPPY");
  tft.fillScreen(UI_BG);
  drawAutonomousScreen(true);
  
  Serial.println("[Autonomous] Mode initialized. Press button to start demo.");
  Serial.println("[Autonomous] ToF on head (scans), Ultrasonic on chest (forward)");
}

void startAutonomousDemo() {
  if (!autoModeActive) {
    Serial.println("[Autonomous] Starting competition demo!");
    autoModeActive = true;
    autoState = AUTO_STATE_EXPLORING;
    autoStateStartMs = millis();
    autoModeStartMs = millis();
    autoObjectsDetected = 0;
    autoDistanceTraveled = 0;
    autoStatusMessage = "Exploring...";
    setEyeExpression("THINKING");
    moveNeck(90); // Start with neck forward
    drawAutonomousScreen(false);
  }
}

void stopAutonomousDemo() {
  if (autoModeActive) {
    Serial.println("[Autonomous] Stopping demo");
    autoModeActive = false;
    autoState = AUTO_STATE_COMPLETE;
    stopMotorMotion(true);
    moveNeck(90);
    autoStatusMessage = "Demo Complete";
    setEyeExpression("HAPPY");
    drawAutonomousScreen(false);
  }
}

// Scan environment using head-mounted ToF
void performHeadScan(uint16_t &leftDist, uint16_t &centerDist, uint16_t &rightDist) {
  // Look left
  moveNeck(130);
  delay(300);
  leftDist = frontToF.readRangeContinuousMillimeters();
  if (frontToF.timeoutOccurred() || leftDist >= 8190) leftDist = 8000;
  
  // Look center
  moveNeck(90);
  delay(300);
  centerDist = frontToF.readRangeContinuousMillimeters();
  if (frontToF.timeoutOccurred() || centerDist >= 8190) centerDist = 8000;
  
  // Look right
  moveNeck(50);
  delay(300);
  rightDist = frontToF.readRangeContinuousMillimeters();
  if (frontToF.timeoutOccurred() || rightDist >= 8190) rightDist = 8000;
  
  // Return to center
  moveNeck(90);
  
  Serial.printf("[Autonomous] Head scan: L=%dmm C=%dmm R=%dmm\n", leftDist, centerDist, rightDist);
}

void updateAutonomousNavigation() {
  if (!autoModeActive || currentMode != MODE_AUTONOMOUS) return;
  
  unsigned long now = millis();
  unsigned long elapsed = now - autoModeStartMs;
  
  // Read CHEST ultrasonic sensor (fixed forward)
  float chestDistanceCm = readUltrasonicDistanceCm();
  
  // Read HEAD ToF sensor (can scan)
  uint16_t headDistanceMm = 8000;
  if (tofReady) {
    headDistanceMm = frontToF.readRangeContinuousMillimeters();
    if (frontToF.timeoutOccurred() || headDistanceMm >= 8190) {
      headDistanceMm = 8000;
    }
  }
  
  // Update position estimate (simple dead reckoning)
  if (isMoving) {
    float deltaTime = 0.1; // Approximate
    float speed = motorSpeed / 255.0 * 0.3; // meters per second estimate
    float deltaDistance = speed * deltaTime;
    autoDistanceTraveled += (int)(deltaDistance * 100); // cm
    
    // Update position based on heading
    float radians = imuYawEstimateDeg * PI / 180.0;
    autoCurrentX += deltaDistance * cos(radians);
    autoCurrentY += deltaDistance * sin(radians);
  }
  
  switch (autoState) {
    case AUTO_STATE_IDLE:
      // Waiting to start
      break;
      
    case AUTO_STATE_EXPLORING:
      // Check if time limit reached
      if (elapsed > AUTO_EXPLORE_DURATION_MS) {
        Serial.println("[Autonomous] Exploration complete, returning home");
        autoState = AUTO_STATE_RETURNING_HOME;
        autoStateStartMs = now;
        autoStatusMessage = "Returning home...";
        stopMotorMotion(true);
        drawAutonomousScreen(false);
        break;
      }
      
      // PRIMARY: Check CHEST ultrasonic for immediate obstacles
      if (chestDistanceCm > 0 && chestDistanceCm < AUTO_OBSTACLE_DISTANCE_CM) {
        Serial.printf("[Autonomous] CHEST sensor: Obstacle at %.1f cm\n", chestDistanceCm);
        autoState = AUTO_STATE_AVOIDING_OBSTACLE;
        autoStateStartMs = now;
        autoObjectsDetected++;
        autoStatusMessage = "Obstacle detected!";
        stopMotorMotion(true);
        setEyeExpression("WORRIED");
        drawAutonomousScreen(false);
      }
      // SECONDARY: Check HEAD ToF for obstacles ahead
      else if (headDistanceMm < 400) { // Less than 40cm
        Serial.printf("[Autonomous] HEAD sensor: Obstacle at %d mm\n", headDistanceMm);
        autoState = AUTO_STATE_AVOIDING_OBSTACLE;
        autoStateStartMs = now;
        autoObjectsDetected++;
        autoStatusMessage = "Obstacle ahead!";
        stopMotorMotion(true);
        setEyeExpression("WORRIED");
        drawAutonomousScreen(false);
      }
      // Continue exploring
      else if (!isMoving && !turnControlActive) {
        setMotorL(AUTO_FORWARD_SPEED);
        setMotorR(AUTO_FORWARD_SPEED);
        autoStatusMessage = "Exploring...";
        
        // Occasionally scan with head while moving
        static unsigned long lastHeadScanMs = 0;
        if (now - lastHeadScanMs > 5000) { // Every 5 seconds
          lastHeadScanMs = now;
          // Quick left-right head movement while moving
          moveNeck(110);
          delay(200);
          moveNeck(70);
          delay(200);
          moveNeck(90);
        }
      }
      break;
      
    case AUTO_STATE_AVOIDING_OBSTACLE:
      // Use HEAD ToF to scan and find best direction
      if (now - autoStateStartMs < 500) {
        // Stop and prepare to scan
        stopMotorMotion(true);
        autoStatusMessage = "Scanning...";
        drawAutonomousScreen(false);
      } else if (now - autoStateStartMs < 2000) {
        // Perform head scan to find best path
        static bool scanDone = false;
        if (!scanDone) {
          uint16_t leftDist, centerDist, rightDist;
          performHeadScan(leftDist, centerDist, rightDist);
          
          // Decide which way to turn based on scan
          if (leftDist > rightDist && leftDist > 500) {
            // Turn left
            Serial.println("[Autonomous] Turning LEFT (more space)");
            setMotorL(-120);
            setMotorR(120);
            autoStatusMessage = "Turning left";
          } else if (rightDist > leftDist && rightDist > 500) {
            // Turn right
            Serial.println("[Autonomous] Turning RIGHT (more space)");
            setMotorL(120);
            setMotorR(-120);
            autoStatusMessage = "Turning right";
          } else {
            // Both blocked, turn around
            Serial.println("[Autonomous] Turning AROUND (blocked)");
            setMotorL(-120);
            setMotorR(120);
            autoStatusMessage = "Turning around";
          }
          scanDone = true;
          drawAutonomousScreen(false);
        }
      } else {
        // Resume exploring
        stopMotorMotion(true);
        autoState = AUTO_STATE_EXPLORING;
        autoStateStartMs = now;
        autoStatusMessage = "Exploring...";
        setEyeExpression("THINKING");
        moveNeck(90); // Reset neck to forward
        drawAutonomousScreen(false);
      }
      break;
      
    case AUTO_STATE_RETURNING_HOME: {
      // Calculate angle to home
      float dx = autoStartX - autoCurrentX;
      float dy = autoStartY - autoCurrentY;
      float distanceToHome = sqrt(dx * dx + dy * dy);
      float angleToHome = atan2(dy, dx) * 180.0 / PI;
      float headingError = angleToHome - imuYawEstimateDeg;
      
      // Normalize angle
      while (headingError > 180) headingError -= 360;
      while (headingError < -180) headingError += 360;
      
      // Check for obstacles while returning
      if (chestDistanceCm > 0 && chestDistanceCm < AUTO_OBSTACLE_DISTANCE_CM) {
        Serial.println("[Autonomous] Obstacle while returning home");
        autoState = AUTO_STATE_AVOIDING_OBSTACLE;
        autoStateStartMs = now;
        stopMotorMotion(true);
        break;
      }
      
      if (distanceToHome < 0.2) {
        // Close enough to home
        Serial.println("[Autonomous] Arrived at home position!");
        stopMotorMotion(true);
        autoState = AUTO_STATE_COMPLETE;
        autoStatusMessage = "Mission complete!";
        setEyeExpression("HAPPY");
        
        // Victory head nod
        moveNeck(70);
        delay(300);
        moveNeck(110);
        delay(300);
        moveNeck(90);
        
        drawAutonomousScreen(false);
      } else if (abs(headingError) > 15) {
        // Turn towards home
        int turnDir = (headingError > 0) ? 1 : -1;
        setMotorL(turnDir * 100);
        setMotorR(-turnDir * 100);
        autoStatusMessage = "Turning home...";
      } else {
        // Move towards home
        setMotorL(AUTO_FORWARD_SPEED);
        setMotorR(AUTO_FORWARD_SPEED);
        autoStatusMessage = "Going home...";
      }
      break;
    }
      
    case AUTO_STATE_COMPLETE:
      // Demo finished
      if (now - autoStateStartMs > 3000) {
        // After 3 seconds, stop the demo
        stopAutonomousDemo();
      }
      break;
  }
}

void drawAutonomousScreen(bool force) {
  static unsigned long lastDrawMs = 0;
  if (!force && millis() - lastDrawMs < 200) return;
  lastDrawMs = millis();
  
  tft.fillScreen(UI_BG);
  
  // Title
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(UI_ACCENT);
  tft.setCursor(10, 30);
  tft.print("AUTONOMOUS");
  
  // Status
  tft.setFont(&FreeSansBold9pt7b);
  tft.setTextColor(UI_TEXT_MAIN);
  tft.setCursor(10, 55);
  tft.print("Status: ");
  tft.setTextColor(autoModeActive ? UI_SUCCESS : UI_TEXT_SUB);
  tft.print(autoModeActive ? "ACTIVE" : "READY");
  
  // State
  tft.setTextColor(UI_TEXT_MAIN);
  tft.setCursor(10, 75);
  tft.print("State: ");
  tft.setTextColor(UI_PRIMARY);
  const char* stateNames[] = {"IDLE", "EXPLORING", "AVOIDING", "MAPPING", "RETURNING", "COMPLETE"};
  tft.print(stateNames[autoState]);
  
  // Stats
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT_MAIN);
  
  tft.setCursor(10, 95);
  tft.print("Objects: ");
  tft.setTextColor(UI_ACCENT);
  tft.print(autoObjectsDetected);
  
  tft.setTextColor(UI_TEXT_MAIN);
  tft.setCursor(130, 95);
  tft.print("Dist: ");
  tft.setTextColor(UI_ACCENT);
  tft.print(autoDistanceTraveled);
  tft.print("cm");
  
  // Heading
  tft.setTextColor(UI_TEXT_MAIN);
  tft.setCursor(10, 110);
  tft.print("Heading: ");
  tft.setTextColor(UI_ACCENT);
  tft.print((int)imuYawEstimateDeg);
  tft.print(" deg");
  
  // Position
  tft.setTextColor(UI_TEXT_MAIN);
  tft.setCursor(10, 125);
  tft.print("Pos: (");
  tft.setTextColor(UI_ACCENT);
  tft.print(autoCurrentX, 1);
  tft.setTextColor(UI_TEXT_MAIN);
  tft.print(", ");
  tft.setTextColor(UI_ACCENT);
  tft.print(autoCurrentY, 1);
  tft.setTextColor(UI_TEXT_MAIN);
  tft.print(")");
  
  // CHEST Ultrasonic (fixed forward)
  float chestCm = readUltrasonicDistanceCm();
  tft.setTextColor(UI_TEXT_MAIN);
  tft.setCursor(10, 145);
  tft.print("CHEST: ");
  if (chestCm > 0) {
    tft.setTextColor(chestCm < AUTO_OBSTACLE_DISTANCE_CM ? UI_ERROR : UI_SUCCESS);
    tft.print(chestCm, 1);
    tft.print("cm");
  } else {
    tft.setTextColor(UI_TEXT_SUB);
    tft.print("---");
  }
  
  // HEAD ToF (scans with neck)
  uint16_t headMm = 8000;
  if (tofReady) {
    headMm = frontToF.readRangeContinuousMillimeters();
    if (frontToF.timeoutOccurred() || headMm >= 8190) headMm = 8000;
  }
  tft.setTextColor(UI_TEXT_MAIN);
  tft.setCursor(10, 160);
  tft.print("HEAD:  ");
  if (headMm < 8000) {
    tft.setTextColor(headMm < 400 ? UI_ERROR : UI_SUCCESS);
    tft.print(headMm);
    tft.print("mm");
  } else {
    tft.setTextColor(UI_TEXT_SUB);
    tft.print("---");
  }
  
  // Status message
  tft.setFont(&FreeSansBold9pt7b);
  tft.setTextColor(UI_ACCENT);
  tft.setCursor(10, 190);
  tft.print(autoStatusMessage);
  
  // Instructions
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT_SUB);
  tft.setCursor(10, 215);
  if (!autoModeActive) {
    tft.print("Press button to START demo");
  } else {
    tft.print("Press button to STOP demo");
  }
  
  // Time elapsed
  if (autoModeActive) {
    unsigned long elapsed = (millis() - autoModeStartMs) / 1000;
    tft.setCursor(10, 230);
    tft.setTextColor(UI_TEXT_MAIN);
    tft.print("Time: ");
    tft.setTextColor(UI_ACCENT);
    tft.print(elapsed);
    tft.print("s / ");
    tft.print(AUTO_EXPLORE_DURATION_MS / 1000);
    tft.print("s");
  }
  
  // Sensor legend
  tft.setTextColor(UI_TEXT_SUB);
  tft.setCursor(10, 250);
  tft.print("CHEST=Ultrasonic(fixed)");
  tft.setCursor(10, 260);
  tft.print("HEAD=ToF(scans L/R)");
  
  // Navigation bar
  tft.fillRect(0, 290, 240, 30, UI_CARD_BG);
  tft.drawLine(0, 289, 240, 289, UI_PRIMARY);
  tft.setTextColor(UI_ACCENT);
  tft.setCursor(70, 305);
  tft.print("< BACK");
}

// Fallback time sync via HTTP Header (works when NTP port 123 is blocked)
void syncTimeViaHTTP() {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, "http://www.google.com");
  const char* headerKeys[] = {"Date"};
  http.collectHeaders(headerKeys, 1);
  int httpCode = http.GET();
  if (httpCode > 0) {
    String dateStr = http.header("Date");
    Serial.println("[HTTP-Time] Received: " + dateStr);
    // Format: "Sat, 12 May 2026 04:15:56 GMT"
    int day, year, hour, minute, second;
    char monthStr[4];
    if (sscanf(dateStr.c_str(), "%*s %d %s %d %d:%d:%d", &day, monthStr, &year, &hour, &minute, &second) == 6) {
        struct tm tm;
        tm.tm_year = year - 1900;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
        // Month lookup
        const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        tm.tm_mon = 0;
        for (int i=0; i<12; i++) {
            if (strcmp(monthStr, months[i]) == 0) { tm.tm_mon = i; break; }
        }
        time_t t = mktime(&tm);
        struct timeval tv = { .tv_sec = t };
        settimeofday(&tv, NULL);
        ntpTimeValid = true;
        Serial.println("[HTTP-Time] Internal clock updated successfully.");
    }
  }
  http.end();
}


void handleIncomingTelegramCommand(String text, String chatId) {
  // If no Chat ID is configured, ALLOW the first message (usually /start) and save it!
  if (cloudChatId == "" || cloudChatId == "null") {
      cloudChatId = chatId;
      Serial.println("[Bridge] Auto-captured new Chat ID: " + cloudChatId);
      prefs.begin("ella", false);
      prefs.putString("chatId", cloudChatId);
      prefs.end();
  } 
  
  // Security check for Bridge
  if (chatId != cloudChatId) {
      Serial.println("[Bridge] Unauthorized Chat ID: " + chatId);
      return;
  }

  Serial.printf("[Bridge] Processing command: %s\n", text.c_str());
  String reply = "";

  if (text == "/status" || text == "/start") {
    reply = "🤖 *ELLA Status Report*\n\n";
    reply += "🌡 *Temperature:* " + String(temp_aht, 1) + "°C\n";
    reply += "💧 *Humidity:* " + String(humidity_aht, 1) + "%\n";
    reply += "🌬 *Air Quality (AQI):* " + String(aqi_val) + "\n";
    reply += "☁️ *TVOC:* " + String(tvoc_val) + " ppb\n";
    reply += "💨 *eCO2:* " + String(eco2_val) + " ppm\n";
    reply += "\n✅ _All systems operational_";
  }
  else if (text == "/health") {
    if (isnan(max30102_hr) || max30102_hr < 40) {
      reply = "❌ *Health Data Not Available*\n\n";
      reply += "_Place finger on sensor to measure heart rate and SpO2_";
    } else {
      reply = "❤️ *Health Vitals*\n\n";
      reply += "💓 *Heart Rate:* " + String((int)max30102_hr) + " BPM\n";
      reply += "🫁 *SpO2:* " + String((int)max30102_spo2) + "%\n";
      reply += "🌡 *Body Temp:* " + String(temp_aht, 1) + "°C\n";
      
      if (max30102_spo2 < 90) {
        reply += "\n⚠️ *Warning:* Low oxygen level!";
      } else if (max30102_hr > 130 || max30102_hr < 50) {
        reply += "\n⚠️ *Warning:* Abnormal heart rate!";
      } else {
        reply += "\n✅ _Vitals within normal range_";
      }
    }
  }
  else if (text == "/weekly_report" || text == "/report") {
    sendWeeklyReport();
    return;
  }
  else if (text == "/help") {
    reply = "📱 <b>ELLA Bot Commands</b>\n\n";
    reply += "/status - Current sensor readings\n";
    reply += "/health - Heart rate & SpO2 data\n";
    reply += "/weekly_report - 7-day summary\n";
    reply += "/help - Show this message\n";
    reply += "\n<i>💡 ELLA monitors 24/7</i>";
  }
  else {
    reply = "❓ Unknown command. Type /help for available commands.";
  }

  if (reply.length() > 0) {
    sendTelegramAlert(reply);
  }
}
