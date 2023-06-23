#define ARDUINOJSON_DECODE_UNICODE 0  // disable unicode decoding (saves program space)
#include <ArduinoJson.h>              // https://arduinojson.org/
#include <WiFi.h>
#include <HTTPClient.h>
#include "fabgl.h"                    // http://www.fabglib.org/index.html
#include "AiEsp32RotaryEncoder.h"     // https://github.com/igorantolic/ai-esp32-rotary-encoder/blob/master/README.md
#include <WebSocketsClient.h>         // https://github.com/Links2004/arduinoWebSockets
#include "config.h"
#include <vector>
#include <algorithm>
#include <unordered_set>

//use for TTGO VGA32 board
#define ROTARY_ENCODER_A_PIN 34         //DT
#define ROTARY_ENCODER_B_PIN 39         //CLK
#define ROTARY_ENCODER_BUTTON_PIN 13    //SW

//use for Bitluni board
//#define ROTARY_ENCODER_A_PIN 39         //DT
//#define ROTARY_ENCODER_B_PIN 36         //CLK
//#define ROTARY_ENCODER_BUTTON_PIN 35    //SW

#define ROTARY_ENCODER_STEPS 4

//colors
#define BG_BLK_FG_WHITE "\e[40;97m"   // background: black, foreground: white
#define BG_BLK_FG_GRN   "\e[40;92m"   // background: black, foreground: green
#define BG_BLK_FG_RED   "\e[40;91m"   // background: black, foreground: red
#define BG_BLK_FG_YEL   "\e[40;93m"   // background: black, foreground: yellow
#define BG_BLK_FG_BLU   "\e[40;94m"   // background: black, foreground: blue
#define BG_BLK_FG_MAG   "\e[40;95m"   // background: black, foreground: magenta
#define BG_BLK_FG_CYAN  "\e[40;96m"   // background: black, foreground: cyan

#define BG_WHITE_FG_BLK "\e[47;30m"   // background: white, foreground: black
#define BG_WHITE_FG_BLU "\e[47;94m"  // background: white, foreground: blue
#define BG_WHITE_FG_RED "\e[47;91m"   // background: white, foreground: red
#define BG_RED_FG_BLK   "\e[101;30m"  // background: red, foreground: black
#define BG_RED_FG_WHITE "\e[101;97m"  // background: red, foreground: white
#define BG_GRN_FG_BLK   "\e[102;30m"  // background: green, foreground: black
#define BG_GRN_FG_WHITE "\e[102;97m"  // background: green, foreground: white
#define BG_YEL_FG_BLK   "\e[103;30m"  // background: yellow, foreground: black
#define BG_BLU_FG_WHITE "\e[104;97m"  // background: blue, foreground: white
#define BG_BLU_FG_BLK   "\e[104;30m"  // background: blue, foreground: black
#define BG_MAG_FG_BLK   "\e[105;30m"  // background: magenta, foreground: black
#define BG_MAG_FG_WHITE "\e[105;97m"  // background: magenta, foreground: white
#define BG_CYAN_FG_BLK  "\e[106;30m"  // background: cyan, foreground: black

#define INVERSE         "\e[7m"       // inverse current color
#define BLINK           "\e[5m"       //  blink 
#define NORMAL          "\e[0m"       // normal state


fabgl::VGATextController DisplayController;
fabgl::Terminal          Terminal;

// Terminal process focus
String process = PROCESS;
String servr = SERVER;
String ws_server = WS_SERVER;
String load_orders_endpoint = LOAD_ORDERS_ENDPOINT;
String update_db_endpoint = UPDATE_DB_ENDPOINT;


unsigned long pressedTime;
unsigned long releasedTime;
unsigned int currentLineNumber;
unsigned int currentRowID;
unsigned long currentOrderNum;
String currentStatus;
unsigned int orderCurrentPage = 0;
unsigned int lineCurrentPage = 0;
unsigned int init_cursorV = 0;  //where initial item selection begins - changes on each screen
unsigned int cursorV = 0;
unsigned int cursorH = 0;
unsigned int cursorPos = 4;
unsigned int OrderSelectRowCount = 0;
unsigned int LineSelectRowCount = 0;
unsigned int rowCount = 0;
unsigned int orderIndex = 0;
int enc_prev_val = 0;
unsigned int orderPages = 0;
unsigned int linePages = 0;
std::unordered_set<unsigned int> active_array;


