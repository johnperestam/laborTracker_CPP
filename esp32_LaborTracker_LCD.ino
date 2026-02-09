#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>              
#include "AiEsp32RotaryEncoder.h"     //https://github.com/igorantolic/ai-esp32-rotary-encoder
#include <Preferences.h>
#include "settings.h"

Preferences prefs;
// --------------------------------------------------------
// LCD CONFIG
// --------------------------------------------------------
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Custom arrow characters
//byte upArrow[8]   = {0x04,0x0E,0x15,0x04,0x04,0x04,0x04,0x00};
//byte downArrow[8] = {0x04,0x04,0x04,0x04,0x15,0x0E,0x04,0x00};

byte upArrow[8]   = {0x04, 0x04, 0x0E, 0x0E, 0x1F, 0x1F, 0x00, 0x00};
byte downArrow[8] = {0x00, 0x00, 0x1F, 0x1F, 0x0E, 0x0E, 0x04, 0x04};

// --------------------------------------------------------
// ROTARY ENCODER CONFIG
// --------------------------------------------------------
#define ROTARY_ENCODER_A_PIN 17
#define ROTARY_ENCODER_B_PIN 16
#define ROTARY_ENCODER_BUTTON_PIN 15
#define ROTARY_ENCODER_STEPS 4

volatile int encoderDelta = 0;
volatile bool buttonPressed = false;

unsigned long buttonPressStart = 0;
bool buttonDown = false;

//---------------------------------------------------------

#define MACHINE_MAX_LEN 12
char MACHINE[MACHINE_MAX_LEN] = "";
#define MAC_ADDR_LEN 8
char MAC_ADDR[MAC_ADDR_LEN] = "";
//char MACHINE[12]          = "sawing"; 


char REST_URL[256];
const char* WS_PATH = "/";

#define LCD_COLS    20
#define LCD_ROWS    4

// --------------------------------------------------------
// SLEEP LOGIC
// --------------------------------------------------------
unsigned long lastActivity = 0;
const unsigned long SLEEP_TIMEOUT = 300000;  
bool backlightOn = true; // Track current state
bool isSleeping = false;

// --------------------------------------------------------
// DATA / STATE
// --------------------------------------------------------
enum Screen { SCR_ORDERS, SCR_LINES, SCR_STATUS };
Screen currentScreen = SCR_ORDERS;

JsonDocument doc;

int cursorRow = 0;
int scrollOffset = 0;        
int selectedIndex = 0;

int selectedIndexOrders = 0;
int scrollOffsetOrders = 0;

int selectedIndexLines = 0;
int scrollOffsetLines = 0;

int selectedIndexStatus = 0;
int scrollOffsetStatus = 0;

const int visibleRows = 3;

long lastEncoderPos = 0;

int selectedOrder = 0;
int selectedLine = 0;

struct StatusOption {
    int code;
    const char* label;
};

StatusOption statusOpts[2];
int numStatusOpts = 0;
int statusMenuIndex = 0;

String (*getItemCallback)(int) = nullptr;

// setup the rotary encoder
AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);
void IRAM_ATTR readEncoderISR(){ rotaryEncoder.readEncoder_ISR();}

// ======================= HELPERS =======================
String truncate20(const String &s) {
    if (s.length() > 19) return s.substring(0, 16) + "..";
    return s;
}

void lcdClearLine(uint8_t row) {
    lcd.setCursor(0, row);
    for (uint8_t i = 0; i < LCD_COLS; i++) lcd.print(" ");
}

void lcdWriteRow(uint8_t row, bool selected, const String &text) {
    lcdClearLine(row);
    lcd.setCursor(0, row);
    if (currentScreen == SCR_STATUS) {
        // print full text without truncating
        lcd.print(selected ? ">" : " ");
        lcd.print(text);
    } else {
        lcd.print(selected ? ">" : " ");
        lcd.print(truncate20(text));
    }
}

int getCursorRowForScreen() {
    if (currentScreen == SCR_STATUS) {
        return selectedIndex + 2;   // options start at row 2
    }
    return selectedIndex - scrollOffset + 1; // default
}

