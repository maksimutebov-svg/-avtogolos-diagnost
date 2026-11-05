ESP32
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <SoftwareSerial.h>

// ===== ПОРТЫ =====
HardwareSerial SerialELM(2);     // UART2 для HC-05 (ELM327): RX=16, TX=17
HardwareSerial SerialPlayer(1);  // UART1 для Плеера: RX=19, TX=18
SoftwareSerial SerialArduino(25, 26); // Связь с Arduino

DFRobotDFPlayerMini myDFPlayer;
bool isSystemStarted = false;

// ===== ПЕРЕМЕННЫЕ ДАННЫХ =====
float rpm = 0, tempCoolant = 0, speed = 0, load = 0;
float tempIntake = 0, throttle = 0, volt = 12.0;

// ===== ТРЕНДЫ =====
const int WINDOW_SIZE = 10;
float tempHistory[WINDOW_SIZE] = {0};
float rpmHistory[WINDOW_SIZE]  = {0};
float voltHistory[WINDOW_SIZE] = {0};
float dtHistory[WINDOW_SIZE]   = {0};   
int historyIndex = 0;
bool historyFilled = false;
unsigned long lastMeasMs = 0;

// ===== ФЛАГИ =====
bool coldPlayed = false, warmPlayed = false, normalPlayed = false, overheatPlayed = false;
bool lowVoltPlayed = false, highRpmPlayed = false;
bool oilPressurePlayed = false;
bool engineWasHighRpm = false;

// ===== ОЧЕРЕДЬ ОШИБОК (DTC) =====
String lastErrorCodes = "";
int dtcQueue[6];           
int dtcQueueCount = 0;     
int dtcPlayIndex = 0;      
unsigned long lastDtcPlayTime = 0; 

// ===== ТАЙМЕРЫ =====
unsigned long lastVoiceTime = 0;
const unsigned long VOICE_PAUSE = 5000;
unsigned long lastLoopTime = 0;
const unsigned long LOOP_INTERVAL = 2000;
unsigned long lastDTCCheck = 0; 
const unsigned long DTC_INTERVAL = 15000; 

void setup() {
  Serial.begin(115200);
  Serial.println("=== ESP32 OBD System Start ===");
    
  // ЗАДЕРЖКА 20 СЕКУНД ДЛЯ СОПРЯЖЕНИЯ HC-05 И ELM327
  delay(20000); 
  
  SerialELM.begin(38400, SERIAL_8N1, 16, 17);
  SerialPlayer.begin(9600, SERIAL_8N1, 19, 18);
  SerialArduino.begin(9600);
  SerialArduino.setTimeout(50);

  if (myDFPlayer.begin(SerialPlayer)) {
    myDFPlayer.volume(30);
    Serial.println("DFPlayer: OK");
  } else {
    Serial.println("DFPlayer: ERROR");
  }

  Serial.println("Initializing ELM327...");
  
  // --- РЕТРАИ И ДИАГНОСТИКА ---
  bool ok = false;
  for (int a = 1; a <= 3 && !ok; a++) {
    ok = sendAndWaitELM("ATZ", "ELM", 5000);
    if (!ok) delay(1000);
  }
  delay(1500); 
  
  for (int a = 1; a <= 3; a++) {
    if (sendAndWaitELM("ATE0", "OK", 2000)) break;
    delay(500);
  }
  for (int a = 1; a <= 3; a++) {
    if (sendAndWaitELM("ATS0", "OK", 2000)) break;
    delay(500);
  }
  
  // ЖЕСТКАЯ УСТАНОВКА ПРОТОКОЛА (ATSP5)
  for (int a = 1; a <= 3; a++) {
    if (sendAndWaitELM("ATSP5", "OK", 3000)) break;
    delay(500);
  }

  delay(1000);
  lastLoopTime = millis();
  Serial.println("ESP32 готова. Жду START от Arduino...");
}

