#include <linux/module.h>       //MODULE_AUTHOR etc.
#include <linux/slab.h>         //kzalloc
#include <linux/interrupt.h>    //irq_handler_t
#include <linux/fs.h>           //register_blkdev
#include <linux/genhd.h>        //alloc_desc
#include <linux/spinlock_types.h>     //spinlock
#include <linux/spinlock.h>     //spinlock
#include <linux/blkdev.h>       //request queue blk_queue_hardsect_size
#include <linux/vmalloc.h>      //vmalloc
#include <linux/ioctl.h>        //ioctl command
//#include <asm-generic/uaccess.h>     //VERIFY_READ and VERIFY_WRITE
#define MINOR_FIRST 0           //first requested minor
#define MINOR_NB 1              //nb of minors requested

//device registers
#define PL011_OFFSET( add, offset ) ({ (const void*)((add)+(offset)); })
#define PL011_DR(base) (base)
#define PL011_LCR(base) PL011_OFFSET( (base), 0x2c)

#define KERNEL_SECTOR_SIZE	512
#define EMBB_GPIO_ADD 0x40000000
#define EMBB_GPIO_DEV_NAME "embbGpioDev"    //visible in /proc/devices
#define EMBB_GPIO_DISK_NAME "embbGpio"  //visible in /dev
#define EMBB_GPIO_NSECTORS 64
/* Commands are associated with numbers, which should be unique across the
 * system.
 * _IOC_TYPEBITS: magic number,
 * _IOC_NRBITS: the sequential number,
 * _direction: _IOC_NONE (no data transfer), _IOC_READ, _IOC_WRITE or both,
 *   seen from the user's point of view (_IOC_READ writes to user space)
 * _IOC_SIZEBITS: size of involved user data, may be ignored,
 * 
 * access_ok, is kernel-oriented, returns true for success! On error -EFAULT
 * should be returned to the caller,
 *
 * Helper macros to setup command numbers:
 * _IO(type, nr) - command without arguments,
 * _IOR(type, nr, datatype) - reading the data from the driver,
 * _IOW(type, nr, datatype) - writing the data to the driver, 
 * _IOWR(type, nr, datatype) - bidirectional transfer,
 *
 * By convention values should be exchanged by pointer, negative values is used
 * to indicate an error (sets up errno variable)
 */
#define EMBB_GPIO_MAGIC 'E'         // 8-bit magic number
#define EMBB_GPIO_GET_FIFO_TH _IOR(EMBB_GPIO_MAGIC, 1, int)
#define EMBB_GPIO_SET_FIFO_TH _IOW(EMBB_GPIO_MAGIC, 2, int)
#define EMBB_GPIO_TEST _IO(EMBB_GPIO_MAGIC,3)
#define EMBB_GPIO_MAXNR 3

typedef struct EmbbGpioDev EmbbGpioDev;

struct EmbbGpioDev
{
    int major;
    size_t minorsNb;
    u16 hardSect;
    size_t nSectors;
    size_t size;
    struct gendisk *gd;
    struct request_queue *rq;
    u8 *data;
    spinlock_t rqLock;
};

static const int irq_nb = 0x14;         //first column in 'cat /proc/interrupts' output
static EmbbGpioDev *devPtr = NULL;

static void embbGpioReqHandler(struct request_queue *rq)
{   /* Handler for queued requests */
}

static int embbGpioOpen(struct inode *inode, struct file *filp)
{
    printk(KERN_WARNING "DEVICE OPENED\n");
    return 0;
}

static int embbGpioRelease(struct inode *inode, struct file *filp)
{
    printk(KERN_WARNING "DEVICE RELEASED\n");
    return 0;
}

/* A higher-level block subsystem intercepts also ioctl requests.
 * @dataPtr is optional, points to user space
 */
static int embbGpioIoctl(struct inode *inode, struct file *filp,
        unsigned int cmd, unsigned long dataPtr)
{
    int ret=0;
    //simple security checks
    if( (_IOC_TYPE(cmd) != EMBB_GPIO_MAGIC)||(_IOC_NR(cmd) > EMBB_GPIO_MAXNR) )
        ret = -ENOTTY;
    if(_IOC_DIR(cmd) & _IOC_READ)
        ret = !access_ok(VERIFY_WRITE, (void __user*)dataPtr, _IOC_SIZE(cmd) );
    if(_IOC_DIR(cmd) & _IOC_WRITE)
        ret = !access_ok(VERIFY_READ, (void __user*)dataPtr, _IOC_SIZE(cmd) );
    if(ret)
    {
        ret = -EFAULT;
        goto out;
    }
    //parse control command
    switch(cmd)
    {
        case EMBB_GPIO_TEST:
            printk(KERN_WARNING "IOCTL TEST\n");
            break;
        //case EMBB_GPIO_GET_FIFO_TH:
        //    ret = 888;
        //    if( copy_to_user(arg, &ret, sizeof(ret)) )
        //    {
        //        ret = -EFAULT;
        //        goto out;
        //    }
        //    ret = 0;
        //    break;
        //case EMBB_GPIO_SET_FIFO_TH:
        //    if( copy_from_user(&ret, arg, sizeof(ret)) )
        //    {
        //        ret = -EFAULT;
        //        goto out;
        //    }
        //    printk(KERN_WARNING "fifo th set to: %d\n", ret);
        //    ret = 0;
        //    break;
        default:
            ret = -ENOTTY;
            break;
    }
out:
    return ret;
}

