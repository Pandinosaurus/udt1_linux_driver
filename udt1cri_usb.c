/* SocketCAN driver for UniSwarm UDT1CRI CAN debugger
 *
 * Copyright (C) 2018 UniSwarm
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published
 * by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.
 *
 * This driver is inspired by the 4.6.2 version of net/can/usb/usb_8dev.c
 * and net/can/usb/mcba_usb.c
 */

#include <asm/unaligned.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/can/led.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/usb.h>

/* vendor and product id */
#define UDT1CRI_MODULE_NAME "udt1cri_usb"
#define UDT1CRI_VENDOR_ID 0x04d8
#define UDT1CRI_PRODUCT_ID 0xee0c

/* driver constants */
#define UDT1CRI_MAX_RX_URBS 20
#define UDT1CRI_MAX_TX_URBS 20
#define UDT1CRI_CTX_FREE UDT1CRI_MAX_TX_URBS

/* RX buffer must be bigger than msg size since at the
 * beggining USB messages are stacked.
 */
#define UDT1CRI_USB_RX_BUFF_SIZE 512
#define UDT1CRI_USB_TX_BUFF_SIZE (sizeof(struct udt1cri_usb_msg))

/* UDT1CRI endpoint numbers */
#define UDT1CRI_USB_EP_IN 1
#define UDT1CRI_USB_EP_OUT 1

#define UDT1CRI_CAN_CLOCK 40000000

/* Microchip command id */
#define UDT1CRI_CMD_RECEIVE_MESSAGE 0xE3
#define UDT1CRI_CMD_I_AM_ALIVE_FROM_CAN 0xF5
#define UDT1CRI_CMD_I_AM_ALIVE_FROM_USB 0xF7
#define UDT1CRI_CMD_CHANGE_BIT_RATE 0xA1
#define UDT1CRI_CMD_TRANSMIT_MESSAGE_EV 0xA3
#define UDT1CRI_CMD_SETUP_TERMINATION_RESISTANCE 0xA8
#define UDT1CRI_CMD_READ_FW_VERSION 0xA9
#define UDT1CRI_CMD_NOTHING_TO_SEND 0xFF
#define UDT1CRI_CMD_TRANSMIT_MESSAGE_RSP 0xE2

#define UDT1CRI_VER_REQ_USB 1
#define UDT1CRI_VER_REQ_CAN 2

#define UDT1CRI_DLC_MASK 0xf
#define UDT1CRI_DLC_RTR_MASK 0x40

#define UDT1CRI_CAN_RTR_MASK            0x40000000
#define UDT1CRI_CAN_EXID_MASK           0x80000000

#define UDT1CRI_RX_IS_RTR(usb_msg)     ((usb_msg)->eid & UDT1CRI_DLC_RTR_MASK)
#define UDT1CRI_RX_IS_EXID(usb_msg)    ((usb_msg)->eid & UDT1CRI_CAN_EXID_MASK)
#define UDT1CRI_TX_IS_RTR(can_frame)   ((can_frame)->can_id & UDT1CRI_CAN_RTR_MASK)
#define UDT1CRI_TX_IS_EXID(can_frame)  ((can_frame)->can_id & UDT1CRI_CAN_EXID_MASK)

struct udt1cri_usb_ctx {
	struct udt1cri_priv *priv;
	u32 ndx;
	u8 dlc;
	bool can;
};

/* Structure to hold all of our device specific stuff */
struct udt1cri_priv {
	struct can_priv can; /* must be the first member */
	struct sk_buff *echo_skb[UDT1CRI_MAX_TX_URBS];
	struct udt1cri_usb_ctx tx_context[UDT1CRI_MAX_TX_URBS];
	struct usb_device *udev;
	struct net_device *netdev;
	struct usb_anchor tx_submitted;
	struct usb_anchor rx_submitted;
	struct can_berr_counter bec;
	u8 termination_state;
	bool usb_ka_first_pass;
	bool can_ka_first_pass;
};

/* command frame */
struct __packed udt1cri_usb_msg_can {
	u8 cmd_id;
	u8 dlc;
	u8 flags;
	u8 checksum;
	u32 eid;
	u32 timestamp;
	u8 data[8];
};
#define FLAG_CAN_EID 0x01
#define FLAG_CAN_RTR 0x02
#define FLAG_CAN_FDF 0x08

