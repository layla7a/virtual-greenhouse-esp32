🌱 Virtual Smart Greenhouse — ESP32
A software-only IoT project running on ESP32-CAM. No sensors or hardware needed. The system simulates a complete smart greenhouse using MQTT and a built-in web dashboard.

What it does
Simulates soil moisture, light levels, and pest detection. Controls virtual irrigation, pest sprayer, and grow lights automatically or manually via MQTT or the web dashboard.

Features
Realistic sensor simulation with time-based algorithms
MQTT publishing every 20 seconds to broker.hivemq.com
Auto mode runs irrigation, sprayer, and grow light automatically
Manual mode lets you control everything from your phone or PC
Web dashboard with live gauges and moisture history chart
Works on bare ESP32 with no external components

MQTT Topics
Broker: broker.hivemq.com — Port: 1883
Sensors (published by ESP32): LMa_greenhouse/sensors/moisture, LMa_greenhouse/sensors/light, LMa_greenhouse/sensors/pest
Actuators (send ON or OFF): LMa_greenhouse/actuators/irrigation, LMa_greenhouse/actuators/sprayer, LMa_greenhouse/actuators/growlight
Mode (send auto or manual): LMa_greenhouse/mode

How to use
Edit the WiFi credentials and MQTT broker in the .ino file, flash to ESP32, open Serial Monitor at 115200 baud to get the IP address, then open that IP in your browser on the same WiFi network.

Libraries needed
PubSubClient, ESPAsyncWebServer
