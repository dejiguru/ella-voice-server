// ============================================================
// ELLA MOUTH DISPLAY - Secondary ESP32
// Receives text/expressions from main ELLA ESP via UART
// ============================================================

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

// TFT Pins (adjust for your hardware)
#define TFT_CS    5   // SS
#define TFT_DC    4   // DC
#define TFT_RST   2   // RST

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// UART pins (default Serial for programming, Serial1 for communication)
// TX of ESP32 connects to RX of main ELLA
// RX of ESP32 connects to TX of main ELLA

// Mouth state
enum MouthState {
  MOUTH_IDLE,
  MOUTH_SPEAKING,
  MOUTH_HAPPY,
  MOUTH_SAD,
  MOUTH_SURPRISED,
  MOUTH_LISTENING
};

MouthState currentMouth = MOUTH_IDLE;
unsigned long lastUpdate = 0;
String currentText = "";
bool newMessage = false;

// Colors
#define MOUTH_BG     ILI9341_BLACK
#define MOUTH_COLOR  ILI9341_CYAN
#define LIP_COLOR    ILI9341_PINK

// ============================================================
void setup() {
  Serial.begin(115200);  // Debug
  Serial1.begin(115200); // Communication with main ESP

  tft.begin();
  tft.setRotation(1); // Landscape
  tft.fillScreen(MOUTH_BG);

  // Draw idle mouth
  drawIdleMouth();

  Serial.println("[MOUTH] ELLA Mouth Display Ready");
  Serial1.println("[MOUTH] READY"); // Signal to main ESP
}

// ============================================================
void loop() {
  // Check for incoming messages from main ESP
  if (Serial1.available()) {
    String msg = Serial1.readStringUntil('\n');
    msg.trim();

    if (msg.length() > 0) {
      processMessage(msg);
    }
  }

  // Update mouth animation based on state
  unsigned long now = millis();
  if (now - lastUpdate > 100) {
    updateMouthAnimation();
    lastUpdate = now;
  }
}

// ============================================================
void processMessage(String msg) {
  Serial.print("[MOUTH] Received: ");
  Serial.println(msg);

  // Protocol: MOUTH:EXPRESSION or TEXT:"message" or SAY:"message"

  if (msg.startsWith("EXPR:")) {
    // Expression command
    String expr = msg.substring(5);
    setExpression(expr);
  }
  else if (msg.startsWith("SAY:")) {
    // Speech command - show text and speaking animation
    currentText = msg.substring(4);
    currentMouth = MOUTH_SPEAKING;
    newMessage = true;
    showSpeakingText(currentText);
  }
  else if (msg.startsWith("IDLE:")) {
    currentMouth = MOUTH_IDLE;
    drawIdleMouth();
  }
  else if (msg.startsWith("LISTEN")) {
    currentMouth = MOUTH_LISTENING;
    drawListeningMouth();
  }
  else if (msg.startsWith("HAPPY")) {
    currentMouth = MOUTH_HAPPY;
    drawHappyMouth();
  }
  else if (msg.startsWith("SAD")) {
    currentMouth = MOUTH_SAD;
    drawSadMouth();
  }
  else if (msg.startsWith("SURPRISE")) {
    currentMouth = MOUTH_SURPRISED;
    drawSurprisedMouth();
  }
  else {
    // Default: treat as text to display
    currentText = msg;
    currentMouth = MOUTH_SPEAKING;
    showSpeakingText(msg);
  }
}

// ============================================================
void setExpression(String expr) {
  expr.toUpperCase();

  if (expr == "HAPPY" || expr == "JOY") {
    currentMouth = MOUTH_HAPPY;
    drawHappyMouth();
  }
  else if (expr == "SAD" || expr == "SORRY") {
    currentMouth = MOUTH_SAD;
    drawSadMouth();
  }
  else if (expr == "SURPRISE" || expr == "SHOCK") {
    currentMouth = MOUTH_SURPRISED;
    drawSurprisedMouth();
  }
  else if (expr == "LISTEN" || expr == "HEAR") {
    currentMouth = MOUTH_LISTENING;
    drawListeningMouth();
  }
  else {
    currentMouth = MOUTH_IDLE;
    drawIdleMouth();
  }
}

