#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <ArduinoJson.h>
#include <ESP_Mail_Client.h>

#define BUZZER_PIN 13
#define LED_PIN 2
#define BUTTON_PIN 18
#define MQ135_PIN 32
#define TRIG_PIN 27
#define ECHO_PIN 26

const char* ssid = "mobile";
const char* password = "123";

#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
#define AUTHOR_EMAIL "satwadronacharya@gmail.com"
#define AUTHOR_PASSWORD "rruliwkjfqglufiv"
#define RECIPIENT_EMAIL "loginothers123@gmail.com"

const String RESCUE_USERNAME = "rescue";
const String RESCUE_PASSWORD = "rescue_saver";

String ACCOUNT_SID = "your ssid";
String AUTH_TOKEN = "auth";
String FROM_NUMBER = "+91";
String TO_NUMBER = "+91";

const float LATITUDE = 17.435478421236663;
const float LONGITUDE = 78.35060428759945 ;

Adafruit_MPU6050 mpu;
WebServer server(80);
SMTPSession smtp;

// === Thresholds & States ===
float earthquake_threshold = 1.5;
int gas_threshold = 3000;
float flood_threshold_cm = 8.0;

enum SystemState {
  MONITORING,
  EARTHQUAKE_WAIT,
  WATER_WAIT,
  GAS_WAIT,
  EARTHQUAKE_ALERT,
  WATER_ALERT,
  GAS_ALERT,
  FALSE_POSITIVE
};

SystemState currentState = MONITORING;
unsigned long waitStartTime = 0;
unsigned long lastBlinkTime = 0;
unsigned long lastWebUpdate = 0;
const unsigned long updateInterval = 10000;
bool ledState = false;
bool buttonPressed = false;
bool lastButtonState = false;
bool manualAlert = false;

// === Last Sensor Values ===
float last_accel = 0;
float last_water = 0;
int last_gas = 0;
String last_status = "MONITORING";

// === Alert Log ===
String alertLog[10];
int logIndex = 0;
int logCount = 0;

// === Alert Log Functions ===
void addToLog(String message) {
  String timestamp = String(millis() / 1000) + "s";
  alertLog[logIndex] = "[" + timestamp + "] " + message;
  logIndex = (logIndex + 1) % 10;
  if (logCount < 10) logCount++;
}

String getAlertLog() {
  String log = "";
  for (int i = 0; i < logCount; i++) {
    int idx = (logIndex - logCount + i + 10) % 10;
    log += alertLog[idx] + "\\n";
  }
  return log;
}

// === Email Functions ===
// === Email Functions ===
void sendEmailAlert(String subject, String message) {
  Serial.println("Sending email alerts...");
  
  Session_Config config;
  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = AUTHOR_EMAIL;
  config.login.password = AUTHOR_PASSWORD;
  config.time.ntp_server = F("pool.ntp.org,time.nist.gov");
  config.time.gmt_offset = 5.5; // IST timezone
  config.time.day_light_offset = 0;

  SMTP_Message email;
  email.sender.name = "Disaster Detection System";
  email.sender.email = AUTHOR_EMAIL;
  email.subject = subject;
  email.addRecipient("Emergency Alert", RECIPIENT_EMAIL);
  
  String emailContent = message + "\n\n";
  emailContent += "Location: Guwahati ASSAM PALTAN BAZAR\n";
  emailContent += "System Time: " + String(millis()/1000) + " seconds since boot\n";
  emailContent += "Timestamp: " + String(millis()) + "ms\n\n";  // ✅ FIXED LINE
  emailContent += "This is an automated alert from your Disaster Detection System.\n";
  emailContent += "Please take immediate action if it is a real emergency.";
  
  email.text.content = emailContent.c_str();
  email.text.charSet = "us-ascii";
  email.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;
  email.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_high;

  if (!smtp.connect(&config)) {
    Serial.println("❌ SMTP Connection failed");
    return;
  }

  if (MailClient.sendMail(&smtp, &email)) {
    Serial.println("✅ Email sent successfully!");
    addToLog("Email alert sent successfully");
  } else {
    Serial.println("❌ Email sending failed");
    addToLog("Email sending failed");
  }

  smtp.closeSession();
}

// === API Verification Functions ===
bool verifyEarthquake() {
  HTTPClient http;
  http.begin("https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/all_hour.geojson");
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, payload);
    
    JsonArray features = doc["features"];
    for (JsonObject feature : features) {
      JsonArray coords = feature["geometry"]["coordinates"];
      float lon = coords[0];
      float lat = coords[1];
      float magnitude = feature["properties"]["mag"];
      
      float distance = sqrt(pow(lat - LATITUDE, 2) + pow(lon - LONGITUDE, 2));
      if (distance < 1.0 && magnitude > 2.0) {
        http.end();
        return true;
      }
    }
  }
  http.end();
  return false;
}

bool verifyFlood() {
  HTTPClient http;
  http.begin("https://api.openweathermap.org/data/2.5/weather?lat=" + String(LATITUDE) + "&lon=" + String(LONGITUDE) + "&appid=d1c74259b417a4f517aa6e754fb3b404");
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);
    
    JsonArray weather = doc["weather"];
    for (JsonObject w : weather) {
      String main = w["main"];
      if (main == "Rain" || main == "Thunderstorm") {
        float precipitation = doc["rain"]["1h"];
        if (precipitation > 10.0) {
          http.end();
          return true;
        }
      }
    }
  }
  http.end();
  return false;
}

bool verifyGasLeak() {
  HTTPClient http;
  http.begin("https://api.openaq.org/v2/latest?coordinates=" + String(LATITUDE) + "," + String(LONGITUDE) + "&radius=50000");
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, payload);
    
    JsonArray results = doc["results"];
    for (JsonObject result : results) {
      JsonArray measurements = result["measurements"];
      for (JsonObject measurement : measurements) {
        String parameter = measurement["parameter"];
        if (parameter == "co" || parameter == "no2") {
          float value = measurement["value"];
          if (value > 30.0) {
            http.end();
            return true;
          }
        }
      }
    }
  }
  http.end();
  return false;
}

// === Enhanced API Handler ===
// === Enhanced API Handler ===
void handle_api() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");

  if (server.method() == HTTP_OPTIONS) {
    server.send(200, "text/plain", "");
    return;
  }

  if (server.method() == HTTP_POST) {
    String body = server.arg("plain");
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
      server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }

    String action = doc["action"];

    // Authentication check for rescue center actions
    String username = doc["username"];
    String password = doc["password"];

    if (action == "login") {
      if (username == RESCUE_USERNAME && password == RESCUE_PASSWORD) {
        server.send(200, "application/json",
                    "{\"status\":\"success\",\"message\":\"Login successful\"}");
        addToLog("Rescue Center Login Successful");
      } else {
        server.send(401, "application/json",
                    "{\"status\":\"error\",\"message\":\"Incorrect credentials. Unsuccessful login attempt.\"}");
        addToLog("Failed Login Attempt - User: " + username);
      }
      return;
    }

    // Emergency Stop
    if (action == "emergency_stop") {
      Serial.println("🛑 EMERGENCY STOP triggered from rescue center!");
      currentState  = MONITORING;
      digitalWrite(BUZZER_PIN, LOW);
      digitalWrite(LED_PIN, LOW);
      ledState     = false;
      manualAlert  = false;

      sendSMS("🛑 Emergency Stop activated from Rescue Center. All alerts stopped.");
      sendEmailAlert("🛑 Emergency Stop Activated",
                     "Emergency Stop has been activated from the Rescue Center. "
                     "All alerts have been stopped.");
      addToLog("EMERGENCY STOP - All alerts stopped");

      server.send(200, "application/json",
                  "{\"status\":\"stopped\",\"message\":\"All alerts stopped successfully\"}");
      return;
    }

    // Manual Alert Triggers  --------------------------------------------------
    if (action == "test_earthquake") {
      Serial.println("🛑 Earthquake alert triggered from rescue center!");
      currentState = EARTHQUAKE_ALERT;
      last_status  = "TEST EARTHQUAKE ALERT!";
      manualAlert  = true;

      sendSMS("🛑 Earthquake Alert triggered from Rescue Center (TEST)");
      sendEmailAlert("🛑 TEST Earthquake Alert Triggered",
                     "This is a earthquake alert sent from the Rescue Center. "
                     "Please treat it as a drill and verify system readiness.");

      addToLog("TEST EARTHQUAKE Alert Triggered");
      server.send(200, "application/json",
                  "{\"status\":\"triggered\",\"type\":\"earthquake\",\"message\":\"Test earthquake alert activated\"}");
      return;
    }

    if (action == "test_flood") {
      Serial.println("Flood alert triggered from rescue center!");
      currentState  = WATER_ALERT;
      last_status   = "TEST FLOOD ALERT!";
      manualAlert   = true;
      lastBlinkTime = millis();

      sendSMS("🛑 Flood Alert triggered from Rescue Center (TEST)");
      sendEmailAlert("🛑 Flood Alert Triggered",
                     "This is a flood alert sent from the Rescue Center. "
                     "Please treat it as a drill and verify system readiness.");

      addToLog("TEST FLOOD Alert Triggered");
      server.send(200, "application/json",
                  "{\"status\":\"triggered\",\"type\":\"flood\",\"message\":\"Test flood alert activated\"}");
      return;
    }

    if (action == "test_gas") {
      Serial.println("🛑 GAS LEAK ALERT triggered from rescue center!");
      currentState  = GAS_ALERT;
      last_status   = "TEST GAS ALERT!";
      manualAlert   = true;
      lastBlinkTime = millis();

      sendSMS("🛑 Gas Leak Alert triggered from Rescue Center (TEST)");
      sendEmailAlert("🛑 Gas Leak Alert Triggered",
                     "This is a gas-leak alert sent from the Rescue Center. "
                     "Please treat it as a drill and verify system readiness.");

      addToLog("TEST GAS Alert Triggered");
      server.send(200, "application/json",
                  "{\"status\":\"triggered\",\"type\":\"gas\",\"message\":\"Test gas alert activated\"}");
      return;
    }

    if (action == "get_log") {
      String log = getAlertLog();
      server.send(200, "application/json",
                  "{\"status\":\"success\",\"log\":\"" + log + "\"}");
      return;
    }

    server.send(400, "application/json", "{\"error\":\"Unknown action\"}");
    return;
  }

  // GET request – return sensor data
  String json = "{";
  json += "\"accel\":" + String(last_accel, 2) + ",";
  json += "\"water\":" + String(last_water, 1) + ",";
  json += "\"gas\":"   + String(last_gas) + ",";
  json += "\"status\":\"" + last_status + "\",";
  json += "\"manual\":" + String(manualAlert ? "true" : "false") + ",";
  json += "\"timestamp\":" + String(millis());
  json += "}";
  server.send(200, "application/json", json);
}

