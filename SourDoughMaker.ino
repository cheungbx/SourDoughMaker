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
const int MenuPin     = D1; // GPIO5
const int MinusPin    = D2; // GPIO4
const int RunResetPin = D5; // GPIO14
const int ColourPin   = D6; // GPIO12

// --- Servo Instances ---
Servo MenuServo;
Servo MinusServo;
Servo RunResetServo;
Servo ColourServo;

// --- AP Default Fallback Configuration ---
const char* defaultPassword = "12376254"; 
String apSsid = "SourDough"; // Will dynamically append chip signature on initialization

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

// Parameter Options and Defaults (Matching User Specifications Exactly)
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
const int ADDR_HOTPROOF     = 104; 

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
void moveToNextValidState();

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

String getOpModeName(OpModeOption opt) {
  if (opt == MODE_STANDALONE) return "STANDALONE (Access Point)";
  if (opt == MODE_WIFI) return "WIFI (Client Station)";
  return "NONE / UNCONFIGURED";
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

Servo* getServoByPin(int pin) {
  if (pin == MenuPin) return &MenuServo;
  if (pin == MinusPin) return &MinusServo;
  if (pin == RunResetPin) return &RunResetServo;
  if (pin == ColourPin) return &ColourServo;
  return nullptr;
}

void computeDynamicAPProperties() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  if (mac.length() >= 6) {
    apSsid += mac.substring(mac.length() - 6);
  } else {
    apSsid += "XXXXXX";
  }
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

void eraseAllEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
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

void executeStandaloneAPProcess() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), defaultPassword);
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
  html += "setInterval(function() { window.location.reload(); }, 3000);";
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
    html += "Pausing:";
  } else {
    html += "Running";
  }
  
  html += " | <strong>Total Time:</strong> " + getFormattedTime(remainingMin);
  if (currentState != MENU_SELECTION && currentState != DONE && !isPaused) {
    html += " " + String(spinner[spinnerIdx]);
  }
  html += "</p><hr>";

  // Locks down options entirely when process leaves MENU_SELECTION state
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
  int kneadOptions[] = {0, 15, 30, 45, 60, 90};
  for (int i = 0; i < 6; i++) {
    int m = kneadOptions[i];
    html += "<option value='" + String(m) + "' " + (kneadMin == m ? "selected" : "") + ">" + String(m) + "</option>";
  }
  html += "</select></div>";

  html += "<div class='" + String(currentState == DEGAS ? "highlight" : "") + "'>";
  html += "<label>Degas (min): </label>";
  html += "<select name='degas' " + disabledAttr + " onchange='location.href=\"/set?d=\"+this.value'>";
  int degasOptions[] = {0, 15, 30, 45, 60, 90, 120, 150, 180};
  for (int i = 0; i < 9; i++) {
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
  int bakeOptions[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
  for (int i = 0; i < 13; i++) {
    int m = bakeOptions[i];
    html += "<option value='" + String(m) + "' " + (bakeMin == m ? "selected" : "") + ">" + String(m) + "</option>";
  }
  html += "</select></div><br>"; 

  // Completely dynamic swap behavior of homepage buttons based on runtime execution
  if (currentState == MENU_SELECTION) {
    html += "<button onclick='location.href=\"/run\"'>Run</button><br>";
  } else if (currentState != DONE) {
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

  // Configurations loaded automatically from memory storage on start
  loadSettingsFromEEPROM();
  calculateRemainingTime();

  computeDynamicAPProperties();
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
      
      // Pause/resume execution mechanics
      if (isPaused) {
        stepTimer = millis() - stepTimer; 
        if (DebugLevel >= 1) {
          Serial.println("[SYSTEM] Process status: Paused");
        }
      } else {
        stepTimer = millis() - stepTimer;
        previousMillis = millis(); 
        if (DebugLevel >= 1) {
          Serial.println("[SYSTEM] Process status: Running");
        }
      }
    }
    request->redirect("/");
  });

  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    triggerReset = true; 
    request->redirect("/");
  });

  server.begin();

  // --- UNIFIED PROGRAM START COLD LOGGING ---
  if (DebugLevel >= 1) {
    for (int i = 0; i < 5; i++) Serial.println("==================================================");
    Serial.println("\n--- SYSTEM BOOT ---");
    Serial.printf("[CONFIG] ServoMode status: %s\n", ServoMode ? "ENABLED" : "DISABLED");
    Serial.printf("[CONFIG] testRun status  : %s\n", testRun ? "ENABLED (1s=1m)" : "DISABLED");

    if (operationMode == MODE_WIFI && WiFi.status() == WL_CONNECTED) {
      Serial.printf("[SYSTEM] Connected Node IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.printf("[SYSTEM] Broadcast SSID: %s\n", apSsid.c_str());
    }
    
    Serial.printf("[SYSTEM] Active Wi-Fi Mode: %s\n", getOpModeName(operationMode).c_str());
  }
}

