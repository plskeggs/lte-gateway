/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <strings.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/gatt.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/hci_err.h>
#include <dk_buttons_and_leds.h>
#include <settings/settings.h>

#include "net/nrf_cloud.h"
#include "ble.h"

#include "ble_codec.h"
#include "gateway.h"
#include "ctype.h"
#include "nrf_cloud_transport.h"
#include "ble_conn_mgr.h"
#include "ui.h"

#define SEND_NOTIFY_STACK_SIZE 2048
#define SEND_NOTIFY_PRIORITY 9
#define SUBSCRIPTION_LIMIT 16
#define NOTIFICATION_QUEUE_LIMIT 10
#define MAX_BUF_SIZE 11000
#define STR(x) #x
#define BT_UUID_GATT_CCC_VAL_STR STR(BT_UUID_GATT_CCC_VAL)

#include <logging/log.h>
LOG_MODULE_REGISTER(ble, CONFIG_NRF_CLOUD_GATEWAY_LOG_LEVEL);

static char buffer[MAX_BUF_SIZE];
static struct gw_msg output = {
	.data.ptr = buffer,
	.data.len = 0,
	.data_max_len = MAX_BUF_SIZE
};

static bool discover_in_progress;
static bool scan_waiting;
static bool print_scan_results;

static int num_devices_found;
static int num_names_found;

struct k_timer rec_timer;
struct k_timer scan_timer;
struct k_timer auto_conn_start_timer;

struct k_work scan_off_work;
struct k_work ble_device_encode_work;
struct k_work start_auto_conn_work;

static atomic_t queued_notifications;

struct ble_scanned_dev ble_scanned_devices[MAX_SCAN_RESULTS];

/* Must be statically allocated */
/* TODO: The array needs to remain the entire time the sub exists.
 * Should probably be stored with the conn manager.
 */
static struct bt_gatt_subscribe_params sub_param[BT_MAX_SUBSCRIBES];
/* Array of connections corresponding to subscriptions above */
static struct bt_conn *sub_conn[BT_MAX_SUBSCRIBES];
static uint16_t sub_value[BT_MAX_SUBSCRIBES];
static uint8_t curr_subs;
static int next_sub_index;

static notification_cb_t notify_callback;

struct rec_data_t {
	void *fifo_reserved;
	struct bt_gatt_subscribe_params sub_params;
	struct bt_gatt_read_params read_params;
	char addr_trunc[BT_ADDR_STR_LEN];
	uint8_t data[256];
	bool read;
	uint16_t length;
};

K_FIFO_DEFINE(rec_fifo);

atomic_val_t bt_conn_getref(struct bt_conn *conn);
static void conn_cnt_foreach(struct bt_conn *conn, void *data)
{
	size_t *cur_cnt = data;
	struct bt_conn_info info;
	char addr[BT_ADDR_LE_STR_LEN] = "n/a";
	int err;

	err = bt_conn_get_info(conn, &info);
	if (!err) {
		bt_addr_le_to_str(info.le.dst, addr, sizeof(addr));
		LOG_INF("  %zd. %u, %u, %u, %u, %s", *cur_cnt, info.id, info.role, info.type, 
			bt_conn_getref(conn), log_strdup(addr));
	} else {
		LOG_ERR("  %zd. Error %d getting conn info", *cur_cnt, err);
	}

	(*cur_cnt)++;
}

void print_conns(void)
{
	size_t conn_count = 0;

	LOG_INF("List of raw BT conns:\r\n  num, id, role, type, ref, addr");
	bt_conn_foreach(BT_CONN_TYPE_LE, conn_cnt_foreach, &conn_count);
	LOG_INF("End of list.");
}

/* Convert ble address string to uppcase */
void bt_to_upper(char *addr, uint8_t addr_len)
{
	for (int i = 0; i < addr_len; i++) {
		addr[i] = toupper(addr[i]);
	}
}

/* Get uuid string from bt_uuid object */
void bt_uuid_get_str(const struct bt_uuid *uuid, char *str, size_t len)
{
	uint32_t tmp1, tmp5;
	uint16_t tmp0, tmp2, tmp3, tmp4;

	switch (uuid->type) {
	case BT_UUID_TYPE_16:
		snprintk(str, len, "%04x", BT_UUID_16(uuid)->val);
		break;
	case BT_UUID_TYPE_32:
		snprintk(str, len, "%08x", BT_UUID_32(uuid)->val);
		break;
	case BT_UUID_TYPE_128:
		memcpy(&tmp0, &BT_UUID_128(uuid)->val[0], sizeof(tmp0));
		memcpy(&tmp1, &BT_UUID_128(uuid)->val[2], sizeof(tmp1));
		memcpy(&tmp2, &BT_UUID_128(uuid)->val[6], sizeof(tmp2));
		memcpy(&tmp3, &BT_UUID_128(uuid)->val[8], sizeof(tmp3));
		memcpy(&tmp4, &BT_UUID_128(uuid)->val[10], sizeof(tmp4));
		memcpy(&tmp5, &BT_UUID_128(uuid)->val[12], sizeof(tmp5));

		snprintk(str, len, "%08x%04x%04x%04x%08x%04x",
			tmp5, tmp4, tmp3, tmp2, tmp1, tmp0);
		break;
	default:
		(void)memset(str, 0, len);
		return;
	}
}

static void bt_addr_str(const bt_addr_le_t *le_addr, char *addr_trunc, size_t len)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (len > BT_ADDR_LE_STR_LEN) {
		len = BT_ADDR_LE_STR_LEN;
	}
	bt_addr_le_to_str(le_addr, addr, sizeof(addr));

	memcpy(addr_trunc, addr, len);
	addr_trunc[len - 1] = 0;

	bt_to_upper(addr_trunc, len);
}

static void bt_conn_addr(struct bt_conn *conn, char *addr_trunc, size_t len)
{
	bt_addr_str(bt_conn_get_dst(conn), addr_trunc, len);
}

static struct ble_device_conn *bt_conn_addr_lookup(struct bt_conn *conn, 
						   char *addr_trunc, size_t len)
{
	struct ble_device_conn *connected_ptr;

	bt_conn_addr(conn, addr_trunc, len);
	ble_conn_mgr_get_conn_by_addr(addr_trunc, &connected_ptr);
	return connected_ptr;
}

static int svc_attr_data_add(const struct bt_gatt_service_val *gatt_service,
			      uint16_t handle, struct ble_device_conn *ble_conn_ptr)
{
	char str[UUID_STR_LEN];

	bt_uuid_get_str(gatt_service->uuid, str, sizeof(str));

	bt_to_upper(str, strlen(str));
	LOG_INF("Service %s", log_strdup(str));

	return ble_conn_mgr_add_uuid_pair(gatt_service->uuid, handle, 0, 0,
					  BT_ATTR_SERVICE, ble_conn_ptr, true);
}

