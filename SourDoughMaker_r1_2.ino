// 1. Core Arduino and Network includes MUST go first
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <EEPROM.h> 

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

// --- Configuration Flags & Constants ---
const char* release_version = "1.4";
const int DebugLevel = 3; 

/*  
DonLim DL-T065-K Bread Machine
Solder a 10-pin female header on the circuit board for the following buttons , GND and 5V.
Pin 1 - ground Black 
Pin 2 - 5V Red
Pin 3 - RunReset button Orange
Pin 4 - Minus button Yellow
Pin 5 - Plus button Green 
Pin 6 - Weight button Blue
Pin 7 - Menu button Purple
Pin 8 - Colour button Grey
*/

// --- Stable Pin Assignments ---
const int RunResetPin = D1; // GPIO5
const int MinusPin    = D2; // GPIO4
const int DropPin     = D5; // GPIO14
const int MenuPin     = D6; // GPIO12
const int ColourPin   = D7; // GPIO13

// --- AP Default Fallback Configuration ---
const char* defaultPassword = "99819872"; 
String apSsid = "SourDough"; // Will dynamically append chip signature on initialization

// Dynamic Network Strings populated out of EEPROM storage
String clientSsid = "";
String clientPassword = "";

bool clientLocked = false;
IPAddress allowedClientIP;

AsyncWebServer server(80);

// --- Profile Instruction Struct ---
enum InstructionType { TYPE_COLOUR, TYPE_KNEAD, TYPE_DROP, TYPE_DEGAS, TYPE_PROOF, TYPE_REST, TYPE_BAKE };

struct ProfileInstruction {
  InstructionType type;
  String functionName;
  long durationSec;      // Used for time-based steps
  int colourOption;      // 0=Light, 1=Medium, 2=Dark
};

// Maximum lines of instructions supported
const int MAX_INSTRUCTIONS = 10;

// Dynamic Profile Variables
String breadName = "Pain de Campagne (France)";
ProfileInstruction instructions[MAX_INSTRUCTIONS];
int instructionCount = 0;

// UI State Flags
bool isConfirmed = false;
String syntaxErrorMsg = "";
String rawProfileInput = "";

// State Machine Engine States
enum EngineState { WIFI_CONFIG_AP, MENU_SELECTION, RUNNING_STEP, DONE };
EngineState currentState = MENU_SELECTION;

int currentStepIdx = 0; // Index of instruction currently being executed

enum BakeColourOption { LIGHT = 0, MEDIUM = 1, DARK = 2 };
BakeColourOption bakeColour = DARK;

// Operational Mode Enumerations
enum OpModeOption { MODE_NONE = 0, MODE_STANDALONE = 1, MODE_WIFI = 2 };
OpModeOption operationMode = MODE_WIFI; 

bool isPaused = false;
unsigned long previousMillis = 0;

// Dynamic Structural Tracking Variables
int breadStateStep = 0; 
unsigned long stepTimer = 0;
bool stepActive = false;
int loopCounter = 0; 
long currentStepRemainingSec = 0;

// Trackers to suppress duplicate periodic log messages
EngineState lastLoggedState = MENU_SELECTION;
int lastLoggedStep = -1;
int lastLoggedLoop = -1;

// Spinning indicator
const char spinner[] = {'/', '-', '\\', '|'};
int spinnerIdx = 0;

// --- EEPROM Layout Address Mapping ---
const int EEPROM_SIZE = 1024; 
const int ADDR_VALID_MARKER = 0; 
const int ADDR_OP_MODE      = 6;  
const int ADDR_WIFI_MARKER  = 7;  
const int ADDR_WIFI_SSID    = 8;   
const int ADDR_WIFI_PASS    = 40;  
const int ADDR_PROFILE_TEXT = 110; // Stores raw multi-line profile string up to 800 bytes

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

// --- Forward Declarations ---
String getFormattedTimeHMS(long totalSeconds);
String getPinName(int pin);
void shortPress(int pin, int numberOfTimes = 1);
void longPress(int pin);
void moveToNextInstruction();
bool parseAndValidateProfile(String input, String &outFormatted, String &outError);

