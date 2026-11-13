/*************************************************************************
   PROJECT: Edgehax EDX115 - 4G/LTE Industrial Gateway (Quectel EC200U + ESP32-S3)
            Sample Code 
   AUTHOR: Edgehax
   CREATED: 13/10/2026
   COPYRIGHT: Edgehax @MIT license for usage on Edgehax boards
   VERSION: 1.0.0

   FUNCTION: 4G testing with HTTP call to a url and send SMS.
   DESC: This script will connect using 4G sim, make an HTTPS POST call to a
   test url setup on Pipedream and send an SMS. You need to set your own
   Pipedream url and configure the same in this code to be able to receive a call.

   SIMCARD: 4G sim cards from Airtel/Vodafone/Jio/BSNL can be used.
            SIM1 = J5 (label on PCB). To test HTTP request ensure you have data plan and sms pack.

   IMPORTANT: Configure the APN accordingly as per your Sim provider.

   TODO: (Before you upload the code to Edgehax board)
   1) Add phone number(s)
   2) Change APN as per your Sim provider
   3) Setup a test http URL in pipedream.com
   4) Arduino IDE -> Tools: Board "ESP32S3 Dev Module"
   5) Serial Monitor: 115200 baud, "Both NL & CR" (for manual AT commands)

   HISTORY:
   ------------------------------------------
   Date      | Firmware Version | Comments
   ------------------------------------------
   13/10/2026 -    1.0.0       -    Initial release of EDX115 sample script.

*************************************************************************/

#include <Arduino.h>
#include <ArduinoJson.h>   // Lib version 7.x

/*************************************************************************
  PHONE: Enter the phone number (international format) to receive the SMS
*************************************************************************/
#define SMS_TARGET1  "+91xxxxxxxxxx"   // Enter your phone number
#define SMS_MESSAGE  "Hello from EDX115 Board! You are all set to rock and roll. :)"

#define GSM_PIN ""   // In case your sim card has PIN protection

/*************************************************************************
  APN: Set APN as per your sim card.

  AIRTEL: "airtelgprs.com"
  BSNL: "bsnlnet"
  VODA: "portalnmms"
  JIO: "jionet"
*************************************************************************/
const char apn[]      = "airtelgprs.com";
const char gprsUser[] = "";
const char gprsPass[] = "";

/*************************************************************************
  CLOUD: API/Callback url to send your data to cloud.
         In this example we have used "Pipedream" which will allow to
         create a dummy API call back URL and send data for testing purpose.
*************************************************************************/
String send_data_to_url = "https://eogas6eaag50nu2.m.pipedream.net";
String gpsLatLong = "";

int timeout = 60;   // GPS latch timeout duration (seconds)

#define TEST_GNSS            true    // runs only if the module supports GNSS
#define DUMP_AT_COMMANDS     true    // print every AT command and response

/*************************************************************************
  PIN MAPPING: EDX115
*************************************************************************/
#define SerialAT   Serial1
#define SerialMon  Serial
#define UART_BAUD  115200
#define PIN_TX     17      // ESP32 TX -> EC200U RX
#define PIN_RX     18      // ESP32 RX <- EC200U TX
#define PWR_PIN    47      // EC200U power key
#define RST_PIN    48
#define LED_PIN    3       // Programmable LED (IO3)


/*************************************************************************
  AT HELPERS
*************************************************************************/
bool atQuiet = false;

void printResponse(const String& buf) {
  if (!DUMP_AT_COMMANDS || atQuiet) return;
  int start = 0;
  while (start < (int)buf.length()) {
    int e = buf.indexOf('\n', start);
    if (e < 0) e = buf.length();
    String line = buf.substring(start, e);
    line.trim();
    if (line.length()) {
      SerialMon.print("   << ");
      SerialMon.println(line);
    }
    start = e + 1;
  }
}

