#include <zephyr.h>
#include <errno.h>
#include <string.h>
#include <settings/settings.h>

#include "password.h"

#define DEFAULT_PASSWORD CONFIG_SHELL_DEFAULT_PASSWORD

#if 1
int init_passwd(void)
{
	return 0;
}

int check_passwd(const char *passwd)
{
	return strcmp(passwd, DEFAULT_PASSWORD);
}

int is_valid_passwd(const char *passwd)
{
	if (strlen(passwd) < 8) {
		return -EINVAL;
	} else {
		return 0;
	}
}

int set_passwd(const char *passwd)
{
	return -ENOTSUP;
}

#else
#include <nrf_cc310_platform.h>
#include <ocrypto_sha256.h>

#define USE_CC3XX 1

#define MIN_PASSWORD_LENGTH 6
#define MAX_PASSWORD_LENGTH 16
#define MAX_SALT_LENGTH 16
#define MAX_HASH_LENGTH 32

#define SETTINGS_KEY_PASSWD "passwd"
#define SETTINGS_FULL_PASSWD NRF_CLOUD_SETTINGS_NAME \
			   "/" \
			   SETTINGS_KEY_PASSWD

struct passwd_store {
	uint8_t salt[MAX_SALT_LENGTH];
	uint8_t hash[MAX_HASH_LENGTH];
} __packed;

static struct passwd_store passwd_hashed;

SETTINGS_STATIC_HANDLER_DEFINE(passwd, SETTINGS_FULL_PASSWD, NULL,
			       passwd_settings_set, NULL, NULL);

static int read_passwd_hash(const char *key, size_t len_rd,
			     settings_read_cb read_cb, void *cb_arg)
{
	if (!key) {
		LOG_DBG("Key is NULL");
		return -EINVAL;
	}

	LOG_DBG("Settings key: %s, size: %d", log_strdup(key), len_rd);

	if (!strncmp(key, SETTINGS_KEY_PASSWD, strlen(SETTINGS_KEY_PASSWD)) &&
	    (len_rd == sizeof(passwd_hashed))) {
		if (read_cb(cb_arg, (void *)&passwd_hashed, len_rd) == len_rd) {
			LOG_HEXDUMP_DBG(passwd_hashed.salt, MAX_SALT_LENGTH,
					"Saved passwd salt");
			LOG_HEXDUMP_DBG(passwd_hashed.hash, MAX_HASH_LENGTH,
					"Saved passwd hash");
			return 0;
		}
	}
	return -ENOTSUP;
}

static int store_passwd_hash(void)
{
	ret = settings_save_one(SETTINGS_FULL_PASSWD, &passwd_hashed,
				sizeof(passwd_hashed));
	if (ret) {
		LOG_ERR("settings_save_one failed: %d", ret);
	}
	return ret;
}

int init_passwd(void)
{
	int ret;

#if USE_CC3XX
	ret = nrf_cc3xx_platform_init();
#else
	ret = 0;
#endif
	if (ret) {
		LOG_ERR("Error initializing nrf_cc3xx_platform: %d", ret);
	} else {
		ret = settings_load_subtree(settings_handler_passwd.name);
		if (ret) {
			LOG_ERR("Cannot load settings: %d", ret);
		}
	}
	return ret;
}

int check_passwd(const char *passwd)
{
	int err;
	uint8_t check_hash[32];

	err = hash_passwd(passwd);
	if (!err) {
		return memcmp(check_hash,
			      passwd_hashed.hash,
			      sizeof(passwd_hashed.hash));
	}
	return err;
}

int is_valid_passwd(const char *passwd)
{
	if ((strlen(passwd) > MAX_PASSWORD_LENGTH) ||
	    (strlen(passwd) < MIN_PASSWORD_LENGTH)) {
		return -EINVAL;
	} else {
		return 0;
	}
}

static int pick_salt(uint8_t *salt, size_t len)
{
	int ret;

	/* @TODO use NRG */
#if USE_CC3XX
	size_t olen;

	ret = nrf_cc3xx_platform_entropy_get(salt, len, &olen);
	/* do we need to check olen != len? */
#else
	strncpy(salt, "KJAA(U@jDI$#L+_901293u1234mnada", len);
	ret = 0;
#endif
	return ret;
}

static int hash_passwd(const char *passwd)
{
	ocrypto_sha256_ctx ctx;
	uint8_t buf[MAX_SALT_LENGTH + MAX_PASSWORD_LENGTH];

	memset(&buf, 0, sizeof(buf));

	ocrypto_sha256_init(&ctx);

	pick_salt(buf, MAX_SALT_LENGTH);
	memcpy(passwd_hashed.salt, buf, MAX_SALT_LENGTH);
	memcpy(&buf[MAX_SALT_LENGTH], passwd, strlen(passwd));

	ocrypto_sha256_update(&ctx, (const uint8_t *)buf, strlen(buf));
	ocrypto_sha256_final(&ctx, passwd_hashed.hash);

	return 0;
}

int set_passwd(const char *passwd)
{
	int err = hash_passwd(passwd);

	if (!err) {
		err = store_passwd_hash();
	}
	
	return err;
}
#endif
