# ESP32WifiMqttManager

基于 **FreeRTOS 后台多线程任务**实现的 ESP32 非阻塞异步 WiFi 与 MQTT 网络连接管理库。

---

## 🌟 核心特性

1. **彻底消除主线程阻塞**：
   传统的 WiFiClient / PubSubClient 网络库在域名解析（DNS / HTTP-DNS）及 TCP 握手重连时会产生长达 **5 ~ 15 秒** 的同步阻塞。本库将所有 DNS 解析与 MQTT 连接握手操作全部移入 FreeRTOS 后台任务独立执行，保证 Arduino 主线程 `loop()` 绝不卡顿（轮询率保持在微秒级）。
2. **完美支持前台实时业务**：
   在网络波动或 MQTT 重连重试期间，前台应用层业务（如按键扫描、LED 闪烁、传感器采样等）依然能够保持高频流畅响应，决不卡死。
3. **内置备用 HTTP-DNS 解析**：
   支持绕过本地 53 端口 DNS 劫持（Clash 等 Fake-IP 范围 `198.18.x.x`），并集成阿里云公共 HTTP-DNS 备用查询。
4. **多线程并发安全保护**：
   内置原子状态标志锁 `_isConnecting`。当后台连接任务正在运行时，会自动安全拦截并阻断前台主线程任何对 MQTT 实例的发送和订阅 API 调用（返回 `false`），规避并发竞态冲突。

---

## 📂 示例程序

库中提供了一个完整的无阻塞闪灯与连接的实例，您可以参考：
* [examples/BasicAsyncConnect/BasicAsyncConnect.ino](examples/BasicAsyncConnect/BasicAsyncConnect.ino)

---

## 🛠️ 快速接入指南

### 1. PlatformIO 依赖配置

在您项目的 `platformio.ini` 中加入依赖引用：

```ini
lib_deps =
    knolleary/PubSubClient @ ^2.8
    https://github.com/voicevon/esp32_wifi_mqtt_manager.git
```

### 2. 核心 API 介绍

#### 配置结构体 `NetworkConfig`
```cpp
struct NetworkConfig {
    const char* wifiSsid = nullptr;
    const char* wifiPassword = nullptr;
    const char* mqttBroker = nullptr;           // 支持域名或 IP
    uint16_t mqttPort = 1883;
    const char* mqttUsername = nullptr;         // 可选，无则传 nullptr
    const char* mqttPassword = nullptr;         // 可选，无则传 nullptr
    const char* clientIdPrefix = "ESP32Client";   // 客户端 Client ID 前缀
    uint32_t wifiReconnectIntervalMs = 20000;   // WiFi 断线重连检测周期 (默认 20s)
    uint32_t mqttReconnectIntervalMs = 5000;    // MQTT 重连尝试检测周期 (默认 5s)
    bool useHttpDns = true;                     // 是否启用阿里云备用 HTTP-DNS 解析
};
```

#### 类成员函数 `ESP32WifiMqttManager`
* `ESP32WifiMqttManager(Client& netClient)`: 构造函数，需要传入一个底层客户端（如 `WiFiClient`）。
* `void begin(const NetworkConfig& config)`: 传入配置并初始化网络连接。
* `void loop()`: 维持网络状态机与心跳的核心轮询，需要在主程序的 `loop()` 中非阻塞调用。
* `bool isConnected()`: 获取 WiFi 与 MQTT 是否同时连通。
* `NetworkState getState()`: 获取当前网络所处状态（共 5 种状态）。
* `void onStateChange(StateCallback cb)`: 注册网络状态变更回调。常用于在 `STATE_MQTT_CONNECTED` 状态下执行主题订阅操作。
* `void setMqttCallback(MqttMessageCallback cb)`: 注册 MQTT 接收消息的回调函数。
* `bool publish(const char* topic, const char* payload)`: 发布文本消息。
* `bool subscribe(const char* topic, uint8_t qos = 0)`: 订阅主题。