// Reads the modem until "expect" arrives (true), an ERROR arrives or timeout (false)
bool readUntil(const char* expect, uint32_t timeoutMs, String* out) {
  String buf;
  buf.reserve(256);
  bool ok = false, err = false;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) buf += (char)SerialAT.read();
    if (buf.indexOf(expect) >= 0) { ok = true; break; }
    if (buf.indexOf("ERROR") >= 0) { err = true; break; }
    delay(2);
  }
  if (ok || err) {   // collect the rest of the reply
    unsigned long t = millis();
    while (millis() - t < 150) {
      while (SerialAT.available()) buf += (char)SerialAT.read();
      delay(2);
    }
  }
  printResponse(buf);
  if (!ok && !err && DUMP_AT_COMMANDS && !atQuiet) {
    SerialMon.print("   << (timeout waiting for ");
    SerialMon.print(expect);
    SerialMon.println(")");
  }
  if (out) *out = buf;
  return ok;
}

bool atCmd(const String& cmd, uint32_t timeoutMs, const char* expect, String* out) {
  while (SerialAT.available()) SerialAT.read();   // drop old unsolicited text
  if (DUMP_AT_COMMANDS && !atQuiet) {
    SerialMon.print("   >> ");
    SerialMon.println(cmd);
  }
  SerialAT.print(cmd);
  SerialAT.print("\r\n");
  return readUntil(expect, timeoutMs, out);
}

bool at(const String& cmd) {
  return atCmd(cmd, 1000, "OK", nullptr);
}

String afterTag(const String& resp, const char* tag) {
  int i = resp.indexOf(tag);
  if (i < 0) return "";
  i += strlen(tag);
  int e = resp.indexOf('\n', i);
  String v = (e < 0) ? resp.substring(i) : resp.substring(i, e);
  v.trim();
  return v;
}

String csvField(const String& s, int idx) {
  int start = 0;
  for (int i = 0; i < idx; i++) {
    start = s.indexOf(',', start);
    if (start < 0) return "";
    start++;
  }
  int end = s.indexOf(',', start);
  String v = (end < 0) ? s.substring(start) : s.substring(start, end);
  v.trim();
  return v;
}

String firstDigitsLine(const String& resp) {
  int start = 0;
  while (start < (int)resp.length()) {
    int e = resp.indexOf('\n', start);
    if (e < 0) e = resp.length();
    String line = resp.substring(start, e);
    line.trim();
    bool digits = line.length() >= 6;
    for (uint16_t i = 0; i < line.length() && digits; i++) {
      if (!isDigit(line[i])) digits = false;
    }
    if (digits) return line;
    start = e + 1;
  }
  return "";
}

bool isNetworkConnected() {
  String r;
  if (atCmd("AT+CREG?", 1000, "OK", &r)) {
    int st = csvField(afterTag(r, "+CREG:"), 1).toInt();
    if (st == 1 || st == 5) return true;
  }
  r = "";
  if (atCmd("AT+CEREG?", 1000, "OK", &r)) {
    int st = csvField(afterTag(r, "+CEREG:"), 1).toInt();
    if (st == 1 || st == 5) return true;
  }
  return false;
}

String getOperator() {
  String r;
  if (!atCmd("AT+COPS?", 3000, "OK", &r)) return "";
  String v = afterTag(r, "+COPS:");
  int q1 = v.indexOf('"');
  int q2 = v.indexOf('"', q1 + 1);
  return (q1 >= 0 && q2 > q1) ? v.substring(q1 + 1, q2) : "";
}

int getSignalQuality() {   // raw CSQ 0-31, 99 = unknown
  String r;
  if (!atCmd("AT+CSQ", 1000, "OK", &r)) return 99;
  return csvField(afterTag(r, "+CSQ:"), 0).toInt();
}


/*************************************************************************
  MODEM POWER
  - If the module is already ON it is powered down first (AT+QPOWD=1) so that
    the SIM card is detected again on the next power ON.
  - Power ON pulse (same as the tested EDX115 code):
    PWR HIGH 2000 ms -> LOW 2500 ms -> HIGH
*************************************************************************/
bool modemAlive() {
  bool q = atQuiet;
  atQuiet = true;
  bool ok = false;
  for (int i = 0; i < 3 && !ok; i++) {
    ok = atCmd("AT", 500, "OK", nullptr);
  }
  atQuiet = q;
  return ok;
}

