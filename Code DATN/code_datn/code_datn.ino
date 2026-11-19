#include "esp_camera.h"
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <PCF8574.h>
#include <ESP32Servo.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>

// ================= CẤU HÌNH HỆ THỐNG =================
const char* ssid = "hoangle";
const char* password = "12345678";
WiFiServer server(8888);
WiFiClient client;
Preferences prefs;

#define SDA_PIN 13
#define SCL_PIN 14
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial);

// CAMERA PINS (AI-THINKER)
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      21
#define Y4_GPIO_NUM      19
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

// Quản lý đa luồng & Camera
camera_fb_t* fb_buffer = nullptr;       // Gửi ảnh
SemaphoreHandle_t frameMutex;           // Chia việc
bool cameraEnabled = false;             // Mềm
bool cameraInitialized = false;         // Cứng

// Quản lý Timeout tách biệt (CẬP NHẬT MỚI)
unsigned long globalTimeoutStart = 0;
const long RECOG_TIMEOUT = 10000;  // 10 giây (Nhận diện/Mở cửa)
const long ACTION_TIMEOUT = 20000; // 20 giây (Cài đặt/Thêm mới)

// WiFi ngầm
unsigned long lastWiFiCheck = 0;
bool isWiFiConnected = false;

// Keypad & PCF8574
PCF8574 pcf(0x20);
int rowPins[4] = {0, 7, 6, 5};
int colPins[4] = {4, 3, 2, 1};
String keymap[4][4] = {{"1","2","3","A"},{"4","5","6","B"},{"7","8","9","C"},{"*","0","#","D"}};

// Servo
Servo myServo;
#define SERVO_PIN 12

// State Machine
enum State { MAIN, FACE, FINGER, PIN, MENU_LOCK, SUBMENU, SFACE, SFACEDEL, SFINGERDEL, SPIN };
State state = MAIN;
String pinBuffer = "";
int menuIndex = 0;
int currentFaceID = -1;
bool isEnteringPin = false;

// ================= HÀM GIAO DIỆN =================
void drawWiFiStatus() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (WiFi.status() == WL_CONNECTED) {
    if (client && client.connected()) {
      // Khi đã có Server Python kết nối vào
      display.print("WIFI: SERVER"); 
    } else {
      // Khi chỉ có WiFi nhưng chưa có Server kết nối
      display.print("ID: "); 
      display.print(WiFi.localIP().toString());
    }
  } else {
    display.print("WIFI: DISCONNECTED");
  }
  display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
}

void show(String l1, String l2 = "") {
  display.clearDisplay();
  drawWiFiStatus();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 12); display.println(l1);
  display.setCursor(0, 32); display.println(l2);
  display.display();
}

void resetToMain() {
  cameraEnabled = false;
  isEnteringPin = false;
  pinBuffer = "";
  state = MAIN;
  showMainMenu();
}

void drawScrollGui() {
  display.setCursor(113, 16); display.print("A");
  display.drawLine(112, 30, 115, 27, SSD1306_WHITE);
  display.drawLine(115, 27, 118, 30, SSD1306_WHITE);
  display.drawLine(115, 27, 115, 47, SSD1306_WHITE);
  display.drawLine(112, 44, 115, 47, SSD1306_WHITE);
  display.drawLine(115, 47, 118, 44, SSD1306_WHITE);
  display.setCursor(113, 52); display.print("B");
}

void showMenu() {
  String items[7] = {" 1: Add Face", " 2: Del Face", " 3: Add Finger", " 4: Del Finger", " 5: Change PIN", " 6: Back", " 7: RESET ALL"};
  display.clearDisplay();
  drawWiFiStatus();
  display.setCursor(40, 12); display.println("SETTING");
  for (int i = 0; i < 4; i++) {
    int idx = menuIndex + i;
    if (idx < 7) { display.setCursor(0, 24 + i * 10); display.println(items[idx]); }
  }
  drawScrollGui();
  display.display();
}

void showMainMenu() {
  String items[4] = {" 1: Face", " 2: Finger", " 3: PIN", " 4: Setting"};
  display.clearDisplay();
  drawWiFiStatus();
  display.setCursor(50, 12); display.println("MENU");
  for (int i = 0; i < 4; i++) { display.setCursor(0, 24 + i * 10); display.println(items[i]); }
  display.display();
}