const char* stopped_status = "Stopped";
const char* running_status = "Running";
const char* not_started_status = "Not Started";
const char* new_order = "new order";

// WiFi
const char ssid[] = WIFI_SSID;                     // Network Name
const char passwd[] = WIFI_PASSWD;               // Network Password
WiFiClient client;            // Use this for WiFi instead of EthernetClient
HTTPClient http;
WebSocketsClient webSocket;

// make space for JSON document
DynamicJsonDocument doc(80000);
//StaticJsonDocument<80000> doc;

//create static JSON document
StaticJsonDocument<30> message;

// setup the rotary encoder
AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);

//rotary encoder ISR
void IRAM_ATTR readEncoderISR(){
    rotaryEncoder.readEncoder_ISR();
}

enum class State {
  OrderSelect,
  LineSelect,
  StatusSelect
};


State state = State::OrderSelect;

void post(const JsonDocument& post_req ){
  http.begin("http://" + servr + update_db_endpoint);
  http.addHeader("Content-Type", "application/json");
  String sJsonPost;
  serializeJson(post_req,sJsonPost);
  unsigned int httpResponseCode = http.POST(sJsonPost);
   Serial.printf("response code: %d\n",httpResponseCode);
   Serial.println(sJsonPost);
  if (httpResponseCode == 200){
    active_array.clear();
    state = State::OrderSelect;
    OrderSelectScreen(0,true);
  } else {
    Serial.println("no dice!");
    Terminal.printf("\e[%d;%dH %s",34,1,"Error posting to DB");
    delay(5000);
    state = State::OrderSelect;
    OrderSelectScreen(0,false);
  }
  http.end();
}

void loadOrders(){
  http.useHTTP10(true);
  http.begin("http://" + servr + load_orders_endpoint + "?process=" + process);
  http.GET();
  
//  DeserializationError error = deserializeJson(doc, http.getStream(),DeserializationOption::Filter(filter));
  doc.clear();
  DeserializationError error = deserializeJson(doc, http.getStream());
  if (error) {
    Terminal.printf("deserializeJson() failed: %s",error.f_str());
    delay(5000);
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }
  orderPages = doc.size();
  http.end();
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t payload_length) {
  switch(type) {
    case WStype_DISCONNECTED:
//      Terminal.printf("\e[%d;%dH %s",34,1,"WS -");
      Serial.println("WebSock Disconnected!");
      break;
    case WStype_CONNECTED:
//      Terminal.printf("\e[%d;%dH %s",34,1,"WS *");
      Serial.println("WebSock Connected!   ");
      break;
    case WStype_TEXT:
      {
      String ws_str = reinterpret_cast<char *>(payload);
        if (ws_str.length() == 9) {        // string = "new order"
          state = State::OrderSelect;
          OrderSelectScreen(0,true);
        } else {
        String newStatus = ws_str.substring(ws_str.indexOf(",")+1,ws_str.length());
          if (newStatus == "complete_job") {
            state = State::OrderSelect;
            OrderSelectScreen(0,false);            
          } else {
            unsigned int rowId = ws_str.substring(0,ws_str.indexOf(",")).toInt();
            //Serial.println("no crash yet!");
            if (newStatus == "start_job" || newStatus == "resume_job"){
              newStatus = "Running";
            } else if (newStatus == "stop_job"){
              newStatus = "Stopped";
            }
            for (int k = 0; k < doc.size(); k++ ){
              for (int i = 0; i < doc[k].size(); i++){
                linePages = doc[k][i]["lines"].size();
                for (int j = 0; j < linePages; j++){
                  for (int l = 0; l < doc[k][i]["lines"][j].size(); l++){
                    if (doc[k][i]["lines"][j][l]["row_id"] == rowId){
                      doc[k][i]["lines"][j][l]["process_status"] = newStatus;
                      switch (state){
                        case State::OrderSelect:
                          //screenHeader();
                          printLine(i,0);
                          Terminal.printf("\e[%d;%dH",cursorV,1);
                          break;
                        case State::LineSelect:
                          if (currentOrderNum == doc[k][i]["order_num"]){
                            //screenHeader();
                            printLine(j,i);
                            Terminal.printf("\e[%d;%dH",cursorV,1);
                          }
                          break;
                        case State::StatusSelect:
                          if (currentRowID == doc[k][i]["lines"][j][l]["row_id"]) {
                            state = State::LineSelect;
                            LineSelectScreen(lineCurrentPage,i);
                            state = State::StatusSelect;
                            currentStatus = newStatus;
                            StatusSelectScreen(currentRowID,currentLineNumber,newStatus);
                          }
                          break;
                        }
                    }
                  }
                }
              }
            }
          }
      activeLinesDisplay();
      }
    }
    break;
  }
}


