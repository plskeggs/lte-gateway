#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <logging/log.h>
#include <net/fota_download.h>
#include <sys/crc.h>

#include "nrf_cloud_fota.h"
#include "ble.h"
#include "gateway.h"
#include "ble_conn_mgr.h"
#include "peripheral_dfu.h"

#define DFU_CONTROL_POINT_UUID "8EC90001F3154F609FB8838830DAEA50"
#define DFU_PACKET_UUID        "8EC90002F3154F609FB8838830DAEA50"
#define DFU_BUTTONLESS_UUID    "8EC90003F3154F609FB8838830DAEA50"
#define MAX_CHUNK_SIZE 20
#define PROGRESS_UPDATE_INTERVAL 5

#define CALL_TO_PRINTK(fmt, ...) do {		 \
		printk(fmt "\n", ##__VA_ARGS__); \
	} while (false)

#define LOGPKINF(...)	do {					     \
				if (use_printk) {		     \
					CALL_TO_PRINTK(__VA_ARGS__); \
				} else {			     \
					LOG_INF(__VA_ARGS__);        \
				}				     \
		      } while (false)

#define STRDUP(x) use_printk ? (x) : log_strdup(x)

enum nrf_dfu_op_t {
  NRF_DFU_OP_PROTOCOL_VERSION = 0x00,
  NRF_DFU_OP_OBJECT_CREATE = 0x01,
  NRF_DFU_OP_RECEIPT_NOTIF_SET = 0x02,
  NRF_DFU_OP_CRC_GET = 0x03,
  NRF_DFU_OP_OBJECT_EXECUTE = 0x04,
  NRF_DFU_OP_OBJECT_SELECT = 0x06,
  NRF_DFU_OP_MTU_GET = 0x07,
  NRF_DFU_OP_OBJECT_WRITE = 0x08,
  NRF_DFU_OP_PING = 0x09,
  NRF_DFU_OP_HARDWARE_VERSION = 0x0A,
  NRF_DFU_OP_FIRMWARE_VERSION = 0x0B,
  NRF_DFU_OP_ABORT = 0x0C,
  NRF_DFU_OP_RESPONSE = 0x60,
  NRF_DFU_OP_INVALID = 0xFF
};

enum nrf_dfu_result_t {
  NRF_DFU_RES_CODE_INVALID = 0x00,
  NRF_DFU_RES_CODE_SUCCESS = 0x01,
  NRF_DFU_RES_CODE_OP_CODE_NOT_SUPPORTED = 0x02,
  NRF_DFU_RES_CODE_INVALID_PARAMETER = 0x03,
  NRF_DFU_RES_CODE_INSUFFICIENT_RESOURCES = 0x04,
  NRF_DFU_RES_CODE_INVALID_OBJECT = 0x05,
  NRF_DFU_RES_CODE_UNSUPPORTED_TYPE = 0x07,
  NRF_DFU_RES_CODE_OPERATION_NOT_PERMITTED = 0x08,
  NRF_DFU_RES_CODE_OPERATION_FAILED = 0x0A,
  NRF_DFU_RES_CODE_EXT_ERROR = 0x0B
};

enum  nrf_dfu_firmware_type_t {
  NRF_DFU_FIRMWARE_TYPE_SOFTDEVICE = 0x00,
  NRF_DFU_FIRMWARE_TYPE_APPLICATION = 0x01,
  NRF_DFU_FIRMWARE_TYPE_BOOTLOADER = 0x02,
  NRF_DFU_FIRMWARE_TYPE_UNKNOWN = 0xFF
};

struct notify_packet {
	uint8_t magic;
	uint8_t op;
	uint8_t result;
	union {
		struct select {
			uint32_t max_size;
			uint32_t offset;
			uint32_t crc;
		} select;
		struct crc {
			uint32_t offset;
			uint32_t crc;
		} crc;
		struct hw_version {
			uint32_t part;
			uint32_t variant;
			uint32_t rom_size;
			uint32_t ram_size;
			uint32_t rom_page_size;
		} hw;
		struct fw_version {
			uint8_t type;
			uint32_t version;
			uint32_t addr;
			uint32_t len;
		} fw;
	};
} __attribute__((packed));

K_SEM_DEFINE(peripheral_dfu_active, 1, 1);
static size_t download_size;
static size_t completed_size;
static bool finish_page;
static bool notify_received;
static bool test_mode;
static bool verbose;
static uint16_t notify_length;
static uint32_t max_size;
static uint32_t flash_page_size;
static uint32_t page_remaining;
static uint8_t notify_data[MAX_CHUNK_SIZE];
static char ble_addr[BT_ADDR_STR_LEN];
struct notify_packet *notify_packet_data = (struct notify_packet *)notify_data;
static struct nrf_cloud_fota_ble_job fota_ble_job;
static char app_version[16];
static int image_size;
static uint16_t original_crc;
static int socket_retries_left;
static bool first_fragment;
static bool init_packet;
static bool use_printk;
static struct download_client dlc;

