#ifndef ESP32_WIFI_MQTT_MANAGER_H
#define ESP32_WIFI_MQTT_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <functional>
#include <atomic>

enum NetworkState {
    STATE_DISCONNECTED,      // 完全断开（初始状态或网络彻底丢失）
    STATE_WIFI_CONNECTING,   // WiFi 正在尝试连接
    STATE_WIFI_CONNECTED,    // WiFi 已连接成功，MQTT 未连接
    STATE_MQTT_CONNECTING,   // WiFi 已连接，MQTT 正在尝试连接
    STATE_MQTT_CONNECTED     // WiFi 与 MQTT 均已成功连接
};

struct NetworkConfig {
    const char* wifiSsid = nullptr;
    const char* wifiPassword = nullptr;
    const char* mqttBroker = nullptr;         // 支持域名或 IP
    uint16_t mqttPort = 1883;
    const char* mqttUsername = nullptr;       // 可选，无则传 nullptr
    const char* mqttPassword = nullptr;       // 可选，无则传 nullptr
    const char* clientIdPrefix = "ESP32Client"; // Client ID 前缀
    uint32_t wifiReconnectIntervalMs = 20000; // WiFi 断线重连检测周期（默认 20s）
    uint32_t mqttReconnectIntervalMs = 5000;  // MQTT 重连检测周期（默认 5s）
    bool useHttpDns = true;                   // 是否启用阿里云备用 HTTP-DNS 解析
};

class ESP32WifiMqttManager {
public:
    typedef std::function<void(NetworkState)> StateCallback;
    typedef std::function<void(char* topic, byte* payload, unsigned int length)> MqttMessageCallback;

    // 构造函数，需要传入一个 Client 实例（如 WiFiClient）以构造 PubSubClient
    ESP32WifiMqttManager(Client& netClient);

    // 初始化配置并开始连接流程
    void begin(const NetworkConfig& config);
    
    // 维持心跳与状态检测的核心循环，需在 Arduino 的 loop() 中非阻塞调用
    void loop();

    // 状态查询接口
    bool isConnected();       // WiFi 与 MQTT 均连通
    bool isWifiConnected();
    bool isMqttConnected();
    NetworkState getState() const;

    // 注册状态变化回调
    void onStateChange(StateCallback cb);

    // 注册 MQTT 消息接收回调
    void setMqttCallback(MqttMessageCallback cb);

    // 基础 MQTT 接口封装
    bool subscribe(const char* topic, uint8_t qos = 0);
    bool publish(const char* topic, const char* payload);
    bool publish(const char* topic, const uint8_t* payload, unsigned int plength, bool retained = false);

    // 流式大包（如图片）发布接口，直接映射 PubSubClient 的 Streaming API
    bool beginPublish(const char* topic, size_t totalLength, bool retained = false);
    size_t write(const uint8_t* buffer, size_t size);
    bool endPublish();

    // 获取底层 PubSubClient 实例
    PubSubClient& getMqttClient() { return _mqttClient; }

private:
    static ESP32WifiMqttManager* _instance;

    Client& _netClient;
    PubSubClient _mqttClient;
    NetworkConfig _config;
    NetworkState _state = STATE_DISCONNECTED;
    
    StateCallback _stateCb = nullptr;
    MqttMessageCallback _mqttCb = nullptr;

    uint32_t _lastWifiCheck = 0;
    uint32_t _lastMqttCheck = 0;
    IPAddress _resolvedBrokerIp;
    uint32_t _lastDnsResolveMs = 0;
    std::atomic<bool> _isConnecting{false};

    void updateState(NetworkState newState);
    void checkWiFi();
    void checkMQTT();
    IPAddress resolveBrokerIp(const char* host);
    static void staticMqttCallback(char* topic, byte* payload, unsigned int length);

    friend void asyncMqttConnectTask(void* pvParameters);
};

#endif // ESP32_WIFI_MQTT_MANAGER_H
