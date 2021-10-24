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
#include <linux/version.h>

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

/* UDT1CRI command id */
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

#define UDT1CRI_CAN_STATE_WRN_TH 95
#define UDT1CRI_CAN_STATE_ERR_PSV_TH 127

#define UDT1CRI_TERMINATION_DISABLED CAN_TERMINATION_DISABLED
#define UDT1CRI_TERMINATION_ENABLED 120

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
	bool usb_ka_first_pass;
	bool can_ka_first_pass;
	bool can_speed_check;
	atomic_t free_ctx_cnt;
};

/* CAN frame */
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
	__be16 can_bitrate;
	__le16 rx_lost;
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
	__be16 bitrate;
	u8 unused[17];
};

struct __packed udt1cri_usb_msg_termination {
	u8 cmd_id;
	u8 termination;
	u8 unused[18];
};

struct __packed udt1cri_usb_msg_fw_ver {
	u8 cmd_id;
	u8 pic;
	u8 unused[18];
};

static const struct usb_device_id udt1cri_usb_table[] = {
	{ USB_DEVICE(UDT1CRI_VENDOR_ID, UDT1CRI_PRODUCT_ID) },
	{} /* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, udt1cri_usb_table);

static const u16 udt1cri_termination[] = { UDT1CRI_TERMINATION_DISABLED,
					UDT1CRI_TERMINATION_ENABLED };

static const u32 udt1cri_bitrate[] = { 20000,  33333,  50000,  80000,  83333,
				    100000, 125000, 150000, 175000, 200000,
				    225000, 250000, 275000, 300000, 500000,
				    625000, 800000, 1000000 };

static inline void udt1cri_init_ctx(struct udt1cri_priv *priv)
{
	int i = 0;

	for (i = 0; i < UDT1CRI_MAX_TX_URBS; i++) {
		priv->tx_context[i].ndx = UDT1CRI_CTX_FREE;
		priv->tx_context[i].priv = priv;
	}

	atomic_set(&priv->free_ctx_cnt, ARRAY_SIZE(priv->tx_context));
}

static inline struct udt1cri_usb_ctx *udt1cri_usb_get_free_ctx(struct udt1cri_priv *priv,
							 struct can_frame *cf)
{
	int i = 0;
	struct udt1cri_usb_ctx *ctx = NULL;

	for (i = 0; i < UDT1CRI_MAX_TX_URBS; i++) {
		if (priv->tx_context[i].ndx == UDT1CRI_CTX_FREE) {
			ctx = &priv->tx_context[i];
			ctx->ndx = i;

			if (cf) {
				ctx->can = true;
				ctx->dlc = cf->can_dlc;
			} else {
				ctx->can = false;
				ctx->dlc = 0;
			}

			atomic_dec(&priv->free_ctx_cnt);
			break;
		}
	}

	if (!atomic_read(&priv->free_ctx_cnt))
		/* That was the last free ctx. Slow down tx path */
		netif_stop_queue(priv->netdev);

	return ctx;
}

/* udt1cri_usb_free_ctx and udt1cri_usb_get_free_ctx are executed by different
 * threads. The order of execution in below function is important.
 */
static inline void udt1cri_usb_free_ctx(struct udt1cri_usb_ctx *ctx)
{
	/* Increase number of free ctxs before freeing ctx */
	atomic_inc(&ctx->priv->free_ctx_cnt);

	ctx->ndx = UDT1CRI_CTX_FREE;

	/* Wake up the queue once ctx is marked free */
	netif_wake_queue(ctx->priv->netdev);
}

