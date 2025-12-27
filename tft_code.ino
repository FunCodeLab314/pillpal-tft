// ============================================================================
//  PillPal ESP32-S3 - SCREEN FLOW & LOGGING UPDATE
// ============================================================================

#include <lvgl.h>
#include <PINS_JC4827W543.h>
#include "TAMC_GT911.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h> 
#include <ArduinoJson.h>
#include "time.h"  // For Real Time
#include "ui.h"

// ==================== CONFIGURATION ====================
#define WIFI_SSID       "ESP32"
#define WIFI_PASSWORD   "12345678"

// Time Configuration (Philippines GMT+8)
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 28800; // GMT+8 = 8 * 3600
const int   daylightOffset_sec = 0;

#define MQTT_SERVER     "690e24c4c0b347498798415094d4ebd6.s1.eu.hivemq.cloud" 
#define MQTT_PORT       8883
#define MQTT_TOPIC      "pillpal/dispense_cmd"
#define MQTT_CLIENT_ID  "PillPal_Kiosk_S3"
#define MQTT_USERNAME   "pillpal_admin"
#define MQTT_PASSWORD   "Pillpal123456"

#define DISPENSER_MAC_ADDRESS "98:88:e0:03:90:ed"
static BLEUUID serviceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static BLEUUID commandCharUUID("beb5483e-36e1-4688-b7f5-ea07361b26a8");
static BLEUUID statusCharUUID("a1e1f32a-57b1-4176-a19f-d31e5f8f831b");

// ==================== GLOBALS ====================
WiFiClientSecure espClient;
PubSubClient client(espClient);

String currentSlot = "1";
String currentPatientName = "";
String currentMedication = "";

volatile bool uiNeedsUpdate = false;
volatile bool alarmActive = false;
bool bleConnected = false;

// BLE Objects
BLEClient* pBLEClient = NULL;
BLERemoteCharacteristic* pCommandChar = NULL;
BLERemoteCharacteristic* pStatusChar = NULL;

// Hardware
#define TOUCH_SDA 8
#define TOUCH_SCL 4
#define TOUCH_INT 3
#define TOUCH_RST 38
#define TOUCH_WIDTH 480
#define TOUCH_HEIGHT 272

TAMC_GT911 touchController = TAMC_GT911(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, TOUCH_WIDTH, TOUCH_HEIGHT);

uint32_t screenWidth, screenHeight, bufSize;
lv_display_t *disp;
lv_color_t *disp_draw_buf;
lv_indev_t *indev;

// ==================== HELPER FUNCTIONS ====================

// Log to Serial AND Screen 1
void logStatus(String msg) {
    Serial.println(msg); // Print to Serial Monitor
    
    // Update Screen 1 Label
    if (ui_uiserialLabel) {
        lv_label_set_text(ui_uiserialLabel, msg.c_str());
        lv_refr_now(disp); // Force immediate refresh
    }
}

// Update Time on Screen 1
void printLocalTime() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        // Only log this if we are on screen 1 to avoid spamming
        if(lv_scr_act() == ui_Screen1) Serial.println("Failed to obtain time");
        return;
    }
    
    // Format: 12:00 am
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%I:%M %p", &timeinfo);
    
    if(ui_timeLbl) {
        lv_label_set_text(ui_timeLbl, timeStringBuff);
    }
}

// ==================== BLE FUNCTIONS ====================

bool connectToBLEDispenser() {
    if (bleConnected && pBLEClient != NULL && pBLEClient->isConnected()) {
        return true;
    }

    logStatus(">>> [BLE] Connecting...");
    if (pBLEClient == NULL) {
        pBLEClient = BLEDevice::createClient();
    }
    
    if (!pBLEClient->connect(BLEAddress(DISPENSER_MAC_ADDRESS))) {
        logStatus("!!! [BLE] Connection failed");
        bleConnected = false;
        return false;
    }
    
    BLERemoteService* pRemoteService = pBLEClient->getService(serviceUUID);
    if (pRemoteService == nullptr) {
        logStatus("!!! [BLE] Service not found");
        pBLEClient->disconnect();
        bleConnected = false;
        return false;
    }
    
    pCommandChar = pRemoteService->getCharacteristic(commandCharUUID);
    if (pCommandChar == nullptr) {
        logStatus("!!! [BLE] Cmd Char not found");
        pBLEClient->disconnect();
        bleConnected = false;
        return false;
    }
    
    pStatusChar = pRemoteService->getCharacteristic(statusCharUUID);
    
    bleConnected = true;
    logStatus(">>> [BLE] Connected!");
    return true;
}

