// 1. Core Arduino and Network includes MUST go first
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <EEPROM.h> 
#include <Servo.h>

// 2. Add explicit type mapping for legacy libraries to avoid core breakages
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;

// 3. Temporarily silence unused parameter warnings inside the third-party stack
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#pragma GCC diagnostic pop 

// --- Configuration Flags ---
const bool testRun = true;        // Set to true for 1 min = 1 sec testing. Set to false for actual baking.
const bool ServoMode = true;      // Set to true to use Servo Motors, false for direct digital outputs.
const int DebugLevel = 2; 

// --- Stable Pin Assignments ---
const int MenuPin     = D1; // GPIO5 (Stable)
const int MinusPin    = D2; // GPIO4 (Stable)
const int RunResetPin = D5; // GPIO14 (Stable)
const int ColourPin   = D6; // GPIO12 (Stable)

// --- Servo Instances ---
Servo MenuServo;
Servo MinusServo;
Servo RunResetServo;
Servo ColourServo;

// --- AP Default Fallback Credentials ---
const char* defaultSsid = "SourDough";
const char* defaultPassword = "12376254"; 

// Dynamic Network Strings populated out of EEPROM storage
String clientSsid = "";
String clientPassword = "";

bool clientLocked = false;
IPAddress allowedClientIP;

AsyncWebServer server(80);

// --- State and Variables ---
enum EngineState { WIFI_CONFIG_AP, MENU_SELECTION, KNEADING, DEGAS, HOTPROOF, PROOF, BAKE, DONE };
EngineState currentState = MENU_SELECTION;

enum BakeColourOption { LIGHT = 0, MEDIUM = 1, DARK = 2 };
BakeColourOption bakeColour = DARK;

// Operational Mode Enumerations
enum OpModeOption { MODE_NONE = 0, MODE_STANDALONE = 1, MODE_WIFI = 2 };
OpModeOption operationMode = MODE_WIFI; 

bool isPaused = false;
unsigned long previousMillis = 0;

// Parameter Options and Defaults
int kneadMin = 30;
int degasMin = 30;
float hotProofHr = 1.0;
float proofHr = 1.0; 
int bakeMin = 60;
int remainingMin = 0;

// Dynamic Structural Tracking Variables
int breadStateStep = 0; 
unsigned long stepTimer = 0;
bool stepActive = false;
int loopCounter = 0; 

// Trackers to suppress duplicate periodic log messages
EngineState lastLoggedState = MENU_SELECTION;
int lastLoggedStep = -1;
int lastLoggedLoop = -1;

// Spinning indicator
const char spinner[] = {'/', '-', '\\', '|'};
int spinnerIdx = 0;

// --- EEPROM Layout Address Mapping ---
const int EEPROM_SIZE = 128; 
const int ADDR_VALID_MARKER = 0; 
const int ADDR_KNEAD        = 1;
const int ADDR_DEGAS        = 2;
const int ADDR_PROOF        = 3; 
const int ADDR_BAKE         = 4;
const int ADDR_BAKE_COLOUR  = 5;  
const int ADDR_OP_MODE      = 6;  
const int ADDR_WIFI_MARKER  = 7;  
const int ADDR_WIFI_SSID    = 8;   
const int ADDR_WIFI_PASS    = 40;  
const int ADDR_HOTPROOF     = 104; // Reserved non-overlapping address slot

const uint8_t VALID_CONFIG_MAGIC = 0xAA; 
const uint8_t VALID_WIFI_MAGIC   = 0xBB;

// --- Asynchronous Web Event Action Flags ---
volatile bool triggerRun = false;
volatile bool triggerReset = false;
volatile bool triggerEraseAll = false;
volatile bool triggerSaveConfig = false;
volatile bool triggerSwitchMode = false; 
OpModeOption targetSwitchMode = MODE_WIFI;

String pendingSsid = "";
String pendingPassword = "";
OpModeOption pendingOpMode = MODE_WIFI;

// --- Forward Declarations for defaults ---
String getFormattedTime(int totalMinutes);
String getPinName(int pin);
Servo* getServoByPin(int pin);
void shortPress(int pin, int numberOfTimes = 1);
void longPress(int pin, int numberOfTimes = 1);

// --- Helper UI functions ---
void calculateRemainingTime() {
  if (currentState == MENU_SELECTION) {
    remainingMin = kneadMin + degasMin + (int)(hotProofHr * 60.0 + 0.5) + (int)(proofHr * 60.0 + 0.5) + bakeMin;
  }
}

String getFormattedTime(int totalMinutes) {
  int hours = totalMinutes / 60;
  int minutes = totalMinutes % 60;
  char buf[16]; 
  sprintf(buf, "%02d:%02d", hours, minutes);
  return String(buf);
}

String getPinName(int pin) {
  if (pin == MenuPin) return "MenuPin";
  if (pin == MinusPin) return "MinusPin";
  if (pin == RunResetPin) return "RunResetPin";
  if (pin == ColourPin) return "ColourPin";
  return "UnknownPin";
}

String getBakeColourName(BakeColourOption opt) {
  if (opt == LIGHT) return "Light";
  if (opt == MEDIUM) return "Medium";
  return "Dark";
}

