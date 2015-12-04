#include <linux/module.h>       //MODULE_AUTHOR etc.
#include <linux/slab.h>         //kzalloc
#include <linux/interrupt.h>    //irq_handler_t
#include <linux/fs.h>           //register_blkdev
#include <linux/genhd.h>        //alloc_desc
#include <linux/spinlock.h>     //spinlock
#include <linux/spinlock_types.h>     //spinlock
#include <linux/blkdev.h>       //request queue
#include <linux/vmalloc.h>      //vmalloc
#include <linux/ioctl.h>        //ioctl command
#include <linux/blkdev.h>       //blk_queue_hardsect_size
#define MINOR_FIRST 0           //first requested minor
#define MINOR_NB 1              //nb of minors requested

//device registers
#define PL011_OFFSET( add, offset ) ({ (const void*)((add)+(offset)); })
#define PL011_DR(base) (base)
#define PL011_LCR(base) PL011_OFFSET( (base), 0x2c)

#define KERNEL_SECTOR_SIZE	512
#define EMBB_GPIO_ADD 0x40000000
#define EMBB_GPIO_DEV_NAME "embbGpioDev"
#define EMBB_GPIO_DISK_NAME "embbGpioDisk"
//ioctl
#define EMBB_GPIO_MAGIC 'E'
#define EMBB_GPIO_GET_FIFO_TH _IOR(EMBB_GPIO_MAGIC, 1, int)
#define EMBB_GPIO_SET_FIFO_TH _IOW(EMBB_GPIO_MAGIC, 2, int)
#define EMBB_GPIO_MAXNR 2

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
    struct spinlock_t rqLock;
};

static const int irq_nb = 0x14;         //first column in 'cat /proc/interrupts' output
static EmbbGpioDev *devPtr = NULL;

static void embbGpioReqHandler(struct request_queue *rq)
{   /* Handler for queued requests */
}

static int embbGpioOpen(struct inode *inode, struct file *filp)
{
    printk(KERN_WARNING "device opened\n");
    return 0;
}

static int embbGpioRelease(struct inode *inode, struct file *filp)
{
    printk(KERN_WARNING "device released\n");
    return 0;
}

/* A higher-level block subsystem intercepts also ioctl requests.
 * @arg is optional as unsigned long,
 * ioctl commands numbers should be unique across the system,
 */
static int embbGpioIoctl(struct inode *inode, struct file *filp,
        unsigned int cmd, unsigned long arg)
{
    int ret=0;
    //security checks
    if( (_IOC_TYPE(cmd) != EMBB_GPIO_MAGIC)||(_IOC_NR(cmd) > EMBB_GPIO_MAXNR) )
        return -ENOTTY;
    //commands are user-oriented whereas access_ok is kernel-oriented
    //check user-space pointers
    if(_IOC_DIR(cmd) & _IOC_READ)
        ret = !access_ok(verify_write, (void __user*)arg, _IOC_SIZE(cmd) );
    if(_IOC_DIR(cmd) & _IOC_WRITE)
        ret = !access_ok(verify_read, (void __user*)arg, _IOC_SIZE(cmd) );
    if(ret)
    {
        ret = -EFAULT;
        goto out;
    }
    //parse control command
    switch(cmd)
    {
        case EMBB_GPIO_GET_FIFO_TH:
            ret = 888;
            if( copy_to_user(arg, &ret, sizeof(ret)) )
            {
                ret = -EFAULT;
                goto out;
            }
            ret = 0;
            break;
        case EMBB_GPIO_SET_FIFO_TH:
            if( copy_from_user(&ret, arg, sizeof(ret)) )
            {
                ret = -EFAULT;
                goto out;
            }
            printk(KERN_WARNING "fifo th set to: %d\n", ret);
            ret = 0;
            break;
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
    /* Allocate the wrapper structure */
    devPtr = (EmbbGpioDev *) kzalloc( sizeof( struct EmbbGpioDev), GFP_KERNEL );
    if(!devPtr)
        goto fail_kzalloc;
    /* Register the device and choose dynamically major number (0) */
    err = register_blkdev(0, EMBB_GPIO_DEV_NAME);
    if(err<0)
        goto fail_register;
    devPtr->major = err;
    /* Parameters setup */
    devPtr->minorsNb=1;
    devPtr->hardSect = 512;
    devPtr->nSectors = 64;          // one packet is 4096 -> 64*512 is 8 packets
    devPtr->size = devPtr->nSectors*devPtr->hardSect;       //for the kernel a disk is just a linear 512-bytes array
    devPtr->data = vmalloc(devPtr->size);
    if( !devPtr->data )
        goto fail_vmalloc;
    /* Prepare a request queue for a block device, args: handler + spin lock */
    spin_lock_init(&devPtr->rqLock);
    devPtr->rq = blk_init_queue( embbGpioReqHandler, devPtr->rqLock);
    if ( !devPtr->rq )
        goto fail_init_queue;
    /* Assumption sector size as in kernel */
    devPtr->rq->queuedata = devPtr;             //TODO: what is this?
    /* Gendisk structure allocation and setup */
    devPtr->gd = alloc_disk(devPtr->minorsNb);
    if( !devPtr->gd )
        goto fail_alloc_disk;
    devPtr->gd->major = devPtr->major;
    devPtr->gd->first_minor = 1;
    devPtr->gd->fops = &embbGpioOps;
    devPtr->gd->queue = devPtr->rq;
    snprintf(devPtr->gd->disk_name, 32, EMBB_GPIO_DISK_NAME);
    set_capacity(devPtr->gd, devPtr->nSectors*(devPtr->hardSect/KERNEL_SECTOR_SIZE));
    devPtr->gd->private_data = devPtr;
    /* Only when everything is set up */
    add_disk(devPtr->gd);
    printk(KERN_WARNING "device initialised successfully");
    return 0;
    /* Failures */
    //de-alloc disk?
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