static int chrc_attr_data_add(const struct bt_gatt_chrc *gatt_chrc,
			       struct ble_device_conn *ble_conn_ptr)
{
	uint16_t handle = gatt_chrc->value_handle;

	return ble_conn_mgr_add_uuid_pair(gatt_chrc->uuid, handle, 1,
					  gatt_chrc->properties, BT_ATTR_CHRC,
					  ble_conn_ptr, false);
}

static int ccc_attr_data_add(const struct bt_uuid *uuid, uint16_t handle,
			      struct ble_device_conn *ble_conn_ptr)
{
	LOG_DBG("\tHandle: %d", handle);

	return ble_conn_mgr_add_uuid_pair(uuid, handle, 2, 0, BT_ATTR_CCC,
					  ble_conn_ptr, false);
}

/* Add attributes to the connection manager objects */
static int attr_add(const struct bt_gatt_dm *dm,
	const struct bt_gatt_dm_attr *attr,
	struct ble_device_conn *ble_conn_ptr)
{
	const struct bt_gatt_service_val *gatt_service =
		bt_gatt_dm_attr_service_val(attr);
	const struct bt_gatt_chrc *gatt_chrc =
		bt_gatt_dm_attr_chrc_val(attr);
	int err = 0;

	if ((bt_uuid_cmp(attr->uuid, BT_UUID_GATT_PRIMARY) == 0) ||
		(bt_uuid_cmp(attr->uuid, BT_UUID_GATT_SECONDARY) == 0)) {
		err = svc_attr_data_add(gatt_service, attr->handle,
					ble_conn_ptr);
	} else if (bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CHRC) == 0) {
		err = chrc_attr_data_add(gatt_chrc, ble_conn_ptr);
	} else if (bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC) == 0) {
		err = ccc_attr_data_add(attr->uuid, attr->handle, ble_conn_ptr);
	}
	return err;
}

int ble_dm_data_add(struct bt_gatt_dm *dm)
{
	const struct bt_gatt_dm_attr *attr = NULL;
	char addr_trunc[BT_ADDR_STR_LEN];
	struct ble_device_conn *ble_conn_ptr;
	struct bt_conn *conn_obj;
	int err;

	conn_obj = bt_gatt_dm_conn_get(dm);

	ble_conn_ptr = bt_conn_addr_lookup(conn_obj, addr_trunc, BT_ADDR_STR_LEN);

	if (!conn_obj) {
		LOG_ERR("No conn_obj");
	}
	if (!ble_conn_ptr) {
		LOG_ERR("Connection not found for addr %s", log_strdup(addr_trunc));
	}
	discover_in_progress = true;

	attr = bt_gatt_dm_service_get(dm);

	err = attr_add(dm, attr, ble_conn_ptr);
	if (err) {
		LOG_ERR("Unable to add attribute: %d", err);
		return err;
	}

	while (NULL != (attr = bt_gatt_dm_attr_next(dm, attr))) {
		err = attr_add(dm, attr, ble_conn_ptr);
		if (err) {
			LOG_ERR("Unable to add attribute: %d", err);
			return err;
		}
	}
	return 0;
}

void ble_register_notify_callback(notification_cb_t callback)
{
	notify_callback = callback;
}

/* Thread responsible for transferring ble data over MQTT */
void send_notify_data(int unused1, int unused2, int unused3)
{
	char uuid[BT_UUID_STR_LEN];
	char path[BT_MAX_PATH_LEN];

	memset(uuid, 0, BT_UUID_STR_LEN);
	memset(path, 0, BT_MAX_PATH_LEN);

	struct ble_device_conn *connected_ptr;

	while (1) {
		int err;
		struct rec_data_t *rx_data = k_fifo_get(&rec_fifo, K_NO_WAIT);

		if (rx_data == NULL) {
			/* no pending notifications, so let others take turn */
			k_sleep(K_MSEC(10));
			continue;
		}
		err = ble_conn_mgr_get_conn_by_addr(rx_data->addr_trunc,
					      &connected_ptr);
		if (err) {
			LOG_ERR("Connection not found for addr %s",
				log_strdup(rx_data->addr_trunc));
			goto cleanup;
		}

		uint16_t handle;

		if (rx_data->read) {
			handle = rx_data->read_params.single.handle;
			LOG_INF("Read: Addr %s Handle %d",
				log_strdup(rx_data->addr_trunc), handle);
		} else {
			handle = rx_data->sub_params.value_handle;
			LOG_DBG("Notify Addr %s Handle %d",
				log_strdup(rx_data->addr_trunc), handle);
		}

		err = ble_conn_mgr_get_uuid_by_handle(handle, uuid,
						      connected_ptr);
		if (err) {
			if (discover_in_progress) {
				LOG_INF("Ignoring notification on %s due to BLE"
					" discovery in progress",
					log_strdup(rx_data->addr_trunc));
			} else {
				LOG_ERR("Unable to convert handle: %d", err);
			}
			goto cleanup;
		}

		bool ccc = !rx_data->read;

		if (strcmp(uuid, BT_UUID_GATT_CCC_VAL_STR) == 0) {
			ccc = true;
			handle--;
			LOG_INF("Force ccc for handle %u", handle);
		}

		err = ble_conn_mgr_generate_path(connected_ptr, handle, path,
						 ccc);
		if (err) {
			LOG_ERR("Unable to generate path: %d", err);
			goto cleanup;
		}

		LOG_HEXDUMP_DBG(rx_data->data, rx_data->length, "notify");

		if (!rx_data->read && notify_callback) {
			err = notify_callback(rx_data->addr_trunc, uuid,
					      rx_data->data, rx_data->length);
			if (err) {
				/* callback should return 0 if it did not
				 * process the data
				 */
				goto cleanup;
			}
		}
		if (connected_ptr && connected_ptr->hidden) {
			LOG_DBG("Suppressing notification write to cloud");
			goto cleanup;
		}

		k_mutex_lock(&output.lock, K_FOREVER);
		if (rx_data->read && !ccc) {
			err = device_chrc_read_encode(rx_data->addr_trunc,
				uuid, path, ((char *)rx_data->data),
				rx_data->length, &output);
		} else if (rx_data->read && ccc) {
			err = device_descriptor_value_encode(rx_data->addr_trunc,
							     BT_UUID_GATT_CCC_VAL_STR,
							     path,
							     ((char *)rx_data->data),
							     rx_data->length, 
							     &output, false);
		} else {
			err = device_value_changed_encode(rx_data->addr_trunc,
				uuid, path, ((char *)rx_data->data),
				rx_data->length, &output);
		}
		if (err) {
			k_mutex_unlock(&output.lock);
			LOG_ERR("Unable to encode: %d", err);
			goto cleanup;
		}
		LOG_DBG("UUID %s, path %s, len %u, json %s",
			log_strdup(uuid), log_strdup(path),
			rx_data->length, log_strdup((char *)output.data.ptr));
		err = g2c_send(&output.data);
		k_mutex_unlock(&output.lock);
		if (err) {
			LOG_ERR("Unable to send: %d", err);
		}

cleanup:
		k_free(rx_data);
		atomic_dec(&queued_notifications);
	}
}

