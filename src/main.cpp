#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Arduino_SNMP_Manager.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <WiFiManager.h>

// SNMP settings
const char* community = "public"; // SNMP community string
IPAddress routerIP(192, 168, 1, 1); // Router IP address
const int snmpVersion = 1; // SNMP Version 2c (1 = v2c, 0 = v1)
WiFiUDP udp; // UDP object
SNMPManager snmp = SNMPManager(community); // Initialize SNMP
SNMPGet snmpRequest = SNMPGet(community, snmpVersion); // Initialize GET requests

// OIDs
const char* oidIfSpeedGauge = ".1.3.6.1.2.1.2.2.1.5.10"; // IF-MIB::ifSpeed.10
const char* oidInOctetsCount64 = ".1.3.6.1.2.1.31.1.1.1.6.10"; // IF-MIB::ifHCInOctets.10
const char* oidOutOctetsCount64 = ".1.3.6.1.2.1.31.1.1.1.10.10"; // IF-MIB::ifHCOutOctets.10
const char* oidUptime = ".1.3.6.1.2.1.1.3.0"; // SNMPv2-MIB::sysUpTime
const char* oidLoad1 = ".1.3.6.1.4.1.2021.10.1.3.1"; // UCD-SNMP-MIB::laLoad.1
const char* oidLoad5 = ".1.3.6.1.4.1.2021.10.1.3.2"; // UCD-SNMP-MIB::laLoad.2
const char* oidLoad15 = ".1.3.6.1.4.1.2021.10.1.3.3"; // UCD-SNMP-MIB::laLoad.3
const char* oidMemTotalReal = ".1.3.6.1.4.1.2021.4.5.0"; // UCD-SNMP-MIB::memTotalReal.0
const char* oidMemAvailReal = ".1.3.6.1.4.1.2021.4.6.0"; // UCD-SNMP-MIB::memAvailReal.0
const char* oidMemBuffer = ".1.3.6.1.4.1.2021.4.14.0"; // UCD-SNMP-MIB::memBuffer.0
const char* oidMemCached = ".1.3.6.1.4.1.2021.4.15.0"; // UCD-SNMP-MIB::memCached.0
const char* oidDskPercent = ".1.3.6.1.4.1.2021.9.1.9.1"; // UCD-SNMP-MIB::dskPercent.1

// Variables
unsigned int ifSpeedResponse = 0;
long long unsigned int inOctetsResponse = 0;
long long unsigned int outOctetsResponse = 0;
unsigned int uptime = 0;
unsigned int lastUptime = 0;
long long unsigned int lastInOctets = 0;
char* load1Str = nullptr; // 1-minute load string
char* load5Str = nullptr; // 5-minute load string
char* load15Str = nullptr; // 15-minute load string
int memTotalReal = 0; // Total RAM (KB)
int memAvailReal = 0; // Available RAM (KB)
int memBuffer = 0; // Buffered RAM (KB)
int memCached = 0; // Cached RAM (KB)
int dskPercent = 0; // Disk usage percentage
unsigned long pollStart = 0;
unsigned long intervalBetweenPolls = 0;

// SNMP callback objects
ValueCallback* callbackIfSpeed;
ValueCallback* callbackInOctets;
ValueCallback* callbackOutOctets;
ValueCallback* callbackUptime;
ValueCallback* callbackLoad1;
ValueCallback* callbackLoad5;
ValueCallback* callbackLoad15;
ValueCallback* callbackMemTotalReal;
ValueCallback* callbackMemAvailReal;
ValueCallback* callbackMemBuffer;
ValueCallback* callbackMemCached;
ValueCallback* callbackDskPercent;