// --- Time Formatter Helper ---
String getFormattedTimeHMS(long totalSeconds) {
  if (totalSeconds < 0) totalSeconds = 0;
  long hours = totalSeconds / 3600;
  long minutes = (totalSeconds % 3600) / 60;
  long seconds = totalSeconds % 60;
  char buf[32]; 
  sprintf(buf, "%02ld:%02ld:%02ld", hours, minutes, seconds);
  return String(buf);
}

// Calculate Total Sourdough Process Duration in seconds
long calculateTotalDurationSec() {
  long total = 0;
  for (int i = 0; i < instructionCount; i++) {
    if (instructions[i].type != TYPE_COLOUR && instructions[i].type != TYPE_DROP) {
      total += instructions[i].durationSec;
    }
  }
  return total;
}

long calculateTotalRemainingSec() {
  if (currentState == MENU_SELECTION) {
    return calculateTotalDurationSec();
  }
  long remaining = currentStepRemainingSec;
  for (int i = currentStepIdx + 1; i < instructionCount; i++) {
    if (instructions[i].type != TYPE_COLOUR && instructions[i].type != TYPE_DROP) {
      remaining += instructions[i].durationSec;
    }
  }
  return remaining;
}

String getPinName(int pin) {
  if (pin == MenuPin) return "MenuPin";
  if (pin == MinusPin) return "MinusPin";
  if (pin == RunResetPin) return "RunResetPin";
  if (pin == ColourPin) return "ColourPin";
  if (pin == DropPin) return "DropPin";
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
    case RUNNING_STEP:   return instructions[currentStepIdx].functionName;
    case DONE:           return "DONE";
  }
  return "UNKNOWN";
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

// --- Dynamic Pin Press Actions (Digital I/O Only) ---
void shortPress(int pin, int numberOfTimes) {
  if (DebugLevel >= 2) {
    Serial.print("["); Serial.print(getFormattedTimeHMS(calculateTotalRemainingSec())); 
    Serial.print("] ShortPress -> "); Serial.print(getPinName(pin));
    Serial.print(" "); Serial.println(numberOfTimes);
  }

  for (int i = 0; i < numberOfTimes; i++) {
    digitalWrite(pin, LOW);
    delay(200);
    digitalWrite(pin, HIGH);
    delay(300);
  }
}

void longPress(int pin) {
  if (DebugLevel >= 1) {
    Serial.print("["); 
    Serial.print(getFormattedTimeHMS(calculateTotalRemainingSec())); 
    Serial.print("] LongPress -> "); 
    Serial.println(getPinName(pin));
  }

  digitalWrite(pin, LOW);
  delay(3000);
  digitalWrite(pin, HIGH);
  delay(1000);
}

void executeBakeColourSequence() {
  if (DebugLevel >= 1) {
    Serial.print("["); Serial.print(getFormattedTimeHMS(calculateTotalRemainingSec())); 
    Serial.print("] Execution Routing -> BakeColour Process Initialized. Selection: ");
    Serial.println(getBakeColourName(bakeColour));
  }

  if (bakeColour == DARK) {
    shortPress(ColourPin, 1);
  } else if (bakeColour == LIGHT) {
    shortPress(ColourPin, 2);
  } else {
    if (DebugLevel >= 1) {
      Serial.print("["); Serial.print(getFormattedTimeHMS(calculateTotalRemainingSec()));
      Serial.println("] Execution Routing -> BakeColour is Medium, no adjustments required.");
    }
  }
}

