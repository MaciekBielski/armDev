/* This version involves tasklets to read characters. On each IRQ a tasklet is
 * registered that copies the data to driver's internal buffer and read
 * function reds only from that buffer. It blocks only when this buffer is
 * empty.
 * - can be disabled and reenabled,
 * - run at interrupt time at the same CPU that schedules them,
 * - cat /proc/footasklet (?)
 * - tasklet_schedule(&), tasklet_kill(&) 
 * Kfifo:
 * - no spinlocking required in only one reader/writer
 */
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
#include <linux/interrupt.h>        //request_irq(), tasklet
#include <linux/semaphore.h>        //sem
#include <linux/wait.h>             //wait functions
#include <linux/sched.h>            //schedule()
#include <linux/spinlock.h>         //irq_pending spinlock
#include <asm-generic/current.h>    //current()
#include <linux/kfifo.h>            //generic fifo implementation

#define MINOR_FIRST 0           //first requested minor
#define MINOR_NB 1              //nb of minors requested
#define DEV_NAME "pl011_uart"
#define RBUFF_SZ 64
#define RPACKET_SZ 4
#define WBUFF_SZ 8

//device registers
#define PL011_PHYS_ADD 0xe0000000
#define PL011_MEM_SZ 0x1000
#define PL011_OFFSET( add, offset ) ({ (const void*)((add)+(offset)); })
#define PL011_DR(base) (base)
#define PL011_LCR(base) PL011_OFFSET( (base), 0x2c)
#define PL011_IMSC(base) PL011_OFFSET( (base), 0x38)
#define PL011_ICR(base) PL011_OFFSET( (base), 0x44)

typedef struct pl011_dev
{
    unsigned char *w_buff;
    unsigned char *iomem;
    unsigned long io_start;
    unsigned long io_size;
    struct resource* io_mem_region;
    struct cdev chrdev;
    struct semaphore sem;
    //int irq_pending;
    spinlock_t flag_lock;
    wait_queue_head_t rqh;      //read queue head
    struct kfifo r_fifo;
    struct tasklet_struct r_tasklet;
} pl011_dev;

static struct class *pl011_class = NULL;
static struct pl011_dev *pl011_device = NULL;
static unsigned int pl011_major=0;
//access address - is it possible to not make it global?
static const int irq_nb = 0x14;         //first column in 'cat /proc/interrupts' output
//size_t uart_id = 0xC0CADEAD;          //used for shared IRQ lines

static void pl011_r_tasklet(unsigned long opaque)
{
    /* If tasklet is scheduled while it runs then it will run again after it
     * completes. The device may send a new IRQ only after ioread, but it's
     * corresponding tasklet will be scheduled so everything is OK.
     * The same tasklet never runs in parallel with itself.
     * Separate characters do not really need to be extracted.
     */
    pl011_dev *uart = (pl011_dev *)opaque;
    //do NOT read if there is no room in fifo
    if(kfifo_avail(&uart->r_fifo)>=4)
    {
        uint32_t tmp = ioread32(PL011_DR(uart->iomem));
        uint8_t len=0;
        //left-truncating more than one NULL bytes: 0x00
        //if first 3 bytes are not null then the last one is always taken
        for(; len <sizeof(tmp)-1;  len++)
            if( !((tmp & 0xff<<8*len)>> 8*len) )
                break;
        len++;      //how many bytes will be put in fifo
        kfifo_in(&uart->r_fifo, &tmp,len);
        //printk(KERN_WARNING "tmp: 0x%08x, real_len: %d, elems: %d, avail: %d\n",
        //        tmp, len, kfifo_len(&uart->r_fifo), kfifo_avail(&uart->r_fifo));
        //     kfifo_out(&uart->r_fifo, &c, sizeof(c));
        wake_up_interruptible(&uart->rqh);
    }
}