static void udt1cri_usb_write_bulk_callback(struct urb *urb)
{
	struct udt1cri_usb_ctx *ctx = urb->context;
	struct net_device *netdev;

	WARN_ON(!ctx);

	netdev = ctx->priv->netdev;

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);

	if (ctx->can) {
		if (!netif_device_present(netdev))
			return;

		netdev->stats.tx_packets++;
		netdev->stats.tx_bytes += ctx->dlc;

		can_led_event(netdev, CAN_LED_EVENT_TX);
#if LINUX_VERSION_CODE <= KERNEL_VERSION(5,12,0)
		can_get_echo_skb(netdev, ctx->ndx);
#else
		can_get_echo_skb(netdev, ctx->ndx, NULL);
#endif
	}

	if (urb->status)
		netdev_info(netdev, "Tx URB aborted (%d)\n", urb->status);

	/* Release the context */
	udt1cri_usb_free_ctx(ctx);
}

/* Send data to device */
static netdev_tx_t udt1cri_usb_xmit(struct udt1cri_priv *priv,
				 struct udt1cri_usb_msg *usb_msg,
				 struct udt1cri_usb_ctx *ctx)
{
	struct urb *urb;
	u8 *buf;
	int err;

	/* create a URB, and a buffer for it, and copy the data to the URB */
	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb)
		return -ENOMEM;

	buf = usb_alloc_coherent(priv->udev, UDT1CRI_USB_TX_BUFF_SIZE, GFP_ATOMIC,
				 &urb->transfer_dma);
	if (!buf) {
		err = -ENOMEM;
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

	return 0;

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

	return err;
}

/* Send data to device */
static netdev_tx_t udt1cri_usb_start_xmit(struct sk_buff *skb,
				       struct net_device *netdev)
{
	struct udt1cri_priv *priv = netdev_priv(netdev);
	struct can_frame *cf = (struct can_frame *)skb->data;
	struct udt1cri_usb_ctx *ctx = NULL;
	struct net_device_stats *stats = &priv->netdev->stats;
	int err;
	struct udt1cri_usb_msg_can usb_msg = {
		.cmd_id = UDT1CRI_CMD_TRANSMIT_MESSAGE_EV
	};

	if (can_dropped_invalid_skb(netdev, skb))
		return NETDEV_TX_OK;

	ctx = udt1cri_usb_get_free_ctx(priv, cf);
	if (!ctx)
		return NETDEV_TX_BUSY;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(5,12,0)
	can_put_echo_skb(skb, priv->netdev, ctx->ndx);
#else
	can_put_echo_skb(skb, priv->netdev, ctx->ndx, 0);
#endif

	usb_msg.cmd_id = UDT1CRI_CMD_TRANSMIT_MESSAGE_EV;

	usb_msg.flags = 0;
	usb_msg.eid = (cf->can_id);
	if (cf->can_id & CAN_EFF_FLAG)
		usb_msg.flags |= FLAG_CAN_EID;

	usb_msg.dlc = cf->can_dlc;

	memcpy(usb_msg.data, cf->data, usb_msg.dlc);

	if (cf->can_id & CAN_RTR_FLAG)
		usb_msg.flags |= FLAG_CAN_RTR;

	err = udt1cri_usb_xmit(priv, (struct udt1cri_usb_msg *)&usb_msg, ctx);
	if (err)
		goto xmit_failed;

	return NETDEV_TX_OK;

xmit_failed:
#if LINUX_VERSION_CODE <= KERNEL_VERSION(5,12,0)
	can_free_echo_skb(priv->netdev, ctx->ndx);
#else
	can_free_echo_skb(priv->netdev, ctx->ndx, NULL);
#endif
	udt1cri_usb_free_ctx(ctx);
	dev_kfree_skb(skb);
	stats->tx_dropped++;

	return NETDEV_TX_OK;
}

/* Send cmd to device */
static void udt1cri_usb_xmit_cmd(struct udt1cri_priv *priv,
			      struct udt1cri_usb_msg *usb_msg)
{
	struct udt1cri_usb_ctx *ctx = NULL;
	int err;

	ctx = udt1cri_usb_get_free_ctx(priv, NULL);
	if (!ctx) {
		netdev_err(priv->netdev,
			   "Lack of free ctx. Sending (%d) cmd aborted",
			   usb_msg->cmd_id);

		return;
	}

