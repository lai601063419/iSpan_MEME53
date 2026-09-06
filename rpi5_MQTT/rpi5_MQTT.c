#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "MQTTAsync.h"

#define ADDRESS     "tcp://192.168.69.190:1883"
#define CLIENTID    "RPi5_C_Client"
#define TOPIC       "rack/access/+"
#define STATUS_TOPIC       "rack/access/report-in"
#define QOS         1
#define TIMEOUT     10000L

MQTTAsync client;
volatile int is_connected = 0;

// 接收到訊息時的回調函數
int msgarrvd(void *context, char *topicName, int topicLen, MQTTAsync_message *message) {
    printf("[收到訊息] 主題: %s | 內容: %.*s\n", 
           topicName, message->payloadlen, (char*)message->payload);
    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topicName);
    return 1;
}

// 連線中斷時的回調函數
void connlost(void *context, char *cause) {
    printf("\n[警告] 連線中斷！原因: %s\n", cause ? cause : "未知");
}

// 訂閱成功的回調函數
void onSubscribe(void* context, MQTTAsync_successData* response) {
    printf("[成功] 已成功訂閱主題: %s\n", TOPIC);
}

// 連線成功的回調函數
void onConnect(void* context, MQTTAsync_successData* response) {
    printf("[成功] 已成功連線至 MQTT Broker!\n");
    is_connected = 1;

    // 1. 發布在線狀態 (Online) 至狀態主題
    MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
    pubmsg.payload = "{\"online\":true}";
    pubmsg.payloadlen = (int)strlen(pubmsg.payload);
    pubmsg.qos = 1;
    pubmsg.retained = 1; // Retained 讓新訂閱者也能看到在線狀態
    MQTTAsync_sendMessage(client, STATUS_TOPIC, &pubmsg, NULL);

    char* topics[] = {TOPIC};
    int qos[] = {QOS}; // 各主題對應的 QoS
    int topic_count = 1;
    // 連線成功後進行訂閱
    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    opts.onSuccess = onSubscribe;
    
    int rc = MQTTAsync_subscribeMany(client, topic_count, topics, qos, &opts);
    if (rc != MQTTASYNC_SUCCESS) {
        printf("[錯誤] 訂閱失敗, 錯誤碼: %d\n", rc);
    }
}

// 連線失敗的回調函數
void onConnectFailure(void* context, MQTTAsync_failureData* response) {
    printf("[錯誤] 連線失敗, 錯誤碼: %d\n", response ? response->code : -1);
}

int main(int argc, char* argv[]) {
    MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
    int rc;

    // 建立 MQTT 用戶端
    MQTTAsync_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTAsync_setCallbacks(client, NULL, connlost, msgarrvd, NULL);

    // 設定連線參數
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.onSuccess = onConnect;
    conn_opts.onFailure = onConnectFailure;
    conn_opts.context = client;

    MQTTAsync_willOptions will_opts = MQTTAsync_willOptions_initializer;

    // 1. 設定遺囑內容
    will_opts.topicName = STATUS_TOPIC;           // 遺囑發布的主題
    will_opts.message = "{\"online\":false}";     // 遺囑內容
    will_opts.qos = 1;                              // QoS 等級
    will_opts.retained = 1;                         // 通常設為 1 (Retained)，讓後續連線的人也能看到離線狀態

    // 2. 將遺囑設定掛載到連線選項中
    conn_opts.will = &will_opts;


    // 發起連線
    if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS) {
        printf("[錯誤] 發起連線失敗, 錯誤碼: %d\n", rc);
        return EXIT_FAILURE;
    }

    while(is_connected == 0) {
      printf("正在連線至 %s ...\n", ADDRESS);
      sleep(1);
    }

    // 主迴圈：每秒發送一次資料
    int count = 0;
    while (1) {
        // char payload[64];
        // snprintf(payload, sizeof(payload), "Hello from RPi5! 計數: %d", ++count);

        // MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
        // pubmsg.payload = payload;
        // pubmsg.payloadlen = (int)strlen(payload);
        // pubmsg.qos = QOS;
        // pubmsg.retained = 0;

        // MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
        
        // rc = MQTTAsync_sendMessage(client, TOPIC, &pubmsg, &opts);
        // if (rc == MQTTASYNC_SUCCESS) {
        //     printf("[發送] %s\n", payload);
        // } else {
        //     printf("[錯誤] 發送失敗, 錯誤碼: %d\n", rc);
        // }

        sleep(3); // 每 3 秒發送一次
    }

    // 中斷連線與釋放資源
    MQTTAsync_destroy(&client);
    return EXIT_SUCCESS;
}