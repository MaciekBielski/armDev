#include <linux/init.h>
#include <linux/module.h>
#include <linux/utsname.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/delay.h>            //msleep_interruptible
#include <linux/string.h>           //memset
#include <asm/page.h>               //PAGE_SIZE
#include <asm/uaccess.h>            //copy_to/from_user
#include <linux/ioport.h>           //request_mem_region
#include <asm/io.h>                 //ioremap(), ioread/write
#include <linux/interrupt.h>        //request_irq()
#include <linux/semaphore.h>        //sem
#include <linux/wait.h>             //wait functions
#include <linux/sched.h>            //schedule()
#include <linux/spinlock.h>         //irq_pending spinlock
#include <asm-generic/current.h>    //current()

#define MINOR_FIRST 0           //first requested minor
#define MINOR_NB 1              //nb of minors requested
#define DEV_NAME "pl011_uart"
#define RBUFF_SZ 8
#define WBUFF_SZ 8

//device registers
#define PL011_PHYS_ADD 0xe0000000
#define PL011_MEM_SZ 0x1000
#define PL011_OFFSET( add, offset ) ({ (const void*)((add)+(offset)); })
#define PL011_DR(base) (base)
#define PL011_IMSC(base) PL011_OFFSET( (base), 0x38)
#define PL011_ICR(base) PL011_OFFSET( (base), 0x44)

typedef struct pl011_dev
{
    unsigned char *r_buff, *r_put, *r_get;
    unsigned char *w_buff;
    unsigned char *iomem;
    unsigned long io_start;
    unsigned long io_size;
    struct resource* io_mem_region;
    struct cdev chrdev;
    struct semaphore sem;
    int irq_pending;
    spinlock_t flag_lock;
    wait_queue_head_t rqh;      //read queue head
} pl011_dev;

static struct class *pl011_class = NULL;
static struct pl011_dev *pl011_device = NULL;
static unsigned int pl011_major=0;
//access address - is it possible to not make it global?
static const int irq_nb = 0x3b;
//size_t uart_id = 0xC0CADEAD;         //used for shared IRQ lines

static irq_handler_t data_handler(int nb, void *dev_id, struct pt_regs *regs)
{
    pl011_dev *uart = (pl011_dev *) dev_id;
    //ACK the IRQ but it does not allow to transmit next char automatically
    iowrite8(0x10, PL011_ICR(uart->iomem));
    //spin_lock(&uart->flag_lock);
    uart->irq_pending=1;
    //spin_unlock(&uart->flag_lock);
    wake_up_interruptible_all(&uart->rqh);
    //printk(KERN_WARNING "[!] IRQ");
    return IRQ_HANDLED;
}

static int pl011_open(struct inode *inode, struct file *filep)
{
    /* called on first access when filep->f_count==0 */
    pl011_dev *uart = container_of( inode->i_cdev, struct pl011_dev, chrdev);
    filep->private_data = uart;
    filep->f_pos=0;
    //setting IRQ
    int err=0;
    err = request_irq(irq_nb, (irq_handler_t) data_handler, 0,
            "pl011_uart", uart);
    if( IS_ERR(err) )
    {
        printk(KERN_WARNING "request_irq() failed\n");
        goto out;
    }
    init_waitqueue_head(&uart->rqh);
    sema_init(&uart->sem, 1);           //one down() possible
    spin_lock_init(&uart->flag_lock);
    uart->irq_pending=0;
    enable_irq(irq_nb);
    iowrite8(0x70, PL011_IMSC(uart->iomem));
    printk(KERN_WARNING "open(), pos: %llu\n", filep->f_pos);
out:
    return err;
}

static int pl011_release(struct inode *inode, struct file *filep)
{
    pl011_dev* uart = (pl011_dev*) filep->private_data;
    iowrite8(0x00, PL011_IMSC(uart->iomem));
    disable_irq(irq_nb);
    free_irq(irq_nb, uart);
    printk(KERN_WARNING "release()\n");
    return 0;
}

