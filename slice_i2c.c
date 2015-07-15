/*
 * Copyright (C) 2015 Motorola Mobility, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/slice_attach.h>

#include "endo.h"
#include "greybus.h"
#include "svc_msg.h"

#define SLICE_I2C_GBUF_MSG_SIZE_MAX	PAGE_SIZE

#define HS_PAYLOAD_SIZE   (sizeof(struct svc_function_handshake))
#define HS_MSG_SIZE       (sizeof(struct svc_msg_header) + HS_PAYLOAD_SIZE)

/* SVC message header + 2 bytes of payload */
#define HP_BASE_SIZE      (sizeof(struct svc_msg_header) + 2)

#define SR_PAYLOAD_SIZE   (sizeof(struct svc_function_unipro_management))
#define SR_MSG_SIZE       (sizeof(struct svc_msg_header) + SR_PAYLOAD_SIZE)

#define UNIPRO_RX_MSG_SIZE 2

#define SLICE_REG_INT      0x00
#define SLICE_REG_SVC      0x01
#define SLICE_REG_UNIPRO   0x02

#define SLICE_INT_SVC      0x01
#define SLICE_INT_UNIPRO   0x02

struct slice_i2c_data {
	struct i2c_client *client;
	struct greybus_host_device *hd;
	bool present;
	struct notifier_block attach_nb;   /* attach/detach notifications */
};

#pragma pack(push, 1)
/* for messages from bundle -> base                           */
/* this should match struct slice_unipro_msg_tx on module side */
struct unipro_header {
	__u8	checksum;
	__u8	hd_cport;
	__le16	length;
};

/* for messages from base -> bundle                            */
/* this should match struct slice_unipro_msg_rx on module side */
struct slice_i2c_msg_hdr {
	uint8_t  checksum;
	uint8_t  intf_cport_id;
	uint8_t  hd_cport_id;
};

struct slice_i2c_msg {
	struct slice_i2c_msg_hdr hdr;
	uint8_t data[0];
};
#pragma pack(pop)

static inline struct slice_i2c_data *hd_to_dd(struct greybus_host_device *hd)
{
	return (struct slice_i2c_data *)&hd->hd_priv;
}

static int slice_i2c_write_reg(struct slice_i2c_data *dd, uint8_t reg,
			       uint8_t *buf, int size)
{
	struct i2c_msg msgs[2];

	msgs[0].addr = dd->client->addr;
	msgs[0].flags = dd->client->flags;
	msgs[0].len = sizeof(reg);
	msgs[0].buf = &reg;

	msgs[1].addr = dd->client->addr;
	msgs[1].flags = dd->client->flags;
	msgs[1].len = size;
	msgs[1].buf = buf;

	return i2c_transfer(dd->client->adapter, msgs, 2);
}

static int slice_i2c_read_reg(struct slice_i2c_data *dd, uint8_t reg,
			      uint8_t *buf, int size)
{
	struct i2c_msg msgs[2];

	msgs[0].addr = dd->client->addr;
	msgs[0].flags = dd->client->flags;
	msgs[0].len = sizeof(reg);
	msgs[0].buf = &reg;

	msgs[1].addr = dd->client->addr;
	msgs[1].flags = dd->client->flags | I2C_M_RD;
	msgs[1].len = size;
	msgs[1].buf = buf;

	return i2c_transfer(dd->client->adapter, msgs, 2);
}

static void send_hot_unplug(struct greybus_host_device *hd, int iid)
{
	struct svc_msg msg;

	msg.header.function_id = SVC_FUNCTION_HOTPLUG;
	msg.header.message_type = SVC_MSG_DATA;
	msg.header.payload_length = 2;
	msg.hotplug.hotplug_event = SVC_HOTUNPLUG_EVENT;
	msg.hotplug.interface_id = iid;

	/* Write out hotplug message */
	greybus_svc_in(hd, (u8 *)&msg, HP_BASE_SIZE);

	printk("%s: SVC->AP hotplug event (unplug) sent\n", __func__);
}