static struct block_device_operations embbGpioOps = {
    .owner   = THIS_MODULE,
    .open    = embbGpioOpen,
    .release = embbGpioRelease,
	.ioctl   = embbGpioIoctl,
};

static int __init embbGpioInit(void)
{
    printk(KERN_WARNING "%s\n", __TIME__);
    int err=0, err_flag=0;
    /* Allocate the wrapper structure, pointer has to be global */
    devPtr = (EmbbGpioDev *) kzalloc( sizeof( struct EmbbGpioDev), GFP_KERNEL );
    if(!devPtr)
        goto fail_kzalloc;
    /* Parameters setup */
    devPtr->minorsNb=1;
    devPtr->hardSect = 512;
    devPtr->nSectors = EMBB_GPIO_NSECTORS;          // one packet is 4096 -> 64*512 is 8 packets
    devPtr->size = devPtr->nSectors*devPtr->hardSect;       //for the kernel a disk is just a linear 512-bytes array
    /* Register the device and choose dynamically major number (0) */
    err = register_blkdev(0, EMBB_GPIO_DEV_NAME);
    if(err<0)
        goto fail_register;
    devPtr->major = err;
    devPtr->data = vmalloc(devPtr->size);
    if( !devPtr->data )
        goto fail_vmalloc;
    /* Prepare a request queue for a block device, args: handler + spin lock */
    spin_lock_init(&devPtr->rqLock);
    devPtr->rq = blk_init_queue( embbGpioReqHandler, &devPtr->rqLock);
    if ( !devPtr->rq )
        goto fail_init_queue;
    devPtr->rq->queuedata = devPtr;             //TODO: this is perhaps passed as an opaque
    /* Gendisk structure allocation and setup */
    devPtr->gd = alloc_disk(devPtr->minorsNb);
    if( !devPtr->gd )
        goto fail_alloc_disk;
    devPtr->gd->major = devPtr->major;
    devPtr->gd->first_minor = 1;
    devPtr->gd->fops = &embbGpioOps;
    devPtr->gd->queue = devPtr->rq;
    /* this should be done for each partition separately */
    snprintf(devPtr->gd->disk_name, 32, "%s%c",EMBB_GPIO_DISK_NAME, 'A');
    set_capacity(devPtr->gd, devPtr->nSectors*(devPtr->hardSect/KERNEL_SECTOR_SIZE));
    devPtr->gd->private_data = devPtr;
    /* Only when everything is set up */
    add_disk(devPtr->gd);
    printk(KERN_WARNING "INIT SUCCESS\n");
    return 0;
    /* Failures */
fail_alloc_disk:    
    if(!err_flag++)
        printk( KERN_WARNING "alloc disk failed\n");
    blk_cleanup_queue(devPtr->rq);
fail_init_queue:
    if(!err_flag);
        printk( KERN_WARNING "queue init failed\n");
fail_vmalloc:
    if(!err_flag++)
        printk( KERN_WARNING "vmalloc failed\n");
    unregister_blkdev(devPtr->major, EMBB_GPIO_DEV_NAME);
fail_register:    
    if(!err_flag++)
        printk( KERN_WARNING "register_blkdev error\n");
    kfree(devPtr);
    devPtr=NULL;
fail_kzalloc:
    if(!err_flag)
        printk( KERN_WARNING "kzalloc error\n");
    return err;
}

static void __exit embbGpioExit(void)
{
    del_gendisk(devPtr->gd);
    vfree(devPtr->data);
    blk_cleanup_queue(devPtr->rq);
    unregister_blkdev(devPtr->major, EMBB_GPIO_DEV_NAME);
    kfree(devPtr);
    devPtr=NULL;
    return;
}

module_init(embbGpioInit);
module_exit(embbGpioExit);

MODULE_AUTHOR("Maciej Bielski");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A block device driver for ");