	err = udt1cri_usb_xmit(priv, usb_msg, ctx);
	if (err)
		netdev_err(priv->netdev, "Failed to send cmd (%d)",
			   usb_msg->cmd_id);
}

static void udt1cri_usb_xmit_change_bitrate(struct udt1cri_priv *priv, u16 bitrate)
{
	struct udt1cri_usb_msg_change_bitrate usb_msg = {
		.cmd_id = UDT1CRI_CMD_CHANGE_BIT_RATE
	};

	put_unaligned_be16(bitrate, &usb_msg.bitrate);

	udt1cri_usb_xmit_cmd(priv, (struct udt1cri_usb_msg *)&usb_msg);
}

static void udt1cri_usb_xmit_read_fw_ver(struct udt1cri_priv *priv, u8 pic)
{
	struct udt1cri_usb_msg_fw_ver usb_msg = {
		.cmd_id = UDT1CRI_CMD_READ_FW_VERSION,
		.pic = pic
	};

	udt1cri_usb_xmit_cmd(priv, (struct udt1cri_usb_msg *)&usb_msg);
}

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

	can_led_event(priv->netdev, CAN_LED_EVENT_RX);
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

	if (msg->termination_state)
		priv->can.termination = UDT1CRI_TERMINATION_ENABLED;
	else
		priv->can.termination = UDT1CRI_TERMINATION_DISABLED;
}

static u32 convert_can2host_bitrate(struct udt1cri_usb_msg_ka_can *msg)
{
	const u32 bitrate = get_unaligned_be16(&msg->can_bitrate);

	if ((bitrate == 33) || (bitrate == 83))
		return bitrate * 1000 + 333;
	else
		return bitrate * 1000;
}

static void udt1cri_usb_process_ka_can(struct udt1cri_priv *priv,
				    struct udt1cri_usb_msg_ka_can *msg)
{
	if (unlikely(priv->can_ka_first_pass)) {
		netdev_info(priv->netdev, "PIC CAN version %hhu.%hhu\n",
			    msg->soft_ver_major, msg->soft_ver_minor);

		priv->can_ka_first_pass = false;
	}

	if (unlikely(priv->can_speed_check)) {
		const u32 bitrate = convert_can2host_bitrate(msg);

		priv->can_speed_check = false;

		if (bitrate != priv->can.bittiming.bitrate)
			netdev_err(
			    priv->netdev,
			    "Wrong bitrate reported by the device (%u). Expected %u",
			    bitrate, priv->can.bittiming.bitrate);
	}

	priv->bec.txerr = msg->tx_err_cnt;
	priv->bec.rxerr = msg->rx_err_cnt;

	if (msg->tx_bus_off)
		priv->can.state = CAN_STATE_BUS_OFF;

	else if ((priv->bec.txerr > UDT1CRI_CAN_STATE_ERR_PSV_TH) ||
		 (priv->bec.rxerr > UDT1CRI_CAN_STATE_ERR_PSV_TH))
		priv->can.state = CAN_STATE_ERROR_PASSIVE;

	else if ((priv->bec.txerr > UDT1CRI_CAN_STATE_WRN_TH) ||
		 (priv->bec.rxerr > UDT1CRI_CAN_STATE_WRN_TH))
		priv->can.state = CAN_STATE_ERROR_WARNING;
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

	udt1cri_usb_xmit_read_fw_ver(priv, UDT1CRI_VER_REQ_USB);
	udt1cri_usb_xmit_read_fw_ver(priv, UDT1CRI_VER_REQ_CAN);

	return err;
}

/* Open USB device */
static int udt1cri_usb_open(struct net_device *netdev)
{
	struct udt1cri_priv *priv = netdev_priv(netdev);
	int err;

	/* common open */
	err = open_candev(netdev);
	if (err)
		return err;

	priv->can_speed_check = true;
	priv->can.state = CAN_STATE_ERROR_ACTIVE;

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
	.ndo_start_xmit = udt1cri_usb_start_xmit,
};

/* UDT1CRI CANBUS has hardcoded bittiming values by default.
 * This function sends request via USB to change the speed and align bittiming
 * values for presentation purposes only
 */
