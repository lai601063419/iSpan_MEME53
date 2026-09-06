#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>      
#include <unistd.h>     
#include <termios.h>   
#include <errno.h>
#include <pthread.h>
#include <sys/select.h>
#include <time.h>
#include "MQTTAsync.h"

#include "Lai_DEV.h"
#include "Lai_MQTT.h"

int USB_fd, LED_fd;
char* door_status[] = {"UNLOCKED", "LOCKED", "PERM_LOCKED", "PERM_UNLOCKED"};
char now_status[20];
char now_RFID[10] = "00000000";
char access_RFID[10] = "00000000";
struct timespec ts;

// 接收到訊息時的回調函數
int msgarrvd(void *context, char *topicName, int topicLen, MQTTAsync_message *message) {
  //%.*s 中的. (Precision 精度修飾符)：對於字串來說，表示「最多只印出指定長度的字元」。
  // 動態傳入 %.*s 中的 * (指定長度，必須是 int) 
  // 動態傳入 %.*s 中的 s (資料指標，轉型為 char*)
  printf("[收到訊息] 主題: %s | 內容: %.*s\n", 
      topicName, message->payloadlen, (char*)message->payload);

  if(strcmp(topicName, "rack/access/cmd") == 0){
    char key[10] = {'\0'};
    char* key_start = ((char*)message->payload) + 11;
    char* key_end = strchr(key_start, '\"');
    snprintf(key, key_end - key_start + 1, "%s", key_start);
    if(strcmp(key, "LOCK") == 0){
      write(USB_fd, "2\n", strlen("2\n"));
    }
    else if(strcmp(key, "UNLOCK") == 0){
      write(USB_fd, "3\n", strlen("3\n"));
    }
    else if(strcmp(key, "AUTO") == 0){
      write(USB_fd, "1\n", strlen("1\n"));
    }
  }
  MQTTAsync_freeMessage(&message);
  MQTTAsync_free(topicName);
  return 1;
}

// 連線成功的回調函數
void onConnect(void* context, MQTTAsync_successData* response) {
  int rc;
  printf("[成功] 已成功連線至 MQTT Broker!\n");
  is_connected = 1;

  // 1. 發布在線狀態 (Online) 至狀態主題
  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
  pubmsg.payload = "{\"online\":true}";
  pubmsg.payloadlen = (int)strlen(pubmsg.payload);
  pubmsg.qos = 1;
  pubmsg.retained = 1; // Retained 讓新訂閱者也能看到在線狀態
  MQTTAsync_sendMessage(client, REPORT_TOPIC, &pubmsg, NULL);

  char payload[128];
  clock_gettime(CLOCK_REALTIME, &ts);
  snprintf(payload, sizeof(payload), "{\"timestamp\":%ld,\"lock_state\":\"%s\",\"last_access_by\":\"%s\"}", 
  ts.tv_sec, now_status, access_RFID);

  pubmsg.payload = payload;
  pubmsg.payloadlen = (int)strlen(payload);
  pubmsg.qos = QOS;
  pubmsg.retained = 0;
  rc = MQTTAsync_sendMessage(client, STATUS_TOPIC, &pubmsg, NULL);
  if (rc == MQTTASYNC_SUCCESS) {
      printf("[發送] %s\n", payload);
  } else {
      printf("[錯誤] 發送失敗, 錯誤碼: %d\n", rc);
  }

  char* topics[] = {
      TOPIC
  };
  int qos[] = {QOS}; // 各主題對應的 QoS
  int topic_count = 1;
  // 連線成功後進行訂閱
  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
  opts.onSuccess = onSubscribe;
  
  rc = MQTTAsync_subscribeMany(client, topic_count, topics, qos, &opts);
  if (rc != MQTTASYNC_SUCCESS) {
      printf("[錯誤] 訂閱失敗, 錯誤碼: %d\n", rc);
  }
}