K_THREAD_DEFINE(ble_rec_thread, SEND_NOTIFY_STACK_SIZE,
	send_notify_data, NULL, NULL, NULL,
	SEND_NOTIFY_PRIORITY, 0, 0);

static void discovery_completed(struct bt_gatt_dm *disc, void *ctx)
{
	LOG_DBG("Attribute count: %d", bt_gatt_dm_attr_cnt(disc));

	int err;

	err = ble_dm_data_add(disc);
	/* TBD: how can we cancel remainder of discovery if we get an error? */

	bt_gatt_dm_data_release(disc);

	bt_gatt_dm_continue(disc, NULL);
}

/* Despite the name. This is what is called at the end of a discovery service.*/
static void discovery_service_not_found(struct bt_conn *conn, void *ctx)
{
	char addr_trunc[BT_ADDR_STR_LEN];
	struct ble_device_conn *connected_ptr;

	connected_ptr = bt_conn_addr_lookup(conn, addr_trunc, BT_ADDR_STR_LEN);

	LOG_DBG("Service not found for %s!", log_strdup(addr_trunc));

	/* only set discovered true and send results if it seems we were
	 * successful at doing a full discovery
	 */
	if (connected_ptr != NULL) {
		if (connected_ptr->connected && connected_ptr->num_pairs) {
			connected_ptr->encode_discovered = true;
			connected_ptr->discovered = true;
		} else {
			LOG_WRN("Discovery not completed");
		}
		connected_ptr->discovering = false;
	} else {
		LOG_ERR("Connection not found for addr %s", log_strdup(addr_trunc));
	}
	discover_in_progress = false;

	/* check scan waiting */
	if (scan_waiting) {
		scan_start(print_scan_results);
	}
}

static void discovery_error_found(struct bt_conn *conn, int err, void *ctx)
{
	char addr_trunc[BT_ADDR_STR_LEN];
	struct ble_device_conn *connected_ptr;

	connected_ptr = bt_conn_addr_lookup(conn, addr_trunc, BT_ADDR_STR_LEN);
	if (connected_ptr != NULL) {
		connected_ptr->num_pairs = 0;
		connected_ptr->discovering = false;
		connected_ptr->discovered = false;
	}
	discover_in_progress = false;

	LOG_ERR("The discovery procedure failed for %s: err %d",
		log_strdup(addr_trunc), err);

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		LOG_INF("Saving settings");
		settings_save();
	}

	/* Disconnect? */
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);

	/* check scan waiting */
	if (scan_waiting) {
		scan_start(print_scan_results);
	}
}

static uint8_t gatt_read_callback(struct bt_conn *conn, uint8_t err,
	struct bt_gatt_read_params *params,
	const void *data, uint16_t length)
{
	char addr_trunc[BT_ADDR_STR_LEN];
	int ret = BT_GATT_ITER_CONTINUE;

	if ((length > 0) && (data != NULL)) {
		bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);
		LOG_INF("Read %s", log_strdup(addr_trunc));

		struct rec_data_t read_data = {
			.fifo_reserved = NULL,
			.length = length,
			.read = true
		};

		memset(&read_data.sub_params, 0, sizeof(read_data.sub_params));
		memcpy(&read_data.read_params, params,
			sizeof(struct bt_gatt_read_params));
		memcpy(&read_data.addr_trunc, addr_trunc, strlen(addr_trunc));
		memcpy(&read_data.data, data, length);

		size_t size = sizeof(struct rec_data_t);

		char *mem_ptr = k_malloc(size);

		if (mem_ptr == NULL) {
			LOG_ERR("Out of memory error in gatt_read_callback(): "
				"%d queued notifications",
				atomic_get(&queued_notifications));
			ret = BT_GATT_ITER_STOP;
		} else {
			atomic_inc(&queued_notifications);
			memcpy(mem_ptr, &read_data, size);
			k_fifo_put(&rec_fifo, mem_ptr);
		}
	}

	return ret;
}

static int gatt_read_handle(bt_addr_le_t *addr, uint16_t handle, bool ccc)
{
	int err;
	static struct bt_gatt_read_params params;
	struct bt_conn *conn;

	if (ccc) {
		handle++;
	}
	params.handle_count = 1;
	params.single.handle = handle;
	params.func = gatt_read_callback;

	conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
	if (conn == NULL) {
		LOG_ERR("Null Conn object");
		return -EINVAL;
	}

	err = bt_gatt_read(conn, &params);
	bt_conn_unref(conn);
	return err;
}

int gatt_read(char *ble_addr, char *chrc_uuid, bool ccc)
{
	int err;
	struct ble_device_conn *connected_ptr;
	uint16_t handle;

	err = ble_conn_mgr_get_conn_by_addr(ble_addr, &connected_ptr);
	if (err) {
		return err;
	}

	err = ble_conn_mgr_get_handle_by_uuid(&handle, chrc_uuid,
					      connected_ptr);
	if (err) {
		return err;
	}

	return gatt_read_handle(&connected_ptr->bt_addr, handle, ccc);
}

static void on_sent(struct bt_conn *conn, uint8_t err,
	struct bt_gatt_write_params *params)
{
	const void *data;
	uint16_t length;

	/* Make a copy of volatile data that is required by the callback. */
	data = params->data;
	length = params->length;

	LOG_DBG("Sent Data of Length: %d", length);
}

int gatt_write(const char *ble_addr, const char *chrc_uuid, uint8_t *data,
	       uint16_t data_len, bt_gatt_write_func_t cb)
{
	int err;
	struct bt_conn *conn;
	struct ble_device_conn *connected_ptr;
	uint16_t handle;
	static struct bt_gatt_write_params params;

	err = ble_conn_mgr_get_conn_by_addr(ble_addr, &connected_ptr);
	if (err) {
		return err;
	}

	err = ble_conn_mgr_get_handle_by_uuid(&handle, chrc_uuid,
					      connected_ptr);
	if (err) {
		return err;
	}

	conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &connected_ptr->bt_addr);
	if (conn == NULL) {
		LOG_ERR("Null Conn object");
		return -EINVAL;
	}

	for (int i = 0; i < data_len; i++) {
		LOG_DBG("Writing: %x", data[i]);
	}

	LOG_DBG("Writing to addr: %s to chrc %s with handle %d",
		log_strdup(ble_addr), log_strdup(chrc_uuid), handle);
	LOG_HEXDUMP_DBG(data, data_len, "Data to write");

	params.func = cb ? cb : on_sent;
	params.handle = handle;
	params.offset = 0;
	params.data = data;
	params.length = data_len;

	err = bt_gatt_write(conn, &params);
	bt_conn_unref(conn);
	return err;
}

