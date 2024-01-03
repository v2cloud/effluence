#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <curl/curl.h>

#include "module.h"

#include "configuration.h"
#include "utils.h"

CURL	*hnd = NULL;

int	zbx_module_api_version(void)
{
	return ZBX_MODULE_API_VERSION;
}

int	zbx_module_init(void)
{
	const char	efflu_config[] = "EFFLU_CONFIG", *config_file_name;
	FILE		*config_file;
	int		ret = ZBX_MODULE_OK;

	printf("Initializing \"effluence\" module...\n");

	if (NULL == (config_file_name = secure_getenv(efflu_config)))
	{
		printf("Path to configuration file must be set using \"%s\" environment variable.\n", efflu_config);
		return ZBX_MODULE_FAIL;
	}

	printf("Reading configuration from \"%s\"...\n", config_file_name);

	if (NULL == (config_file = fopen(config_file_name, "r")))
	{
		printf("Failure to open \"%s\": %s.\n", config_file_name, strerror(errno));
		return ZBX_MODULE_FAIL;
	}

	if (0 != efflu_parse_configuration(config_file))
	{
		printf("Failure to read configuration from \"%s\".\n", config_file_name);
		ret = ZBX_MODULE_FAIL;
	}

	if (0 != fclose(config_file))
		printf("Failure to close \"%s\": %s.\n", config_file_name, strerror(errno));

	/* TODO ping InfluxDB to see if it's up */

	if (ZBX_MODULE_OK == ret)
		printf("Module \"effluence\" has been successfully initialized.\n");

	return ret;
}

int	zbx_module_uninit(void)
{
	printf("Uninitializing \"effluence\" module...\n");

	printf("Cleaning up cURL handle...\n");

	if (NULL != hnd)
		curl_easy_cleanup(hnd);

	printf("Cleaning up configuration data...\n");

	efflu_clean_configuration();

	printf("Module \"effluence\" has been successfully uninitialized.\n");

	return ZBX_MODULE_OK;
}

static char	*efflu_escape(const char *string)
{
	size_t		size_esc;
	const char	*p;
	char		*string_esc, *q;

	p = string;
	size_esc = 0;

	do
	{
		if ('\"' == *p || '\\' == *p)
			size_esc++;

		size_esc++;
	}
	while ('\0' != *p++);

	p = string;
	q = string_esc = malloc(size_esc);	/* TODO malloc() may return NULL, handle that */

	do
	{
		if ('\"' == *p || '\\' == *p)
			*q++ = '\\';
	}
	while ('\0' != (*q++ = *p++));
	
	return string_esc;
}

/* To avoid confusion data is split into measurements according to Zabbix data types, measurements are named  */
/* after Zabbix DB tables: history, history_uint, history_str, history_text, history_log. Actually, there is  */
/* no real difference in how "Numeric (float)" and "Numeric (unsigned)" or "Character" and "Text" are stored. */

static void	efflu_append_FLOAT_line(char **post, size_t *size, size_t *offset, const ZBX_HISTORY_FLOAT *data)
{
	efflu_strappf(post, size, offset, "history,itemid=" ZBX_FS_UI64 " value=%f %" PRIu64 "\n",
			data->itemid, data->value, (uint64_t)data->clock * 1000000000L + data->ns);
}

static void	efflu_append_INTEGER_line(char **post, size_t *size, size_t *offset, const ZBX_HISTORY_INTEGER *data)
{
	/* 64 bit unsigned integers used by Zabbix won't necessarily fit into 64 bit signed integer used by InfluxDB */
	/* To avoid overflows we trade off some accuracy by sending integers as floats (without "i" after the value) */
	efflu_strappf(post, size, offset, "history_uint,itemid=" ZBX_FS_UI64 " value=" ZBX_FS_UI64 " %" PRIu64 "\n",
			data->itemid, data->value, (uint64_t)data->clock * 1000000000L + data->ns);
}

static void	efflu_append_STRING_line(char **post, size_t *size, size_t *offset, const ZBX_HISTORY_STRING *data)
{
	char	*value_esc;

	value_esc = efflu_escape(data->value);
	efflu_strappf(post, size, offset, "history_str,itemid=" ZBX_FS_UI64 " value=\"%s\" %" PRIu64 "\n",
			data->itemid, value_esc, (uint64_t)data->clock * 1000000000L + data->ns);
	free(value_esc);
}

static void	efflu_append_TEXT_line(char **post, size_t *size, size_t *offset, const ZBX_HISTORY_TEXT *data)
{
	char	*value_esc;

	value_esc = efflu_escape(data->value);
	efflu_strappf(post, size, offset, "history_text,itemid=" ZBX_FS_UI64 " value=\"%s\" %" PRIu64 "\n",
			data->itemid, value_esc, (uint64_t)data->clock * 1000000000L + data->ns);
	free(value_esc);
}

static void	efflu_append_LOG_line(char **post, size_t *size, size_t *offset, const ZBX_HISTORY_LOG *data)
{
	char	*value_esc, *source_esc;

	value_esc = efflu_escape(data->value);
	source_esc = efflu_escape(data->source);
	efflu_strappf(post, size, offset, "history_log,itemid=" ZBX_FS_UI64 " value=\"%s\",source=\"%s\",timestamp=%d,"
			"logeventid=%d,severity=%d %" PRIu64 "\n",
			data->itemid, value_esc, source_esc, data->timestamp, data->logeventid, data->severity,
			(uint64_t)data->clock * 1000000000L + data->ns);
	free(value_esc);
	free(source_esc);
}