// --- Global sequence logic engine dealing with automated skips & logging updates ---
void moveToNextValidState() {
  breadStateStep = 0;
  loopCounter = 0;
  stepActive = false;

  if (currentState == KNEADING) {
    currentState = DEGAS;
    if (degasMin == 0) {
      Serial.println("Skipping the Degas Process");
      moveToNextValidState();
    } else if (DebugLevel >= 1) {
      int cycleMin = 30;
      int NoOfLoop = degasMin / cycleMin;
      int finalMin = degasMin % cycleMin;
      Serial.printf("Starting Process: DEGAS | cycleMin: %d | NoOfLoop: %d | finalMin: %d\n", cycleMin, NoOfLoop, finalMin);
    }
 } else if (currentState == DEGAS) {
    currentState = HOTPROOF;
    if (hotProofHr == 0.0) {
      Serial.println("Skipping the HotProof Process");
      moveToNextValidState();
    } else if (DebugLevel >= 1) {
      int cycleMin = 60;
      int hotProofMin = (int)(hotProofHr * 60.0 + 0.5); 
      int NoOfLoop = hotProofMin / cycleMin;
      int finalMin = hotProofMin % cycleMin;
      Serial.printf("[%s] HOTPROOF Initialization | cycleMin: %d | NoOfLoop: %d | finalMin: %d\n", 
                    getFormattedTime(remainingMin).c_str(), cycleMin, NoOfLoop, finalMin);
    }  } else if (currentState == HOTPROOF) {
    currentState = PROOF;
    if (proofHr == 0.0) {
      Serial.println("Skipping the Proof Process");
      moveToNextValidState();
    } else if (DebugLevel >= 1) {
      int totalProofMin = (int)(proofHr * 60.0 + 0.5); 
      Serial.printf("Starting Process: PROOF | cycleMin: %d | NoOfLoop: %d | finalMin: %d\n", totalProofMin, 1, totalProofMin);
    }
  } else if (currentState == PROOF) {
    currentState = BAKE;
    if (bakeMin == 0) {
      Serial.println("Skipping the Bake Process");
      moveToNextValidState();
    } else if (DebugLevel >= 1) {
      int cycleMin = 60;
      int NoOfLoop = bakeMin / cycleMin;
      int finalMin = bakeMin % cycleMin;
      Serial.printf("Starting Process: BAKE | cycleMin: %d | NoOfLoop: %d | finalMin: %d\n", cycleMin, NoOfLoop, finalMin);
    }
  } else if (currentState == BAKE) {
    longPress(RunResetPin);
    currentState = DONE;
    if (DebugLevel >= 1) {
      Serial.println("Sourdough is done");
    }
  }
}