//--------------------------------------------------------
// UPDATE SCREEN HELPERS
//--------------------------------------------------------
void updateOrderRow(int orderIndex) {
    if (currentScreen != SCR_ORDERS) return;

    int total = doc.as<JsonArray>().size();
    if (orderIndex < scrollOffset || orderIndex >= scrollOffset + visibleRows) {
        return;   // it's off-screen, nothing to redraw
    }

    uint8_t row = 1 + (orderIndex - scrollOffset);
    lcdWriteRow(row, orderIndex == selectedIndex, getOrderItem(orderIndex));
}

void updateLineRow(int lineIndex) {
    if (currentScreen != SCR_LINES) return;

    JsonArray lines = doc[selectedOrder]["lines"].as<JsonArray>();
    int total = lines.size();

    if (lineIndex < scrollOffset || lineIndex >= scrollOffset + visibleRows) {
        return; // off-screen
    }

    uint8_t row = 1 + (lineIndex - scrollOffset);
    lcdWriteRow(row, lineIndex == selectedIndex, getLineItem(lineIndex));
}

//--------------------------------------------------------
// LIST ROWS REFRESH
//--------------------------------------------------------
void refreshListRows(uint8_t totalItems, String (*getItem)(int)) {

    uint8_t rowOffset = (currentScreen == SCR_STATUS) ? 2 : 1;

    for (uint8_t r = 0; r < visibleRows; r++) {

        uint8_t idx = scrollOffset + r;
        uint8_t lcdRow = rowOffset + r;

        lcdClearLine(lcdRow);     // <<< IMPORTANT: clear entire row first
        lcd.setCursor(0, lcdRow); // now safe to write fresh content

        if (idx < totalItems) {
            lcd.print(idx == selectedIndex ? ">" : " ");
            if (currentScreen == SCR_STATUS) {
                lcd.print(getItem(idx)); // full text
            } else {
                lcd.print(truncate20(getItem(idx)));
            }
        }
    }
}

// --------------------------------------------------------
// DRAW SCREENS
// --------------------------------------------------------
// ORDERS SCREEN
String getOrderItem(int idx) {
    JsonArray orders = doc.as<JsonArray>();
    JsonObject o = orders[idx];

    // If any line in this order is active, mark it
    bool hasActiveLine = false;
    JsonArray lines = o["lines"].as<JsonArray>();
    for (JsonObject ln : lines) {
        if (ln["process_status"].as<int>() == 1) {  // status 1 = active
            hasActiveLine = true;
            break;
        }
    }

    String prefix = hasActiveLine ? "*" : " ";
    return prefix + String(o["order_num"].as<int>()) + " " + String(o["customer_name"].as<const char*>());
}
void drawOrders(bool resetScroll = true) {
    currentScreen = SCR_ORDERS;

    JsonArray orders = doc.as<JsonArray>();
    int totalItems = orders.size();

    lcd.setCursor(0,0);
    lcd.print("--- SELECT ORDER ---");

    getItemCallback = getOrderItem;

    if (resetScroll) {
        cursorRow = 0;
        scrollOffset = 0;
        selectedIndex = 0;
    } else {
        // Restore saved Orders position
        selectedIndex = selectedIndexOrders;
        scrollOffset   = scrollOffsetOrders;
        if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
        if (selectedIndex >= scrollOffset + visibleRows) scrollOffset = selectedIndex - visibleRows + 1;
    }

    refreshListRows(totalItems, getOrderItem);
    updateScrollArrows(totalItems);
}

