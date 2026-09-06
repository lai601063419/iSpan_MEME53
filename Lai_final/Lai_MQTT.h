#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "MQTTAsync.h"

#define ADDRESS     "tcp://192.168.69.190:1883"
#define CLIENTID    "Access_Client"
#define TOPIC       "rack/access/+"
#define REPORT_TOPIC       "rack/access/report-in"
#define STATUS_TOPIC       "rack/access/state"
#define EVENT_TOPIC       "rack/access/event"
#define QOS         1
#define TIMEOUT     10000L

extern MQTTAsync client;
extern MQTTAsync_connectOptions conn_opts;
extern volatile int is_connected;

int msgarrvd(void *context, char *topicName, int topicLen, MQTTAsync_message *message);

// 連線中斷時的回調函數
void connlost(void *context, char *cause);

// 訂閱成功的回調函數
void onSubscribe(void* context, MQTTAsync_successData* response);

// 連線成功的回調函數
void onConnect(void* context, MQTTAsync_successData* response);

// 連線失敗的回調函數
void onConnectFailure(void* context, MQTTAsync_failureData* response);

void init_MQTT(void);