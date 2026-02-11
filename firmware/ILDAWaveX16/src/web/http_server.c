/**
 * @file http_server.c
 * @brief HTTP server with WiFi configuration
 */

#include "http_server.h"
#include "ota_handler.h"
#include "config.h"
#include "hal/dac_timer.h"
#include "core/frame_buffer.h"
#include "input/etherdream_server.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

extern bool wifi_is_connected(void);
extern bool wifi_save_credentials(const char* ssid, const char* password);
extern bool wifi_connect_to(const char* ssid, const char* password);

static const char* TAG = "HTTP";
static httpd_handle_t s_server = NULL;

static const char* INDEX_HTML = 
"<!DOCTYPE html>\n"
"<html><head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>ILDAWaveX16</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;padding:16px}\n"
".header{text-align:center;padding:20px 0;border-bottom:1px solid #333}\n"
".header h1{font-size:1.8em;color:#00d9ff}\n"
".status{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:12px;margin:20px 0}\n"
".stat{background:#252547;padding:16px;border-radius:12px;text-align:center}\n"
".stat-value{font-size:1.8em;font-weight:700;color:#00d9ff}\n"
".stat-label{font-size:.75em;color:#888;margin-top:4px}\n"
".card{background:#252547;border-radius:12px;padding:20px;margin:16px 0}\n"
".indicator{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:8px}\n"
".on{background:#2ecc71}.off{background:#666}\n"
"a{color:#00d9ff}\n"
"</style>\n"
"</head><body>\n"
"<div class='header'><h1>⚡ ILDAWaveX16</h1></div>\n"
"<div class='status'>\n"
"<div class='stat'><div class='stat-value' id='kpps'>--</div><div class='stat-label'>kpps</div></div>\n"
"<div class='stat'><div class='stat-value' id='buffer'>--</div><div class='stat-label'>Buffer</div></div>\n"
"<div class='stat'><div class='stat-value' id='latency'>--</div><div class='stat-label'>Latency ms</div></div>\n"
"</div>\n"
"<div class='card'>\n"
"<p><span class='indicator' id='edInd'></span><span id='edStatus'>Waiting...</span></p>\n"
"<p style='margin-top:10px'><span class='indicator' id='wifiInd'></span><span id='wifiStatus'>WiFi...</span></p>\n"
"<p style='margin-top:10px'><a href='/wifi'>⚙️ WiFi Settings</a></p>\n"
"</div>\n"
"<script>\n"
"async function refresh(){\n"
"  const s=await(await fetch('/api/status')).json();\n"
"  document.getElementById('kpps').textContent=(s.scan_rate/1000).toFixed(1);\n"
"  document.getElementById('buffer').textContent=s.buffer_level;\n"
"  document.getElementById('latency').textContent=s.latency_ms||'--';\n"
"  document.getElementById('edInd').className='indicator '+(s.ed_connected?'on':'off');\n"
"  document.getElementById('edStatus').textContent=s.ed_connected?'Streaming':'Waiting...';\n"
"  document.getElementById('wifiInd').className='indicator '+(s.wifi_connected?'on':'off');\n"
"  document.getElementById('wifiStatus').textContent=s.wifi_connected?'WiFi: '+s.wifi_ssid:'WiFi: Not connected';\n"
"}\n"
"setInterval(refresh,500);refresh();\n"
"</script>\n"
"</body></html>\n";

