/*
 * Copyright (C) 2014 National Instruments Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device_poll.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/kthread.h>
#include <linux/delay.h>

/* sysfs attributes */

#define to_ext_attr(x) container_of(x, struct dev_ext_attribute, attr)

/* get sysfs attributes */

ssize_t device_poll_get_interval(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	struct device_poll *device_poll = ea->var;

	return sprintf(buf, "%d\n", device_poll->interval);
}

static ssize_t device_poll_get_policy(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	struct device_poll *device_poll = ea->var;

	switch (device_poll->policy) {
	case SCHED_NORMAL:
		return sprintf(buf, "SCHED_NORMAL (SCHED_OTHER)\n");
	case SCHED_FIFO:
		return sprintf(buf, "SCHED_FIFO\n");
	case SCHED_RR:
		return sprintf(buf, "SCHED_RR\n");
	case SCHED_BATCH:
		return sprintf(buf, "SCHED_BATCH\n");
	case SCHED_IDLE:
		return sprintf(buf, "SCHED_IDLE\n");
	default:
		return sprintf(buf, "unknown\n");
	}
}

static ssize_t device_poll_get_priority(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	struct device_poll *device_poll = ea->var;

	return sprintf(buf, "%d\n", device_poll->priority);
}

/* set sysfs attributes */

static ssize_t device_poll_set_interval(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	struct device_poll *device_poll = ea->var;
	int interval;
	int ret = 0;

	if (device_poll->use_capability && !capable(device_poll->capability))
		return -EPERM;

	if (0 > kstrtoint(buf, 0, &interval))
		return -EINVAL;

	device_poll->ops->lock(device_poll);
	if (device_poll->interval != interval) {
		device_poll->interval = interval;

		ret = device_poll->ops->reinit(device_poll);
	}
	device_poll->ops->unlock(device_poll);

	if (ret)
		return ret;

	return size;
}

static ssize_t device_poll_set_policy(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	struct device_poll *device_poll = ea->var;
	char policy_str[16] = { 0 };
	int policy;
	struct sched_param param;

	if (device_poll->use_capability && !capable(device_poll->capability))
		return -EPERM;

	if (1 != sscanf(buf, "%15s", policy_str))
		return -EINVAL;

	if ((0 == strcmp(policy_str, "SCHED_NORMAL") ||
	     (0 == strcmp(policy_str, "SCHED_OTHER"))))
		policy = SCHED_NORMAL;
	else if (0 == strcmp(policy_str, "SCHED_FIFO"))
		policy = SCHED_FIFO;
	else if (0 == strcmp(policy_str, "SCHED_RR"))
		policy = SCHED_RR;
	else if (0 == strcmp(policy_str, "SCHED_BATCH"))
		policy = SCHED_BATCH;
	else if (0 == strcmp(policy_str, "SCHED_IDLE"))
		policy = SCHED_IDLE;
	else
		return -EINVAL;

	device_poll->ops->lock(device_poll);
	if (device_poll->policy != policy) {
		device_poll->policy = policy;

		if (device_poll->task) {
			param.sched_priority = device_poll->priority;
			sched_setscheduler(device_poll->task,
					   device_poll->policy,
					   &param);
		}
	}
	device_poll->ops->unlock(device_poll);

	return size;
}

static ssize_t device_poll_set_priority(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	struct device_poll *device_poll = ea->var;
	int priority;
	struct sched_param param;

	if (device_poll->use_capability && !capable(device_poll->capability))
		return -EPERM;

	if (0 > kstrtoint(buf, 0, &priority))
		return -EINVAL;

	device_poll->ops->lock(device_poll);
	if (device_poll->priority != priority) {
		device_poll->priority = priority;

		if (device_poll->task) {
			param.sched_priority = device_poll->priority;
			sched_setscheduler(device_poll->task,
					   device_poll->policy,
					   &param);
		}
	}
	device_poll->ops->unlock(device_poll);

	return size;
}

/* sysfs attributes */

static const DEVICE_ATTR(interval, S_IWUSR | S_IRUGO,
			 device_poll_get_interval, device_poll_set_interval);

static const DEVICE_ATTR(policy, S_IWUSR | S_IRUGO,
			 device_poll_get_policy, device_poll_set_policy);

static const DEVICE_ATTR(priority, S_IWUSR | S_IRUGO,
			 device_poll_get_priority, device_poll_set_priority);