static void	efflu_write(efflu_destination_t destination, const char *post)
{
	char		*db_encoded, *endpoint, *token = NULL;
	CURLcode	ret;
	long		res;

	if (NULL == hnd && (NULL == (hnd = curl_easy_init())))
	{
		printf("Failed to initialize cURL handle.\n");
		return;
	}

    CURL *curl;
	curl = curl_easy_init();

	if (!curl) {
	    printf("Error: Failed to initialize libcurl\n");
	}

	if(curl) {

	    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");

	    if(-1 == asprintf(&endpoint, "%s/api/v2/write?org=%s&bucket=%s", destination.url, destination.org, destination.bucket)) {
	        printf("Failed to compose write endpoint URL.\n");
	    }

	    else if(-1 == asprintf(&token, "Authorization: Token %s", destination.token)) {
	        printf("Failed to set token authorization\n");
	    }

	    else if (CURLE_OK != (ret = curl_easy_setopt(curl, CURLOPT_URL, endpoint))) {
	        printf("Error setting CURLOPT_URL: %s\n", curl_easy_strerror(ret));
	    }

	    else if (CURLE_OK != (ret = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L))) {
	        printf("Error setting CURLOPT_FOLLOWLOCATION: %s\n", curl_easy_strerror(ret));
	    }

	    else if (CURLE_OK != (ret =curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https"))) {
	        printf("Error setting CURLOPT_DEFAULT_PROTOCOL: %s\n", curl_easy_strerror(ret));
	    }

	    struct curl_slist *headers = NULL;
	    headers = curl_slist_append(headers, token);
	    headers = curl_slist_append(headers, "Content-Type: text/plain; charset=utf-8");
	    headers = curl_slist_append(headers, "Accept: application/json");

	    if (CURLE_OK != (ret = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers))) {
	        printf("Error setting CURLOPT_HTTPHEADER: %s\n", curl_easy_strerror(ret));
	    }

	    else if (CURLE_OK != (ret = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post))) {
	        printf("Error setting CURLOPT_POSTFIELDS: %s\n", curl_easy_strerror(ret));
	    }

	    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);
	    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10);

	    ret = curl_easy_perform(curl);

	    if (ret == CURLE_OPERATION_TIMEDOUT) {
	        printf("Error: Connection timed out\n");
	    }
	    else if(ret != CURLE_OK) {
	        printf("Error in curl_easy_perform: %s\n", curl_easy_strerror(ret));
	    }

	    curl_slist_free_all(headers);
	    free(endpoint);
	    free(token);
	    curl_easy_cleanup(curl);
	}

}

#define EFFLU_CALLBACK(data_type)	\
static void	efflu_##data_type##_cb(const ZBX_HISTORY_##data_type *history, int history_num)	\
{												\
	int	i;										\
	char	*post = NULL;									\
	size_t	size = 0, offset = 0;								\
												\
	for (i = 0; i < history_num; i++)							\
	{											\
		efflu_append_##data_type##_line(&post, &size, &offset, history++);		\
												\
		if (NULL == post)								\
		{										\
			printf("Failed to allocate memory for data to POST.\n");		\
			return;									\
		}										\
	}											\
												\
	efflu_write(efflu_configured_destination(EFFLU_DATA_TYPE_##data_type), post);		\
	free(post);										\
}

EFFLU_CALLBACK(FLOAT)
EFFLU_CALLBACK(INTEGER)
EFFLU_CALLBACK(STRING)
EFFLU_CALLBACK(TEXT)
EFFLU_CALLBACK(LOG)

ZBX_HISTORY_WRITE_CBS	zbx_module_history_write_cbs(void)
{
	static ZBX_HISTORY_WRITE_CBS	callbacks;
	efflu_data_type_t		type = 0;

	do
	{
		efflu_destination_t	destination;

		destination = efflu_configured_destination(type);

		if (NULL == destination.url || NULL == destination.bucket)
		{
			printf("History of type \"%s\" will not be exported.\n", efflu_data_type_string(type));
			continue;
		}

		printf("History of type \"%s\" will be exported to URL \"%s\" and database \"%s\".\n",
				efflu_data_type_string(type), destination.url, destination.bucket);

		switch (type)
		{
			case EFFLU_DATA_TYPE_FLOAT:
				callbacks.history_float_cb = efflu_FLOAT_cb;
				break;
			case EFFLU_DATA_TYPE_INTEGER:
				callbacks.history_integer_cb = efflu_INTEGER_cb;
				break;
			case EFFLU_DATA_TYPE_STRING:
				callbacks.history_string_cb = efflu_STRING_cb;
				break;
			case EFFLU_DATA_TYPE_TEXT:
				callbacks.history_text_cb = efflu_TEXT_cb;
				break;
			case EFFLU_DATA_TYPE_LOG:
				callbacks.history_log_cb = efflu_LOG_cb;
				break;
			case EFFLU_DATA_TYPE_COUNT:
				printf("Internal error. Execution should never reach this code.\n");
		}
	}
	while (EFFLU_DATA_TYPE_COUNT > ++type);

	return callbacks;
}