/* command frame */
struct __packed udt1cri_usb_msg {
	u8 cmd_id;
	u8 unused[19];
};

struct __packed udt1cri_usb_msg_ka_usb {
	u8 cmd_id;
	u8 termination_state;
	u8 soft_ver_major;
	u8 soft_ver_minor;
	u8 unused[16];
};

struct __packed udt1cri_usb_msg_ka_can {
	u8 cmd_id;
	u8 tx_err_cnt;
	u8 rx_err_cnt;
	u8 rx_buff_ovfl;
	u8 tx_bus_off;
	u8 can_bitrate_hi;
	u8 can_bitrate_lo;
	u8 rx_lost_lo;
	u8 rx_lost_hi;
	u8 can_stat;
	u8 soft_ver_major;
	u8 soft_ver_minor;
	u8 debug_mode;
	u8 test_complete;
	u8 test_result;
	u8 unused[5];
};

struct __packed udt1cri_usb_msg_change_bitrate {
	u8 cmd_id;
	u8 bitrate_hi;
	u8 bitrate_lo;
	u8 unused[17];
};

struct __packed udt1cri_usb_msg_terminaton {
	u8 cmd_id;
	u8 termination;
	u8 unused[18];
};

struct __packed udt1cri_usb_msg_fw_ver {
	u8 cmd_id;
	u8 pic;
	u8 unused[18];
};

struct bitrate_settings {
	struct can_bittiming bt;
	u16 kbps;
};

/* Required by can-dev but not for the sake of driver as CANBUS is USB based */
static const struct can_bittiming_const udt1cri_bittiming_const = {
	.name = "udt1cri_usb",
	.tseg1_min = 1,
	.tseg1_max = 8,
	.tseg2_min = 1,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 2,
	.brp_max = 128,
	.brp_inc = 2,
};