String getStateName(EngineState state) {
  switch (state) {
    case WIFI_CONFIG_AP: return "WIFI_CONFIG_AP";
    case MENU_SELECTION: return "MENU_SELECTION";
    case KNEADING:       return "KNEADING";
    case DEGAS:          return "DEGAS";
    case HOTPROOF:       return "HOTPROOF";
    case PROOF:          return "PROOF";
    case BAKE:           return "BAKE";
    case DONE:           return "DONE";
  }
  return "UNKNOWN";
}

// Helper to interact with the right servo instance based on the Pin ID
Servo* getServoByPin(int pin) {
  if (pin == MenuPin) return &MenuServo;
  if (pin == MinusPin) return &MinusServo;
  if (pin == RunResetPin) return &RunResetServo;
  if (pin == ColourPin) return &ColourServo;
  return nullptr;
}

// --- Dynamic Pin Press Actions ---
void shortPress(int pin, int numberOfTimes) {
  if (DebugLevel >= 2) {
    Serial.print("["); Serial.print(getFormattedTime(remainingMin)); 
    Serial.print("] ShortPress -> "); Serial.print(getPinName(pin));
    Serial.print(" "); Serial.println(numberOfTimes);
  }

  for (int i = 0; i < numberOfTimes; i++) {
    if (!ServoMode) {
      digitalWrite(pin, LOW);
      delay(200);
      digitalWrite(pin, HIGH);
      delay(300);
    } else {
      Servo* targetServo = getServoByPin(pin);
      if (targetServo != nullptr) {
        targetServo->write(90);
        delay(200);
        targetServo->write(0);
        delay(300);
      }
    }
  }
}

void longPress(int pin, int numberOfTimes) {
  if (DebugLevel >= 2) {
    Serial.print("["); Serial.print(getFormattedTime(remainingMin)); 
    Serial.print("] LongPress -> "); Serial.print(getPinName(pin));
    Serial.print(" "); Serial.println(numberOfTimes);
  }

  for (int i = 0; i < numberOfTimes; i++) {
    if (!ServoMode) {
      digitalWrite(pin, LOW);
      delay(2000);
      digitalWrite(pin, HIGH);
      delay(1000);
    } else {
      Servo* targetServo = getServoByPin(pin);
      if (targetServo != nullptr) {
        targetServo->write(90);
        delay(2000);
        targetServo->write(0);
        delay(1000);
      }
    }
  }
}

// --- Bake Colour Signal Routine ---
void executeBakeColourSequence() {
  if (DebugLevel >= 1) {
    Serial.print("["); Serial.print(getFormattedTime(remainingMin)); 
    Serial.print("] Execution Routing -> BakeColour Process Initialized. Selection: ");
    Serial.println(getBakeColourName(bakeColour));
  }

  if (bakeColour == DARK) {
    shortPress(ColourPin, 1);
  } else if (bakeColour == LIGHT) {
    shortPress(ColourPin, 2);
  } else {
    if (DebugLevel >= 1) {
      Serial.print("["); Serial.print(getFormattedTime(remainingMin));
      Serial.println("] Execution Routing -> BakeColour is Medium, no adjustments required.");
    }
  }
}

// --- EEPROM Storage Helpers ---
void saveSettingsToEEPROM() {
  EEPROM.write(ADDR_VALID_MARKER, VALID_CONFIG_MAGIC);
  EEPROM.write(ADDR_KNEAD, (uint8_t)kneadMin);
  EEPROM.write(ADDR_DEGAS, (uint8_t)degasMin);
  
  int totalProofMin = (int)(proofHr * 60.0 + 0.5);
  EEPROM.write(ADDR_PROOF, (uint8_t)(totalProofMin / 10)); 
  
  int totalHotProofMin = (int)(hotProofHr * 60.0 + 0.5);
  EEPROM.write(ADDR_HOTPROOF, (uint8_t)(totalHotProofMin / 10));

  EEPROM.write(ADDR_BAKE, (uint8_t)bakeMin);
  EEPROM.write(ADDR_BAKE_COLOUR, (uint8_t)bakeColour);
  EEPROM.commit();
}

void loadSettingsFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t marker = EEPROM.read(ADDR_VALID_MARKER);
  
  if (marker == VALID_CONFIG_MAGIC) {
    kneadMin = (int)EEPROM.read(ADDR_KNEAD);
    degasMin = (int)EEPROM.read(ADDR_DEGAS);
    
    int savedProofUnits = (int)EEPROM.read(ADDR_PROOF);
    proofHr = (float)(savedProofUnits * 10) / 60.0;

    int savedHotProofUnits = (int)EEPROM.read(ADDR_HOTPROOF);
    hotProofHr = (float)(savedHotProofUnits * 10) / 60.0;
    
    bakeMin  = (int)EEPROM.read(ADDR_BAKE);
    bakeColour = (BakeColourOption)EEPROM.read(ADDR_BAKE_COLOUR);
  }
}

void saveOpModeToEEPROM(OpModeOption mode) {
  EEPROM.write(ADDR_OP_MODE, (uint8_t)mode);
  EEPROM.commit();
}