static const char* WIFI_HTML = 
"<!DOCTYPE html>\n"
"<html><head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>WiFi Setup - ILDAWaveX16</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;padding:16px;min-height:100vh}\n"
".header{text-align:center;padding:20px 0;border-bottom:1px solid #333}\n"
".header h1{font-size:1.5em;color:#00d9ff}\n"
".card{background:#252547;border-radius:12px;padding:20px;margin:16px 0}\n"
"input,select{width:100%;padding:12px;margin:8px 0 16px;border:1px solid #444;border-radius:8px;background:#1a1a2e;color:#eee;font-size:1em}\n"
"button{width:100%;padding:14px;background:#00d9ff;color:#000;border:none;border-radius:8px;font-size:1em;font-weight:700;cursor:pointer}\n"
"button:hover{background:#00b8d9}\n"
".network{padding:12px;margin:4px 0;background:#1a1a2e;border-radius:8px;cursor:pointer}\n"
".network:hover{background:#333}\n"
".signal{float:right;color:#888}\n"
"a{color:#00d9ff;display:block;text-align:center;margin-top:20px}\n"
".msg{padding:12px;border-radius:8px;margin:10px 0;text-align:center}\n"
".ok{background:#2ecc71;color:#000}.err{background:#e74c3c}\n"
"</style>\n"
"</head><body>\n"
"<div class='header'><h1>📶 WiFi Setup</h1></div>\n"
"<div class='card'>\n"
"<div id='msg'></div>\n"
"<label>Available Networks:</label>\n"
"<div id='networks'><p style='color:#888;text-align:center'>Scanning...</p></div>\n"
"<br><label>SSID:</label>\n"
"<input type='text' id='ssid' placeholder='Network name'>\n"
"<label>Password:</label>\n"
"<input type='password' id='pass' placeholder='Password'>\n"
"<button onclick='connect()'>Connect</button>\n"
"</div>\n"
"<a href='/'>← Back to Dashboard</a>\n"
"<script>\n"
"async function scan(){\n"
"  const r=await fetch('/api/wifi/scan');\n"
"  const nets=await r.json();\n"
"  const el=document.getElementById('networks');\n"
"  if(!nets.length){el.innerHTML='<p style=\"color:#888\">No networks found</p>';return;}\n"
"  el.innerHTML=nets.map(n=>`<div class='network' onclick=\"document.getElementById('ssid').value='${n.ssid}'\">`+\n"
"    `${n.ssid}<span class='signal'>${n.rssi}dBm</span></div>`).join('');\n"
"}\n"
"async function connect(){\n"
"  const ssid=document.getElementById('ssid').value;\n"
"  const pass=document.getElementById('pass').value;\n"
"  if(!ssid){alert('Enter SSID');return;}\n"
"  document.getElementById('msg').innerHTML='<div class=\"msg\">Connecting...</div>';\n"
"  const r=await fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password:pass})});\n"
"  const j=await r.json();\n"
"  document.getElementById('msg').innerHTML=j.success?'<div class=\"msg ok\">Connected! AP will be disabled.</div>':'<div class=\"msg err\">Failed: '+j.error+'</div>';\n"
"  if(j.success)setTimeout(()=>location.href='/',2000);\n"
"}\n"
"scan();\n"
"</script>\n"
"</body></html>\n";

static const char* CAPTIVE_HTML = 
"<!DOCTYPE html>\n"
"<html><head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<meta http-equiv='refresh' content='0;url=http://192.168.4.1/wifi'>\n"
"<title>WiFi Setup</title>\n"
"</head><body>\n"
"<p>Redirecting to <a href='http://192.168.4.1/wifi'>WiFi Setup</a>...</p>\n"
"</body></html>\n";

static esp_err_t index_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

static esp_err_t wifi_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, WIFI_HTML, strlen(WIFI_HTML));
    return ESP_OK;
}

