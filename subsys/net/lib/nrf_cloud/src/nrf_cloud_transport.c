/*
 * Copyright (c) 2017 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include "nrf_cloud_transport.h"
#include "nrf_cloud_mem.h"

#include <zephyr.h>
#include <stdio.h>
#include <fcntl.h>
#include <net/mqtt.h>
#include <net/socket.h>
#include <net/cloud.h>
#include <logging/log.h>
#include <sys/util.h>
#include <settings/settings.h>

#if defined(CONFIG_NRF_MODEM_LIB)
#include <nrf_socket.h>
#endif

#if defined(CONFIG_AWS_FOTA)
#include <net/aws_fota.h>
#endif

LOG_MODULE_REGISTER(nrf_cloud_transport, CONFIG_NRF_CLOUD_LOG_LEVEL);

#if defined(CONFIG_NRF_CLOUD_PROVISION_CERTIFICATES)
#include CONFIG_NRF_CLOUD_CERTIFICATES_FILE
#if defined(CONFIG_MODEM_KEY_MGMT)
#include <modem/modem_key_mgmt.h>
#endif
#endif /* defined(CONFIG_NRF_CLOUD_PROVISION_CERTIFICATES) */

#if !defined(NRF_CLOUD_CLIENT_ID)
#define NRF_IMEI_LEN 15
#define NRF_CLOUD_CLIENT_ID_LEN (sizeof(CONFIG_NRF_CLOUD_CLIENT_ID_PREFIX) - 1 + NRF_IMEI_LEN)
#else
#define NRF_CLOUD_CLIENT_ID_LEN (sizeof(NRF_CLOUD_CLIENT_ID) - 1)
#endif

#define NRF_CLOUD_HOSTNAME CONFIG_NRF_CLOUD_HOST_NAME
#define NRF_CLOUD_PORT CONFIG_NRF_CLOUD_PORT

#if defined(CONFIG_NRF_CLOUD_IPV6)
#define NRF_CLOUD_AF_FAMILY AF_INET6
#else
#define NRF_CLOUD_AF_FAMILY AF_INET
#endif /* defined(CONFIG_NRF_CLOUD_IPV6) */

#define AWS "$aws/things/"
#define AWS_LEN (sizeof(AWS) - 1)

#define NCT_SHADOW_BASE_TOPIC AWS "%s/shadow"
#define NCT_SHADOW_BASE_TOPIC_LEN (AWS_LEN + NRF_CLOUD_CLIENT_ID_LEN + 7)

/*
 * Note that this topic is intentionally not using the AWS Shadow get/accepted
 * topic ("$aws/things/<deviceId>/shadow/get/accepted").
 * Messages on the AWS topic contain the entire shadow, including metadata and
 * they can become too large for the modem to handle.
 * Messages on the topic below are published by nRF Connect for Cloud and
 * contain only a part of the original message so it can be received by the
 * device.
 */
#define NCT_ACCEPTED_TOPIC "%s/shadow/get/accepted"
#define NCT_ACCEPTED_TOPIC_LEN (NRF_CLOUD_CLIENT_ID_LEN + 20)

#define NCT_REJECTED_TOPIC AWS "%s/shadow/get/rejected"
#define NCT_REJECTED_TOPIC_LEN (AWS_LEN + NRF_CLOUD_CLIENT_ID_LEN + 20)

#define NCT_UPDATE_DELTA_TOPIC AWS "%s/shadow/update/delta"
#define NCT_UPDATE_DELTA_TOPIC_LEN (AWS_LEN + NRF_CLOUD_CLIENT_ID_LEN + 20)

#define NCT_UPDATE_TOPIC AWS "%s/shadow/update"
#define NCT_UPDATE_TOPIC_LEN (AWS_LEN + NRF_CLOUD_CLIENT_ID_LEN + 14)

#define NCT_SHADOW_GET AWS "%s/shadow/get"
#define NCT_SHADOW_GET_LEN (AWS_LEN + NRF_CLOUD_CLIENT_ID_LEN + 11)

/* Buffer for keeping the client_id + \0 */
static char client_id_buf[NRF_CLOUD_CLIENT_ID_LEN + 1];
/* Buffers for keeping the topics for nrf_cloud */
static char shadow_base_topic[NCT_SHADOW_BASE_TOPIC_LEN + 1];
static char accepted_topic[NCT_ACCEPTED_TOPIC_LEN + 1];
static char rejected_topic[NCT_REJECTED_TOPIC_LEN + 1];
static char update_delta_topic[NCT_UPDATE_DELTA_TOPIC_LEN + 1];
static char update_topic[NCT_UPDATE_TOPIC_LEN + 1];
static char shadow_get_topic[NCT_SHADOW_GET_LEN + 1];

#if defined(CONFIG_AWS_FOTA)
#define NCT_M_D_TOPIC_PREFIX "m/d/"
#define NCT_TOPIC_PREFIX_M_D_LEN (sizeof(NCT_M_D_TOPIC_PREFIX) - 1)
#define NCT_JOB_STATUS_TOPIC "/jobs"
#define NCT_JOB_STATUS_TOPIC_LEN (sizeof(NCT_JOB_STATUS_TOPIC) - 1)
#define JOB_ID_LEN 8
/* FOTA status message: job id, space, % progress, null */
#define JOB_STATUS_STR_LEN (JOB_ID_LEN + 1 + 3 + 1)
char current_job_id[JOB_ID_LEN + 1];
static int last_sent_fota_progress;
#endif

static bool initialized;
static bool persistent_session;

#define NCT_CC_SUBSCRIBE_ID 1234
#define NCT_DC_SUBSCRIBE_ID 8765