OpModeOption loadOpModeFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t modeByte = EEPROM.read(ADDR_OP_MODE);
  if (modeByte == (uint8_t)MODE_STANDALONE) return MODE_STANDALONE;
  if (modeByte == (uint8_t)MODE_WIFI) return MODE_WIFI;
  return MODE_NONE; 
}

void saveWifiToEEPROM(String ssidStr, String passStr) {
  EEPROM.write(ADDR_WIFI_MARKER, VALID_WIFI_MAGIC);
  
  for (int i = 0; i < 32; i++) EEPROM.write(ADDR_WIFI_SSID + i, 0);
  for (int i = 0; i < 64; i++) EEPROM.write(ADDR_WIFI_PASS + i, 0);
  
  for (size_t i = 0; i < ssidStr.length() && i < 31; i++) {
    EEPROM.write(ADDR_WIFI_SSID + i, ssidStr[i]);
  }
  for (size_t i = 0; i < passStr.length() && i < 63; i++) {
    EEPROM.write(ADDR_WIFI_PASS + i, passStr[i]);
  }
  EEPROM.commit();
}

void eraseWifiConfig() {
  EEPROM.write(ADDR_WIFI_MARKER, 0); 
  EEPROM.commit();
}

void eraseAllEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
}

bool hasWifiConfigInEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  return (EEPROM.read(ADDR_WIFI_MARKER) == VALID_WIFI_MAGIC);
}

void loadWifiFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  clientSsid = "";
  for (int i = 0; i < 32; i++) {
    char c = EEPROM.read(ADDR_WIFI_SSID + i);
    if (c == 0) break;
    clientSsid += c;
  }
  
  clientPassword = "";
  for (int i = 0; i < 64; i++) {
    char c = EEPROM.read(ADDR_WIFI_PASS + i);
    if (c == 0) break;
    clientPassword += c;
  }
}

// --- Process Logic Encapsulations ---
void executeStandaloneAPProcess() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(defaultSsid, defaultPassword);
}

bool executeWifiConnectionProcess() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(clientSsid.c_str(), clientPassword.c_str());
  
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt < 60000UL)) {
    delay(500);
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    EEPROM.write(ADDR_OP_MODE, (uint8_t)MODE_NONE);
    EEPROM.commit();
    delay(1000);
    ESP.restart();
    return false;
  }
  
  return true;
}

// --- HTML Provisioning Pages ---
String generateWifiSetupHtml() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Sourdough Maker Configuration</title>";
  html += "<style>body { font-family: Arial; text-align: center; margin-top: 50px; background-color: #f7f9fa; }";
  html += ".card { background: white; padding: 30px; max-width: 350px; margin: auto; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  html += "input[type=text], input[type=password], select { width: 90%; padding: 10px; margin: 10px 0; font-size: 14px; }";
  html += "input[type=submit] { background-color: #4CAF50; color: white; padding: 12px; border: none; border-radius: 4px; cursor: pointer; width: 96%; font-size: 16px; }</style>";
  html += "</head><body><div class='card'>";
  html += "<h2>Sourdough Maker Configuration</h2>";
  html += "<form action='/save_config' method='POST'>";
  
  html += "<label style='float:left; margin-left:5%; font-size:13px;'>Operation Mode:</label>";
  html += "<select name='opmode'>";
  html += "  <option value='2' selected>WIFI</option>";
  html += "  <option value='1'>Standalone</option>";
  html += "</select><br>";

  html += "<input type='text' name='ssid' placeholder='WiFi SSID' value='" + clientSsid + "'><br>";
  html += "<input type='password' name='password' placeholder='WiFi Password' value='" + clientPassword + "'><br><br>";
  html += "<input type='submit' value='Save Configurations'>";
  html += "</form></div></body></html>";
  return html;
}

