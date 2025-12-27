// ============================================================================
//  PillPal ESP32-S3 - FIXED VERSION
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
#include "time.h"
#include "ui.h"

// ==================== CONFIGURATION ====================
#define WIFI_SSID       "ESP32"
#define WIFI_PASSWORD   "12345678"

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 28800;
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
bool buttonConfigured = false;  // NEW: Track if button is configured

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

void logStatus(String msg) {
    Serial.println(msg);
    
    if (ui_uiserialLabel && lv_scr_act() == ui_Screen1) {
        lv_label_set_text(ui_uiserialLabel, msg.c_str());
        lv_refr_now(disp);
    }
}

void printLocalTime() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return;  // Silently fail
    }
    
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

void on_dispense_btn_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if(code == LV_EVENT_CLICKED) {
        Serial.println(">>> [UI] Dispense Button Clicked");
        
        // 1. Stop the buzzer
        sendAlarmStop();
        delay(100); 
        
        // 2. Send Dispense Command
        sendDispenseCommand(currentSlot);

        // 3. Small delay to ensure command is sent
        delay(100);
        
        // 4. GO BACK TO SCREEN 1
        Serial.println(">>> [UI] Returning to Screen 1");
        lv_scr_load(ui_Screen1);
        
        // 5. Reset for next dispense
        buttonConfigured = false;
        
        // 6. Update status on screen 1
        logStatus("Dispense Complete | Ready");
    }
}