void sendDispenseCommand(String slotNumber) {
    logStatus(">>> [BLE] Dispensing slot " + slotNumber);
    if (!connectToBLEDispenser()) {
        logStatus("!!! [BLE] Cannot dispense");
        return;
    }
    
    JsonDocument doc;
    doc["command"] = "DISPENSE";
    doc["slot"] = slotNumber;
    
    String jsonCmd;
    serializeJson(doc, jsonCmd);
    if (pCommandChar) {
        pCommandChar->writeValue(jsonCmd.c_str(), jsonCmd.length());
        logStatus(">>> [BLE] Command Sent!");
    }
}

void sendAlarmStart() {
    if (!connectToBLEDispenser()) return;
    
    JsonDocument doc;
    doc["command"] = "ALARM_START";
    String jsonCmd;
    serializeJson(doc, jsonCmd);
    if (pCommandChar) {
        pCommandChar->writeValue(jsonCmd.c_str(), jsonCmd.length());
        alarmActive = true;
        logStatus(">>> [BLE] Alarm Started");
    }
}

void sendAlarmStop() {
    if (!connectToBLEDispenser()) return;
    
    JsonDocument doc;
    doc["command"] = "ALARM_STOP";
    String jsonCmd;
    serializeJson(doc, jsonCmd);
    if (pCommandChar) {
        pCommandChar->writeValue(jsonCmd.c_str(), jsonCmd.length());
        alarmActive = false;
        logStatus(">>> [BLE] Alarm Stopped");
    }
}

// ==================== UI BUTTON CALLBACK ====================

// This function handles the Dispense Button on Screen 2
void on_dispense_btn_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if(code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED) {
        Serial.println(">>> [UI] Dispense Button Clicked");
        
        // 1. Stop the buzzer
        sendAlarmStop();
        delay(200); 
        
        // 2. Send Dispense Command
        sendDispenseCommand(currentSlot);

        // 3. Reset UI Labels (Optional cleanup)
        if(ui_patientName) lv_label_set_text(ui_patientName, "---");
        if(ui_medication) lv_label_set_text(ui_medication, "---");
        if(ui_slot) lv_label_set_text(ui_slot, "---");
        
        // 4. GO BACK TO SCREEN 1
        Serial.println(">>> [UI] Returning to Screen 1");
        lv_scr_load(ui_Screen1);
    }
}

// ==================== MQTT CALLBACK ====================
void callback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    logStatus("MQTT Recv: " + message);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        logStatus("MQTT JSON Error");
        return;
    }

    const char* command = doc["command"];
    if (command != NULL && String(command) == "DISPENSE") {
        currentSlot = String((const char*)doc["slot"]);
        currentPatientName = String((const char*)doc["patientName"]);
        currentMedication = String((const char*)doc["medicationName"]);
        
        // 1. Start Alarm
        sendAlarmStart();
        
        // 2. Trigger UI Update (switches screen in loop)
        uiNeedsUpdate = true;
        
    } else if (command != NULL && String(command) == "ALARM_START") {
        sendAlarmStart();
    } else if (command != NULL && String(command) == "ALARM_STOP") {
        sendAlarmStop();
    }
}

void reconnect() {
    // Only try to reconnect if not connected
    if (!client.connected()) {
        logStatus("MQTT Connecting...");
        if (client.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) { 
            logStatus("MQTT Connected!");
            client.subscribe(MQTT_TOPIC);
            // Force status update to screen
            lv_label_set_text(ui_uiserialLabel, "MQTT Connected | Ready");
        } else {
            // Short log to avoid clutter
            Serial.print("MQTT fail rc=");
            Serial.println(client.state());
        }
    }
}