static irqreturn_t slice_i2c_isr(int irq, void *data)
{
	struct greybus_host_device *hd = data;
	struct slice_i2c_data *dd = hd_to_dd(hd);
	uint8_t reg_int;
	struct svc_msg_header svc_hdr;
	uint8_t *rx_buf;
	int msg_size;
	struct unipro_header unipro_hdr;
	int i;
	uint8_t checksum;

	/* Any interrupt while the slice is not attached would be spurious */
	if (!dd->present)
		return IRQ_HANDLED;

	if (slice_i2c_read_reg(dd, SLICE_REG_INT,
			(uint8_t *)&reg_int, sizeof(reg_int)) <= 0) {
		/* read failed */
		return IRQ_HANDLED;
	}

	if (reg_int & SLICE_INT_SVC) {
		slice_i2c_read_reg(dd, SLICE_REG_SVC,
				   (uint8_t *)&svc_hdr, sizeof(svc_hdr));
		printk("svc length = %d\n", svc_hdr.payload_length);
		if (svc_hdr.payload_length > 0) {
			msg_size = sizeof(svc_hdr) + svc_hdr.payload_length;
			rx_buf = kmalloc(msg_size, GFP_KERNEL);
			if (rx_buf) {
				slice_i2c_read_reg(dd, SLICE_REG_SVC,
						   rx_buf, msg_size);
				greybus_svc_in(hd, rx_buf, msg_size);
				kfree(rx_buf);
			} else {
				printk("%s: no memory\n", __func__);
			}
		}
	} else if (reg_int & SLICE_INT_UNIPRO) {
		/* Read out unipro message header to get message length */
		slice_i2c_read_reg(dd, SLICE_REG_UNIPRO,
				   (uint8_t *)&unipro_hdr, sizeof(unipro_hdr));

		/* Sanity check message length */
		if (unipro_hdr.length <= 0) {
			printk("%s: no msg length?\n", __func__);
			goto out;
		}

		/* Allocate required buffer to receive unipro message */
		msg_size = UNIPRO_RX_MSG_SIZE + unipro_hdr.length;
		rx_buf = kmalloc(msg_size, GFP_KERNEL);
		if (!rx_buf) {
			printk("%s: no memory\n", __func__);
			goto out;
		}

		/* Read out entire unipro message */
		slice_i2c_read_reg(dd, SLICE_REG_UNIPRO, rx_buf, msg_size);

		/* Calculate the checksum */
		for (i = 0, checksum = 0; i < msg_size; ++i) {
			checksum += rx_buf[i];
		}

		if (checksum)
			printk("%s: checksum non-zero: 0x%0X\n",
			       __func__, checksum);
		else
			greybus_data_rcvd(hd, unipro_hdr.hd_cport,
					  rx_buf + UNIPRO_RX_MSG_SIZE,
					  unipro_hdr.length);
		kfree(rx_buf);
	}

out:
	return IRQ_HANDLED;
}

static int slice_attach(struct notifier_block *nb,
		unsigned long now_present, void *not_used)
{
	struct slice_i2c_data *dd = container_of(nb, struct slice_i2c_data, attach_nb);
	struct greybus_host_device *hd = dd->hd;
	struct i2c_client *client = dd->client;

	if (now_present != dd->present) {
		printk("%s: Slice is attach state = %lu\n", __func__, now_present);

		dd->present = now_present;

		if (now_present) {
			if (devm_request_threaded_irq(&client->dev,
						   client->irq, NULL,
					           slice_i2c_isr,
						   IRQF_TRIGGER_LOW |
						   IRQF_ONESHOT,
						   "slice_i2c", hd))
				printk(KERN_ERR "%s: Unable to request irq.\n", __func__);
		} else {
			devm_free_irq(&client->dev, client->irq, hd);
			send_hot_unplug(hd, 1);
		}
	}
	return NOTIFY_OK;
}

static int slice_i2c_submit_svc(struct svc_msg *svc_msg,
				struct greybus_host_device *hd)
{
	struct slice_i2c_data *dd = hd_to_dd(hd);

	printk("%s: header.function_id = %d\n", __func__,
	       svc_msg->header.function_id);

	switch (svc_msg->header.function_id) {
	case SVC_FUNCTION_HANDSHAKE:
		printk("%s: AP->SVC handshake\n", __func__);
		slice_i2c_write_reg(dd, SLICE_REG_SVC, (uint8_t *)svc_msg, HS_MSG_SIZE);
		break;

	case SVC_FUNCTION_UNIPRO_NETWORK_MANAGEMENT:
		printk("%s: AP -> SVC set route to Device ID %d\n",
		       __func__, svc_msg->management.set_route.device_id);
		slice_i2c_write_reg(dd, SLICE_REG_SVC, (uint8_t *)svc_msg, SR_MSG_SIZE);
		break;
	}

	return 0;
}

/*
 * Returns an opaque cookie value if successful, or a pointer coded
 * error otherwise.  If the caller wishes to cancel the in-flight
 * buffer, it must supply the returned cookie to the cancel routine.
 */
