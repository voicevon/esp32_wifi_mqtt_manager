#include "ESP32WifiMqttManager.h"
#include <HTTPClient.h>

// 初始化静态实例指针
ESP32WifiMqttManager* ESP32WifiMqttManager::_instance = nullptr;

ESP32WifiMqttManager::ESP32WifiMqttManager(Client& netClient) 
    : _netClient(netClient), _mqttClient(netClient) {
    _instance = this;
}

void ESP32WifiMqttManager::begin(const NetworkConfig& config) {
    _config = config;

    // 默认设置服务器，回调静态函数
    _mqttClient.setCallback(staticMqttCallback);

    Serial.println("[NetManager] Initializing WiFiSTA mode...");
    WiFi.mode(WIFI_STA);

    // 触发首次 WiFi 连接
    WiFi.begin(_config.wifiSsid, _config.wifiPassword);
    updateState(STATE_WIFI_CONNECTING);
    _lastWifiCheck = millis();
}

void ESP32WifiMqttManager::loop() {
    // 1. 优先维护 WiFi 状态
    checkWiFi();

    wl_status_t wifiStatus = WiFi.status();

    if (wifiStatus == WL_CONNECTED) {
        // WiFi 已连接：若此前处于未连接状态，则更新为 STATE_WIFI_CONNECTED
        if (_state == STATE_DISCONNECTED || _state == STATE_WIFI_CONNECTING) {
            updateState(STATE_WIFI_CONNECTED);
        }

        // 2. 只有 WiFi 正常连通，才维护 MQTT 连接
        checkMQTT();

        // 3. 若 MQTT 连通，驱动其心跳循环
        if (_mqttClient.connected()) {
            _mqttClient.loop();
        }
    } else {
        // WiFi 未连接：若此前 MQTT 是连通的，主动断开并清理状态
        if (_state == STATE_MQTT_CONNECTED || _state == STATE_MQTT_CONNECTING || _state == STATE_WIFI_CONNECTED) {
            if (_mqttClient.connected()) {
                _mqttClient.disconnect();
            }
            updateState(STATE_DISCONNECTED);
        }
    }
}

bool ESP32WifiMqttManager::isConnected() {
    return isWifiConnected() && isMqttConnected();
}

bool ESP32WifiMqttManager::isWifiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool ESP32WifiMqttManager::isMqttConnected() {
    return _mqttClient.connected();
}

NetworkState ESP32WifiMqttManager::getState() const {
    return _state;
}

void ESP32WifiMqttManager::onStateChange(StateCallback cb) {
    _stateCb = cb;
}

void ESP32WifiMqttManager::setMqttCallback(MqttMessageCallback cb) {
    _mqttCb = cb;
}

bool ESP32WifiMqttManager::subscribe(const char* topic, uint8_t qos) {
    if (_mqttClient.connected()) {
        return _mqttClient.subscribe(topic, qos);
    }
    return false;
}

bool ESP32WifiMqttManager::publish(const char* topic, const char* payload) {
    if (_mqttClient.connected()) {
        return _mqttClient.publish(topic, payload);
    }
    return false;
}

bool ESP32WifiMqttManager::publish(const char* topic, const uint8_t* payload, unsigned int plength, bool retained) {
    if (_mqttClient.connected()) {
        return _mqttClient.publish(topic, payload, plength, retained);
    }
    return false;
}

bool ESP32WifiMqttManager::beginPublish(const char* topic, size_t totalLength, bool retained) {
    if (_mqttClient.connected()) {
        return _mqttClient.beginPublish(topic, totalLength, retained);
    }
    return false;
}

size_t ESP32WifiMqttManager::write(const uint8_t* buffer, size_t size) {
    if (_mqttClient.connected()) {
        return _mqttClient.write(buffer, size);
    }
    return 0;
}

bool ESP32WifiMqttManager::endPublish() {
    if (_mqttClient.connected()) {
        return _mqttClient.endPublish();
    }
    return false;
}

void ESP32WifiMqttManager::updateState(NetworkState newState) {
    if (_state != newState) {
        Serial.printf("[NetManager] State change: %d -> %d\n", _state, newState);
        _state = newState;
        if (_stateCb) {
            _stateCb(_state);
        }
    }
}