static int send_select_command(char *ble_addr);
static int send_create_command(char *ble_addr, uint32_t size);
static int send_select_data(char *ble_addr);
static int send_create_data(char *ble_addr, uint32_t size);
static int send_prn(char *ble_addr, uint16_t receipt_rate);
static int send_data(char *ble_addr, const char *buf, size_t len);
static int send_request_crc(char *ble_addr);
static int send_execute(char *ble_addr);
static int send_abort(char *ble_addr);
static void on_sent(struct bt_conn *conn, uint8_t err,
		    struct bt_gatt_write_params *params);
static int send_hw_version_get(char *ble_addr);
static int send_fw_version_get(char *ble_addr, uint8_t fw_type);
static uint8_t peripheral_dfu(const char *buf, size_t len);
static int download_client_callback(const struct download_client_evt *event);
static void cancel_dfu(enum nrf_cloud_fota_error error);
static void fota_ble_callback(const struct nrf_cloud_fota_ble_job *
			      const ble_job);

LOG_MODULE_REGISTER(peripheral_dfu, CONFIG_NRF_CLOUD_GATEWAY_LOG_LEVEL);

void peripheral_dfu_set_test_mode(bool test)
{
	test_mode = test;
}

static int notification_callback(const char *addr, const char *chrc_uuid,
				 uint8_t *data, uint16_t len)
{
	if (strcmp(addr, ble_addr) != 0) {
		return 0; /* not for us */
	}
	if (strcmp(chrc_uuid, DFU_CONTROL_POINT_UUID) != 0) {
		return 0; /* not for us */
	}
	notify_length = MIN(len, sizeof(notify_data));
	memcpy(notify_data, data, notify_length);
	notify_received = true;
	LOG_DBG("dfu notified %u bytes", notify_length);
	return 1;
}

static int wait_for_notification(void)
{
	int max_loops = 500;
	while (!notify_received) {
		k_sleep(K_MSEC(10));
		if (!max_loops--) {
			return -ETIMEDOUT;
		}
	}
	return 0;
}

static int decode_notification(void)
{
	struct notify_packet *p = notify_packet_data;
	char buf[256];

	if (notify_length < 3) {
		LOG_ERR("Notification too short: %u < 3", notify_length);
		return -EINVAL;
	}
	if (p->magic != 0x60) {
		LOG_ERR("First byte not 0x60: 0x%02X", p->magic);
		return -EINVAL;
	}
	switch (p->op) {
	case NRF_DFU_OP_OBJECT_CREATE:
		snprintf(buf, sizeof(buf), "DFU Create response: 0x%02X",
			 p->result);
		break;
	case NRF_DFU_OP_RECEIPT_NOTIF_SET:
		snprintf(buf, sizeof(buf),
			 "DFU Set Receipt Notification response: 0x%02X",
			 p->result);
		break;
	case NRF_DFU_OP_CRC_GET:
		if (notify_length < 7) {
			LOG_ERR("Notification too short: %u < 7",
				notify_length);
			return -EINVAL;
		}
		snprintf(buf, sizeof(buf), "DFU CRC response: 0x%02X, "
					   "offset: 0x%08X, crc: 0x%08X",
		       p->result, p->crc.offset, p->crc.crc);
		break;
	case NRF_DFU_OP_OBJECT_EXECUTE:
		snprintf(buf, sizeof(buf), "DFU Execute response: 0x%02X",
			 p->result);
		break;
	case NRF_DFU_OP_OBJECT_SELECT:
		snprintf(buf, sizeof(buf),
			 "DFU Select response: 0x%02X, offset: 0x%08X,"
			 " crc: 0x%08X, max_size: %u", p->result,
			 p->select.offset, p->select.crc, p->select.max_size);
		max_size = p->select.max_size;
		break;
	case NRF_DFU_OP_HARDWARE_VERSION:
		snprintf(buf, sizeof(buf),
			"DFU HW Version Get response: 0x%02X, part: 0x%08X,"
			" variant: 0x%08X, rom_size: 0x%08X, ram_size: 0x%08X,"
			" rom_page_size: 0x%08X", p->result, p->hw.part,
			p->hw.variant, p->hw.rom_size, p->hw.ram_size,
			p->hw.rom_page_size);
		flash_page_size = p->hw.rom_page_size;
		break;
	case NRF_DFU_OP_FIRMWARE_VERSION:
		snprintf(buf, sizeof(buf),
			 "DFU FW Version Get response: 0x%02X, type: 0x%02X,"
			 " version: 0x%02X, addr: 0x%02X, len: 0x%02X",
			 p->result, p->fw.type, p->fw.version, p->fw.addr,
			 p->fw.len);
		break;
	default:
		LOG_ERR("Unknown DFU notification: 0x%02X", p->op);
		return -EINVAL;
	}
	if (p->result == NRF_DFU_RES_CODE_SUCCESS) {
		if (verbose) {
			LOG_INF("%s", log_strdup(buf));
		} else {
			LOG_DBG("%s", log_strdup(buf));
		}
		return 0;
	} else {
		LOG_WRN("BLE operation failed: %d", p->result);
		return -EFAULT;
	}
}