void setup() {

  Serial.begin(115200);
  
  // initalize rotary encoder
  pinMode(ROTARY_ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ROTARY_ENCODER_B_PIN, INPUT_PULLUP);
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, 100, true); //minValue, maxValue, circleValues true|false (when max go to min and vice versa)
  rotaryEncoder.disableAcceleration();
  
  // initalize display and virtual terminal
  DisplayController.begin();                //use for TTGO VGA32 board
  //DisplayController.begin(GPIO_NUM_14,GPIO_NUM_19,GPIO_NUM_27,GPIO_NUM_32,GPIO_NUM_33);     //use for Bitluni board
  DisplayController.setResolution();
  
  Terminal.begin(&DisplayController,80,100);
  
  // initalize WiFi
   WiFi.begin(ssid, passwd);

       while (WiFi.status() != WL_CONNECTED)
    {
      delay(500);
      Terminal.write(".");
    }
  IPAddress broadCast = WiFi.localIP();
  Terminal.printf("%d.%d.%d.%d  ",broadCast[0],broadCast[1],broadCast[2],broadCast[3] );
  delay(800);
  
  if (!client.connect(server, 80)) {
    Terminal.write("Connection failed");
    delay(1000);
    return;
  }

  Terminal.write("Connected!");
  delay(500);
  
  webSocket.begin(ws_server, 8000, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);

  Terminal.enableCursor(true);
  OrderSelectScreen(0,true);
}

void draw(){
    switch (state) {
      case State::OrderSelect:
        OrderSelectScreen(orderCurrentPage,false);
        break;
      case State::LineSelect:
        LineSelectScreen(lineCurrentPage,orderIndex);
        break;
    }
}


void navigate(int n){
  switch (state) {
    case State::OrderSelect:
      if (cursorV + n < init_cursorV){
        if ( orderCurrentPage !=0 && ((orderCurrentPage - 0) | ((orderPages) - orderCurrentPage)) >= 0) {
          //Serial.println("decrimenting orderCurrentPage count");
          orderCurrentPage--;
          draw();
          cursorV = 32;
        } else {
          cursorV = init_cursorV;
        }
      } else if (cursorV + n >= init_cursorV + OrderSelectRowCount) {
        if (orderCurrentPage < orderPages-1) {  // move to next page
          orderCurrentPage++;
          draw();
          cursorV = init_cursorV;
        } else if (orderCurrentPage == orderPages){   // we are at last page - lock cursor
          cursorV = init_cursorV + OrderSelectRowCount-1;
        }
      } else {
        cursorV = cursorV + n;
      }
      Terminal.printf("\e[%d;%dH",cursorV,cursorH);   // move cursor to v,h
      break;
    case State::LineSelect:
      if (cursorV + n < init_cursorV) {
        if ( lineCurrentPage !=0 && ((lineCurrentPage - 0) | ((linePages) - lineCurrentPage)) >= 0) {
          lineCurrentPage--;
          draw();
          cursorV = 32;
        } else {
          cursorV = init_cursorV;
        }
      } else if (cursorV + n >= init_cursorV + LineSelectRowCount) {
        if (lineCurrentPage < linePages-1) {  // move to next page
          lineCurrentPage++;
          draw();
          cursorV = init_cursorV;
        } else if (lineCurrentPage == linePages-1){   // we are at last page - lock cursor
          cursorV = init_cursorV + LineSelectRowCount;
        }
      } else {
        cursorV = cursorV + n;
      }
      Terminal.printf("\e[%d;%dH",cursorV,cursorH);   // move cursor to v,h
      break;
    case State::StatusSelect:
      cursorV = cursorV + n; 
      if (cursorV > init_cursorV + rowCount-1){
        cursorV = init_cursorV + rowCount-1;
      }
      if (cursorV < init_cursorV){
        cursorV = init_cursorV;
      }
      Terminal.printf("\e[%d;%dH",cursorV,cursorH);   // move cursor to v,h 
      break;
  }    
}