void ESP32WifiMqttManager::checkWiFi() {
    if (WiFi.status() != WL_CONNECTED) {
        uint32_t now = millis();
        if (now - _lastWifiCheck >= _config.wifiReconnectIntervalMs) {
            _lastWifiCheck = now;
            Serial.println("[NetManager WiFi] Reconnecting to WiFi...");
            WiFi.begin(_config.wifiSsid, _config.wifiPassword);
            updateState(STATE_WIFI_CONNECTING);
        }
    }
}

void ESP32WifiMqttManager::checkMQTT() {
    if (!_mqttClient.connected()) {
        uint32_t now = millis();
        if (now - _lastMqttCheck >= _config.mqttReconnectIntervalMs) {
            _lastMqttCheck = now;
            updateState(STATE_MQTT_CONNECTING);

            Serial.printf("[NetManager MQTT] Attempting connection to Broker: %s:%d ...\n", _config.mqttBroker, _config.mqttPort);
            
            // 解析 Broker 的 IP 地址
            _resolvedBrokerIp = resolveBrokerIp(_config.mqttBroker);
            if (_resolvedBrokerIp[0] != 0) {
                _mqttClient.setServer(_resolvedBrokerIp, _config.mqttPort);
            } else {
                _mqttClient.setServer(_config.mqttBroker, _config.mqttPort);
            }

            // 构造随机 ClientID
            String clientId = String(_config.clientIdPrefix) + "-" + String(random(0xffff), HEX);

            // 尝试连接
            bool success;
            if (_config.mqttUsername != nullptr && _config.mqttPassword != nullptr) {
                success = _mqttClient.connect(clientId.c_str(), _config.mqttUsername, _config.mqttPassword);
            } else {
                success = _mqttClient.connect(clientId.c_str());
            }

            if (success) {
                Serial.println("[NetManager MQTT] Connected successfully.");
                updateState(STATE_MQTT_CONNECTED);
            } else {
                Serial.printf("[NetManager MQTT] Connection failed, rc=%d\n", _mqttClient.state());
                updateState(STATE_WIFI_CONNECTED); // 回退到已连接WiFi状态
            }
        }
    }
}

IPAddress ESP32WifiMqttManager::resolveBrokerIp(const char* host) {
    IPAddress resolvedIP;
    
    // 1. 如果本身就是 IP 地址直接解析返回
    if (resolvedIP.fromString(host)) {
        return resolvedIP;
    }

    // 2. 尝试标准 DNS 解析
    if (WiFi.hostByName(host, resolvedIP)) {
        // 绕过 Clash 的 Fake-IP 范围 (198.18.x.x)
        if (resolvedIP[0] == 198 && resolvedIP[1] == 18) {
            Serial.printf("[NetManager DNS] Resolved Fake-IP %s, bypassing...\n", resolvedIP.toString().c_str());
        } else {
            return resolvedIP;
        }
    } else {
        Serial.printf("[NetManager DNS] Standard DNS failed for host: %s\n", host);
    }

    // 3. 备用 HTTP-DNS 解析 (仅在开启配置且发生上面解析异常时执行)
    if (_config.useHttpDns) {
        Serial.println("[NetManager DNS] Attempting AliDNS HTTP-DNS...");
        HTTPClient http;
        String url = "http://223.5.5.5/resolve?name=" + String(host) + "&type=A";
        http.begin(url);
        http.setTimeout(3000); // 3 秒超时
        
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            int index = payload.indexOf("\"data\":\"");
            if (index != -1) {
                int start = index + 8;
                int end = payload.indexOf("\"", start);
                if (end != -1) {
                    String ipStr = payload.substring(start, end);
                    if (resolvedIP.fromString(ipStr.c_str())) {
                        Serial.printf("[NetManager DNS] HTTP-DNS resolved %s to %s\n", host, resolvedIP.toString().c_str());
                        http.end();
                        return resolvedIP;
                    }
                }
            }
        } else {
            Serial.printf("[NetManager DNS] HTTP-DNS failed, code: %d\n", httpCode);
        }
        http.end();
    }

    return IPAddress(0, 0, 0, 0);
}

void ESP32WifiMqttManager::staticMqttCallback(char* topic, byte* payload, unsigned int length) {
    if (_instance && _instance->_mqttCb) {
        _instance->_mqttCb(topic, payload, length);
    }
}