int peripheral_dfu_init(void)
{
	int err;

	err = download_client_init(&dlc, download_client_callback);
	if (err) {
		return err;
	}
	return nrf_cloud_fota_ble_set_handler(fota_ble_callback);
}

int peripheral_dfu_config(const char *addr, int size, const char *version,
			   uint32_t crc, bool init_pkt, bool use_prtk)
{
	int err;

	err = k_sem_take(&peripheral_dfu_active, K_NO_WAIT);
	if (err) {
		LOG_ERR("Peripheral DFU already active.");
		return err;
	}
	LOG_DBG("peripheral_dfu_active LOCKED: %s",
		log_strdup(k_thread_name_get(k_current_get())));

	strncpy(ble_addr, addr, sizeof(ble_addr));
	strncpy(app_version, version, sizeof(app_version));
	image_size = size;
	original_crc = crc;
	init_packet = init_pkt;
	use_printk = use_prtk;
	return 0;
}

int peripheral_dfu_start(const char *host, const char *file, int sec_tag,
			 const char *apn, size_t fragment_size)
{
	int err = -1;

	struct download_client_cfg config = {
		.sec_tag = sec_tag,
		.apn = apn,
		.frag_size_override = fragment_size,
		.set_tls_hostname = (sec_tag != -1),
	};

	if (host == NULL || file == NULL) {
		k_sem_give(&peripheral_dfu_active);
		LOG_DBG("peripheral_dfu_active UNLOCKED: %s",
			log_strdup(k_thread_name_get(k_current_get())));
		return -EINVAL;
	}

	socket_retries_left = CONFIG_FOTA_SOCKET_RETRIES;

	first_fragment = true;

	err = download_client_connect(&dlc, host, &config);
	if (err != 0) {
		k_sem_give(&peripheral_dfu_active);
		LOG_DBG("peripheral_dfu_active UNLOCKED: %s",
			log_strdup(k_thread_name_get(k_current_get())));
		return err;
	}

	err = download_client_start(&dlc, file, 0);
	if (err != 0) {
		download_client_disconnect(&dlc);
		k_sem_give(&peripheral_dfu_active);
		LOG_DBG("peripheral_dfu_active UNLOCKED: %s",
			log_strdup(k_thread_name_get(k_current_get())));
		return err;
	}

	return 0;
}

static int download_client_callback(const struct download_client_evt *event)
{
	int err = 0;

	if (event == NULL) {
		return -EINVAL;
	}

	switch (event->id) {
	case DOWNLOAD_CLIENT_EVT_FRAGMENT:
		err = peripheral_dfu(event->fragment.buf,
				     event->fragment.len);
		if (err) {
			LOG_ERR("Error from peripheral_dfu: %d", err);
		}
		break;
	case DOWNLOAD_CLIENT_EVT_DONE:
		LOG_INF("Download client done");
		err = download_client_disconnect(&dlc);
		if (err) {
			LOG_ERR("Error disconnecting from download client: %d",
				err);
		}
		break;
	case DOWNLOAD_CLIENT_EVT_ERROR: {
		/* In case of socket errors we can return 0 to retry/continue,
		 * or non-zero to stop
		 */
		if ((socket_retries_left) && ((event->error == -ENOTCONN) ||
					      (event->error == -ECONNRESET))) {
			LOG_WRN("Download socket error. %d retries left...",
				socket_retries_left);
			socket_retries_left--;
			/* Fall through and return 0 below to tell
			 * download_client to retry
			 */
		} else {
			err = download_client_disconnect(&dlc);
			if (err) {
				LOG_ERR("Error disconnecting from "
					"download client: %d", err);
			}
			LOG_ERR("Download client error");
			cancel_dfu(NRF_CLOUD_FOTA_ERROR_DOWNLOAD);
			err = -EIO;
		}
		break;
	}
	default:
		break;
	}

	return err;
}

static int start_ble_job(struct nrf_cloud_fota_ble_job *const ble_job)
{
	__ASSERT_NO_MSG(ble_job != NULL);
	int ret;
	enum nrf_cloud_fota_status status;

	ret = peripheral_dfu_start(ble_job->info.host, ble_job->info.path,
				  CONFIG_NRF_CLOUD_SEC_TAG, NULL,
				  CONFIG_NRF_CLOUD_FOTA_DOWNLOAD_FRAGMENT_SIZE);
	if (ret) {
		LOG_ERR("Failed to start FOTA download: %d", ret);
		status = NRF_CLOUD_FOTA_FAILED;
	} else {
		LOG_INF("Downloading update");
		status = NRF_CLOUD_FOTA_IN_PROGRESS;
	}
	(void)nrf_cloud_fota_ble_job_update(ble_job, status);

	return ret;
}