// --- String Parsing and Profile Validation Helper ---
bool parseAndValidateProfile(String input, String &outFormatted, String &outError) {
  input.replace("\r\n", "\n");
  input.replace('\r', '\n');
  
  int lineCount = 0;
  String lines[15];
  int startIdx = 0;
  
  for (size_t i = 0; i <= input.length(); i++) {
    if (i == input.length() || input[i] == '\n') {
      String line = input.substring(startIdx, i);
      line.trim();
      if (line.length() > 0) {
        if (lineCount < 15) {
          lines[lineCount++] = line;
        }
      }
      startIdx = i + 1;
    }
  }

  if (lineCount == 0) {
    outError = "Syntax Error: Profile cannot be empty.";
    return false;
  }

  String tempBreadName = lines[0];
  if (tempBreadName.length() > 50) {
    tempBreadName = tempBreadName.substring(0, 50);
  }

  if (lineCount - 1 > MAX_INSTRUCTIONS) {
    outError = "Syntax Error: Exceeds maximum allowed 10 instruction lines.";
    return false;
  }

  ProfileInstruction tempInstructions[MAX_INSTRUCTIONS];
  int tempCount = 0;
  String formattedProfile = tempBreadName + "\n";

  for (int i = 1; i < lineCount; i++) {
    String line = lines[i];
    int spaceIdx = line.indexOf(' ');
    String func = (spaceIdx == -1) ? line : line.substring(0, spaceIdx);
    String arg = (spaceIdx == -1) ? "" : line.substring(spaceIdx + 1);
    func.trim();
    arg.trim();
    String funcUpper = func;
    funcUpper.toUpperCase();

    ProfileInstruction inst;
    
    if (funcUpper == "COLOUR" || funcUpper == "COLOR") {
      inst.type = TYPE_COLOUR;
      inst.functionName = "Colour";
      String argUpper = arg;
      argUpper.toUpperCase();
      if (argUpper == "LIGHT") inst.colourOption = 0;
      else if (argUpper == "MEDIUM") inst.colourOption = 1;
      else if (argUpper == "DARK") inst.colourOption = 2;
      else {
        outError = "Syntax Error in Line " + String(i + 1) + ": Colour option must be Light, Medium, or Dark.";
        return false;
      }
      formattedProfile += "Colour " + String(inst.colourOption == 0 ? "Light" : (inst.colourOption == 1 ? "Medium" : "Dark")) + "\n";
    } 
    else if (funcUpper == "DROP") {
      inst.type = TYPE_DROP;
      inst.functionName = "Drop";
      if (arg.length() > 0) {
        outError = "Syntax Error in Line " + String(i + 1) + ": Drop function must not have any arguments.";
        return false;
      }
      formattedProfile += "Drop\n";
    } 
    else if (funcUpper == "KNEAD" || funcUpper == "DEGAS" || funcUpper == "PROOF" || funcUpper == "REST" || funcUpper == "BAKE") {
      if (funcUpper == "KNEAD") inst.type = TYPE_KNEAD;
      else if (funcUpper == "DEGAS") inst.type = TYPE_DEGAS;
      else if (funcUpper == "PROOF") inst.type = TYPE_PROOF;
      else if (funcUpper == "REST") inst.type = TYPE_REST;
      else if (funcUpper == "BAKE") inst.type = TYPE_BAKE;
      
      inst.functionName = func;
      inst.functionName.toLowerCase();
      if (inst.functionName.length() > 0) {
        inst.functionName[0] = toupper(inst.functionName[0]);
      }

      int h, m, s;
      if (sscanf(arg.c_str(), "%d:%d:%d", &h, &m, &s) != 3 || h < 0 || m < 0 || m > 59 || s < 0 || s > 59) {
        outError = "Syntax Error in Line " + String(i + 1) + ": Invalid time format. Expected hh:mm:ss.";
        return false;
      }
      inst.durationSec = (long)h * 3600 + (long)m * 60 + s;
      formattedProfile += inst.functionName + " " + getFormattedTimeHMS(inst.durationSec) + "\n";
    } 
    else {
      outError = "Syntax Error in Line " + String(i + 1) + ": Unknown function '" + func + "'.";
      return false;
    }

    tempInstructions[tempCount++] = inst;
  }

  breadName = tempBreadName;
  instructionCount = tempCount;
  for (int i = 0; i < tempCount; i++) {
    instructions[i] = tempInstructions[i];
  }
  
  outFormatted = formattedProfile;
  outError = "";
  return true;
}

// --- EEPROM Storage Helpers ---
void saveSettingsToEEPROM() {
  EEPROM.write(ADDR_VALID_MARKER, VALID_CONFIG_MAGIC);
  
  for (int i = 0; i < 800; i++) {
    EEPROM.write(ADDR_PROFILE_TEXT + i, 0);
  }
  
  for (size_t i = 0; i < rawProfileInput.length() && i < 799; i++) {
    EEPROM.write(ADDR_PROFILE_TEXT + i, rawProfileInput[i]);
  }
  
  EEPROM.commit();
}

void loadSettingsFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t marker = EEPROM.read(ADDR_VALID_MARKER);
  
  if (marker == VALID_CONFIG_MAGIC) {
    String storedText = "";
    for (int i = 0; i < 800; i++) {
      char c = EEPROM.read(ADDR_PROFILE_TEXT + i);
      if (c == 0) break;
      storedText += c;
    }
    if (storedText.length() > 0) {
      String outF, outE;
      if (parseAndValidateProfile(storedText, outF, outE)) {
        rawProfileInput = outF;
        isConfirmed = true;
      }
    }
  } else {
    rawProfileInput = "Pain de Campagne (France)\nKnead 00:30:00\nDrop\nDegas 01:00:00\nProof 00:25:00\nRest 04:00:00\nColour Dark\nBake 01:30:00\n";
    String outF, outE;
    parseAndValidateProfile(rawProfileInput, outF, outE);
    rawProfileInput = outF;
    isConfirmed = true;
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
  Serial.printf("[SYSTEM] Connecting to WIFI router: %s\n", clientSsid.c_str()); 
  if (DebugLevel >= 3) {
    Serial.printf("[SYSTEM] WIFI Password: %s\n", clientPassword.c_str());   
  }
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt < 20000UL)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[SYSTEM] Failed to connect to WIFI router %s in 20 seconds. \n", clientSsid.c_str());
    Serial.printf("Restarting in Standalone mode. Double check WIFI set up. \n");

    EEPROM.write(ADDR_OP_MODE, (uint8_t)MODE_NONE);
    EEPROM.commit();
    delay(1000);
    ESP.restart();
    return false;
  }

  if (DebugLevel >= 1) {
    Serial.printf("[WIFI] Connected to SSID: %s | Local IP: %s\n", clientSsid.c_str(), WiFi.localIP().toString().c_str());
  }

  return true;
}

// Helper to send HTTP responses with explicit keep-alive headers
void sendHtmlResponse(AsyncWebServerRequest *request, const String &content) {
  AsyncWebServerResponse *response = request->beginResponse(200, "text/html", content);
  response->addHeader("Connection", "keep-alive");
  response->addHeader("Keep-Alive", "timeout=600, max=100");
  request->send(response);
}