void loop() {
  // === 1. ОЖИДАНИЕ КОМАНДЫ START ===
  if (!isSystemStarted && SerialArduino.available()) {
    char cmd[16];
    SerialArduino.readBytesUntil('\n', cmd, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = 0;
    cmd[strcspn(cmd, "\r\n")] = 0;

    if (strcmp(cmd, "START") == 0) {
      isSystemStarted = true;
      Serial.println(">>> SYSTEM STARTED <<<");
      SerialArduino.println("ACK");
      
      myDFPlayer.play(1); 
      
      lastVoiceTime = millis(); 
      lastDTCCheck = millis(); 
    }
  }

  // === 2. НЕБЛОКИРУЮЩИЙ ЦИКЛ ОПРОСА ===
  if (isSystemStarted && (millis() - lastLoopTime >= LOOP_INTERVAL)) {
    lastLoopTime = millis();

    rpm          = readELM("010C", 0.25, 2);
    tempCoolant  = readELM("0105", 1.0, 1);  tempCoolant = tempCoolant - 40.0;
    speed        = readELM("010D", 1.0, 1);
    load         = readELM("0104", 0.392, 1);
    tempIntake   = readELM("010F", 1.0, 1);  tempIntake = tempIntake - 40.0;
    throttle     = readELM("0111", 0.392, 1);
    volt         = readVoltageELM();

    if (isnan(rpm)) rpm = 0;
    if (isnan(tempCoolant)) tempCoolant = 0;
    if (isnan(speed)) speed = 0;
    if (isnan(load)) load = 0;
    if (isnan(tempIntake)) tempIntake = 0;
    if (isnan(throttle)) throttle = 0;
    if (isnan(volt) || volt < 5.0) volt = 12.0;

    if (rpm > 0 && volt > 5.0) addMeasurement(tempCoolant, rpm, volt);
    
    float tempTrend = calculateTrend(tempHistory);
    float rpmTrend  = calculateTrend(rpmHistory);
    float voltTrend = calculateTrend(voltHistory);

    Serial.println("------------------------------------------");
    Serial.print("1. Обороты: ");     Serial.print(rpm);         Serial.println(" об/мин");
    Serial.print("2. Темп ОЖ: ");     Serial.print(tempCoolant); Serial.println(" °C");
    Serial.print("3. Скорость: ");    Serial.print(speed);       Serial.println(" км/ч");
    Serial.print("4. Нагрузка: ");    Serial.print(load);        Serial.println(" %");
    Serial.print("5. Темп впуска: "); Serial.print(tempIntake);  Serial.println(" °C");
    Serial.print("6. Дроссель: ");    Serial.print(throttle);    Serial.println(" %");
    Serial.print("7. Напряжение: ");  Serial.print(volt);        Serial.println(" В");
    Serial.print("Тренд темп: ");     Serial.print(tempTrend);   Serial.println(" °C/с");
    Serial.print("Тренд об: ");       Serial.print(rpmTrend);    Serial.println(" об/мин/с");
    Serial.print("Тренд вольт: ");    Serial.print(voltTrend);   Serial.println(" В/с");

    SerialArduino.print(rpm, 0);         SerialArduino.print(",");
    SerialArduino.print(tempCoolant, 1); SerialArduino.print(",");
    SerialArduino.print(speed, 0);       SerialArduino.print(",");
    SerialArduino.print(load, 0);        SerialArduino.print(",");
    SerialArduino.print(tempIntake, 1);  SerialArduino.print(",");
    SerialArduino.print(throttle, 0);    SerialArduino.print(",");
    SerialArduino.print(volt, 1);        SerialArduino.print(",");
    SerialArduino.print(tempTrend, 2);   SerialArduino.print(",");
    SerialArduino.print(rpmTrend, 0);    SerialArduino.print(",");
    SerialArduino.println(voltTrend, 2);

    int playerState = myDFPlayer.readState();
    
    if (dtcPlayIndex < dtcQueueCount) {
      if (playerState != 513 || (millis() - lastDtcPlayTime > 6000)) {
        int trackToPlay = dtcQueue[dtcPlayIndex];
        myDFPlayer.play(trackToPlay); 
        lastDtcPlayTime = millis();
        dtcPlayIndex++;
      }
    } 
    else if (playerState != 513 && playerState != 514) {
      handleVoiceLogic();
    }

    if (millis() - lastDTCCheck > DTC_INTERVAL) {
      lastDTCCheck = millis();
      readDTCs();
    }
  }
}

// ============================================================
//  ЧТЕНИЕ ОШИБОК (DTC)
// ============================================================
void readDTCs() {
  while(SerialELM.available()) SerialELM.read();
  
  delay(100); 

  SerialELM.println("03"); 
  unsigned long start = millis();
  char rawResp[128] = {0};
  int i = 0;

  while (millis() - start < 3000) {
    while (SerialELM.available() && i < 127) {
      char c = SerialELM.read();
      if (c == '>') break;
      rawResp[i++] = c;
    }
    if (strchr(rawResp, '>')) break;
  }

  Serial.print("RAW DTC: ");
  Serial.println(rawResp);

  if (strstr(rawResp, "UNABLE TO CONNECT") != NULL) {
    Serial.println(">>> ДИАГНОСТИКА: НЕТ СВЯЗИ С ШИНОЙ <<<");
    return;
  }

  if (strstr(rawResp, "NO DATA") != NULL) {
    Serial.println(">>> ДИАГНОСТИКА: ОШИБОК НЕТ (NO DATA) <<<");
    lastErrorCodes = ""; dtcQueueCount = 0; dtcPlayIndex = 0;
    return;
  }

  char cleanResp[128] = {0};
  int j = 0;
  for (int k = 0; k < i; k++) {
    if ((rawResp[k] == '0' || rawResp[k] == '1' || rawResp[k] == '2') && rawResp[k+1] == ':') {
      k++; 
      continue;
    }
    if (isHex(rawResp[k])) {
      cleanResp[j++] = rawResp[k];
    }
  }

  char *pos = strstr(cleanResp, "43");
  if (pos == NULL) return;

  pos += 2; 
  if (isHex(*pos) && isHex(*(pos+1))) {
    pos += 2; 
  }

  int errorCount = 0;
  String currentCodes = "";
  
  while (*pos && errorCount < 3) {
    if (!isHex(*pos)) { pos++; continue; } 
    
    char dtcHex[5] = {0};
    int k = 0;
    while (k < 4 && *pos && isHex(*pos)) {
      dtcHex[k++] = *pos++;
    }
    if (k < 4) break;

    int hi = (hexToInt(dtcHex[0]) << 4) | hexToInt(dtcHex[1]);
    int lo = (hexToInt(dtcHex[2]) << 4) | hexToInt(dtcHex[3]);
    
    if (hi == 0 && lo == 0) break; 

    char typeChar = 'P';
    switch ((hi >> 6) & 0x03) {
      case 0: typeChar = 'P'; break;
      case 1: typeChar = 'C'; break;
      case 2: typeChar = 'B'; break;
      case 3: typeChar = 'U'; break;
    }
    int digit1 = (hi >> 4) & 0x03;
    int digit2 = hi & 0x0F;

    char codeString[8];
    sprintf(codeString, "%c%d%X%X%X", typeChar, digit1, digit2, lo >> 4, lo & 0x0F);
    
    currentCodes += codeString;
    currentCodes += " ";
    errorCount++;
  }
  
  currentCodes.trim();

  if (currentCodes.length() > 0) {
    Serial.println("==========================================");
    Serial.println(">>> ВНИМАНИЕ: В МАШИНЕ ОБНАРУЖЕНЫ ОШИБКИ! <<<");
    
    int startIdx = 0;
    while (startIdx < currentCodes.length()) {
      int spaceIdx = currentCodes.indexOf(' ', startIdx);
      if (spaceIdx == -1) spaceIdx = currentCodes.length();
      
      String singleCode = currentCodes.substring(startIdx, spaceIdx);
      singleCode.trim();
      
      if (singleCode.length() > 0) {
        Serial.print("- " + singleCode + ": ");
        if (singleCode == "P0118") Serial.println("Ошибка датчика температуры охлаждающей жидкости.");
        else if (singleCode == "P0120") Serial.println("Ошибка датчика дроссельной заслонки.");
        else if (singleCode == "P0335") Serial.println("Ошибка датчика коленчатого вала.");
        else if (singleCode == "P0562") Serial.println("Низкое напряжение бортовой сети.");
        else if (singleCode == "P0300") Serial.println("Пропуски зажигания в цилиндрах.");
        else if (singleCode == "P0420") Serial.println("Неисправность катализатора.");
        else Serial.println("Неизвестная ошибка (нет описания).");
      }
      startIdx = spaceIdx + 1;
    }
    Serial.println("==========================================");

    SerialArduino.print("ERROR: ");
    SerialArduino.println(currentCodes);

    if (currentCodes != lastErrorCodes) {
      lastErrorCodes = currentCodes;
      dtcQueueCount = 0;
      dtcPlayIndex = 0;
      
      dtcQueue[dtcQueueCount++] = 9; 
      if (currentCodes.indexOf("P0118") != -1 && dtcQueueCount < 6) dtcQueue[dtcQueueCount++] = 10;
      if (currentCodes.indexOf("P0120") != -1 && dtcQueueCount < 6) dtcQueue[dtcQueueCount++] = 11;
      if (currentCodes.indexOf("P0335") != -1 && dtcQueueCount < 6) dtcQueue[dtcQueueCount++] = 12;
      if (currentCodes.indexOf("P0562") != -1 && dtcQueueCount < 6) dtcQueue[dtcQueueCount++] = 13;
      if (currentCodes.indexOf("P0300") != -1 && dtcQueueCount < 6) dtcQueue[dtcQueueCount++] = 14;
      if (currentCodes.indexOf("P0420") != -1 && dtcQueueCount < 6) dtcQueue[dtcQueueCount++] = 15;
      
      Serial.println("Очередь озвучки сформирована.");
    }
  } else {
    Serial.println(">>> ДИАГНОСТИКА: ОШИБОК НЕТ <<<");
    lastErrorCodes = ""; 
    dtcQueueCount = 0; 
    dtcPlayIndex = 0;
  }
}

// ============================================================
//  ЛОГИКА ГОЛОСОВЫХ
// ============================================================
void handleVoiceLogic() {
  unsigned long now = millis();
  if (now - lastVoiceTime < VOICE_PAUSE) return;

  if (tempCoolant >= 105.0 && !overheatPlayed) {
    myDFPlayer.play(8); overheatPlayed = true; lastVoiceTime = now; return;
  }

  float oilPressure = 400.0 - (load * 4.5);
  if (rpm > 65 && oilPressure < 20.0 && !oilPressurePlayed) {
    myDFPlayer.play(6); oilPressurePlayed = true; lastVoiceTime = now; return;
  }

  if (volt > 0 && volt < 11.5 && !lowVoltPlayed) {
    myDFPlayer.play(7); lowVoltPlayed = true; lastVoiceTime = now; return;
  }

  if (rpm > 6000.0 && !highRpmPlayed) {
    myDFPlayer.play(5); highRpmPlayed = true; lastVoiceTime = now; return;
  }

  if ((tempCoolant > -45.0 && tempCoolant < 40.0) && !coldPlayed) {
    myDFPlayer.play(2); coldPlayed = true; warmPlayed = false; normalPlayed = false; lastVoiceTime = now; return;
  }

  if (tempCoolant >= 40.0 && tempCoolant < 110.0 && rpm >= 600.0 && rpm <= 1000.0 && volt >= 12.0 && !normalPlayed) {
    myDFPlayer.play(3); normalPlayed = true; warmPlayed = false; lastVoiceTime = now; return;
  }

  if (rpm > 1300) engineWasHighRpm = true;
  if (engineWasHighRpm && rpm <= 800 && rpm >= 600 && !warmPlayed) {
    myDFPlayer.play(4); warmPlayed = true; engineWasHighRpm = false; lastVoiceTime = now; return;
  }

  if (tempCoolant <= 95.0) overheatPlayed = false;
  if (oilPressure >= 30.0) oilPressurePlayed = false;
  if (volt >= 12.5) lowVoltPlayed = false;
  if (rpm < 5500.0) highRpmPlayed = false;
  if (tempCoolant >= 40.0) coldPlayed = false;
  if (rpm < 600 || rpm >= 1300) normalPlayed = false;
}

// ============================================================
//  ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ============================================================
bool isHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int hexToInt(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

void addMeasurement(float t, float r, float v) {
  unsigned long now = millis();
  float dt = (lastMeasMs == 0) ? 2.0 : (now - lastMeasMs) / 1000.0;
  if (dt > 30.0) dt = 2.0;   
  lastMeasMs = now;

  dtHistory[historyIndex]   = dt;
  tempHistory[historyIndex] = t;
  rpmHistory[historyIndex]  = r;
  voltHistory[historyIndex] = v;

  historyIndex = (historyIndex + 1) % WINDOW_SIZE;
  if (historyIndex == 0) historyFilled = true;
}

float calculateTrend(float* history) {
  if (!historyFilled) return 0.0;
  int lastIndex  = (historyIndex + WINDOW_SIZE - 1) % WINDOW_SIZE;
  int firstIndex = historyIndex;
  float delta = history[lastIndex] - history[firstIndex];

  float totalTime = 0.0;
  for (int k = 0; k < WINDOW_SIZE; k++) totalTime += dtHistory[k];
  if (totalTime <= 0.1) totalTime = 0.1;

  return delta / totalTime;
}

bool sendAndWaitELM(const char* cmd, const char* expected, int timeoutMs) {
  while (SerialELM.available()) SerialELM.read();
  
  SerialELM.println(cmd);
  unsigned long start = millis();
  String response = "";

  while (millis() - start < timeoutMs) {
    while (SerialELM.available()) {
      char c = SerialELM.read();
      if (c == '>') { 
        response.trim(); 
        Serial.print("[ELM] ");
        Serial.print(cmd);
        Serial.print(" -> ");
        Serial.println(response);
        
        if (expected == NULL) return true;
        if (response.indexOf(expected) != -1) return true;
        return false;
      }
      response += c;
    }
  }
  
  Serial.print("[ELM] ");
  Serial.print(cmd);
  Serial.println(" -> НЕТ ОТВЕТА (ТАЙМАУТ)");
  return false;
}

float readELM(const char* pid, float multiplier, int dataBytes) {
  while(SerialELM.available()) SerialELM.read();
  
  SerialELM.println(pid);
  unsigned long start = millis();
  char response[64] = {0};
  int i = 0;
  while (millis() - start < 1000) {
    while (SerialELM.available() && i < 63) {
      response[i++] = SerialELM.read();
    }
    if (strchr(response, '>')) break;
  }
  if (strstr(response, "NO DATA") != NULL) return NAN;

  char cleanResp[64] = {0};
  int j = 0;
  for (int k = 0; k < i; k++) {
    if (response[k] != ' ' && response[k] != '\r' && response[k] != '\n') {
      cleanResp[j++] = response[k];
    }
  }

  char searchStr[8];
  snprintf(searchStr, sizeof(searchStr), "41%s", pid + 2);
  char* pos = strstr(cleanResp, searchStr);

  if (pos != NULL) {
    const char* data = pos + 4;
    int len = 0;
    while (data[len] != '\0' && len < 4) len++;
    if (len < dataBytes * 2) return NAN;

    int value = 0;
    for (int k = 0; k < dataBytes * 2; k++) {
      value = (value << 4) | hexToInt(data[k]);
    }
    return value * multiplier;
  }
  return NAN;
}

float readVoltageELM() {
  while(SerialELM.available()) SerialELM.read();
  
  SerialELM.println("AT RV");
  unsigned long start = millis();
  char response[16] = {0};
  int i = 0;
  bool done = false;
  while (millis() - start < 500 && !done) {
    while (SerialELM.available() && i < 15) {
      char c = SerialELM.read();
      if (c == '>') { done = true; break; }
      if (c != '\r' && c != '\n' && c != 'V' && c != 'v') response[i++] = c;
    }
  }
  float v = atof(response);
  if (v < 5.0) return NAN;
  return v;
}

Ардуино
#include <SoftwareSerial.h>
#include <SPI.h>
#include <SD.h>

// ===== ПИНЫ =====
SoftwareSerial espSerial(5, 4);  // RX=5 (с P26), TX=4 (на P25)
const int SD_CS = 9;             // Пин CS для SD-карты
const int BUTTON_PIN = 8;        // Пин для кнопки
const int GND_PIN = 6;           // Пин, который будет работать как GND

bool isSystemStarted = false;
bool isReceivingData = false;

void setup() {
  Serial.begin(9600); // ОБЯЗАТЕЛЬНО 9600!
  
  // 1. Настраиваем Пин 6 как землю (GND) для кнопки
  pinMode(GND_PIN, OUTPUT);
  digitalWrite(GND_PIN, LOW); 

  // 2. Настраиваем Пин 8 для кнопки с внутренней подтяжкой к 5В
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // 3. Инициализация SD-карты (на пине 9)
  if (!SD.begin(SD_CS)) {
    Serial.println(F("[!] SD-карта НЕ НАЙДЕНА или ошибка подключения"));
  } else {
    Serial.println(F("[OK] SD-карта: ГОТОВА"));
    
    File logFile = SD.open("LOG.CSV", FILE_WRITE);
    if (logFile) {
      if (logFile.size() == 0) {
        logFile.println("RPM,Temp,Speed,Load,Intake,Throttle,Volt,TrendTemp,TrendRPM,TrendVolt,Error");
      }
      logFile.close();
    }
  }

  // 4. Инициализация связи с ESP32
  espSerial.begin(9600);
  espSerial.setTimeout(50);   

  Serial.println(F("Arduino готова. Нажмите кнопку (между пинами 8 и 6)..."));
}

void loop() {
  // === 1. ЛОГИКА КНОПКИ ===
  if (!isSystemStarted && digitalRead(BUTTON_PIN) == LOW) {
    delay(100); 
    if (digitalRead(BUTTON_PIN) == LOW) {
      isSystemStarted = true;
      isReceivingData = true; 

      Serial.println(F("Кнопка нажата! Отправляю START на ESP32..."));
      espSerial.println("START");
    }
  }

  // === 2. ПРИЕМ ДАННЫХ ОТ ESP32 И ЗАПИСЬ НА SD ===
  if (isReceivingData && espSerial.available()) {
    String data = espSerial.readStringUntil('\n');
    data.trim();

    if (data.length() > 0 && data.length() < 128) {
      if (data == "ACK") {
        Serial.println(F("Получен ACK от ESP32. Ждем данные..."));
      } else {
        Serial.print(F("Лог: "));
        Serial.println(data);

        File logFile = SD.open("LOG.CSV", FILE_WRITE);
        if (logFile) {
          logFile.println(data);
          logFile.close();
        } else {
          Serial.println(F("[!] Ошибка записи на SD-карту"));
        }
      }
    }
  }
}