static void fota_ble_callback(const struct nrf_cloud_fota_ble_job *
			      const ble_job)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bool init_pkt;
	char *ver = "n/a";
	uint32_t crc = 0;
	int sec_tag = CONFIG_NRF_CLOUD_SEC_TAG;
	char *apn = NULL;
	size_t frag = CONFIG_NRF_CLOUD_FOTA_DOWNLOAD_FRAGMENT_SIZE;

	bt_addr_to_str(&ble_job->ble_id, addr, sizeof(addr));

	if (!ble_conn_mgr_is_addr_connected(addr)) {
		/* TODO: add ability to queue? */
		LOG_WRN("Device not connected; ignoring job");
		return;
	}

	init_pkt = strstr(ble_job->info.path, "dat") != NULL;

	if (peripheral_dfu_config(addr, ble_job->info.file_size, ver, crc,
			      init_pkt, false)) {
		/* could not configure, so don't start job */
		return;
	}

	/* job structure will disappear after this callback, so make a copy */
	fota_ble_job.ble_id = ble_job->ble_id;
	fota_ble_job.info.type = ble_job->info.type;
	fota_ble_job.info.id = strdup(ble_job->info.id);
	fota_ble_job.info.host = strdup(ble_job->info.host);
	fota_ble_job.info.path = strdup(ble_job->info.path);
	fota_ble_job.info.file_size = ble_job->info.file_size;
	if (ble_job->info.path2) {
		fota_ble_job.info.path2 = strdup(ble_job->info.path2);
		fota_ble_job.info.file_size2 = ble_job->info.file_size2;
	} else {
		fota_ble_job.info.path2 = NULL;
		fota_ble_job.info.file_size2 = 0;
	}
	fota_ble_job.error = NRF_CLOUD_FOTA_ERROR_NONE;

	LOG_INF("starting BLE DFU to addr:%s, from host:%s, "
		"path:%s, size:%d, init:%d, ver:%s, crc:%u, "
		"sec_tag:%d, apn:%s, frag_size:%zd",
		log_strdup(addr), log_strdup(ble_job->info.host),
		log_strdup(ble_job->info.path), ble_job->info.file_size,
		init_packet, ver, crc, sec_tag,
		apn ? apn : "<n/a>", frag);
	if (fota_ble_job.info.path2) {
		LOG_INF("second DFU path: %s, size:%d",
			log_strdup(ble_job->info.path2),
			ble_job->info.file_size2);
	}

	(void)start_ble_job(&fota_ble_job);
}

static bool start_second_job(void)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_to_str(&fota_ble_job.ble_id, addr, sizeof(addr));

	if (!ble_conn_mgr_is_addr_connected(addr)) {
		/* TODO: add ability to queue? */
		LOG_WRN("Device not connected; ignoring job");
		return false;
	}

	if (!fota_ble_job.info.path2) {
		LOG_ERR("No second job to start!");
		return false;
	}

	/* rotate jobs */
	free(fota_ble_job.info.path);
	fota_ble_job.info.path = fota_ble_job.info.path2;
	fota_ble_job.info.path2 = NULL;
	fota_ble_job.info.file_size = fota_ble_job.info.file_size2;
	fota_ble_job.info.file_size2 = 0;
	fota_ble_job.error = NRF_CLOUD_FOTA_ERROR_NONE;

	/* update globals */
	init_packet = strstr(fota_ble_job.info.path, "dat") != NULL;
	image_size = fota_ble_job.info.file_size;

	LOG_INF("starting second BLE DFU to addr:%s, from host:%s, "
		"path:%s, size:%d, init:%d",
		log_strdup(addr), log_strdup(fota_ble_job.info.host),
		log_strdup(fota_ble_job.info.path), fota_ble_job.info.file_size,
		init_packet);

	(void)start_ble_job(&fota_ble_job);
}

static void free_job(void)
{
	if (fota_ble_job.info.id) {
		free(fota_ble_job.info.id);
		free(fota_ble_job.info.host);
		free(fota_ble_job.info.path);
		if (fota_ble_job.info.path2) {
			free(fota_ble_job.info.path2);
		}
		memset(&fota_ble_job, 0, sizeof(fota_ble_job));
	}
}