// --- HTML Provisioning Pages ---
String generateWifiSetupHtml() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Billy Sourdough v" + String(release_version) + "</title>";
  html += "<style>body { font-family: Arial; text-align: center; margin-top: 50px; background-color: #f7f9fa; }";
  html += ".card { background: white; padding: 30px; max-width: 350px; margin: auto; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  html += "input[type=text], input[type=password], select { width: 90%; padding: 10px; margin: 10px 0; font-size: 14px; }";
  html += "input[type=submit] { background-color: #4CAF50; color: white; padding: 12px; border: none; border-radius: 4px; cursor: pointer; width: 96%; font-size: 16px; }</style>";
  html += "</head><body><div class='card'>";
  html += "<h2>Billy Sourdough v" + String(release_version) + "</h2>";
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
  html += "<title>Billy Sourdough v" + String(release_version) + "</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; background-color: #f7f9fa; margin:20px; }";
  html += ".card { background: white; padding: 20px; max-width: 450px; margin: auto; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  html += "textarea { width: 92%; height: 180px; font-family: monospace; font-size: 14px; padding: 8px; margin: 10px 0; border: 1px solid #ccc; border-radius: 4px; }";
  html += "button, input[type=submit] { padding: 10px 18px; font-size: 15px; margin: 6px; background-color: #4CAF50; color: white; border: none; cursor: pointer; border-radius: 4px; }";
  html += "button.edit { background-color: #2196F3; }";
  html += "button.pause { background-color: #ff9800; }";
  html += "button.reset { background-color: #f44336; }";
  html += "button.reset-confirm { background-color: #b71c1c; font-weight: bold; }";
  html += "button.erase-init { background-color: #757575; font-size: 13px; padding: 6px 12px; margin: 5px auto; display: block; }";
  html += "button.erase-confirm { background-color: #d32f2f; font-size: 13px; padding: 6px 12px; font-weight: bold; margin: 5px auto; display: none; }";
  html += "button.switch-init { background-color: #0288d1; font-size: 13px; padding: 6px 12px; margin: 5px auto; display: block; }";
  html += "button.switch-confirm { background-color: #01579b; font-size: 13px; padding: 6px 12px; font-weight: bold; margin: 5px auto; display: none; }";
  html += ".error-box { color: #d32f2f; font-weight: bold; background-color: #ffebee; padding: 8px; border-radius: 4px; margin: 10px 0; text-align: left; font-size: 13px; }";
  html += ".step-line { text-align: left; padding: 8px; margin: 4px 0; border-radius: 4px; font-family: monospace; font-size: 15px; }";
  html += ".active-step { background-color: #2196F3; color: white; font-weight: bold; }";
  html += ".pending-step { background-color: #f1f1f1; color: #333; }";
  html += "input.time-edit { width: 80px; padding: 4px; font-size: 14px; text-align: center; }";
  html += "</style>";
  
  html += "<script>";
  // Smart dynamic reload: suppressed if user is focused on an input/textarea element
  if (currentState == RUNNING_STEP || isConfirmed) {
    html += "setInterval(function() {";
    html += "  var activeTag = document.activeElement ? document.activeElement.tagName : '';";
    html += "  if (activeTag !== 'INPUT' && activeTag !== 'TEXTAREA') {";
    html += "    window.location.reload();";
    html += "  }";
    html += "}, 3000);";
  }
  html += "function exposeConfirmButton() { document.getElementById('eraseInitBtn').style.display = 'none'; document.getElementById('eraseConfirmBtn').style.display = 'block'; }";
  html += "function exposeWifiConfirmButton() { document.getElementById('switchWifiInitBtn').style.display = 'none'; document.getElementById('switchWifiConfirmBtn').style.display = 'block'; }";
  html += "function exposeStandaloneConfirmButton() { document.getElementById('switchStandaloneInitBtn').style.display = 'none'; document.getElementById('switchStandaloneConfirmBtn').style.display = 'block'; }";
  html += "function exposeRestartConfirmButton() { document.getElementById('restartInitBtn').style.display = 'none'; document.getElementById('restartConfirmBtn').style.display = 'inline-block'; }";
  html += "</script>";
  html += "</head><body><div class='card'>";
  
  html += "<h2>Billy Sourdough release " + String(release_version) + "</h2>";
  html += "<p style='margin: 8px 0; font-size:16px;'><strong>Total Time: </strong>" + getFormattedTimeHMS(calculateTotalRemainingSec());
  if (currentState != MENU_SELECTION && currentState != DONE && !isPaused) {
    html += " " + String(spinner[spinnerIdx]);
  }
  html += "</p><hr>";

  // State 1: MENU_SELECTION
  if (currentState == MENU_SELECTION) {
    if (syntaxErrorMsg.length() > 0) {
      html += "<div class='error-box'>" + syntaxErrorMsg + "</div>";
    }

    if (!isConfirmed) {
      // Unlocked Textbox
      html += "<form action='/confirm_profile' method='POST'>";
      html += "<textarea name='profile'>" + rawProfileInput + "</textarea><br>";
      html += "<input type='submit' value='Confirm'>";
      html += "</form>";
    } else {
      // Formatted locked profile display
      html += "<form action='/confirm_profile' method='POST'>";
      html += "<textarea name='profile' readonly style='background-color:#eef2f5;'>" + rawProfileInput + "</textarea><br>";
      html += "<button type='button' class='edit' onclick='location.href=\"/edit_profile\"'>Edit</button>";
      html += "<button type='button' onclick='location.href=\"/run\"'>Run</button>";
      html += "</form>";
    }
  } 
  // State 2: RUNNING_STEP or DONE
  else {
    html += "<h3>" + breadName + "</h3>";
    html += "<p style='text-align:left; font-weight:bold; margin-bottom:5px;'>Status: " + String(currentState == DONE ? "Sourdough Done" : (isPaused ? "Paused" : "Running")) + "</p>";
    
    for (int i = 0; i < instructionCount; i++) {
      if (i < currentStepIdx) {
        html += "<div class='step-line' style='color:#888; text-decoration:line-through;'>";
        html += instructions[i].functionName + " ";
        if (instructions[i].type == TYPE_COLOUR) html += (instructions[i].colourOption == 0 ? "Light" : (instructions[i].colourOption == 1 ? "Medium" : "Dark"));
        else if (instructions[i].type != TYPE_DROP) html += getFormattedTimeHMS(instructions[i].durationSec);
        html += "</div>";
      } 
      else if (i == currentStepIdx && currentState == RUNNING_STEP) {
        html += "<div class='step-line active-step'>";
        html += instructions[i].functionName + " ";
        if (instructions[i].type == TYPE_COLOUR) html += (instructions[i].colourOption == 0 ? "Light" : (instructions[i].colourOption == 1 ? "Medium" : "Dark"));
        else if (instructions[i].type == TYPE_DROP) html += "Executing...";
        else html += getFormattedTimeHMS(currentStepRemainingSec);
        html += "</div>";
      } 
      else {
        html += "<div class='step-line pending-step'>";
        if (instructions[i].type == TYPE_COLOUR) {
          html += instructions[i].functionName + " " + String(instructions[i].colourOption == 0 ? "Light" : (instructions[i].colourOption == 1 ? "Medium" : "Dark"));
        } else if (instructions[i].type == TYPE_DROP) {
          html += instructions[i].functionName;
        } else {
          html += "<form action='/update_step' method='GET' style='display:inline;'>";
          html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
          html += "<span>" + instructions[i].functionName + " </span>";
          html += "<input type='text' class='time-edit' name='time' value='" + getFormattedTimeHMS(instructions[i].durationSec) + "'> ";
          html += "<button type='submit' style='padding:3px 8px; font-size:12px;'>Update</button>";
          html += "</form>";
        }
        html += "</div>";
      }
    }

    html += "<br>";
    if (currentState != DONE) {
      html += "<button class='pause' onclick='location.href=\"/pause\"'>" + String(isPaused ? "Resume" : "Pause") + "</button>";
    }
  }

  html += "<button id='restartInitBtn' class='reset' onclick='exposeRestartConfirmButton()'>Restart</button>";
  html += "<button id='restartConfirmBtn' class='reset reset-confirm' style='display:none;' onclick='location.href=\"/reset\"'>Confirm to Restart</button>";

  if (currentState == MENU_SELECTION) {
    html += "<hr>";
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
  }

  html += "</div></body></html>";
  return html;
}