void* init_MQTT_thread_func(void* arg) {
  int rc;
  init_MQTT();
  do{
    rc = MQTTAsync_connect(client, &conn_opts);
    if(rc != MQTTASYNC_SUCCESS && rc != -3) { //-3是 MQTTASYNC_CONNECT_IN_PROGRESS，表示連線正在進行中
      printf("[錯誤] 發起連線失敗, 錯誤碼: %d\n", rc);
    }
    sleep(5); // 等待 5 秒後重試
  }while (is_connected == 0);
  pthread_exit(NULL);
}

void* USB_read_thread_func(void* arg) {
  int rc;
  char temp_buf[64];
  char line_buf[256];
  int line_pos = 0;
  ssize_t num_bytes;
  fd_set readfds;
  struct timeval timeout;

  while (1) {
    FD_ZERO(&readfds);
    FD_SET(USB_fd, &readfds);

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int activity = select(USB_fd + 1, &readfds, NULL, NULL, &timeout);

    if (activity < 0) {
        perror("select 錯誤");
        break;
    } 
    
    if (FD_ISSET(USB_fd, &readfds)) {
      num_bytes = read(USB_fd, temp_buf, sizeof(temp_buf));
      if (num_bytes < 0) {
        perror("USB 讀取失敗");
        break;
      }
      else if (num_bytes > 0) {
        // 逐字元解析並拼裝
        for (int i = 0; i < num_bytes; i++) {
          char ch = temp_buf[i];

          // 遇到換行符，代表一筆完整資料結束
          if (ch == '\n' || ch == '\r') {
            if (line_pos > 0) {
              line_buf[line_pos] = '\0'; // 補上字串結束符
              
              // 完整印出一整行
              printf("[USB 接收]: %s\n", line_buf);
              fflush(stdout);

              strncpy(now_status, door_status[line_buf[0] - '0'], sizeof(now_status) - 1);
              now_status[sizeof(now_status) - 1] = '\0';
              line_pos = 0; // 重置緩衝區

              char payload[128];
              clock_gettime(CLOCK_REALTIME, &ts);
              snprintf(payload, sizeof(payload), "{\"timestamp\":%ld,\"lock_state\":\"%s\",\"last_access_by\":\"%s\"}", 
                ts.tv_sec, now_status, access_RFID);

              MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
              pubmsg.payload = payload;
              pubmsg.payloadlen = (int)strlen(payload);
              pubmsg.qos = QOS;
              pubmsg.retained = 0;
              rc = MQTTAsync_sendMessage(client, STATUS_TOPIC, &pubmsg, NULL);
              if (rc == MQTTASYNC_SUCCESS) {
                  printf("[發送] %s\n", payload);
              } else {
                  printf("[錯誤] 發送失敗, 錯誤碼: %d\n", rc);
              }
            }
          } else {
            // 存入行緩衝區並防範溢位
            if (line_pos < sizeof(line_buf) - 1) {
              line_buf[line_pos++] = ch;
            } else {
              line_pos = 0; // 滿了就強制重置
            }
          }
        }
      }
    }
  }
  
  pthread_exit(NULL);
}

void* LED_thread_func(void* arg) {
  int *granted = (int*)arg;
  int count = *granted ? 1 : 2; // granted 為 1 時閃爍一次，為 0 時閃爍兩次
  for(int i = 0; i < count; ++i){
    write(LED_fd, "0", strlen("0"));
    usleep(150000);
    write(LED_fd, "1", strlen("1"));
    usleep(150000);
  }
  pthread_exit(NULL);
}