// LINES SCREEN
String getLineItem(int idx) {
    JsonArray lines = doc[selectedOrder]["lines"].as<JsonArray>();
    JsonObject ln = lines[idx];

    String prefix = ln["process_status"].as<int>() == 1 ? "*" : " ";
    return prefix + String(ln["line_number"].as<int>()) + ": " + String(ln["description"].as<const char*>());
}
void drawLines(bool resetScroll = true) {
    currentScreen = SCR_LINES;

    JsonArray lines = doc[selectedOrder]["lines"].as<JsonArray>();
    int totalItems = lines.size();

    lcd.setCursor(0,0);
    lcd.print("--- SELECT  LINE ---");

    getItemCallback = getLineItem;

    if (resetScroll) {
        cursorRow = 0;
        selectedIndex = 0;
        scrollOffset = 0;
    } else {
        selectedIndex = selectedIndexLines;
        scrollOffset = scrollOffsetLines;
        if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
        if (selectedIndex >= scrollOffset + visibleRows) scrollOffset = selectedIndex - visibleRows + 1;
    }

    refreshListRows(totalItems, getLineItem);
    updateScrollArrows(totalItems);
}

// STATUS SCREEN
String getStatusOptionItem(int idx) {
    return String(statusOpts[idx].label);
}

void buildStatusOptions(uint8_t currentStatus) {
    numStatusOpts = 0;

    if (currentStatus == 0) {
        statusOpts[0] = {1, "Start"};
        numStatusOpts = 1;
    }
    else if (currentStatus == 1) {
        statusOpts[0] = {2, "Stop"};
        statusOpts[1] = {3, "Complete"};
        numStatusOpts = 2;
    }
    else if (currentStatus == 2) {
        statusOpts[0] = {1, "Start"};
        statusOpts[1] = {3, "Complete"};
        numStatusOpts = 2;
    }
}

void drawStatusScreen(bool resetScroll = true) { 
  currentScreen = SCR_STATUS; 

    // Validate selected order/line
    if (selectedOrder >= doc.size()) {
        selectedOrder = max(0, (int)doc.size() - 1);
    }
    if (doc.size() == 0) {
        // No orders left → show orders screen
        drawOrders();
        return;
    }

    JsonArray lines = doc[selectedOrder]["lines"].as<JsonArray>();
    if (selectedLine >= lines.size()) {
        selectedLine = max(0, (int)lines.size() - 1);
    }
    if (lines.size() == 0) {
        drawOrders();
        return;
    }

  JsonObject ln = doc[selectedOrder]["lines"][selectedLine]; 
  uint8_t currentStatus = ln["process_status"].as<uint8_t>(); 
  uint16_t lineNumber = ln["line_number"].as<uint16_t>();
  int orderNumber = ln["order_num"].as<int>();
  buildStatusOptions(currentStatus); 
  
  lcd.clear();

  //Row 0: sales order# / line#
  char lineInfo[21];
  snprintf(lineInfo, sizeof(lineInfo), "SO#:%7d  LN#:%3d", orderNumber,lineNumber);
  lcd.setCursor(0, 0);
  lcd.print(lineInfo);

  //Row 1: screen header
  char header[21];
  snprintf(header, sizeof(header), "-- UPDATE  STATUS --");
  lcd.setCursor(0, 1);
  lcd.print(header);


  getItemCallback = getStatusOptionItem; 
  
    // Row 2 and 3: options
    for (int i = 0; i < 2; i++) {
        lcdClearLine(2 + i);
        lcd.setCursor(0, 2 + i);
        if (i < numStatusOpts) {
            char buf[21];
            snprintf(buf, sizeof(buf), "%c%s", (i == selectedIndex) ? '>' : ' ', getStatusOptionItem(i).c_str());
            lcd.print(buf);
        }
    }

    if (resetScroll) { scrollOffset = 0; selectedIndex = 0; } 

  }


// --------------------------------------------------------
// SLEEP
// --------------------------------------------------------
void checkSleep() {
    if (backlightOn && millis() - lastActivity > SLEEP_TIMEOUT) {
        lcd.noBacklight();
        backlightOn = false;
        isSleeping = true;
    }
}

void wakeUp() {
    if (!backlightOn) {
        lcd.backlight();
        backlightOn = true;
    }
    lastActivity = millis();  // reset timer on any activity
    isSleeping = false;
}