/* predefined values hardcoded in device's firmware */
static const struct bitrate_settings br_settings[] = {
	{
		.bt = {
			.bitrate = 19940,
			.sample_point = 700,
			.tq = 2500,
			.prop_seg = 5,
			.phase_seg1 = 8,
			.phase_seg2 = 6,
			.sjw = 1,
			.brp = 100,
		},
		.kbps = 20
	},
	{
		.bt = {
			.bitrate = 33333,
			.sample_point = 680,
			.tq = 1200,
			.prop_seg = 8,
			.phase_seg1 = 8,
			.phase_seg2 = 8,
			.sjw = 1,
			.brp = 48,
		},
		.kbps = 33
	},
	{
		.bt = {
			.bitrate = 50000,
			.sample_point = 800,
			.tq = 1000,
			.prop_seg = 8,
			.phase_seg1 = 7,
			.phase_seg2 = 4,
			.sjw = 1,
			.brp = 40,
		},
		.kbps = 50
	},
	{
		.bt = {
			.bitrate = 80000,
			.sample_point = 680,
			.tq = 500,
			.prop_seg = 8,
			.phase_seg1 = 8,
			.phase_seg2 = 8,
			.sjw = 1,
			.brp = 20,
		},
		.kbps = 80
	},
	{
		.bt = {
			.bitrate = 83333,
			.sample_point = 708,
			.tq = 500,
			.prop_seg = 8,
			.phase_seg1 = 8,
			.phase_seg2 = 7,
			.sjw = 1,
			.brp = 20,
		},
		.kbps = 83
	},
	{
		.bt = {
			.bitrate = 100000,
			.sample_point = 700,
			.tq = 1000,
			.prop_seg = 1,
			.phase_seg1 = 5,
			.phase_seg2 = 3,
			.sjw = 1,
			.brp = 40,
		},
		.kbps = 100
	},
	{
		.bt = {
			.bitrate = 125000,
			.sample_point = 600,
			.tq = 400,
			.prop_seg = 3,
			.phase_seg1 = 8,
			.phase_seg2 = 8,
			.sjw = 1,
			.brp = 16,
		},
		.kbps = 125
	},
	{
		.bt = {
			.bitrate = 150375,
			.sample_point = 789,
			.tq = 350,
			.prop_seg = 8,
			.phase_seg1 = 6,
			.phase_seg2 = 4,
			.sjw = 1,
			.brp = 14,
		},
		.kbps = 150
	},

	{
		.bt = {
			.bitrate = 175438,
			.sample_point = 789,
			.tq = 300,
			.prop_seg = 8,
			.phase_seg1 = 6,
			.phase_seg2 = 4,
			.sjw = 1,
			.brp = 12,
		},
		.kbps = 175
	},
	{
		.bt = {
			.bitrate = 200000,
			.sample_point = 680,
			.tq = 200,
			.prop_seg = 8,
			.phase_seg1 = 8,
			.phase_seg2 = 8,
			.sjw = 1,
			.brp = 8,
		},
		.kbps = 200
	},
	{
		.bt = {
			.bitrate = 227272,
			.sample_point = 772,
			.tq = 200,
			.prop_seg = 8,
			.phase_seg1 = 8,
			.phase_seg2 = 5,
			.sjw = 1,
			.brp = 8,
		},
		.kbps = 225
	},
	{
		.bt = {
			.bitrate = 250000,
			.sample_point = 600,
			.tq = 200,
			.prop_seg = 3,
			.phase_seg1 = 8,
			.phase_seg2 = 8,
			.sjw = 1,
			.brp = 8,
		},
		.kbps = 250
	},
	{
		.bt = {
			.bitrate = 277777,
			.sample_point = 708,
			.tq = 150,
			.prop_seg = 8,
			.phase_seg1 = 8,
			.phase_seg2 = 7,
			.sjw = 1,
			.brp = 6,
		},
		.kbps = 275
	},
	{
		.bt = {
			.bitrate = 303030,
			.sample_point = 772,
			.tq = 150,
			.prop_seg = 8,
			.phase_seg1 = 8,
			.phase_seg2 = 5,
			.sjw = 1,
			.brp = 6,
		},
		.kbps = 300
	},
	{
		.bt = {
			.bitrate = 500000,
			.sample_point = 600,
			.tq = 100,
			.prop_seg = 3,
			.phase_seg1 = 8,
			.phase_seg2 = 8,
			.sjw = 1,
			.brp = 4,
		},
		.kbps = 500
	},
	{
		.bt = {
			.bitrate = 625000,
			.sample_point = 750,
			.tq = 200,
			.prop_seg = 1,
			.phase_seg1 = 4,
			.phase_seg2 = 2,
			.sjw = 1,
			.brp = 8,
		},
		.kbps = 625
	},
	{
		.bt = {
			.bitrate = 800000,
			.sample_point = 680,
			.tq = 50,
			.prop_seg = 8,
			.phase_seg1 = 8,
			.phase_seg2 = 8,
			.sjw = 1,
			.brp = 2,
		},
		.kbps = 800
	},
	{
		.bt = {
			.bitrate = 1000000,
			.sample_point = 600,
			.tq = 50,
			.prop_seg = 3,
			.phase_seg1 = 8,
			.phase_seg2 = 8,
			.sjw = 1,
			.brp = 2,
		},
		.kbps = 1000
	}
};

static int debug;
module_param(debug, int, 0664);
MODULE_PARM_DESC(debug,
		 "Binary flag to control device debug (keep alive) prints in dmesg. 0='Debug prints disabled' "
		 __stringify(UDT1CRI_PARAM_DEBUG_USB) "='PIC_USB debugs enabled' "
		 __stringify(UDT1CRI_PARAM_DEBUG_CAN) "='PIC_CAN debugs enabled'");