// NEW: Function to configure button on Screen 2
void configureDispenseButton() {
    if(ui_Button1 && !buttonConfigured) {
        Serial.println(">>> [UI] Configuring Dispense Button");
        
        // Remove any existing event handlers
        lv_obj_remove_event_cb(ui_Button1, on_dispense_btn_event);
        
        // Make sure button is clickable
        lv_obj_add_flag(ui_Button1, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_state(ui_Button1, LV_STATE_DISABLED);
        lv_obj_move_foreground(ui_Button1);
        
        // Add fresh event callback
        lv_obj_add_event_cb(ui_Button1, on_dispense_btn_event, LV_EVENT_CLICKED, NULL);
        
        // Fix the label inside the button
        if(ui_Label3) {
            lv_obj_clear_flag(ui_Label3, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(ui_Label3, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        
        buttonConfigured = true;
        Serial.println(">>> [UI] Button Configured Successfully");
    }
}

// ==================== MQTT CALLBACK ====================
void callback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    Serial.println(">>> [MQTT] Message received: " + message);
    logStatus("MQTT: Dispensing...");

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        logStatus("MQTT JSON Error");
        Serial.println("JSON Error: " + String(error.c_str()));
        return;
    }

    const char* command = doc["command"];
    if (command != NULL && String(command) == "DISPENSE") {
        currentSlot = String((const char*)doc["slot"]);
        currentPatientName = String((const char*)doc["patientName"]);
        currentMedication = String((const char*)doc["medicationName"]);
        
        Serial.println(">>> [MQTT] Dispense command parsed:");
        Serial.println("    Slot: " + currentSlot);
        Serial.println("    Patient: " + currentPatientName);
        Serial.println("    Medication: " + currentMedication);
        
        // 1. Start Alarm immediately
        sendAlarmStart();
        
        // 2. Trigger UI Update
        uiNeedsUpdate = true;
        
    } else if (command != NULL && String(command) == "ALARM_START") {
        sendAlarmStart();
    } else if (command != NULL && String(command) == "ALARM_STOP") {
        sendAlarmStop();
    }
}

void reconnect() {
    if (!client.connected()) {
        Serial.print(">>> [MQTT] Connecting... ");
        logStatus("MQTT Connecting...");
        
        if (client.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) { 
            Serial.println("Connected!");
            logStatus("MQTT Connected | Ready");
            client.subscribe(MQTT_TOPIC);
            Serial.println(">>> [MQTT] Subscribed to: " + String(MQTT_TOPIC));
        } else {
            Serial.print("Failed, rc=");
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
    Serial.println("\n\n=================================");
    Serial.println("PillPal Kiosk Starting...");
    Serial.println("=================================\n");

    // 1. Init Display
    Serial.println(">>> Initializing Display...");
    if (!gfx->begin()) { 
        Serial.println("!!! Display Init Failed!");
        while(1);
    }
    pinMode(GFX_BL, OUTPUT); 
    digitalWrite(GFX_BL, HIGH);
    gfx->fillScreen(RGB565_BLACK);
    Serial.println(">>> Display OK");
    
    // 2. Init Touch
    Serial.println(">>> Initializing Touch...");
    touchController.begin();
    touchController.setRotation(ROTATION_INVERTED);
    Serial.println(">>> Touch OK");
    
    // 3. Init LVGL
    Serial.println(">>> Initializing LVGL...");
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
    Serial.println(">>> LVGL OK");
    
    // 4. Init UI
    Serial.println(">>> Initializing UI Screens...");
    ui_init();
    Serial.println(">>> UI OK - Screen 1 Loaded");
    
    // 5. Connect to WiFi
    Serial.println("\n>>> Connecting to WiFi...");
    logStatus("Connecting WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int wifi_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifi_attempts < 20) {
        delay(500);
        Serial.print(".");
        wifi_attempts++;
        lv_timer_handler(); 
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(">>> WiFi Connected!");
        Serial.println(">>> IP: " + WiFi.localIP().toString());
        logStatus("WiFi: " + WiFi.localIP().toString());
        
        // 6. Init Time (NTP)
        Serial.println(">>> Syncing time with NTP...");
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        delay(2000); // Wait for time sync
        printLocalTime();
        Serial.println(">>> Time Synced");
        
        // 7. Init MQTT
        Serial.println(">>> Configuring MQTT...");
        espClient.setInsecure();
        client.setServer(MQTT_SERVER, MQTT_PORT);
        client.setCallback(callback);
        client.setBufferSize(512);  // Increase buffer for JSON
        reconnect();
        
    } else {
        Serial.println("!!! WiFi Connection Failed!");
        logStatus("WiFi Failed!");
    }
    
    // 8. Init BLE
    Serial.println(">>> Initializing BLE...");
    BLEDevice::init("PillPal-Kiosk");
    connectToBLEDispenser();

    Serial.println("\n=================================");
    Serial.println("System Ready!");
    Serial.println("Waiting for dispense commands...");
    Serial.println("=================================\n");
    logStatus("System Ready");
}

// ==================== LOOP ====================
void loop() {
    lv_timer_handler();
    
    // MQTT Loop
    if (!client.connected() && WiFi.status() == WL_CONNECTED) {
        static unsigned long lastReconnect = 0;
        if (millis() - lastReconnect > 5000) {
            reconnect();
            lastReconnect = millis();
        }
    }
    client.loop();

    // Update Time (Every 1 second)
    static unsigned long lastTimeCheck = 0;
    if (millis() - lastTimeCheck > 1000) {
        if(lv_scr_act() == ui_Screen1) {
            printLocalTime();
        }
        lastTimeCheck = millis();
    }

    // Handle Screen Switch to Screen 2
    if (uiNeedsUpdate) {
        Serial.println("\n>>> [UI] Switching to Screen 2");
        Serial.println("    Patient: " + currentPatientName);
        Serial.println("    Medication: " + currentMedication);
        Serial.println("    Slot: " + currentSlot);
        
        // Update labels BEFORE switching screen
        if(ui_patientName) {
            lv_label_set_text(ui_patientName, currentPatientName.c_str());
            Serial.println(">>> [UI] Patient label updated");
        }
        if(ui_medication) {
            lv_label_set_text(ui_medication, currentMedication.c_str());
            Serial.println(">>> [UI] Medication label updated");
        }
        if(ui_slot) {
            lv_label_set_text(ui_slot, currentSlot.c_str());
            Serial.println(">>> [UI] Slot label updated");
        }
        
        // SWITCH TO SCREEN 2
        lv_scr_load(ui_Screen2);
        Serial.println(">>> [UI] Screen 2 loaded");
        
        // Force LVGL update
        lv_refr_now(disp);
        
        // Configure button AFTER screen switch
        delay(50); // Small delay to ensure screen is fully loaded
        configureDispenseButton();
        
        uiNeedsUpdate = false;
        Serial.println(">>> [UI] Ready for button press\n");
    }

    delay(5); 
}