static void cancel_dfu(enum nrf_cloud_fota_error error)
{
	ble_register_notify_callback(NULL);
	if (fota_ble_job.info.id) {
		char addr[BT_ADDR_LE_STR_LEN];
		enum nrf_cloud_fota_status status;
		int err;

		bt_addr_to_str(&fota_ble_job.ble_id, addr, sizeof(addr));

		LOG_INF("Sending DFU Abort...");
		send_abort(addr);

		/* TODO: adjust error according to actual failure */
		fota_ble_job.error = error;
		status = NRF_CLOUD_FOTA_FAILED;

		err = nrf_cloud_fota_ble_job_update(&fota_ble_job, status);
		if (err) {
			LOG_ERR("Error updating job: %d", err);
		}
		free_job();
		LOGPKINF("Update cancelled.");
	}
	k_sem_give(&peripheral_dfu_active);
	LOG_DBG("peripheral_dfu_active UNLOCKED: %s",
		log_strdup(k_thread_name_get(k_current_get())));
}

static uint8_t peripheral_dfu(const char *buf, size_t len)
{
	static size_t prev_percent = 0;
	static size_t prev_update_percent = 0;
	static uint32_t prev_crc = 0;
	size_t percent = 0;
	int err = 0;

	if (first_fragment) {
		LOG_INF("len:%zd, first:%u, size:%d, init:%u", len,
		       first_fragment, image_size, init_packet);
	}
	download_size = image_size;

	if (test_mode) {
		size_t percent = 0;

		if (first_fragment) {
			completed_size = 0;
			first_fragment = false;
		}
		completed_size += len;

		if (download_size) {
			percent = (100 * completed_size) / download_size;
		}

		LOG_INF("Progress: %zd%%; %zd of %zd",
		       percent, completed_size, download_size);

		if (percent >= 100) {
			LOG_INF("DFU complete");
		}
		return 0;
	}

	if (first_fragment) {
		first_fragment = false;
		LOGPKINF("BLE DFU starting to %s...", STRDUP(ble_addr));

		if (fota_ble_job.info.id) {
			fota_ble_job.dl_progress = 0;
			err = nrf_cloud_fota_ble_job_update(&fota_ble_job,
						    NRF_CLOUD_FOTA_DOWNLOADING);
			if (err) {
				LOG_ERR("Error updating job: %d", err);
				free_job();
				k_sem_give(&peripheral_dfu_active);
				LOG_DBG("peripheral_dfu_active UNLOCKED: %s",
				log_strdup(k_thread_name_get(k_current_get())));
				return err;
			}
		}
		completed_size = 0;
		prev_percent = 0;
		prev_update_percent = 0;
		prev_crc = 0;
		page_remaining = 0;
		finish_page = false;
		ble_register_notify_callback(notification_callback);

		if (init_packet) {
			verbose = true;
			flash_page_size = 0;

			LOG_INF("Loading Init Packet and "
			       "turning on notifications...");
			err = ble_subscribe(ble_addr, DFU_CONTROL_POINT_UUID,
					    BT_GATT_CCC_NOTIFY);
			if (err) {
				goto cleanup;
			}

			LOG_INF("Setting DFU PRN to 0...");
			err = send_prn(ble_addr, 0);
			if (err) {
				goto cleanup;
			}

			LOG_INF("Querying hardware version...");
			(void)send_hw_version_get(ble_addr);

			LOG_INF("Querying firmware version (APP)...");
			(void)send_fw_version_get(ble_addr,
					     NRF_DFU_FIRMWARE_TYPE_APPLICATION);

			LOG_INF("Sending DFU select command...");
			err = send_select_command(ble_addr);
			if (err) {
				goto cleanup;
			}
			/* TODO: check offset and CRC; do no transfer
			 * and skip execute if offset != init length or
			 * CRC mismatch -- however, to do this, we need the
			 * cloud to send us the expected CRC for the file,
			 * which we cannot precalculate here unless we download
			 * and store the whole file locally first
			 */
			if (notify_packet_data->select.offset != image_size) {
				LOG_INF("image size mismatched, so continue; "
				       "offset:%u, size:%u",
				       notify_packet_data->select.offset,
				       image_size);
			} else {
				LOG_INF("image size matches device!");
				if (notify_packet_data->select.crc != original_crc) {
					LOG_INF("crc mismatched, so continue; "
					       "dev crc:0x%08X, this crc:"
					       "0x%08X",
					       notify_packet_data->select.crc,
					       original_crc);
				} else {
					LOG_INF("CRC matches device!");
					/* TODO: we could skip transferring the
					 * init packet and just do execute
					 */
				}
			}

			LOG_INF("Sending DFU create command...");
			err = send_create_command(ble_addr, image_size);
			/* This will need to be the size of the entire init
			 * file. If http chunks it smaller this won't work
			 */
			if (err) {
				goto cleanup;
			}
		} else {
			verbose = false;
			finish_page = false;

			LOG_INF("Loading Firmware");
			LOG_INF("Setting DFU PRN to 0...");
			err = send_prn(ble_addr, 0);
			if (err) {
				goto cleanup;
			}

			LOG_INF("Sending DFU select data...");
			err = send_select_data(ble_addr);
			if (err) {
				goto cleanup;
			}
			if (notify_packet_data->select.offset != image_size) {
				LOG_INF("image size mismatched, so continue; "
				       "offset:%u, size:%u",
				       notify_packet_data->select.offset,
				       image_size);
			} else {
				LOG_INF("image size matches device!");
				if (notify_packet_data->select.crc != original_crc) {
					LOG_INF("crc mismatched, so continue; "
					       "dev crc:0x%08X, this crc:"
					       "0x%08X",
					       notify_packet_data->select.crc,
					       original_crc);
				} else {
					LOG_INF("CRC matches device!");
					/* TODO: we could skip the entire file,
					 * and just send an indication to the
					 * cloud that the job is complete
					 */
				}
			}
		}
	}

	/* output data while handing page boundaries */
	while (len) {
		if (!finish_page) {
			uint32_t file_remaining = download_size - completed_size;

			page_remaining = MIN(max_size, file_remaining);

			LOG_DBG("page remaining %u, max_size %u, len %u",
			       page_remaining, max_size, len);
			if (!init_packet) {
				err = send_create_data(ble_addr, page_remaining);
				if (err) {
					goto cleanup;
				}
			}
			if (len < page_remaining) {
				LOG_DBG("Need to transfer %u in page",
				       page_remaining);
				finish_page = true;
			} else {
				if (!completed_size) {
					LOG_DBG("no need to finish first page "
					       "next time");
				}
			}
		}

		size_t page_len = MIN(len, page_remaining);

		LOG_DBG("Sending DFU data len %u...", page_len);
		err = send_data(ble_addr, buf, page_len);
		if (err) {
			goto cleanup;
		}

		prev_crc = crc32_ieee_update(prev_crc, buf, page_len);

		completed_size += page_len;
		len -= page_len;
		buf += page_len;
		LOG_DBG("Completed size: %u", completed_size);

		page_remaining -= page_len;
		if ((page_remaining == 0) ||
		    (completed_size == download_size)) {
			finish_page = false;
			LOG_DBG("page is complete");
		} else {
			LOG_DBG("page remaining: %u bytes", page_remaining);
		}

		if (!finish_page) {
			/* give hardware time to finish; omitting this step
			 * results in an error on the execute request and/or
			 * CRC mismatch; sometimes we are too fast so we
			 * need to retry, because the transfer was still
			 * going on
			 */
			for (int i = 1; i <= 5; i++) {
				k_sleep(K_MSEC(100));

				LOG_DBG("Sending DFU request CRC %d...", i);
				err = send_request_crc(ble_addr);
				if (err) {
					break;
				}
				if (notify_packet_data->crc.offset !=
				    completed_size) {
					err = -EIO;
				}
				else if (notify_packet_data->crc.crc !=
					 prev_crc) {
					err = -EBADMSG;
				} else {
					LOG_INF("CRC and length match.");
					err = 0;
					break;
				}
			}
			if (err == -EIO) {
				LOG_ERR("Transfer offset wrong; received: %u,"
					" expected: %u",
					notify_packet_data->crc.offset,
					completed_size);
			}
			if (err == -EBADMSG) {
				LOG_ERR("CRC wrong; received: 0x%08X, "
					"expected: 0x%08X",
					notify_packet_data->crc.crc,
					prev_crc);
			}
			if (err) {
				goto cleanup;
			}

			LOG_DBG("Sending DFU execute...");
			err = send_execute(ble_addr);
			if (err) {
				goto cleanup;
			}
		}
	}

	if (download_size) {
		percent = (100 * completed_size) / download_size;
	}

	if (percent != prev_percent) {
		LOGPKINF("Progress: %zd%%, %zd bytes of %zd", percent,
		       completed_size, download_size);
		prev_percent = percent;

		if (fota_ble_job.info.id &&
		    (((percent - prev_update_percent) >=
		      PROGRESS_UPDATE_INTERVAL) ||
		     (percent >= 100))) {
			enum nrf_cloud_fota_status status;

			LOG_INF("Sending job update at %zd%% percent",
			       percent);
			fota_ble_job.error = NRF_CLOUD_FOTA_ERROR_NONE;
			if (percent < 100) {
				status = NRF_CLOUD_FOTA_DOWNLOADING;
				fota_ble_job.dl_progress = percent;
			} else {
				status = NRF_CLOUD_FOTA_SUCCEEDED;
				fota_ble_job.dl_progress = 100;
			}
			err = nrf_cloud_fota_ble_job_update(&fota_ble_job,
							    status);
			if (err) {
				LOG_ERR("Error updating job: %d", err);
				goto cleanup;
			}
			prev_update_percent = percent;
		}
	}

	if (percent >= 100) {
		ble_conn_mgr_force_dfu_rediscover(ble_addr);
		ble_register_notify_callback(NULL);
		LOGPKINF("DFU complete");
		free_job();
		k_sem_give(&peripheral_dfu_active);
		LOG_DBG("peripheral_dfu_active UNLOCKED: %s",
			log_strdup(k_thread_name_get(k_current_get())));
	}

	return err;

cleanup:
	cancel_dfu(NRF_CLOUD_FOTA_ERROR_APPLY_FAIL);
	return err;
}