String generateHtml() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>SourDough Maker</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; background-color: #f7f9fa; margin:20px; }";
  html += ".highlight { background-color: #ffeb3b; font-weight: bold; padding: 5px; border-radius: 4px; }";
  html += "select, button { padding: 10px; font-size: 16px; margin: 10px; }";
  html += "button { background-color: #4CAF50; color: white; border: none; cursor: pointer; border-radius: 4px;}";
  html += "button.pause { background-color: #ff9800; }";
  html += "button.reset { background-color: #f44336; }";
  html += "button.erase-init { background-color: #757575; font-size: 13px; padding: 6px 12px; margin: 5px auto; display: block; }";
  html += "button.erase-confirm { background-color: #d32f2f; font-size: 13px; padding: 6px 12px; font-weight: bold; margin: 5px auto; display: none; }";
  html += "button.switch-init { background-color: #0288d1; font-size: 13px; padding: 6px 12px; margin: 5px auto; display: block; }";
  html += "button.switch-confirm { background-color: #01579b; font-size: 13px; padding: 6px 12px; font-weight: bold; margin: 5px auto; display: none; }";
  html += ".card { background: white; padding: 20px; max-width: 400px; margin: auto; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  html += "</style>";
  
  html += "<script>";
  html += "setInterval(function() { if(!" + String(isPaused) + " || " + String(currentState == MENU_SELECTION) + ") { window.location.reload(); } }, 3000);";
  html += "function exposeConfirmButton() {";
  html += "  document.getElementById('eraseInitBtn').style.display = 'none';";
  html += "  document.getElementById('eraseConfirmBtn').style.display = 'block';";
  html += "}";
  html += "function exposeWifiConfirmButton() {";
  html += "  document.getElementById('switchWifiInitBtn').style.display = 'none';";
  html += "  document.getElementById('switchWifiConfirmBtn').style.display = 'block';";
  html += "}";
  html += "function exposeStandaloneConfirmButton() {";
  html += "  document.getElementById('switchStandaloneInitBtn').style.display = 'none';";
  html += "  document.getElementById('switchStandaloneConfirmBtn').style.display = 'block';";
  html += "}";
  html += "</script>";
  html += "</head><body><div class='card'>";
  html += "<h2>SourDough Maker " + String(testRun ? "(TEST MODE)" : "") + "</h2>";

  if (currentState == MENU_SELECTION) {
    html += "<div style='padding: 5px 0;'>";
    html += "  <button id='eraseInitBtn' class='erase-init' onclick='exposeConfirmButton()'>Erase Settings</button>";
    html += "  <button id='eraseConfirmBtn' class='erase-confirm' onclick='location.href=\"/erase_all\"'>Confirm to Erase Settings</button>";
    
    if (operationMode == MODE_STANDALONE) {
      html += "  <button id='switchWifiInitBtn' class='switch-init' onclick='exposeWifiConfirmButton()'>Switch to WIFI</button>";
      html += "  <button id='switchWifiConfirmBtn' class='switch-confirm' onclick='location.href=\"/switch_mode?to=wifi\"'>Confirm to switch to WIFI</button>";
    } else if (operationMode == MODE_WIFI) {
      html += "  <button id='switchStandaloneInitBtn' class='switch-init' onclick='exposeStandaloneConfirmButton()'>Switch to Standalone</button>";
      html += "  <button id='switchStandaloneConfirmBtn' class='switch-confirm' onclick='location.href=\"/switch_mode?to=standalone\"'>Confirm to switch to Standalone</button>";
    }
    
    html += "</div>";
    html += "<hr>";
  }

  html += "<p style='margin-top: 12px;'><strong>Status: </strong>";
  if (currentState == MENU_SELECTION) {
    html += "Select Options then press Run";
  } else if (currentState == DONE) {
    html += "Sourdough Done";
  } else if (isPaused) {
    html += "Pausing";
  } else {
    html += "Running";
  }
  
  html += " | <strong>Total Time:</strong> " + getFormattedTime(remainingMin);
  if (currentState != MENU_SELECTION && currentState != DONE && !isPaused) {
    html += " " + String(spinner[spinnerIdx]);
  }
  html += "</p><hr>";

  String disabledAttr = (currentState != MENU_SELECTION) ? "disabled" : "";
  
  html += "<div>";
  html += "<label>Bake Colour: </label>";
  html += "<select name='colour' " + disabledAttr + " onchange='location.href=\"/set?c=\"+this.value'>";
  html += "<option value='0' " + String(bakeColour == LIGHT ? "selected" : "") + ">Light</option>";
  html += "<option value='1' " + String(bakeColour == MEDIUM ? "selected" : "") + ">Medium</option>";
  html += "<option value='2' " + String(bakeColour == DARK ? "selected" : "") + ">Dark</option>";
  html += "</select></div>";

  html += "<div class='" + String(currentState == KNEADING ? "highlight" : "") + "'>";
  html += "<label>Knead (min): </label>";
  html += "<select name='knead' " + disabledAttr + " onchange='location.href=\"/set?k=\"+this.value'>";
  int kneadOptions[] = {15, 30, 45, 60, 90};
  for (int i = 0; i < 5; i++) {
    int m = kneadOptions[i];
    html += "<option value='" + String(m) + "' " + (kneadMin == m ? "selected" : "") + ">" + String(m) + "</option>";
  }
  html += "</select></div>";

  html += "<div class='" + String(currentState == DEGAS ? "highlight" : "") + "'>";
  html += "<label>Degas (min): </label>";
  html += "<select name='degas' " + disabledAttr + " onchange='location.href=\"/set?d=\"+this.value'>";
  int degasOptions[] = {15, 30, 45, 60, 90, 120, 150, 180};
  for (int i = 0; i < 8; i++) {
    int m = degasOptions[i];
    html += "<option value='" + String(m) + "' " + (degasMin == m ? "selected" : "") + ">" + String(m) + "</option>";
  }
  html += "</select></div>";

  html += "<div class='" + String(currentState == HOTPROOF ? "highlight" : "") + "'>";
  html += "<label>HotProof (hr): </label>";
  html += "<select name='hotproof' " + disabledAttr + " onchange='location.href=\"/set?hp=\"+this.value'>";
  float hotProofOptions[] = {0.0, 0.5, 1.0, 2.0, 3.0, 6.0, 9.0, 12.0, 15.0, 18.0, 21.0, 24.0};
  for (int i = 0; i < 12; i++) {
    float h = hotProofOptions[i];
    String label = (h == 0.0) ? "0" : ((h == 0.5) ? "0.5" : String((int)h));
    html += "<option value='" + String(h, 1) + "' " + (hotProofHr == h ? "selected" : "") + ">" + label + "</option>";
  }
  html += "</select></div>";

  html += "<div class='" + String(currentState == PROOF ? "highlight" : "") + "'>";
  html += "<label>Proof (hr): </label>";
  html += "<select name='proof' " + disabledAttr + " onchange='location.href=\"/set?p=\"+this.value'>";
  float proofOptions[] = {0.0, 0.5, 1.0, 2.0, 3.0, 6.0, 9.0, 12.0, 15.0, 18.0, 21.0, 24.0};
  for (int i = 0; i < 12; i++) {
    float h = proofOptions[i];
    String label = (h == 0.0) ? "0" : ((h == 0.5) ? "0.5" : String((int)h));
    html += "<option value='" + String(h, 1) + "' " + (proofHr == h ? "selected" : "") + ">" + label + "</option>";
  }
  html += "</select></div>";

  html += "<div class='" + String(currentState == BAKE ? "highlight" : "") + "'>";
  html += "<label>Bake (min): </label>";
  html += "<select name='bake' " + disabledAttr + " onchange='location.href=\"/set?b=\"+this.value'>";
  int bakeOptions[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
  for (int i = 0; i < 12; i++) {
    int m = bakeOptions[i];
    html += "<option value='" + String(m) + "' " + (bakeMin == m ? "selected" : "") + ">" + String(m) + "</option>";
  }
  html += "</select></div><br>"; 

  if (currentState == MENU_SELECTION) {
    html += "<button onclick='location.href=\"/run\"'>Run</button><br>";
  } 
  
  if (currentState != MENU_SELECTION && currentState != DONE) {
    html += "<button class='pause' onclick='location.href=\"/pause\"'>" + String(isPaused ? "Resume" : "Pause") + "</button><br>";
  }
  
  html += "<button class='reset' onclick='location.href=\"/reset\"'>Restart</button>";
  html += "</div></body></html>";
  return html;
}