static int udt1cri_net_set_bittiming(struct net_device *netdev)
{
	struct udt1cri_priv *priv = netdev_priv(netdev);
	const u16 bitrate_kbps = priv->can.bittiming.bitrate / 1000;

	udt1cri_usb_xmit_change_bitrate(priv, bitrate_kbps);

	return 0;
}

static int udt1cri_set_termination(struct net_device *netdev, u16 term)
{
	struct udt1cri_priv *priv = netdev_priv(netdev);
	struct udt1cri_usb_msg_termination usb_msg = {
		.cmd_id = UDT1CRI_CMD_SETUP_TERMINATION_RESISTANCE
	};

	if (term == UDT1CRI_TERMINATION_ENABLED)
		usb_msg.termination = 1;
	else
		usb_msg.termination = 0;

	udt1cri_usb_xmit_cmd(priv, (struct udt1cri_usb_msg *)&usb_msg);

	return 0;
}

static int udt1cri_usb_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	struct net_device *netdev;
	struct udt1cri_priv *priv;
	int err = -ENOMEM;
	struct usb_device *usbdev = interface_to_usbdev(intf);

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
	priv->can_speed_check = false;

	init_usb_anchor(&priv->rx_submitted);
	init_usb_anchor(&priv->tx_submitted);

	usb_set_intfdata(intf, priv);

	/* Init CAN device */
	priv->can.state = CAN_STATE_STOPPED;
	priv->can.termination_const = udt1cri_termination;
	priv->can.termination_const_cnt = ARRAY_SIZE(udt1cri_termination);
	priv->can.bitrate_const = udt1cri_bitrate;
	priv->can.bitrate_const_cnt = ARRAY_SIZE(udt1cri_bitrate);

	priv->can.do_set_termination = udt1cri_set_termination;
	priv->can.do_set_mode = udt1cri_net_set_mode;
	priv->can.do_get_berr_counter = udt1cri_net_get_berr_counter;
	priv->can.do_set_bittiming = udt1cri_net_set_bittiming;

	netdev->netdev_ops = &udt1cri_netdev_ops;

	netdev->flags |= IFF_ECHO; /* we support local echo */

	SET_NETDEV_DEV(netdev, &intf->dev);

	err = register_candev(netdev);
	if (err) {
		netdev_err(netdev, "couldn't register CAN device: %d\n", err);

		goto cleanup_free_candev;
	}

	devm_can_led_init(netdev);

	/* Start USB dev only if we have successfully registered CAN device */
	err = udt1cri_usb_start(priv);
	if (err) {
		if (err == -ENODEV)
			netif_device_detach(priv->netdev);

		netdev_warn(netdev, "couldn't start device: %d\n", err);

		goto cleanup_unregister_candev;
	}

	dev_info(&intf->dev, "UniSwarm UDT1CRI CAN debugger connected\n");

	return 0;

cleanup_unregister_candev:
	unregister_candev(priv->netdev);

cleanup_free_candev:
	free_candev(netdev);

	return err;
}

/* Called by the usb core when driver is unloaded or device is removed */
static void udt1cri_usb_disconnect(struct usb_interface *intf)
{
	struct udt1cri_priv *priv = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);

	netdev_info(priv->netdev, "device disconnected\n");

	unregister_candev(priv->netdev);
	free_candev(priv->netdev);

	udt1cri_urb_unlink(priv);
}

static struct usb_driver udt1cri_usb_driver = {
	.name = UDT1CRI_MODULE_NAME,
	.probe = udt1cri_usb_probe,
	.disconnect = udt1cri_usb_disconnect,
	.id_table = udt1cri_usb_table,
};

module_usb_driver(udt1cri_usb_driver);

MODULE_AUTHOR("Remigiusz Kołłątaj <remigiusz.kollataj@mobica.com>");
MODULE_DESCRIPTION("SocketCAN driver for UniSwarm UDT1CRI CAN debugger");
MODULE_LICENSE("GPL v2");
