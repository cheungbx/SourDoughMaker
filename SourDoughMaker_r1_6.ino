// 1. Core Arduino and Network includes MUST go first
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <EEPROM.h> 
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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
const char* release_version = "1.6";
const int DebugLevel = 2; 

// --- OLED Display Configurations ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
#define SCREEN_ADDRESS 0x3C // Standard I2C address for SSD1306 displays

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

/*  
DonLim DL-T065-K Bread Machine
Pin 1 - ground Black 
Pin 2 - 5V Red
Pin 3 - RunReset button 
Pin 4 - Minus button 
Pin 5 - Weight pin  
Pin 6 - Weight button
Pin 7 - Menu button 
Pin 8 - Colour button


Arduino Nano  10-pin Female header DonLim DL-T065-K Bread Machine
GND           pin 1         GND
VIN           pin 2         5V
D9            pin 3         RunReset button 
D8            pin 4         Minus button 
N.C.          pin 5         Plus button
D7            pin 6         Bake Colour button
D6            pin 7         Weight button 
D5            pin 8         Menu button 
D4            pin 9         N.C.
N.C.          pin 10        N.C.


ESP8266 / NodeMCU Pin Map:
A0  (--> D9)  D1 GPIO 5     RunReset 
A1  (--> D8)  D2 GPIO 4     Minus
A2  (--> D7)  D7 GPIO 13    Bake Colour    
A3  (--> D6)  D0 GPIO 16    Weight
A4  (--> D5)  D6 GPIO 12    Menu

Lolin Wemos D1 mini ESP8266 pin layout
--------------------------
Top View                            
--------------------------

RST                 TX
A0                  RX
16 weightPin        5 SCL
14 SDA              4 SDA
12 MenuPin          0
13 ColourPin        2
15                  GND
3.3V                VBUS
      [USB-Port]
--------------------------
Bottom View       
--------------------------

      [USB-Port]
3.3V                VBUS                    
15                  GND
13 ColourPin        2
12 MenuPin          0
14 SDA              4 SDA
16 weightPin        5 SCL
A0                  RX
RST                 TX
--------------------------
*/

// --- Stable Pin Assignments ---
const int weightPin   = D0; // GPIO16 
const int RunResetPin = D1; // GPIO5
const int MinusPin    = D2; // GPIO4
const int MenuPin     = D6; // GPIO12
const int ColourPin   = D7; // GPIO13

// --- AP Default Fallback Configuration ---
const char* defaultPassword = "99819872"; 
String apSsid = "SourDough"; 

// Dynamic Network Strings populated out of EEPROM storage
String clientSsid = "";
String clientPassword = "";

AsyncWebServer server(80);

// --- Profile Instruction Struct ---
enum InstructionType { TYPE_COLOUR, TYPE_KNEAD, TYPE_DEGAS, TYPE_PROOF, TYPE_REST, TYPE_BAKE, TYPE_WEIGHT, TYPE_MENU, TYPE_STOP };