// ==================== LVGL DRIVERS ====================
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_t *indev_drv, lv_indev_data_t *data) {
    touchController.read();
    if (touchController.isTouched && touchController.touches > 0) {
        data->point.x = touchController.points[0].x;
        data->point.y = touchController.points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ==================== SETUP ====================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("PillPal Kiosk Starting...");

    // 1. Init Display
    if (!gfx->begin()) { 
        Serial.println("Display Init Failed!");
        while(1);
    }
    pinMode(GFX_BL, OUTPUT); 
    digitalWrite(GFX_BL, HIGH);
    gfx->fillScreen(RGB565_BLACK);
    
    // 2. Init Touch
    touchController.begin();
    touchController.setRotation(ROTATION_INVERTED);
    
    // 3. Init LVGL
    lv_init();
    screenWidth = gfx->width();
    screenHeight = gfx->height();
    bufSize = screenWidth * 10;
    disp_draw_buf = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    disp = lv_display_create(screenWidth, screenHeight);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, disp_draw_buf, NULL, bufSize * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(disp);
    
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);
    
    // 4. Init UI
    Serial.println("Initializing UI...");
    ui_init(); // This loads Screen 1 automatically
    
    // 5. Connect to WiFi
    logStatus("Connecting WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int wifi_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifi_attempts < 20) {
        delay(500);
        wifi_attempts++;
        lv_timer_handler(); 
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        logStatus("WiFi Connected: " + WiFi.localIP().toString());
        
        // 6. Init Time (NTP)
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        logStatus("Time Synced");
        
        // 7. Init MQTT
        espClient.setInsecure();
        client.setServer(MQTT_SERVER, MQTT_PORT);
        client.setCallback(callback);
        reconnect(); // Attempt initial connection
        
    } else {
        logStatus("WiFi Failed! Check Creds");
    }
    
    // 8. Init BLE
    BLEDevice::init("PillPal-Kiosk");
    connectToBLEDispenser();

    // 9. Configure Button Event (Fixing variable names here)
    // NOTE: In ui_Screen2.c, the button is named ui_Button1, NOT ui_dispenseBtn
    if(ui_Button1) {
        logStatus("Configuring Button...");
        
        // Ensure button is clickable
        lv_obj_add_flag(ui_Button1, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_state(ui_Button1, LV_STATE_DISABLED);
        lv_obj_move_foreground(ui_Button1);
        
        // Add event callback
        lv_obj_add_event_cb(ui_Button1, on_dispense_btn_event, LV_EVENT_ALL, NULL);
        
        // Fix the label inside the button so it doesn't block touch
        // In ui_Screen2.c, the label inside the button is ui_Label3
        if(ui_Label3) {
            lv_obj_clear_flag(ui_Label3, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(ui_Label3, LV_OBJ_FLAG_IGNORE_LAYOUT);
        }
        logStatus("System Ready");
    } else {
        logStatus("ERROR: Button Not Found");
    }
}

// ==================== LOOP ====================
void loop() {
    lv_timer_handler(); // Must be called frequently
    
    // MQTT Loop
    if (!client.connected() && WiFi.status() == WL_CONNECTED) {
        reconnect();
    }
    client.loop();

    // Update Time (Every 1 second)
    static unsigned long lastTimeCheck = 0;
    if (millis() - lastTimeCheck > 1000) {
        printLocalTime();
        lastTimeCheck = millis();
    }

    // BLE Maintenance
    static unsigned long lastBLECheck = 0;
    if (millis() - lastBLECheck > 5000) { 
        if (!bleConnected) {
            // Only try to reconnect if not busy dispensing
            // connectToBLEDispenser(); // Optional: Auto-reconnect
        }
        lastBLECheck = millis();
    }

    // Switch to Screen 2 when MQTT "DISPENSE" received
    if (uiNeedsUpdate) {
        logStatus("Dispense Req: " + currentPatientName);
        
        if(ui_patientName) lv_label_set_text(ui_patientName, currentPatientName.c_str());
        if(ui_medication) lv_label_set_text(ui_medication, currentMedication.c_str());
        if(ui_slot) lv_label_set_text(ui_slot, currentSlot.c_str());
        
        // SWITCH TO SCREEN 2
        lv_scr_load(ui_Screen2);
        
        uiNeedsUpdate = false;
    }

    delay(5); 
}