static const struct usb_device_id udt1cri_usb_table[] = {
	{ USB_DEVICE(UDT1CRI_VENDOR_ID, UDT1CRI_PRODUCT_ID) },
	{ } /* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, udt1cri_usb_table);

static netdev_tx_t udt1cri_usb_xmit(struct udt1cri_priv *priv,
				 struct udt1cri_usb_msg *usb_msg,
				 struct sk_buff *skb);
static void udt1cri_usb_xmit_cmd(struct udt1cri_priv *priv,
			      struct udt1cri_usb_msg *usb_msg);
static void udt1cri_usb_xmit_read_fw_ver(struct udt1cri_priv *priv, u8 pic);
static void udt1cri_usb_xmit_termination(struct udt1cri_priv *priv, u8 termination);
static inline void udt1cri_init_ctx(struct udt1cri_priv *priv);

static ssize_t termination_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct net_device *netdev = to_net_dev(dev);
	struct udt1cri_priv *priv = netdev_priv(netdev);

	return sprintf(buf, "%hhu\n", priv->termination_state);
}

static ssize_t termination_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct net_device *netdev = to_net_dev(dev);
	struct udt1cri_priv *priv = netdev_priv(netdev);
	int tmp = -1;
	int ret = -1;

	ret = kstrtoint(buf, 10, &tmp);

	if ((ret == 0) && ((tmp == 0) || (tmp == 1))) {
		priv->termination_state = tmp;
		udt1cri_usb_xmit_termination(priv, priv->termination_state);
	}

	return count;
}

static struct device_attribute termination_attr = {
	.attr = {
		.name = "termination",
		.mode = 0666 },
	.show	= termination_show,
	.store	= termination_store
};

static void udt1cri_usb_process_can(struct udt1cri_priv *priv,
				 struct udt1cri_usb_msg_can *msg)
{
	struct can_frame *cf;
	struct sk_buff *skb;
	struct net_device_stats *stats = &priv->netdev->stats;

	skb = alloc_can_skb(priv->netdev, &cf);
	if (!skb)
		return;

	cf->can_id = __le32_to_cpu(msg->eid);
	if (msg->flags & FLAG_CAN_EID)
		cf->can_id |= CAN_EFF_FLAG;

	if (msg->flags & FLAG_CAN_RTR)
		cf->can_id |= CAN_RTR_FLAG;

	cf->can_dlc = msg->dlc & UDT1CRI_DLC_MASK;

	memcpy(cf->data, msg->data, cf->can_dlc);

	stats->rx_packets++;
	stats->rx_bytes += cf->can_dlc;
	netif_rx(skb);
}

static void udt1cri_usb_process_ka_usb(struct udt1cri_priv *priv,
				    struct udt1cri_usb_msg_ka_usb *msg)
{
	if (unlikely(priv->usb_ka_first_pass)) {
		netdev_info(priv->netdev, "PIC USB version %hhu.%hhu\n",
			    msg->soft_ver_major, msg->soft_ver_minor);

		priv->usb_ka_first_pass = false;
	}

	priv->termination_state = msg->termination_state;
}

static void udt1cri_usb_process_ka_can(struct udt1cri_priv *priv,
				    struct udt1cri_usb_msg_ka_can *msg)
{
	if (unlikely(priv->can_ka_first_pass)) {
		netdev_info(priv->netdev,
			    "PIC CAN version %hhu.%hhu\n",
			    msg->soft_ver_major, msg->soft_ver_minor);

		priv->can_ka_first_pass = false;
	}

	priv->bec.txerr = msg->tx_err_cnt;
	priv->bec.rxerr = msg->rx_err_cnt;
}

static void udt1cri_usb_process_rx(struct udt1cri_priv *priv,
				struct udt1cri_usb_msg *msg)
{
	switch (msg->cmd_id) {
	case UDT1CRI_CMD_I_AM_ALIVE_FROM_CAN:
		udt1cri_usb_process_ka_can(priv,
					(struct udt1cri_usb_msg_ka_can *)msg);
		break;

	case UDT1CRI_CMD_I_AM_ALIVE_FROM_USB:
		udt1cri_usb_process_ka_usb(priv,
					(struct udt1cri_usb_msg_ka_usb *)msg);
		break;

	case UDT1CRI_CMD_RECEIVE_MESSAGE:
		udt1cri_usb_process_can(priv, (struct udt1cri_usb_msg_can *)msg);
		break;

	case UDT1CRI_CMD_NOTHING_TO_SEND:
		/* Side effect of communication between PIC_USB and PIC_CAN.
		 * PIC_CAN is telling us that it has nothing to send
		 */
		break;

	case UDT1CRI_CMD_TRANSMIT_MESSAGE_RSP:
		/* Transmission response from the device containing timestamp */
		break;

	default:
		netdev_warn(priv->netdev, "Unsupported msg (0x%hhX)",
			    msg->cmd_id);
		break;
	}
}

/* Callback for reading data from device
 *
 * Check urb status, call read function and resubmit urb read operation.
 */
