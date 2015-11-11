#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int fp;
    fp = open("/dev/pl011_uart0", O_RDONLY);
    if(fp<0)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }
    uint32_t data=0;
    int ret=0;
    for(;ret!=-1;)
    {
        ret=read(fp, &data, 4);
        fprintf(stderr,"waiter: bytes: %d, val: %08x\n", ret, data);
        usleep(200000);
    }
    close(fp);
    return 0;
}