// --------------------------------------------------------
// WEBSOCKET
// --------------------------------------------------------
WebSocketsClient ws;

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.println("[WS] Disconnected!");
            break;

        case WStype_CONNECTED:
            Serial.println("[WS] Connected.");
            char msg[80];
            snprintf(msg, sizeof(msg), "{\"hostname\":\"%s\",\"mac\":\"%s\"}", MACHINE,MAC_ADDR);
            Serial.println(msg);
            ws.sendTXT(msg);
            break;

        case WStype_TEXT: {
            payload[length] = 0;
            String msg = (char*)payload;
            Serial.println("[WS] Incoming: " + msg);

            JsonDocument inc;
            DeserializationError err = deserializeJson(inc, msg);
            if (err) {
                Serial.println("[WS] JSON PARSE ERROR");
                return;
            }

            String type = inc["type"] | "";
            
            // -----------------------------------------------------------
            // SET MACHINE - outgoing message from Admin: {"type": "set_machine","machine": "NEW_MACHINE_NAME", "mac": "MAC_ADDR"}
            //             - incoming message: {"type": "set_machine","machine": "NEW_MACHINE_NAME"}
            // -----------------------------------------------------------
            if (type == "set_machine") {
                const char* newMachine = inc["machine"];
                ws.disconnect();
                if (newMachine && strlen(newMachine) > 0) {
                    // ---- Save to NVS
                    prefs.putString("machine", newMachine);
                    // ---- Update runtime value
                    strncpy((char*)MACHINE, newMachine, sizeof(MACHINE)-1);
                    MACHINE[sizeof(MACHINE)-1] = '\0';
                    // ---- Reconnect websocket
                    ws.begin(SERVER_IP, WS_PORT, WS_PATH);
                    ws.onEvent(onWebSocketEvent);
                    //re-send identification with new MACHINE name
                    char msg[80];
                    //snprintf(msg, sizeof(msg), "{\"hostname\":\"%s\"}", MACHINE);
                    snprintf(msg, sizeof(msg), "{\"hostname\":\"%s\",\"mac\":\"%s\"}", MACHINE, MAC_ADDR);
                    ws.sendTXT(msg);
                    reloadData();
                    selectedOrder = 0;
                    selectedLine = 0;
                    drawOrders();
                }
                return;
            }

            if (type == "new_order") {
                // Save current screen and selection
                Screen prevScreen = currentScreen;
                int prevSelectedOrder = selectedOrder;
                int prevSelectedLine  = selectedLine;
                int prevSelectedIndex = selectedIndex;
                int prevScrollOffset  = scrollOffset;

                // Reload JSON data
                reloadData();

                JsonArray orders = doc.as<JsonArray>();

                // Clamp selections in case previous order/line was removed
                if (prevSelectedOrder >= orders.size()) prevSelectedOrder = orders.size() - 1;
                if (prevSelectedOrder < 0) prevSelectedOrder = 0;

                if (prevSelectedLine >= orders[prevSelectedOrder]["lines"].as<JsonArray>().size())
                    prevSelectedLine = orders[prevSelectedOrder]["lines"].as<JsonArray>().size() - 1;
                if (prevSelectedLine < 0) prevSelectedLine = 0;

                // Restore selection state
                selectedOrder = prevSelectedOrder;
                selectedLine  = prevSelectedLine;
                selectedIndex = prevSelectedIndex;
                scrollOffset  = prevScrollOffset;

                // Redraw the current screen with updated data
                switch(prevScreen) {
                    case SCR_ORDERS:
                        drawOrders(false);   // false = keep scroll/cursor
                        break;
                    case SCR_LINES:
                        drawLines(false);
                        break;
                    case SCR_STATUS:
                        drawStatusScreen(false);
                        break;
                }
            }

            if (type == "update") {
                JsonArray changes = inc["data"].as<JsonArray>();

                for (JsonObject ch : changes) {
                    int row_id = ch["row_id"].as<int>();
                    int newStatus = ch["status"].as<int>();

                    // Search all orders/lines
                    JsonArray orders = doc.as<JsonArray>();
                    for (int oi = 0; oi < orders.size(); oi++) {
                        JsonArray lines = orders[oi]["lines"].as<JsonArray>();

                        for (int li = 0; li < lines.size(); li++) {
                            JsonObject ln = lines[li];

                            if (ln["row_id"].as<int>() == row_id) {

                                // ---- Update JSON
                                ln["process_status"] = newStatus;

                                // ---- Update Orders screen row (if visible)
                                updateOrderRow(oi);

                                // ---- Update Lines screen row (if visible and correct order)
                                if (oi == selectedOrder) {
                                    updateLineRow(li);
                                }

                                // ---- Handle Status screen
                                if (currentScreen == SCR_STATUS &&
                                    oi == selectedOrder &&
                                    li == selectedLine)
                                {
                                    if (newStatus == 3) {
                                        // Remove the line
                                        lines.remove(li);

                                        if (lines.size() == 0) {
                                            // No more lines → remove order
                                            orders.remove(oi);

                                            // Adjust selection
                                            selectedOrder = min(selectedOrder, (int)orders.size() - 1);
                                            selectedLine = 0;

                                            // Redraw orders screen
                                            drawOrders();
                                        } else {
                                            // Still lines left → go back to lines screen
                                            selectedLine = min(selectedLine, (int)lines.size() - 1);
                                            drawLines();
                                        }
                                    } else {
                                        // Status changed but not complete → refresh options
                                        drawStatusScreen(true);
                                    }
                                }

                                return; // only one row_id per message
                            }
                        }
                    }
                }
                return;
            }

            break;
        }
        default: break;
    }
}