int gatt_write_without_response(char *ble_addr, char *chrc_uuid, uint8_t *data,
				uint16_t data_len)
{
	int err;

	struct bt_conn *conn;
	struct ble_device_conn *connected_ptr;
	uint16_t handle;
	/* static struct bt_gatt_write_params params; */

	err = ble_conn_mgr_get_conn_by_addr(ble_addr, &connected_ptr);
	if (err) {
		return err;
	}

	err = ble_conn_mgr_get_handle_by_uuid(&handle, chrc_uuid,
					      connected_ptr);
	if (err) {
		return err;
	}

	conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &connected_ptr->bt_addr);
	if (conn == NULL) {
		LOG_ERR("Null Conn object");
		return -EINVAL;
	}

	LOG_DBG("Writing w/o resp to addr: %s to chrc %s with handle %d",
		log_strdup(ble_addr), log_strdup(chrc_uuid), handle);
	LOG_HEXDUMP_DBG(data, data_len, "Data to write");

	err = bt_gatt_write_without_response(conn, handle, data, data_len,
					     false);
	bt_conn_unref(conn);
	return err;
}

static uint8_t on_received(struct bt_conn *conn,
	struct bt_gatt_subscribe_params *params,
	const void *data, uint16_t length)
{
	char addr_trunc[BT_ADDR_STR_LEN];
	int ret = BT_GATT_ITER_CONTINUE;

	if (!data) {
		return BT_GATT_ITER_STOP;
	}

	if ((length > 0) && (data != NULL)) {

		bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);

		struct rec_data_t tx_data = {
			.read = false,
			.length = length
		};

		memcpy(&tx_data.addr_trunc, addr_trunc, strlen(addr_trunc));
		memcpy(&tx_data.data, data, length);
		memcpy(&tx_data.sub_params, params,
			sizeof(struct bt_gatt_subscribe_params));

		size_t size = sizeof(struct rec_data_t);

		if (atomic_get(&queued_notifications) >=
		     NOTIFICATION_QUEUE_LIMIT) {
			struct rec_data_t *rx_data = k_fifo_get(&rec_fifo,
								K_NO_WAIT);

			LOG_INF("Dropping oldest message");
			if (rx_data != NULL) {
				uint16_t h;

				if (rx_data->read) {
					h = rx_data->read_params.single.handle;
				} else  {
					h = rx_data->sub_params.value_handle;
				}
				LOG_INF("Addr %s Handle %d Queued %d",
					log_strdup(rx_data->addr_trunc),
					h,
					atomic_get(&queued_notifications));
				k_free(rx_data);
			}
			atomic_dec(&queued_notifications);
		}

		char *mem_ptr = k_malloc(size);

		if (mem_ptr == NULL) {
			LOG_ERR("Out of memory error in on_received(): "
				"%d queued notifications",
				atomic_get(&queued_notifications));
			ret = BT_GATT_ITER_STOP;
		} else {
			atomic_inc(&queued_notifications);
			memcpy(mem_ptr, &tx_data, size);
			k_fifo_put(&rec_fifo, mem_ptr);
		}
	}

	return ret;
}

static void send_sub(char *ble_addr, char *path, struct gw_msg *out,
		     uint8_t *value)
{
	k_mutex_lock(&out->lock, K_FOREVER);
	device_descriptor_value_encode(ble_addr,
				       BT_UUID_GATT_CCC_VAL_STR,
				       path, value, sizeof(value),
				       out, true);
	g2c_send(&out->data);
	device_value_write_result_encode(ble_addr,
					 BT_UUID_GATT_CCC_VAL_STR,
					 path, value, sizeof(value),
					 out);
	g2c_send(&out->data);
	k_mutex_unlock(&out->lock);
}

int ble_subscribe(char *ble_addr, char *chrc_uuid, uint8_t value_type)
{
	int err;
	char path[BT_MAX_PATH_LEN];
	struct bt_conn *conn = NULL;
	struct ble_device_conn *connected_ptr;
	uint16_t handle;
	bool subscribed;
	uint8_t param_index;

	err = ble_conn_mgr_get_conn_by_addr(ble_addr, &connected_ptr);
	if (err) {
		LOG_ERR("Connection not found for addr %s", log_strdup(ble_addr));
		return -ENOLINK;
	}

	ble_conn_mgr_get_handle_by_uuid(&handle, chrc_uuid, connected_ptr);

	ble_conn_mgr_get_subscribed(handle, connected_ptr, &subscribed,
				    &param_index);

	if (connected_ptr->connected) {
		conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &connected_ptr->bt_addr);
		if (conn == NULL) {
			/* work around strange error on Flic button --
			 * type changes when it should not
			 */
			connected_ptr->bt_addr.type = 0;
			conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &connected_ptr->bt_addr);
			if (conn == NULL) {
				LOG_ERR("Null Conn object");
				err = -EINVAL;
				goto end;
			}
		}
	}

	err = ble_conn_mgr_generate_path(connected_ptr, handle, path, true);
	if (err) {
		goto end;
	}

	if (subscribed && (value_type == 0)) {
		/* If subscribed then unsubscribe. */
		if (conn != NULL) {
			bt_gatt_unsubscribe(conn, &sub_param[param_index]);
		}
		ble_conn_mgr_remove_subscribed(handle, connected_ptr);

		uint8_t value[2] = {0, 0};

		if (connected_ptr && connected_ptr->hidden) {
			LOG_DBG("suppressing value_changed");
		} else {
			send_sub(ble_addr, path, &output, value);
		}
		LOG_INF("Unsubscribe: Addr %s Handle %d",
			log_strdup(ble_addr), handle);

		sub_value[param_index] = 0;
		sub_conn[param_index] = NULL;
		if (curr_subs) {
			curr_subs--;
		}
	} else if (subscribed && (value_type != 0)) {
		uint8_t value[2] = {
			value_type,
			0
		};

		if (connected_ptr && connected_ptr->hidden) {
			LOG_DBG("suppressing value_changed");
		} else {
			send_sub(ble_addr, path, &output, value);
		}
		LOG_INF("Subscribe Dup: Addr %s Handle %d %s",
			log_strdup(ble_addr), handle,
			(value_type == BT_GATT_CCC_NOTIFY) ?
			"Notify" : "Indicate");

	} else if (value_type == 0) {
		LOG_DBG("Unsubscribe N/A: Addr %s Handle %d",
			log_strdup(ble_addr), handle);
	} else if (curr_subs < SUBSCRIPTION_LIMIT) {
		if (conn != NULL) {
			sub_param[next_sub_index].notify = on_received;
			sub_param[next_sub_index].value = value_type;
			sub_param[next_sub_index].value_handle = handle;
			sub_param[next_sub_index].ccc_handle = handle + 1;
			sub_value[next_sub_index] = value_type;
			sub_conn[next_sub_index] = conn;
			err = bt_gatt_subscribe(conn, &sub_param[next_sub_index]);
			if (err) {
				LOG_ERR("Subscribe failed (err %d)", err);
				goto end;
			}
		}
		ble_conn_mgr_set_subscribed(handle, next_sub_index,
					    connected_ptr);

		uint8_t value[2] = {
			value_type,
			0
		};

		if (connected_ptr && connected_ptr->hidden) {
			LOG_DBG("suppressing value_changed");
		} else {
			send_sub(ble_addr, path, &output, value);
		}
		LOG_INF("Subscribe: Addr %s Handle %d %s",
			log_strdup(ble_addr), handle,
			(value_type == BT_GATT_CCC_NOTIFY) ?
			"Notify" : "Indicate");

		curr_subs++;
		next_sub_index++;
	} else {
		char msg[64];

		sprintf(msg, "Reached subscription limit of %d",
			SUBSCRIPTION_LIMIT);

		/* Send error when limit is reached. */
		k_mutex_lock(&output.lock, K_FOREVER);
		device_error_encode(ble_addr, msg, &output);
		g2c_send(&output.data);
		k_mutex_unlock(&output.lock);
	}

