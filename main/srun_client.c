#include "srun_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "wifi_manager.h"
#include "web_config.h"
#include "mbedtls/md.h"
#include "mbedtls/sha1.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static const char *TAG = "SRUN";
#define SRUN_BODY_MAX 8192
#define SRUN_SLEEP_INTERVAL_US (120ULL * 1000000ULL)
#define SRUN_WIFI_TIMEOUT_MS 30000
#define SRUN_CONFIG_WINDOW_MS 120000
static char s_client_ip[16];

typedef struct { char *data; size_t size; size_t capacity; } response_buf_t;

static esp_err_t body_handler(esp_http_client_event_t *event)
{
    response_buf_t *buf = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !buf || event->data_len <= 0) return ESP_OK;
    if (buf->size + event->data_len + 1 > SRUN_BODY_MAX) return ESP_ERR_NO_MEM;
    if (buf->size + event->data_len + 1 > buf->capacity) {
        size_t capacity = buf->capacity ? buf->capacity * 2 : 1024;
        while (capacity < buf->size + event->data_len + 1) capacity *= 2;
        char *data = realloc(buf->data, capacity);
        if (!data) return ESP_ERR_NO_MEM;
        buf->data = data;
        buf->capacity = capacity;
    }
    memcpy(buf->data + buf->size, event->data, event->data_len);
    buf->size += event->data_len;
    buf->data[buf->size] = '\0';
    return ESP_OK;
}

static char *http_get(const char *url, int *status)
{
    response_buf_t buf = {0};
    esp_http_client_config_t config = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 10000,
        .event_handler = body_handler, .user_data = &buf,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 ESP32-C3");
    esp_err_t err = esp_http_client_perform(client);
    if (status) *status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    if (err != ESP_OK) { free(buf.data); return NULL; }
    if (!buf.data) buf.data = calloc(1, 1);
    return buf.data;
}

static char *url_encode(const char *input)
{
    const char hex[] = "0123456789ABCDEF";
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p; p++)
        len += ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') ? 1 : 3;
    char *output = malloc(len + 1);
    if (!output) return NULL;
    char *q = output;
    for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') *q++ = (char)*p;
        else { *q++ = '%'; *q++ = hex[*p >> 4]; *q++ = hex[*p & 15]; }
    }
    *q = '\0';
    return output;
}

static char *jsonp_json(char *body)
{
    char *start = strchr(body, '(');
    char *end = strrchr(body, ')');
    if (!start || !end || end <= start) return NULL;
    *end = '\0';
    return start + 1;
}

static char *json_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return item && cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static bool hmac_md5_hex(const char *key, const char *text, char output[33])
{
    unsigned char digest[16];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!info || mbedtls_md_hmac(info, (const unsigned char *)key, strlen(key),
                                  (const unsigned char *)text, strlen(text), digest) != 0) return false;
    for (int i = 0; i < 16; i++) snprintf(output + i * 2, 3, "%02x", digest[i]);
    output[32] = '\0';
    return true;
}

static bool sha1_hex(const char *text, char output[41])
{
    unsigned char digest[20];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
                   (const unsigned char *)text, strlen(text), digest) != 0) return false;
    for (int i = 0; i < 20; i++) snprintf(output + i * 2, 3, "%02x", digest[i]);
    output[40] = '\0';
    return true;
}

static void write_u32(unsigned char *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static size_t xencode(const unsigned char *input, size_t input_len, const char *key, unsigned char **output)
{
    size_t n = (input_len + 3) / 4 + 1;
    uint32_t *v = calloc(n + 1, sizeof(uint32_t)), k[4] = {0};
    if (!v) return 0;
    for (size_t i = 0; i < input_len; i++) v[i / 4] |= (uint32_t)input[i] << ((i % 4) * 8);
    v[n - 1] = (uint32_t)input_len;
    for (size_t i = 0; i < strlen(key) && i / 4 < 4; i++) k[i / 4] |= (uint32_t)(unsigned char)key[i] << ((i % 4) * 8);
    if (n < 2) { free(v); return 0; }
    uint32_t z = v[n - 1], y, sum = 0, e, mx;
    for (unsigned int rounds = 6 + 52 / (unsigned int)n; rounds; rounds--) {
        sum += 0x9E3779B9; e = (sum >> 2) & 3;
        for (size_t p = 0; p < n - 1; p++) {
            y = v[p + 1]; mx = (z >> 5 ^ y << 2) + ((y >> 3 ^ z << 4) ^ (sum ^ y));
            mx += k[(p & 3) ^ e] ^ z;
            v[p] += mx; z = v[p];
        }
        y = v[0]; mx = (z >> 5 ^ y << 2) + ((y >> 3 ^ z << 4) ^ (sum ^ y));
        mx += k[((n - 1) & 3) ^ e] ^ z;
        v[n - 1] += mx; z = v[n - 1];
    }
    *output = malloc(n * 4);
    if (!*output) { free(v); return 0; }
    for (size_t i = 0; i < n; i++) write_u32(*output + i * 4, v[i]);
    free(v);
    return n * 4;
}

static char *srun_base64(const unsigned char *data, size_t len)
{
    static const char alphabet[] = "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA";
    char *out = malloc(((len + 2) / 3) * 4 + 1); size_t j = 0;
    if (!out) return NULL;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t value = data[i] << 16;
        if (i + 1 < len) value |= data[i + 1] << 8;
        if (i + 2 < len) value |= data[i + 2];
        out[j++] = alphabet[(value >> 18) & 63]; out[j++] = alphabet[(value >> 12) & 63];
        out[j++] = i + 1 < len ? alphabet[(value >> 6) & 63] : '=';
        out[j++] = i + 2 < len ? alphabet[value & 63] : '=';
    }
    out[j] = '\0'; return out;
}