void checkForActive(){
  active_array.clear();
  const char* lineStatus;
  unsigned int rowid = 0;
  for (int i=0; i < orderPages; i++){
    for (int j=0; j < doc[i].size(); j++){
      for (int k=0; k < doc[i][j]["lines"].size(); k++ ){
        for (int l=0; l < doc[i][j]["lines"][k].size(); l++){
          lineStatus = doc[i][j]["lines"][k][l]["process_status"];
          rowid = doc[i][j]["lines"][k][l]["row_id"];
          if (strcmp(lineStatus,running_status) == 0){
            //Serial.printf("rowid: %d, status: %s\n",rowid,lineStatus);
            active_array.insert(rowid);
          }
        }
      }
    }
  }
}

String getColor(unsigned int n){
  String color;
  const char* lineStatus;
  std::vector<String> status_array;
  unsigned int number_of_lines = 0;
  status_array.clear();
  
  if (linePages > 1) {
    for (int i = 0; i < linePages; i++){
      number_of_lines = doc[orderCurrentPage][n]["lines"][i].size();
      for (int j = 0; j < number_of_lines; j++){
        lineStatus = doc[orderCurrentPage][n]["lines"][i][j]["process_status"];
        status_array.push_back(lineStatus);
      }
    }
  } else {
    number_of_lines = doc[orderCurrentPage][n]["lines"][lineCurrentPage].size();
    for (int i = 0; i < number_of_lines; i++){
      lineStatus = doc[orderCurrentPage][n]["lines"][lineCurrentPage][i]["process_status"];
      status_array.push_back(lineStatus);
    }
  }
  if (std::find(status_array.begin(), status_array.end(), "Running") != status_array.end()){
      color = "\e[40;92m";    // background: black, foreground: green
      status_array.clear();
      return color;
    } else if (std::find(status_array.begin(), status_array.end(), "Stopped") != status_array.end()){
      color = "\e[40;91m";    // background: black, foreground: red
      status_array.clear();
      return color;
    } else {
      color = "\e[40;96m";    // background: black, foreground: cyan
      status_array.clear();
      return color;
    }
  }