struct ProfileInstruction {
  InstructionType type;
  String functionName;
  long durationSec;      // Used for time-based steps
  int colourOption;      // 0=Light, 1=Medium, 2=Dark
  int weightOptionValue; // 0=Light, 1=Heavy
  int menuNumber;        // 1 to 18
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

enum WeightOption { WEIGHT_LIGHT = 0, WEIGHT_HEAVY = 1 };
WeightOption weightOption = WEIGHT_LIGHT;

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

// Sub-step and Maximum Duration Tracking Variables
long currentSubStepSec = 0; 

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
const int ADDR_PROFILE_TEXT = 110; 

const uint8_t VALID_CONFIG_MAGIC = 0xAA; 
const uint8_t VALID_WIFI_MAGIC   = 0xBB;

// --- Asynchronous Web Event Action Flags ---
volatile bool triggerRun = false;
volatile bool triggerReset = false;
volatile bool triggerEraseAll = false;
volatile bool triggerSaveConfig = false;
volatile bool triggerSwitchMode = false; 
volatile bool triggerDisplayUpdate = false; // Flag to request safe OLED rendering in loop context
volatile bool triggerStop = false;

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
void startInstructionSequence(InstructionType type);
void endInstructionSequence(InstructionType type);
void updateOLEDDisplay();
void executeStopSequence();

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

long calculateTotalDurationSec() {
  long total = 0;
  for (int i = 0; i < instructionCount; i++) {
    if (instructions[i].type != TYPE_COLOUR && instructions[i].type != TYPE_WEIGHT && instructions[i].type != TYPE_MENU && instructions[i].type != TYPE_STOP) {
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
    if (instructions[i].type != TYPE_COLOUR && instructions[i].type != TYPE_WEIGHT && instructions[i].type != TYPE_MENU && instructions[i].type != TYPE_STOP) {
      remaining += instructions[i].durationSec;
    }
  }
  return remaining;
}

// Standard Header Printer
void printLogHeader() {
  Serial.print("[");
  Serial.print(getFormattedTimeHMS(calculateTotalRemainingSec()));
  Serial.print("] ");
}

String getPinName(int pin) {
  if (pin == MenuPin) return "MenuPin";
  if (pin == MinusPin) return "MinusPin";
  if (pin == RunResetPin) return "RunResetPin";
  if (pin == ColourPin) return "ColourPin";
  if (pin == weightPin) return "weightPin";
  return "UnknownPin";
}

String getBakeColourName(BakeColourOption opt) {
  if (opt == LIGHT) return "Light";
  if (opt == MEDIUM) return "Medium";
  return "Dark";
}

String getWeightOptionName(WeightOption opt) {
  if (opt == WEIGHT_LIGHT) return "Light";
  return "Heavy";
}

String getOpModeName(OpModeOption opt) {
  if (opt == MODE_STANDALONE) return "STANDALONE";
  if (opt == MODE_WIFI) return "WIFI";
  return "NONE / UNCONFIGURED";
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
    printLogHeader();
    Serial.print("ShortPress -> "); Serial.print(getPinName(pin));
    Serial.print(" "); Serial.println(numberOfTimes);
  }

  for (int i = 0; i < numberOfTimes; i++) {
    digitalWrite(pin, LOW);
    delay(150);
    digitalWrite(pin, HIGH);
    delay(150);
  }
}

void longPress(int pin) {
  if (DebugLevel >= 1) {
    printLogHeader();
    Serial.print("LongPress -> "); 
    Serial.println(getPinName(pin));
  }

  digitalWrite(pin, LOW);
  delay(2000);
  digitalWrite(pin, HIGH);
  delay(500);
}

void executeWeightSequence() {
  if (DebugLevel >= 1) {
    printLogHeader();
    Serial.print("Execution Routing -> Weight Command Initialized. Selection: ");
    Serial.println(getWeightOptionName(weightOption));
  }

  if (weightOption == WEIGHT_LIGHT) {
    shortPress(weightPin, 1);
  }
}

void executeBakeColourSequence() {
  if (DebugLevel >= 1) {
    printLogHeader();
    Serial.print("Execution Routing -> BakeColour Command Initialized. Selection: ");
    Serial.println(getBakeColourName(bakeColour));
  }

  if (bakeColour == DARK) {
    shortPress(ColourPin, 1);
  } else if (bakeColour == LIGHT) {
    shortPress(ColourPin, 2);
  }
}

void executeMenuSequence(int nn) {
  if (DebugLevel >= 1) {
    printLogHeader();
    Serial.print("Execution Routing -> Starting sequence for Menu command: ");
    Serial.println(nn);
  }

  shortPress(MenuPin, nn-1);
  executeWeightSequence();
  executeBakeColourSequence();
  shortPress(RunResetPin, 1);
}

void executeStopSequence() {
  if (DebugLevel >= 1) {
    printLogHeader();
    Serial.println("Execution Routing -> Starting sequence for Stop command:");
  }
  shortPress(RunResetPin, 1);
  longPress(RunResetPin);
}

// --- Centralized Starting and Ending Sequence Controllers ---
void startInstructionSequence(InstructionType type) {
  switch (type) {
    case TYPE_STOP:
      executeStopSequence();
      break;

    case TYPE_KNEAD:
      shortPress(RunResetPin, 1);
      longPress(RunResetPin);
      shortPress(MenuPin, 7);    
      shortPress(MinusPin, 10);  
      shortPress(RunResetPin);
      break;

    case TYPE_PROOF:
      shortPress(MenuPin, 9);    
      shortPress(MinusPin, 10);  
      shortPress(RunResetPin);
      break;

    case TYPE_BAKE:
      shortPress(MenuPin, 13);    
      executeBakeColourSequence();
      shortPress(MinusPin, 10);  
      shortPress(RunResetPin);
      break;

    case TYPE_DEGAS:
    default:
      break;
  }
}

void endInstructionSequence(InstructionType type) {
  switch (type) {
    case TYPE_KNEAD:
    case TYPE_PROOF:
    case TYPE_BAKE:
      longPress(RunResetPin);
      break;

    case TYPE_DEGAS:
      shortPress(RunResetPin, 1);
      longPress(RunResetPin);
      shortPress(MenuPin, 7);    
      shortPress(MinusPin, 9);  
      shortPress(RunResetPin);
      delay(10000); // turn the paddle for 10 seconds to simulate the folding action during degas
      longPress(RunResetPin);
      break;

    case TYPE_STOP:
    default:
      break;
  }
}

// Helper to determine max operational duration per cycle (in seconds)
long getMaxPeriodForType(InstructionType type) {
  switch (type) {
    case TYPE_KNEAD: return 1680;
    case TYPE_DEGAS: return 1680;
    case TYPE_PROOF: return 3480;
    case TYPE_BAKE:  return 3480;
    default:        return 0;
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
    else if (funcUpper == "WEIGHT") {
      inst.type = TYPE_WEIGHT;
      inst.functionName = "Weight";
      String argUpper = arg;
      argUpper.toUpperCase();
      if (argUpper == "LIGHT") inst.weightOptionValue = 0;
      else if (argUpper == "HEAVY") inst.weightOptionValue = 1;
      else {
        outError = "Syntax Error in Line " + String(i + 1) + ": Weight option must be Light or Heavy.";
        return false;
      }
      formattedProfile += "Weight " + String(inst.weightOptionValue == 0 ? "Light" : "Heavy") + "\n";
    }
    else if (funcUpper == "MENU") {
      inst.type = TYPE_MENU;
      inst.functionName = "Menu";
      int nn = arg.toInt();
      if (nn < 1 || nn > 18) {
        outError = "Syntax Error in Line " + String(i + 1) + ": Menu value must be between 1 and 18.";
        return false;
      }
      inst.menuNumber = nn;
      formattedProfile += "Menu " + String(nn) + "\n";
    }
    else if (funcUpper == "STOP") {
      inst.type = TYPE_STOP;
      inst.functionName = "Stop";
      inst.durationSec = 0;
      formattedProfile += "Stop\n";
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
      }
    }
  } else {
    rawProfileInput = "Pain de Campagne (France)\nKnead 00:30:00\nDegas 01:00:00\nProof 00:25:00\nRest 04:00:00\nColour Dark\nBake 01:30:00\n";
    String outF, outE;
    parseAndValidateProfile(rawProfileInput, outF, outE);
    rawProfileInput = outF;
  }
  isConfirmed = false;
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
  
