#include <Arduino.h>
#include <WiFi.h>
#include "Audio.h"

// ==========================================
// IMPORTANT: Update your WiFi credentials!
// ==========================================
const char* ssid = "ella";
const char* password = "12345678";

// I2S Speaker Pins
#define SPK_BCLK 7
#define SPK_LRC  21
#define SPK_DOUT 18

Audio audio;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Isolated TTS & Wi-Fi Noise Test ===");
  Serial.println("Testing Google TTS streaming over Wi-Fi (No motors, no sensors)");

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected.");

  audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DOUT);
  audio.setVolume(21); // Matched to your main sketch volume
  audio.forceMono(true); // Matched to your main sketch setup
  
  Serial.println("Playing Google TTS...");
  audio.connecttospeech("Hello! This is a test of the text to speech system to check for any buzzing or clicking noises.", "en");
}

void loop() {
  audio.loop();
  
  // Repeat the test 5 seconds after speech finishes
  static unsigned long lastSpeakMs = 0;
  if (!audio.isRunning()) {
      if (lastSpeakMs == 0) {
          lastSpeakMs = millis();
      } else if (millis() - lastSpeakMs > 5000) {
          Serial.println("Playing again...");
          audio.connecttospeech("Testing again. Listen for any hiss or static when I stop speaking.", "en");
          lastSpeakMs = 0; // Reset for next time
      }
  }
}

// Optional callback to see what the audio library is doing
void audio_info(const char *info){
    Serial.print("[Audio Info] "); Serial.println(info);
}