bool modemPowerOn() {
  pinMode(PWR_PIN, OUTPUT);
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, LOW);
  delay(1000);

  if (modemAlive()) {
    Serial.println("4G module is already ON, power cycling it so the SIM gets detected...");
    atCmd("AT+QPOWD=1", 3000, "OK", nullptr);
    readUntil("POWERED DOWN", 10000, nullptr);
    delay(2000);
  }

  Serial.println("Powering ON 4G Module");
  digitalWrite(PWR_PIN, HIGH);
  delay(2000);
  digitalWrite(PWR_PIN, LOW);
  delay(2500);
  digitalWrite(PWR_PIN, HIGH);
  Serial.println("4G power ON done, waiting for module to boot...");

  bool up = false;
  for (int i = 0; i < 30 && !up; i++) {
    atQuiet = true;
    up = atCmd("AT", 500, "OK", nullptr);
    atQuiet = false;
    if (!up) { Serial.print("."); delay(1000); }
  }
  Serial.println();
  if (up) delay(2000);   // let the SIM initialise
  return up;
}

// Make sure SIM1 (J5) is the active SIM if dual-SIM mode was enabled before
void selectSim1() {
  String r;
  if (atCmd("AT+QDSIMCFG=\"dsss\"", 2000, "OK", &r) && r.indexOf("\"dsss\",1") >= 0) {
    Serial.println("Dual SIM mode is active in module, selecting SIM1 (J5)");
    at("AT+QDSIM=0");
  }
}

bool waitForSim() {
  for (int i = 0; i < 15; i++) {
    String r;
    atCmd("AT+CPIN?", 3000, "OK", &r);
    if (r.indexOf("+CPIN: READY") >= 0) return true;
    if (r.indexOf("SIM PIN") >= 0 && strlen(GSM_PIN) > 0) {
      Serial.println("SIM is PIN protected, unlocking...");
      at(String("AT+CPIN=\"") + GSM_PIN + "\"");
    }
    delay(2000);
  }
  return false;
}

