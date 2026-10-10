/**
 * @brief Web 配置后台 - HTTP 服务器 + SPIFFS 配置读写
 */

#include "web_config.h"
#include "config_codec.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "cJSON.h"

static const char* TAG = "WEB";

static httpd_handle_t s_server = NULL;

/* ============ HTML 页面模板 ============ */

static const char* HTML_HEAD =
    "<!DOCTYPE html><html lang='zh-CN'><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-C3 深澜配置</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,sans-serif;background:#f5f5f5;padding:20px;color:#333}"
    "h1{font-size:20px;margin-bottom:20px;text-align:center}"
    ".card{background:#fff;border-radius:12px;padding:20px;margin-bottom:16px;box-shadow:0 1px 4px rgba(0,0,0,.1)}"
    "h2{font-size:15px;margin-bottom:12px;color:#666}"
    "label{font-size:13px;color:#888;display:block;margin-bottom:4px}"
    "input{width:100%;padding:10px 12px;border:1px solid #ddd;border-radius:8px;font-size:14px;margin-bottom:12px}"
    "input:focus{outline:none;border-color:#007aff;box-shadow:0 0 0 2px rgba(0,122,255,.2)}"
    ".btn{width:100%;padding:12px;border:none;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer}"
    ".btn-primary{background:#007aff;color:#fff}"
    ".btn-primary:active{background:#005bbf}"
    "hr{border:none;border-top:1px solid #eee;margin:16px 0}"
    "</style></head><body>"
    "<h1>ESP32-C3 深澜自动认证</h1>";

static const char* HTML_FOOT =
    "</body></html>";

/* ============ 配置读写 ============ */

bool load_config(app_config_t* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->auth_server, "10.253.0.235", sizeof(cfg->auth_server) - 1);
    strncpy(cfg->ac_id, "3", sizeof(cfg->ac_id) - 1);
    strncpy(cfg->channel, "phone", sizeof(cfg->channel) - 1);

    FILE* f = fopen("/spiffs/config.json", "r");
    if (!f) {
        ESP_LOGI(TAG, "config.json 不存在，需首次配置");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return false; }

    char* json = malloc(len + 1);
    fread(json, 1, len, f);
    json[len] = 0;
    fclose(f);

    cJSON* root = cJSON_Parse(json);
    free(json);
    if (!root) return false;

    cJSON* item;
    if ((item = cJSON_GetObjectItem(root, "wifi_ssid")) && cJSON_IsString(item))
        strncpy(cfg->wifi_ssid, item->valuestring, sizeof(cfg->wifi_ssid) - 1);
    if ((item = cJSON_GetObjectItem(root, "wifi_password")) && cJSON_IsString(item))
        strncpy(cfg->wifi_password, item->valuestring, sizeof(cfg->wifi_password) - 1);
    if ((item = cJSON_GetObjectItem(root, "campus_username")) && cJSON_IsString(item))
        strncpy(cfg->campus_username, item->valuestring, sizeof(cfg->campus_username) - 1);
    if ((item = cJSON_GetObjectItem(root, "campus_password")) && cJSON_IsString(item))
        strncpy(cfg->campus_password, item->valuestring, sizeof(cfg->campus_password) - 1);
    if ((item = cJSON_GetObjectItem(root, "channel")) && cJSON_IsString(item))
        strncpy(cfg->channel, item->valuestring, sizeof(cfg->channel) - 1);
    if ((item = cJSON_GetObjectItem(root, "auth_server")) && cJSON_IsString(item))
        strncpy(cfg->auth_server, item->valuestring, sizeof(cfg->auth_server) - 1);
    if ((item = cJSON_GetObjectItem(root, "ac_id")) && cJSON_IsString(item))
        strncpy(cfg->ac_id, item->valuestring, sizeof(cfg->ac_id) - 1);
    if ((item = cJSON_GetObjectItem(root, "user_type")) && cJSON_IsString(item))
        strncpy(cfg->user_type, item->valuestring, sizeof(cfg->user_type) - 1);
    if ((item = cJSON_GetObjectItem(root, "auth_ip")) && cJSON_IsString(item))
        strncpy(cfg->auth_ip, item->valuestring, sizeof(cfg->auth_ip) - 1);

    cJSON_Delete(root);

    if (cfg->wifi_ssid[0] == 0 || cfg->campus_username[0] == 0)
        return false;

    if (cfg->auth_server[0] == 0) strncpy(cfg->auth_server, "10.253.0.235", sizeof(cfg->auth_server) - 1);
    if (cfg->ac_id[0] == 0) strncpy(cfg->ac_id, "3", sizeof(cfg->ac_id) - 1);

    return true;
}