// Initialize LCD (address 0x27, verify with I2C scanner)
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Settings
const int pollIntervals[] = {1000, 3000, 5000, 10000, 15000, 20000, 25000, 30000, 60000}; // Polling intervals in ms
const int numIntervals = 9; // Number of intervals
int currentIntervalIndex = 2; // Start with 5 seconds
unsigned long pollInterval = pollIntervals[currentIntervalIndex]; // Current polling interval
const int routerBootDelay = 45000; // Delay for router boot in ms (45 seconds)
const int buttonPin = 4; // Button pin (GPIO 4)
const unsigned long debounceDelay = 200; // Debounce delay in ms
const unsigned long menuDisplayDuration = 3000; // Menu display duration in ms (3 seconds)
volatile unsigned long lastButtonPress = 0; // Time of last button press
volatile bool buttonPressed = false; // Flag for button press
unsigned long menuDisplayStart = 0; // Time when menu display started
char prevFirstLine[21] = ""; // Previous first line
char prevLoadStr[21] = ""; // Previous load line
char prevRamDiskStr[21] = ""; // Previous RAM/Disk line
char prevUptimeStr[21] = ""; // Previous uptime line

// Function to format traffic in KB, MB, GB, TB with one decimal place, no trailing zeros
void formatTraffic(long long unsigned int bytes, char* buffer, int maxLen) {
  float value = bytes;
  const char* unit;
  
  if (value >= 1099511627776) { // TB (2^40)
    value /= 1099511627776;
    unit = "T";
  } else if (value >= 1073741824) { // GB (2^30)
    value /= 1073741824;
    unit = "G";
  } else if (value >= 1048576) { // MB (2^20)
    value /= 1048576;
    unit = "M";
  } else { // KB (2^10)
    value /= 1024;
    unit = "K";
  }
  
  snprintf(buffer, maxLen, "%.1f%s", value, unit);
  // Remove trailing zero if present (e.g., "123.0" -> "123")
  char* decimal = strchr(buffer, '.');
  if (decimal && strcmp(decimal, ".0") == 0) {
    decimal[0] = '\0';
  }
}

// Function to format and center a string
void formatCentered(const char* input, char* output, int maxLen, int lcdWidth = 20) {
  int len = strlen(input);
  if (len > lcdWidth) {
    strncpy(output, input, lcdWidth);
    output[lcdWidth] = '\0';
    return;
  }
  int padding = (lcdWidth - len) / 2;
  snprintf(output, maxLen, "%*s%s%*s", padding, "", input, lcdWidth - len - padding, "");
}

// Function to format and center the first line
void formatFirstLine(long long unsigned int inBytes, long long unsigned int outBytes, char* output, int maxLen) {
  char dnStr[10], upStr[10];
  formatTraffic(inBytes, dnStr, sizeof(dnStr));
  formatTraffic(outBytes, upStr, sizeof(upStr));
  
  // Form the string: DN:XXX.XU UP:XXX.XU
  char temp[21];
  snprintf(temp, sizeof(temp), "DN:%s UP:%s", dnStr, upStr);
  
  // Center the string
  formatCentered(temp, output, maxLen);
}

// Interrupt service routine for button press
void IRAM_ATTR buttonISR() {
  unsigned long currentTime = millis();
  if (currentTime - lastButtonPress > debounceDelay) {
    buttonPressed = true;
    lastButtonPress = currentTime;
  }
}

// Function to perform SNMP requests
void getSNMP() {
  snmpRequest.addOIDPointer(callbackIfSpeed);
  snmpRequest.addOIDPointer(callbackInOctets);
  snmpRequest.addOIDPointer(callbackOutOctets);
  snmpRequest.addOIDPointer(callbackUptime);
  snmpRequest.addOIDPointer(callbackLoad1);
  snmpRequest.addOIDPointer(callbackLoad5);
  snmpRequest.addOIDPointer(callbackLoad15);
  snmpRequest.addOIDPointer(callbackMemTotalReal);
  snmpRequest.addOIDPointer(callbackMemAvailReal);
  snmpRequest.addOIDPointer(callbackMemBuffer);
  snmpRequest.addOIDPointer(callbackMemCached);
  snmpRequest.addOIDPointer(callbackDskPercent);

  snmpRequest.setIP(WiFi.localIP());
  snmpRequest.setUDP(&udp);
  snmpRequest.setRequestID(random(5555));
  snmpRequest.sendTo(routerIP);
  snmpRequest.clearOIDList();
}

// Function to check data updates
void doSNMPCalculations() {
  if (uptime == lastUptime) {
    return; // Data not updated
  } else if (uptime < lastUptime) {
    // Router rebooted
    lastUptime = uptime;
    lastInOctets = inOctetsResponse;
    return;
  }
  // Update last values
  lastUptime = uptime;
  lastInOctets = inOctetsResponse;
}