//---------------------------------------------------------
// RELOAD DATA
//---------------------------------------------------------
void reloadData(){
    const int maxRetries = 4;
    bool success = false;
    for (int attempt = 1; attempt <= maxRetries; attempt++) {
        Serial.printf("[WS] Download attempt %d...\n", attempt);
        success = fetchOrders();
        if (success) break;
        Serial.println("[WS] Download failed, retrying...");
        delay(500);
    }
    if (!success) {
        Serial.printf("[WS] JSON download failed after %d attempts.\n",maxRetries);
    }
}

// --------------------------------------------------------
// FETCH ORDERS
// --------------------------------------------------------
bool fetchOrders() {
    snprintf(REST_URL, sizeof(REST_URL),"http://%s/LaborTracker/API/OpenOrdersLite.php?process=%s",SERVER_IP,MACHINE);
    HTTPClient http;
    http.begin(REST_URL);
    int code = http.GET();
    if (code != 200) return false;
    String body = http.getString();
    http.end();
    if (doc.size() > 0) doc.clear();  
    DeserializationError err = deserializeJson(doc, body);
    doc.shrinkToFit();
    return !err;
}

// --------------------------------------------------------
// SCROLLING HELPERS
// --------------------------------------------------------
void updateScrollArrows(int totalItems) {
    // Clear arrow positions
    lcd.setCursor(19, 1); lcd.print(" ");
    lcd.setCursor(19, 3); lcd.print(" ");

    // UP arrow if a previous page exists
    if (scrollOffset > 0) {
        lcd.setCursor(19, 1);
        lcd.write((uint8_t)0);
    }

    // DOWN arrow if a next page exists
    if (scrollOffset + visibleRows < totalItems) {
        lcd.setCursor(19, 3);
        lcd.write((uint8_t)1);
    }
}