  printLogHeader();
  Serial.printf("[WIFI] Standalone AP Started -> SSID: %s\n", apSsid.c_str());
  printLogHeader();
  Serial.printf("[WIFI] Device IP Address: %s\n", WiFi.softAPIP().toString().c_str());
}

bool executeWifiConnectionProcess() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(clientSsid.c_str(), clientPassword.c_str());
  
  printLogHeader();

  if (DebugLevel >= 1) {
    Serial.printf("[WIFI] Connecting to Router -> SSID: %s\n", clientSsid.c_str()); 
  }

  if (DebugLevel >= 3) {
      Serial.printf("[WIFI] Expected Password: %s\n", clientPassword.c_str());
  }
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt < 20000UL)) {
    delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) {
    printLogHeader();
    Serial.printf("[WIFI] Connection Failed -> Could not connect to SSID: %s\n", clientSsid.c_str());
    printLogHeader();
    Serial.println("[SYSTEM] Resetting mode selection and rebooting...");

    EEPROM.write(ADDR_OP_MODE, (uint8_t)MODE_NONE);
    EEPROM.commit();
    delay(1000);
    ESP.restart();
    return false;
  }

  printLogHeader();
  Serial.println("[WIFI] Status: CONNECTED");
  printLogHeader();
  Serial.printf("[WIFI] Device IP Address: %s\n", WiFi.localIP().toString().c_str());

  return true;
}

void sendHtmlResponse(AsyncWebServerRequest *request, const String &content) {
  AsyncWebServerResponse *response = request->beginResponse(200, "text/html", content);
  response->addHeader("Connection", "keep-alive");
  response->addHeader("Keep-Alive", "timeout=600, max=100");
  request->send(response);
}

