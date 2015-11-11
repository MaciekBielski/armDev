/* This is an old method of initializing module but should work */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/utsname.h>
#include <linux/moduleparam.h>

/* parameters passed during loading module */
static char *arg_str = "<empty>";
static uint arg_val = 0xdeadbeef;

module_param(arg_str, charp, S_IRUGO);
module_param(arg_val, uint, S_IRUGO);

static int __init hello_init(void)
{
    printk(KERN_ALERT "Hello ARM world, kernel: %s\n", utsname()->release );
    printk(KERN_ALERT "Params: \n\t%s\n\t%08x\n", arg_str, arg_val );
    return 0;
}

static void __exit goodbye_init(void)
{
    printk(KERN_ALERT "Goodbye ARM world\n");
    printk(KERN_ALERT "Params: \n\t%s\n\t%08x\n", arg_str, arg_val );
}

module_init(hello_init);
module_exit(goodbye_init);

MODULE_AUTHOR("MB");
MODULE_LICENSE("GPL");