static void udt1cri_usb_read_bulk_callback(struct urb *urb)
{
	struct udt1cri_priv *priv = urb->context;
	struct net_device *netdev;
	int retval;
	int pos = 0;

	netdev = priv->netdev;

	if (!netif_device_present(netdev))
		return;

	switch (urb->status) {
	case 0: /* success */
		break;

	case -ENOENT:
	case -EPIPE:
	case -EPROTO:
	case -ESHUTDOWN:
		return;

	default:
		netdev_info(netdev, "Rx URB aborted (%d)\n", urb->status);
		goto resubmit_urb;
	}

	while (pos < urb->actual_length) {
		struct udt1cri_usb_msg *msg;

		if (pos + sizeof(struct udt1cri_usb_msg) > urb->actual_length) {
			netdev_err(priv->netdev, "format error\n");
			break;
		}

		msg = (struct udt1cri_usb_msg *)(urb->transfer_buffer + pos);
		udt1cri_usb_process_rx(priv, msg);

		pos += sizeof(struct udt1cri_usb_msg);
	}

resubmit_urb:

	usb_fill_bulk_urb(urb, priv->udev,
			  usb_rcvbulkpipe(priv->udev, UDT1CRI_USB_EP_OUT),
			  urb->transfer_buffer, UDT1CRI_USB_RX_BUFF_SIZE,
			  udt1cri_usb_read_bulk_callback, priv);

	retval = usb_submit_urb(urb, GFP_ATOMIC);

	if (retval == -ENODEV)
		netif_device_detach(netdev);
	else if (retval)
		netdev_err(netdev, "failed resubmitting read bulk urb: %d\n",
			   retval);
}

/* Start USB device */
static int udt1cri_usb_start(struct udt1cri_priv *priv)
{
	struct net_device *netdev = priv->netdev;
	int err, i;

	udt1cri_init_ctx(priv);

	for (i = 0; i < UDT1CRI_MAX_RX_URBS; i++) {
		struct urb *urb = NULL;
		u8 *buf;

		/* create a URB, and a buffer for it */
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(priv->udev, UDT1CRI_USB_RX_BUFF_SIZE,
					 GFP_KERNEL, &urb->transfer_dma);
		if (!buf) {
			netdev_err(netdev, "No memory left for USB buffer\n");
			usb_free_urb(urb);
			err = -ENOMEM;
			break;
		}

		usb_fill_bulk_urb(urb, priv->udev,
				  usb_rcvbulkpipe(priv->udev, UDT1CRI_USB_EP_IN),
				  buf, UDT1CRI_USB_RX_BUFF_SIZE,
				  udt1cri_usb_read_bulk_callback, priv);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		usb_anchor_urb(urb, &priv->rx_submitted);

		err = usb_submit_urb(urb, GFP_KERNEL);
		if (err) {
			usb_unanchor_urb(urb);
			usb_free_coherent(priv->udev, UDT1CRI_USB_RX_BUFF_SIZE,
					  buf, urb->transfer_dma);
			usb_free_urb(urb);
			break;
		}

		/* Drop reference, USB core will take care of freeing it */
		usb_free_urb(urb);
	}

	/* Did we submit any URBs */
	if (i == 0) {
		netdev_warn(netdev, "couldn't setup read URBs\n");
		return err;
	}

	/* Warn if we've couldn't transmit all the URBs */
	if (i < UDT1CRI_MAX_RX_URBS)
		netdev_warn(netdev, "rx performance may be slow\n");

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	udt1cri_usb_xmit_read_fw_ver(priv, UDT1CRI_VER_REQ_USB);
	udt1cri_usb_xmit_read_fw_ver(priv, UDT1CRI_VER_REQ_CAN);

	return err;
}

static inline void udt1cri_init_ctx(struct udt1cri_priv *priv)
{
	int i = 0;

	for (i = 0; i < UDT1CRI_MAX_TX_URBS; i++)
		priv->tx_context[i].ndx = UDT1CRI_CTX_FREE;
}

static inline struct udt1cri_usb_ctx *udt1cri_usb_get_free_ctx(struct udt1cri_priv *priv)
{
	int i = 0;
	struct udt1cri_usb_ctx *ctx = 0;

	for (i = 0; i < UDT1CRI_MAX_TX_URBS; i++) {
		if (priv->tx_context[i].ndx == UDT1CRI_CTX_FREE) {
			ctx = &priv->tx_context[i];
			ctx->ndx = i;
			ctx->priv = priv;
			break;
		}
	}

	return ctx;
}