static bool srun_request(const char *server, const char *path, const char *query, cJSON **json)
{
    char *url = NULL;
    int status = 0;
    if (asprintf(&url, "http://%s/%s?%s&callback=jQueryESP32&_=%lld",
                 server, path, query, (long long)esp_timer_get_time()) < 0) return false;
    ESP_LOGI(TAG, "HTTP GET %s", path);
    char *body = http_get(url, &status);
    free(url);
    ESP_LOGI(TAG, "HTTP 返回 status=%d body=%u", status, body ? (unsigned)strlen(body) : 0U);
    if (!body || status != 200) { free(body); return false; }
    char *json_text = body[0] == '{' ? body : jsonp_json(body);
    if (!json_text) { free(body); return false; }
    *json = cJSON_Parse(json_text); free(body);
    return *json != NULL;
}

static bool srun_login(const app_config_t *cfg)
{
    ESP_LOGI(TAG, "开始 challenge 请求");
    char ip[16] = {0};
    if (cfg->auth_ip[0]) snprintf(ip, sizeof(ip), "%s", cfg->auth_ip);
    char username[96]; snprintf(username, sizeof(username), "%s%s%s", cfg->campus_username,
                                 cfg->user_type[0] ? "@" : "", cfg->user_type);
    char *u = url_encode(username), *i = url_encode(ip); if (!u || !i) { free(u); free(i); return false; }
    char query[512]; snprintf(query, sizeof(query), "username=%s&ip=%s", u, i); free(u); free(i);
    cJSON *challenge = NULL;
    if (!srun_request(cfg->auth_server, "cgi-bin/get_challenge", query, &challenge)) return false;
    ESP_LOGI(TAG, "challenge 响应已收到");
    char *token = json_string(challenge, "challenge"); if (!token) { cJSON_Delete(challenge); return false; }
    char *server_ip = json_string(challenge, "client_ip");
    /* The portal's client_ip is authoritative for the checksum and login. */
    if (server_ip && strlen(server_ip) < sizeof(ip))
        snprintf(ip, sizeof(ip), "%s", server_ip);
    if (!ip[0] || strlen(token) > 128) { cJSON_Delete(challenge); return false; }
    char *token_copy = strdup(token); cJSON_Delete(challenge); if (!token_copy) return false;

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "username", username); cJSON_AddStringToObject(info, "password", cfg->campus_password);
    cJSON_AddStringToObject(info, "ip", ip); cJSON_AddStringToObject(info, "acid", cfg->ac_id[0] ? cfg->ac_id : "3");
    cJSON_AddStringToObject(info, "enc_ver", "srun_bx1");
    char *info_json = cJSON_PrintUnformatted(info); cJSON_Delete(info); if (!info_json) { free(token_copy); return false; }
    unsigned char *encoded = NULL; size_t encoded_len = xencode((unsigned char *)info_json, strlen(info_json), token_copy, &encoded);
    free(info_json); if (!encoded_len) { free(token_copy); return false; }
    char *encoded_info = srun_base64(encoded, encoded_len); free(encoded); if (!encoded_info) { free(token_copy); return false; }
    char md5[33] = {0}, checksum[41] = {0}, info_field[1400];
    bool ok = hmac_md5_hex(token_copy, cfg->campus_password, md5);
    snprintf(info_field, sizeof(info_field), "{SRBX1}%s", encoded_info); free(encoded_info);
    char checksum_input[1900]; snprintf(checksum_input, sizeof(checksum_input), "%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
        token_copy, username, token_copy, md5, token_copy, cfg->ac_id[0] ? cfg->ac_id : "3", token_copy, ip,
        token_copy, "200", token_copy, "1", token_copy, info_field);
    ok = ok && sha1_hex(checksum_input, checksum);
    char password_field[40]; snprintf(password_field, sizeof(password_field), "{MD5}%s", md5);
    char *eu = url_encode(username), *ep = url_encode(password_field), *eacid = url_encode(cfg->ac_id[0] ? cfg->ac_id : "3");
    char *eip = url_encode(ip), *einfo = url_encode(info_field), *echk = url_encode(checksum);
    if (!eu || !ep || !eacid || !eip || !einfo || !echk) ok = false;
    char login_query[3500];
    if (ok) snprintf(login_query, sizeof(login_query), "action=login&username=%s&password=%s&ac_id=%s&ip=%s&info=%s&chksum=%s&n=200&type=1&os=Linux&name=ESP32-C3&double_stack=0", eu, ep, eacid, eip, einfo, echk);
    cJSON *result = NULL; if (ok) ok = srun_request(cfg->auth_server, "cgi-bin/srun_portal", login_query, &result);
    char *error = result ? json_string(result, "error") : NULL;
    char *res = result ? json_string(result, "res") : NULL;
    char *ecode = result ? json_string(result, "ecode") : NULL;
    bool accepted = ok && ((error && strcmp(error, "ok") == 0) ||
                           (res && strcmp(res, "ok") == 0) ||
                           (error && strcmp(error, "ip_already_online_error") == 0));
    if (ok && !accepted) {
        ESP_LOGW(TAG, "深澜登录失败 error=%s res=%s ecode=%s",
                 error ? error : "-", res ? res : "-", ecode ? ecode : "-");
    }
    ok = accepted;
    if (result) cJSON_Delete(result);
    free(token_copy); free(eu); free(ep); free(eacid); free(eip); free(einfo); free(echk);
    return ok;
}