void setup() {
  // Set console baud rate
  SerialMon.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  delay(3000);

  SerialAT.begin(UART_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  Serial.println();
  Serial.println();
  Serial.println("/**********************************************************/");
  Serial.println("  EDGEHAX EDX115 4G/LTE Module (Quectel EC200U) Sample Program");
  Serial.println("  To initialize the network test, please make sure the antenna has been");
  Serial.println("  connected and SIM card is inserted in SIM1 (J5).");
  Serial.println("/**********************************************************/\n\n");

  String res;
  Serial.println("Initializing Modem...");

  if (!modemPowerOn()) {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("Modem not responding. Check the DC power adapter and the 4G antenna.");
    Serial.println("Continuing without modem init for now...");
    digitalWrite(LED_PIN, LOW);
  }

  // LED on the board used as an indicator
  // If blinking: searching for network
  // If turned ON: connected to network
  // If turned OFF: modem booted but not connected to network, check your SIM, coverage etc.
  digitalWrite(LED_PIN, LOW);

  at("ATE0");                // echo off
  at("AT+CMEE=2");           // verbose error messages

  Serial.println("Running module information commands...");
  atCmd("ATI", 1000, "OK", &res);   // Quectel / EC200U / Revision: ...
  Serial.println();

  // Parse model + firmware revision from ATI
  String modemName = "";
  String modemInfo = "";
  int start = 0;
  while (start < (int)res.length()) {
    int e = res.indexOf('\n', start);
    if (e < 0) e = res.length();
    String line = res.substring(start, e);
    line.trim();
    if (line.startsWith("Revision:")) modemInfo = afterTag(line, "Revision:");
    else if (line != "Quectel" && line != "OK" && line != "ATI" && line.length() && modemName.length() == 0) modemName = line;
    start = e + 1;
  }
  res = "";

  atCmd("AT+GSN", 1000, "OK", &res);
  String imei = firstDigitsLine(res);
  res = "";

  Serial.println("Modem Name : " + modemName);
  delay(1000);
  Serial.println("Modem Info : " + modemInfo);
  Serial.println("Modem IMEI : " + imei);
  delay(1000);

  String payload;
  JsonDocument payloadObj;
  payloadObj["edgehax_edx115_module_testing"] = modemName;
  payloadObj["modemInfo"] = modemInfo;
  payloadObj["imei"] = imei;
  serializeJson(payloadObj, payload);   // Convert data to json format

  // SIM card check / unlock
  Serial.println();
  Serial.println("Checking SIM card...");
  selectSim1();

  if (!waitForSim()) {
    Serial.println("SIM not ready. Trying SIM re-initialisation (CFUN=0 -> CFUN=1)...");
    atCmd("AT+CFUN=0", 15000, "OK", nullptr);
    delay(2000);
    atCmd("AT+CFUN=1", 15000, "OK", nullptr);
    delay(3000);
    if (!waitForSim()) {
      Serial.println("Still no SIM. Disabling SIM hot-plug detect and power cycling the module...");
      at("AT+QSIMDET=0,0");
      modemPowerOn();
      selectSim1();
      waitForSim();
    }
  }

  {
    String r;
    atCmd("AT+CPIN?", 3000, "OK", &r);
    if (r.indexOf("+CPIN: READY") >= 0) {
      r = "";
      atCmd("AT+CIMI", 2000, "OK", &r);
      Serial.println("SIM READY, IMSI: " + firstDigitsLine(r));
    } else {
      Serial.println("######### SIM NOT DETECTED! #########");
      Serial.println("Check: SIM fully inserted in J5 (contacts facing the board), SIM works in a phone,");
      Serial.println("DC adapter used, then power cycle the whole board.");
    }
  }

  Serial.println();
  Serial.println("Network mode connectivity testing (Automatic, LTE or GSM)...");

  bool isConnected = false;
  for (int i = 0; i < 3; i++) {
    uint8_t network[]     = { 0, 3, 1 };
    const char* netName[] = { "Automatic", "LTE only", "GSM only" };
    Serial.printf("Try %s method\n", netName[i]);
    atCmd(String("AT+QCFG=\"nwscanmode\",") + network[i] + ",1", 3000, "OK", nullptr);
    delay(3000);
    int tryCount = 60;
    atQuiet = true;   // keep the log readable while polling
    while (tryCount--) {
      String networkOperator = getOperator();
      Serial.print("Operator: ");
      Serial.println(networkOperator);
      int signal = getSignalQuality();
      Serial.print("Signal: ");
      Serial.println(signal);
      Serial.print("isNetworkConnected: ");
      isConnected = isNetworkConnected();
      Serial.println(isConnected ? "CONNECTED" : "NOT CONNECTED YET");
      if (isConnected) {
        break;
      }
      delay(1000);
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
    atQuiet = false;
    if (isConnected) {
      break;
    }
  }

  if (isConnected) {
    digitalWrite(LED_PIN, HIGH);   // Modem connected to network
    Serial.println();
    Serial.println("Yehhh....Device is connected to Sim network.");
  } else {
    digitalWrite(LED_PIN, LOW);
    Serial.println();
    Serial.println("######### NOT CONNECTED to the network. Check antenna, SIM validity and coverage. #########");
  }
  Serial.println();

  delay(1000);
  Serial.println("Checking UE (User Equipment) system information...");
  Serial.println();
  at("AT+QNWINFO");
  at("AT+QENG=\"servingcell\"");

  delay(1000);
  Serial.println("");
  Serial.println("");

  if (isNetworkConnected()) {
    Serial.println("Mobile Network is connected.......");
  }

  // GPRS / data connection (PDP context 1)
  bool gprsConnected = false;
  Serial.print("Connecting to APN: ");
  Serial.println(apn);
  atCmd(String("AT+QICSGP=1,1,\"") + apn + "\",\"" + gprsUser + "\",\"" + gprsPass + "\",1", 2000, "OK", nullptr);
  {
    String r;
    if (atCmd("AT+QIACT?", 2000, "OK", &r) && r.indexOf("+QIACT: 1,1") >= 0) {
      gprsConnected = true;
    } else if (atCmd("AT+QIACT=1", 60000, "OK", nullptr)) {
      gprsConnected = true;
    }
    if (gprsConnected) {
      r = "";
      atCmd("AT+QIACT?", 2000, "OK", &r);
      Serial.println("APN connect success");
      Serial.println("");
      Serial.println("GPRS network is connected");
      Serial.println("IP: " + afterTag(r, "+QIACT:"));
    } else {
      Serial.println("APN connect failed (check APN and data plan)");
    }
  }
  Serial.println("");
  Serial.println("");

  delay(2000);
  Serial.println("Testing a sample HTTPS Call to Pipedream server...");
  Serial.println("NOTE: Please ensure to deploy an end point on Pipedream (pipedream.com) to test. Example: " + send_data_to_url);
  Serial.println();
  Serial.println();
  Serial.println("MODEM TESTING IN PROGRESS....");
  Serial.println();
  delay(3000);

  if (gprsConnected) {
    Serial.println("Configuring HTTP");
    at("AT+QHTTPCFG=\"contextid\",1");
    at("AT+QHTTPCFG=\"responseheader\",0");
    at("AT+QHTTPCFG=\"contenttype\",4");   // 4 = application/json (use 1 = text/plain if rejected)
    if (send_data_to_url.startsWith("https")) {
      at("AT+QHTTPCFG=\"sslctxid\",1");
      at("AT+QSSLCFG=\"sslversion\",1,4");
      at("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF");
      at("AT+QSSLCFG=\"seclevel\",1,0");     // no certificate check (test only)
      at("AT+QSSLCFG=\"sni\",1,1");          // optional, ignored if unsupported
    }

    Serial.print("Paylod length: ");
    Serial.println(payload.length());
    Serial.println("Setting http call back URL");

    bool urlOk = false;
    if (!atCmd("AT+QHTTPURL=" + String(send_data_to_url.length()) + ",80", 5000, "CONNECT", nullptr)) {
      Serial.println("ERROR: HTTP URL SETTING FAILED!");
    } else {
      SerialAT.print(send_data_to_url);
      urlOk = readUntil("OK", 5000, nullptr);
      if (!urlOk) Serial.println("ERROR: HTTP URL NOT ACCEPTED");
    }

    if (urlOk) {
      Serial.println("Executing POST call");
      if (!atCmd("AT+QHTTPPOST=" + String(payload.length()) + ",80,80", 60000, "CONNECT", nullptr)) {
        Serial.println("ERROR: HTTP POST CALL FAILED! (server unreachable?)");
      } else {
        SerialAT.print(payload);   // Send the body/data of the post call
        String r;
        if (!readUntil("+QHTTPPOST:", 80000, &r)) {
          Serial.println("ERROR: SENDING POST DATA");
        } else {
          String result = afterTag(r, "+QHTTPPOST:");   // <err>,<httpcode>,<length>
          int err = csvField(result, 0).toInt();
          int httpCode = csvField(result, 1).toInt();
          Serial.print("Data sent to API: ");
          Serial.println(payload);
          Serial.printf("Post call result: err=%d, HTTP status=%d\n", err, httpCode);
          if (err == 0 && httpCode >= 200 && httpCode < 300) {
            Serial.println("HTTP POST TEST : PASS");
          } else {
            Serial.println("HTTP POST TEST : FAIL");
          }
          if (err == 0) {
            Serial.print("Post call response:");
            Serial.println();
            atCmd("AT+QHTTPREAD=80", 10000, "+QHTTPREAD:", nullptr);
          }
        }
      }
    }
  } else {
    Serial.println("Skipping HTTP test, no data connection.");
  }
  delay(2000);

  Serial.println("Sending Test SMS to number(s):");
  Serial.println(SMS_TARGET1);

  if (isNetworkConnected()) {
    Serial.print("Sending SMS... please wait ");
    at("AT+CMGF=1");             // Text mode
    at("AT+CSCS=\"GSM\"");
    if (!atCmd(String("AT+CMGS=\"") + SMS_TARGET1 + "\"", 5000, ">", nullptr)) {
      SerialAT.write(0x1B);      // ESC cancels a pending SMS
      Serial.println("SMS sent status: FAILED (no prompt from module)");
    } else {
      SerialAT.print(SMS_MESSAGE);
      SerialAT.write(0x1A);      // Ctrl+Z, sends the SMS
      bool smsOk = readUntil("+CMGS:", 60000, nullptr);
      Serial.print("Send SMS returned: ");
      Serial.println(smsOk ? "SUCCESS" : "FAILED (check SMS pack / message centre number)");
    }
  } else {
    Serial.println("Network not connected, SMS not sent.");
  }
  delay(2000);

  Serial.println("END OF MODEM TESTING");

  /******************************************************************************************

      GNSS: GPS/GNSS onboard.
      The below code runs only if the EC200U variant supports GPS/GNSS
      so it is safe to ignore if you are testing a module without GPS.

      Quectel EC200U GNSS AT commands:
        AT+QGPS=1         -> turn ON GNSS
        AT+QGPSLOC=2      -> position in decimal degrees
        AT+QGPSEND        -> turn OFF GNSS

      ***** IMPORTANT NOTE *******
      Connect the GNSS antenna. Inside a building you wont get any GPS co-ordinates,
      test in open sky.
  ******************************************************************************************/
#if TEST_GNSS
  Serial.println("Enabling GPS/GNSS");
  Serial.println("Checking if modem supports GPS/GNSS...");
  {
    String r;
    bool gnssOn = false;
    atCmd("AT+QGPS?", 2000, "OK", &r);
    if (r.indexOf("+QGPS: 1") >= 0) {
      gnssOn = true;
    } else {
      r = "";
      gnssOn = atCmd("AT+QGPS=1", 3000, "OK", &r);
      if (!gnssOn && (r.indexOf("504") >= 0 || r.indexOf("ongoing") >= 0)) gnssOn = true;   // already running
    }

    if (!gnssOn) {
      Serial.println("Modem doesnt support GPS/GNSS, so no need of GPS testing.");
      Serial.println(">>>>> End of 4G Module Testing <<<<<<");
      return;
    }

    Serial.println("GPS/GNSS is supported!");
    Serial.println("GPS/GNSS Power ON READY!");
    Serial.println("Fetching GPS co-ordinates...");
    Serial.println("Awaiting for GPS latch: ");

    atQuiet = true;
    bool latched = false;
    for (timeout = 30; timeout != 0; timeout--) {
      gpsLatLong = "";
      if (atCmd("AT+QGPSLOC=2", 3000, "+QGPSLOC:", &gpsLatLong)) {
        latched = true;
        break;
      }
      Serial.print(".");
      Serial.print(timeout);
      delay(1000);
    }
    atQuiet = false;
    Serial.println();

    if (latched) {
      String v = afterTag(gpsLatLong, "+QGPSLOC:");
      Serial.println("**** GPS Latched ****");
      Serial.println("CGPS INFO: " + v);
      Serial.println("Latitude : " + csvField(v, 1));
      Serial.println("Longitude: " + csvField(v, 2));
    } else {
      Serial.println(">>>>> TIME OUT: GPS Latch timedout. <<<<<<");
      Serial.println("Please ensure you are outside and have sky visibility in order for GPS to latch initially.");
    }
    Serial.println("GPS testing ended.");
  }
#endif
  Serial.println(">>>>> End of 4G Module Testing <<<<<<");
}


void loop() {
  // Manual AT command bridge: type AT commands in the Serial Monitor (Both NL & CR)
  while (1) {
    while (SerialAT.available()) {
      SerialMon.write(SerialAT.read());
    }
    while (SerialMon.available()) {
      SerialAT.write(SerialMon.read());
    }
  }
}
 