static irq_handler_t data_handler(int nb, void *dev_id, struct pt_regs *regs)
{
    /* the trick: IRQ is turned off but on read it is checked whether
     * it should be triggered again */
    pl011_dev *uart = (pl011_dev *) dev_id;
    iowrite8(0x10, PL011_ICR(uart->iomem));
    tasklet_schedule(&uart->r_tasklet);
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
    //spin_lock_init(&uart->flag_lock);
    iowrite8(0x10, PL011_LCR(uart->iomem));    //enable FIFO
    smp_wmb();
    //uart->irq_pending=0;
    enable_irq(irq_nb);
    iowrite8(0x70, PL011_IMSC(uart->iomem));    //enable IRQs
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
    /* normally fpos has to be updated to have a correct filep->f_pos
     * at next call but this is not used now yet */
    pl011_dev* uart = (pl011_dev*)filep->private_data;
    int err=0;
    /* sleeps if no data in kfifo, wakes up if any data has appeared */
    down_interruptible(&uart->sem);
    while(kfifo_is_empty(&uart->r_fifo))
    {
        /* process is about to block */
        DEFINE_WAIT(rqe);
        up(&uart->sem);
        prepare_to_wait(&uart->rqh, &rqe, TASK_INTERRUPTIBLE);
          if(kfifo_is_empty(&uart->r_fifo))
            schedule();
        finish_wait(&uart->rqh, &rqe);
        /* necessary for Ctrl-C to work properly */
        if(signal_pending(current))
            return -ERESTARTSYS;
        down_interruptible(&uart->sem);
    }
    up(&uart->sem);
    /* no read is sleeping now */
    //get the data from fifo
    size_t read_sz = kfifo_len(&uart->r_fifo);
    read_sz = read_sz>4 ? 4 : read_sz;
    uint32_t tmp =0;
    kfifo_out(&uart->r_fifo, &tmp, read_sz);
    //printk(KERN_WARNING "read:sz %d\n", read_sz);
    if( copy_to_user((uint32_t *)data, &tmp, read_sz) )
    {
        err = -EFAULT;
        goto out;
    }
    //success
    err = read_sz;
    *fpos += read_sz;
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
        goto fail_cdev_add;
    //creating entries in: '/dev' and '/sys/dev/char'
    device = device_create( klass, NULL, devt, NULL /*opaque*/,
        DEV_NAME "%d", MINOR_FIRST);
    if( IS_ERR(device) )
        goto fail_dev_create;
    // device internal logic setup
    if( !(uart->w_buff = kzalloc( WBUFF_SZ, GFP_KERNEL)))
        goto fail_w_buff;
    uart->io_start = PL011_PHYS_ADD;
    uart->io_size = PL011_MEM_SZ;
    if( !(uart->io_mem_region = request_mem_region(uart->io_start, uart->io_size,
            "pl011_regs")))
        goto fail_io_mem_region;
    uart->iomem = ioremap(uart->io_start, uart->io_size);
    if( (err=kfifo_alloc(&uart->r_fifo, RBUFF_SZ, GFP_KERNEL)) )
        goto fail_kfifo;
    tasklet_init(&uart->r_tasklet, pl011_r_tasklet, uart);
    //success
    return 0;
    //fail
fail_kfifo:
    if(!err_flag++)
    {
        printk(KERN_WARNING "kfifo_alloc() failed\n");
    }
    iounmap(uart->iomem);
fail_io_mem_region:
    if(!err_flag++)
    {
        printk(KERN_WARNING "request_mem_region() failed\n");
        err = -ENODEV;
    }
    //kfree(uart->r_buff);
    kfree(uart->w_buff);
fail_w_buff:
    if(!err_flag++)
    {
        printk(KERN_WARNING "kzalloc() failed\n");
        err = -ENOMEM;
    }
    device_destroy(klass, MKDEV(pl011_major, MINOR_FIRST));
fail_dev_create:
    if(!err_flag++)
    {
        printk(KERN_WARNING "device_create() failed\n");
        err = PTR_ERR(device);
    }
    cdev_del(&uart->chrdev);
fail_cdev_add:
    if(!err_flag++)
        printk(KERN_WARNING "cdev_add() failed\n");
    return err;
}

static int pl011_destroy_device( struct pl011_dev *uart, struct class *klass)
{
    /* device internal logic cleanup */
    tasklet_kill(&uart->r_tasklet);
    kfifo_free(&uart->r_fifo);
    iounmap(uart->iomem);
    release_mem_region(uart->io_start, uart->io_size);
    //kfree(uart->r_buff);
    //uart->r_buff = NULL;
    kfree(uart->w_buff);
    uart->w_buff = NULL;
    /* kernel structures cleanup */
    device_destroy(klass, MKDEV(pl011_major, MINOR_FIRST));
    cdev_del(&uart->chrdev);
    return 0;
}

static int __init pl011_init(void)
{
    printk(KERN_WARNING "%s\n", __TIME__);
    int err=0, err_flag=0;
    dev_t devt=0;
    // get minor nb
    err = alloc_chrdev_region(&devt, MINOR_FIRST, MINOR_NB, DEV_NAME);
    if(err<0)
        goto fail_chrdev_region;
    //create a device class- will fail if class already exists
    pl011_major = MAJOR(devt);
    pl011_class = class_create(THIS_MODULE, DEV_NAME);
    if( IS_ERR(pl011_class) )
        goto fail_class_create;
    // allocate the device
    pl011_device = (struct pl011_dev *) kzalloc( sizeof(struct pl011_dev),
            GFP_KERNEL);
    if(!pl011_device)
        goto fail_dev_alloc;
    //construction of the device with the class
    err = pl011_construct_device(pl011_device, pl011_class);
    if(err)
        goto fail_construct_fun;
    printk(KERN_ALERT "pl011 allocated \n\tnode: /dev/%s\n\tmajor: %d\n",
            DEV_NAME, pl011_major);
    //success
    return 0;
    //fail
fail_construct_fun:    
    if(!err_flag++);            //messages from inside the function
    kfree(pl011_device);
    pl011_device=NULL;
fail_dev_alloc:    
    if(!err_flag++)
    {
        printk(KERN_WARNING "kzalloc() failed\n");
        err = -ENOMEM;
    }
    class_destroy(pl011_class);
    pl011_class=NULL;
fail_class_create:    
    if(!err_flag++)
    {
        printk(KERN_WARNING "class_create() failed\n");
        err = PTR_ERR(pl011_class);
    }
    unregister_chrdev_region(MKDEV(pl011_major, MINOR_FIRST), MINOR_NB);
fail_chrdev_region:    
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