// --- OLED Display Renderer Controller ---
void updateOLEDDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  // Line 1: Title and release version
  display.printf("Billy Sourdough %s\n", release_version);

  // Line 2: Horizontal separator
  display.drawLine(0, 10, 127, 9, SSD1306_WHITE);

  // Lines 3 & 4: Network Mode, SSID, and IP address
  display.setCursor(0, 16);
  if (operationMode == MODE_STANDALONE || currentState == WIFI_CONFIG_AP) {
    display.printf("Standalone %s\n", apSsid.c_str());
    display.setCursor(0, 26);
    display.println(WiFi.softAPIP().toString());
  } else {
    display.printf("WIFI %s\n", clientSsid.c_str());
    display.setCursor(0, 26);
    display.println(WiFi.localIP().toString());
  }

  // Line 6: Status and time tracking
  display.setCursor(0, 38);
  String statusStr = "";
  long displayTimeSec = 0;

  if (currentState == RUNNING_STEP) {
    statusStr = "Running";
    displayTimeSec = calculateTotalRemainingSec();
  } else if (currentState == DONE) {
    statusStr = "Done";
    displayTimeSec = calculateTotalDurationSec();
  } else {
    statusStr = isConfirmed ? "Confirm" : "Input";
    displayTimeSec = calculateTotalDurationSec();
  }

  display.printf("%s %s\n", statusStr.c_str(), getFormattedTimeHMS(displayTimeSec).c_str());

  // Line 7: Instruction breakdown during runtime execution
  if (currentState == RUNNING_STEP && instructionCount > 0 && currentStepIdx < instructionCount) {
    display.setCursor(0, 50);
    ProfileInstruction currentInst = instructions[currentStepIdx];

    if (currentInst.type == TYPE_COLOUR) {
      display.printf("%d/%d %s %s", currentStepIdx + 1, instructionCount, currentInst.functionName.c_str(),
                    (currentInst.colourOption == 0 ? "Light" : (currentInst.colourOption == 1 ? "Medium" : "Dark")));
    } else if (currentInst.type == TYPE_WEIGHT) {
      display.printf("%d/%d %s %s", currentStepIdx + 1, instructionCount, currentInst.functionName.c_str(),
                    (currentInst.weightOptionValue == 0 ? "Light" : "Heavy"));
    } else if (currentInst.type == TYPE_MENU) {
      display.printf("%d/%d Menu %d", currentStepIdx + 1, instructionCount, currentInst.menuNumber);
    } else if (currentInst.type == TYPE_STOP) {
      display.printf("%d/%d Stop", currentStepIdx + 1, instructionCount);
    } else {
      display.printf("%d/%d %s %s", currentStepIdx + 1, instructionCount, currentInst.functionName.c_str(), getFormattedTimeHMS(currentStepRemainingSec).c_str());
    }
  }

  display.display();
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
  html += "2Billy Sourdough v" + String(release_version) + "</h2>";
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
  html += "button.stop { background-color: #e53935; }";
  html += "button.stop-confirm { background-color: #b71c1c; font-weight: bold; }";
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
  html += "function exposeStopConfirmButton() { document.getElementById('stopInitBtn').style.display = 'none'; document.getElementById('stopConfirmBtn').style.display = 'inline-block'; }";
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
      html += "<form action='/confirm_profile' method='POST'>";
      html += "<textarea name='profile'>" + rawProfileInput + "</textarea><br>";
      html += "<input type='submit' value='Confirm'>";
      html += "</form>";

    } else {
      // 1. Formatted locked profile display
      html += "<form action='/confirm_profile' method='POST'>";
      html += "<textarea name='profile' readonly style='background-color:#eef2f5;'>" + rawProfileInput + "</textarea><br>";
      html += "</form>"; 

      // 2. Run Button (Line 1)
      html += "<div style='margin: 5px 0;'><button type='button' onclick='location.href=\"/run\"'>Run</button></div>";

      // 3. Edit Button (Line 2)
      html += "<div style='margin: 5px 0;'><button type='button' class='edit' onclick='location.href=\"/edit_profile\"'>Edit</button></div>";

      // 4. Separator 1 (Line 3)
      html += "<hr>";
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
        else if (instructions[i].type == TYPE_WEIGHT) html += (instructions[i].weightOptionValue == 0 ? "Light" : "Heavy");
        else if (instructions[i].type == TYPE_MENU) html += String(instructions[i].menuNumber);
        else if (instructions[i].type == TYPE_STOP) html += "";
        else html += getFormattedTimeHMS(instructions[i].durationSec);
        html += "</div>";
      } 
      else if (i == currentStepIdx && currentState == RUNNING_STEP) {
        html += "<div class='step-line active-step'>";
        html += instructions[i].functionName + " ";
        if (instructions[i].type == TYPE_COLOUR) html += (instructions[i].colourOption == 0 ? "Light" : (instructions[i].colourOption == 1 ? "Medium" : "Dark"));
        else if (instructions[i].type == TYPE_WEIGHT) html += (instructions[i].weightOptionValue == 0 ? "Light" : "Heavy");
        else if (instructions[i].type == TYPE_MENU) html += "Executing Menu " + String(instructions[i].menuNumber) + "...";
        else if (instructions[i].type == TYPE_STOP) html += "Executing Stop...";
        else html += getFormattedTimeHMS(currentStepRemainingSec);
        html += "</div>";
      } 
      else {
        html += "<div class='step-line pending-step'>";
        if (instructions[i].type == TYPE_COLOUR) {
          html += instructions[i].functionName + " " + String(instructions[i].colourOption == 0 ? "Light" : (instructions[i].colourOption == 1 ? "Medium" : "Dark"));
        } else if (instructions[i].type == TYPE_WEIGHT) {
          html += instructions[i].functionName + " " + String(instructions[i].weightOptionValue == 0 ? "Light" : "Heavy");
        } else if (instructions[i].type == TYPE_MENU) {
          html += instructions[i].functionName + " " + String(instructions[i].menuNumber);
        } else if (instructions[i].type == TYPE_STOP) {
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
      html += "<div style='margin: 5px 0;'><button class='pause' onclick='location.href=\"/pause\"'>" + String(isPaused ? "Resume" : "Pause") + "</button></div>";
      html += "<div style='margin: 5px 0;'>";
      html += "<button id='stopInitBtn' class='stop' onclick='exposeStopConfirmButton()'>Stop</button>";
      html += "<button id='stopConfirmBtn' class='stop stop-confirm' style='display:none;' onclick='location.href=\"/stop\"'>Confirm to Stop</button>";
      html += "</div>";
    }
  }

  html += "<div style='margin: 5px 0;'>";
  html += "<button id='restartInitBtn' class='reset' onclick='exposeRestartConfirmButton()'>Restart</button>";
  html += "<button id='restartConfirmBtn' class='reset reset-confirm' style='display:none;' onclick='location.href=\"/reset\"'>Confirm to Restart</button>";
  html += "</div>";

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