bool save_config(const app_config_t* cfg)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;

    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", cfg->wifi_password);
    cJSON_AddStringToObject(root, "campus_username", cfg->campus_username);
    cJSON_AddStringToObject(root, "campus_password", cfg->campus_password);
    cJSON_AddStringToObject(root, "channel", cfg->channel);
    cJSON_AddStringToObject(root, "auth_server", cfg->auth_server);
    cJSON_AddStringToObject(root, "ac_id", cfg->ac_id);
    cJSON_AddStringToObject(root, "user_type", cfg->user_type);
    cJSON_AddStringToObject(root, "auth_ip", cfg->auth_ip);

    char* json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return false;

    bool ok = false;
    FILE* f = fopen("/spiffs/config.json", "w");
    if (f) {
        size_t len = strlen(json);
        ok = (fwrite(json, 1, len, f) == len);
        /* fclose -> SPIFFS_close -> spiffs_fflush_cache, 已落盘 */
        fclose(f);
        if (ok) ESP_LOGI(TAG, "config.json 已保存 (%zu 字节)", len);
    } else {
        ESP_LOGE(TAG, "config.json 打开失败");
    }
    free(json);
    return ok;
}

/*
 * 写入认证客户端读取的 ESurfingClient.json。
 * 与 config.json 分开写, 任一失败都要让调用方知晓, 否则设备会带着
 * 不一致的配置重启 (WiFi 连上了但认证用的是旧账号)。
 */
static bool save_esurfing_client(const app_config_t* cfg)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;

    cJSON_AddBoolToObject(root, "enabled", true);
    cJSON_AddNumberToObject(root, "log_lv", 4);
    cJSON* accounts = cJSON_AddArrayToObject(root, "accounts");
    cJSON* acct = cJSON_CreateObject();
    cJSON_AddStringToObject(acct, "username", cfg->campus_username);
    cJSON_AddStringToObject(acct, "password", cfg->campus_password);
    cJSON_AddStringToObject(acct, "channel", cfg->channel[0] ? cfg->channel : "phone");
    cJSON_AddStringToObject(acct, "mark", "");
    cJSON_AddItemToArray(accounts, acct);

    char* json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return false;

    bool ok = false;
    FILE* f = fopen("/spiffs/ESurfingClient.json", "w");
    if (f) {
        size_t len = strlen(json);
        ok = (fwrite(json, 1, len, f) == len);
        fclose(f);
        if (ok) ESP_LOGI(TAG, "ESurfingClient.json 已保存");
    } else {
        ESP_LOGE(TAG, "ESurfingClient.json 打开失败");
    }
    free(json);
    return ok;
}

/*
 * 注意: 不要在此调用 esp_vfs_spiffs_unregister()。
 *
 * 1. SPIFFS_close() 内部已调用 spiffs_fflush_cache() (spiffs_hydrogen.c),
 *    即 fclose() 本身就会把 cache 写回 flash, 无需额外 flush。
 * 2. esp_vfs_spiffs_unregister() 会撤掉 /spiffs 的 VFS 挂载点。
 *    若此时 HTTP 服务器仍有在途请求(或后续 handler 再访问文件),
 *    对 /spiffs/ 下任何文件的访问都会失败, 这正是 v1.3.1
 *    "保存后配置丢失" 的根因。
 *
 * 落盘由各写入点的 fclose() 保证, 如需额外保险可对每个 fd 调用 fsync(),
 * 它映射到 SPIFFS_fflush(), 不会破坏挂载点。
 */

/* ============ HTTP 处理器 ============ */