end:
	if (conn != NULL) {
		bt_conn_unref(conn);
	}
	return err;
}

int ble_subscribe_handle(char *ble_addr, uint16_t handle, uint8_t value_type)
{
	char uuid[BT_UUID_STR_LEN];
	struct ble_device_conn *conn_ptr;
	int err;

	LOG_DBG("Addr %s handle %u value_type %u", log_strdup(ble_addr),
		handle, (unsigned)value_type);
	err = ble_conn_mgr_get_conn_by_addr(ble_addr, &conn_ptr);
	if (err == 0) {
		err = ble_conn_mgr_get_uuid_by_handle(handle, uuid, conn_ptr);
		if (err == 0) {
			LOG_DBG("Subscribing uuid %s",
				log_strdup(uuid));
			err = ble_subscribe(ble_addr, uuid, value_type);
		}
	}
	return err;
}

int ble_subscribe_all(char *ble_addr, uint8_t value_type)
{
	unsigned int i;
	struct ble_device_conn *conn_ptr;
	struct uuid_handle_pair *up;
	int err;

	err = ble_conn_mgr_get_conn_by_addr(ble_addr, &conn_ptr);
	if (err) {
		return err;
	}

	for (i = 0; i < conn_ptr->num_pairs; i++) {
		up = conn_ptr->uuid_handle_pairs[i];
		if (up == NULL) {
			continue;
		}
		if ((up->properties & BT_GATT_CHRC_NOTIFY) !=
		    BT_GATT_CHRC_NOTIFY) {
			continue;
		}
		err = ble_subscribe_handle(ble_addr, up->handle, value_type);
		if (err) {
			break;
		}
	}
	return err;
}


int ble_subscribe_device(struct bt_conn *conn, bool subscribe)
{
	uint16_t handle;
	int i;
	int count = 0;
	struct ble_device_conn *connected_ptr;
	char addr_trunc[BT_ADDR_STR_LEN];

	if (conn == NULL) {
		return -EINVAL;
	}

	connected_ptr = bt_conn_addr_lookup(conn, addr_trunc, BT_ADDR_STR_LEN);

	if (connected_ptr == NULL) {
		LOG_ERR("Could not find ble_device_conn for %s",
			log_strdup(addr_trunc));
		return -EINVAL;
	}

	for (i = 0; i < BT_MAX_SUBSCRIBES; i++) {
		if (conn == sub_conn[i]) {
			handle = sub_param[i].value_handle;
			if (subscribe && sub_value[i]) {
				sub_param[i].value = sub_value[i];
				bt_gatt_subscribe(conn, &sub_param[i]);
				ble_conn_mgr_set_subscribed(handle, i,
							    connected_ptr);
				LOG_INF("Subscribe: Addr %s Handle %d Idx %d",
					log_strdup(addr_trunc), handle, i);
				count++;
			} else {
				bt_gatt_unsubscribe(conn, &sub_param[i]);
				ble_conn_mgr_remove_subscribed(handle,
							       connected_ptr);
				sub_conn[i] = NULL;
				if (curr_subs) {
					curr_subs--;
				}
				LOG_INF("Unsubscribe: Addr %s Handle %d Idx %d",
					log_strdup(addr_trunc), handle, i);
				count++;
			}
		}
	}
	LOG_INF("Subscriptions changed for %d handles", count);
	return 0;
}

static struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed,
	.service_not_found = discovery_service_not_found,
	.error_found = discovery_error_found,
};

int ble_discover(struct ble_device_conn *connection_ptr)
{
	int err = 0;
	char *ble_addr = connection_ptr->addr;
	struct bt_conn *conn = NULL;

	if (!discover_in_progress) {

		conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &connection_ptr->bt_addr);
		if (conn == NULL) {
			LOG_DBG("ERROR: Null Conn object");
			return -EINVAL;
		}

		if (!connection_ptr->discovered) {
			LOG_INF("Discovering: %s\n", log_strdup(ble_addr));
			if (connection_ptr->num_pairs) {
				LOG_INF("Marking device as discovered; "
					"num pairs = %u",
					connection_ptr->num_pairs);
				connection_ptr->discovering = false;
				connection_ptr->discovered = true;
				connection_ptr->encode_discovered = true;
				goto end;
			}
			discover_in_progress = true;
			connection_ptr->discovering = true;

			for (int i = 1; i < 3; i++) {
				if (i > 1) {
					LOG_INF("Retrying...");
				}
				err = bt_gatt_dm_start(conn, NULL, &discovery_cb, NULL);
				if (!err) {
					break;
				}
				LOG_WRN("Service discovery err %d", err);
				k_sleep(K_MSEC(500));
			}
			if (err) {
				LOG_ERR("Aborting discovery.  Disconnecting from device...");
				connection_ptr->discovering = false;
				discover_in_progress = false;
				bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
				bt_conn_unref(conn);
				ble_conn_set_connected(connection_ptr, false);
				return err;
			}
		} else {
			connection_ptr->encode_discovered = true;
		}
	} else {
		err = -EBUSY;
	}

end:
	if (conn) {
		bt_conn_unref(conn);
	}
	return err;
}

int disconnect_device_by_addr(char *ble_addr)
{
	int err;
	struct bt_conn *conn;
	bt_addr_le_t addr;

	err = bt_addr_le_from_str(ble_addr, "random", &addr);
	if (err) {
		LOG_ERR("Address from string failed (err %d)", err);
		return err;
	}

	conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &addr);
	if (conn == NULL) {
		LOG_DBG("ERROR: Null Conn object");
		return -EINVAL;
	}

	/* cloud commanded this, so remove any notifications or indications */
	err = ble_subscribe_device(conn, false);
	if (err) {
		LOG_ERR("Error unsubscribing device: %d", err);
	}

	/* Disconnect device. */
	err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (err) {
		LOG_ERR("Error disconnecting: %d", err);
	}
	bt_conn_unref(conn);

	return err;
}