void setup() {
  Serial.begin(74880);
  delay(200); 

  // Initialize Standard Hardware Wire I2C using SDA (D4 / GPIO2) and SCL (D5 / GPIO14)
  Wire.begin(D4, D5);

  // Initialize SSD1306 Display immediately at start
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("[OLED] SSD1306 allocation failed"));
  } else {
    display.clearDisplay();
    display.display();
  }

  pinMode(MenuPin, OUTPUT);
  pinMode(MinusPin, OUTPUT);
  pinMode(RunResetPin, OUTPUT);
  pinMode(ColourPin, OUTPUT);
  pinMode(weightPin, OUTPUT);
  
  digitalWrite(MenuPin, HIGH);
  digitalWrite(MinusPin, HIGH);
  digitalWrite(RunResetPin, HIGH);
  digitalWrite(ColourPin, HIGH); 
  digitalWrite(weightPin, HIGH);

  loadSettingsFromEEPROM();
  computeDynamicAPProperties();
  loadWifiFromEEPROM();
  OpModeOption storedMode = loadOpModeFromEEPROM();

  // Render setup screen immediately at boot without waiting for Wi-Fi completion
  updateOLEDDisplay();

  printLogHeader();
  Serial.printf("PROGRAM START -> Billy Sourdough Release %s\n", release_version);

  printLogHeader();
  Serial.printf("[SYSTEM] Wi-Fi Operation Mode: %s\n", getOpModeName(storedMode).c_str());

  if (storedMode == MODE_STANDALONE) {
    operationMode = MODE_STANDALONE;
    printLogHeader();
    Serial.printf("[WIFI] Target Standalone SSID: %s\n", apSsid.c_str());
    if (DebugLevel >= 3) {
      Serial.printf("[WIFI] Expected Password: %s\n", defaultPassword);
    }
    executeStandaloneAPProcess();
    currentState = MENU_SELECTION; 
  } 
  else if (storedMode == MODE_WIFI) {
    operationMode = MODE_WIFI;
    printLogHeader();
     if (DebugLevel >= 1) {
      Serial.printf("[WIFI] Target Router SSID: %s\n", clientSsid.c_str());
     }
     if (DebugLevel >= 3) {
        Serial.printf("[WIFI] Password: %s\n", clientPassword.c_str());
     }
    if (executeWifiConnectionProcess()) {
      currentState = MENU_SELECTION; 
    }
  } 
  else {
    currentState = WIFI_CONFIG_AP;
    printLogHeader();
    if (DebugLevel >= 1) {
      Serial.printf("[WIFI] Provisioning Access Point Mode Active -> SSID: %s\n", apSsid.c_str());
    }
    if (DebugLevel >= 3) {
      Serial.printf("[WIFI] Password: %s\n", defaultPassword);
    }

    executeStandaloneAPProcess();
  }

  // Refresh OLED after network interface setup completes to display the IP Address
  updateOLEDDisplay();

  // --- Web Routing Definitions ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState == WIFI_CONFIG_AP) {
      sendHtmlResponse(request, generateWifiSetupHtml());
    } else {
      sendHtmlResponse(request, generateHtml());
    }
  });

  server.on("/confirm_profile", HTTP_POST, [](AsyncWebServerRequest *request){
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
    triggerDisplayUpdate = true;
    request->redirect("/");
  });

  server.on("/run", HTTP_ANY, [](AsyncWebServerRequest *request){
    if (currentState == MENU_SELECTION && isConfirmed) {
      triggerRun = true; 
    }
    request->redirect("/");
  });

  server.on("/edit_profile", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState == MENU_SELECTION) {
      isConfirmed = false;
    }
    triggerDisplayUpdate = true;
    request->redirect("/");
  });

  server.on("/pause", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState == RUNNING_STEP) {
      isPaused = !isPaused; 
    }
    triggerDisplayUpdate = true;
    request->redirect("/");     
  });

  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState == RUNNING_STEP) {
      triggerStop = true;
    }
    request->redirect("/");
  });

  server.on("/update_step", HTTP_GET, [](AsyncWebServerRequest *request){
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
    triggerDisplayUpdate = true;
    request->redirect("/");
  });

  server.on("/erase_all", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState != MENU_SELECTION) {
      request->send(403, "text/plain", "Action Denied: Process running.");
      return;
    }
    
    triggerEraseAll = true; 

    String redirectHtml = "<!DOCTYPE html><html><head>"
                          "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                          "<title>Erasing Settings...</title>"
                          "<style>body{font-family:Arial,sans-serif;text-align:center;padding-top:50px;background:#f7f9fa;}</style>"
                          "<script>"
                          "var seconds = 7;"
                          "function countdown() {"
                          "  document.getElementById('timer').innerText = seconds;"
                          "  if (seconds <= 0) {"
                          "    window.location.href = '/?nocache=' + new Date().getTime();"
                          "  } else {"
                          "    seconds--;"
                          "    setTimeout(countdown, 1000);"
                          "  }"
                          "}"
                          "window.onload = countdown;"
                          "</script></head><body>"
                          "<h2>Erasing Configurations...</h2>"
                          "<p>EEPROM wiped successfully. Restoring defaults and restarting...</p>"
                          "<p>Redirecting in <span id='timer'>7</span> seconds...</p>"
                          "</body></html>";

    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", redirectHtml);
    response->addHeader("Connection", "close");
    request->send(response);
  });

  server.on("/switch_mode", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState != MENU_SELECTION) {
      request->send(403, "text/plain", "Action Denied: Mode locked during execution.");
      return;
    }

    if (request->hasParam("to")) {
      String target = request->getParam("to")->value();
      if (target == "wifi" || target == "standalone") {
        targetSwitchMode = (target == "wifi") ? MODE_WIFI : MODE_STANDALONE;
        triggerSwitchMode = true;

        String redirectHtml = "<!DOCTYPE html><html><head>"
                              "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                              "<title>Switching Mode...</title>"
                              "<style>body{font-family:Arial,sans-serif;text-align:center;padding-top:50px;background:#f7f9fa;}</style>"
                              "<script>"
                              "var seconds = 7;"
                              "function countdown() {"
                              "  document.getElementById('timer').innerText = seconds;"
                              "  if (seconds <= 0) {"
                              "    window.location.href = '/?nocache=' + new Date().getTime();"
                              "  } else {"
                              "    seconds--;"
                              "    setTimeout(countdown, 1000);"
                              "  }"
                              "}"
                              "window.onload = countdown;"
                              "</script></head><body>"
                              "<h2>Switching Network Mode...</h2>"
                              "<p>The controller is switching to <b>" + target + "</b> mode and restarting.</p>"
                              "<p>Reconnecting in <span id='timer'>7</span> seconds...</p>"
                              "</body></html>";

        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", redirectHtml);
        response->addHeader("Connection", "close");
        request->send(response);
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

      String redirectHtml = "<!DOCTYPE html><html><head>"
                            "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                            "<title>Saving Settings...</title>"
                            "<style>body{font-family:Arial,sans-serif;text-align:center;padding-top:50px;background:#f7f9fa;}</style>"
                            "<script>"
                            "var seconds = 7;"
                            "function countdown() {"
                            "  document.getElementById('timer').innerText = seconds;"
                            "  if (seconds <= 0) {"
                            "    window.location.href = '/?nocache=' + new Date().getTime();"
                            "  } else {"
                            "    seconds--;"
                            "    setTimeout(countdown, 1000);"
                            "  }"
                            "}"
                            "window.onload = countdown;"
                            "</script></head><body>"
                            "<h2>Configurations Saved!</h2>"
                            "<p>Connecting to your Wi-Fi network and restarting...</p>"
                            "<p>Redirecting in <span id='timer'>7</span> seconds...</p>"
                            "<p><small><i>Note: If switching networks, make sure your phone is reconnected to the target network.</i></small></p>"
                            "</body></html>";

      AsyncWebServerResponse *response = request->beginResponse(200, "text/html", redirectHtml);
      response->addHeader("Connection", "close");
      request->send(response);
      return;
    }
    request->redirect("/");
  });

  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request){
    triggerReset = true; 
    String redirectHtml = "<!DOCTYPE html><html><head>"
                          "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                          "<title>Restarting...</title>"
                          "<style>body{font-family:Arial,sans-serif;text-align:center;padding-top:50px;background:#f7f9fa;}</style>"
                          "<script>"
                          "var seconds = 5;"
                          "function countdown() {"
                          "  document.getElementById('timer').innerText = seconds;"
                          "  if (seconds <= 0) {"
                          "    window.location.href = '/?nocache=' + new Date().getTime();"
                          "  } else {"
                          "    seconds--;"
                          "    setTimeout(countdown, 1000);"
                          "  }"
                          "}"
                          "window.onload = countdown;"
                          "</script></head><body>"
                          "<h2>Sourdough Controller Restarting...</h2>"
                          "<p>Reconnecting in <span id='timer'>5</span> seconds...</p>"
                          "</body></html>";

    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", redirectHtml);
    response->addHeader("Connection", "close");
    request->send(response);
  });

  server.begin();
}

