// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, Sony Mobile Communications Inc.
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/rpmsg.h>
#include <linux/soc/qcom/qrtr.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "qrtr.h"

struct qrtr_smd_dev {
	struct qrtr_endpoint ep;
	struct rpmsg_endpoint *channel;
	struct device *dev;
	struct workqueue_struct *service_wq;
	spinlock_t rx_lock; /* serialize receive with endpoint removal */
	bool removing;
};

struct qrtr_service_work {
	struct qrtr_smd_dev *parent;
	unsigned int node;
	unsigned int port;
	u16 service;
	u16 instance;
	bool announce;

	struct work_struct work;
};

static int qcom_smd_qrtr_device_match(struct device *dev, const struct device_driver *drv)
{
	struct qrtr_device *qdev = to_qrtr_device(dev);
	struct qrtr_driver *qdrv = to_qrtr_driver(drv);
	const struct qrtr_device_id *id = qdrv->id_table;

	if (!id)
		return 0;

	while (id->service != 0) {
		if (id->service == qdev->service && id->instance == qdev->instance)
			return 1;
		id++;
	}

	return 0;
}

static int qcom_smd_qrtr_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct qrtr_device *qdev = to_qrtr_device(dev);

	return add_uevent_var(env, "MODALIAS=%s%x:%x", QRTR_MODULE_PREFIX, qdev->service,
			      qdev->instance);
}

static int qcom_smd_qrtr_device_probe(struct device *dev)
{
	struct qrtr_device *qdev = to_qrtr_device(dev);
	struct qrtr_driver *qdrv = to_qrtr_driver(dev->driver);

	return qdrv->probe(qdev);
}

static void qcom_smd_qrtr_device_remove(struct device *dev)
{
	struct qrtr_device *qdev = to_qrtr_device(dev);
	struct qrtr_driver *qdrv = to_qrtr_driver(dev->driver);

	if (qdrv->remove)
		qdrv->remove(qdev);
}

const struct bus_type qrtr_bus = {
	.name		= "qrtr",
	.match		= qcom_smd_qrtr_device_match,
	.uevent		= qcom_smd_qrtr_uevent,
	.probe		= qcom_smd_qrtr_device_probe,
	.remove		= qcom_smd_qrtr_device_remove,
};
EXPORT_SYMBOL_NS_GPL(qrtr_bus, "QRTR");