int set_shadow_modem(void *modem)
{
	int err;

	k_mutex_lock(&output.lock, K_FOREVER);
	err = gateway_shadow_data_encode(modem, &output);
	if (!err) {
		err = gw_shadow_publish(&output.data);
		if (err) {
			LOG_ERR("nrf_cloud_gw_shadow_publish() failed %d", err);
		}
	} else {
		LOG_ERR("gateway_shadow_data_encode() failed %d", err);
	}
	k_mutex_unlock(&output.lock);
	return err;
}

int set_shadow_ble_conn(char *ble_address, bool connecting, bool connected)
{
	int err;

	LOG_DBG("Connecting=%u, connected=%u", connecting, connected);
	k_mutex_lock(&output.lock, K_FOREVER);
	err = device_shadow_data_encode(ble_address, connecting, connected,
					&output);
	if (!err) {
		err = gw_shadow_publish(&output.data);
		if (err) {
			LOG_ERR("nrf_cloud_gw_shadow_publish() failed %d", err);
		}
	} else {
		LOG_ERR("device_shadow_data_encode() failed %d", err);
	}
	k_mutex_unlock(&output.lock);
	return err;
}

int set_shadow_desired_conn(struct desired_conn *desired, int num_desired)
{
	int err;

	k_mutex_lock(&output.lock, K_FOREVER);
	err = gateway_desired_list_encode(desired,
					  num_desired,
					  &output);
	if (!err) {
		err = gw_shadow_publish(&output.data);
		if (err) {
			LOG_ERR("nrf_cloud_gw_shadow_publish() failed %d", err);
		}
	} else {
		LOG_ERR("nrf_cloud_encode_gateway_desired_list() failed %d", err);
	}
	k_mutex_unlock(&output.lock);

	return err;
}

void auto_conn_start_work_handler(struct k_work *work)
{
	int err;

	/* Restart to scanning for allowlisted devices */
	struct bt_conn_le_create_param param = BT_CONN_LE_CREATE_PARAM_INIT(
		BT_CONN_LE_OPT_NONE,
		BT_GAP_SCAN_FAST_INTERVAL,
		BT_GAP_SCAN_FAST_WINDOW);

	err = bt_conn_le_create_auto(&param, BT_LE_CONN_PARAM_DEFAULT);

	if (err == -EALREADY) {
		LOG_INF("Auto connect already running");
	} else if (err == -EINVAL) {
		LOG_INF("Auto connect not necessary or invalid");
	} else if (err == -EAGAIN) {
		LOG_WRN("Device not ready; try starting auto connect later");
		k_timer_start(&auto_conn_start_timer, K_SECONDS(3), K_SECONDS(0));
	} else if (err == -ENOMEM) {
		LOG_ERR("No more connections available when enabling auto connect");
		print_conns();
	} else if (err) {
		LOG_ERR("Error enabling auto connect: %d", err);
	} else {
		LOG_INF("Auto connect started");
	}
}

K_WORK_DEFINE(auto_conn_start_work, auto_conn_start_work_handler);

void auto_conn_start_timer_handler(struct k_timer *timer)
{
	k_work_submit(&auto_conn_start_work);
}

K_TIMER_DEFINE(auto_conn_start_timer, auto_conn_start_timer_handler, NULL);

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr_trunc[BT_ADDR_STR_LEN];
	struct ble_device_conn *connection_ptr;

	connection_ptr = bt_conn_addr_lookup(conn, addr_trunc, BT_ADDR_STR_LEN);

	if (conn_err) {
		LOG_ERR("Failed to connect to %s (%u)", log_strdup(addr_trunc),
			conn_err);
		if (connection_ptr) {
			ble_conn_set_connected(connection_ptr, false);
		}
		bt_conn_unref(conn);
		k_timer_start(&auto_conn_start_timer, K_SECONDS(3), K_SECONDS(0));
		return;
	}
	if (connection_ptr && connection_ptr->hidden) {
		LOG_DBG("suppressing device_connect");
	} else {
		k_mutex_lock(&output.lock, K_FOREVER);
		device_connect_result_encode(addr_trunc, true, &output);
		g2c_send(&output.data);
		k_mutex_unlock(&output.lock);
	}

#if defined(CONFIG_BT_BONDABLE)
	int err;

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Failed to set security: %d", err);
	} else {
		LOG_INF("Security set; waiting for changed callback");
		return;
	}
#endif

	if (connection_ptr != NULL) {
		if (!connection_ptr->connected) {
			LOG_INF("Connected: %s", log_strdup(addr_trunc));
			if (!connection_ptr->connected) {
				set_shadow_ble_conn(addr_trunc, false, true);
			}
			ble_conn_set_connected(connection_ptr, true);
			ble_subscribe_device(conn, true);
		} else {
			LOG_INF("Reconnected: %s", log_strdup(addr_trunc));
		}
		if (connection_ptr->added_to_allowlist) {
			if (!ble_add_to_allowlist(addr_trunc, false)) {
				connection_ptr->added_to_allowlist = false;
			}
		}
	}

	ui_led_set_pattern(UI_BLE_CONNECTED, PWM_DEV_1);

	/* Start the timer to begin scanning again. */
	k_timer_start(&auto_conn_start_timer, K_SECONDS(3), K_SECONDS(0));
}

#if defined(CONFIG_BT_BONDABLE)
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr_trunc[BT_ADDR_STR_LEN + 1];
	struct ble_device_conn *connection_ptr;

	connection_ptr = bt_conn_addr_lookup(conn, addr_trunc, BT_ADDR_STR_LEN);

	if (!err) {
		LOG_INF("Security changed: %s level %u", log_strdup(addr_trunc),
			level);
	} else {
		if (err != BT_SECURITY_ERR_PAIR_NOT_SUPPORTED) {
			LOG_WRN("Security failed: %s level %u err %d",
				log_strdup(addr_trunc), level, err);
			return;
		} else {
			LOG_INF("Pairing not supported");
		}
	}

	if (connection_ptr != NULL) {
		if (!connection_ptr->connected) {
			LOG_INF("Connected: %s", log_strdup(addr_trunc));
			set_shadow_ble_conn(addr_trunc, false, true);
			ble_conn_set_connected(connection_ptr, true);
			ble_subscribe_device(conn, true);
		} else {
			LOG_INF("Reconnected: %s", log_strdup(addr_trunc));
		}
		if (connection_ptr->added_to_allowlist) {
			if (!ble_add_to_allowlist(addr_trunc, false)) {
				connection_ptr->added_to_allowlist = false;
			}
		}
	}

	ui_led_set_pattern(UI_BLE_CONNECTED, PWM_DEV_1);

	/* Start the timer to begin scanning again. */
	k_timer_start(&auto_conn_start_timer, K_SECONDS(3), K_SECONDS(0));
}
#endif

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr_trunc[BT_ADDR_STR_LEN];
	struct ble_device_conn *connection_ptr = NULL;

	connection_ptr = bt_conn_addr_lookup(conn, addr_trunc, BT_ADDR_STR_LEN);

	if (!ble_conn_mgr_is_desired(addr_trunc)) {
		LOG_INF("suppressing device_disconnect");
	} else {
		k_mutex_lock(&output.lock, K_FOREVER);
		device_disconnect_result_encode(addr_trunc, false, &output);
		g2c_send(&output.data);
		k_mutex_unlock(&output.lock);
	}

	/* if device disconnected on purpose, don't bother updating
	 * shadow; it will likely reconnect shortly
	 */
	if (reason != BT_HCI_ERR_REMOTE_USER_TERM_CONN) {
		LOG_INF("Disconnected: %s (reason 0x%02x)", log_strdup(addr_trunc),
			reason);
		if (connection_ptr && !connection_ptr->hidden) {
			(void)set_shadow_ble_conn(addr_trunc, false, false);
		}
		if (connection_ptr) {
			ble_conn_set_connected(connection_ptr, false);
		}
	} else {
		LOG_INF("Disconnected: temporary");
	}

	if (connection_ptr &&
	    !connection_ptr->free &&
	    !connection_ptr->added_to_allowlist) {
		if (!ble_add_to_allowlist(connection_ptr->addr, true)) {
			connection_ptr->added_to_allowlist = true;
		}
	}
	bt_conn_unref(conn);

	ui_led_set_pattern(UI_BLE_DISCONNECTED, PWM_DEV_1);

	/* Start the timer to begin scanning again. */
	k_timer_start(&auto_conn_start_timer, K_SECONDS(3), K_SECONDS(0));
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
#if defined(CONFIG_BT_BONDABLE)
	.security_changed = security_changed