// Helper to advance state machine step-by-step
void moveToNextInstruction() {
  currentStepIdx++;
  breadStateStep = 0;
  loopCounter = 0;
  stepActive = false;
  currentSubStepSec = 0;

  if (currentStepIdx >= instructionCount) {
    currentState = DONE;
    printLogHeader();
    Serial.println("[SOURDOUGH] Process Complete: Bread is Done.");
    triggerDisplayUpdate = true;
    return;
  }

  ProfileInstruction inst = instructions[currentStepIdx];
  currentStepRemainingSec = inst.durationSec;
  triggerDisplayUpdate = true;
}

void loop() {
  // Safe OLED Display Update inside the main loop context
  if (triggerDisplayUpdate) {
    triggerDisplayUpdate = false;
    updateOLEDDisplay();
  }

  if (triggerRun) {
    triggerRun = false; 
    if (currentState == MENU_SELECTION && isConfirmed) {
      saveSettingsToEEPROM();
      isPaused = false;
      
      currentState = RUNNING_STEP;
      currentStepIdx = 0;
      breadStateStep = 0;
      loopCounter = 0;
      stepActive = false;
      currentSubStepSec = 0;
      previousMillis = millis();
      currentStepRemainingSec = instructions[0].durationSec;

      printLogHeader();
      Serial.println("[SOURDOUGH] Process Sequence Initiated.");
      printLogHeader();
      Serial.printf("[SOURDOUGH] Profile Name : %s\n", breadName.c_str());
      printLogHeader();
      Serial.printf("[SOURDOUGH] Total Steps  : %d\n", instructionCount);
      printLogHeader();
      Serial.printf("[SOURDOUGH] Total Duration: %s\n", getFormattedTimeHMS(calculateTotalDurationSec()).c_str());

      if (instructionCount == 0) {
        currentState = DONE;
      }
      triggerDisplayUpdate = true;
    }
  }

  if (triggerStop) {
    triggerStop = false;
    if (currentState == RUNNING_STEP) {
      executeStopSequence();
      isPaused = false;
      isConfirmed = false;
      currentState = MENU_SELECTION;
      printLogHeader();
      Serial.println("[SOURDOUGH] Process Aborted via Stop command. Returning to Menu Input.");
      triggerDisplayUpdate = true;
    }
  }

  if (triggerReset) { 
    triggerReset = false; 
    printLogHeader();
    Serial.println("[SYSTEM] Device Restart Requested. Waiting to flush network buffer...");
    delay(2000); 
    ESP.restart(); 
  }

  if (triggerEraseAll) { 
    triggerEraseAll = false; 
    printLogHeader();
    Serial.println("[EEPROM] Erasing configuration and restarting...");
    delay(2000); 
    eraseAllEEPROM(); 
    ESP.restart(); 
  }
  
  if (triggerSwitchMode) { 
    triggerSwitchMode = false; 
    printLogHeader();
    Serial.printf("[SYSTEM] Switching Mode to %s and restarting...\n", getOpModeName(targetSwitchMode).c_str());
    delay(2000); 
    saveOpModeToEEPROM(targetSwitchMode); 
    ESP.restart(); 
  }
  
  if (triggerSaveConfig) { 
    triggerSaveConfig = false; 
    printLogHeader();
    Serial.println("[SYSTEM] Saving new configuration parameters and restarting...");
    delay(2000); 
    saveOpModeToEEPROM(pendingOpMode); 
    if (pendingOpMode == MODE_WIFI) saveWifiToEEPROM(pendingSsid, pendingPassword); 
    ESP.restart(); 
  }

  if (currentState == WIFI_CONFIG_AP || currentState == MENU_SELECTION || currentState == DONE || isPaused) {
    return; 
  }

  // --- Master 1-Second Tick ---
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= 1000UL) {
    previousMillis = currentMillis;
    
    if (currentStepRemainingSec > 0 && instructions[currentStepIdx].type != TYPE_COLOUR && instructions[currentStepIdx].type != TYPE_WEIGHT && instructions[currentStepIdx].type != TYPE_MENU && instructions[currentStepIdx].type != TYPE_STOP) {
      currentStepRemainingSec--;
      currentSubStepSec++;
    }
    
    spinnerIdx = (spinnerIdx + 1) % 4;

    // Trigger dynamic OLED refresh every second
    triggerDisplayUpdate = true;
  }

  // Dynamic Instruction Execution Router
  ProfileInstruction inst = instructions[currentStepIdx];

  switch (inst.type) {
    case TYPE_STOP: {
      printLogHeader();
      Serial.println("STARTING INSTRUCTION -> Function: Stop");
      executeStopSequence();
      moveToNextInstruction();
      break;
    }

    case TYPE_COLOUR: {
      printLogHeader();
      Serial.printf("STARTING INSTRUCTION %d/%d -> Function: Colour | Option: %s\n", currentStepIdx + 1, instructionCount, (inst.colourOption == 0 ? "Light" : (inst.colourOption == 1 ? "Medium" : "Dark")));
      bakeColour = (BakeColourOption)inst.colourOption;
      executeBakeColourSequence();
      moveToNextInstruction();
      break;
    }

    case TYPE_WEIGHT: {
      printLogHeader();
      Serial.printf("STARTING INSTRUCTION %d/%d -> Function: Weight | Option: %s\n", currentStepIdx + 1, instructionCount, (inst.weightOptionValue == 0 ? "Light" : "Heavy"));
      weightOption = (WeightOption)inst.weightOptionValue;
      moveToNextInstruction();
      break;
    }

    case TYPE_MENU: {
      printLogHeader();
      Serial.printf("STARTING INSTRUCTION %d/%d -> Function: Menu | Option: %d\n", currentStepIdx + 1, instructionCount, inst.menuNumber);
      executeMenuSequence(inst.menuNumber);
      moveToNextInstruction();
      break;
    }

    case TYPE_REST: {
      if (!stepActive) {
        printLogHeader();
        Serial.printf("STARTING INSTRUCTION %d/%d -> Function: %s | Duration: %s\n", currentStepIdx + 1, instructionCount, inst.functionName.c_str(), getFormattedTimeHMS(inst.durationSec).c_str());
        stepActive = true;
      }
      if (currentStepRemainingSec <= 0) {
        moveToNextInstruction();
      }
      break;
    }

    case TYPE_KNEAD:
    case TYPE_DEGAS:
    case TYPE_PROOF:
    case TYPE_BAKE: {
      long maxPeriod = getMaxPeriodForType(inst.type);

      if (!stepActive) {
        printLogHeader();
        Serial.printf("STARTING INSTRUCTION %d/%d -> Function: %s | Duration: %s\n", currentStepIdx + 1, instructionCount, inst.functionName.c_str(), getFormattedTimeHMS(inst.durationSec).c_str());
        startInstructionSequence(inst.type);
        stepActive = true;
        currentSubStepSec = 0;
      }

      if (currentStepRemainingSec > 0 && currentSubStepSec >= maxPeriod) {
        endInstructionSequence(inst.type);
        startInstructionSequence(inst.type);
        currentSubStepSec = 0;
      }

      if (currentStepRemainingSec <= 0) {
        if (currentSubStepSec < maxPeriod) {
          endInstructionSequence(inst.type);
        }
        moveToNextInstruction();
      }
      break;
    }
  }
}