// Function to display on LCD
void printToLCD() {
  // Check if menu is active
  if (millis() - menuDisplayStart < menuDisplayDuration) {
    lcd.clear();
    char pollStr[18];
    snprintf(pollStr, sizeof(pollStr), "Poll interval: %ds", pollInterval / 1000);
    char centeredPoll[21];
    formatCentered(pollStr, centeredPoll, sizeof(centeredPoll));
    lcd.setCursor(0, 0);
    lcd.print(centeredPoll);
    return;
  }

  // Parse strings to floats
  float load1 = (load1Str && load1Str[0] != '\0') ? atof(load1Str) : 0.0;
  float load5 = (load5Str && load5Str[0] != '\0') ? atof(load5Str) : 0.0;
  float load15 = (load15Str && load15Str[0] != '\0') ? atof(load15Str) : 0.0;

  // Calculate RAM usage percentage
  int usedMemory = (memTotalReal > 0 && memAvailReal >= 0 && memBuffer >= 0 && memCached >= 0) 
                   ? (memTotalReal - memAvailReal - memBuffer - memCached) : 0;
  unsigned int ramUsage = (memTotalReal > 0 && usedMemory >= 0) ? (usedMemory * 100 / memTotalReal) : 0;

  // First line: DN:XXX.XU UP:XXX.XU (centered)
  char firstLine[21];
  formatFirstLine(inOctetsResponse, outOctetsResponse, firstLine, sizeof(firstLine));
  if (strcmp(firstLine, prevFirstLine) != 0) {
    lcd.setCursor(0, 0);
    lcd.print(firstLine);
    strcpy(prevFirstLine, firstLine);
  }
  
  // Second line: System load (1, 5, 15 minutes)
  char loadStr[21];
  if (load1 == 0.0 && load5 == 0.0 && load15 == 0.0) {
    snprintf(loadStr, sizeof(loadStr), "Load: Err");
  } else {
    snprintf(loadStr, sizeof(loadStr), "Load: %.2f %.2f %.2f", load1, load5, load15);
  }
  if (strcmp(loadStr, prevLoadStr) != 0) {
    lcd.setCursor(0, 1);
    lcd.print(loadStr);
    strcpy(prevLoadStr, loadStr);
  }
  
  // Third line: RAM and Disk usage
  char ramDiskStr[21];
  if (memTotalReal <= 0 || memAvailReal < 0 || memBuffer < 0 || memCached < 0 || usedMemory < 0) {
    if (dskPercent < 0 || dskPercent > 100) {
      snprintf(ramDiskStr, sizeof(ramDiskStr), "RAM: Err Disk: Err");
    } else {
      snprintf(ramDiskStr, sizeof(ramDiskStr), "RAM: Err Disk: %d%%", dskPercent);
    }
  } else {
    if (dskPercent < 0 || dskPercent > 100) {
      snprintf(ramDiskStr, sizeof(ramDiskStr), "RAM: %u%% Disk: Err", ramUsage);
    } else {
      snprintf(ramDiskStr, sizeof(ramDiskStr), "RAM: %u%% Disk: %d%%", ramUsage, dskPercent);
    }
  }
  if (strcmp(ramDiskStr, prevRamDiskStr) != 0) {
    lcd.setCursor(0, 2);
    lcd.print(ramDiskStr);
    strcpy(prevRamDiskStr, ramDiskStr);
  }
  
  // Fourth line: Uptime
  unsigned long seconds = uptime / 100; // Convert hundredths of a second to seconds
  unsigned long days = seconds / (24 * 3600);
  seconds %= (24 * 3600);
  unsigned long hours = seconds / 3600;
  seconds %= 3600;
  unsigned long minutes = seconds / 60;
  seconds %= 60;
  char uptimeStr[16];
  snprintf(uptimeStr, sizeof(uptimeStr), "Up: %u:%02lu:%02lu:%02lu", days, hours, minutes, seconds);
  char centeredUptime[21];
  formatCentered(uptimeStr, centeredUptime, sizeof(centeredUptime));
  if (strcmp(centeredUptime, prevUptimeStr) != 0) {
    lcd.setCursor(0, 3);
    lcd.print(centeredUptime);
    strcpy(prevUptimeStr, centeredUptime);
  }
}

