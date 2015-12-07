#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <unistd.h>

#define DEVPATH "/dev/embbGpioA"
#define EMBB_GPIO_MAGIC 'E'         // 8-bit magic number
#define EMBB_GPIO_TEST _IO(EMBB_GPIO_MAGIC,3)

int main(int argc, char** argv)
{
    int fd;
    fd = open(DEVPATH, O_RDWR);
    if(fd<0)
    {
        perror("opening");
        exit(EXIT_FAILURE);
    }
    if( ioctl(fd, EMBB_GPIO_TEST) <0)
        perror("ioctl error");
    fprintf(stderr, "U: device opened\n");
    close(fd);
    return 0;
}