void showDeleteMenu(String title) {
  display.clearDisplay();
  drawWiFiStatus();
  display.setCursor(30, 12); display.println(title);
  display.setCursor(0, 24); display.println("1: ID 1   2: ID 2");
  display.setCursor(0, 36); display.println("3: ID 3   4: ID 4");
  display.setCursor(0, 48); display.println("5: ID 5   6: BACK");
  display.display();
}

// ================= CÁC TÁC VỤ HỆ THỐNG =================
void stopCameraProcess() {
  cameraEnabled = false;
  vTaskDelay(50 / portTICK_PERIOD_MS); 
  if (xSemaphoreTake(frameMutex, portMAX_DELAY)) {
    if (fb_buffer) { esp_camera_fb_return(fb_buffer); fb_buffer = nullptr; }
    xSemaphoreGive(frameMutex);
  }
}

void openDoorWithInfo(String info) {
  show("THANH CONG", info);
  myServo.write(0); delay(5000); 
  show("DONG CUA"); myServo.write(90); delay(1000);
  resetToMain();
}

int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) return -1;
  return finger.fingerID;
}

bool enrollFinger(int id) {
  int p = -1;
  unsigned long startEnroll = millis();
  show("DANG KY ID: " + String(id), "Dat ngon tay...");
  while (p != FINGERPRINT_OK) { 
    p = finger.getImage(); 
    if (millis() - startEnroll > ACTION_TIMEOUT) return false;
  }
  p = finger.image2Tz(1); if (p != FINGERPRINT_OK) return false;
  show("NHAC TAY RA"); delay(1000); p = 0;
  while (p != FINGERPRINT_NOFINGER) { p = finger.getImage(); }
  show("DAT LAI LAN 2"); p = -1;
  while (p != FINGERPRINT_OK) { 
    p = finger.getImage(); 
    if (millis() - startEnroll > ACTION_TIMEOUT) return false;
  }
  p = finger.image2Tz(2); if (p != FINGERPRINT_OK) return false;
  if (finger.createModel() != FINGERPRINT_OK) return false;
  if (finger.storeModel(id) != FINGERPRINT_OK) return false;
  return true;
}

bool isFaceRegistered(int id) { return prefs.getBool(("f" + String(id)).c_str(), false); }
int getFreeFaceID() {
  for (int i = 1; i <= 5; i++) { if (!isFaceRegistered(i)) return i; }
  return -1; 
}

// ================= MULTI-THREADING CAMERA =================
void init_camera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM; config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000; 
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;                   //320*240
  config.jpeg_quality = 12; config.fb_count = 2;
  cameraInitialized = (esp_camera_init(&config) == ESP_OK);
}