// -------------------- MOVE "DOWN" -------------------------
void moveSelectionDown(int totalItems) {
    if (selectedIndex >= totalItems - 1) return;

    // -------------------------------
    // SPECIAL CASE FOR STATUS SCREEN
    // -------------------------------
    if (currentScreen == SCR_STATUS) {
        int prevRow = 2 + selectedIndex;   // options start on row 2
        selectedIndex++;
        int newRow  = 2 + selectedIndex;

        // update cursor only
        lcd.setCursor(0, prevRow);
        lcd.print(" ");
        lcd.setCursor(0, newRow);
        lcd.print(">");

        return; // VERY IMPORTANT — avoids paging logic
    }

    // ------------------------------------
    // NORMAL PAGED SCROLLING (ORDERS/LINES)
    // ------------------------------------
    int prevIndex = selectedIndex;
    selectedIndex++;

    int prevRow = 1 + (prevIndex - scrollOffset);
    int newRow  = 1 + (selectedIndex - scrollOffset);

    if (newRow > visibleRows) {
        // Need new page
        scrollOffset += visibleRows;
        if (scrollOffset + visibleRows > totalItems)
            scrollOffset = totalItems - visibleRows;
        if (scrollOffset < 0)
            scrollOffset = 0;

        refreshListRows(totalItems, getItemCallback);

    } else {
        // Update only cursor
        lcd.setCursor(0, prevRow);
        lcd.print(" ");
        lcd.setCursor(0, newRow);
        lcd.print(">");
    }

    updateScrollArrows(totalItems);
}

// -------------------- MOVE "UP" -------------------------
void moveSelectionUp(int totalItems) {
    if (selectedIndex <= 0) return;

    // -------------------------------
    // SPECIAL CASE FOR STATUS SCREEN
    // -------------------------------
    if (currentScreen == SCR_STATUS) {
        int prevRow = 2 + selectedIndex;   // options start at row 2
        selectedIndex--;
        int newRow  = 2 + selectedIndex;

        // update only the cursor
        lcd.setCursor(0, prevRow);
        lcd.print(" ");
        lcd.setCursor(0, newRow);
        lcd.print(">");

        return; // <<< critical - prevents paging logic
    }

    // ------------------------------------
    // NORMAL PAGED SCROLLING (ORDERS/LINES)
    // ------------------------------------
    int prevIndex = selectedIndex;
    selectedIndex--;

    int prevRow = 1 + (prevIndex - scrollOffset);
    int newRow  = 1 + (selectedIndex - scrollOffset);

    if (newRow < 1) {
        // Need previous page
        scrollOffset -= visibleRows;
        if (scrollOffset < 0) scrollOffset = 0;

        refreshListRows(totalItems, getItemCallback);

        // Cursor appears at bottom row of newly displayed page
        newRow = min(visibleRows, totalItems - scrollOffset);
        lcd.setCursor(0, newRow);
        lcd.print(">");

    } else {
        // Within current page → update cursor only
        lcd.setCursor(0, prevRow);
        lcd.print(" ");
        lcd.setCursor(0, newRow);
        lcd.print(">");
    }

    updateScrollArrows(totalItems);
}

// --------------------------------------------------------
// ROTARY HANDLER
// --------------------------------------------------------
void handleRotary() {
    long pos = rotaryEncoder.readEncoder();

    if (pos != lastEncoderPos) {
        wakeUp();
        int totalItems = 0;
        switch (currentScreen) {
            case SCR_ORDERS: totalItems = doc.as<JsonArray>().size(); break;
            case SCR_LINES:  totalItems = doc[selectedOrder]["lines"].as<JsonArray>().size(); break;
            case SCR_STATUS: totalItems = numStatusOpts; break;
        }

        if (pos > lastEncoderPos) moveSelectionDown(totalItems);
        else if (pos < lastEncoderPos) moveSelectionUp(totalItems);

        lastEncoderPos = pos;
    }
}