#endif
};

#if defined(CONFIG_BT_BONDABLE)
#if defined(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
int user_passkey = 0;

enum bt_security_err auth_pairing_accept(struct bt_conn *conn,
					 const struct bt_conn_pairing_feat * const feat)
{
	enum bt_security_err out = BT_SECURITY_ERR_SUCCESS; /* for now */
	char addr_trunc[BT_ADDR_STR_LEN];

	bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);
	printk("%s: %s: %u %u %u %u %u %u\n", __FUNCTION__, addr_trunc,
	       feat->auth_req, feat->init_key_dist, feat->io_capability,
	       feat->max_enc_key_size, feat->oob_data_flag, feat->resp_key_dist);
	return out;
}
#endif

void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr_trunc[BT_ADDR_STR_LEN];

	bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);
	printk("%s: %s: %d\n", __FUNCTION__, addr_trunc, passkey);
}

void auth_passkey_entry(struct bt_conn *conn)
{
	unsigned int passkey = 0;
	int err;
	char addr_trunc[BT_ADDR_STR_LEN];

	bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);
	printk("%s: %s: giving fixed passkey %d\n", __FUNCTION__, addr_trunc, passkey);
	err = bt_conn_auth_passkey_entry(conn, passkey);
}

void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	int err;
	char addr_trunc[BT_ADDR_STR_LEN];

	bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);
	printk("%s: %s: checking passkey %d against %d\n", __FUNCTION__, addr_trunc,
	       passkey, user_passkey);
	if (passkey == user_passkey) {
		printk("-- passkey match\n");
		err = bt_conn_auth_passkey_confirm(conn);
	} else {
		printk("-- passkey incorrect\n");
		err = bt_conn_auth_cancel(conn);
	}
}

void auth_cancel(struct bt_conn *conn)
{
	char addr_trunc[BT_ADDR_STR_LEN];

	bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);
	printk("%s: %s: cancelling user request\n", __FUNCTION__, addr_trunc);
}

void auth_pairing_confirm(struct bt_conn *conn)
{
	char addr_trunc[BT_ADDR_STR_LEN];
	bool confirm = true;

	bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);
	printk("%s: %s: confirming pairing\n", __FUNCTION__, addr_trunc);

	if (confirm) {
		printk("-- confirmed\n");
		bt_conn_auth_pairing_confirm(conn);
	} else {
		printk("-- denied\n");
		bt_conn_auth_cancel(conn);
	}
}

void auth_pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr_trunc[BT_ADDR_STR_LEN];

	bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);
	printk("%s: %s: pairing complete; bonded: %d\n",
	       __FUNCTION__, addr_trunc, bonded);
}

void auth_pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr_trunc[BT_ADDR_STR_LEN];
	int err;

	bt_conn_addr(conn, addr_trunc, BT_ADDR_STR_LEN);
	printk("%s: %s: pairing failed; reason: %d\n",
	       __FUNCTION__, addr_trunc, reason);
	/* not clear this is the right thing to do... */
	if (reason == BT_SECURITY_ERR_UNSPECIFIED) {
		err = bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(conn));
		if (err) {
			LOG_ERR("Error %d unpairing", err);
		} else {
			LOG_INF("Unpaired");
		}
	}
}

void auth_bond_deleted(uint8_t tid, const bt_addr_le_t *peer)
{
	char addr_trunc[BT_ADDR_STR_LEN];

	bt_addr_str(peer, addr_trunc, BT_ADDR_STR_LEN);
	printk("%s: %s: tid: %d: bond deleted\n", __FUNCTION__, addr_trunc, tid);
}

static struct bt_conn_auth_cb auth_callbacks = {
	.bond_deleted = auth_bond_deleted,
	.cancel = auth_cancel,
	.oob_data_request = NULL,
#if defined(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
	.pairing_accept = auth_pairing_accept,
#endif
	.pairing_complete = auth_pairing_complete,
	.pairing_confirm = auth_pairing_confirm,
	.pairing_failed = auth_pairing_failed,
	.passkey_confirm = auth_passkey_confirm,
	.passkey_display = auth_passkey_display,
	.passkey_entry = auth_passkey_entry,
#if defined(CONFIG_BT_BREDR)
	.pincode_entry = NULL
#endif
};
#endif

static bool data_cb(struct bt_data *data, void *user_data)
{
	char *name = user_data;
	int len = MIN(data->data_len, NAME_LEN - 1);

	switch (data->type) {
	case BT_DATA_NAME_SHORTENED:
	case BT_DATA_NAME_COMPLETE:
		memcpy(name, data->data, len);
		name[len] = '\0';
		return false;
	default:
		return true;
	}
}

void ble_device_found_enc_handler(struct k_work *work)
{
	LOG_DBG("Encoding scan...");
	k_mutex_lock(&output.lock, K_FOREVER);
	device_found_encode(num_devices_found, &output);
	LOG_DBG("Sending scan...");
	g2c_send(&output.data);
	k_mutex_unlock(&output.lock);
}