int main() {
  USB_fd = open(USB_DEV, O_RDWR | O_NOCTTY);

  if (USB_fd < 0) {
    perror("開啟 USB 裝置失敗");
    return 1;
  }
  int spi_fd = open(SPI_DEV, O_RDWR);

  if (spi_fd < 0) {
    perror("開啟 SPI 裝置失敗");
    return 1;
  }
  int pwm_fd = open(PWM_DEV, O_RDWR);

  if (pwm_fd < 0) {
    perror("開啟 PWM 裝置失敗");
    return 1;
  }
  LED_fd = open(LED_DEV, O_RDWR);

  if (LED_fd < 0) {
    perror("開啟 LED 裝置失敗");
    return 1;
  }

  printf("成功開啟裝置: %s\n", USB_DEV);
  printf("成功開啟裝置: %s\n", SPI_DEV);
  printf("成功開啟裝置: %s\n", PWM_DEV);
  printf("成功開啟裝置: %s\n", LED_DEV);

  if (init_USB(USB_fd) != 0) {
    close(USB_fd);
    close(spi_fd);
    close(pwm_fd);
    close(LED_fd);
    return 1;
  }

  pthread_t init_MQTT_thread, USB_read_thread, LED_thread;
  pthread_create(&init_MQTT_thread, NULL, init_MQTT_thread_func, NULL);
  pthread_create(&USB_read_thread, NULL, USB_read_thread_func, NULL);

  // 分離執行緒：結束時系統自動回收記憶體，無需 pthread_join
  pthread_detach(init_MQTT_thread);
  pthread_detach(USB_read_thread);

  char read_buf[256], spi_buf[256];
  char music[5];

  write(USB_fd, "1\n", strlen("1\n"));
  write(LED_fd, "1", strlen("1"));
  while (1) {
    memset(read_buf, 0, sizeof(read_buf));
    memset(spi_buf, 0, sizeof(spi_buf));

    ssize_t num_bytes = read(spi_fd, spi_buf, sizeof(spi_buf) - 1);
    char payload[128];
    int now_granted = 0, rc;

    if (num_bytes < 0) {
      perror("讀取失敗");
      break;
    } 
    else if (num_bytes > 0) {
      spi_buf[num_bytes] = '\0';
      snprintf(now_RFID, sizeof(now_RFID), "%02X%02X%02X%02X", 
        spi_buf[0], spi_buf[1], spi_buf[2], spi_buf[3]);
      printf("[讀卡成功%zd bytes] UID: %s (Check: %02X)\n", 
        num_bytes, now_RFID, spi_buf[4]);
      fflush(stdout);

      if(spi_buf[0] == 0x2B){
        strncpy(music, SUCCESS_MUSIC, sizeof(music));
        write(USB_fd, "0\n", strlen("0\n"));
        now_granted = 1;
        snprintf(access_RFID, sizeof(access_RFID), "%02X%02X%02X%02X", 
        spi_buf[0], spi_buf[1], spi_buf[2], spi_buf[3]);
      }
      else{
        strncpy(music, FAIL_MUSIC, sizeof(music));
        now_granted = 0;
      }
      
      clock_gettime(CLOCK_REALTIME, &ts);
      snprintf(payload, sizeof(payload), "{\"timestamp\":%ld,\"card_uid\":\"%s\",\"granted\":%s}", 
        ts.tv_sec, now_RFID, now_granted ? "true" : "false");

      MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
      pubmsg.payload = payload;
      pubmsg.payloadlen = (int)strlen(payload);
      pubmsg.qos = QOS;
      pubmsg.retained = 0;
      rc = MQTTAsync_sendMessage(client, EVENT_TOPIC, &pubmsg, NULL);
      if (rc == MQTTASYNC_SUCCESS) {
          printf("[發送] %s\n", payload);
      } else {
          printf("[錯誤] 發送失敗, 錯誤碼: %d\n", rc);
      }

      pthread_create(&LED_thread, NULL, LED_thread_func, (void*)(&now_granted));
      write(pwm_fd, music, strlen(music));
      pthread_join(LED_thread, NULL); // 等待 LED 執行緒結束，確保 LED 閃爍完成
      usleep(1000000);
    }
    usleep(4000);
  }
  MQTTAsync_destroy(&client);
  close(USB_fd);
  close(spi_fd);
  close(pwm_fd);
  close(LED_fd);
  return 0;
}