void setup() {
  // Initialize serial port
  Serial.begin(115200);

  // Initialize button pin
  pinMode(buttonPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(buttonPin), buttonISR, CHANGE);

  // Initialize I2C and LCD
  Wire.begin(21, 22); // SDA = GPIO21, SCL = GPIO22 (default ESP32 I2C pins)
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(F("Wait for router boot"));
  
  // Display countdown for router boot
  unsigned long startTime = millis();
  int secondsLeft = routerBootDelay / 1000; // 45 seconds
  while (secondsLeft >= 0) {
    char timeStr[15];
    snprintf(timeStr, sizeof(timeStr), "Time left: %ds", secondsLeft);
    char centeredTime[21];
    formatCentered(timeStr, centeredTime, sizeof(centeredTime));
    lcd.setCursor(0, 1);
    lcd.print(centeredTime);
    
    // Wait for approximately 1 second
    while (millis() - startTime < (routerBootDelay - secondsLeft * 1000)) {
      delay(10);
    }
    secondsLeft--;
  }

  // Clear LCD and show Wi-Fi connection message
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Connecting to WiFi..."));

  // Initialize WiFiManager
  WiFiManager wifiManager;
  wifiManager.autoConnect("ESP32_AP"); // Creates AP if no saved credentials
  // If autoConnect returns, WiFi is connected

  Serial.println(F("\nWiFi connected"));
  lcd.clear();
  lcd.print(F("WiFi connected"));

  // Allocate memory for load strings
  load1Str = (char*)malloc(8);
  load5Str = (char*)malloc(8);
  load15Str = (char*)malloc(8);
  if (!load1Str || !load5Str || !load15Str) {
    while (1); // Halt on failure
  }
  strcpy(load1Str, "0.0");
  strcpy(load5Str, "0.0");
  strcpy(load15Str, "0.0");

  // Initialize SNMP
  snmp.setUDP(&udp);
  snmp.begin();

  // Set up callback objects
  callbackIfSpeed = snmp.addGaugeHandler(routerIP, oidIfSpeedGauge, &ifSpeedResponse);
  callbackInOctets = snmp.addCounter64Handler(routerIP, oidInOctetsCount64, &inOctetsResponse);
  callbackOutOctets = snmp.addCounter64Handler(routerIP, oidOutOctetsCount64, &outOctetsResponse);
  callbackUptime = snmp.addTimestampHandler(routerIP, oidUptime, &uptime);
  callbackLoad1 = snmp.addStringHandler(routerIP, oidLoad1, &load1Str);
  callbackLoad5 = snmp.addStringHandler(routerIP, oidLoad5, &load5Str);
  callbackLoad15 = snmp.addStringHandler(routerIP, oidLoad15, &load15Str);
  callbackMemTotalReal = snmp.addIntegerHandler(routerIP, oidMemTotalReal, &memTotalReal);
  callbackMemAvailReal = snmp.addIntegerHandler(routerIP, oidMemAvailReal, &memAvailReal);
  callbackMemBuffer = snmp.addIntegerHandler(routerIP, oidMemBuffer, &memBuffer);
  callbackMemCached = snmp.addIntegerHandler(routerIP, oidMemCached, &memCached);
  callbackDskPercent = snmp.addIntegerHandler(routerIP, oidDskPercent, &dskPercent);

  delay(1000);
  lcd.clear();
}

void loop() {
  snmp.loop();
  delay(200); // Allow SNMP processing

  // Handle button press
  if (buttonPressed) {
    if (digitalRead(buttonPin) == LOW) { // Button is pressed (active LOW)
      currentIntervalIndex = (currentIntervalIndex + 1) % numIntervals;
      pollInterval = pollIntervals[currentIntervalIndex];
      menuDisplayStart = millis();
    }
    buttonPressed = false;
  }

  intervalBetweenPolls = millis() - pollStart;
  if (intervalBetweenPolls >= pollInterval) {
    pollStart += pollInterval; // Prevent delay drift
    getSNMP();
    doSNMPCalculations();
    printToLCD();
  }
}