static void *slice_i2c_message_send(struct greybus_host_device *hd, u16 hd_cport_id,
				   struct gb_message *message, gfp_t gfp_mask)
{
	struct slice_i2c_data *dd = hd_to_dd(hd);
	void *buffer;
	size_t buffer_size;
	struct slice_i2c_msg *tx_buf;
	int tx_buf_size;
	struct gb_connection *connection;
	int i;

	connection = gb_connection_hd_find(hd, hd_cport_id);

	buffer = message->buffer;
	buffer_size = sizeof(*message->header) + message->payload_size;

	tx_buf_size = buffer_size + sizeof(struct slice_i2c_msg_hdr);
	tx_buf = (struct slice_i2c_msg *)kmalloc(tx_buf_size, GFP_KERNEL);
	if (!tx_buf) {
		printk("%s: no memory\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	if (!connection) {
		pr_err("Invalid cport supplied to send\n");
		return ERR_PTR(-EINVAL);
	}

	printk("%s: AP (CPort %d) -> Module (CPort %d)\n",
	       __func__, connection->hd_cport_id, connection->intf_cport_id);

	/* The slice expects the first byte to be the checksum, the second byte
	 * to be the destination cport, and third byte to be the source cport.
	 */
	tx_buf->hdr.hd_cport_id = connection->hd_cport_id;
	tx_buf->hdr.intf_cport_id = connection->intf_cport_id;
	tx_buf->hdr.checksum = 0; // Checksum to be filled in below
	memcpy(&tx_buf->data, buffer, buffer_size);

	/* Calculate checksum */
	for (i = 1; i < tx_buf_size; ++i)
		tx_buf->hdr.checksum += ((uint8_t *)tx_buf)[i];
	tx_buf->hdr.checksum = ~tx_buf->hdr.checksum + 1;

	slice_i2c_write_reg(dd, SLICE_REG_UNIPRO, (uint8_t *)tx_buf, tx_buf_size);

	kfree(tx_buf);

	return NULL;
}

/*
 * The cookie value supplied is the value that message_send()
 * returned to its caller.  It identifies the buffer that should be
 * canceled.  This function must also handle (which is to say,
 * ignore) a null cookie value.
 */
static void slice_i2c_message_cancel(void *cookie)
{
	printk("%s: enter\n", __func__);
}

static struct greybus_host_driver slice_i2c_host_driver = {
	.hd_priv_size		= sizeof(struct slice_i2c_data),
	.message_send		= slice_i2c_message_send,
	.message_cancel		= slice_i2c_message_cancel,
	.submit_svc		= slice_i2c_submit_svc,
};

static int slice_i2c_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct slice_i2c_data *dd;
	struct greybus_host_device *hd;
	u16 endo_id = 0x4755;
	u8 ap_intf_id = 0x01;
	int retval;

	if (client->irq < 0) {
		pr_err("%s: IRQ not defined\n", __func__);
		return -EINVAL;
	}

	hd = greybus_create_hd(&slice_i2c_host_driver, &client->dev,
			       SLICE_I2C_GBUF_MSG_SIZE_MAX);
	if (!hd) {
		printk(KERN_ERR "%s: Unable to create greybus host driver.\n",
		       __func__);
		return -ENOMEM;
	}

	retval = greybus_endo_setup(hd, endo_id, ap_intf_id);
	if (retval)
		return retval;

	dd = hd_to_dd(hd);
	dd->hd = hd;
	dd->client = client;
	dd->attach_nb.notifier_call = slice_attach;

	i2c_set_clientdata(client, dd);

	register_slice_attach_notifier(&dd->attach_nb);

	return 0;
}

static int slice_i2c_remove(struct i2c_client *client)
{
	struct slice_i2c_data *dd = i2c_get_clientdata(client);

	unregister_slice_attach_notifier(&dd->attach_nb);
	greybus_remove_hd(dd->hd);
	i2c_set_clientdata(client, NULL);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id of_slice_i2c_match[] = {
	{ .compatible = "moto,slice_i2c", },
	{},
};
#endif

static const struct i2c_device_id slice_i2c_id[] = {
	{ "slice_i2c", 0 },
	{ }
};

static struct i2c_driver slice_i2c_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "slice_i2c",
		.of_match_table = of_match_ptr(of_slice_i2c_match),
	},
	.id_table = slice_i2c_id,
	.probe = slice_i2c_probe,
	.remove  = slice_i2c_remove,
};

module_i2c_driver(slice_i2c_driver);

MODULE_AUTHOR("Motorola Mobility, LLC");
MODULE_DESCRIPTION("Slice I2C bus driver");
MODULE_LICENSE("GPL");