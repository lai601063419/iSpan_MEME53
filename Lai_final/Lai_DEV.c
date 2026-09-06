#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h> 

#include "Lai_DEV.h"

int init_USB(int USB_fd){
  struct termios tty;
  memset(&tty, 0, sizeof(tty));

  if (tcgetattr(USB_fd, &tty) != 0) {
    perror("無法取得 tty 屬性");
    return 1;
  }

  cfsetospeed(&tty, B115200);
  cfsetispeed(&tty, B115200);

  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag |= CREAD | CLOCAL;

  tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
  tty.c_iflag &= ~(IXON | IXOFF | IXANY);
  tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

  tty.c_oflag &= ~OPOST;

  tty.c_cc[VMIN]  = 1;
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(USB_fd, TCSANOW, &tty) != 0) {
    perror("無法套用 tty 設定");
    return 2;
  }
  return 0;
}
