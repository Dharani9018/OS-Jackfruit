#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/jiffies.h>

#include <linux/version.h>
#include <linux/interrupt.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_MS 1000

extern int del_timer_sync(struct timer_list *timer);

struct monitored_entry {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[CONTAINER_ID_LEN];
    int soft_warned;
    struct list_head list;
};

static dev_t dev_num;
static struct cdev mon_cdev;
static struct class *mon_class;
static struct device *mon_device;
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(list_lock);
static struct timer_list check_timer;

static void check_memory(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;
    struct task_struct *task;
    unsigned long rss_bytes;

    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        rcu_read_lock();
        task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
        rcu_read_unlock();

        if (!task) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (task->mm)
            rss_bytes = get_mm_rss(task->mm) << PAGE_SHIFT;
        else
            continue;

        if (rss_bytes > entry->hard_limit_bytes) {
            printk(KERN_WARNING "container_monitor: container %s pid %d exceeded hard limit (%lu > %lu bytes), killing\n",
                   entry->container_id, entry->pid, rss_bytes, entry->hard_limit_bytes);
            send_sig(SIGKILL, task, 1);
        } else if (rss_bytes > entry->soft_limit_bytes && !entry->soft_warned) {
            entry->soft_warned = 1;
            printk(KERN_INFO "container_monitor: container %s pid %d exceeded soft limit (%lu > %lu bytes)\n",
                   entry->container_id, entry->pid, rss_bytes, entry->soft_limit_bytes);
        }
    }
    mutex_unlock(&list_lock);

    mod_timer(t, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitored_entry *entry, *tmp;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    switch (cmd) {
    case MONITOR_REGISTER:
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) return -ENOMEM;
        entry->pid               = req.pid;
        entry->soft_limit_bytes  = req.soft_limit_bytes;
        entry->hard_limit_bytes  = req.hard_limit_bytes;
        entry->soft_warned       = 0;
        strncpy(entry->container_id, req.container_id, CONTAINER_ID_LEN - 1);
        mutex_lock(&list_lock);
        list_add(&entry->list, &monitored_list);
        mutex_unlock(&list_lock);
        printk(KERN_INFO "container_monitor: registered pid %d container %s\n",
               req.pid, req.container_id);
        break;

    case MONITOR_UNREGISTER:
        mutex_lock(&list_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                break;
            }
        }
        mutex_unlock(&list_lock);
        break;

    default:
        return -EINVAL;
    }

    return 0;
}

static const struct file_operations mon_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void)
{
    int rc;

    rc = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (rc < 0) {
        printk(KERN_ERR "container_monitor: alloc_chrdev_region failed\n");
        return rc;
    }

    cdev_init(&mon_cdev, &mon_fops);
    mon_cdev.owner = THIS_MODULE;
    rc = cdev_add(&mon_cdev, dev_num, 1);
    if (rc < 0) goto err_cdev;

    mon_class = class_create(DEVICE_NAME);
    if (IS_ERR(mon_class)) { rc = PTR_ERR(mon_class); goto err_class; }

    mon_device = device_create(mon_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(mon_device)) { rc = PTR_ERR(mon_device); goto err_device; }

    timer_setup(&check_timer, check_memory, 0);
    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));

    printk(KERN_INFO "container_monitor: loaded\n");
    return 0;

err_device: class_destroy(mon_class);
err_class:  cdev_del(&mon_cdev);
err_cdev:   unregister_chrdev_region(dev_num, 1);
    return rc;
}

static void __exit monitor_exit(void)
{
    struct monitored_entry *entry, *tmp;

    timer_shutdown_sync(&check_timer);

    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_lock);

    device_destroy(mon_class, dev_num);
    class_destroy(mon_class);
    cdev_del(&mon_cdev);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "container_monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Container memory monitor");