/* device_poll internal functions */

static int device_poll_thread(void *info)
{
	struct device_poll *device_poll = info;
	int polling_interval;
	int polling_interval_us;
	struct sched_param param;

	polling_interval = device_poll->interval;

	/* If we got changed to interrupt mode before the polling thread
	   started. */
	if (unlikely(0 >= polling_interval)) {
		while (!kthread_should_stop())
			usleep_range(1000, 1100);
		return -EINTR;
	}

	polling_interval_us = polling_interval * 1000;

	param.sched_priority = device_poll->priority;
	sched_setscheduler(current, device_poll->policy, &param);

	while (!kthread_should_stop()) {
		/* Ensure changes to device_poll->enabled made on other CPUs
		   are seen here. */
		smp_rmb();
		if (device_poll->enabled)
			device_poll->ops->interrupt(device_poll);

		if (20 > polling_interval)
			usleep_range(polling_interval_us, polling_interval_us
				     + 100);
		else
			msleep(polling_interval);
	}

	return 0;
}

/* device_poll external functions */

int device_poll_init(struct device_poll *device_poll)
{
	int ret;

	if (!device_poll || !device_poll->device || !device_poll->ops)
		return -EINVAL;

	if (!device_poll->ops->reinit || !device_poll->ops->lock ||
	    !device_poll->ops->unlock || !device_poll->ops->interrupt)
		return -EINVAL;

	if (device_poll->use_capability && !cap_valid(device_poll->capability))
		return -EINVAL;

	device_poll->task = NULL;
	device_poll->enabled = 0;

	device_poll->interval_attr.attr = dev_attr_interval;
	device_poll->policy_attr.attr = dev_attr_policy;
	device_poll->priority_attr.attr = dev_attr_priority;

	device_poll->interval_attr.var = device_poll;
	device_poll->policy_attr.var = device_poll;
	device_poll->priority_attr.var = device_poll;

	if (device_poll->use_capability) {
		device_poll->interval_attr.attr.attr.mode |=
			(S_IWUSR | S_IRUGO);
		device_poll->policy_attr.attr.attr.mode |=
			(S_IWUSR | S_IRUGO);
		device_poll->priority_attr.attr.attr.mode |=
			(S_IWUSR | S_IRUGO);
	}

	sysfs_attr_init(&device_poll->interval_attr.attr.attr);
	sysfs_attr_init(&device_poll->policy_attr.attr.attr);
	sysfs_attr_init(&device_poll->priority_attr.attr.attr);

	device_poll->attrs[0] = &device_poll->interval_attr.attr.attr;
	device_poll->attrs[1] = &device_poll->policy_attr.attr.attr;
	device_poll->attrs[2] = &device_poll->priority_attr.attr.attr;
	device_poll->attrs[3] = NULL;

	device_poll->attr_group.name = "device_poll";
	device_poll->attr_group.attrs = device_poll->attrs;

	ret = sysfs_create_group(&device_poll->device->kobj,
				 &device_poll->attr_group);
	if (ret)
		device_poll_exit(device_poll);

	return ret;
}
EXPORT_SYMBOL(device_poll_init);

void device_poll_exit(struct device_poll *device_poll)
{
	if (!device_poll || !device_poll->device)
		return;

	sysfs_remove_group(&device_poll->device->kobj,
			   &device_poll->attr_group);
}
EXPORT_SYMBOL(device_poll_exit);

int device_poll_request_irq(struct device_poll *device_poll)
{
	int err;

	if (!device_poll)
		return -EINVAL;

	/* If interrupts are enabled. */
	if (device_poll->interval <= 0)
		return -ERANGE;

	/* Start up the polling thread. */
	device_poll->task = kthread_run(device_poll_thread,
					device_poll, "poll/%s",
					dev_name(device_poll->device));
	if (IS_ERR(device_poll->task)) {
		err = PTR_ERR(device_poll->task);
		device_poll->task = NULL;
		dev_err(device_poll->device,
			"Unable to create polling thread: %d\n", err);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL(device_poll_request_irq);

void device_poll_free_irq(struct device_poll *device_poll)
{
	if (device_poll_is_active(device_poll)) {
		kthread_stop(device_poll->task);
		device_poll->task = NULL;
	}
}
EXPORT_SYMBOL(device_poll_free_irq);