void printLine(int n, unsigned int index){
  Terminal.write(NORMAL);
  switch (state) {
    case State::OrderSelect:
      {
      unsigned long orderNum = doc[orderCurrentPage][n]["order_num"];
      const char* custName = doc[orderCurrentPage][n]["customer_name"];
      const char* po = doc[orderCurrentPage][n]["po"];
      const char* salesRep = doc[orderCurrentPage][n]["sales_rep"];
      const char* dateDue = doc[orderCurrentPage][n]["date_due"];
      String orderColor = getColor(n);
      Terminal.printf("\e[%d;1H",n+init_cursorV);
      Terminal.printf("%s %-7d%s%c%s%-25.25s%s%c%s%-18.18s%s%c%s%-15.15s%s%c%s%-10s",
                      orderColor,orderNum,BG_BLK_FG_WHITE,(char)179,
                      orderColor,custName,BG_BLK_FG_WHITE,(char)179,
                      orderColor,po,BG_BLK_FG_WHITE,(char)179,
                      orderColor,salesRep,BG_BLK_FG_WHITE,(char)179,
                      orderColor,dateDue);
      Terminal.write(BG_BLK_FG_WHITE);
      }
      break;
    case State::LineSelect:
      {
      unsigned int rowid = doc[orderCurrentPage][index]["lines"][lineCurrentPage][n]["row_id"];
      unsigned int lineNum = doc[orderCurrentPage][index]["lines"][lineCurrentPage][n]["line_number"];
      unsigned int qty = doc[orderCurrentPage][index]["lines"][lineCurrentPage][n]["quantity"];
      const char* uom = doc[orderCurrentPage][index]["lines"][lineCurrentPage][n]["uom"];
      const char* desc = doc[orderCurrentPage][index]["lines"][lineCurrentPage][n]["description"];
      float w = doc[orderCurrentPage][index]["lines"][lineCurrentPage][n]["width"];
      float l = doc[orderCurrentPage][index]["lines"][lineCurrentPage][n]["length"];
      const char* stat = doc[orderCurrentPage][index]["lines"][lineCurrentPage][n]["process_status"];
      String lineColor;
      
      Terminal.printf("\e[%d;1H",n+init_cursorV);

      if (strcmp(stat,running_status) == 0){
        lineColor = "\e[40;92m";    // background: black, foreground: green
      } else if (strcmp(stat,stopped_status) == 0){
        lineColor = "\e[40;91m";    // background: black, foreground: red
      } else {
        lineColor = "\e[40;96m";    // background: black, foreground: cyan
      }

      Terminal.printf("%s %3d%s%c%s%5d%s%c%s%-4.4s%s%c%s%-34.34s%s%c%s%-8.3f%s%c%s%-8.3f%s%c%s%-11.11s",
                      lineColor,lineNum,BG_BLK_FG_WHITE,(char)179,
                      lineColor,qty,BG_BLK_FG_WHITE,(char)179,
                      lineColor,uom,BG_BLK_FG_WHITE,(char)179,
                      lineColor,desc,BG_BLK_FG_WHITE,(char)179,
                      lineColor,w,BG_BLK_FG_WHITE,(char)179,
                      lineColor,l,BG_BLK_FG_WHITE,(char)179,
                      lineColor,stat);
      Terminal.write(BG_BLK_FG_WHITE);
      }
      break;
  }
}


void activeLinesDisplay(){
  checkForActive();
  Terminal.write("\e[1;65H");   // move cursor to 1,65
  Terminal.printf("%sACTIVE LINES:%s%3d",BG_WHITE_FG_BLU,BG_WHITE_FG_RED,active_array.size());  // show number of active lines
  Terminal.write(NORMAL);
}

