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
#include <linux/delay.h>
#include <linux/workqueue.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_MS 1000

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
static struct delayed_work check_work;
static int module_running = 1;

// Function to get RSS for a process
static unsigned long get_process_rss(pid_t pid)
{
    struct task_struct *task;
    unsigned long rss_bytes = 0;
    
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm) {
        rss_bytes = get_mm_rss(task->mm) << PAGE_SHIFT;
    }
    rcu_read_unlock();
    
    return rss_bytes;
}

// Periodic checker function
static void check_memory_usage(struct work_struct *work)
{
    struct monitored_entry *entry, *tmp;
    unsigned long rss_bytes;
    
    if (!module_running)
        return;
    
    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        rss_bytes = get_process_rss(entry->pid);
        
        // Check if process still exists
        if (rss_bytes == 0 && !pid_task(find_vpid(entry->pid), PIDTYPE_PID)) {
            printk(KERN_INFO "container_monitor: process %d (%s) no longer exists, removing\n",
                   entry->pid, entry->container_id);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }
        
        // Hard limit enforcement
        if (rss_bytes > entry->hard_limit_bytes) {
            printk(KERN_WARNING "container_monitor: HARD LIMIT EXCEEDED - container %s pid %d RSS=%lu > hard=%lu, KILLING\n",
                   entry->container_id, entry->pid, rss_bytes, entry->hard_limit_bytes);
            
            // Kill the process
            struct task_struct *task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
            if (task) {
                send_sig(SIGKILL, task, 1);
            }
            
            // Remove from monitoring list
            list_del(&entry->list);
            kfree(entry);
        }
        // Soft limit warning
        else if (rss_bytes > entry->soft_limit_bytes && !entry->soft_warned) {
            entry->soft_warned = 1;
            printk(KERN_INFO "container_monitor: SOFT LIMIT EXCEEDED - container %s pid %d RSS=%lu > soft=%lu\n",
                   entry->container_id, entry->pid, rss_bytes, entry->soft_limit_bytes);
        }
        // Reset warning if back under soft limit
        else if (rss_bytes <= entry->soft_limit_bytes && entry->soft_warned) {
            entry->soft_warned = 0;
            printk(KERN_INFO "container_monitor: container %s pid %d back under soft limit (RSS=%lu)\n",
                   entry->container_id, entry->pid, rss_bytes);
        }
    }
    mutex_unlock(&list_lock);
    
    // Reschedule the work
    if (module_running) {
        schedule_delayed_work(&check_work, msecs_to_jiffies(CHECK_INTERVAL_MS));
    }
}

// ioctl handler
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitored_entry *entry, *existing;
    int found = 0;
    
    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;
    
    switch (cmd) {
    case MONITOR_REGISTER:
        // Check if already registered
        mutex_lock(&list_lock);
        list_for_each_entry(existing, &monitored_list, list) {
            if (existing->pid == req.pid) {
                found = 1;
                break;
            }
        }
        
        if (found) {
            mutex_unlock(&list_lock);
            printk(KERN_WARNING "container_monitor: pid %d already registered\n", req.pid);
            return -EEXIST;
        }
        
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) {
            mutex_unlock(&list_lock);
            return -ENOMEM;
        }
        
        entry->pid = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned = 0;
        strncpy(entry->container_id, req.container_id, CONTAINER_ID_LEN - 1);
        entry->container_id[CONTAINER_ID_LEN - 1] = '\0';
        
        list_add(&entry->list, &monitored_list);
        mutex_unlock(&list_lock);
        
        printk(KERN_INFO "container_monitor: REGISTERED container %s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);
        break;
        
    case MONITOR_UNREGISTER:
        mutex_lock(&list_lock);
        list_for_each_entry_safe(entry, existing, &monitored_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                printk(KERN_INFO "container_monitor: UNREGISTERED pid=%d\n", req.pid);
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
    .owner = THIS_MODULE,
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
    if (IS_ERR(mon_class)) {
        rc = PTR_ERR(mon_class);
        goto err_class;
    }
    
    mon_device = device_create(mon_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(mon_device)) {
        rc = PTR_ERR(mon_device);
        goto err_device;
    }
    
    // Initialize workqueue for periodic checking
    INIT_DELAYED_WORK(&check_work, check_memory_usage);
    schedule_delayed_work(&check_work, msecs_to_jiffies(CHECK_INTERVAL_MS));
    
    printk(KERN_INFO "container_monitor: LOADED - monitoring started (interval=%dms)\n", CHECK_INTERVAL_MS);
    return 0;
    
err_device:
    class_destroy(mon_class);
err_class:
    cdev_del(&mon_cdev);
err_cdev:
    unregister_chrdev_region(dev_num, 1);
    return rc;
}

static void __exit monitor_exit(void)
{
    struct monitored_entry *entry, *tmp;
    
    // Stop the checker
    module_running = 0;
    cancel_delayed_work_sync(&check_work);
    
    // Free all entries
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
    
    printk(KERN_INFO "container_monitor: UNLOADED\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Container Memory Monitor with Soft/Hard Limits");
MODULE_AUTHOR("Container Runtime");