// --------------------------------------------------------
// BUTTON HANDLING
// --------------------------------------------------------
void handleButtonPress() {
    static bool prev = false;
    bool pressed = rotaryEncoder.isEncoderButtonDown();

    if (pressed && !prev) buttonPressStart = millis();

    if (!pressed && prev) {
        unsigned long dur = millis() - buttonPressStart;

        // SHORT PRESS = ENTER
        if (dur < 600) {
            switch (currentScreen) {
                case SCR_ORDERS:
                    selectedIndexOrders = selectedIndex; // save Orders cursor
                    scrollOffsetOrders = scrollOffset;   // save Orders scroll
                    selectedOrder = selectedIndex;
                    selectedIndex = selectedIndexLines;  // restore Lines cursor
                    drawLines(true);
                    break;

                case SCR_LINES:
                    selectedIndexLines = selectedIndex;  // save Lines cursor
                    scrollOffsetLines = scrollOffset;    // save Lines scroll
                    selectedLine = selectedIndex;
                    selectedIndex = selectedIndexStatus; // restore Status cursor
                    drawStatusScreen(true);
                    break;

                case SCR_STATUS: {
                    // Apply the selected status to the selected line
                    int newStatus = statusOpts[selectedIndex].code;
                    int rowid = doc[selectedOrder]["lines"][selectedLine]["row_id"].as<int>();
                    // Apply the selected status to the selected line
                    doc[selectedOrder]["lines"][selectedLine]["process_status"] = newStatus;

                    // Construct JSON string
                    char buf[64];
                    snprintf(buf, sizeof(buf), "{\"type\":\"update\",\"data\":[{\"row_id\":%d,\"status\":%d}]}", rowid, newStatus);
                    ws.sendTXT(buf);
                    Serial.printf("[WS sending] %s\n",buf);

                    // Return to Lines screen
                    selectedIndexStatus = selectedIndex; // save Status cursor
                    scrollOffsetStatus = scrollOffset;   // save Status scroll
                    drawLines(true);
                    selectedIndex = selectedIndexLines;  // restore Lines cursor
                    scrollOffset = scrollOffsetLines;    // restore scroll
                    break;
                }
            }
        }
        // LONG PRESS = ESC
        else {
            switch (currentScreen) {
                case SCR_LINES:
                    drawOrders(false);
                    break;
                case SCR_STATUS:
                    drawLines(false);
                    break;
                default:
                    break;
            }
        }
    }

    prev = pressed;
}
// --------------------------------------------------------
// SETUP
// --------------------------------------------------------
void setup() {
    Serial.begin(115200);

    prefs.begin("config", false);   // namespace "config", RW mode
    String savedMachine = prefs.getString("machine", "");
    if (savedMachine.length() > 0) {
        strncpy(MACHINE, savedMachine.c_str(), sizeof(MACHINE) - 1);
        MACHINE[sizeof(MACHINE) - 1] = '\0';
    } else {
        // Optional default
        strncpy(MACHINE, "sawing", MACHINE_MAX_LEN - 1);
    }

    pinMode(ROTARY_ENCODER_A_PIN, INPUT_PULLUP);
    pinMode(ROTARY_ENCODER_B_PIN, INPUT_PULLUP);

    rotaryEncoder.begin();
    rotaryEncoder.setup(readEncoderISR);
    rotaryEncoder.setBoundaries(0, 100, true); //minValue, maxValue, circleValues true|false (when max go to min and vice versa)
    rotaryEncoder.disableAcceleration();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) delay(200);

    WiFi.macAddress().toCharArray(MAC_ADDR, MAC_ADDR_LEN);
    delay(200);

    ws.begin(SERVER_IP, WS_PORT, WS_PATH);
    ws.onEvent(onWebSocketEvent);
    ws.setReconnectInterval(5000);

    fetchOrders();

    delay(200);

    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.createChar(0, upArrow);
    lcd.createChar(1, downArrow);

    drawOrders();
    lastActivity = millis();
}


unsigned long lastLoopTime = 0;
const unsigned long LOOP_INTERVAL = 10;  // 10 ms
// --------------------------------------------------------
// LOOP
// --------------------------------------------------------
void loop() {
    unsigned long now = millis();

    // Run the loop tasks every LOOP_INTERVAL ms
    if (now - lastLoopTime >= LOOP_INTERVAL) {
        lastLoopTime = now;

        // 1️ Handle WebSocket messages
        ws.loop();

        // 2️ Handle rotary encoder movement
        handleRotary();

        // 3️ Handle button presses
        handleButtonPress();

        // 4️ Check for sleep / backlight timeout
        checkSleep();
    }

    // No delay() here → non-blocking
}