K_WORK_DEFINE(ble_device_encode_work, ble_device_found_enc_handler);

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
	struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	char name[NAME_LEN];
	struct ble_scanned_dev *scanned = &ble_scanned_devices[num_devices_found];

	if (num_devices_found >= MAX_SCAN_RESULTS) {
		return;
	}

	/* We're only interested in connectable events */
	if (type != BT_HCI_ADV_IND && type != BT_HCI_ADV_DIRECT_IND) {
		return;
	}

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

	/* Check for duplicate addresses */
	for (int j = 0; j < num_devices_found; j++) {
		if (!(strncasecmp(addr_str,
				  ble_scanned_devices[j].addr,
				  BT_ADDR_LE_DEVICE_LEN))) {
			return; /* no need to continue; we saw this */
		}
	}

	memcpy(scanned->type,
	       addr_str + BT_ADDR_LE_DEVICE_LEN_SHIFT, BT_ADDR_LE_TYPE_LEN);
	scanned->type[BT_ADDR_LE_TYPE_LEN] = 0;

	bt_to_upper(addr_str, BT_ADDR_LE_STR_LEN);
	memcpy(scanned->addr, addr_str,
	       BT_ADDR_LE_DEVICE_LEN);
	scanned->addr[BT_ADDR_LE_DEVICE_LEN] = 0;

	memset(name, 0, sizeof(name));
	bt_data_parse(ad, data_cb, name);
	strcpy(scanned->name, name);
	if (strlen(name)) {
		num_names_found++;
	}

	scanned->rssi = (int)rssi;
	num_devices_found++;

	LOG_INF("%d. %s, %d, %s", num_devices_found,
		log_strdup(scanned->addr), rssi, log_strdup(scanned->name));
}

struct ble_scanned_dev *get_scanned_device(unsigned int i)
{
	if (i < num_devices_found) {
		return &ble_scanned_devices[i];
	} else {
		return NULL;
	}
}

int get_num_scan_results(void)
{
	return num_devices_found;
}

int get_num_scan_names(void)
{
	return num_names_found;
}

void scan_off_handler(struct k_work *work)
{
	int err;

	LOG_INF("Stopping scan...");

	err = bt_le_scan_stop();
	if (err) {
		LOG_INF("Stopping scanning failed (err %d)", err);
	} else {
		LOG_INF("Scan successfully stopped");
	}

	/* Start the timer to begin scanning again. */
	k_timer_start(&auto_conn_start_timer, K_SECONDS(3), K_SECONDS(0));

	LOG_DBG("Submitting scan...");
	k_work_submit(&ble_device_encode_work);

	if (print_scan_results) {
		print_scan_results = false;

		struct ble_scanned_dev *scanned;
		int i;

		printk("Scan results:\n");
		for (i = 0, scanned = ble_scanned_devices;
		     i < num_devices_found; i++, scanned++) {
			printk("%d. %s, %d, %s\n", i, scanned->addr,
			       (int)scanned->rssi, scanned->name);
			k_sleep(K_MSEC(50));
		}
	}
}

K_WORK_DEFINE(scan_off_work, scan_off_handler);

void scan_timer_handler(struct k_timer *timer)
{
	k_work_submit(&scan_off_work);
}

K_TIMER_DEFINE(scan_timer, scan_timer_handler, NULL);

int ble_add_to_allowlist(char *addr_str, bool add)
{
	int err;
	bt_addr_le_t addr;

	LOG_INF("%s allowlist: %s", add ? "Adding address to" : "Removing address from",
		log_strdup(addr_str));

	bt_conn_create_auto_stop();

	err = bt_addr_le_from_str(addr_str, "random", &addr);
	if (err) {
		LOG_ERR("Invalid peer address: %d", err);
		goto done;
	}

	err = add ? bt_le_whitelist_add(&addr) : bt_le_whitelist_rem(&addr);
	if (err) {
		LOG_ERR("Error %s allowlist: %d", add ? "adding to" : "removing from", err);
	}

done:
	/* Start the timer to begin scanning again. */
	k_timer_start(&auto_conn_start_timer, K_SECONDS(3), K_SECONDS(0));

	return err;
}

void ble_stop_activity(void)
{
	int err;

	LOG_INF("Stop scan...");
	err = bt_le_scan_stop();
	if (err) {
		LOG_DBG("Error stopping scan: %d", err);
	}

	LOG_INF("Stop autoconnect...");
	err = bt_conn_create_auto_stop();
	if (err) {
		LOG_DBG("Error stopping autoconnect: %d", err);
	}

	LOG_INF("Clear allowlist...");
	err = bt_le_whitelist_clear();
	if (err) {
		LOG_DBG("Error clearing allowlist: %d", err);
	}

	struct ble_device_conn *conn;
	uint8_t ret;

	LOG_INF("Disconnect devices...");
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		conn = get_connected_device(i);
		if (conn && (conn->connected)) {
			ret = disconnect_device_by_addr(conn->addr);
			if (ret) {
				LOG_DBG("Error disconnecting %s",
					log_strdup(conn->addr));
			}
		}
	}
}

int device_discovery_send(struct ble_device_conn *conn_ptr)
{
	if (conn_ptr && conn_ptr->hidden) {
		LOG_DBG("suppressing device_discovery_send");
		return 0;
	}
	k_mutex_lock(&output.lock, K_FOREVER);
	int ret = device_discovery_encode(conn_ptr, &output);

	if (!ret) {
		LOG_INF("Sending discovery; JSON Size: %d", output.data.len);
		g2c_send(&output.data);
	}
	memset((char *)output.data.ptr, 0, output.data.len);
	k_mutex_unlock(&output.lock);

	return ret;
}

void scan_start(bool print)
{
	int err;

	print_scan_results = print;
	num_devices_found = 0;
	num_names_found = 0;
	memset(ble_scanned_devices, 0, sizeof(ble_scanned_devices));

	struct bt_le_scan_param param = {
		.type     = BT_LE_SCAN_TYPE_ACTIVE,
		.options  = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = 0x0010,
		.window   = 0x0010,
	};

	if (!discover_in_progress) {
		/* Stop the auto connect */
		bt_conn_create_auto_stop();

		err = bt_le_scan_start(&param, device_found);
		if (err) {
			LOG_ERR("Bluetooth set active scan failed "
				"(err %d)\n", err);
		} else {
			LOG_INF("Bluetooth active scan enabled");

			/* TODO: Get scan timeout from scan message */
			k_timer_start(&scan_timer, K_SECONDS(10), K_SECONDS(0));
		}

		scan_waiting = false;
	} else {
		LOG_INF("Scan waiting... Discover in progress");
		scan_waiting = true;
	}
}

void ble_clear_discover_inprogress()
{
	discover_in_progress = false;
}

static void ble_ready(int err)
{
	LOG_INF("Bluetooth ready");

	bt_conn_cb_register(&conn_callbacks);
#if defined(CONFIG_BT_BONDABLE)
	bt_conn_auth_cb_register(&auth_callbacks);
#endif
}

int ble_init(void)
{
	int err;

	LOG_INF("Initializing Bluetooth..");
	k_mutex_init(&output.lock);

	err = bt_enable(ble_ready);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}

	for (int i = 0; i < MAX_SCAN_RESULTS; i++) {
		memset(ble_scanned_devices[i].name, 0,
		       sizeof(ble_scanned_devices[1].name));
	}

	return 0;
}