#define NCT_RX_LIST 0
#define NCT_TX_LIST 1

static int nct_settings_set(const char *key, size_t len_rd,
			    settings_read_cb read_cb, void *cb_arg);

#define SETTINGS_NAME "nrf_cloud"
#define SETTINGS_KEY_PERSISTENT_SESSION "p_sesh"
#define SETTINGS_FULL_PERSISTENT_SESSION SETTINGS_NAME \
					 "/" \
					 SETTINGS_KEY_PERSISTENT_SESSION

SETTINGS_STATIC_HANDLER_DEFINE(nrf_cloud, SETTINGS_NAME, NULL, nct_settings_set,
			       NULL, NULL);

/* Forward declaration of the event handler registered with MQTT. */
static void nct_mqtt_evt_handler(struct mqtt_client *client,
				 const struct mqtt_evt *evt);

/* nrf_cloud transport instance. */
static struct nct {
	struct mqtt_sec_config tls_config;
	struct mqtt_client client;
	struct sockaddr_storage broker;
	struct mqtt_utf8 dc_tx_endp;
	struct mqtt_utf8 dc_rx_endp;
	struct mqtt_utf8 dc_m_endp;
	struct mqtt_utf8 job_status_endp;
	uint32_t message_id;
	uint8_t rx_buf[CONFIG_NRF_CLOUD_MQTT_MESSAGE_BUFFER_LEN];
	uint8_t tx_buf[CONFIG_NRF_CLOUD_MQTT_MESSAGE_BUFFER_LEN];
	uint8_t payload_buf[CONFIG_NRF_CLOUD_MQTT_PAYLOAD_BUFFER_LEN];
} nct;

