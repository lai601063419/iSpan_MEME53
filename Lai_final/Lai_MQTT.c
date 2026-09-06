#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "MQTTAsync.h"

#include "Lai_MQTT.h"

MQTTAsync client;
MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;

volatile int is_connected = 0;

// 連線中斷時的回調函數
void connlost(void *context, char *cause) {
    printf("\n[警告] 連線中斷！原因: %s\n", cause ? cause : "未知");
    is_connected = 0;
}

// 訂閱成功的回調函數
void onSubscribe(void* context, MQTTAsync_successData* response) {
    printf("[成功] 已成功訂閱主題: %s\n", TOPIC);
}



// 連線失敗的回調函數
void onConnectFailure(void* context, MQTTAsync_failureData* response) {
    printf("[錯誤] 連線失敗, 錯誤碼: %d\n", response ? response->code : -1);
    is_connected = 0;
}

void init_MQTT(void) {
    MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);

    // 建立 MQTT 用戶端
    MQTTAsync_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTAsync_setCallbacks(client, NULL, connlost, msgarrvd, NULL);

    // 設定連線參數
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.onSuccess = onConnect;
    conn_opts.onFailure = onConnectFailure;
    conn_opts.context = client; 

    // === 新增以下自動重連設定 ===
    conn_opts.automaticReconnect = 1; // 啟用自動重連
    conn_opts.minRetryInterval = 1;   // 首次重試間隔 (秒)
    conn_opts.maxRetryInterval = 5;  // 最長重試間隔 (秒)

    static MQTTAsync_willOptions will_opts = MQTTAsync_willOptions_initializer;

    // 1. 設定遺囑內容
    will_opts.topicName = REPORT_TOPIC;           // 遺囑發布的主題
    will_opts.message = "{\"online\":false}";     // 遺囑內容
    will_opts.qos = 1;                              // QoS 等級
    will_opts.retained = 1;                         // 通常設為 1 (Retained)，讓後續連線的人也能看到離線狀態

    // 2. 將遺囑設定掛載到連線選項中
    conn_opts.will = &will_opts;
}