static inline void udt1cri_usb_free_ctx(struct udt1cri_usb_ctx *ctx)
{
	ctx->ndx = UDT1CRI_CTX_FREE;
	ctx->priv = 0;
	ctx->dlc = 0;
	ctx->can = false;
}

static void udt1cri_usb_write_bulk_callback(struct urb *urb)
{
	struct udt1cri_usb_ctx *ctx = urb->context;
	struct net_device *netdev;

	WARN_ON(!ctx);

	netdev = ctx->priv->netdev;

	if (ctx->can) {
		if (!netif_device_present(netdev))
			return;

		netdev->stats.tx_packets++;
		netdev->stats.tx_bytes += ctx->dlc;

		can_get_echo_skb(netdev, ctx->ndx);

		netif_wake_queue(netdev);
	}

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);

	if (urb->status)
		netdev_info(netdev, "Tx URB aborted (%d)\n",
			    urb->status);

	/* Release context */
	udt1cri_usb_free_ctx(ctx);
}

/* Send data to device */
static netdev_tx_t udt1cri_usb_start_xmit(struct sk_buff *skb,
				       struct net_device *netdev)
{
	struct udt1cri_priv *priv = netdev_priv(netdev);
	struct can_frame *cf = (struct can_frame *)skb->data;
	struct udt1cri_usb_msg_can usb_msg;

	usb_msg.cmd_id = UDT1CRI_CMD_TRANSMIT_MESSAGE_EV;
	memcpy(usb_msg.data, cf->data, sizeof(usb_msg.data));

	usb_msg.eid = __cpu_to_le32(cf->can_id);
	if (UDT1CRI_TX_IS_EXID(cf)) {
		
	} else {
		
	}

	usb_msg.dlc = cf->can_dlc;

	if (UDT1CRI_TX_IS_RTR(cf))
		usb_msg.dlc |= UDT1CRI_DLC_RTR_MASK;

	return udt1cri_usb_xmit(priv, (struct udt1cri_usb_msg *)&usb_msg, skb);
}

/* Send data to device */
static void udt1cri_usb_xmit_cmd(struct udt1cri_priv *priv,
			      struct udt1cri_usb_msg *usb_msg)
{
	udt1cri_usb_xmit(priv, usb_msg, 0);
}

/* Send data to device */
static netdev_tx_t udt1cri_usb_xmit(struct udt1cri_priv *priv,
				 struct udt1cri_usb_msg *usb_msg,
				 struct sk_buff *skb)
{
	struct net_device_stats *stats = &priv->netdev->stats;
	struct udt1cri_usb_ctx *ctx = 0;
	struct urb *urb;
	u8 *buf;
	int err;

	ctx = udt1cri_usb_get_free_ctx(priv);
	if (!ctx) {
		/* Slow down tx path */
		netif_stop_queue(priv->netdev);

		return NETDEV_TX_BUSY;
	}

	if (skb) {
		ctx->dlc = ((struct udt1cri_usb_msg_can *)usb_msg)->dlc
				& UDT1CRI_DLC_MASK;
		can_put_echo_skb(skb, priv->netdev, ctx->ndx);
		ctx->can = true;
	} else {
		ctx->can = false;
	}

	/* create a URB, and a buffer for it, and copy the data to the URB */
	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		netdev_err(priv->netdev, "No memory left for URBs\n");
		goto nomem;
	}

	buf = usb_alloc_coherent(priv->udev, UDT1CRI_USB_TX_BUFF_SIZE, GFP_ATOMIC,
				 &urb->transfer_dma);
	if (!buf) {
		netdev_err(priv->netdev, "No memory left for USB buffer\n");
		goto nomembuf;
	}

	memcpy(buf, usb_msg, UDT1CRI_USB_TX_BUFF_SIZE);

	usb_fill_bulk_urb(urb, priv->udev,
			  usb_sndbulkpipe(priv->udev, UDT1CRI_USB_EP_OUT), buf,
			  UDT1CRI_USB_TX_BUFF_SIZE, udt1cri_usb_write_bulk_callback,
			  ctx);

	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &priv->tx_submitted);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (unlikely(err))
		goto failed;

	/* Release our reference to this URB, the USB core will eventually free
	 * it entirely.
	 */
	usb_free_urb(urb);

	return NETDEV_TX_OK;