static int send_data(char *ble_addr, const char *buf, size_t len)
{
	/* "chunk" The data */
	int idx = 0;
	int err = 0;

	while (idx < len) {
		uint8_t size = MIN(MAX_CHUNK_SIZE, (len - idx));
		LOG_DBG("Sending write without response: %d, %d", size, idx);
		err = gatt_write_without_response(ble_addr, DFU_PACKET_UUID,
					    (uint8_t *)&buf[idx], size);
		if (err) {
			LOG_ERR("Error writing chunk at %d size %u: %d",
				idx, size, err);
			break;
		}
		idx += size;
	}
	return err;
}

/* name should be a string constant so log_strdup not needed */
static int do_cmd(char *ble_addr, char *uuid, uint8_t *buf, uint16_t len,
		  char *name)
{
	int err;

	notify_received = false;
	err = gatt_write(ble_addr, uuid, buf, len, on_sent);
	if (err) {
		LOG_ERR("Error writing %s: %d", name, err);
		return err;
	}
	err = wait_for_notification();
	if (err) {
		LOG_ERR("timeout waiting for notification from %s: %d",
			name, err);
		return err;
	}
	err = decode_notification();
	if (err) {
		LOG_ERR("notification decode error from %s: %d",
			name, err);
	}
	return err;
}