static ssize_t pl011_read(struct file *filep, char __user *data, size_t sz,
        loff_t *fpos)
{
    /* reads a character from the device one by one, characters are written to
     * the round buffer but it does not improve the efficiency yet */
    /* normally fpos has to be updated to have a correct filep->f_pos
     * at next call but this is not used now yet */
    pl011_dev* uart = (pl011_dev*)filep->private_data;
    int err=0;
    /* sleeps if no pending IRQ, woken up by data_handler */
    down_interruptible(&uart->sem);
    //spin_lock(&uart->flag_lock);
    while(!uart->irq_pending)
    {
        //spin_unlock(&uart->flag_lock);
        /* process is about to block */
        DEFINE_WAIT(rqe);
        up(&uart->sem);
        prepare_to_wait(&uart->rqh, &rqe, TASK_INTERRUPTIBLE);
        //spin_lock(&uart->flag_lock);
        if(!uart->irq_pending)
        {
            //spin_unlock(&uart->flag_lock);
            printk(KERN_WARNING "-sleep-\n");
            schedule();
        }
        else
            //spin_unlock(&uart->flag_lock);
        finish_wait(&uart->rqh, &rqe);
        printk(KERN_WARNING "-wake-\n");
        /* necessary for Ctrl-C to work properly */
        if(signal_pending(current))
            return -ERESTARTSYS;
        down_interruptible(&uart->sem);
        //msleep_interruptible(500);
        //spin_lock(&uart->flag_lock);
    }
    /* no read is sleeping now */
    uart->irq_pending=0;
    //spin_unlock(&uart->flag_lock);
    /* only now allow the next IRQ to be triggerred*/
    *uart->r_put=ioread8(PL011_DR(uart->iomem));
    //printk(KERN_WARNING "= %02x\n", *uart->r_put);
    uart->r_put++;
    if(uart->r_put - uart->r_buff >= RBUFF_SZ)
        uart->r_put = uart->r_buff;
    //read the whole data from the beginning
    if( copy_to_user(data, uart->r_get, 1) )
    {
        err = -EFAULT;
        goto out;
    }
    uart->r_get += 1;
    if(uart->r_get - uart->r_buff >= RBUFF_SZ)
        uart->r_get = uart->r_buff;
    //success
    err = 1;
    *fpos += 1;
out:
    up(&uart->sem);
    return err;
}

static ssize_t pl011_write(struct file *filep, const char __user *udata,
        size_t sz, loff_t *fpos)
{
    /* writes characters to the device */
    int err=0;
    pl011_dev* uart = (pl011_dev*)filep->private_data;
    //control how much you write before buffer rewind
    if( sz > WBUFF_SZ - filep->f_pos )
        sz = WBUFF_SZ - filep->f_pos; 
    printk(KERN_WARNING "sz: %d\n", sz);
    //write the data from the last write end
    unsigned char *start = uart->w_buff + filep->f_pos;
    if( copy_from_user(start, udata, sz) )
    {
        err = -EFAULT;
        goto out;
    }
    //update file position after write
    *fpos = (*fpos+sz)%RBUFF_SZ;
    err = sz;
    size_t i=0;
    for(; i<sz; i++)
        iowrite8(*(start+i), PL011_DR(uart->iomem) );
out:
    return err;
}

/*
static int pl011_ioctl(struct inode *inode, struct file *filep,
        unsigned int cmd, unsigned long arg)
{
    //cmd numbers specified in a header file
    switch(cmd)
    {
    case PL011_CMD1:
        //
    break
    default:
            return -ENOTTY;
    }
}
*/

static struct file_operations pl011_fops ={
    .owner = THIS_MODULE,
    .open = pl011_open,
    .release = pl011_release,
    .read = pl011_read,
    .write = pl011_write,
};