// === Login Page ===
void handle_login() {
  String html = R"rawliteral(
    <!DOCTYPE html><html lang="en"><head>
      <meta charset="UTF-8">
      <title>Government Rescue Center - Login</title>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <style>
        body { background: linear-gradient(135deg, #1e3c72 0%, #2a5298 100%); color: #fff; font-family: 'Segoe UI',sans-serif; margin: 0; padding: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center; }
        .login-container { background: rgba(255,255,255,0.1); backdrop-filter: blur(10px); border-radius: 20px; padding: 40px; box-shadow: 0 8px 32px rgba(0,0,0,0.3); max-width: 400px; width: 90%; }
        .logo { text-align: center; margin-bottom: 30px; }
        .logo svg { width: 64px; height: 64px; fill: #fff; margin-bottom: 10px; }
        .title { font-size: 1.8em; font-weight: 700; text-align: center; margin-bottom: 10px; }
        .subtitle { text-align: center; color: rgba(255,255,255,0.8); margin-bottom: 30px; }
        .form-group { margin-bottom: 20px; }
        .form-label { display: block; margin-bottom: 8px; font-weight: 500; }
        .form-input { width: 100%; padding: 12px 16px; border: none; border-radius: 10px; background: rgba(255,255,255,0.2); color: #fff; font-size: 16px; box-sizing: border-box; }
        .form-input::placeholder { color: rgba(255,255,255,0.6); }
        .form-input:focus { outline: none; box-shadow: 0 0 0 2px #4fc3f7; }
        .login-btn { width: 100%; padding: 14px; background: #4fc3f7; color: #fff; border: none; border-radius: 10px; font-size: 16px; font-weight: 600; cursor: pointer; transition: background 0.3s; }
        .login-btn:hover { background: #29b6f6; }
        .error-msg { background: rgba(244,67,54,0.8); padding: 10px; border-radius: 8px; margin-top: 15px; text-align: center; display: none; }
        .back-btn { position: absolute; top: 20px; left: 20px; background: rgba(255,255,255,0.2); border: none; border-radius: 50%; width: 50px; height: 50px; color: #fff; cursor: pointer; display: flex; align-items: center; justify-content: center; }
      </style>
    </head>
    <body>
      <button class="back-btn" onclick="window.location.href='/'">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor"><path d="M20 11H7.83l5.59-5.59L12 4l-8 8 8 8 1.41-1.41L7.83 13H20v-2z"/></svg>
      </button>
      <div class="login-container">
        <div class="logo">
          <svg viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm3.5 6L12 10.5 8.5 8 12 5.5 15.5 8zM8.5 16L12 13.5 15.5 16 12 18.5 8.5 16z"/></svg>
          <div class="title">Government Rescue Center</div>
          <div class="subtitle">Authorized Personnel Only</div>
        </div>
        <form id="loginForm">
          <div class="form-group">
            <label class="form-label">Username</label>
            <input type="text" class="form-input" id="username" placeholder="Enter username" required>
          </div>
          <div class="form-group">
            <label class="form-label">Password</label>
            <input type="password" class="form-input" id="password" placeholder="Enter password" required>
          </div>
          <button type="submit" class="login-btn">Login to Rescue Center</button>
          <div class="error-msg" id="errorMsg"></div>
        </form>
      </div>
      <script>
        document.getElementById('loginForm').addEventListener('submit', function(e) {
          e.preventDefault();
          const username = document.getElementById('username').value;
          const password = document.getElementById('password').value;
          const errorMsg = document.getElementById('errorMsg');
          
          fetch('/api', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'login', username: username, password: password })
          })
          .then(response => response.json())
          .then(data => {
            if (data.status === 'success') {
              window.location.href = '/rescue';
            } else {
              errorMsg.textContent = data.message;
              errorMsg.style.display = 'block';
              setTimeout(() => errorMsg.style.display = 'none', 3000);
            }
          })
          .catch(error => {
            errorMsg.textContent = 'Login failed. Please try again.';
            errorMsg.style.display = 'block';
          });
        });
      </script>
    </body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}

// === Rescue Center Dashboard ===
void handle_rescue() {
  String html = R"rawliteral(
    <!DOCTYPE html><html lang="en"><head>
      <meta charset="UTF-8">
      <title>Government Rescue Center - Control Panel</title>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { background: #181f2a; color: #fff; font-family: 'Segoe UI',sans-serif; overflow-x: hidden; }
        .header { background: linear-gradient(135deg, #1e3c72 0%, #2a5298 100%); padding: 20px; display: flex; align-items: center; justify-content: space-between; position: sticky; top: 0; z-index: 100; box-shadow: 0 2px 10px rgba(0,0,0,0.3); }
        .header-left { display: flex; align-items: center; gap: 15px; }
        .header-icon { background: rgba(255,255,255,0.1); border-radius: 50%; width: 50px; height: 50px; display: flex; align-items: center; justify-content: center; }
        .header-icon svg { width: 28px; height: 28px; fill: #fff; }
        .header-title { font-size: 1.8em; font-weight: 700; }
        .header-right { position: relative; }
        .menu-btn { background: rgba(255,255,255,0.1); border: none; border-radius: 10px; padding: 10px 16px; color: #fff; cursor: pointer; font-size: 16px; transition: background 0.3s; }
        .menu-btn:hover { background: rgba(255,255,255,0.2); }
        .dropdown { position: relative; }
        .dropdown-content { display: none; position: absolute; right: 0; background: #232e41; min-width: 150px; box-shadow: 0px 8px 16px rgba(0,0,0,0.3); z-index: 1000; border-radius: 10px; overflow: hidden; top: 100%; }
        .dropdown-content a { color: #fff; padding: 12px 16px; text-decoration: none; display: block; transition: background 0.3s; }
        .dropdown-content a:hover { background: #2986f9; }
        .show { display: block; }
        .status-bar { background: #232e41; padding: 15px 20px; display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #333; }
        .live-status { display: flex; align-items: center; gap: 10px; }
        .live-dot { width: 8px; height: 8px; background: #4caf50; border-radius: 50%; animation: pulse 2s infinite; }
        @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.5; } 100% { opacity: 1; } }
        .main-content { max-height: calc(100vh - 140px); overflow-y: auto; padding: 20px; }
        .main-content::-webkit-scrollbar { width: 8px; }
        .main-content::-webkit-scrollbar-track { background: #232e41; border-radius: 4px; }
        .main-content::-webkit-scrollbar-thumb { background: #4fc3f7; border-radius: 4px; }
        .main-content::-webkit-scrollbar-thumb:hover { background: #29b6f6; }
        .control-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 20px; margin-bottom: 30px; }
        .panel-card { background: #232e41; border-radius: 15px; padding: 25px; box-shadow: 0 4px 15px rgba(0,0,0,0.2); transition: transform 0.3s ease; }
        .panel-card:hover { transform: translateY(-5px); }
        .panel-title { font-size: 1.3em; font-weight: 600; margin-bottom: 20px; display: flex; align-items: center; gap: 10px; border-bottom: 2px solid #2986f9; padding-bottom: 10px; }
        .panel-title svg { width: 24px; height: 24px; }
        .sensor-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 15px; margin-bottom: 20px; }
        .sensor-value { background: rgba(255,255,255,0.05); padding: 15px; border-radius: 10px; text-align: center; transition: all 0.3s ease; border: 2px solid transparent; }
        .sensor-value:hover { background: rgba(255,255,255,0.1); transform: scale(1.05); }
        .sensor-label { font-size: 0.9em; color: #aaa; margin-bottom: 5px; }
        .sensor-number { font-size: 1.8em; font-weight: 600; }
        .sensor-unit { font-size: 0.7em; color: #bbb; }
        .control-buttons { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 12px; }
        .control-btn { padding: 12px 20px; border: none; border-radius: 8px; font-weight: 600; cursor: pointer; transition: all 0.3s; position: relative; overflow: hidden; }
        .control-btn:before { content: ''; position: absolute; top: 0; left: -100%; width: 100%; height: 100%; background: linear-gradient(90deg, transparent, rgba(255,255,255,0.2), transparent); transition: left 0.5s; }
        .control-btn:hover:before { left: 100%; }
        .btn-test { background: #ff9800; color: #fff; }
        .btn-test:hover { background: #f57c00; transform: translateY(-2px); }
        .btn-stop { background: #f44336; color: #fff; }
        .btn-stop:hover { background: #d32f2f; transform: translateY(-2px); }
        .btn-log { background: #2196f3; color: #fff; }
        .btn-log:hover { background: #1976d2; transform: translateY(-2px); }
        .alert-status { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }
        .alert-item { background: rgba(255,255,255,0.05); padding: 15px; border-radius: 10px; display: flex; align-items: center; gap: 12px; transition: all 0.3s ease; border: 2px solid transparent; }
        .alert-item.alert-active { border-color: #f44336; animation: alertBlink 1s infinite; }
        @keyframes alertBlink { 0%, 50% { background: rgba(244,67,54,0.1); } 51%, 100% { background: rgba(244,67,54,0.3); } }
        .alert-icon { width: 40px; height: 40px; border-radius: 50%; display: flex; align-items: center; justify-content: center; position: relative; }
        .alert-icon.earthquake { background: rgba(255,193,7,0.2); color: #ffc107; }
        .alert-icon.flood { background: rgba(33,150,243,0.2); color: #2196f3; }
        .alert-icon.gas { background: rgba(76,175,80,0.2); color: #4caf50; }
        .alert-icon.alert-active { animation: iconPulse 0.8s infinite; }
        @keyframes iconPulse { 0% { transform: scale(1); } 50% { transform: scale(1.2); } 100% { transform: scale(1); } }
        .alert-details { flex: 1; }
        .alert-name { font-weight: 600; margin-bottom: 3px; }
        .alert-state { font-size: 0.9em; color: #aaa; }
        .alert-state.active { color: #f44336; font-weight: 600; animation: textBlink 1s infinite; }
        @keyframes textBlink { 0%, 50% { opacity: 1; } 51%, 100% { opacity: 0.7; } }
        .emergency-section { background: linear-gradient(135deg, #2d1b1b, #3d2b2b); border: 2px solid #f44336; border-radius: 15px; padding: 20px; margin-top: 20px; }
        .emergency-title { color: #f44336; font-size: 1.4em; font-weight: 700; margin-bottom: 15px; display: flex; align-items: center; gap: 10px; }
        .emergency-contacts { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 15px; }
        .contact-item { display: flex; align-items: center; gap: 12px; background: rgba(244,67,54,0.1); padding: 15px; border-radius: 10px; transition: all 0.3s ease; border: 1px solid rgba(244,67,54,0.2); }
        .contact-item:hover { background: rgba(244,67,54,0.2); transform: scale(1.02); border-color: rgba(244,67,54,0.4); }
        .contact-icon { width: 24px; height: 24px; fill: #f44336; flex-shrink: 0; }
        .contact-details { flex: 1; }
        .contact-name { color: #fff; font-weight: 600; font-size: 0.95em; margin-bottom: 3px; }
        .contact-link { color: #f44336; text-decoration: none; font-weight: 500; font-size: 1.1em; }
        .contact-link:hover { color: #ff6b6b; }
        .emergency-nav { position: fixed; bottom: 20px; right: 20px; background: #2196f3; border: none; border-radius: 50%; width: 60px; height: 60px; color: #fff; cursor: pointer; box-shadow: 0 4px 20px rgba(33,150,243,0.4); z-index: 1000; transition: all 0.3s ease; }
        .emergency-nav:hover { transform: scale(1.1); box-shadow: 0 6px 25px rgba(33,150,243,0.6); }
        .modal { display: none; position: fixed; z-index: 2000; left: 0; top: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); }
        .modal-content { background: #232e41; margin: 5% auto; padding: 20px; border-radius: 15px; width: 80%; max-width: 500px; color: #fff; max-height: 80vh; overflow-y: auto; }
        .close { color: #aaa; float: right; font-size: 28px; font-weight: bold; cursor: pointer; }
        .close:hover { color: #fff; }
        #logContent { background: #1a1a1a; padding: 15px; border-radius: 8px; font-family: monospace; white-space: pre-wrap; max-height: 300px; overflow-y: auto; margin-top: 15px; }
        @media (max-width: 768px) {
          .header { padding: 15px; }
          .header-title { font-size: 1.4em; }
          .control-grid { grid-template-columns: 1fr; }
          .main-content { padding: 15px; }
          .emergency-nav { bottom: 15px; right: 15px; width: 50px; height: 50px; }
          .emergency-contacts { grid-template-columns: 1fr; }
        }
      </style>
    </head>
    <body>
      <div class="header">
        <div class="header-left">
          <div class="header-icon">
            <svg viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm3.5 6L12 10.5 8.5 8 12 5.5 15.5 8zM8.5 16L12 13.5 15.5 16 12 18.5 8.5 16z"/></svg>
          </div>
          <div class="header-title">Government Rescue Center</div>
        </div>
        <div class="header-right">
          <div class="dropdown">
            <button class="menu-btn" onclick="toggleDropdown()">Menu ⋮</button>
            <div class="dropdown-content" id="menuDropdown">
              <a href="#" onclick="logout()">Logout</a>
            </div>
          </div>
        </div>
      </div>
      
      <div class="status-bar">
        <div class="live-status">
          <div class="live-dot"></div>
          <span>Live Monitoring Active</span>
        </div>
        <div id="currentTime"></div>
      </div>
      
      <div class="main-content">
        <div class="control-grid">
          <div class="panel-card">
            <div class="panel-title">
              <svg fill="currentColor" viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 15h2v2h-2v-2zm0-8h2v6h-2V9z"/></svg>
              Alert Status
            </div>
            <div class="alert-status">
              <div class="alert-item" id="earthquakeAlert">
                <div class="alert-icon earthquake">
                  <svg width="20" height="20" fill="currentColor" viewBox="0 0 24 24"><path d="M12 2l-2 7h4l-2-7zm0 20l2-7h-4l2 7z"/></svg>
                </div>
                <div class="alert-details">
                  <div class="alert-name">Earthquake</div>
                  <div class="alert-state" id="earthquakeStatus">Monitoring</div>
                </div>
              </div>
              <div class="alert-item" id="floodAlert">
                <div class="alert-icon flood">
                  <svg width="20" height="20" fill="currentColor" viewBox="0 0 24 24"><path d="M12 2C9.38 2 7.25 4.13 7.25 6.75c0 2.57 2.01 4.65 4.63 4.74v8.76h.24v-8.76c2.62-.09 4.63-2.17 4.63-4.74C16.75 4.13 14.62 2 12 2z"/></svg>
                </div>
                <div class="alert-details">
                  <div class="alert-name">Flood</div>
                  <div class="alert-state" id="floodStatus">Monitoring</div>
                </div>
              </div>
              <div class="alert-item" id="gasAlert">
                <div class="alert-icon gas">
                  <svg width="20" height="20" fill="currentColor" viewBox="0 0 24 24"><path d="M12 2C9.38 2 7.25 4.13 7.25 6.75S9.38 11.5 12 11.5s4.75-2.13 4.75-4.75S14.62 2 12 2zm0 7.5c-1.52 0-2.75-1.23-2.75-2.75S10.48 4 12 4s2.75 1.23 2.75 2.75S13.52 9.5 12 9.5z"/></svg>
                </div>
                <div class="alert-details">
                  <div class="alert-name">Gas Leak</div>
                  <div class="alert-state" id="gasStatus">Monitoring</div>
                </div>
              </div>
            </div>
          </div>
          
          <div class="panel-card">
            <div class="panel-title">
              <svg fill="currentColor" viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-2 15l-5-5 1.41-1.41L10 14.17l7.59-7.59L19 8l-9 9z"/></svg>
              Real-time Sensor Data
            </div>
            <div class="sensor-grid">
              <div class="sensor-value">
                <div class="sensor-label">Accelerometer</div>
                <div class="sensor-number" id="accelValue">--</div>
                <div class="sensor-unit">g</div>
              </div>
              <div class="sensor-value">
                <div class="sensor-label">Water Level</div>
                <div class="sensor-number" id="waterValue">--</div>
                <div class="sensor-unit">cm</div>
              </div>
              <div class="sensor-value">
                <div class="sensor-label">Gas Level</div>
                <div class="sensor-number" id="gasValue">--</div>
                <div class="sensor-unit">ppm</div>
              </div>
            </div>
          </div>
          
          <div class="panel-card">
            <div class="panel-title">
              <svg fill="currentColor" viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm5 11h-4v4h-2v-4H7v-2h4V7h2v4h4v2z"/></svg>
              Manual Control Panel
            </div>
            <div class="control-buttons">
              <button class="control-btn btn-test" onclick="testAlert('earthquake')">Test Earthquake</button>
              <button class="control-btn btn-test" onclick="testAlert('flood')">Test Flood</button>
              <button class="control-btn btn-test" onclick="testAlert('gas')">Test Gas Leak</button>
              <button class="control-btn btn-stop" onclick="emergencyStop()">Emergency Stop</button>
              <button class="control-btn btn-log" onclick="showAlertLog()">Alert Log</button>
            </div>
          </div>
        </div>
        
        <div class="panel-card emergency-section">
          <div class="emergency-title">
            <svg viewBox="0 0 24 24" width="24" height="24" fill="currentColor"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 15h2v2h-2v-2zm0-8h2v6h-2V9z"/></svg>
            Emergency Rescue Team Contacts
          </div>
          <div class="emergency-contacts">
            <div class="contact-item">
              <svg class="contact-icon" viewBox="0 0 24 24"><path d="M6.62 10.79c1.44 2.83 3.76 5.14 6.59 6.59l2.2-2.2c.27-.27.67-.36 1.02-.24 1.12.37 2.33.57 3.57.57.55 0 1 .45 1 1V20c0 .55-.45 1-1 1-9.39 0-17-7.61-17-17 0-.55.45-1 1-1h3.5c.55 0 1 .45 1 1 0 1.25.2 2.45.57 3.57.11.35.03.74-.25 1.02l-2.2 2.2z"/></svg>
              <div class="contact-details">
                <div class="contact-name">Assam State Disaster Management Authority (ASDMA)</div>
                <a href="tel:+916126785432" class="contact-link">+91 612 678 5432</a>
              </div>
            </div>
            <div class="contact-item">
              <svg class="contact-icon" viewBox="0 0 24 24"><path d="M6.62 10.79c1.44 2.83 3.76 5.14 6.59 6.59l2.2-2.2c.27-.27.67-.36 1.02-.24 1.12.37 2.33.57 3.57.57.55 0 1 .45 1 1V20c0 .55-.45 1-1 1-9.39 0-17-7.61-17-17 0-.55.45-1 1-1h3.5c.55 0 1 .45 1 1 0 1.25.2 2.45.57 3.57.11.35.03.74-.25 1.02l-2.2 2.2z"/></svg>
              <div class="contact-details">
                <div class="contact-name">State Disaster Response Force (SDRF Assam)</div>
                <a href="tel:+916129876543" class="contact-link">+91 612 987 6543</a>
              </div>
            </div>
            <div class="contact-item">
              <svg class="contact-icon" viewBox="0 0 24 24"><path d="M6.62 10.79c1.44 2.83 3.76 5.14 6.59 6.59l2.2-2.2c.27-.27.67-.36 1.02-.24 1.12.37 2.33.57 3.57.57.55 0 1 .45 1 1V20c0 .55-.45 1-1 1-9.39 0-17-7.61-17-17 0-.55.45-1 1-1h3.5c.55 0 1 .45 1 1 0 1.25.2 2.45.57 3.57.11.35.03.74-.25 1.02l-2.2 2.2z"/></svg>
              <div class="contact-details">
                <div class="contact-name">National Disaster Response Force (NDRF)</div>
                <a href="tel:+911124356789" class="contact-link">+91 112 435 6789</a>
              </div>
            </div>
            <div class="contact-item">
              <svg class="contact-icon" viewBox="0 0 24 24"><path d="M20 4H4c-1.1 0-1.99.9-1.99 2L2 18c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V6c0-1.1-.9-2-2-2zm0 4l-8 5-8-5V6l8 5 8-5v2z"/></svg>
              <div class="contact-details">
                <div class="contact-name">Emergency Coordination Center</div>
                <a href="mailto:emergency@rescuecenter.gov.in" class="contact-link">emergency@rescuecenter.gov.in</a>
              </div>
            </div>
          </div>
        </div>
      </div>
      
      <button class="emergency-nav" onclick="openEmergencyNavigation()" title="Emergency Navigation">
        <svg width="30" height="30" fill="currentColor" viewBox="0 0 24 24"><path d="M12 2C6.477 2 2 6.478 2 12c0 5.502 4.477 10 10 10s10-4.498 10-10c0-5.522-4.477-10-10-10zm0 17.2a7.2 7.2 0 1 1 0-14.4 7.2 7.2 0 0 1 0 14.4zm0-10.4a1.2 1.2 0 1 1 0 2.4 1.2 1.2 0 0 1 0-2.4zm0 3.2c-2.208 0-4 1.791-4 4 0 .662.27 1.26.703 1.699l.848.849 2.449 2.449c.155.155.358.24.571.24.212 0 .416-.085.57-.24l2.45-2.449.848-.849A2 2 0 0 0 16 16c0-2.209-1.792-4-4-4z"/></svg>
      </button>
      
      <!-- Alert Log Modal -->
      <div id="logModal" class="modal">
        <div class="modal-content">
          <span class="close" onclick="closeModal()">&times;</span>
          <h2>Alert Log History</h2>
          <div id="logContent">Loading...</div>
        </div>
      </div>
      
      <script>
        function toggleDropdown() {
          document.getElementById("menuDropdown").classList.toggle("show");
        }
        
        window.onclick = function(event) {
          if (!event.target.matches('.menu-btn')) {
            var dropdowns = document.getElementsByClassName("dropdown-content");
            for (var i = 0; i < dropdowns.length; i++) {
              var openDropdown = dropdowns[i];
              if (openDropdown.classList.contains('show')) {
                openDropdown.classList.remove('show');
              }
            }
          }
        }
        
        function updateTime() {
          document.getElementById('currentTime').textContent = new Date().toLocaleString();
        }
        setInterval(updateTime, 1000);
        updateTime();
        
        function updateSensorData() {
          fetch('/api')
            .then(response => response.json())
            .then(data => {
              document.getElementById('accelValue').textContent = data.accel.toFixed(2);
              document.getElementById('waterValue').textContent = data.water.toFixed(1);
              document.getElementById('gasValue').textContent = data.gas;
              
              // Update alert status with animations
              const status = data.status.toUpperCase();
              
              // Reset all alerts
              document.getElementById('earthquakeAlert').className = 'alert-item';
              document.getElementById('floodAlert').className = 'alert-item';
              document.getElementById('gasAlert').className = 'alert-item';
              document.querySelector('#earthquakeAlert .alert-icon').className = 'alert-icon earthquake';
              document.querySelector('#floodAlert .alert-icon').className = 'alert-icon flood';
              document.querySelector('#gasAlert .alert-icon').className = 'alert-icon gas';
              document.getElementById('earthquakeStatus').className = 'alert-state';
              document.getElementById('floodStatus').className = 'alert-state';
              document.getElementById('gasStatus').className = 'alert-state';
              
              // Earthquake status
              if (status.includes('EARTHQUAKE') && status.includes('ALERT')) {
                document.getElementById('earthquakeAlert').className = 'alert-item alert-active';
                document.querySelector('#earthquakeAlert .alert-icon').className = 'alert-icon earthquake alert-active';
                document.getElementById('earthquakeStatus').textContent = status.includes('TEST') ? 'TEST ALERT ACTIVE' : 'ACTIVE ALERT';
                document.getElementById('earthquakeStatus').className = 'alert-state active';
              } else {
                document.getElementById('earthquakeStatus').textContent = 'Monitoring';
              }
              
              // Flood status
              if (status.includes('FLOOD') && status.includes('ALERT')) {
                document.getElementById('floodAlert').className = 'alert-item alert-active';
                document.querySelector('#floodAlert .alert-icon').className = 'alert-icon flood alert-active';
                document.getElementById('floodStatus').textContent = status.includes('TEST') ? 'TEST ALERT ACTIVE' : 'ACTIVE ALERT';
                document.getElementById('floodStatus').className = 'alert-state active';
              } else {
                document.getElementById('floodStatus').textContent = 'Monitoring';
              }
              
              // Gas status
              if (status.includes('GAS') && status.includes('ALERT')) {
                document.getElementById('gasAlert').className = 'alert-item alert-active';
                document.querySelector('#gasAlert .alert-icon').className = 'alert-icon gas alert-active';
                document.getElementById('gasStatus').textContent = status.includes('TEST') ? 'TEST ALERT ACTIVE' : 'ACTIVE ALERT';
                document.getElementById('gasStatus').className = 'alert-state active';
              } else {
                document.getElementById('gasStatus').textContent = 'Monitoring';
              }
            })
            .catch(error => console.error('Error:', error));
        }
        setInterval(updateSensorData, 3000);
        updateSensorData();
        
        function testAlert(type) {
          const action = 'test_' + type;
          fetch('/api', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: action })
          })
          .then(response => response.json())
          .then(data => {
            alert('Test ' + type + ' alert triggered successfully!');
          })
          .catch(error => {
            alert('Failed to trigger test alert');
          });
        }
        
        function emergencyStop() {
          if (confirm('Are you sure you want to stop all alerts?')) {
            fetch('/api', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ action: 'emergency_stop' })
            })
            .then(response => response.json())
            .then(data => {
              alert('Emergency stop activated successfully!');
            })
            .catch(error => {
              alert('Failed to activate emergency stop');
            });
          }
        }
        
        function showAlertLog() {
          document.getElementById('logModal').style.display = 'block';
          fetch('/api', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'get_log' })
          })
          .then(response => response.json())
          .then(data => {
            document.getElementById('logContent').textContent = data.log || 'No alerts logged yet.';
          })
          .catch(error => {
            document.getElementById('logContent').textContent = 'Failed to load alert log.';
          });
        }
        
        function closeModal() {
          document.getElementById('logModal').style.display = 'none';
        }
        
        function logout() {
          if (confirm('Are you sure you want to logout?')) {
            window.location.href = '/';
          }
        }
        
        function openEmergencyNavigation() {
          if ("geolocation" in navigator) {
            navigator.geolocation.getCurrentPosition(function(position) {
              const lat = position.coords.latitude;
              const lng = position.coords.longitude;
              window.open('https://www.google.com/maps/dir/?api=1&origin=' + lat + ',' + lng + '&destination=opp+infosys+stp+campus+gate+2+Gachibowli,Hyderabad,Telangana+500032&travelmode=driving', '_blank');
            }, function() {
              window.open('https://www.google.com/maps/dir/?api=1&destination=opp+infosys+stp+campus+gate+2+Gachibowli,Hyderabad,Telangana+500032&travelmode=driving', '_blank');
            });
          } else {
            window.open('https://www.google.com/maps/dir/?api=1&destination=opp+infosys+stp+campus+gate+2+Gachibowli,Hyderabad,Telangana+500032&travelmode=driving', '_blank');
          }
        }
        
        window.onclick = function(event) {
          const modal = document.getElementById('logModal');
          if (event.target == modal) {
            modal.style.display = 'none';
          }
        }
      </script>
    </body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}



void handle_root() {
  String html = R"rawliteral(
    <!DOCTYPE html><html lang="en"><head>
      <meta charset="UTF-8" />
      <title>Smart Disaster Detection System</title>
      <meta name="viewport" content="width=device-width, initial-scale=1.0" />
      <style>
        body { background: #181f2a; color: #fff; font-family: 'Segoe UI',sans-serif; margin: 0; padding-bottom: 160px; }
        .header { background: #2986f9; padding: 25px 20px 18px 20px; display: flex; align-items: center; justify-content: space-between; position: relative; }
        .header-content { display: flex; align-items: center; justify-content: center; gap: 18px; flex: 1; }
        .header-icon { background: rgba(255,255,255,0.085); border-radius: 50%; width: 64px; height: 64px; display: flex; align-items: center; justify-content: center;}
        .header-icon svg { width: 38px; height: 38px; fill: #fff;}
        .header-txt { text-align: left;}
        .header-title { font-size: 2.2em; font-weight: 700; margin-bottom: 2px; letter-spacing: 0.02em;}
        .header-sub { font-size: 1.1em; font-weight: 400; color: #e9eaf3; margin-top: 0;}
        .header-right { position: relative; }
        .menu-btn { background: rgba(255,255,255,0.15); border: none; border-radius: 10px; padding: 12px 18px; color: #fff; cursor: pointer; font-size: 16px; font-weight: 500; transition: all 0.3s ease; box-shadow: 0 2px 8px rgba(0,0,0,0.2); }
        .menu-btn:hover { background: rgba(255,255,255,0.25); transform: translateY(-2px); box-shadow: 0 4px 12px rgba(0,0,0,0.3); }
        .dropdown { position: relative; }
        .dropdown-content { display: none; position: absolute; right: 0; background: #232e41; min-width: 220px; box-shadow: 0px 8px 16px rgba(0,0,0,0.3); z-index: 1000; border-radius: 10px; overflow: hidden; top: 100%; margin-top: 5px; }
        .dropdown-content a { color: #fff; padding: 14px 20px; text-decoration: none; display: block; transition: background 0.3s; font-weight: 500; }
        .dropdown-content a:hover { background: #2986f9; }
        .show { display: block; animation: dropdownSlide 0.3s ease; }
        @keyframes dropdownSlide { from { opacity: 0; transform: translateY(-10px); } to { opacity: 1; transform: translateY(0); } }
        .status { margin: 24px 0 0 0; text-align: center;}
        .chip { display: inline-block; padding: 8px 20px; border-radius: 20px; background: #192c23; color: #5ae36f; font-weight: bold; font-size: 18px; }
        .verification-badge { display: inline-block; padding: 6px 12px; border-radius: 12px; background: #1a4d2e; color: #4ade80; font-size: 14px; margin-left: 10px; }
        .live-indicator { display: inline-block; width: 8px; height: 8px; background: #5ae36f; border-radius: 50%; margin-right: 8px; animation: pulse 2s infinite; }
        @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.5; } 100% { opacity: 1; } }
        .dashboard { display: flex; justify-content: center; gap: 30px; margin: 40px 20px 60px 20px; flex-wrap: wrap; }
        .card { background: #232e41; border-radius: 17px; box-shadow: 0 2px 14px #10151e66; padding: 30px 35px; min-width: 220px; flex: 1 1 280px; margin-bottom: 30px;}
        .main-value { font-size: 2.6em; font-weight: 600; margin: 20px 0 8px 0;}
        .unit { font-size: 0.6em; color: #b9bbbf;}
        .threshold { margin-top: 14px; color: #8e98a0; font-size: 1.1em;}
        .thresh-ok { color: #5ba0ff; } .thresh-warn { color: #f4d35e; } .thresh-critical { color: #5ae36f; }
        .alert-section { margin: 32px 20px 40px 20px; padding: 0 20px;}
        .alert-title { display: flex; align-items: center; font-size: 1.18em; font-weight: 600; margin-bottom: 16px; color: #fff;}
        .alert-title svg { width: 20px; height: 20px; margin-right: 7px; fill: #ef5350;}
        .alerts { display: flex; gap: 22px; flex-wrap: wrap; justify-content: center;}
        .alert-card { background: #232a38; border-radius: 11px; padding: 24px 32px 20px 22px; display: flex; align-items: center; min-width: 260px; border: 2px solid transparent; box-sizing: border-box; margin-bottom: 30px; transition: all 0.3s ease; }
        .alert-active { border-color: #ef5350; animation: alertPulse 1.5s infinite; background: rgba(239,83,80,0.1); }
        @keyframes alertPulse { 0%, 100% { box-shadow: 0 0 0 0 rgba(239,83,80,0.7); } 50% { box-shadow: 0 0 0 10px rgba(239,83,80,0); } }
        .alert-false { border-color: #f59e0b; background: #1a1611;}
        .alert-icon { margin-right: 20px; display: flex; align-items: center; justify-content: center; width: 44px; height: 44px; border-radius: 100%; background: rgba(255,255,255,0.04); position: relative;}
        .alert-icon.alert-active { animation: iconShake 0.8s infinite; }
        @keyframes iconShake { 0%, 100% { transform: translateX(0); } 10%, 30%, 50%, 70%, 90% { transform: translateX(-2px); } 20%, 40%, 60%, 80% { transform: translateX(2px); } }
        .icon-earthquake { color: #ffda65;}
        .icon-flood { color: #63a8ff;}
        .icon-gas { color: #31f47e;}
        .alert-info { text-align: left;}
        .alert-label { font-size: 1.12em; font-weight: 500; margin-bottom: 5px;}
        .alert-status { font-size: 0.98em;}
        .alert-active-txt { color: #ef5350; font-weight: 700; animation: textBlink 1s infinite; }
        @keyframes textBlink { 0%, 50% { opacity: 1; } 51%, 100% { opacity: 0.7; } }
        .alert-ok-txt { color: #828696;}
        .alert-false-txt { color: #f59e0b; font-weight: 600;}
        .map-nav-fixed { position: fixed; left: 20px; bottom: 20px; z-index: 1000; display: flex; flex-direction: column; align-items: center; }
        .map-nav-btn { background: none; border: none; outline: none; cursor: pointer; padding: 0; display: inline-block; border-radius: 50%; transition: box-shadow 0.2s; box-shadow: 0 4px 18px #2986f966; width: 54px; height: 54px; }
        .map-nav-btn:active { box-shadow: 0 2px 9px #2986f966; }
        .map-nav-icon { width: 48px; height: 48px; fill: #2986f9;}
        .map-nav-label { color: #4cc2ff; padding-top: 4px; font-size: 1.04em; margin-top: 4px; text-align: center;}
        .last-update { position: fixed; top: 10px; left: 10px; background: rgba(0,0,0,0.7); padding: 5px 10px; border-radius: 5px; font-size: 12px; color: #fff; z-index: 999; }
        .emergency-contact { background: #2d1b1b; border: 2px solid #ef5350; border-radius: 15px; padding: 20px; margin: 40px 20px 20px 20px; text-align: center; position: relative; }
        .emergency-title { color: #ef5350; font-size: 1.4em; font-weight: 700; margin-bottom: 15px; display: flex; align-items: center; justify-content: center; gap: 10px; }
        .emergency-title svg { width: 24px; height: 24px; fill: #ef5350; }
        .emergency-contacts { display: flex; justify-content: center; gap: 30px; flex-wrap: wrap; }
        .contact-item { display: flex; align-items: center; gap: 10px; background: rgba(239, 83, 80, 0.1); padding: 12px 20px; border-radius: 10px; transition: background 0.3s ease; }
        .contact-item:hover { background: rgba(239, 83, 80, 0.2); }
        .contact-icon { width: 20px; height: 20px; fill: #ef5350; }
        .contact-link { color: #fff; text-decoration: none; font-weight: 500; font-size: 1.1em; }
        .contact-link:hover { color: #ef5350; }
        @media (max-width: 800px) { 
          .dashboard { flex-direction: column; align-items: center; margin: 40px 10px 60px 10px; } 
          .card { min-width: 90vw; } 
          .alerts { flex-direction: column; align-items: center; }
          .map-nav-fixed { left: 12px; bottom: 12px; }
          .emergency-contacts { flex-direction: column; align-items: center; gap: 15px; }
          .emergency-contact { margin: 40px 10px 20px 10px; }
          .header-content { flex-direction: column; gap: 10px; }
          .header-title { font-size: 1.8em; text-align: center; }
          .header { padding: 20px 15px 15px 15px; }
        }
      </style>
    </head>
    <body>
      <div class="last-update" id="lastUpdate">Last Update: --</div>
      <div class="header">
        <div class="header-content">
          <div class="header-icon">
            <svg viewBox="0 0 48 48"><path d="M22 44v-9.9l-8.65 3.3L13 35.4l2.4-9.45H8.05l13.35-23v9.9L30.05 4.7 35 12.55l-2.4 9.45H39.9Z"/></svg>
          </div>
          <div class="header-txt">
            <div class="header-title">Smart Disaster Detection System</div>
            <div class="header-sub">ESP32 with AI-powered API verification</div>
          </div>
        </div>
        <div class="header-right">
          <div class="dropdown">
            <button class="menu-btn" onclick="toggleDropdown()">Menu ⋮</button>
            <div class="dropdown-content" id="menuDropdown">
              <a href="/login">🏛️ Government Rescue Center</a>
            </div>
          </div>
        </div>
      </div>
      
      <div class="status">
        <span class="chip"><span class="live-indicator"></span>Status: <span id="statustext" style="color:#57e47d;">MONITORING</span></span>
        <span class="verification-badge">API-Verified Alerts</span>
      </div>
      
      <div class="alert-section">
        <div class="alert-title">
          <svg viewBox="0 0 24 24"><path d="M12.5 21q-.625 0-1.062-.438Q11 20.125 11 19.5q0-.625.438-1.062.437-.438 1.062-.438.625 0 1.062.438Q14 18.875 14 19.5q0 .625-.438 1.062Q13.125 21 12.5 21Zm0-4q-.425 0-.713-.288Q11.5 16.425 11.5 16V8q0-.425.287-.713Q12.075 7 12.5 7q.425 0 .713.287.287.288.287.713v8q0 .425-.287.712Q12.925 17 12.5 17Z"/></svg>
          Verified Alert Status
        </div>
        <div class="alerts">
          <div class="alert-card" id="eq">
            <span class="alert-icon icon-earthquake" id="eqIcon">
              <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="11" fill="none" stroke="currentColor" stroke-width="2"/><line x1="12" y1="7" x2="12" y2="17" stroke="currentColor" stroke-width="2"/><line x1="7" y1="12" x2="17" y2="12" stroke="currentColor" stroke-width="2"/></svg>
            </span>
            <span class="alert-info">
              <div class="alert-label">Earthquake Detection</div>
              <div class="alert-status alert-ok-txt" id="eq-status">No threats detected</div>
            </span>
          </div>
          <div class="alert-card" id="flood">
            <span class="alert-icon icon-flood" id="floodIcon">
              <svg viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16" rx="4" fill="none" stroke="currentColor" stroke-width="2"/><circle cx="12" cy="12" r="2" fill="currentColor"/></svg>
            </span>
            <span class="alert-info">
              <div class="alert-label">Flood Detection</div>
              <div class="alert-status alert-ok-txt" id="flood-status">No threats detected</div>
            </span>
          </div>
          <div class="alert-card" id="gas">
            <span class="alert-icon icon-gas" id="gasIcon">
              <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="11" fill="none" stroke="currentColor" stroke-width="2"/><path d="M8 16c2-4 6-4 8 0" fill="none" stroke="currentColor" stroke-width="2"/><circle cx="12" cy="10" r="1" fill="currentColor"/></svg>
            </span>
            <span class="alert-info">
              <div class="alert-label">Gas Leak Detection</div>
              <div class="alert-status alert-ok-txt" id="gas-status">No threats detected</div>
            </span>
          </div>
        </div>
      </div>
      
      <div class="dashboard">
        <div class="card">
          <div>Accelerometer (MPU6050)</div>
          <div class="main-value"><span id="accelval">--</span><span class="unit"> g</span></div>
          <div class="threshold">Threshold: <span class="thresh-warn">1.5 g</span></div>
        </div>
        <div class="card">
          <div>Water Level (Ultrasonic)</div>
          <div class="main-value"><span id="waterval">--</span><span class="unit"> cm</span></div>
          <div class="threshold">Threshold: <span class="thresh-ok">15.0 cm</span></div>
        </div>
        <div class="card">
          <div>Gas Level (MQ135)</div>
          <div class="main-value"><span id="gasval">--</span><span class="unit"> ppm</span></div>
          <div class="threshold">Threshold: <span class="thresh-critical">2000 ppm</span></div>
        </div>
      </div>
      
      <div class="emergency-contact">
        <div class="emergency-title">
          <svg viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 15h2v2h-2v-2zm0-8h2v6h-2V9z"/></svg>
          SOS Emergency Contacts
        </div>
        <div class="emergency-contacts">
          <div class="contact-item">
            <svg class="contact-icon" viewBox="0 0 24 24"><path d="M6.62 10.79c1.44 2.83 3.76 5.14 6.59 6.59l2.2-2.2c.27-.27.67-.36 1.02-.24 1.12.37 2.33.57 3.57.57.55 0 1 .45 1 1V20c0 .55-.45 1-1 1-9.39 0-17-7.61-17-17 0-.55.45-1 1-1h3.5c.55 0 1 .45 1 1 0 1.25.2 2.45.57 3.57.11.35.03.74-.25 1.02l-2.2 2.2z"/></svg>
            <a href="tel:8618552566" class="contact-link">8618552566</a>
          </div>
          <div class="contact-item">
            <svg class="contact-icon" viewBox="0 0 24 24"><path d="M20 4H4c-1.1 0-1.99.9-1.99 2L2 18c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V6c0-1.1-.9-2-2-2zm0 4l-8 5-8-5V6l8 5 8-5v2z"/></svg>
            <a href="mailto:loginothers@gmail.com" class="contact-link">loginothers@gmail.com</a>
          </div>
        </div>
      </div>
      
      <div class="map-nav-fixed">
        <button class="map-nav-btn" id="mapNavBtn" title="Emergency Navigation">
          <svg class="map-nav-icon" viewBox="0 0 24 24"><path d="M12 2C6.477 2 2 6.478 2 12c0 5.502 4.477 10 10 10s10-4.498 10-10c0-5.522-4.477-10-10-10zm0 17.2a7.2 7.2 0 1 1 0-14.4 7.2 7.2 0 0 1 0 14.4zm0-10.4a1.2 1.2 0 1 1 0 2.4 1.2 1.2 0 0 1 0-2.4zm0 3.2c-2.208 0-4 1.791-4 4 0 .662.27 1.26.703 1.699l.848.849 2.449 2.449c.155.155.358.24.571.24.212 0 .416-.085.57-.24l2.45-2.449.848-.849A2 2 0 0 0 16 16c0-2.209-1.792-4-4-4z"/></svg>
        </button>
        <div class="map-nav-label">Emergency Navigation</div>
      </div>
      
      <script>
        function toggleDropdown() {
          document.getElementById("menuDropdown").classList.toggle("show");
        }
        
        window.onclick = function(event) {
          if (!event.target.matches('.menu-btn')) {
            var dropdowns = document.getElementsByClassName("dropdown-content");
            for (var i = 0; i < dropdowns.length; i++) {
              var openDropdown = dropdowns[i];
              if (openDropdown.classList.contains('show')) {
                openDropdown.classList.remove('show');
              }
            }
          }
        }
        
        function updateVals() {
          fetch('/api')
            .then(resp => resp.json())
            .then(data => {
              document.getElementById('accelval').textContent = data.accel.toFixed(2);
              document.getElementById('waterval').textContent = data.water.toFixed(1);
              document.getElementById('gasval').textContent = data.gas;
              document.getElementById('statustext').textContent = data.status;
              document.getElementById('lastUpdate').textContent = 'Last Update: ' + new Date().toLocaleTimeString();
              
              let status = data.status.toUpperCase();
              
              // Reset all alert states
              document.getElementById('eq').className = 'alert-card';
              document.getElementById('flood').className = 'alert-card';
              document.getElementById('gas').className = 'alert-card';
              document.getElementById('eqIcon').className = 'alert-icon icon-earthquake';
              document.getElementById('floodIcon').className = 'alert-icon icon-flood';
              document.getElementById('gasIcon').className = 'alert-icon icon-gas';
              
              if (status.includes('EARTHQUAKE')) {
                if (status.includes('ALERT')) {
                  document.getElementById('eq').classList.add('alert-active');
                  document.getElementById('eqIcon').classList.add('alert-active');
                  document.getElementById('eq-status').textContent = status.includes('TEST') ? 'TEST ALERT ACTIVE' : 'VERIFIED THREAT';
                  document.getElementById('eq-status').className = 'alert-status alert-active-txt';
                } else if (status.includes('FALSE')) {
                  document.getElementById('eq').classList.add('alert-false');
                  document.getElementById('eq-status').textContent = 'False positive resolved';
                  document.getElementById('eq-status').className = 'alert-status alert-false-txt';
                } else {
                  document.getElementById('eq-status').textContent = 'Verifying with USGS...';
                  document.getElementById('eq-status').className = 'alert-status alert-false-txt';
                }
              } else {
                document.getElementById('eq-status').textContent = 'No threats detected';
                document.getElementById('eq-status').className = 'alert-status alert-ok-txt';
              }
              
              if (status.includes('FLOOD')) {
                if (status.includes('ALERT')) {
                  document.getElementById('flood').classList.add('alert-active');
                  document.getElementById('floodIcon').classList.add('alert-active');
                  document.getElementById('flood-status').textContent = status.includes('TEST') ? 'TEST ALERT ACTIVE' : 'VERIFIED THREAT';
                  document.getElementById('flood-status').className = 'alert-status alert-active-txt';
                } else if (status.includes('FALSE')) {
                  document.getElementById('flood').classList.add('alert-false');
                  document.getElementById('flood-status').textContent = 'False positive resolved';
                  document.getElementById('flood-status').className = 'alert-status alert-false-txt';
                } else {
                  document.getElementById('flood-status').textContent = 'Verifying with weather APIs...';
                  document.getElementById('flood-status').className = 'alert-status alert-false-txt';
                }
              } else {
                document.getElementById('flood-status').textContent = 'No threats detected';
                document.getElementById('flood-status').className = 'alert-status alert-ok-txt';
              }
              
              if (status.includes('GAS')) {
                if (status.includes('ALERT')) {
                  document.getElementById('gas').classList.add('alert-active');
                  document.getElementById('gasIcon').classList.add('alert-active');
                  document.getElementById('gas-status').textContent = status.includes('TEST') ? 'TEST ALERT ACTIVE' : 'VERIFIED THREAT';
                  document.getElementById('gas-status').className = 'alert-status alert-active-txt';
                } else if (status.includes('FALSE')) {
                  document.getElementById('gas').classList.add('alert-false');
                  document.getElementById('gas-status').textContent = 'False positive resolved';
                  document.getElementById('gas-status').className = 'alert-status alert-false-txt';
                } else {
                  document.getElementById('gas-status').textContent = 'Verifying with air quality APIs...';
                  document.getElementById('gas-status').className = 'alert-status alert-false-txt';
                }
              } else {
                document.getElementById('gas-status').textContent = 'No threats detected';
                document.getElementById('gas-status').className = 'alert-status alert-ok-txt';
              }
            })
            .catch(error => {
              console.error('Error fetching data:', error);
              document.getElementById('lastUpdate').textContent = 'Connection Error';
            });
        }
        
        updateVals();
        setInterval(updateVals, 10000);
        
        document.getElementById('mapNavBtn').onclick = function() {
          function openDirections(lat, lng) {
            window.open('https://www.google.com/maps/dir/?api=1&origin=' + lat + ',' + lng + '&destination=opp+infosys+stp+campus+gate+2+Gachibowli,Hyderabad,Telangana+500032&travelmode=driving', '_blank');
          }
          if ("geolocation" in navigator) {
            navigator.geolocation.getCurrentPosition(function(position) {
              openDirections(position.coords.latitude, position.coords.longitude);
            }, function() {
              fetch('https://ip-api.io/json')
                .then(res => res.json())
                .then(data => {
                  if (data && data.latitude && data.longitude) {
                    openDirections(data.latitude, data.longitude);
                  } else {
                    window.open('https://www.google.com/maps/dir/?api=1&destination=opp+infosys+stp+campus+gate+2+Gachibowli,Hyderabad,Telangana+500032&travelmode=driving', '_blank');
                  }
                })
                .catch(() => window.open('https://www.google.com/maps/dir/?api=1&destination=opp+infosys+stp+campus+gate+2+Gachibowli,Hyderabad,Telangana+500032&travelmode=driving', '_blank'));
            });
          } else {
            window.open('https://www.google.com/maps/dir/?api=1&destination=opp+infosys+stp+campus+gate+2+Gachibowli,Hyderabad,Telangana+500032&travelmode=driving', '_blank');
          }
        };
      </script>
    </body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}


// === Sensor Functions ===
float measureDistance() {
  digitalWrite(TRIG_PIN, LOW); 
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); 
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;
  return (duration * 0.0343) / 2.0;
}

void updateSensorsForWeb() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float ax = a.acceleration.x / 9.81;
  float ay = a.acceleration.y / 9.81;
  float az = a.acceleration.z / 9.81;
  last_accel = sqrt(ax * ax + ay * ay + az * az);
  last_gas = analogRead(MQ135_PIN);
  last_water = measureDistance();
  
  if (!manualAlert) {
    switch(currentState){
      case MONITORING: last_status = "MONITORING"; break;
      case EARTHQUAKE_WAIT: last_status = "EARTHQUAKE VERIFYING"; break;
      case WATER_WAIT: last_status = "FLOOD VERIFYING"; break;
      case GAS_WAIT: last_status = "GAS VERIFYING"; break;
      case EARTHQUAKE_ALERT: last_status = "EARTHQUAKE ALERT!"; break;
      case WATER_ALERT: last_status = "FLOOD ALERT!"; break;
      case GAS_ALERT: last_status = "GAS ALERT!"; break;
      case FALSE_POSITIVE: last_status = "FALSE POSITIVE RESOLVED"; break;
      default: last_status = "MONITORING";
    }
  }
}

void monitorSensors() {
  if (manualAlert) return;
  
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float ax = a.acceleration.x / 9.81;
  float ay = a.acceleration.y / 9.81;
  float az = a.acceleration.z / 9.81;
  float totalAccel = sqrt(ax * ax + ay * ay + az * az);

  int gasValue = analogRead(MQ135_PIN);
  float distance_cm = measureDistance();

  if (totalAccel > earthquake_threshold) {
    Serial.println("⚠ Earthquake motion detected - Verifying with USGS API...");
    currentState = EARTHQUAKE_WAIT;
    waitStartTime = millis();
    addToLog("Earthquake motion detected - verifying through API...");
    return;
  }
  if (gasValue > gas_threshold) {
    Serial.println("⚠ Gas levels high - Verifying with air quality APIs...");
    currentState = GAS_WAIT;
    waitStartTime = millis();
    addToLog("High gas levels detected - verifying...");
    return;
  }
  if (distance_cm > 0 && distance_cm < flood_threshold_cm) {
    Serial.println("⚠ Flood risk detected - Verifying with weather APIs...");
    currentState = WATER_WAIT;
    waitStartTime = millis();
    addToLog("Flood risk detected - verifying through API...");
    return;
  }

  static unsigned long lastLogTime = 0;
  if (millis() - lastLogTime >= updateInterval) {
    lastLogTime = millis();
    Serial.printf("Monitoring - Accel: %.2f g | Gas: %d | Distance: %.2f cm\n", totalAccel, gasValue, distance_cm);
  }
}

void resetSystem() {
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  currentState = MONITORING;
  ledState = false;
  manualAlert = false;
  addToLog("System reset - monitoring resumed");
  Serial.println("System reset. Monitoring resumed.");
}

void sendSMS(String messageBody) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "https://api.twilio.com/2010-04-01/Accounts/" + ACCOUNT_SID + "/Messages.json";
    http.begin(url);
    http.setAuthorization(ACCOUNT_SID.c_str(), AUTH_TOKEN.c_str());
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String data = "From=" + FROM_NUMBER + "&To=" + TO_NUMBER + "&Body=" + messageBody;
    int httpCode = http.POST(data);
    if (httpCode > 0) Serial.println("✅ SMS sent");
    else Serial.println("❌ SMS failed");
    http.end();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(MQ135_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  Wire.begin();
  if (!mpu.begin()) {
    Serial.println("MPU6050 not found");
    while(true);
  }
  Serial.println("MPU6050 initialized");

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handle_root);
  server.on("/api", handle_api);
  server.on("/login", handle_login);
  server.on("/rescue", handle_rescue);
  server.begin();
  
  // Send initial email notification
  sendEmailAlert("🚀Disaster System Initialized", "Your ESP32 Disaster Detection System has been successfully initialized at location Guwahati ASSAM and is now monitoring for disasters. Both SMS and Email Alert notifications are active.");
  
  addToLog("System initialized successfully");
  Serial.println("🚀 Smart Disaster Detection System Ready");
  Serial.println("API verification enabled");
  Serial.println("Government Rescue Center portal enabled");
  Serial.println("📱 Login credentials : rescue / rescue_saver");
  Serial.println("📧 Email notification system enabled");
}

void loop() {
  server.handleClient();

  if (millis() - lastWebUpdate > 10000) {
    updateSensorsForWeb();
    lastWebUpdate = millis();
  }

  bool currentButtonState = digitalRead(BUTTON_PIN) == LOW;
  if (currentButtonState && !lastButtonState) buttonPressed = true;
  lastButtonState = currentButtonState;

  switch(currentState) {
    case MONITORING:
      monitorSensors();
      break;

    case EARTHQUAKE_WAIT:
      if (millis() - waitStartTime >= 3000) {
        Serial.println("🔍 Verifying earthquake with USGS API...");
        if (verifyEarthquake()) {
          Serial.println("✅ Earthquake VERIFIED by USGS!");
          sendSMS("🚨 VERIFIED Earthquake Alert! Location: Guwahati Assam (PALTAN BAZAR MARKET). MOVE TO NEARBY CENTER IMMEDIATELY ");
          sendEmailAlert("🚨 VERIFIED Earthquake Alert", "A verified earthquake has been detected in Guwahati ASSAM PALTAN BAZAR MARKET . Please contact the rescue center");
          addToLog("VERIFIED Earthquake Alert - USGS confirmed");
          currentState = EARTHQUAKE_ALERT;
        } else {
          Serial.println("❌ Earthquake NOT verified by USGS API verification - False positive");
          addToLog("Earthquake false positive - USGS verification failed");
          currentState = FALSE_POSITIVE;
          delay(2000);
          resetSystem();
        }
      }
      break;

    case WATER_WAIT:
      if (millis() - waitStartTime >= 3000) {
        Serial.println("🔍 Verifying flood with weather APIs...");
        if (verifyFlood()) {
          Serial.println("✅ Flood VERIFIED by weather data!");
          sendSMS("🚨 VERIFIED Flood Alert! Location: Dharwad. Move to higher ground.");
          sendEmailAlert("🚨 VERIFIED Flood Alert", "A verified flood risk has been detected in Dharwad area. Please move to higher ground immediately and avoid flooded areas.");
          addToLog("VERIFIED Flood Alert - weather data confirmed");
          currentState = WATER_ALERT;
          lastBlinkTime = millis();
        } else {
          Serial.println("❌ Flood NOT verified by weather APIs - False positive");
          addToLog("Flood false positive - weather verification failed");
          currentState = FALSE_POSITIVE;
          delay(2000);
          resetSystem();
        }
      }
      break;

    case GAS_WAIT:
      if (millis() - waitStartTime >= 3000) {
        Serial.println("🔍 Verifying gas leak with air quality APIs...");
        if (verifyGasLeak()) {
          Serial.println("✅ Gas leak VERIFIED by air quality data!");
          sendSMS("🚨 VERIFIED Gas Leak Alert! Location: Dharwad. Evacuate area immediately.");
          sendEmailAlert("🚨 VERIFIED Gas Leak Alert", "A verified gas leak has been detected in Dharwad area. Please evacuate the area immediately and avoid any ignition sources.");
          addToLog("VERIFIED Gas Leak Alert - air quality confirmed");
          currentState = GAS_ALERT;
          lastBlinkTime = millis();
        } else {
          Serial.println("❌ Gas leak NOT verified by air quality APIs - False positive");
          addToLog("Gas leak false positive - air quality verification failed");
          currentState = FALSE_POSITIVE;
          delay(2000);
          resetSystem();
        }
      }
      break;

    case EARTHQUAKE_ALERT:
      digitalWrite(BUZZER_PIN, HIGH);
      digitalWrite(LED_PIN, HIGH);
      if (buttonPressed) {
        Serial.println("✅ Earthquake alert reset by user");
        sendSMS("✅ Earthquake Alert Stopped by user.");
        sendEmailAlert("✅ Earthquake Alert Stopped", "The earthquake alert has been stopped by user intervention.");
        addToLog("Earthquake alert stopped by user button");
        resetSystem();
      }
      break;

    case WATER_ALERT:
      if (millis() - lastBlinkTime >= 1000) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
        digitalWrite(BUZZER_PIN, ledState);
        lastBlinkTime = millis();
      }
      if (buttonPressed) {
        Serial.println("✅ Flood alert reset by user");
        sendSMS("✅ Flood Alert Stopped by user.");
        sendEmailAlert("✅ Flood Alert Stopped", "The flood alert has been stopped by user intervention.");
        addToLog("Flood alert stopped by user button");
        resetSystem();
      }
      break;

    case GAS_ALERT:
      if (millis() - lastBlinkTime >= 2000) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
        digitalWrite(BUZZER_PIN, ledState);
        lastBlinkTime = millis();
      }
      if (buttonPressed) {
        Serial.println("✅ Gas alert reset by rescue center");
        sendSMS("✅ Gas Alert Stopped by rescue center");
        sendEmailAlert("✅ Gas Alert Stopped", "The gas leak alert has been stopped by rescue center ");
        addToLog("Gas alert stopped by user button");
        resetSystem();
      }
      break;

    case FALSE_POSITIVE:
      break;
  }
  buttonPressed = false;
  delay(100);
}

