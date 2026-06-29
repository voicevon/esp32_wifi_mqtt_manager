#include <Arduino.h>
#include <ESP32WifiMqttManager.h>

// 实例化底层 TCP 客户端和网络管理器
WiFiClient espClient;
ESP32WifiMqttManager netManager(espClient);

// 定义板载指示灯引脚 (ESP32 DevKit 常用 GPIO 2)
#define STATUS_LED_PIN 2

// LED 状态反转控制变量
unsigned long lastBlinkMs = 0;
bool ledState = false;

// MQTT 接收消息回调
void mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT RX] Topic: %s, Length: %u\n", topic, length);
    
    char payloadStr[64];
    unsigned int len = length < 63 ? length : 63;
    memcpy(payloadStr, payload, len);
    payloadStr[len] = '\0';
    Serial.printf("[MQTT RX] Payload: %s\n", payloadStr);
}

// 网络状态改变回调
void networkStateCallback(NetworkState state) {
    Serial.printf("[System] Network State changed to: %d\n", state);
    
    // 当 MQTT 成功连通时，自动订阅相关主题
    if (state == STATE_MQTT_CONNECTED) {
        netManager.subscribe("test/led/control");
        Serial.println("[System] Subscribed to topic: test/led/control");
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n====================================");
    Serial.println("ESP32 Non-blocking Network Demo Start");
    Serial.println("====================================");

    // 初始化状态指示灯
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    // 配置网络参数
    NetworkConfig config;
    config.wifiSsid = "Your_WiFi_SSID";
    config.wifiPassword = "Your_WiFi_Password";
    config.mqttBroker = "broker.hivemq.com"; // 默认测试公共 Broker，支持域名或 IP
    config.mqttPort = 1883;
    config.clientIdPrefix = "ESP32AsyncDemo";
    config.wifiReconnectIntervalMs = 20000;  // WiFi 断线重连检测周期 (20秒)
    config.mqttReconnectIntervalMs = 5000;   // MQTT 重连尝试检测周期 (5秒)
    config.useHttpDns = true;                // 启用 HTTP-DNS 防劫持解析

    // 注册回调并启动管理器
    netManager.onStateChange(networkStateCallback);
    netManager.setMqttCallback(mqttMessageCallback);
    netManager.begin(config);
}

void loop() {
    unsigned long now = millis();

    // 1. 驱动网络管理器循环（内部包含 WiFi 状态维护与后台 FreeRTOS 异步 MQTT 重连）
    netManager.loop();

    // 2. 状态指示灯逻辑（根据当前网络状态，以不同频率闪烁）
    unsigned long period = 0;
    NetworkState state = netManager.getState();
    
    if (state == STATE_MQTT_CONNECTED) {
        period = 1000; // WiFi 与 MQTT 均正常：1秒周期快闪 (0.5s 亮 / 0.5s 灭)
    } else if (state == STATE_WIFI_CONNECTED || state == STATE_MQTT_CONNECTING) {
        period = 2000; // 仅 WiFi 正常：2秒周期中闪 (1s 亮 / 1s 灭)
    } else {
        period = 0;    // WiFi 未连接：指示灯保持常灭
    }

    // 3. 执行无阻塞指示灯闪烁（证明主循环完全没有被网络握手或 DNS 解析卡死）
    if (period == 0) {
        digitalWrite(STATUS_LED_PIN, LOW);
    } else {
        if ((now % period) < (period / 2)) {
            digitalWrite(STATUS_LED_PIN, HIGH);
        } else {
            digitalWrite(STATUS_LED_PIN, LOW);
        }
    }

    // 4. 定时上报测试消息（可选，MQTT 正常连接后每 10 秒发送一次）
    static unsigned long lastPublishMs = 0;
    if (netManager.isConnected()) {
        if (now - lastPublishMs > 10000) {
            lastPublishMs = now;
            netManager.publish("test/status", "online");
            Serial.println("[MQTT TX] Published 'online' to topic 'test/status'");
        }
    }
}