static int pl011_construct_device(struct pl011_dev *uart, struct class *klass)
{
    int err=0, err_flag=0;
    dev_t devt = MKDEV(pl011_major, MINOR_FIRST);
    struct device *device = NULL;
    // init cdev object, memory has been already allocated,
    // assign this cdev with dev_t object and inform kernel about it
    cdev_init(&uart->chrdev, &pl011_fops);
    err = cdev_add(&uart->chrdev, devt, 1);
    if(err)
        goto fail_step1;
    //creating entries in: '/dev' and '/sys/dev/char'
    device = device_create( klass, NULL, devt, NULL /*opaque*/,
        DEV_NAME "%d", MINOR_FIRST);
    if( IS_ERR(device) )
        goto fail_step2;
    // device internal logic setup
    if(!uart->r_buff)
        uart->r_buff = kzalloc( RBUFF_SZ, GFP_KERNEL);
    if(!uart->w_buff)
        uart->w_buff = kzalloc( WBUFF_SZ, GFP_KERNEL);
    if(!uart->r_buff || !uart->w_buff)
        goto fail_step3;
    uart->r_put = uart->r_buff;
    uart->r_get = uart->r_buff;
    uart->io_start = PL011_PHYS_ADD;
    uart->io_size = PL011_MEM_SZ;
    uart->io_mem_region = request_mem_region(uart->io_start, uart->io_size,
            "pl011_regs");
    if( !uart->io_mem_region )
        goto fail_step4;
    uart->iomem = ioremap(uart->io_start, uart->io_size);
    //success
    return 0;
    //fail
fail_step4:
    if(!err_flag++)
    {
        printk(KERN_WARNING "request_mem_region() failed\n");
        err = -ENODEV;
    }
    kfree(uart->r_buff);
    kfree(uart->w_buff);
fail_step3:
    if(!err_flag++)
    {
        printk(KERN_WARNING "kzalloc() failed\n");
        err = -ENOMEM;
    }
    device_destroy(klass, MKDEV(pl011_major, MINOR_FIRST));
fail_step2:
    if(!err_flag++)
    {
        printk(KERN_WARNING "device_create() failed\n");
        err = PTR_ERR(device);
    }
    cdev_del(&uart->chrdev);
fail_step1:
    if(!err_flag++)
        printk(KERN_WARNING "cdev_add() failed\n");
    return err;
}

static int pl011_destroy_device( struct pl011_dev *uart, struct class *klass)
{
    /* device internal logic cleanup */
    iounmap(uart->iomem);
    release_mem_region(uart->io_start, uart->io_size);
    kfree(uart->r_buff);
    uart->r_put = uart->r_get = uart->r_buff = NULL;
    kfree(uart->w_buff);
    uart->w_buff = NULL;
    /* kernel structures cleanup */
    device_destroy(klass, MKDEV(pl011_major, MINOR_FIRST));
    cdev_del(&uart->chrdev);
    return 0;
}

static int __init pl011_init(void)
{
    int err=0, err_flag=0;
    dev_t devt=0;
    // get minor nb
    err = alloc_chrdev_region(&devt, MINOR_FIRST, MINOR_NB, DEV_NAME);
    if(err<0)
        goto fail_step1;
    //create a device class- will fail if class already exists
    pl011_major = MAJOR(devt);
    pl011_class = class_create(THIS_MODULE, DEV_NAME);
    if( IS_ERR(pl011_class) )
        goto fail_step2;
    // allocate the device
    pl011_device = (struct pl011_dev *) kzalloc( sizeof(struct pl011_dev),
            GFP_KERNEL);
    if(!pl011_device)
        goto fail_step3;
    //construction of the device with the class
    err = pl011_construct_device(pl011_device, pl011_class);
    if(err)
        goto fail_step4;
    printk(KERN_ALERT "pl011 allocated \n\tnode: /dev/%s\n\tmajor: %d\n",
            DEV_NAME, pl011_major);
    //success
    return 0;
    //fail
fail_step4:    
    if(!err_flag++);            //messages from inside the function
    kfree(pl011_device);
    pl011_device=NULL;
fail_step3:    
    if(!err_flag++)
    {
        printk(KERN_WARNING "kzalloc() failed\n");
        err = -ENOMEM;
    }
    class_destroy(pl011_class);
    pl011_class=NULL;
fail_step2:    
    if(!err_flag++)
    {
        printk(KERN_WARNING "class_create() failed\n");
        err = PTR_ERR(pl011_class);
    }
    unregister_chrdev_region(MKDEV(pl011_major, MINOR_FIRST), MINOR_NB);
fail_step1:    
    if(!err_flag++)
        printk(KERN_WARNING "alloc_chrdev_region() failed\n");
    return err;
}

static void __exit pl011_exit(void)
{
    pl011_destroy_device(pl011_device, pl011_class);
    kfree(pl011_device);
    pl011_device=NULL;
    class_destroy(pl011_class);
    pl011_class=NULL;
    unregister_chrdev_region(MKDEV(pl011_major, MINOR_FIRST), MINOR_NB);
    printk(KERN_ALERT "Unregistered & Unloaded\n");
    return;
}

module_init(pl011_init);
module_exit(pl011_exit);

MODULE_AUTHOR("Maciej Bielski");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("An example character device driver");