// ============================================================
void updateMouthAnimation() {
  if (currentMouth == MOUTH_SPEAKING) {
    // Animate talking - alternating open/closed
    static bool talkState = false;
    talkState = !talkState;

    if (talkState) {
      drawOpenMouth();
    } else {
      drawClosedMouth();
    }
  }
}

// ============================================================
void showSpeakingText(String text) {
  tft.fillScreen(MOUTH_BG);
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(MOUTH_COLOR);
  tft.setTextSize(1);

  // Center text
  int16_t x = (tft.width() - tft.textWidth(text.c_str())) / 2;
  int16_t y = tft.height() / 2;

  tft.setCursor(x, y);
  tft.println(text);
}

// ============================================================
void drawIdleMouth() {
  tft.fillScreen(MOUTH_BG);

  // Draw closed smile
  int centerX = tft.width() / 2;
  int centerY = tft.height() / 2;
  int mouthWidth = 120;
  int mouthHeight = 20;

  tft.fillRoundRect(centerX - mouthWidth/2, centerY - mouthHeight/2,
                    mouthWidth, mouthHeight, 10, LIP_COLOR);

  // Draw teeth hint
  tft.fillRect(centerX - 40, centerY - 5, 80, 8, ILI9341_WHITE);
}

// ============================================================
void drawOpenMouth() {
  tft.fillScreen(MOUTH_BG);

  int centerX = tft.width() / 2;
  int centerY = tft.height() / 2;

  // Open mouth (oval)
  tft.fillEllipse(centerX, centerY, 70, 45, LIP_COLOR);
  tft.fillEllipse(centerX, centerY, 55, 35, ILI9341_BLACK);

  // Tongue
  tft.fillEllipse(centerX, centerY + 20, 30, 15, ILI9341_RED);
}

// ============================================================
void drawClosedMouth() {
  drawIdleMouth();
}

// ============================================================
void drawHappyMouth() {
  tft.fillScreen(MOUTH_BG);

  int centerX = tft.width() / 2;
  int centerY = tft.height() / 2;

  // Big smile
  tft.fillRoundRect(centerX - 80, centerY - 10, 160, 50, 25, LIP_COLOR);
  tft.fillRect(centerX - 70, centerY, 140, 30, ILI9341_BLACK);

  // Teeth
  tft.fillRect(centerX - 50, centerY, 100, 15, ILI9341_WHITE);
}

// ============================================================
void drawSadMouth() {
  tft.fillScreen(MOUTH_BG);

  int centerX = tft.width() / 2;
  int centerY = tft.height() / 2 + 20;

  // Frown
  tft.fillRoundRect(centerX - 60, centerY - 30, 120, 40, 20, LIP_COLOR);
  tft.fillRect(centerX - 50, centerY - 20, 100, 20, ILI9341_BLACK);
}

// ============================================================
void drawSurprisedMouth() {
  tft.fillScreen(MOUTH_BG);

  int centerX = tft.width() / 2;
  int centerY = tft.height() / 2;

  // O-shaped mouth
  tft.fillEllipse(centerX, centerY, 50, 50, LIP_COLOR);
  tft.fillEllipse(centerX, centerY, 35, 35, ILI9341_BLACK);
}

// ============================================================
void drawListeningMouth() {
  tft.fillScreen(MOUTH_BG);

  int centerX = tft.width() / 2;
  int centerY = tft.height() / 2;

  // Small open mouth (listening)
  tft.fillEllipse(centerX, centerY, 30, 20, LIP_COLOR);
  tft.fillEllipse(centerX, centerY, 20, 12, ILI9341_BLACK);
}