failed:
	usb_unanchor_urb(urb);
	usb_free_coherent(priv->udev, UDT1CRI_USB_TX_BUFF_SIZE, buf,
			  urb->transfer_dma);

	if (err == -ENODEV)
		netif_device_detach(priv->netdev);
	else
		netdev_warn(priv->netdev, "failed tx_urb %d\n", err);

nomembuf:
	usb_free_urb(urb);

nomem:
	can_free_echo_skb(priv->netdev, ctx->ndx);
	dev_kfree_skb(skb);
	stats->tx_dropped++;

	return NETDEV_TX_OK;
}

static void udt1cri_usb_xmit_change_bitrate(struct udt1cri_priv *priv, u16 bitrate)
{
	struct udt1cri_usb_msg_change_bitrate usb_msg;

	usb_msg.cmd_id =  UDT1CRI_CMD_CHANGE_BIT_RATE;
	usb_msg.bitrate_hi = (0xff00 & bitrate) >> 8;
	usb_msg.bitrate_lo = (0xff & bitrate);

	udt1cri_usb_xmit_cmd(priv, (struct udt1cri_usb_msg *)&usb_msg);
}

static void udt1cri_usb_xmit_read_fw_ver(struct udt1cri_priv *priv, u8 pic)
{
	struct udt1cri_usb_msg_fw_ver usb_msg;

	usb_msg.cmd_id = UDT1CRI_CMD_READ_FW_VERSION;
	usb_msg.pic = pic;

	udt1cri_usb_xmit_cmd(priv, (struct udt1cri_usb_msg *)&usb_msg);
}

static void udt1cri_usb_xmit_termination(struct udt1cri_priv *priv, u8 termination)
{
	struct udt1cri_usb_msg_terminaton usb_msg;

	usb_msg.cmd_id = UDT1CRI_CMD_SETUP_TERMINATION_RESISTANCE;
	usb_msg.termination = termination;

	udt1cri_usb_xmit_cmd(priv, (struct udt1cri_usb_msg *)&usb_msg);
}

/* Open USB device */
static int udt1cri_usb_open(struct net_device *netdev)
{
	int err;

	/* common open */
	err = open_candev(netdev);
	if (err)
		return err;

	can_led_event(netdev, CAN_LED_EVENT_OPEN);

	netif_start_queue(netdev);

	return 0;
}

static void udt1cri_urb_unlink(struct udt1cri_priv *priv)
{
	usb_kill_anchored_urbs(&priv->rx_submitted);
	usb_kill_anchored_urbs(&priv->tx_submitted);
}

/* Close USB device */
static int udt1cri_usb_close(struct net_device *netdev)
{
	struct udt1cri_priv *priv = netdev_priv(netdev);

	priv->can.state = CAN_STATE_STOPPED;

	netif_stop_queue(netdev);

	/* Stop polling */
	udt1cri_urb_unlink(priv);

	close_candev(netdev);

	can_led_event(netdev, CAN_LED_EVENT_STOP);

	return 0;
}

/* Set network device mode
 *
 * Maybe we should leave this function empty, because the device
 * set mode variable with open command.
 */
static int udt1cri_net_set_mode(struct net_device *netdev, enum can_mode mode)
{
	return 0;
}

static int udt1cri_net_get_berr_counter(const struct net_device *netdev,
				     struct can_berr_counter *bec)
{
	struct udt1cri_priv *priv = netdev_priv(netdev);

	bec->txerr = priv->bec.txerr;
	bec->rxerr = priv->bec.rxerr;

	return 0;
}

static const struct net_device_ops udt1cri_netdev_ops = {
	.ndo_open = udt1cri_usb_open,
	.ndo_stop = udt1cri_usb_close,
	.ndo_start_xmit = udt1cri_usb_start_xmit
};

/* Microchip CANBUS has hardcoded bittiming values by default.
 * This function sends request via USB to change the speed and align bittiming
 * values for presentation purposes only
 */