bool isAuthorizedClient(AsyncWebServerRequest *request) {
  IPAddress clientIP = request->client()->remoteIP();
  if (!clientLocked) {
    allowedClientIP = clientIP;
    clientLocked = true;
    
    if (DebugLevel >= 1) {
      Serial.printf("[CLIENT] User Device Connected. IP: %s\n", clientIP.toString().c_str());
    }
    return true;
  }
  return (clientIP == allowedClientIP);
}

void setup() {
  Serial.begin(74880);
  delay(200); 

  for (int i = 0; i < 5; i++) Serial.println("==================================================");
  Serial.printf("\n--- SYSTEM BOOT (Release %s) ---\n", release_version);
  
  pinMode(MenuPin, OUTPUT);
  pinMode(MinusPin, OUTPUT);
  pinMode(RunResetPin, OUTPUT);
  pinMode(ColourPin, OUTPUT);
  pinMode(DropPin, OUTPUT);
  
  digitalWrite(MenuPin, HIGH);
  digitalWrite(MinusPin, HIGH);
  digitalWrite(RunResetPin, HIGH);
  digitalWrite(ColourPin, HIGH); 
  digitalWrite(DropPin, HIGH);

  loadSettingsFromEEPROM();

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

  if (DebugLevel >= 1) {
    if (operationMode == MODE_WIFI && WiFi.status() == WL_CONNECTED) {
      Serial.printf("[SYSTEM] Connected Node IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.printf("[SYSTEM] Broadcast SSID: %s\n", apSsid.c_str());
      Serial.printf("[SYSTEM] AP IP Address: %s\n",  WiFi.softAPIP().toString().c_str());
      Serial.printf("[SYSTEM] Connected Node IP: %s\n", WiFi.localIP().toString().c_str());
    }
    
    Serial.printf("[SYSTEM] Active Wi-Fi Mode: %s\n", getOpModeName(operationMode).c_str());
  }

  // --- Web Routing Definitions ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState == WIFI_CONFIG_AP) {
      sendHtmlResponse(request, generateWifiSetupHtml());
    } else {
      if (!isAuthorizedClient(request)) {
        request->send(403, "text/plain", "Access Denied: Exclusive Session Active.");
        return;
      }
      sendHtmlResponse(request, generateHtml());
    }
  });

  server.on("/confirm_profile", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState == MENU_SELECTION) {
      if (request->hasParam("profile", true)) {
        String input = request->getParam("profile", true)->value();
        String outFormatted, outError;
        if (parseAndValidateProfile(input, outFormatted, outError)) {
          rawProfileInput = outFormatted;
          syntaxErrorMsg = "";
          isConfirmed = true;
        } else {
          rawProfileInput = input;
          syntaxErrorMsg = outError;
          isConfirmed = false;
        }
      }
    }
    request->redirect("/");
  });

  server.on("/edit_profile", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState == MENU_SELECTION) {
      isConfirmed = false;
    }
    request->redirect("/");
  });

  server.on("/update_step", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState == RUNNING_STEP) {
      if (request->hasParam("idx") && request->hasParam("time")) {
        int idx = request->getParam("idx")->value().toInt();
        String timeStr = request->getParam("time")->value();
        if (idx > currentStepIdx && idx < instructionCount) {
          int h, m, s;
          if (sscanf(timeStr.c_str(), "%d:%d:%d", &h, &m, &s) == 3 && h >= 0 && m >= 0 && m <= 59 && s >= 0 && s <= 59) {
            instructions[idx].durationSec = (long)h * 3600 + (long)m * 60 + s;
          }
        }
      }
    }
    request->redirect("/");
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

  server.on("/run", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState == MENU_SELECTION && isConfirmed) {
      triggerRun = true; 
    }
    request->redirect("/");
  });

  server.on("/pause", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAuthorizedClient(request)) return;
    if (currentState != MENU_SELECTION && currentState != DONE && currentState != WIFI_CONFIG_AP) {
      isPaused = !isPaused;
      
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
    request->send(200, "text/plain", "Device is restarting now...");
  });

  server.begin();
}