static esp_err_t root_get_handler(httpd_req_t* req)
{
    ESP_LOGI(TAG, "收到 HTTP 请求: %s", req->uri);
    app_config_t cfg = {0};
    load_config(&cfg);

    /* 转义后再嵌入 value='...': 未转义的引号会提前闭合属性,
       导致密码含 ' 或 " 时表单显示错乱 (v1.3.1 缺陷) */
    char ssid[400], wifi_pw[400], user[400], camp_pw[400], server[400], acid[64], user_type[128], auth_ip[100];
    html_escape(cfg.wifi_ssid, ssid, sizeof(ssid));
    html_escape(cfg.wifi_password, wifi_pw, sizeof(wifi_pw));
    html_escape(cfg.campus_username, user, sizeof(user));
    html_escape(cfg.campus_password, camp_pw, sizeof(camp_pw));
    html_escape(cfg.auth_server, server, sizeof(server));
    html_escape(cfg.ac_id, acid, sizeof(acid));
    html_escape(cfg.user_type, user_type, sizeof(user_type));
    html_escape(cfg.auth_ip, auth_ip, sizeof(auth_ip));

    char* html = NULL;
    size_t len = asprintf(&html,
        "%s"
        "<div class='card'><h2>WiFi 设置</h2>"
        "<form id='config' action='/save' method='POST'>"
        "<label>2.4g WiFi SSID</label><input name='wifi_ssid' value='%s' placeholder='仅支持2.4g WiFi'>"
        "<label>2.4g WiFi 密码</label><input name='wifi_password' type='password' value='%s' placeholder='密码(留空为开放网络)'>"
        "<hr><h2>校园网账号</h2>"
        "<label>用户名</label><input name='campus_username' value='%s' placeholder='学号/手机号'>"
        "<label>密码</label><input name='campus_password' type='password' value='%s' placeholder='密码'>"
        "<label>深澜服务器</label><input name='auth_server' value='%s' placeholder='例如 10.253.0.235'>"
        "<label>AC ID</label><input name='ac_id' value='%s' placeholder='例如 3'>"
        "<label>运营商后缀（如 cmcc / unicom / telecom）</label><input name='user_type' value='%s' placeholder='按学校要求填写；账号已有 @ 后缀则留空'>"
        "<label>认证 IP（可选）</label><input name='auth_ip' value='%s' placeholder='通常留空，自动获取'>"
        "<button type='submit' class='btn btn-primary'>保存并重启</button>"
        "</form></div>"
        "%s",
        HTML_HEAD,
        ssid, wifi_pw, user, camp_pw,
        server, acid, user_type, auth_ip,
        HTML_FOOT);

    if (html) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, html, len);
        free(html);
    }
    return ESP_OK;
}

/* 表单体上限。字段本身最长 64 字节, 经 URL 编码最坏放大到 3 倍
   (%XX), 5 个字段约 960 字节 + 键名开销, 2048 留有余量。
   v1.3.1 用 512 字节硬截断, 长 SSID/密码会静默丢失。 */
#define FORM_BODY_MAX 2048

static esp_err_t save_post_handler(httpd_req_t* req)
{
    /* 拒绝超大请求体, 避免在内存受限的设备上分配过多 */
    if (req->content_len <= 0 || req->content_len > FORM_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空或过长");
        return ESP_FAIL;
    }

    char* content = calloc(1, req->content_len + 1);
    if (!content) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_FAIL;
    }

    /* 循环接收: httpd_req_recv 一次不一定收满 */
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, content + received,
                                 req->content_len - received);
        if (ret <= 0) {
            free(content);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "读取失败");
            return ESP_FAIL;
        }
        received += ret;
    }
    content[received] = '\0';

    app_config_t cfg;
    /* form_parse 会原地修改 content (分隔符替换为 \0) */
    if (!form_parse(content, &cfg)) {
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "WiFi SSID 和校园网账号不能为空");
        return ESP_FAIL;
    }

    if (!save_config(&cfg)) {
        free(content);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "配置写入失败");
        return ESP_FAIL;
    }

    free(content);

    /* 返回成功页面并自动重启 */
    const char* resp = "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2;url=/'></head>"
        "<body style='font-family:sans-serif;padding:40px;text-align:center'>"
        "<h2>已保存</h2><p>设备正在重启，稍后返回首页...</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, resp, strlen(resp));

    /* 给客户端留出接收响应的时间, 然后重启应用新配置 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static esp_err_t save_get_handler(httpd_req_t* req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "配置已保存，请返回首页");
    return ESP_OK;
}

static esp_err_t restart_post_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "重启中...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t* req)
{
    char sta_ip[16] = "0.0.0.0";
    char sta_ssid[33] = "";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK)
            snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&ip.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            memcpy(sta_ssid, ap.ssid, sizeof(ap.ssid));
    }
    char json[256];
    snprintf(json, sizeof(json),
        "{\"connected\":%s,\"ip\":\"%s\",\"ssid\":\"%s\"}",
        wifi_is_connected() ? "true" : "false", sta_ip, sta_ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t health_get_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "ESP32-C3 SRUN OK\n");
    return ESP_OK;
}

/* ============ 启动/停止 ============ */

esp_err_t web_config_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;

    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_uri_t uri_save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    httpd_uri_t uri_save_get = {
        .uri = "/save",
        .method = HTTP_GET,
        .handler = save_get_handler,
    };
    httpd_uri_t uri_restart = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_post_handler,
    };
    httpd_uri_t uri_status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_uri_t uri_health = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
    };

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务器启动失败");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uri_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uri_save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uri_save_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uri_restart));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uri_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uri_health));

    ESP_LOGI(TAG, "Web 配置后台已启动: http://192.168.4.1 (health: /health)");
    return ESP_OK;
}

esp_err_t web_config_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}

const char* web_config_get_status(void)
{
    return wifi_is_connected() ? "WiFi 已连接" : "WiFi 未连接";
}
