#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#define SPI_DEVICE "/dev/spidev0.0"

/* RC522 暫存器位址 */
#define CommandReg    0x01
#define CommIEnReg    0x02
#define CommIrqReg    0x04
#define ErrorReg      0x06
#define FIFODataReg   0x09
#define FIFOLevelReg  0x0A
#define ControlReg    0x0C
#define BitFramingReg 0x0D
#define ModeReg       0x11
#define TxControlReg  0x14
#define TxASKReg      0x15
#define RFCfgReg      0x26
#define TModeReg      0x2A
#define TPrescalerReg 0x2B
#define TReloadRegL   0x2C
#define TReloadRegH   0x2D
#define VersionReg    0x37

/* RC522 指令 */
#define PCD_IDLE       0x00
#define PCD_TRANSCEIVE 0x0C
#define PCD_RESETRST   0x0F

/* PICC (卡片) 指令 */
#define PICC_REQIDL    0x26
#define PICC_ANTICOLL  0x93

static uint32_t spi_speed = 1000000; // 1 MHz

static void write_reg(int fd, uint8_t reg, uint8_t value) {
    uint8_t tx[] = { (uint8_t)((reg << 1) & 0x7E), value };
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .len = 2,
        .speed_hz = spi_speed,
    };
    ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

static uint8_t read_reg(int fd, uint8_t reg) {
    uint8_t tx[] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t rx[2] = {0};
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = 2,
        .speed_hz = spi_speed,
    };
    ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    return rx[1];
}

static void antenna_on(int fd) {
    uint8_t temp = read_reg(fd, TxControlReg);
    if (!(temp & 0x03)) {
        write_reg(fd, TxControlReg, temp | 0x03);
    }
}

static void rc522_init(int fd) {
    write_reg(fd, CommandReg, PCD_RESETRST);
    usleep(50000);

    /* 設定 Timer */
    write_reg(fd, TModeReg, 0x8D);
    write_reg(fd, TPrescalerReg, 0x3E);
    write_reg(fd, TReloadRegL, 30);
    write_reg(fd, TReloadRegH, 0);
    
    write_reg(fd, TxASKReg, 0x40);      // 強制 100% ASK 調變
    write_reg(fd, ModeReg, 0x3D);
    
    /* 提升天線接收增益 (RxGain) 至 48dB (最大值)，大幅增加感應距離 */
    write_reg(fd, RFCfgReg, 0x70); 

    antenna_on(fd);

    ver = rc522_read_reg(spi, VersionReg);
    pr_info("rc522: Version Register = 0x%02X\n", ver);
}

static int to_card(int fd, uint8_t cmd, uint8_t *send_data, int send_len, uint8_t *back_data, uint32_t *back_bits) {
    uint8_t irq_en = 0x77;
    uint8_t wait_irq = 0x30;
    uint8_t n, last_bits;
    int i;

    write_reg(fd, CommIEnReg, irq_en | 0x80);
    write_reg(fd, CommIrqReg, 0x7F); // 清除 IRQ 標誌
    write_reg(fd, CommandReg, PCD_IDLE);
    write_reg(fd, FIFOLevelReg, 0x80); // 清空 FIFO

    for (i = 0; i < send_len; i++) {
        write_reg(fd, FIFODataReg, send_data[i]);
    }

    write_reg(fd, CommandReg, cmd);
    if (cmd == PCD_TRANSCEIVE) {
        write_reg(fd, BitFramingReg, read_reg(fd, BitFramingReg) | 0x80); // 開始傳輸
    }

    /* 改用毫秒級延遲輪詢，避免 CPU 輪詢太快錯過晶片響應 */
    i = 2000;
    do {
        usleep(100);
        n = read_reg(fd, CommIrqReg);
        i--;
    } while ((i != 0) && !(n & 0x01) && !(n & wait_irq));

    write_reg(fd, BitFramingReg, read_reg(fd, BitFramingReg) & (~0x80));

    if (i == 0 || (read_reg(fd, ErrorReg) & 0x1B)) {
        return -1;
    }

    if (back_data && back_bits) {
        n = read_reg(fd, FIFOLevelReg);
        last_bits = read_reg(fd, ControlReg) & 0x07;
        if (last_bits) {
            *back_bits = (n - 1) * 8 + last_bits;
        } else {
            *back_bits = n * 8;
        }

        if (n == 0) n = 1;
        if (n > 16) n = 16;

        for (i = 0; i < n; i++) {
            back_data[i] = read_reg(fd, FIFODataReg);
        }
    }
    return 0;
}

static int request_card(int fd, uint8_t req_mode, uint8_t *tag_type) {
    uint32_t back_bits;
    write_reg(fd, BitFramingReg, 0x07); // 最後一個 Byte 只送 7 bits
    tag_type[0] = req_mode;
    
    if (to_card(fd, PCD_TRANSCEIVE, tag_type, 1, tag_type, &back_bits) == 0) {
        if (back_bits == 0x10) return 0; // 回傳 16 bits 代表成功尋卡
    }
    return -1;
}

static int get_card_uid(int fd, uint8_t *uid) {
    uint32_t back_bits;
    uint8_t send_buf[2] = { PICC_ANTICOLL, 0x20 };

    write_reg(fd, BitFramingReg, 0x00);
    if (to_card(fd, PCD_TRANSCEIVE, send_buf, 2, uid, &back_bits) == 0) {
        // 驗證 BCC (CheckByte)
        if (uid[0] ^ uid[1] ^ uid[2] ^ uid[3] == uid[4]) {
            return 0;
        }
    }
    return -1;
}

int main() {
    int fd = open(SPI_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("無法開啟 SPI 裝置");
        return 1;
    }

    uint8_t mode = SPI_MODE_0;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed);

    rc522_init(fd);
    
    printf("RC522 晶片版本: 0x%02X\n", read_reg(fd, 0x37));
    printf("RC522 就緒，請將 13.56MHz IC 卡貼近天線...\n");

    uint8_t tag_type[2];
    uint8_t uid[5];

    while (1) {
        // 尋卡
        if (request_card(fd, PICC_REQIDL, tag_type) == 0) {
            // 防碰撞取得 UID
            if (get_card_uid(fd, uid) == 0) {
                printf("[讀卡成功] UID: %02X %02X %02X %02X (Check: %02X)\n", uid[0], uid[1], uid[2], uid[3], uid[4]);
                usleep(1000000); // 讀取成功後暫停 1 秒
            }
        }
        usleep(50000); // 50ms 輪詢一次
    }

    close(fd);
    return 0;
}