void screenHeader(){
  Terminal.write(NORMAL);
  switch(state){
    case State::OrderSelect:
      Terminal.clear();
      Terminal.write(BG_BLK_FG_WHITE); // background: black, foreground: white
      Terminal.write("\e[1;1H");   // move cursor to 1,1 
      {
      init_cursorV = 4;
      cursorV = 4;                   //set cursor position for item selection
      cursorH = 1;
      Terminal.printf("%s%s",BG_WHITE_FG_BLK,"                               SELECT ORDER                                     ");
      Terminal.write(BG_BLK_FG_WHITE);
      Terminal.printf("%s%c%s%c%s%c%s%c%s"," ORDER #",(char)179,"        CUSTOMER         ",(char)179,"        PO        ",(char)179,"   SALES REP   ",(char)179," DATE DUE ");
      Terminal.printf("%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%",
                     (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)197,
                     (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)197,
                     (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)197,
                     (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)197,
                     (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196);
      //============================================================================================================================
      
      for (int i=0; i < 34; i++){
        //Terminal.write("\e[4;9H");    // move cursor to 4,9
        Terminal.printf("\e[%d;9H",4+i);
        Terminal.printf("%c",(char)179);
        Terminal.printf("\e[%d;35H",4+i);
        Terminal.printf("%c",(char)179);
        Terminal.printf("\e[%d;54H",4+i);
        Terminal.printf("%c",(char)179);
        Terminal.printf("\e[%d;70H",4+i);
        Terminal.printf("%c",(char)179);
      }
      Terminal.write("\e[34;1H");   // move cursor to 34,1
      Terminal.write(BG_WHITE_FG_BLK);
      Terminal.printf("                               PAGE %d OF %d                                      ",orderCurrentPage+1,orderPages);    
      Terminal.write(BG_BLK_FG_WHITE);
      Terminal.printf("\e[%d;%dH",init_cursorV,cursorH);   // move cursor to 4,1
      }
      break;
    case State::LineSelect:
      Terminal.clear();
      Terminal.write(BG_BLK_FG_WHITE); // background: black, foreground: white
      Terminal.write(BG_WHITE_FG_BLK);
      Terminal.write("\e[1;1H");   // move cursor to 1,1 
      {
      init_cursorV = 5;
      cursorV = 5;                   //set cursor position for item selection
      cursorH = 1;
      const char* custName = doc[orderCurrentPage][orderIndex]["customer_name"];
      const unsigned long orderNum = doc[orderCurrentPage][orderIndex]["order_num"];
      Terminal.write(BG_BLK_FG_WHITE);
      Terminal.printf("%s  SELECTED ORDER: %-7d %-54s\n",BG_WHITE_FG_BLU,orderNum,custName);
      Terminal.write(BG_WHITE_FG_BLK);  // background: white, foreground: black
      Terminal.write("                                SELECT  LINE                                    ");
      Terminal.write(BG_BLK_FG_WHITE);
      Terminal.printf("%s%c%s%c%s%c%s%c%s%c%s%c%s","LN #",(char)179," QTY ",(char)179,"UOM ",(char)179,"            DESCRIPTION           ",(char)179," WIDTH  ",(char)179," LENGTH ",(char)179,"   STATUS  ");
      Terminal.printf("%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%",
      (char)196,(char)196,(char)196,(char)196,(char)197,
      (char)196,(char)196,(char)196,(char)196,(char)196,(char)197,
      (char)196,(char)196,(char)196,(char)196,(char)197,
      (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,
      (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)197,
      (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)197,
      (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)197,
      (char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196,(char)196);
      //=========================================================================================================================
      Terminal.write("\e[34;1H");   // move cursor to 34,1
      Terminal.write(BG_WHITE_FG_BLK);
      Terminal.printf("                               PAGE %d OF %d                                      ",lineCurrentPage+1,linePages);    
      Terminal.write(BG_BLK_FG_WHITE);
      Terminal.printf("\e[%d;%dH",init_cursorV,cursorH);   // move cursor to 5,1
      }
      break;
    case State::StatusSelect:
      init_cursorV = 15;
      cursorV = 15;
      cursorH = 26;
      Terminal.write(BG_BLK_FG_WHITE); // background: black, foreground: white
      Terminal.write("\e[10;25H");   // move cursor to 25,10
      Terminal.printf("%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c",
      (char)201,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,
      (char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)187);
      //Terminal.write("+============================+");
      Terminal.write("\e[11;25H");   // move cursor to 25,11
      Terminal.printf("%c%28.28s%c",(char)186," ",(char)186);
      Terminal.write("\e[12;25H");   // move cursor to 25,12
      Terminal.printf("%c%28.28s%c",(char)186," ",(char)186);
      Terminal.write("\e[13;25H");   // move cursor to 25,13
      Terminal.printf("%c%28.28s%c",(char)186," ",(char)186);
      Terminal.write("\e[14;25H");   // move cursor to 25,14
      Terminal.printf("%c%28.28s%c",(char)204," ",(char)185);
      Terminal.write("\e[15;25H");   // move cursor to 25,15
      Terminal.printf("%c%28.28s%c",(char)186," ",(char)186);
      Terminal.write("\e[16;25H");   // move cursor to 25,16
      Terminal.printf("%c%28.28s%c",(char)186," ",(char)186);
      Terminal.write("\e[17;25H");   // move cursor to 25,17
      Terminal.printf("%c%28.28s%c",(char)186," ",(char)186);
      Terminal.write("\e[18;25H");   // move cursor to 25,18
      Terminal.printf("%c%28.28s%c",(char)186," ",(char)186);
      Terminal.write("\e[19;25H");   // move cursor to 25,19
      Terminal.printf("%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c",
      (char)200,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,
      (char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)188);
      break;
  }
}

void OrderSelectScreen(int curPage, bool reload){
  lineCurrentPage = 0;
  if (curPage == 0 && reload){
    Serial.println("orders reloading....");
    loadOrders();
  }
  screenHeader();
  OrderSelectRowCount = doc[orderCurrentPage].size();
  for (int i=0; i < OrderSelectRowCount; i++) {
    printLine(i,0);
    }
  activeLinesDisplay();
  Terminal.printf("\e[%d;%dH",init_cursorV,cursorH);   // move cursor to 4,1
}

void LineSelectScreen(int curPage,int index){
  linePages = doc[orderCurrentPage][index]["lines"].size();
  screenHeader();
  LineSelectRowCount = doc[orderCurrentPage][index]["lines"][curPage].size();
  for (int i=0; i < LineSelectRowCount; i++) {
    printLine(i,index);
  }
  if (linePages <= 1 || curPage == linePages-1){
    Terminal.write("\r\n");
    Terminal.write(BG_YEL_FG_BLK);
    Terminal.write("RETURN TO ORDER SELECT");
  }
  activeLinesDisplay();
  Terminal.write(BG_BLK_FG_WHITE);
  Terminal.printf("\e[%d;%dH",init_cursorV,cursorH);   // move cursor to 4,1
  
}

void StatusSelectScreen(int rowID, int lineNumber, String lineStat){
  currentStatus = lineStat;
  screenHeader();
  Terminal.write("\e[11;26H");   // move cursor to 26,11
  Terminal.write(BG_WHITE_FG_BLK);
  Terminal.write("     UPDATE LINE STATUS     ");
  Terminal.write(BG_BLK_FG_WHITE);
  Terminal.write(BG_BLK_FG_WHITE); // background: black, foreground: white
  Terminal.write("\e[12;26H");   // move cursor to 26,12
  Terminal.printf("Line #: %d",lineNumber);
  Terminal.write("\e[13;26H");   // move cursor to 26,13

  if (lineStat == "Stopped") {                  // line is stopped - options are: resume or complete
    rowCount = 3;
    Terminal.printf("%s%s%s%s","Current Status: ",BG_BLK_FG_RED,"Stopped",BG_BLK_FG_WHITE);
    Terminal.write("\e[14;26H");   // move cursor to 26,14
    //Terminal.write(BG_WHITE_FG_BLK);
    Terminal.printf("%c%c%c%c%c%c%c%c%c%c%s%c%c%c%c%c%c%c%c%c",
    (char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205," OPTIONS ",(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205);
    Terminal.write(BG_BLK_FG_WHITE);
    Terminal.printf("\e[%d;26H",15);   // move cursor to 26,15
    Terminal.write(BG_GRN_FG_WHITE); // background: green, foreground: WHITE
    Terminal.write("Resume");
    Terminal.printf("\e[%d;26H",16);   // move cursor to 26,17
    Terminal.write(BG_MAG_FG_WHITE); // background: magenta, foreground: white
    Terminal.write("Complete");
    Terminal.printf("\e[%d;26H",17);   // move cursor to 26,18
    Terminal.write(BG_YEL_FG_BLK); // background: yellow, foreground: black
    Terminal.write("RETURN TO LINE SELECT");
    Terminal.write(BG_BLK_FG_WHITE); // background: black, foreground: white
  }
  if (lineStat == "Running") {                  // line is running - options are: stop or complete
    rowCount = 3;
    Terminal.printf("%s%s%s%s","Current Status: ",BG_BLK_FG_GRN,"Running",BG_BLK_FG_WHITE);
    Terminal.write("\e[14;26H");   // move cursor to 26,14
    //Terminal.write(BG_WHITE_FG_BLK);
    //Terminal.write("========== OPTIONS =========");
    Terminal.printf("%c%c%c%c%c%c%c%c%c%c%s%c%c%c%c%c%c%c%c%c",
    (char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205," OPTIONS ",(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205);
    Terminal.write(BG_BLK_FG_WHITE);
    Terminal.printf("\e[%d;26H",15);   // move cursor to 26,16
    Terminal.write(BG_RED_FG_WHITE); // background: red, foreground: white
    Terminal.write("Stop");
    Terminal.printf("\e[%d;26H",16);   // move cursor to 26,17
    Terminal.write(BG_MAG_FG_WHITE); // background: magenta, foreground: white
    Terminal.write("Complete");
    Terminal.printf("\e[%d;26H",17);   // move cursor to 26,18
    Terminal.write(BG_YEL_FG_BLK); // background: yellow, foreground: black
    Terminal.write("RETURN TO LINE SELECT");
    Terminal.write(BG_BLK_FG_WHITE); // background: black, foreground: white
  }  
  if (lineStat == "Not Started") {                  // line is not started - options are: start
    rowCount = 2;
    Terminal.printf("%s%s%s%s","Current Status: ",BG_BLK_FG_CYAN,"Not Started",BG_BLK_FG_WHITE);
    Terminal.write("\e[14;26H");   // move cursor to 26,14
    //Terminal.write(BG_WHITE_FG_BLK);
    Terminal.printf("%c%c%c%c%c%c%c%c%c%c%s%c%c%c%c%c%c%c%c%c",
    (char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205," OPTIONS ",(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205,(char)205);
    Terminal.write(BG_BLK_FG_WHITE);
    Terminal.printf("\e[%d;26H",15);   // move cursor to 26,15
    Terminal.write(BG_BLK_FG_GRN); // background: black, foreground: green
    Terminal.write("Start");
    Terminal.printf("\e[%d;26H",16);   // move cursor to 26,18
    Terminal.write(BG_YEL_FG_BLK); // background: yellow, foreground: black
    Terminal.write("RETURN TO LINE SELECT");
    Terminal.write(BG_BLK_FG_WHITE); // background: black, foreground: white
  }
  Terminal.printf("\e[%d;%dH",init_cursorV,cursorH);   // move cursor to 26,15 
}

void loop() {
  String websock_message;
  if (rotaryEncoder.encoderChanged()){
    if (rotaryEncoder.readEncoder() < enc_prev_val){
      enc_prev_val = rotaryEncoder.readEncoder();
      navigate(-1);
    }
    if (rotaryEncoder.readEncoder() > enc_prev_val){
      enc_prev_val = rotaryEncoder.readEncoder();
      navigate(1);
    }
  }
  if (rotaryEncoder.isEncoderButtonClicked()){
    //Serial.printf("cursor pos: %d\n",cursorV);
    switch (state){
      case State::OrderSelect:
        {
        orderIndex = cursorV-init_cursorV;
        currentOrderNum = doc[orderCurrentPage][orderIndex]["order_num"];
        //Serial.printf("orderIndex: %d, orderNum: %d\n", orderIndex,currentOrderNum);
        state = State::LineSelect;
        LineSelectScreen(lineCurrentPage,orderIndex);
        }
        break;
      case State::LineSelect:
        {
        if (doc[orderCurrentPage][orderIndex]["lines"][lineCurrentPage].size() <= cursorV-init_cursorV ){        //RETURN TO ORDER SELECT option
          state = State::OrderSelect;
          draw();
          } else {
            currentRowID = doc[orderCurrentPage][orderIndex]["lines"][lineCurrentPage][cursorV-init_cursorV]["row_id"];
            currentLineNumber = doc[orderCurrentPage][orderIndex]["lines"][lineCurrentPage][cursorV-init_cursorV]["line_number"];
            String lStatus = doc[orderCurrentPage][orderIndex]["lines"][lineCurrentPage][cursorV-init_cursorV]["process_status"];   
            state = State::StatusSelect;
            StatusSelectScreen(currentRowID,currentLineNumber,lStatus);
          }
        }
        break; 
      case State::StatusSelect:
       message.clear();
       switch(cursorV-init_cursorV){
        case 0:            // first option
          if (currentStatus == "Running"){  
            websock_message = String(currentRowID)+",stop_job";
            message[String(currentRowID)] = "stop_job";
            webSocket.sendTXT(websock_message);
            post(message);
          }
          if (currentStatus == "Stopped" || currentStatus == "Not Started"){
            websock_message = String(currentRowID)+",start_job";
            message[String(currentRowID)] = "start_job";
            webSocket.sendTXT(websock_message);
            post(message);         
          }
          break;
        case 1:           // second option
          if (currentStatus == "Not Started"){   //RETURN TO LINE SELECT option
            state = State::LineSelect;
            draw();
          }
          else {
            //lineCurrentPage = 0;
            websock_message = String(currentRowID)+",complete_job";
            message[String(currentRowID)] = "complete_job";
            webSocket.sendTXT(websock_message);
            post(message);
          }
          break;
        case 2:           // RETURN TO LINE SELECT option
          state = State::LineSelect;
          draw();
          break;
       }
    } 
  }
    webSocket.loop();
}