/* set number of writes between CRC reports; 0 disables */
static int send_prn(char *ble_addr, uint16_t receipt_rate)
{
	char smol_buf[3];

	smol_buf[0] = NRF_DFU_OP_RECEIPT_NOTIF_SET;
	smol_buf[1] = receipt_rate & 0xff;
	smol_buf[2] = (receipt_rate >> 8) & 0xff;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "PRN");
}

static int send_select_command(char *ble_addr)
{
	char smol_buf[2];

	smol_buf[0] = NRF_DFU_OP_OBJECT_SELECT;
	smol_buf[1] = 0x01;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "Select Cmd");
}

static int send_select_data(char *ble_addr)
{
	char smol_buf[2];

	smol_buf[0] = NRF_DFU_OP_OBJECT_SELECT;
	smol_buf[1] = 0x02;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "Select Data");
}

static int send_create_command(char *ble_addr, uint32_t size)
{
	uint8_t smol_buf[6];

	smol_buf[0] = NRF_DFU_OP_OBJECT_CREATE;
	smol_buf[1] = 0x01;
	smol_buf[2] = size & 0xFF;
	smol_buf[3] = (size >> 8) & 0xFF;
	smol_buf[4] = (size >> 16) & 0xFF;
	smol_buf[5] = (size >> 24) & 0xFF;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "Create Cmd");
}

static int send_create_data(char *ble_addr, uint32_t size)
{
	uint8_t smol_buf[6];

	LOG_DBG("Size is %d", size);

	smol_buf[0] = NRF_DFU_OP_OBJECT_CREATE;
	smol_buf[1] = 0x02;
	smol_buf[2] = size & 0xFF;
	smol_buf[3] = (size >> 8) & 0xFF;
	smol_buf[4] = (size >> 16) & 0xFF;
	smol_buf[5] = (size >> 24) & 0xFF;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "Create Data");
}

static void on_sent(struct bt_conn *conn, uint8_t err,
	struct bt_gatt_write_params *params)
{
	const void *data;
	uint16_t length;
	char str[32];

	/* TODO: Make a copy of volatile data that is passed to the
	 * callback.  Check err value at least in the wait function.
	 */
	data = params->data;
	length = params->length;

	sprintf(str, "resp err:%u, from write:", err);
	LOG_HEXDUMP_DBG(data, length, log_strdup(str));
}

static int send_request_crc(char *ble_addr)
{
	char smol_buf[1];

	/* Requesting 32 bit offset followed by 32 bit CRC */
	smol_buf[0] = NRF_DFU_OP_CRC_GET;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "CRC Request");
}

static int send_execute(char *ble_addr)
{
	char smol_buf[1];

	LOG_DBG("Sending Execute");

	smol_buf[0] = NRF_DFU_OP_OBJECT_EXECUTE;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "Execute");
}

static int send_hw_version_get(char *ble_addr)
{
	char smol_buf[1];

	LOG_INF("Sending HW Version Get; errors are normal");

	smol_buf[0] = NRF_DFU_OP_HARDWARE_VERSION;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "Get HW Ver");
}