static const struct mqtt_topic nct_cc_rx_list[] = {
	{
		.topic = {
			.utf8 = accepted_topic,
			.size = NCT_ACCEPTED_TOPIC_LEN
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE
	},
	{
		.topic = {
			.utf8 = rejected_topic,
			.size = NCT_REJECTED_TOPIC_LEN
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE
	},
	{
		.topic = {
			.utf8 = update_delta_topic,
			.size = NCT_UPDATE_DELTA_TOPIC_LEN
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE
	}
};

static const struct mqtt_topic nct_cc_tx_list[] = {
	{
		.topic = {
			.utf8 = shadow_get_topic,
			.size = NCT_SHADOW_GET_LEN
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE
	},
	{
		.topic = {
			.utf8 = update_topic,
			.size = NCT_UPDATE_TOPIC_LEN
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE
	}
};

static uint32_t const nct_cc_rx_opcode_map[] = {
	NCT_CC_OPCODE_UPDATE_REQ,
	NCT_CC_OPCODE_UPDATE_REJECT_RSP,
	NCT_CC_OPCODE_UPDATE_ACCEPT_RSP
};

/* Internal routine to reset data endpoint information. */
static void dc_endpoint_reset(void)
{
	nct.dc_rx_endp.utf8 = NULL;
	nct.dc_rx_endp.size = 0;

	nct.dc_tx_endp.utf8 = NULL;
	nct.dc_tx_endp.size = 0;

	nct.dc_m_endp.utf8 = NULL;
	nct.dc_m_endp.size = 0;

	nct.job_status_endp.utf8 = NULL;
	nct.job_status_endp.size = 0;
}

/* Get the next unused message id. */
static uint32_t dc_get_next_message_id(void)
{
	nct.message_id++;

	if ((uint16_t)nct.message_id == 0) {
		nct.message_id++;
	}

	return nct.message_id;
}

/* Free memory allocated for the data endpoint and reset the endpoint.
 *
 * Casting away const for rx, tx, and m seems to be OK because the
 * nct_dc_endpoint_set() caller gets the buffers from
 * json_decode_and_alloc(), which uses nrf_cloud_malloc() to call
 * k_malloc().
 *
 * The job_status_endp.utf8 buffer is allocated in this file as
 * non-const, so casting away const here is safe.
 */
static void dc_endpoint_free(void)
{
	if (nct.dc_rx_endp.utf8 != NULL) {
		nrf_cloud_free((void *)nct.dc_rx_endp.utf8);
	}
	if (nct.dc_tx_endp.utf8 != NULL) {
		nrf_cloud_free((void *)nct.dc_tx_endp.utf8);
	}
	if (nct.dc_m_endp.utf8 != NULL) {
		nrf_cloud_free((void *)nct.dc_m_endp.utf8);
	}
	if (nct.job_status_endp.utf8 != NULL) {
		nrf_cloud_free((void *)nct.job_status_endp.utf8);
	}
	dc_endpoint_reset();
}

static uint32_t dc_send(const struct nct_dc_data *dc_data, uint8_t qos)
{
	if (dc_data == NULL) {
		return -EINVAL;
	}

	struct mqtt_publish_param publish = {
		.message.topic.qos = qos,
		.message.topic.topic.size = nct.dc_tx_endp.size,
		.message.topic.topic.utf8 = nct.dc_tx_endp.utf8,
	};

	/* Populate payload. */
	if ((dc_data->data.len != 0) && (dc_data->data.ptr != NULL)) {
		publish.message.payload.data = (uint8_t *)dc_data->data.ptr;
		publish.message.payload.len = dc_data->data.len;
	}

	if (dc_data->id != 0) {
		publish.message_id = dc_data->id;
	} else {
		publish.message_id = dc_get_next_message_id();
	}

	return mqtt_publish(&nct.client, &publish);
}

static bool strings_compare(const char *s1, const char *s2, uint32_t s1_len,
			    uint32_t s2_len)
{
	return (strncmp(s1, s2, MIN(s1_len, s2_len))) ? false : true;
}

/* Verify if the topic is a control channel topic or not. */
static bool control_channel_topic_match(uint32_t list_id,
					const struct mqtt_topic *topic,
					enum nct_cc_opcode *opcode)
{
	struct mqtt_topic *topic_list;
	uint32_t list_size;

	if (list_id == NCT_RX_LIST) {
		topic_list = (struct mqtt_topic *)nct_cc_rx_list;
		list_size = ARRAY_SIZE(nct_cc_rx_list);
	} else if (list_id == NCT_TX_LIST) {
		topic_list = (struct mqtt_topic *)nct_cc_tx_list;
		list_size = ARRAY_SIZE(nct_cc_tx_list);
	} else {
		return false;
	}

	for (uint32_t index = 0; index < list_size; index++) {
		if (strings_compare(
			    topic->topic.utf8, topic_list[index].topic.utf8,
			    topic->topic.size, topic_list[index].topic.size)) {
			*opcode = nct_cc_rx_opcode_map[index];
			return true;
		}
	}
	return false;
}

/* Function to get the client id */
static int nct_client_id_get(char *id)
{
#if !defined(NRF_CLOUD_CLIENT_ID)
#if defined(CONFIG_NRF_MODEM_LIB)
	int at_socket_fd;
	int bytes_written;
	int bytes_read;
	char imei_buf[NRF_IMEI_LEN + 1];
	int ret;

	at_socket_fd = nrf_socket(NRF_AF_LTE, NRF_SOCK_DGRAM, NRF_PROTO_AT);
	__ASSERT_NO_MSG(at_socket_fd >= 0);

	bytes_written = nrf_write(at_socket_fd, "AT+CGSN", 7);
	__ASSERT_NO_MSG(bytes_written == 7);

	bytes_read = nrf_read(at_socket_fd, imei_buf, NRF_IMEI_LEN);
	__ASSERT_NO_MSG(bytes_read == NRF_IMEI_LEN);
	imei_buf[NRF_IMEI_LEN] = 0;

	snprintf(id, NRF_CLOUD_CLIENT_ID_LEN + 1, "%s%s", CONFIG_NRF_CLOUD_CLIENT_ID_PREFIX, imei_buf);

	ret = nrf_close(at_socket_fd);
	__ASSERT_NO_MSG(ret == 0);
#else
#error Missing NRF_CLOUD_CLIENT_ID
#endif /* defined(CONFIG_NRF_MODEM_LIB) */
#else
	memcpy(id, NRF_CLOUD_CLIENT_ID, NRF_CLOUD_CLIENT_ID_LEN + 1);
#endif /* !defined(NRF_CLOUD_CLIENT_ID) */

	LOG_DBG("client_id = %s", log_strdup(id));

	return 0;
}

static int nct_topics_populate(void)
{
	int ret;

	ret = nct_client_id_get(client_id_buf);
	if (ret != 0) {
		return ret;
	}

	ret = snprintf(shadow_base_topic, sizeof(shadow_base_topic),
		       NCT_SHADOW_BASE_TOPIC, client_id_buf);
	if (ret != NCT_SHADOW_BASE_TOPIC_LEN) {
		return -ENOMEM;
	}
	LOG_DBG("shadow_base_topic: %s", log_strdup(shadow_base_topic));

	ret = snprintf(accepted_topic, sizeof(accepted_topic),
		       NCT_ACCEPTED_TOPIC, client_id_buf);
	if (ret != NCT_ACCEPTED_TOPIC_LEN) {
		return -ENOMEM;
	}
	LOG_DBG("accepted_topic: %s", log_strdup(accepted_topic));

	ret = snprintf(rejected_topic, sizeof(rejected_topic),
		       NCT_REJECTED_TOPIC, client_id_buf);
	if (ret != NCT_REJECTED_TOPIC_LEN) {
		return -ENOMEM;
	}
	LOG_DBG("rejected_topic: %s", log_strdup(rejected_topic));

	ret = snprintf(update_delta_topic, sizeof(update_delta_topic),
		       NCT_UPDATE_DELTA_TOPIC, client_id_buf);
	if (ret != NCT_UPDATE_DELTA_TOPIC_LEN) {
		return -ENOMEM;
	}
	LOG_DBG("update_delta_topic: %s", log_strdup(update_delta_topic));

	ret = snprintf(update_topic, sizeof(update_topic), NCT_UPDATE_TOPIC,
		       client_id_buf);
	if (ret != NCT_UPDATE_TOPIC_LEN) {
		return -ENOMEM;
	}
	LOG_DBG("update_topic: %s", log_strdup(update_topic));

	ret = snprintf(shadow_get_topic, sizeof(shadow_get_topic),
		       NCT_SHADOW_GET, client_id_buf);
	if (ret != NCT_SHADOW_GET_LEN) {
		return -ENOMEM;
	}
	LOG_DBG("shadow_get_topic: %s", log_strdup(shadow_get_topic));

	return 0;
}

/* Provisions root CA certificate using modem_key_mgmt API */
static int nct_provision(void)
{
	static sec_tag_t sec_tag_list[] = { CONFIG_NRF_CLOUD_SEC_TAG };

	nct.tls_config.peer_verify = 2;
	nct.tls_config.cipher_count = 0;
	nct.tls_config.cipher_list = NULL;
	nct.tls_config.sec_tag_count = ARRAY_SIZE(sec_tag_list);
	nct.tls_config.sec_tag_list = sec_tag_list;
	nct.tls_config.hostname = NRF_CLOUD_HOSTNAME;

#if defined(CONFIG_NRF_CLOUD_PROVISION_CERTIFICATES)
#if defined(CONFIG_NRF_MODEM_LIB)
	{
		int err;

		/* Delete certificates */
		nrf_sec_tag_t sec_tag = CONFIG_NRF_CLOUD_SEC_TAG;

		for (enum modem_key_mgmt_cred_type type = 0; type < 5;
		     type++) {
			err = modem_key_mgmt_delete(sec_tag, type);
			LOG_DBG("modem_key_mgmt_delete(%u, %d) => result = %d",
				sec_tag, type, err);
		}

		/* Provision CA Certificate. */
		err = modem_key_mgmt_write(CONFIG_NRF_CLOUD_SEC_TAG,
					   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
					   NRF_CLOUD_CA_CERTIFICATE,
					   strlen(NRF_CLOUD_CA_CERTIFICATE));
		if (err) {
			LOG_ERR("NRF_CLOUD_CA_CERTIFICATE err: %d", err);
			return err;
		}

		/* Provision Private Certificate. */
		err = modem_key_mgmt_write(
			CONFIG_NRF_CLOUD_SEC_TAG,
			MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT,
			NRF_CLOUD_CLIENT_PRIVATE_KEY,
			strlen(NRF_CLOUD_CLIENT_PRIVATE_KEY));
		if (err) {
			LOG_ERR("NRF_CLOUD_CLIENT_PRIVATE_KEY err: %d", err);
			return err;
		}

		/* Provision Public Certificate. */
		err = modem_key_mgmt_write(
			CONFIG_NRF_CLOUD_SEC_TAG,
			MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
			NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE,
			strlen(NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE));
		if (err) {
			LOG_ERR("NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE err: %d",
				err);
			return err;
		}
	}
#else
	{
		int err;

		err = tls_credential_add(CONFIG_NRF_CLOUD_SEC_TAG,
					 TLS_CREDENTIAL_CA_CERTIFICATE,
					 NRF_CLOUD_CA_CERTIFICATE,
					 sizeof(NRF_CLOUD_CA_CERTIFICATE));
		if (err < 0) {
			LOG_ERR("Failed to register ca certificate: %d", err);
			return err;
		}
		err = tls_credential_add(CONFIG_NRF_CLOUD_SEC_TAG,
					 TLS_CREDENTIAL_PRIVATE_KEY,
					 NRF_CLOUD_CLIENT_PRIVATE_KEY,
					 sizeof(NRF_CLOUD_CLIENT_PRIVATE_KEY));
		if (err < 0) {
			LOG_ERR("Failed to register private key: %d", err);
			return err;
		}
		err = tls_credential_add(
			CONFIG_NRF_CLOUD_SEC_TAG,
			TLS_CREDENTIAL_SERVER_CERTIFICATE,
			NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE,
			sizeof(NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE));
		if (err < 0) {
			LOG_ERR("Failed to register public certificate: %d",
				err);
			return err;
		}
	}
#endif /* defined(CONFIG_NRF_MODEM_LIB) */
#endif /* defined(CONFIG_NRF_CLOUD_PROVISION_CERTIFICATES) */

	return 0;
}

#if defined(CONFIG_AWS_FOTA)
static int job_status_stream(const struct nct_dc_data *dc_data)
{
	if (dc_data == NULL) {
		return -EINVAL;
	}

	if (nct.job_status_endp.utf8 == NULL) {
		LOG_ERR("Job status topic not set");
		return -EACCES;
	}

	struct mqtt_publish_param publish = {
		.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE };

	publish.message.topic.topic.size = nct.job_status_endp.size;
	publish.message.topic.topic.utf8 = nct.job_status_endp.utf8;

	/* Populate payload. */
	if ((dc_data->data.len != 0) && (dc_data->data.ptr != NULL)) {
		publish.message.payload.data = (uint8_t *)dc_data->data.ptr;
		publish.message.payload.len = dc_data->data.len;
	}

	publish.message_id = 0;

	return mqtt_publish(&nct.client, &publish);
}

/* Handle AWS FOTA events */
static void aws_fota_cb_handler(struct aws_fota_event *fota_evt)
{
	if (fota_evt == NULL) {
		return;
	}

	char fota_status[JOB_STATUS_STR_LEN] = { 0 };
	struct nct_dc_data prog;
	int err;

	switch (fota_evt->id) {
	case AWS_FOTA_EVT_START:
		LOG_DBG("AWS_FOTA_EVT_START");
		if (aws_fota_get_job_id(current_job_id, sizeof(current_job_id))
			< JOB_ID_LEN) {
			LOG_ERR("Failed to get current job ID");
			current_job_id[0] = 0;
		}
		break;
	case AWS_FOTA_EVT_DONE:
		LOG_DBG("AWS_FOTA_EVT_DONE: rebooting to apply update");
		last_sent_fota_progress = 0;
		current_job_id[0] = 0;
		nct_apply_update();
		break;

	case AWS_FOTA_EVT_ERASE_PENDING:
		LOG_DBG("AWS_FOTA_EVT_ERASE_PENDING: rebooting");
		nct_apply_update();
		break;

	case AWS_FOTA_EVT_ERASE_DONE:
		LOG_DBG("AWS_FOTA_EVT_ERASE_DONE");
		break;

	case AWS_FOTA_EVT_ERROR:
		LOG_ERR("AWS_FOTA_EVT_ERROR");
		last_sent_fota_progress = 0;
		current_job_id[0] = 0;
		break;
	case AWS_FOTA_EVT_DL_PROGRESS:
		LOG_DBG("AWS_FOTA_EVT_DL_PROGRESS");
		if ((fota_evt->dl.progress < 0) ||
		    (fota_evt->dl.progress > AWS_FOTA_EVT_DL_COMPLETE_VAL)) {
			LOG_ERR("Invalid progress value %d",
				fota_evt->dl.progress);
		}
		/* Do not send complete status more than once */
		if ((last_sent_fota_progress == AWS_FOTA_EVT_DL_COMPLETE_VAL) &&
		    (fota_evt->dl.progress == AWS_FOTA_EVT_DL_COMPLETE_VAL)) {
			return;
		}

		/* Reset if new progress is less than previous */
		if (last_sent_fota_progress > fota_evt->dl.progress) {
			last_sent_fota_progress = 0;
		}

		/* Send dl complete status regardless of increment setting */
		/* Otherwise skip if increment is not met or disabled (0) */
#if defined(CONFIG_FOTA_DOWNLOAD_PROGRESS_EVT)
		if ((fota_evt->dl.progress < AWS_FOTA_EVT_DL_COMPLETE_VAL) &&
		    (((fota_evt->dl.progress - last_sent_fota_progress) <
		      CONFIG_NRF_CLOUD_FOTA_PROGRESS_PCT_INCREMENT) ||
		     (CONFIG_NRF_CLOUD_FOTA_PROGRESS_PCT_INCREMENT == 0))) {
			return;
		}
#endif

		if (current_job_id[0] == 0) {
			LOG_ERR("Invalid job ID, progress will not be sent");
			return;
		}

		prog.data.len =
			snprintf(fota_status, sizeof(fota_status), "%s %d",
				 current_job_id, fota_evt->dl.progress);
		if ((prog.data.len <= 0) ||
		    (prog.data.len >= sizeof(fota_status))) {
			LOG_ERR("Failed to create FOTA progress message");
			return;
		}

		prog.data.ptr = fota_status;
		LOG_DBG("Job status (ID/progress): %s",
			log_strdup(prog.data.ptr));
		err = job_status_stream(&prog);
		if (err) {
			LOG_ERR("job_status_stream failed %d", err);
			return;
		}

		last_sent_fota_progress = fota_evt->dl.progress;
		break;
	}
}
#endif /* defined(CONFIG_AWS_FOTA) */

static int nct_settings_set(const char *key, size_t len_rd,
			    settings_read_cb read_cb, void *cb_arg)
{
	if (!key) {
		return -EINVAL;
	}

	int read_val;

	LOG_DBG("Settings key: %s, size: %d", log_strdup(key), len_rd);

	if (!strncmp(key, SETTINGS_KEY_PERSISTENT_SESSION,
		     strlen(SETTINGS_KEY_PERSISTENT_SESSION)) &&
	    (len_rd == sizeof(read_val))) {
		if (read_cb(cb_arg, (void *)&read_val, len_rd) == len_rd) {
#if !IS_ENABLED(CONFIG_MQTT_CLEAN_SESSION)
			persistent_session = (bool)read_val;
#endif
			LOG_DBG("Read setting val: %d", read_val);
			return 0;
		}
	}
	return -ENOTSUP;
}

int save_session_state(const int session_valid)
{
	int ret = 0;

#if !IS_ENABLED(CONFIG_MQTT_CLEAN_SESSION)
	LOG_DBG("Setting session state: %d", session_valid);
	persistent_session = (bool)session_valid;
	ret = settings_save_one(SETTINGS_FULL_PERSISTENT_SESSION,
				&session_valid, sizeof(session_valid));
#endif
	return ret;
}

static int nct_settings_init(void)
{
	int ret = 0;

#if !IS_ENABLED(CONFIG_MQTT_CLEAN_SESSION)
	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("Settings init failed: %d", ret);
		return ret;
	}

	ret = settings_load_subtree(settings_handler_nrf_cloud.name);
	if (ret) {
		LOG_ERR("Cannot load settings: %d", ret);
	}
#else
	ARG_UNUSED(settings_handler_nrf_cloud);
#endif

	return ret;
}

/* Connect to MQTT broker. */
int nct_mqtt_connect(void)
{
	int err;
	if (!initialized) {

		mqtt_client_init(&nct.client);

		nct.client.broker = (struct sockaddr *)&nct.broker;
		nct.client.evt_cb = nct_mqtt_evt_handler;
		nct.client.client_id.utf8 = (uint8_t *)client_id_buf;
		nct.client.client_id.size = strlen(client_id_buf);
		nct.client.protocol_version = MQTT_VERSION_3_1_1;
		nct.client.password = NULL;
		nct.client.user_name = NULL;
		nct.client.clean_session = persistent_session ? 0U : 1U;
		LOG_DBG("MQTT clean session flag: %u",
			nct.client.clean_session);

#if defined(CONFIG_MQTT_LIB_TLS)
		nct.client.transport.type = MQTT_TRANSPORT_SECURE;
		nct.client.rx_buf = nct.rx_buf;
		nct.client.rx_buf_size = sizeof(nct.rx_buf);
		nct.client.tx_buf = nct.tx_buf;
		nct.client.tx_buf_size = sizeof(nct.tx_buf);

		struct mqtt_sec_config *tls_config =
			   &nct.client.transport.tls.config;
		memcpy(tls_config, &nct.tls_config,
			   sizeof(struct mqtt_sec_config));
#else
		nct.client.transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif
#if defined(CONFIG_AWS_FOTA)
		err = aws_fota_init(&nct.client, aws_fota_cb_handler);
		if (err != 0) {
			LOG_ERR("aws_fota_init failed %d", err);
			return -ENOEXEC;
		}
#endif /* defined(CONFIG_AWS_FOTA) */

		initialized = true;
	}

	err = mqtt_connect(&nct.client);
	if (err != 0) {
		LOG_DBG("mqtt_connect failed %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_NRF_CLOUD_SEND_NONBLOCKING)) {
		err = fcntl(nct_socket_get(), F_SETFL, O_NONBLOCK);
		if (err == -1) {
			LOG_ERR("Failed to set socket as non-blocking, err: %d",
				errno);
			LOG_WRN("Continuing with blocking socket");
			err = 0;
		} else {
			LOG_INF("Using non-blocking socket");
		}
	}  else if (IS_ENABLED(CONFIG_NRF_CLOUD_SEND_TIMEOUT)) {
		struct timeval timeout = {
			.tv_sec = CONFIG_NRF_CLOUD_SEND_TIMEOUT_SEC
		};

		err = setsockopt(nct_socket_get(), SOL_SOCKET, SO_SNDTIMEO,
				 &timeout, sizeof(timeout));
		if (err == -1) {
			LOG_ERR("Failed to set timeout, errno: %d", errno);
			err = 0;
		} else {
			LOG_INF("Using socket send timeout of %d seconds",
				CONFIG_NRF_CLOUD_SEND_TIMEOUT_SEC);
		}
	}

	return err;
}

static int publish_get_payload(struct mqtt_client *client, size_t length)
{
	if (length > sizeof(nct.payload_buf)) {
		return -EMSGSIZE;
	}

	return mqtt_readall_publish_payload(client, nct.payload_buf, length);
}

/* Handle MQTT events. */
static void nct_mqtt_evt_handler(struct mqtt_client *const mqtt_client,
				 const struct mqtt_evt *_mqtt_evt)
{
	int err;
	struct nct_evt evt = { .status = _mqtt_evt->result };
	struct nct_cc_data cc;
	struct nct_dc_data dc;
	bool event_notify = false;

#if defined(CONFIG_AWS_FOTA)
	err = aws_fota_mqtt_evt_handler(mqtt_client, _mqtt_evt);
	if (err == 0) {
		/* Event handled by FOTA library so we can skip it */
		return;
	} else if (err < 0) {
		LOG_ERR("aws_fota_mqtt_evt_handler: Failed! %d", err);
		LOG_DBG("Disconnecting MQTT client...");

		err = mqtt_disconnect(mqtt_client);
		if (err) {
			LOG_ERR("Could not disconnect: %d", err);
		}
	}
#endif /* defined(CONFIG_AWS_FOTA) */

	switch (_mqtt_evt->type) {
	case MQTT_EVT_CONNACK: {
		const struct mqtt_connack_param *p = &_mqtt_evt->param.connack;

		LOG_DBG("MQTT_EVT_CONNACK: result %d", _mqtt_evt->result);

		evt.param.flag = (p->session_present_flag != 0) &&
				 persistent_session;

		if (persistent_session && (p->session_present_flag == 0)) {
			/* Session not present, clear saved state */
			save_session_state(0);
		}

		evt.type = NCT_EVT_CONNECTED;
		event_notify = true;
		break;
	}
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &_mqtt_evt->param.publish;

		LOG_DBG("MQTT_EVT_PUBLISH: id = %d len = %d",
			p->message_id,
			p->message.payload.len);

		int err = publish_get_payload(mqtt_client,
					      p->message.payload.len);

		if (err < 0) {
			LOG_ERR("publish_get_payload: failed %d", err);
			mqtt_disconnect(mqtt_client);
			event_notify = false;
			break;
		}

		/* If the data arrives on one of the subscribed control channel
		 * topic. Then we notify the same.
		 */
		if (control_channel_topic_match(NCT_RX_LIST, &p->message.topic,
						&cc.opcode)) {
			cc.id = p->message_id;
			cc.data.ptr = nct.payload_buf;
			cc.data.len = p->message.payload.len;
			cc.topic.len = p->message.topic.topic.size;
			cc.topic.ptr = p->message.topic.topic.utf8;

			evt.type = NCT_EVT_CC_RX_DATA;
			evt.param.cc = &cc;
			event_notify = true;
		} else {
			/* Try to match it with one of the data topics. */
			dc.id = p->message_id;
			dc.data.ptr = nct.payload_buf;
			dc.data.len = p->message.payload.len;
			dc.topic.len = p->message.topic.topic.size;
			dc.topic.ptr = p->message.topic.topic.utf8;

			evt.type = NCT_EVT_DC_RX_DATA;
			evt.param.dc = &dc;
			event_notify = true;
		}

		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			const struct mqtt_puback_param ack = {
				.message_id = p->message_id
			};

			/* Send acknowledgment. */
			mqtt_publish_qos1_ack(mqtt_client, &ack);
		}
		break;
	}
	case MQTT_EVT_SUBACK: {
		LOG_DBG("MQTT_EVT_SUBACK: id = %d result = %d",
			_mqtt_evt->param.suback.message_id, _mqtt_evt->result);

		if (_mqtt_evt->param.suback.message_id == NCT_CC_SUBSCRIBE_ID) {
			evt.type = NCT_EVT_CC_CONNECTED;
			event_notify = true;
		}
		if (_mqtt_evt->param.suback.message_id == NCT_DC_SUBSCRIBE_ID) {
			evt.type = NCT_EVT_DC_CONNECTED;
			event_notify = true;

			/* Subscribing complete, session is now valid */
			err = save_session_state(1);
			if (err) {
				LOG_ERR("Failed to save session state: %d",
					err);
			}
		}
		break;
	}
	case MQTT_EVT_UNSUBACK: {
		LOG_DBG("MQTT_EVT_UNSUBACK");

		if (_mqtt_evt->param.suback.message_id == NCT_CC_SUBSCRIBE_ID) {
			evt.type = NCT_EVT_CC_DISCONNECTED;
			event_notify = true;
		}
		break;
	}
	case MQTT_EVT_PUBACK: {
		LOG_DBG("MQTT_EVT_PUBACK: id = %d result = %d",
			_mqtt_evt->param.puback.message_id, _mqtt_evt->result);

		evt.type = NCT_EVT_CC_TX_DATA_ACK;
		evt.param.data_id = _mqtt_evt->param.puback.message_id;
		event_notify = true;
		break;
	}
	case MQTT_EVT_DISCONNECT: {
		LOG_DBG("MQTT_EVT_DISCONNECT: result = %d", _mqtt_evt->result);

		evt.type = NCT_EVT_DISCONNECTED;
		event_notify = true;
		break;
	}
	default:
		break;
	}

	if (event_notify) {
		err = nct_input(&evt);

		if (err != 0) {
			LOG_ERR("nct_input: failed %d", err);
		}
	}
}

int nct_init(void)
{
	int err;

	dc_endpoint_reset();

	err = nct_topics_populate();
	if (err) {
		return err;
	}

	err = nct_settings_init();
	if (err) {
		return err;
	}

	return nct_provision();
}

#if defined(CONFIG_NRF_CLOUD_STATIC_IPV4)
int nct_connect(void)
{
	int err;

	struct sockaddr_in *broker = ((struct sockaddr_in *)&nct.broker);

	inet_pton(AF_INET, CONFIG_NRF_CLOUD_STATIC_IPV4_ADDR,
		  &broker->sin_addr);
	broker->sin_family = AF_INET;
	broker->sin_port = htons(NRF_CLOUD_PORT);

	LOG_DBG("IPv4 Address %s", CONFIG_NRF_CLOUD_STATIC_IPV4_ADDR);
	err = nct_mqtt_connect();

	return err;
}
#else
int nct_connect(void)
{
	int err;
	struct addrinfo *result;
	struct addrinfo *addr;
	struct addrinfo hints = {
		.ai_family = NRF_CLOUD_AF_FAMILY,
		.ai_socktype = SOCK_STREAM
	};

	err = getaddrinfo(NRF_CLOUD_HOSTNAME, NULL, &hints, &result);
	if (err) {
		LOG_DBG("getaddrinfo failed %d", err);
		return -ECHILD;
	}

	addr = result;
	err = -ECHILD;

	/* Look for address of the broker. */
	while (addr != NULL) {
		/* IPv4 Address. */
		if ((addr->ai_addrlen == sizeof(struct sockaddr_in)) &&
		    (NRF_CLOUD_AF_FAMILY == AF_INET)) {
			char addr_str[INET_ADDRSTRLEN];
			struct sockaddr_in *broker =
				((struct sockaddr_in *)&nct.broker);

			broker->sin_addr.s_addr =
				((struct sockaddr_in *)addr->ai_addr)
					->sin_addr.s_addr;
			broker->sin_family = AF_INET;
			broker->sin_port = htons(NRF_CLOUD_PORT);

			inet_ntop(AF_INET,
				 &broker->sin_addr.s_addr,
				 addr_str,
				 sizeof(addr_str));

			LOG_DBG("IPv4 address: %s", log_strdup(addr_str));

			err = nct_mqtt_connect();
			break;
		} else if ((addr->ai_addrlen == sizeof(struct sockaddr_in6)) &&
			   (NRF_CLOUD_AF_FAMILY == AF_INET6)) {
			/* IPv6 Address. */
			struct sockaddr_in6 *broker =
				((struct sockaddr_in6 *)&nct.broker);

			memcpy(broker->sin6_addr.s6_addr,
			       ((struct sockaddr_in6 *)addr->ai_addr)
				       ->sin6_addr.s6_addr,
			       sizeof(struct in6_addr));
			broker->sin6_family = AF_INET6;
			broker->sin6_port = htons(NRF_CLOUD_PORT);

			LOG_DBG("IPv6 Address");
			err = nct_mqtt_connect();
			break;
		} else {
			LOG_DBG("ai_addrlen = %u should be %u or %u",
				(unsigned int)addr->ai_addrlen,
				(unsigned int)sizeof(struct sockaddr_in),
				(unsigned int)sizeof(struct sockaddr_in6));
		}

		addr = addr->ai_next;
	}

	/* Free the address. */
	freeaddrinfo(result);

	return err;
}
#endif /* defined(CONFIG_NRF_CLOUD_STATIC_IPV4) */

int nct_cc_connect(void)
{
	LOG_DBG("nct_cc_connect");

	const struct mqtt_subscription_list subscription_list = {
		.list = (struct mqtt_topic *)&nct_cc_rx_list,
		.list_count = ARRAY_SIZE(nct_cc_rx_list),
		.message_id = NCT_CC_SUBSCRIBE_ID
	};

	return mqtt_subscribe(&nct.client, &subscription_list);
}

int nct_cc_send(const struct nct_cc_data *cc_data)
{
	static uint32_t msg_id;

	if (cc_data == NULL) {
		LOG_ERR("cc_data == NULL");
		return -EINVAL;
	}

	if (cc_data->opcode >= ARRAY_SIZE(nct_cc_tx_list)) {
		LOG_ERR("opcode = %d", cc_data->opcode);
		return -ENOTSUP;
	}

	struct mqtt_publish_param publish = {
		.message.topic.qos = nct_cc_tx_list[cc_data->opcode].qos,
		.message.topic.topic.size =
			nct_cc_tx_list[cc_data->opcode].topic.size,
		.message.topic.topic.utf8 =
			nct_cc_tx_list[cc_data->opcode].topic.utf8,
	};

	/* Populate payload. */
	if ((cc_data->data.len != 0) && (cc_data->data.ptr != NULL)) {
		publish.message.payload.data = (uint8_t *)cc_data->data.ptr,
		publish.message.payload.len = cc_data->data.len;
	}

	publish.message_id = cc_data->id ? cc_data->id : ++msg_id;

	LOG_DBG("mqtt_publish: id = %d opcode = %d len = %d", publish.message_id,
		cc_data->opcode, cc_data->data.len);

	int err = mqtt_publish(&nct.client, &publish);

	if (err) {
		LOG_ERR("mqtt_publish failed %d", err);
	}

	return err;
}

int nct_cc_disconnect(void)
{
	LOG_DBG("nct_cc_disconnect");

	static const struct mqtt_subscription_list subscription_list = {
		.list = (struct mqtt_topic *)nct_cc_rx_list,
		.list_count = ARRAY_SIZE(nct_cc_rx_list),
		.message_id = NCT_CC_SUBSCRIBE_ID
	};

	return mqtt_unsubscribe(&nct.client, &subscription_list);
}

void nct_dc_endpoint_set(const struct nrf_cloud_data *tx_endp,
			 const struct nrf_cloud_data *rx_endp,
			 const struct nrf_cloud_data *m_endp)
{
	LOG_DBG("nct_dc_endpoint_set");

	/* In case the endpoint was previous set, free and reset
	 * before copying new one.
	 */
	dc_endpoint_free();

	nct.dc_tx_endp.utf8 = (const uint8_t *)tx_endp->ptr;
	nct.dc_tx_endp.size = tx_endp->len;

	nct.dc_rx_endp.utf8 = (const uint8_t *)rx_endp->ptr;
	nct.dc_rx_endp.size = rx_endp->len;

	if (m_endp != NULL) {
		nct.dc_m_endp.utf8 = (const uint8_t *)m_endp->ptr;
		nct.dc_m_endp.size = m_endp->len;

#if defined(CONFIG_AWS_FOTA)
		void *job_status_utf8;
		int ret;

		nct.job_status_endp.size =
			nct.dc_m_endp.size + NCT_TOPIC_PREFIX_M_D_LEN +
			NRF_CLOUD_CLIENT_ID_LEN + NCT_JOB_STATUS_TOPIC_LEN + 1;
		job_status_utf8 = nrf_cloud_malloc(nct.job_status_endp.size);
		if (job_status_utf8 == NULL) {
			LOG_ERR("Failed to allocate mem for job status topic");
			nct.job_status_endp.utf8 = NULL;
			nct.job_status_endp.size = 0;
			return;
		}
		ret = snprintf(job_status_utf8,
			       nct.job_status_endp.size, "%s%s%s%s",
			       nct.dc_m_endp.utf8, NCT_M_D_TOPIC_PREFIX,
			       client_id_buf, NCT_JOB_STATUS_TOPIC);
		if ((ret <= 0) || (ret >= nct.job_status_endp.size)) {
			nrf_cloud_free(job_status_utf8);
			nct.job_status_endp.utf8 = NULL;
			nct.job_status_endp.size = 0;
			LOG_ERR("Failed to build job status topic");
			return;
		}
		nct.job_status_endp.utf8 = (const uint8_t *)job_status_utf8;
		/* size is actually string length */
		nct.job_status_endp.size = ret;
#endif
	}
}

void nct_dc_endpoint_get(struct nrf_cloud_data *const tx_endp,
			 struct nrf_cloud_data *const rx_endp,
			 struct nrf_cloud_data *const m_endp)
{
	LOG_DBG("nct_dc_endpoint_get");

	tx_endp->ptr = nct.dc_tx_endp.utf8;
	tx_endp->len = nct.dc_tx_endp.size;

	rx_endp->ptr = nct.dc_rx_endp.utf8;
	rx_endp->len = nct.dc_rx_endp.size;

	if (m_endp != NULL) {
		m_endp->ptr = nct.dc_m_endp.utf8;
		m_endp->len = nct.dc_m_endp.size;
	}
}

int nct_dc_connect(void)
{
	LOG_DBG("nct_dc_connect");

	struct mqtt_topic subscribe_topic = {
		.topic = {
			.utf8 = nct.dc_rx_endp.utf8,
			.size = nct.dc_rx_endp.size
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE
	};

	const struct mqtt_subscription_list subscription_list = {
		.list = &subscribe_topic,
		.list_count = 1,
		.message_id = NCT_DC_SUBSCRIBE_ID
	};

	return mqtt_subscribe(&nct.client, &subscription_list);
}

int nct_dc_send(const struct nct_dc_data *dc_data)
{
	return dc_send(dc_data, MQTT_QOS_1_AT_LEAST_ONCE);
}

int nct_dc_stream(const struct nct_dc_data *dc_data)
{
	return dc_send(dc_data, MQTT_QOS_0_AT_MOST_ONCE);
}

int nct_dc_disconnect(void)
{
	LOG_DBG("nct_dc_disconnect");

	const struct mqtt_subscription_list subscription_list = {
		.list = (struct mqtt_topic *)&nct.dc_rx_endp,
		.list_count = 1,
		.message_id = NCT_DC_SUBSCRIBE_ID
	};

	return mqtt_unsubscribe(&nct.client, &subscription_list);
}

int nct_disconnect(void)
{
	LOG_DBG("nct_disconnect");

	dc_endpoint_free();
	return mqtt_disconnect(&nct.client);
}

void nct_process(void)
{
	mqtt_input(&nct.client);
	mqtt_live(&nct.client);
}

int nct_keepalive_time_left(void)
{
	return (int)mqtt_keepalive_time_left(&nct.client);
}

int nct_socket_get(void)
{
	return nct.client.transport.tls.sock;
}
