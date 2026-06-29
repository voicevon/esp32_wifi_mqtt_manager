#include "ESP32WifiMqttManager.h"
#include <HTTPClient.h>

// 初始化静态实例指针
ESP32WifiMqttManager* ESP32WifiMqttManager::_instance = nullptr;

// FreeRTOS 后台异步解析与连接任务
void asyncMqttConnectTask(void* pvParameters) {
    ESP32WifiMqttManager* mgr = (ESP32WifiMqttManager*)pvParameters;
    if (!mgr) {
        vTaskDelete(NULL);
        return;
    }
    
    mgr->_isConnecting = true;
    
    uint32_t now = millis();
    // 如果尚未解析成功过，或者解析缓存已超过 5 分钟，则进行 DNS 域名解析
    if (mgr->_resolvedBrokerIp[0] == 0 || (now - mgr->_lastDnsResolveMs > 300000)) {
        mgr->_lastDnsResolveMs = now;
        IPAddress tempIP = mgr->resolveBrokerIp(mgr->_config.mqttBroker);
        if (tempIP[0] != 0) {
            mgr->_resolvedBrokerIp = tempIP;
            Serial.printf("[NetManager MQTT Task] DNS resolved IP: %s\n", tempIP.toString().c_str());
        } else {
            Serial.println("[NetManager MQTT Task] DNS resolution failed, will fallback to domain.");
        }
    }

    // 根据解析出的 IP 分配服务器
    if (mgr->_resolvedBrokerIp[0] != 0) {
        mgr->_mqttClient.setServer(mgr->_resolvedBrokerIp, mgr->_config.mqttPort);
    } else {
        mgr->_mqttClient.setServer(mgr->_config.mqttBroker, mgr->_config.mqttPort);
    }

    // 构建 Client ID 
    String clientId = String(mgr->_config.clientIdPrefix) + "-" + String(random(0xffff), HEX);

    // 尝试连接 MQTT Broker (阻塞操作)
    Serial.println("[NetManager MQTT Task] Attempting connection to Broker...");
    bool success;
    if (mgr->_config.mqttUsername != nullptr && mgr->_config.mqttPassword != nullptr) {
        success = mgr->_mqttClient.connect(clientId.c_str(), mgr->_config.mqttUsername, mgr->_config.mqttPassword);
    } else {
        success = mgr->_mqttClient.connect(clientId.c_str());
    }

    if (success) {
        Serial.println("[NetManager MQTT Task] Connected successfully.");
    } else {
        Serial.printf("[NetManager MQTT Task] Connection failed, rc=%d\n", mgr->_mqttClient.state());
    }

    mgr->_isConnecting = false;
    vTaskDelete(NULL); // 连接任务完成，自我销毁
}

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

        // 2. 仅在非连接过程中，维护 MQTT 探测
        if (!_isConnecting) {
            checkMQTT();
        }

        // 3. 在主线程检测并同步更新状态与心跳
        if (_mqttClient.connected()) {
            if (_state != STATE_MQTT_CONNECTED) {
                updateState(STATE_MQTT_CONNECTED);
            }
            _mqttClient.loop();
        } else {
            // 如果连接已断开，且我们当前没有正在连接：
            if (!_isConnecting) {
                if (_state == STATE_MQTT_CONNECTED || _state == STATE_MQTT_CONNECTING) {
                    updateState(STATE_WIFI_CONNECTED);
                }
            }
        }
    } else {
        // WiFi 未连接：若此前 MQTT 是连通的，主动断开并清理状态
        if (_state == STATE_MQTT_CONNECTED || _state == STATE_MQTT_CONNECTING || _state == STATE_WIFI_CONNECTED) {
            if (!_isConnecting && _mqttClient.connected()) {
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
    if (_isConnecting) return false;
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
    if (_isConnecting) return false;
    if (_mqttClient.connected()) {
        return _mqttClient.subscribe(topic, qos);
    }
    return false;
}

bool ESP32WifiMqttManager::publish(const char* topic, const char* payload) {
    if (_isConnecting) return false;
    if (_mqttClient.connected()) {
        return _mqttClient.publish(topic, payload);
    }
    return false;
}

bool ESP32WifiMqttManager::publish(const char* topic, const uint8_t* payload, unsigned int plength, bool retained) {
    if (_isConnecting) return false;
    if (_mqttClient.connected()) {
        return _mqttClient.publish(topic, payload, plength, retained);
    }
    return false;
}

bool ESP32WifiMqttManager::beginPublish(const char* topic, size_t totalLength, bool retained) {
    if (_isConnecting) return false;
    if (_mqttClient.connected()) {
        return _mqttClient.beginPublish(topic, totalLength, retained);
    }
    return false;
}

size_t ESP32WifiMqttManager::write(const uint8_t* buffer, size_t size) {
    if (_isConnecting) return 0;
    if (_mqttClient.connected()) {
        return _mqttClient.write(buffer, size);
    }
    return 0;
}

bool ESP32WifiMqttManager::endPublish() {
    if (_isConnecting) return false;
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
            
            if (_isConnecting) {
                return; // 已经在连接中，不重复触发
            }
            
            updateState(STATE_MQTT_CONNECTING);
            Serial.println("[NetManager MQTT] Launching asynchronous MQTT connect task...");
            
            // 启动后台异步连接任务，并捕获结果
            BaseType_t ret = xTaskCreate(asyncMqttConnectTask, "mqtt_async_conn", 8192, this, 1, NULL);
            if (ret == pdPASS) {
                _isConnecting = true; // 任务创建成功，立即加锁
            } else {
                Serial.println("[NetManager MQTT] Error: Failed to create MQTT task!");
                updateState(STATE_WIFI_CONNECTED); // 创建失败，直接回退状态
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