// Captive portal: trick devices into thinking there IS internet.
// Android checks /generate_204 and expects HTTP 204.
// iOS/macOS check /hotspot-detect.html and expect "Success".
// Windows checks /connecttest.txt and /ncsi.txt.
static esp_err_t captive_204_handler(httpd_req_t* req) {
    // Android: expects HTTP 204 No Content
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t captive_success_handler(httpd_req_t* req) {
    // iOS/macOS: expects body containing "Success"
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    return ESP_OK;
}

static esp_err_t captive_ncsi_handler(httpd_req_t* req) {
    // Windows NCSI: expects 200 OK with text body
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Microsoft NCSI");
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t* req) {
    cJSON* json = cJSON_CreateObject();
    
    cJSON_AddBoolToObject(json, "running", g_status.running);
    cJSON_AddBoolToObject(json, "ed_connected", g_status.ed_connected);
    cJSON_AddNumberToObject(json, "buffer_level", g_status.buffer_level);
    cJSON_AddNumberToObject(json, "scan_rate", g_status.current_scan_rate);
    cJSON_AddNumberToObject(json, "ed_point_rate", g_status.ed_point_rate);
    
    cJSON_AddBoolToObject(json, "wifi_connected", wifi_is_connected());
    
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(json, "wifi_ssid", (char*)ap.ssid);
        cJSON_AddNumberToObject(json, "wifi_rssi", ap.rssi);
    } else {
        cJSON_AddStringToObject(json, "wifi_ssid", "");
    }
    
    if (g_status.current_scan_rate > 0 && g_status.buffer_level > 0) {
        float latency = (float)g_status.buffer_level / (float)g_status.current_scan_rate * 1000.0f;
        cJSON_AddNumberToObject(json, "latency_ms", (int)latency);
    } else {
        cJSON_AddNumberToObject(json, "latency_ms", 0);
    }
    
    const char* str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    free((void*)str);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t* req) {
    ESP_LOGI(TAG, "WiFi scan request");
    
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Found %d networks", ap_count);
    
    if (ap_count > 20) ap_count = 20;
    
    wifi_ap_record_t* ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_list) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    
    cJSON* json = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON* ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char*)ap_list[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(ap, "auth", ap_list[i].authmode);
        cJSON_AddItemToArray(json, ap);
    }
    free(ap_list);
    
    const char* str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    free((void*)str);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t wifi_connect_handler(httpd_req_t* req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[ret] = 0;
    
    cJSON* json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON* ssid_j = cJSON_GetObjectItem(json, "ssid");
    cJSON* pass_j = cJSON_GetObjectItem(json, "password");
    
    cJSON* resp = cJSON_CreateObject();
    
    if (ssid_j && cJSON_IsString(ssid_j)) {
        const char* ssid = ssid_j->valuestring;
        const char* pass = (pass_j && cJSON_IsString(pass_j)) ? pass_j->valuestring : "";
        
        ESP_LOGI(TAG, "WiFi connect request: %s", ssid);
        
        if (wifi_connect_to(ssid, pass)) {
            cJSON_AddBoolToObject(resp, "success", true);
        } else {
            cJSON_AddBoolToObject(resp, "success", false);
            cJSON_AddStringToObject(resp, "error", "Connection failed");
        }
    } else {
        cJSON_AddBoolToObject(resp, "success", false);
        cJSON_AddStringToObject(resp, "error", "Missing SSID");
    }
    
    cJSON_Delete(json);
    
    const char* str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    free((void*)str);
    cJSON_Delete(resp);
    return ESP_OK;
}

esp_err_t http_server_init(void) {
    return ESP_OK;
}

esp_err_t http_server_start(void) {
    if (s_server) return ESP_OK;
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.core_id = CORE_SERVICES;
    config.max_uri_handlers = 16;
    
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) return ret;
    
    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_register_uri_handler(s_server, &index_uri);
    
    httpd_uri_t wifi_uri = { .uri = "/wifi", .method = HTTP_GET, .handler = wifi_handler };
    httpd_register_uri_handler(s_server, &wifi_uri);
    
    httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_register_uri_handler(s_server, &status_uri);
    
    httpd_uri_t scan_uri = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler };
    httpd_register_uri_handler(s_server, &scan_uri);
    
    httpd_uri_t connect_uri = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_handler };
    httpd_register_uri_handler(s_server, &connect_uri);
    
    // Captive portal: respond correctly to each platform's connectivity check
    // Android
    httpd_uri_t gen204_uri = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_204_handler };
    httpd_register_uri_handler(s_server, &gen204_uri);
    // iOS / macOS
    httpd_uri_t hotspot_uri = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_success_handler };
    httpd_register_uri_handler(s_server, &hotspot_uri);
    // Windows NCSI
    httpd_uri_t ncsi_uri = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_ncsi_handler };
    httpd_register_uri_handler(s_server, &ncsi_uri);
    httpd_uri_t ncsi2_uri = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_ncsi_handler };
    httpd_register_uri_handler(s_server, &ncsi2_uri);
    // Additional Apple check
    httpd_uri_t apple_uri = { .uri = "/library/test/success.html", .method = HTTP_GET, .handler = captive_success_handler };
    httpd_register_uri_handler(s_server, &apple_uri);
    
    // Register OTA handlers
    ota_handler_register(s_server);
    
    ESP_LOGI(TAG, "HTTP on port %d", HTTP_PORT);
    return ESP_OK;
}

esp_err_t http_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}