// Helper to advance state machine step-by-step
void moveToNextInstruction() {
  currentStepIdx++;
  breadStateStep = 0;
  loopCounter = 0;
  stepActive = false;

  if (currentStepIdx >= instructionCount) {
    longPress(RunResetPin);
    currentState = DONE;
    if (DebugLevel >= 1) {
      Serial.println("Sourdough is done");
    }
    return;
  }

  ProfileInstruction inst = instructions[currentStepIdx];
  currentStepRemainingSec = inst.durationSec;

  if (DebugLevel >= 1) {
    Serial.printf("Starting Instruction %d: %s\n", currentStepIdx + 1, inst.functionName.c_str());
  }
}

void loop() {
  if (triggerRun) {
    triggerRun = false; 
    if (currentState == MENU_SELECTION && isConfirmed) {
      saveSettingsToEEPROM();
      isPaused = false;

      if (DebugLevel >= 1) {
        Serial.println("==================================================");
        Serial.println("[SOURDOUGH] Process Sequence Initiated.");
        Serial.printf("[SOURDOUGH] Profile Name   : %s\n", breadName.c_str());
        Serial.printf("[SOURDOUGH] Total Steps    : %d\n", instructionCount);
        Serial.printf("[SOURDOUGH] Est Duration   : %s\n", getFormattedTimeHMS(calculateTotalDurationSec()).c_str());
        Serial.println("==================================================");
      }
      
      currentState = RUNNING_STEP;
      currentStepIdx = 0;
      breadStateStep = 0;
      loopCounter = 0;
      stepActive = false;
      previousMillis = millis();
      currentStepRemainingSec = instructions[0].durationSec;

      if (instructionCount == 0) {
        currentState = DONE;
      }
    }
  }

  if (triggerReset) {
    triggerReset = false;
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

  unsigned long currentInterval = 1000UL;
  unsigned long currentMillis = millis();
  
  if (currentMillis - previousMillis >= currentInterval) {
    previousMillis = currentMillis;
    
    if (currentStepRemainingSec > 0 && instructions[currentStepIdx].type != TYPE_COLOUR && instructions[currentStepIdx].type != TYPE_DROP) {
      currentStepRemainingSec--;
      if (currentStepRemainingSec < 0) currentStepRemainingSec = 0;
    }
    
    spinnerIdx = (spinnerIdx + 1) % 4;

    if (DebugLevel >= 2) {
      if (currentState != lastLoggedState || breadStateStep != lastLoggedStep || loopCounter != lastLoggedLoop) {
        Serial.printf("[%s] State: %s | Step: %d | Loop: %d\n", getFormattedTimeHMS(calculateTotalRemainingSec()).c_str(), getStateName(currentState).c_str(), breadStateStep, loopCounter);
        lastLoggedState = currentState;
        lastLoggedStep = breadStateStep;
        lastLoggedLoop = loopCounter;
      }
    }
  }

  // Dynamic Instruction Execution Router
  ProfileInstruction inst = instructions[currentStepIdx];

  switch (inst.type) {
    case TYPE_COLOUR: {
      bakeColour = (BakeColourOption)inst.colourOption;
      executeBakeColourSequence();
      moveToNextInstruction();
      break;
    }

    case TYPE_DROP: {
      shortPress(DropPin, 1);
      moveToNextInstruction();
      break;
    }

    case TYPE_KNEAD: {
      int kneadMin = inst.durationSec / 60;
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
          targetDuration = (cycleMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            loopCounter++;
          }
        } else if (breadStateStep == 1) {
          targetDuration = (finalMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            breadStateStep = 2;
          }
        }
      }
      if (breadStateStep == 2 || currentStepRemainingSec <= 0) {
        moveToNextInstruction();
      }
      break;
    }

    case TYPE_DEGAS: {
      int degasMin = inst.durationSec / 60;
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
          targetDuration = (cycleMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            loopCounter++;
          }
        } else if (breadStateStep == 1) {
          targetDuration = (finalMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            breadStateStep = 2;
          }
        }
      }
      if (breadStateStep == 2 || currentStepRemainingSec <= 0) {
        moveToNextInstruction();
      }
      break;
    }

    case TYPE_PROOF: {
      int ProofMin = inst.durationSec / 60;
      int cycleMin = 60;
      int NoOfLoop = ProofMin / cycleMin;
      int finalMin = ProofMin % cycleMin;

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
          targetDuration = (cycleMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            loopCounter++;
          }
        } else if (breadStateStep == 1) {
          targetDuration = (finalMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            breadStateStep = 2;
          }
        }
      }
      if (breadStateStep == 2 || currentStepRemainingSec <= 0) {
        moveToNextInstruction();
      }
      break;
    }

    case TYPE_REST: {
      int finalMin = inst.durationSec / 60;

      if (!stepActive) {
        longPress(RunResetPin);
        stepTimer = millis();
        stepActive = true;
      } else {
        unsigned long targetDuration = (finalMin * 60000UL);
        if (millis() - stepTimer >= targetDuration || currentStepRemainingSec <= 0) {
          moveToNextInstruction();
        }
      }
      break;
    }

    case TYPE_BAKE: {
      int bakeMin = inst.durationSec / 60;
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
          targetDuration = (cycleMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            loopCounter++;
          }
        } else if (breadStateStep == 1) {
          targetDuration = (finalMin * 60000UL);
          if (millis() - stepTimer >= targetDuration) {
            stepActive = false;
            breadStateStep = 2;
          }
        }
      }
      if (breadStateStep == 2 || currentStepRemainingSec <= 0) {
        moveToNextInstruction();
      }
      break;
    }
  }
}