static int send_fw_version_get(char *ble_addr, uint8_t fw_type)
{
	char smol_buf[2];

	LOG_INF("Sending FW Version Get; errors are normal");

	smol_buf[0] = NRF_DFU_OP_FIRMWARE_VERSION;
	smol_buf[1] = fw_type;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "Get FW Ver");
}

static int send_abort(char *ble_addr)
{
	char smol_buf[1];

	smol_buf[0] = 0x0C;

	return do_cmd(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
		      sizeof(smol_buf), "Abort");
}

#if 0
/* unclear what these functions are for */

int write_image_size_to_dfu_packet(char *ble_addr, int image_size)
{
	/* The length here is expected to be
	 * <len softdevice><len bootloader><len app> where each is a uint32
	 * In the case we aren't updating all of them (psst we are only doing
	 * the app) the others need to be 0 So ours should look like
	 * 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 <len app>
	 * all lengths are little endian
	 *
	 * https://infocenter.nordicsemi.com/index.jsp?topic=%2Fcom.nordic.infocenter.sdk5.v11.0.0%2Fbledfu_transport_bleservice.html
	 */
#define SIZE_LEN 12 /* Size of 3 uint32s */
	char buf[SIZE_LEN];
	int err;

	memset(buf, 0, sizeof(char) * SIZE_LEN);
	buf[8] = image_size & 0xFF;
	buf[9] = (image_size >> 8) & 0xFF;
	buf[10] = (image_size >> 16) & 0xFF;
	buf[11] = (image_size >> 24) & 0xFF;
	err = gatt_write_without_response(ble_addr, DFU_PACKET_UUID, buf,
					  SIZE_LEN);
	if (err) {
		LOG_ERR("Error sending image size: %d", err);
	}
	return err;
}

int send_initialize_dfu_parameters(char *ble_addr)
{
	char smol_buf[1];
	int err;

	smol_buf[0] = 0x02;
	err = gatt_write(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
			 sizeof(smol_buf), on_sent);
	if (err) {
		LOG_ERR("Error sending initialize dfu: %d", err);
	}
	return err;
}

int write_dfu_parameters(char *ble_addr, uint32_t app_version, uint16_t crc)
{
#define NUM_DEVICES 1
#define ANY_SOFTDEVICE 0xFFFE
	uint16_t device_type = 0;
	uint16_t device_rev = 0;
	/* This is bad if we change but it is fine for now */
	uint16_t softdevice[NUM_DEVICES] = {ANY_SOFTDEVICE};
	uint16_t softdevice_len = NUM_DEVICES;
	char *buf;
	int buf_len = (sizeof(device_type) + sizeof(device_rev) +
		       (sizeof(softdevice) * NUM_DEVICES) +
		       sizeof(softdevice_len));
	buf = malloc(buf_len);
	int i;
	int j;
	int err;

	if (buf == NULL) {
		LOG_ERR("Out of memory allocating DFU parameter buffer");
		return -ENOMEM;
	}

	/* Order is:
	 * 1) device type
	 * 2) device revision
	 * 3) application version
	 * 4) softdevice array length
	 * n) each softdevice
	 * n+1) CRC-16-CCITT of the image
	 */
	buf[0] = device_type & 0xFF;
	buf[1] = (device_type >> 8) & 0xFF;
	buf[2] = device_rev & 0xFF;
	buf[3] = (device_rev >> 8) & 0xFF;
	buf[4] = app_version & 0xFF;
	buf[5] = (app_version >> 8) & 0xFF;
	buf[6] = (app_version >> 16) & 0xFF;
	buf[7] = (app_version >> 24) & 0xFF;
	buf[8] = softdevice_len & 0xFF;
	buf[9] = (softdevice_len >> 8) & 0xFF;
	for (i = 10, j = 0; (i < (10 + (2 * softdevice_len))); i += 2, j++) {
		buf[i] = softdevice[j] & 0xFF;
		buf[i + 1] = (softdevice[j] >> 8) & 0xFF;
	}
	buf[i] = crc & 0xFF;
	buf[i + 1] = (crc >> 8) & 0xFF;

	err = gatt_write_without_response(ble_addr, DFU_PACKET_UUID, buf,
					  buf_len);
	if (err) {
		LOG_ERR("Error sending dfu parameters: %d", err);
	}
	free(buf);
	return err;
}

int send_start_dfu_packet(char *ble_addr)
{
	char smol_buf[1];
	int err;

	smol_buf[0] = 0x01;
	err = gatt_write(ble_addr, DFU_CONTROL_POINT_UUID, smol_buf,
			 sizeof(smol_buf), on_sent);
	if (err) {
		LOG_ERR("Error sending start dfu: %d", err);
	}
	return err;
}
#endif