int __qrtr_driver_register(struct qrtr_driver *drv, struct module *owner)
{
	drv->driver.bus = &qrtr_bus;
	drv->driver.owner = owner;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_NS_GPL(__qrtr_driver_register, "QRTR");

void qrtr_driver_unregister(struct qrtr_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_NS_GPL(qrtr_driver_unregister, "QRTR");

static void qcom_smd_qrtr_dev_release(struct device *dev)
{
	struct qrtr_device *qdev = to_qrtr_device(dev);

	kfree(qdev);
}

static int qcom_smd_qrtr_match_device(struct device *dev, const void *data)
{
	const struct qrtr_service_work *service = data;
	const struct qrtr_device *qdev = to_qrtr_device(dev);

	return qdev->node == service->node && qdev->port == service->port;
}

static void qcom_smd_qrtr_register_service(struct qrtr_service_work *service)
{
	struct qrtr_device *qdev;
	int ret;

	qdev = kzalloc_obj(*qdev);
	if (!qdev)
		return;

	qdev->node = service->node;
	qdev->port = service->port;
	qdev->service = service->service;
	qdev->instance = service->instance;
	device_initialize(&qdev->dev);
	qdev->dev.bus = &qrtr_bus;
	qdev->dev.parent = service->parent->dev;
	qdev->dev.release = qcom_smd_qrtr_dev_release;

	ret = dev_set_name(&qdev->dev, "%u-%u", qdev->node, qdev->port);
	if (!ret)
		ret = device_add(&qdev->dev);
	if (ret) {
		dev_err(service->parent->dev, "Failed to register QRTR device: %pe\n",
			ERR_PTR(ret));
		put_device(&qdev->dev);
	}
}

static void qcom_smd_qrtr_service_worker(struct work_struct *work)
{
	struct qrtr_service_work *service = container_of(work, struct qrtr_service_work, work);
	struct device *dev;

	dev = device_find_child(service->parent->dev, service,
				qcom_smd_qrtr_match_device);
	if (dev) {
		const struct qrtr_device *qdev = to_qrtr_device(dev);

		if (service->announce && qdev->service == service->service &&
		    qdev->instance == service->instance) {
			put_device(dev);
			goto out;
		}

		device_unregister(dev);
		put_device(dev);
	}

	if (service->announce)
		qcom_smd_qrtr_register_service(service);
out:
	kfree(service);
}

static int qcom_smd_qrtr_queue_service(struct qrtr_endpoint *parent, unsigned int node,
				       unsigned int port, u16 service, u16 instance,
				       bool announce)
{
	struct qrtr_smd_dev *qsdev = container_of(parent, struct qrtr_smd_dev, ep);
	struct qrtr_service_work *event;

	/* GLINK invokes the receive callback from IRQ context. */
	event = kmalloc_obj(*event, GFP_ATOMIC);
	if (!event)
		return -ENOMEM;

	event->parent = qsdev;
	event->node = node;
	event->port = port;
	event->service = service;
	event->instance = instance;
	event->announce = announce;
	INIT_WORK(&event->work, qcom_smd_qrtr_service_worker);
	queue_work(qsdev->service_wq, &event->work);

	return 0;
}

static int qcom_smd_qrtr_add_device(struct qrtr_endpoint *parent, unsigned int node,
				    unsigned int port, u16 service, u16 instance)
{
	return qcom_smd_qrtr_queue_service(parent, node, port, service, instance, true);
}

static int qcom_smd_qrtr_del_device(struct qrtr_endpoint *parent, unsigned int node,
				    unsigned int port)
{
	return qcom_smd_qrtr_queue_service(parent, node, port, 0, 0, false);
}

static int qcom_smd_qrtr_device_unregister(struct device *dev, void *data)
{
	device_unregister(dev);

	return 0;
}

/* from smd to qrtr */
static int qcom_smd_qrtr_callback(struct rpmsg_device *rpdev,
				  void *data, int len, void *priv, u32 addr)
{
	struct qrtr_smd_dev *qsdev;
	unsigned long flags;
	int rc;

	/* Pairs with the publication in qcom_smd_qrtr_probe(). */
	qsdev = smp_load_acquire(&rpdev->dev.driver_data);
	if (!qsdev)
		return -EAGAIN;

	spin_lock_irqsave(&qsdev->rx_lock, flags);
	rc = qsdev->removing ? -ENODEV : qrtr_endpoint_post(&qsdev->ep, data, len);
	spin_unlock_irqrestore(&qsdev->rx_lock, flags);
	if (rc == -EINVAL) {
		dev_err(qsdev->dev, "invalid ipcrouter packet\n");
		/* return 0 to let smd drop the packet */
		rc = 0;
	}

	return rc;
}

/* from qrtr to smd */
static int qcom_smd_qrtr_send(struct qrtr_endpoint *ep, struct sk_buff *skb)
{
	struct qrtr_smd_dev *qsdev = container_of(ep, struct qrtr_smd_dev, ep);
	int rc;

	rc = skb_linearize(skb);
	if (rc)
		goto out;

	rc = rpmsg_send(qsdev->channel, skb->data, skb->len);

out:
	if (rc)
		kfree_skb(skb);
	else
		consume_skb(skb);
	return rc;
}

static int qcom_smd_qrtr_probe(struct rpmsg_device *rpdev)
{
	struct qrtr_smd_dev *qsdev;
	int rc;

	qsdev = devm_kzalloc(&rpdev->dev, sizeof(*qsdev), GFP_KERNEL);
	if (!qsdev)
		return -ENOMEM;

	qsdev->channel = rpdev->ept;
	qsdev->dev = &rpdev->dev;
	qsdev->ep.xmit = qcom_smd_qrtr_send;
	qsdev->ep.add_device = qcom_smd_qrtr_add_device;
	qsdev->ep.del_device = qcom_smd_qrtr_del_device;
	spin_lock_init(&qsdev->rx_lock);
	qsdev->service_wq = alloc_ordered_workqueue("qrtr-%s", 0, dev_name(qsdev->dev));
	if (!qsdev->service_wq)
		return -ENOMEM;

	rc = qrtr_endpoint_register(&qsdev->ep, QRTR_EP_NID_AUTO);
	if (rc) {
		destroy_workqueue(qsdev->service_wq);
		return rc;
	}

	/* Publish the initialized endpoint to the receive callback. */
	smp_store_release(&rpdev->dev.driver_data, qsdev);

	dev_dbg(&rpdev->dev, "Qualcomm SMD QRTR driver probed\n");

	return 0;
}

static void qcom_smd_qrtr_remove(struct rpmsg_device *rpdev)
{
	struct qrtr_smd_dev *qsdev = dev_get_drvdata(&rpdev->dev);
	unsigned long flags;

	/* Stop new receive work before draining the queue and removing children. */
	spin_lock_irqsave(&qsdev->rx_lock, flags);
	qsdev->removing = true;
	spin_unlock_irqrestore(&qsdev->rx_lock, flags);
	destroy_workqueue(qsdev->service_wq);

	device_for_each_child(qsdev->dev, NULL, qcom_smd_qrtr_device_unregister);

	qrtr_endpoint_unregister(&qsdev->ep);

	WRITE_ONCE(rpdev->dev.driver_data, NULL);
}

static const struct rpmsg_device_id qcom_smd_qrtr_smd_match[] = {
	{ "IPCRTR" },
	{}
};

static struct rpmsg_driver qcom_smd_qrtr_driver = {
	.probe = qcom_smd_qrtr_probe,
	.remove = qcom_smd_qrtr_remove,
	.callback = qcom_smd_qrtr_callback,
	.id_table = qcom_smd_qrtr_smd_match,
	.drv = {
		.name = "qcom_smd_qrtr",
	},
};

static int __init qcom_smd_qrtr_init(void)
{
	int ret;

	ret = bus_register(&qrtr_bus);
	if (ret)
		return ret;

	ret = register_rpmsg_driver(&qcom_smd_qrtr_driver);
	if (ret)
		bus_unregister(&qrtr_bus);

	return ret;
}

static void __exit qcom_smd_qrtr_exit(void)
{
	unregister_rpmsg_driver(&qcom_smd_qrtr_driver);
	bus_unregister(&qrtr_bus);
}

subsys_initcall(qcom_smd_qrtr_init);
module_exit(qcom_smd_qrtr_exit);

MODULE_ALIAS("rpmsg:IPCRTR");
MODULE_DESCRIPTION("Qualcomm IPC-Router SMD interface driver");
MODULE_LICENSE("GPL v2");