void loop() {
  if (triggerRun) {
    triggerRun = false; 
    if (currentState == MENU_SELECTION) {
      saveSettingsToEEPROM();
      calculateRemainingTime();
      isPaused = false;

      // Log selected options at the beginning of the Sourdough process execution loop
      if (DebugLevel >= 1) {
        Serial.println("==================================================");
        Serial.println("[SOURDOUGH] Process Sequence Initiated.");
        Serial.printf("[SOURDOUGH] - Knead time   : %d min\n", kneadMin);
        Serial.printf("[SOURDOUGH] - Degas time   : %d min\n", degasMin);
        Serial.printf("[SOURDOUGH] - HotProof time: %.1f hr\n", hotProofHr);
        Serial.printf("[SOURDOUGH] - Proof time   : %.1f hr\n", proofHr);
        Serial.printf("[SOURDOUGH] - Bake time    : %d min\n", bakeMin);
        Serial.printf("[SOURDOUGH] - Bake Colour  : %s\n", getBakeColourName(bakeColour).c_str());
        Serial.printf("[SOURDOUGH] - Est Duration : %s\n", getFormattedTime(remainingMin).c_str());
        Serial.println("==================================================");
      }
      
      currentState = KNEADING;
      breadStateStep = 0;
      loopCounter = 0;
      stepActive = false;
      previousMillis = millis();

      if (kneadMin == 0) {
        Serial.println("Skipping the Knead Process");
        moveToNextValidState();
      } else if (DebugLevel >= 1) {
        int cycleMin = 30;
        int NoOfLoop = kneadMin / cycleMin;
        int finalMin = kneadMin % cycleMin;
        Serial.printf("Starting Process: KNEADING | cycleMin: %d | NoOfLoop: %d | finalMin: %d\n", cycleMin, NoOfLoop, finalMin);
      }
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
    
    // Remaining counts down and active spinning marker triggers
    if (remainingMin > 0) remainingMin--;
    spinnerIdx = (spinnerIdx + 1) % 4;

    if (DebugLevel >= 2) {
      if (currentState != lastLoggedState || breadStateStep != lastLoggedStep || loopCounter != lastLoggedLoop) {
        Serial.printf("[%s] State: %s | Step: %d | Loop: %d\n", getFormattedTime(remainingMin).c_str(), getStateName(currentState).c_str(), breadStateStep, loopCounter);
        lastLoggedState = currentState;
        lastLoggedStep = breadStateStep;
        lastLoggedLoop = loopCounter;
      }
    }
    
    if (remainingMin <= 0 && currentState != DONE) {
      longPress(RunResetPin);
      currentState = DONE;
      if (DebugLevel >= 1) {
        Serial.println("Sourdough is done");
      }
    }
  }

  // --- Sourdough Mechanical Phase Driver ---
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
        moveToNextValidState();
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
        moveToNextValidState();
      }
      break;
    }

    case HOTPROOF: {
      int cycleMin = 60;
      int hotProofMin = (int)(hotProofHr * 60.0 + 0.5);
      int NoOfLoop = hotProofMin / cycleMin;
      int finalMin = hotProofMin % cycleMin;

      if (!stepActive) {
        if (breadStateStep == 0) {
          if (loopCounter < NoOfLoop) {
            longPress(RunResetPin);
            shortPress(MenuPin, 9);
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
        moveToNextValidState();
      }
      break;
    }

    case PROOF: {
      int finalMin = (int)(proofHr * 60.0 + 0.5);

      if (!stepActive) {
        longPress(RunResetPin);
        stepTimer = millis();
        stepActive = true;
      } else {
        unsigned long targetDuration = testRun ? (finalMin * 1000UL) : (finalMin * 60000UL);
        if (millis() - stepTimer >= targetDuration) {
          moveToNextValidState();
        }
      }
      break;
    }

    case BAKE: {
      int cycleMin = 60;
      int NoOfLoop = bakeMin / cycleMin;
      int finalMin = bakeMin % cycleMin;

      if (!stepActive) {
        if (breadStateStep == 0) {
          if (loopCounter < NoOfLoop) {
            longPress(RunResetPin);
            executeBakeColourSequence(); 
            shortPress(MenuPin, 13);
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
        moveToNextValidState();
      }
      break;
    }
    default:
      break;
  }
}