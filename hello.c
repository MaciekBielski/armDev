/* This is an old method of initializing module but should work */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/utsname.h>
#include <linux/moduleparam.h>
#include <linux/kfifo.h>

/* parameters passed during loading module */
static char *arg_str = "<empty>";
static uint arg_val = 0xdeadbeef;
//DEFINE_KFIFO(fifo, int, 8);
static struct kfifo fifo;

module_param(arg_str, charp, S_IRUGO);
module_param(arg_val, uint, S_IRUGO);

static int __init hello_init(void)
{
    printk(KERN_ALERT "Hello ARM world, kernel: %s\n", utsname()->release );
    printk(KERN_ALERT "Params: \n\t%s\n\t%08x\n", arg_str, arg_val );
    int tab[]={-3,-2,-1,0,20,128};
    kfifo_alloc(&fifo, 8*sizeof(int), GFP_KERNEL);
    size_t i=0;
    for(i=0; i<6; i++)
    {
        kfifo_in(&fifo,tab+i, sizeof(int));
        printk(KERN_ALERT "put: %d\n",tab[i]);
    }
    printk(KERN_ALERT "fifo sz: %d\n",kfifo_size(&fifo));
    return 0;
}

static void __exit goodbye_init(void)
{
    size_t i=0;
    for(i=0; i<6; i++)
    {
        int tmp=888;
        kfifo_out(&fifo, &tmp,sizeof(tmp));
        printk(KERN_ALERT "val: %d\n",(int)tmp);
    }
    kfifo_free(&fifo);
    printk(KERN_ALERT "Goodbye ARM world\n");
    printk(KERN_ALERT "Params: \n\t%s\n\t%08x\n", arg_str, arg_val );
}

module_init(hello_init);
module_exit(goodbye_init);

MODULE_AUTHOR("MB");
MODULE_LICENSE("GPL");