void captureTask(void* pv) {
  while (1) {
    if (cameraEnabled && cameraInitialized) {
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        if (xSemaphoreTake(frameMutex, portMAX_DELAY)) {
          if (fb_buffer) esp_camera_fb_return(fb_buffer);
          fb_buffer = fb;
          xSemaphoreGive(frameMutex);
        }
      }
    }
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void sendTask(void* pv) {
  while (1) {
    if (client && client.connected() && cameraEnabled) {
      camera_fb_t* fb_to_send = nullptr;
      if (xSemaphoreTake(frameMutex, portMAX_DELAY)) {
        if (fb_buffer) { fb_to_send = fb_buffer; fb_buffer = nullptr; }
        xSemaphoreGive(frameMutex);
      }
      if (fb_to_send) {
        uint32_t len = fb_to_send->len;
        client.write((uint8_t*)&len, 4);
        client.write(fb_to_send->buf, len);
        esp_camera_fb_return(fb_to_send);
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ================= KEYPAD =================
String lastKey = ""; // Biến toàn cục lưu phím vừa nhấn

String getKey() {
  String currentKey = "";
  
  for (int r = 0; r < 4; r++) {
    pcf.write(rowPins[r], LOW);
    for (int c = 0; c < 4; c++) {
      if (pcf.read(colPins[c]) == LOW) {
        currentKey = keymap[r][c];
      }
    }
    pcf.write(rowPins[r], HIGH);
  }

  // LOGIC TRÁNH DÍNH PHÍM KHÔNG DÙNG DELAY
  if (currentKey != "") { 
    if (currentKey == lastKey) {
      return ""; // Đang giữ phím cũ, không trả về gì cả
    } else {
      lastKey = currentKey; // Ghi nhớ phím mới nhấn
      return currentKey;    // Trả về phím mới
    }
  } else {
    lastKey = ""; // Đã nhả tay (không có phím nào nhấn)
    return "";
  }
}

// ================= SETUP & LOOP =================
void setup() {
  Serial.begin(57600); Wire.begin(SDA_PIN, SCL_PIN);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  frameMutex = xSemaphoreCreateMutex();
  prefs.begin("lock-data", false);
  if (prefs.getString("pin", "").length() == 0) prefs.putString("pin", "0000");

  WiFi.begin(ssid, password);
  show("BOOT", "Ket noi WiFi...");
  
  server.begin(); pcf.begin();
  for (int i = 0; i < 8; i++) pcf.write(i, HIGH);
  myServo.attach(SERVO_PIN); myServo.write(90);

  init_camera();
  xTaskCreatePinnedToCore(captureTask, "capture", 4096, NULL, 5, NULL, 0);
  xTaskCreatePinnedToCore(sendTask, "send", 8192, NULL, 4, NULL, 1);
  
  delay(2000);   // khoảng chờ wifi
  isWiFiConnected = (WiFi.status() == WL_CONNECTED);
  showMainMenu();
}

void loop() {
  // Kiểm tra WiFi định kỳ 
  if (millis() - lastWiFiCheck > 2000) {
    lastWiFiCheck = millis();
    if (state == MAIN) showMainMenu();
    else if (state == SUBMENU) showMenu();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }

  // QUẢN LÝ TIMEOUT TÁCH BIỆT (YÊU CẦU MỚI)
  if (state != MAIN && state != SUBMENU) {
    bool isActionState = (state == SFACE || state == SFACEDEL || state == SFINGERDEL || state == SPIN);
    long currentLimit = isActionState ? ACTION_TIMEOUT : RECOG_TIMEOUT; // 20s hoặc 10s
    
    if (millis() - globalTimeoutStart > currentLimit) {
      show("HET THOI GIAN"); delay(1000); 
      if (isActionState) {
        // Về Submenu nếu đang trong phần cài đặt
        stopCameraProcess(); isEnteringPin = false; pinBuffer = "";
      }
      else {
        // Về Main nếu đang ở phần nhận diện mở khóa
        resetToMain();
      }
    }
  }

  // Quản lý kết nối Client
  if (!client || !client.connected()) {
    client = server.available();
  }

  String key = getKey();
  if (key != "") globalTimeoutStart = millis(); // Reset timeout khi nhấn phím

  // XỬ LÝ PHẢN HỒI TỪ SERVER PYTHON
  if (client && client.connected() && client.available()) {
    String cmd = client.readStringUntil('\n'); cmd.trim();
    
    if (cmd.startsWith("1D")) { 
        stopCameraProcess();
        if (cmd == "1DN") { show("KHONG KHOP"); delay(2000); resetToMain(); }
        else { openDoorWithInfo("FACE ID: " + cmd.substring(2)); }
    }
    else if (cmd.startsWith("1O")) { 
        stopCameraProcess();
        int id = cmd.substring(2).toInt();
        prefs.putBool(("f" + String(id)).c_str(), true); 
        show("THANH CONG", "Luu ID: " + String(id)); delay(2000); state = SUBMENU; showMenu(); 
    }
    else if (cmd == "1TR") { 
        show("DANG NHAN DANG", "VUI LONG CHO..."); 
        globalTimeoutStart = millis(); // Reset timeout khi server đang xử lý
    }
    else if (cmd == "1TRS") { 
        show("ID: " + String(currentFaceID), "DANG THEM KHUON MAT"); 
        globalTimeoutStart = millis(); // Reset timeout khi server đang xử lý
    }
    else if (cmd == "1NF") { 
        show("NHAN DANG...", "KO THAY MAT"); 
    }
    else if (cmd == "1NFS") { 
        show("ID: " + String(currentFaceID), "KO THAY MAT"); 
    }
    else if (cmd == "1S") { 
        stopCameraProcess(); show("LOI CHUP"); delay(2000); state = SUBMENU; showMenu(); 
    }
  }

  // Logic nhập PIN
  if (isEnteringPin && key != "") {
    if (key >= "0" && key <= "9") {
      pinBuffer += key;
      String h = ""; for(int i=0; i<pinBuffer.length(); i++) h += "*"; show("PIN:", h);
      if (pinBuffer.length() == 4) {
        isEnteringPin = false;
        if ((state == PIN || state == MENU_LOCK) && pinBuffer == prefs.getString("pin", "0000")) {
          if (state == PIN) openDoorWithInfo("DUNG PIN");
          else { menuIndex = 0; showMenu(); state = SUBMENU; }   //menu0-3
        } else if (state == SPIN) {
          prefs.putString("pin", pinBuffer); show("DOI PIN OK"); delay(1000); state = SUBMENU; showMenu();
        } else { show("SAI PIN"); delay(1000); resetToMain(); }
        pinBuffer = "";
      }
      key = ""; // chống giữ trạng thái phím
    }
  }

  // Xử lý các trạng thái phím bấm
  switch (state) {
    case MAIN:
      if (key == "1") { // Nhận dạng Face (10s)
        if (client && client.connected()) {
          show("NHAN DANG..."); client.println("1B"); cameraEnabled = true; globalTimeoutStart = millis(); state = FACE; 
        } else { show("NO SERVER"); delay(1000); showMainMenu(); }
      }
      else if (key == "2") { show("DAT TAY"); globalTimeoutStart = millis(); state = FINGER; } // 10s
      else if (key == "3") { show("NHAP PIN"); isEnteringPin = true; globalTimeoutStart = millis(); state = PIN; } // 10s
      else if (key == "4") { show("ADMIN PIN"); isEnteringPin = true; globalTimeoutStart = millis(); state = MENU_LOCK; }
      break;

    case SUBMENU:
      if (key == "A" && menuIndex > 0) { menuIndex--; showMenu(); }
      else if (key == "B" && menuIndex < 3) { menuIndex++; showMenu(); }
      else if (key == "1") {        // Thêm Face (20s)
        int fid = getFreeFaceID();
        if (fid != -1 && client && client.connected()) {
          currentFaceID = fid;
          show("ID: " + String(fid)); client.println("1T" + String(fid));
          cameraEnabled = true; globalTimeoutStart = millis(); state = SFACE;
        } else { show("ERROR/WIFI"); delay(1000); showMenu(); }
      }
      else if (key == "2") { showDeleteMenu("XOA MAT"); state = SFACEDEL; }
      else if (key == "3") { // Thêm vân tay (20s)
        state = SFINGERDEL; // Chuyển state tạm để tính timeout 20s
        int fID = -1; for (int i=1; i<=5; i++) { if (finger.loadModel(i) != FINGERPRINT_OK) { fID = i; break; } }
        if (fID != -1 && enrollFinger(fID)) show("OK"); else show("LOI"); delay(1000); state = SUBMENU; showMenu();
      }
      else if (key == "4") { showDeleteMenu("XOA TAY"); state = SFINGERDEL; }
      else if (key == "5") { show("PIN MOI"); isEnteringPin = true; globalTimeoutStart = millis(); state = SPIN; } // 20s
      else if (key == "6") { resetToMain(); }
      else if (key == "7") {show("DANG XOA HET...");
        if (client && client.connected()) {
          client.println("2RESET"); // Gửi lệnh cho Python
          finger.emptyDatabase(); // Xóa vân tay
          prefs.clear(); // Xóa Preferences
          prefs.putString("pin", "0000");
          show("DA XOA SACH!");
          delay(2000);
          ESP.restart();
        } else{ show("ER: SEVER = DIS");}
        }
      break;

    case SFACEDEL:
      if (key == "6") { showMenu(); state = SUBMENU; }
      else if (key >= "1" && key <= "5") {
        if (client && client.connected()) {
          client.println("2F" + key); 
          prefs.putBool(("f" + key).c_str(), false);       // trả lại ghế trống
          show("XOA OK"); delay(1000); showDeleteMenu("XOA MAT");
        }
      }
      break;

    case SFINGERDEL:
      if (key == "6") { showMenu(); state = SUBMENU; }
      else if (key >= "1" && key <= "5") {
        if (finger.deleteModel(key.toInt()) == FINGERPRINT_OK) show("XOA OK"); else show("LOI"); 
        delay(1000); showDeleteMenu("XOA TAY");
      }
      break;

    case FINGER:
      int id = getFingerprintID(); 
      if (id != -1) { openDoorWithInfo("FINGER ID: " + String(id)); }
      break;
  }
}