static bool srun_online(const app_config_t *cfg)
{
    cJSON *root = NULL; bool online = false; char query[96]; snprintf(query, sizeof(query), "_=%lu", (unsigned long)esp_timer_get_time());
    if (srun_request(cfg->auth_server, "cgi-bin/rad_user_info", query, &root)) {
        char *error = json_string(root, "error");
        char *client_ip = json_string(root, "client_ip");
        if (!client_ip) client_ip = json_string(root, "online_ip");
        if (client_ip && strlen(client_ip) < sizeof(s_client_ip)) snprintf(s_client_ip, sizeof(s_client_ip), "%s", client_ip);
        char *online_ip = json_string(root, "online_ip");
        char *user_name = json_string(root, "user_name");
        online = (online_ip && online_ip[0] && user_name && user_name[0]) ||
                 (error && strcmp(error, "ok") == 0);
        if (online && user_name) {
            char expected[96];
            snprintf(expected, sizeof(expected), "%s%s%s", cfg->campus_username,
                     cfg->user_type[0] ? "@" : "", cfg->user_type);
            char base[64];
            snprintf(base, sizeof(base), "%s", cfg->campus_username);
            char *at = strchr(base, '@');
            if (at) *at = '\0';
            bool account_matches = expected[0] && strcmp(user_name, expected) == 0;
            if (!account_matches && base[0]) account_matches = strcmp(user_name, base) == 0;
            if (!account_matches) {
                ESP_LOGW(TAG, "深澜当前账号与配置不一致，当前账号=%s", user_name);
                online = false;
            }
        }
        cJSON_Delete(root);
    }
    return online;
}

static void srun_task(void *arg)
{
    app_config_t cfg = *(app_config_t *)arg; free(arg);
    int64_t config_deadline_us = esp_timer_get_time() + (int64_t)SRUN_CONFIG_WINDOW_MS * 1000;
    bool timer_wakeup = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    if (!timer_wakeup) ESP_LOGI(TAG, "重启后保留2分钟配置窗口");
    if (wifi_wait_connected(SRUN_WIFI_TIMEOUT_MS)) {
        if (!srun_online(&cfg)) { ESP_LOGI(TAG, "未认证，开始深澜登录"); if (srun_login(&cfg)) ESP_LOGI(TAG, "深澜认证成功"); else ESP_LOGW(TAG, "深澜认证失败，下次唤醒后重试"); }
        else ESP_LOGI(TAG, "深澜在线");
    } else {
        ESP_LOGW(TAG, "WiFi 连接超时，下次唤醒后重试");
    }
    int64_t remaining_us = config_deadline_us - esp_timer_get_time();
    if (!timer_wakeup && remaining_us > 0) vTaskDelay(pdMS_TO_TICKS((uint32_t)((remaining_us + 999) / 1000)));
    ESP_ERROR_CHECK(web_config_stop());
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SRUN_SLEEP_INTERVAL_US));
    ESP_LOGI(TAG, "进入深度睡眠，120秒后唤醒");
    esp_deep_sleep_start();
}

void srun_start(const app_config_t *cfg)
{
    app_config_t *copy = malloc(sizeof(*copy)); if (!copy) return; *copy = *cfg;
    if (xTaskCreate(srun_task, "srun_auth", 24576, copy, 4, NULL) != pdPASS) {
        free(copy);
        ESP_LOGE(TAG, "认证任务创建失败");
    }
}