bool isAuthorizedClient(AsyncWebServerRequest *request) {
  IPAddress clientIP = request->client()->remoteIP();
  if (!clientLocked) {
    allowedClientIP = clientIP;
    clientLocked = true;
    return true;
  }
  return (clientIP == allowedClientIP);
}

void setup() {
  Serial.begin(115200);
  delay(200); 

  if (ServoMode) {
    MenuServo.attach(MenuPin);
    MinusServo.attach(MinusPin);
    RunResetServo.attach(RunResetPin);
    ColourServo.attach(ColourPin);

    MenuServo.write(0);
    MinusServo.write(0);
    RunResetServo.write(0);
    ColourServo.write(0);
  } else {
    pinMode(MenuPin, OUTPUT);
    pinMode(MinusPin, OUTPUT);
    pinMode(RunResetPin, OUTPUT);
    pinMode(ColourPin, OUTPUT);
    
    digitalWrite(MenuPin, HIGH);
    digitalWrite(MinusPin, HIGH);
    digitalWrite(RunResetPin, HIGH);
    digitalWrite(ColourPin, HIGH); 
  }

  loadSettingsFromEEPROM();
  calculateRemainingTime();

  loadWifiFromEEPROM();
  OpModeOption storedMode = loadOpModeFromEEPROM();

  if (storedMode == MODE_STANDALONE) {
    operationMode = MODE_STANDALONE;
    executeStandaloneAPProcess();
    currentState = MENU_SELECTION; 
  } 
  else if (storedMode == MODE_WIFI) {
    operationMode = MODE_WIFI;
    if (executeWifiConnectionProcess()) {
      currentState = MENU_SELECTION; 
    }
  } 
  else {
    currentState = WIFI_CONFIG_AP;
    executeStandaloneAPProcess();
  }

  // --- Web Routing Definitions ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState == WIFI_CONFIG_AP) {
      request->send(200, "text/html", generateWifiSetupHtml());
    } else {
      if (!isAuthorizedClient(request)) {
        request->send(403, "text/plain", "Access Denied: Exclusive Session Active.");
        return;
      }
      request->send(200, "text/html", generateHtml());
    }
  });

  server.on("/erase_all", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState != MENU_SELECTION) {
      request->send(403, "text/plain", "Action Denied: Process running.");
      return;
    }
    triggerEraseAll = true; 
    request->send(200, "text/plain", "EEPROM Erase Triggered safely. Device is rebooting...");
  });

  server.on("/switch_mode", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState != MENU_SELECTION) {
      request->send(403, "text/plain", "Action Denied: Mode locked during execution.");
      return;
    }

    if (request->hasParam("to")) {
      String target = request->getParam("to")->value();
      if (target == "wifi") {
        targetSwitchMode = MODE_WIFI;
        triggerSwitchMode = true;
        request->send(200, "text/plain", "Switching to WIFI Mode. Device is rebooting...");
        return;
      } else if (target == "standalone") {
        targetSwitchMode = MODE_STANDALONE;
        triggerSwitchMode = true;
        request->send(200, "text/plain", "Switching to Standalone Mode. Device is rebooting...");
        return;
      }
    }
    request->redirect("/");
  });

  server.on("/save_config", HTTP_POST, [](AsyncWebServerRequest *request){
    if (currentState == WIFI_CONFIG_AP) {
      int modeVal = MODE_WIFI;
      if (request->hasParam("opmode", true)) modeVal = request->getParam("opmode", true)->value().toInt();
      pendingOpMode = (OpModeOption)modeVal;
      
      if (request->hasParam("ssid", true)) pendingSsid = request->getParam("ssid", true)->value();
      if (request->hasParam("password", true)) pendingPassword = request->getParam("password", true)->value();
      
      triggerSaveConfig = true; 
      request->send(200, "text/plain", "Configuration data accepted. Restarting...");
      return;
    }
    request->redirect("/");
  });

  server.on("/set", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState == MENU_SELECTION) {
      if (request->hasParam("c")) bakeColour = (BakeColourOption)request->getParam("c")->value().toInt();
      if (request->hasParam("k")) kneadMin = request->getParam("k")->value().toInt();
      if (request->hasParam("d")) degasMin = request->getParam("d")->value().toInt();
      if (request->hasParam("hp")) hotProofHr = request->getParam("hp")->value().toFloat();
      if (request->hasParam("p")) proofHr = request->getParam("p")->value().toFloat();
      if (request->hasParam("b")) bakeMin = request->getParam("b")->value().toInt();
      calculateRemainingTime();
    }
    request->redirect("/");
  });

  server.on("/run", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState == MENU_SELECTION) {
      triggerRun = true; 
    }
    request->redirect("/");
  });

  server.on("/pause", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState != MENU_SELECTION && currentState != DONE && currentState != WIFI_CONFIG_AP) {
      isPaused = !isPaused;
    }
    request->redirect("/");
  });

  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    triggerReset = true; 
    request->redirect("/");
  });

  server.begin();

  // --- Consolidated Setup Debug Logs (Only triggered if DebugLevel >= 1) ---
  if (DebugLevel >= 1) {
    // Exactly 10 lines of separators log prior to system text profiles
    for (int i = 0; i < 10; i++) {
      Serial.println("==================================================");
    }
    
    Serial.println("\n--- SYSTEM BOOT ---");
    Serial.print("[CONFIG] ServoMode status: "); Serial.println(ServoMode ? "ENABLED (True)" : "DISABLED (False)");
    Serial.print("[CONFIG] testRun status  : "); Serial.println(testRun ? "ENABLED (True)" : "DISABLED (False)");
    Serial.print("[CONFIG] DebugLevel level: "); Serial.println(DebugLevel);

    if (ServoMode) {
      Serial.println("[HARDWARE] Servos initialized and anchored to 0 degrees.");
    } else {
      Serial.println("[HARDWARE] Digital Pins configured and pulled HIGH.");
    }

    if (operationMode == MODE_STANDALONE || storedMode == MODE_STANDALONE) {
      Serial.println("[SYSTEM] Standalone-AP Process Initiated.");
      Serial.print("[SYSTEM] Broadcast SSID: "); Serial.println(defaultSsid);
      Serial.print("[SYSTEM] Network Active Gateway IP: "); Serial.println(WiFi.softAPIP());
    } 
    else if (operationMode == MODE_WIFI) {
      Serial.print("[SYSTEM] WIFI-Connection Process Commenced. Attempting access point connection to: "); 
      Serial.println(clientSsid);
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[SYSTEM] Wi-Fi Connection Established Successfully! Assigned local node IP: "); 
        Serial.println(WiFi.localIP());
      } else {
        Serial.println("\n[SYSTEM] ERROR: Wi-Fi Connection failed after 1 minute timeline threshold.");
        Serial.println("[SYSTEM] Erasing local EEPROM OperationMode profile and forcing reboot...");
      }
    }

    if (currentState == MENU_SELECTION) {
      Serial.println("==================================================");
      Serial.print("[SYSTEM] Menu Selection Process Initialized.\n");
      Serial.print("[SYSTEM] Operation Mode: ");
      Serial.println((operationMode == MODE_STANDALONE) ? "Standalone" : "WIFI");
      Serial.println("==================================================");
    }
  }
}

