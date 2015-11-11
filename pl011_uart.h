#ifndef PL011_UART_H
#define PL011_UART_H

#include <linux/ioctl.h>    //not sure if that is a correct file

#define PL011_CMD_MAGIC 0xed
//second value is an ordinal number
//third value is a type
#define PL011_CMD1 _IOR(PL011_CMD_MAGIC, 1, int) 
#endif //PL011_UART_H

/*
 * instead of copy_from/to_user:
 *
 * access_ok() - return -EFAULT if wrong, _IOC_READ is from user's perspective
 * but VERIFY_WRITE from driver's perspective
 *
 * put_user(), get_user(), __put_user(), __get_user()- for 1,2,4,8 bit values
 */