static int udt1cri_net_set_bittiming(struct net_device *netdev)
{
	u8 i;
	struct udt1cri_priv *priv = netdev_priv(netdev);
	struct can_bittiming *bt = &priv->can.bittiming;
	const struct bitrate_settings *settings = 0;
	const u8 setting_cnt = sizeof(br_settings) /
			       sizeof(struct bitrate_settings);

	for (i = 0; i < setting_cnt; ++i)
		if (br_settings[i].bt.bitrate == bt->bitrate)
			settings = &br_settings[i];

	if (settings) {
		memcpy(bt, &settings->bt, sizeof(struct can_bittiming));

		/* recalculate bitrate as it may be different than default */
		bt->bitrate = 1000000000 / ((bt->sjw + bt->prop_seg +
					    bt->phase_seg1 + bt->phase_seg2) *
					    bt->tq);

		udt1cri_usb_xmit_change_bitrate(priv, settings->kbps);
	} else {
		netdev_err(netdev, "Unsupported bittrate (%u). Use one of: 20000, 33333, 50000, 80000, 83333, 100000, 125000, 150000, 175000, 200000, 225000, 250000, 275000, 300000, 500000, 625000, 800000, 1000000\n",
			   bt->bitrate);

		return -EINVAL;
	}

	return 0;
}

static int udt1cri_usb_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	struct net_device *netdev;
	struct udt1cri_priv *priv;
	int err = -ENOMEM;
	struct usb_device *usbdev = interface_to_usbdev(intf);

	dev_info(&intf->dev, "UniSwarm UDT1CRI CAN debugger connected\n");

	netdev = alloc_candev(sizeof(struct udt1cri_priv), UDT1CRI_MAX_TX_URBS);
	if (!netdev) {
		dev_err(&intf->dev, "Couldn't alloc candev\n");
		return -ENOMEM;
	}

	priv = netdev_priv(netdev);

	priv->udev = usbdev;
	priv->netdev = netdev;
	priv->usb_ka_first_pass = true;
	priv->can_ka_first_pass = true;

	init_usb_anchor(&priv->rx_submitted);
	init_usb_anchor(&priv->tx_submitted);

	usb_set_intfdata(intf, priv);

	err = udt1cri_usb_start(priv);
	if (err) {
		if (err == -ENODEV)
			netif_device_detach(priv->netdev);

		netdev_warn(netdev, "couldn't start device: %d\n", err);

		goto cleanup_candev;
	}

	/* Init CAN device */
	priv->can.state = CAN_STATE_STOPPED;
	priv->can.clock.freq = UDT1CRI_CAN_CLOCK;
	priv->can.bittiming_const = &udt1cri_bittiming_const;
	priv->can.do_set_mode = udt1cri_net_set_mode;
	priv->can.do_get_berr_counter = udt1cri_net_get_berr_counter;
	priv->can.do_set_bittiming = udt1cri_net_set_bittiming;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
			CAN_CTRLMODE_LISTENONLY |
			CAN_CTRLMODE_ONE_SHOT;

	netdev->netdev_ops = &udt1cri_netdev_ops;

	netdev->flags |= IFF_ECHO; /* we support local echo */

	SET_NETDEV_DEV(netdev, &intf->dev);

	err = register_candev(netdev);
	if (err) {
		netdev_err(netdev,
			   "couldn't register CAN device: %d\n", err);
		goto cleanup_candev;
	}

	err = device_create_file(&netdev->dev, &termination_attr);
	if (err)
		goto cleanup_unregister_candev;

	return err;

cleanup_unregister_candev:
	unregister_candev(netdev);

cleanup_candev:
	free_candev(netdev);

	return err;
}

/* Called by the usb core when driver is unloaded or device is removed */
static void udt1cri_usb_disconnect(struct usb_interface *intf)
{
	struct udt1cri_priv *priv = usb_get_intfdata(intf);

	device_remove_file(&priv->netdev->dev, &termination_attr);

	usb_set_intfdata(intf, NULL);

	if (priv) {
		netdev_info(priv->netdev, "device disconnected\n");

		unregister_candev(priv->netdev);
		free_candev(priv->netdev);

		udt1cri_urb_unlink(priv);
	}
}

static struct usb_driver udt1cri_usb_driver = {
	.name =	 UDT1CRI_MODULE_NAME,
	.probe = udt1cri_usb_probe,
	.disconnect = udt1cri_usb_disconnect,
	.id_table = udt1cri_usb_table,
};

module_usb_driver(udt1cri_usb_driver);

MODULE_AUTHOR("Remigiusz Kołłątaj <remigiusz.kollataj@mobica.com>");
MODULE_DESCRIPTION("SocketCAN driver for UniSwarm UDT1CRI CAN debugger");
MODULE_LICENSE("GPL v2");