void loop() {
  if (triggerRun) {
    triggerRun = false; 
    if (currentState == MENU_SELECTION) {
      currentState = KNEADING;
      breadStateStep = 0;
      loopCounter = 0;
      stepActive = false;
      previousMillis = millis(); 
      saveSettingsToEEPROM();

      Serial.println("\n==================================================");
      Serial.println("[SYSTEM] --- Sourdough Process Started ---");
      Serial.print("[PARAM]  Bake Colour : "); Serial.println(getBakeColourName(bakeColour));
      Serial.print("[PARAM]  Knead Time  : "); Serial.print(kneadMin); Serial.println(" min");
      Serial.print("[PARAM]  Degas Time  : "); Serial.print(degasMin); Serial.println(" min");
      Serial.print("[PARAM]  HotProof    : "); Serial.print(hotProofHr, 1); Serial.println(" hr");
      Serial.print("[PARAM]  Proof Time  : "); Serial.print(proofHr, 1); Serial.println(" hr");
      Serial.print("[PARAM]  Bake Time   : "); Serial.print(bakeMin); Serial.println(" min");
      Serial.print("[PARAM]  Total Time  : "); Serial.println(getFormattedTime(remainingMin));
      Serial.println("==================================================\n");
    }
  }

  if (triggerReset) {
    triggerReset = false;
    delay(500); 
    ESP.restart(); 
  }

  if (triggerEraseAll) {
    triggerEraseAll = false;
    delay(500);
    eraseAllEEPROM();
    ESP.restart();
  }

  if (triggerSwitchMode) {
    triggerSwitchMode = false;
    delay(500);
    saveOpModeToEEPROM(targetSwitchMode);
    ESP.restart();
  }

  if (triggerSaveConfig) {
    triggerSaveConfig = false;
    delay(500);
    saveOpModeToEEPROM(pendingOpMode);
    if (pendingOpMode == MODE_WIFI) {
      saveWifiToEEPROM(pendingSsid, pendingPassword);
    }
    ESP.restart();
  }

  if (currentState == WIFI_CONFIG_AP || currentState == MENU_SELECTION || currentState == DONE || isPaused) {
    return; 
  }

  unsigned long currentInterval = testRun ? 1000UL : 60000UL;
  unsigned long currentMillis = millis();
  
  if (currentMillis - previousMillis >= currentInterval) {
    previousMillis = currentMillis;
    if (remainingMin > 0) remainingMin--;
    spinnerIdx = (spinnerIdx + 1) % 4;

    if (DebugLevel >= 2) {
      if (currentState != lastLoggedState || breadStateStep != lastLoggedStep || loopCounter != lastLoggedLoop) {
        Serial.print("["); Serial.print(getFormattedTime(remainingMin));
        Serial.print("] Processing Active -> Stage: "); Serial.print(getStateName(currentState));
        Serial.print(" | Step: "); Serial.print(breadStateStep);
        Serial.print(" | Loop: "); Serial.println(loopCounter);

        lastLoggedState = currentState;
        lastLoggedStep = breadStateStep;
        lastLoggedLoop = loopCounter;
      }
    }
  }

  // --- Sourdough Step Sequencing Automation Engine ---
  switch (currentState) {
    case KNEADING: {
      int cycleMin = 30;
      int NoOfLoop = kneadMin / cycleMin;
      int finalMin = kneadMin % cycleMin;

      if (!stepActive) {
        if (breadStateStep == 0) {
          if (loopCounter < NoOfLoop) {
            longPress(RunResetPin);
            shortPress(MenuPin, 7);     
            shortPress(MinusPin, 10);   
            shortPress(RunResetPin);
            stepTimer = millis();
            stepActive = true;
          } else {
            breadStateStep = 1; 
          }
        }
        if (breadStateStep == 1) {
          if (finalMin > 0) {
            longPress(RunResetPin);
            shortPress(MenuPin, 7);     
            shortPress(MinusPin, 10);   
            shortPress(RunResetPin);
            stepTimer = millis();
            stepActive = true;
          } else {
            breadStateStep = 2; 
          }
        }
      } else { 
        unsigned long targetDuration;
        if (breadStateStep == 0) {
          targetDuration = testRun ? (cycleMin * 1000UL) : (cycleMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            loopCounter++;
          }
        } else if (breadStateStep == 1) {
          targetDuration = testRun ? (finalMin * 1000UL) : (finalMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            breadStateStep = 2;
          }
        }
      }
      if (breadStateStep == 2) {
        loopCounter = 0;
        breadStateStep = 0;
        stepActive = false;
        currentState = DEGAS;
      }
      break;
    }

    case DEGAS: {
      int cycleMin = 30;
      int NoOfLoop = degasMin / cycleMin;
      int finalMin = degasMin % cycleMin;

      if (!stepActive) {
        if (breadStateStep == 0) {
          if (loopCounter < NoOfLoop) {
            Serial.print("["); Serial.print(getFormattedTime(remainingMin));
            Serial.print("] LoopCounter: "); Serial.print(loopCounter);
            Serial.print(" | CycleMin: "); Serial.println(cycleMin);

            longPress(RunResetPin);
            shortPress(MenuPin, 7);
            shortPress(MinusPin, 9);
            shortPress(RunResetPin);
            stepTimer = millis();
            stepActive = true;
          } else {
            breadStateStep = 1;
          }
        }
        if (breadStateStep == 1) {
          if (finalMin > 0) {
            Serial.print("["); Serial.print(getFormattedTime(remainingMin));
            Serial.print("] FinalMin: "); Serial.println(finalMin);

            longPress(RunResetPin);
            shortPress(MenuPin, 7);
            shortPress(MinusPin, 9);
            shortPress(RunResetPin);
            stepTimer = millis();
            stepActive = true;
          } else {
            breadStateStep = 2;
          }
        }
      } else {
        unsigned long targetDuration;
        if (breadStateStep == 0) {
          targetDuration = testRun ? (cycleMin * 1000UL) : (cycleMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            loopCounter++;
          }
        } else if (breadStateStep == 1) {
          targetDuration = testRun ? (finalMin * 1000UL) : (finalMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            breadStateStep = 2;
          }
        }
      }
      if (breadStateStep == 2) {
        loopCounter = 0;
        breadStateStep = 0;
        stepActive = false;
        currentState = HOTPROOF;
      }
      break;
    }

    case HOTPROOF: {
      int cycleMin = 60;
      int hotProofMin = (int)(hotProofHr * 60.0 + 0.5);
      int NoOfLoop = hotProofMin / cycleMin;
      int finalMin = hotProofMin % cycleMin;

      if (!stepActive) {
        if (breadStateStep == 0 && loopCounter == 0) {
          Serial.print("["); Serial.print(getFormattedTime(remainingMin));
          Serial.print("] Setup Params -> CycleMin: "); Serial.print(cycleMin);
          Serial.print(" | NoOfLoop: "); Serial.print(NoOfLoop);
          Serial.print(" | FinalMin: "); Serial.println(finalMin);
        }

        if (breadStateStep == 0) {
          if (loopCounter < NoOfLoop) {
            Serial.print("["); Serial.print(getFormattedTime(remainingMin));
            Serial.print("] LoopCounter: "); Serial.print(loopCounter);
            Serial.print(" | CycleMin: "); Serial.println(cycleMin);

            longPress(RunResetPin);
            executeBakeColourSequence();
            shortPress(MenuPin, 9);
            shortPress(MinusPin, 10);
            shortPress(RunResetPin, 1);
            stepTimer = millis();
            stepActive = true;
          } else {
            breadStateStep = 1;
          }
        }
        if (breadStateStep == 1) {
          if (finalMin > 0) {
            Serial.print("["); Serial.print(getFormattedTime(remainingMin));
            Serial.print("] FinalMin: "); Serial.println(finalMin);

            longPress(RunResetPin);
            executeBakeColourSequence();
            shortPress(MenuPin, 9);
            shortPress(MinusPin, 10);
            shortPress(RunResetPin);
            stepTimer = millis();
            stepActive = true;
          } else {
            breadStateStep = 2;
          }
        }
      } else {
        unsigned long targetDuration;
        if (breadStateStep == 0) {
          targetDuration = testRun ? (cycleMin * 1000UL) : (cycleMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            loopCounter++;
          }
        } else if (breadStateStep == 1) {
          targetDuration = testRun ? (finalMin * 1000UL) : (finalMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            breadStateStep = 2;
          }
        }
      }
      if (breadStateStep == 2) {
        loopCounter = 0;
        breadStateStep = 0;
        stepActive = false;
        currentState = PROOF;
      }
      break;
    }

    case PROOF: {
      int cycleMin = 60;
      int NoOfLoop = 0;
      int finalMin = (int)(proofHr * 60.0 + 0.5);

      if (!stepActive) {
        longPress(RunResetPin);
        
        Serial.print("["); Serial.print(getFormattedTime(remainingMin));
        Serial.print("] Setup Params -> CycleMin: "); Serial.print(cycleMin);
        Serial.print(" | NoOfLoop: "); Serial.print(NoOfLoop);
        Serial.print(" | FinalMin: "); Serial.println(finalMin);

        stepTimer = millis();
        stepActive = true;
      } else {
        unsigned long targetDuration = testRun ? (finalMin * 1000UL) : (finalMin * 60000UL);
        if (millis() - stepTimer >= targetDuration) {
          stepActive = false;
          currentState = BAKE;
        }
      }
      break;
    }

    case BAKE: {
      int cycleMin = 60;
      int NoOfLoop = bakeMin / cycleMin;
      int finalMin = bakeMin % cycleMin;

      if (!stepActive) {
        if (breadStateStep == 0 && loopCounter == 0) {
          Serial.print("["); Serial.print(getFormattedTime(remainingMin));
          Serial.print("] Setup Params -> CycleMin: "); Serial.print(cycleMin);
          Serial.print(" | NoOfLoop: "); Serial.print(NoOfLoop);
          Serial.print(" | FinalMin: "); Serial.println(finalMin);
        }

        if (breadStateStep == 0) {
          if (loopCounter < NoOfLoop) {
            Serial.print("["); Serial.print(getFormattedTime(remainingMin));
            Serial.print("] LoopCounter: "); Serial.print(loopCounter);
            Serial.print(" | CycleMin: "); Serial.println(cycleMin);

            longPress(RunResetPin);
            executeBakeColourSequence(); 
            shortPress(MenuPin, 13);
            shortPress(MinusPin, 10);
            shortPress(RunResetPin, 1);
            stepTimer = millis();
            stepActive = true;
          } else {
            breadStateStep = 1;
          }
        }
        if (breadStateStep == 1) {
          if (finalMin > 0) {
            Serial.print("["); Serial.print(getFormattedTime(remainingMin));
            Serial.print("] FinalMin: "); Serial.println(finalMin);

            longPress(RunResetPin);
            executeBakeColourSequence(); 
            shortPress(MenuPin, 13);
            shortPress(MinusPin, 10);
            shortPress(RunResetPin);
            stepTimer = millis();
            stepActive = true;
          } else {
            breadStateStep = 2;
          }
        }
      } else {
        unsigned long targetDuration;
        if (breadStateStep == 0) {
          targetDuration = testRun ? (cycleMin * 1000UL) : (cycleMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            loopCounter++;
          }
        } else if (breadStateStep == 1) {
          targetDuration = testRun ? (finalMin * 1000UL) : (finalMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            breadStateStep = 2;
          }
        }
      }
      if (breadStateStep == 2) {
        longPress(RunResetPin);
        currentState = DONE;
        stepActive = false;
        breadStateStep = 0;
        loopCounter = 0;
      }
      break;
    }
    default:
      break;
  }
}