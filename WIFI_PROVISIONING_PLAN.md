# Web-Based Wi-Fi Provisioning — Implementation Plan (Revised)

Add the ability to configure Wi-Fi credentials through a web UI instead of reflashing.
On first boot (no stored STA credentials), the device comes up with the AP enabled but the
STA **idle** (not connected), serves a setup page, and runs a captive portal that redirects any
connected device to it. The user scans for networks, picks one, enters a password, and saves —
the device then reboots into normal AP+STA operation.

> This revision corrects two blockers and several gaps found while reviewing the first draft
> against the current source. See **§0 — What Changed** for the diff against the original plan.

---

## §0 — What Changed vs. the First Draft

| # | First draft said | Problem | Correction in this plan |
|---|------------------|---------|--------------------------|
| 1 | First boot → `WIFI_MODE_AP` | `esp_wifi_scan_start()` needs the STA started; in pure AP mode it returns `ESP_ERR_WIFI_MODE`, so the setup page's scan fails | Keep **`WIFI_MODE_APSTA`** (already set at `main.c:428`); just **don't call `esp_wifi_connect()`**. STA is enabled (scannable) but idle. |
| 2 | `wifi_config_save()` sets `provisioned=1` | `save()` is also called on first boot (`main.c:~373`) and by `wifi_config_reset_to_defaults()` (`:393`) and `wifi_config_sync_defaults()` (`:363`) → flag set wrongly; device never provisions and factory reset breaks | Set `provisioned` **only in the `/api/config` POST handler**. Keep `save()` flag-agnostic. Remove the first-boot default-save. |
| 3 | Captive portal = DNS hijack + 3 probe URLs | Clients only reach the portal if DHCP tells them to use the device as DNS — that code only runs after `IP_EVENT_STA_GOT_IP`, which never fires in provisioning | Explicitly advertise **192.168.4.1 as gateway + DNS** via DHCP at provisioning startup. Add a **catch-all 404→302 redirect**, not just 3 URLs. |
| 4 | Only A-record hijack | AAAA (IPv6) probes left unanswered stall iOS/Android detection | Answer AAAA with **NODATA** (NOERROR, 0 answers) so clients fall back to IPv4. |
| 5 | `/api/config` POST enhanced | Endpoint currently has **no auth**; once provisioned, anyone on the AP could rewrite the uplink | Gate `/api/scan` and `/api/config` with the existing dashboard auth **when not in provisioning mode**. |
| 6 | Factory reset writes defaults back | `reset_to_defaults()` re-saves default creds, so reset lands on defaults, not provisioning | `/api/config/reset` should **erase creds + clear the flag** so the device returns to provisioning. |
| — | (not covered) | Wrong-password lockout, provisioning-AP secrecy, NVS-at-rest | Added in §6 (recovery) and §7 (security). |

---

## §1 — Resolved Behavior Decisions

- **AP behavior on first boot:** AP enabled, **STA idle** (APSTA mode, no `esp_wifi_connect()`). No upstream connection until the user provides credentials; reboot into normal AP+STA afterward.
- **Captive portal scope:** active **only** in provisioning mode. Normal operation keeps the existing forward-to-upstream DNS proxy untouched.
- **Setup-page auth:** **none during provisioning** (device unconfigured, reachable only via direct AP connection). The **same endpoints require dashboard auth once provisioned**.
- **Scan in dashboard settings:** yes — reuse `/api/scan` from the existing settings modal so users can change networks without a factory reset.

---

## §2 — Provisioning State & Boot Flow

```
boot
 └─ wifi_config_load()                 // returns NVS config, or in-memory defaults on first boot
 └─ g_provisioning = !wifi_config_has_sta_credentials()   // reads "provisioned" NVS flag
 └─ esp_wifi_set_mode(APSTA)           // unchanged from today
 └─ esp_wifi_set_config(AP, ...)       // always — AP must broadcast (uses default AP SSID/pass)
 └─ if (!g_provisioning) esp_wifi_set_config(STA, ...)
 └─ esp_wifi_start()
 └─ if (g_provisioning):
 │     ├─ configure AP DHCP: gateway+DNS = 192.168.4.1
 │     ├─ dns_proxy_set_captive_mode(true, &ap_ip); dns_proxy_start();
 │     ├─ web_server_set_provisioning_mode(true);   // root serves setup.html, 404 redirects
 │     └─ (STA stays idle — no connect)
 └─ else: normal path (connect STA → on GOT_IP enable NAT, forward-DNS, DHCP as today)
```

Boot path A (provisioning) and B (normal) differ only by the `g_provisioning` flag. The flag is
**derived from NVS at boot**, set to `true` until the user saves real credentials.

---

## §3 — Detailed Changes by File

### 3.1  `wifi_config_manager.[ch]` — provisioned flag

**Add a dedicated flag; do NOT couple it to `wifi_config_save()`.**

```c
// wifi_config_manager.h
#define NVS_KEY_PROVISIONED "provisioned"

/** True only if the user has saved real STA credentials (flag == 1). */
bool      wifi_config_has_sta_credentials(void);
/** Explicitly set/clear the provisioned flag (called by the web handler). */
esp_err_t wifi_config_set_provisioned(bool provisioned);
```

```c
// wifi_config_manager.c
bool wifi_config_has_sta_credentials(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t flag = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_PROVISIONED, &flag);
    nvs_close(h);
    return (err == ESP_OK && flag == 1);
}

esp_err_t wifi_config_set_provisioned(bool provisioned) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_PROVISIONED, provisioned ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
```

- `wifi_config_erase()`: also `nvs_erase_key(h, NVS_KEY_PROVISIONED)` so factory reset returns to provisioning.
- **Leave `wifi_config_save()`, `sync_defaults()`, `reset_to_defaults()` untouched** — none of them should touch the flag.

### 3.2  `main.c` — first-boot default-save removal + provisioning branch

**Remove the obsolete default-save** (currently `~main.c:373`):

```c
if (cfg_err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "No WiFi config in NVS — first boot (provisioning)");
    // REMOVED: wifi_config_save(&wifi_cfg);   // do not persist defaults; keep STA unprovisioned
}
```

`wifi_config_load()` already fills `wifi_cfg` with defaults on `NOT_FOUND` (`:134`), so the **AP fields
are still populated** and the AP can broadcast during provisioning.

**Add the provisioning branch** in `wifi_init_repeater()` (after AP/STA config is built, around the
current `esp_wifi_set_mode(WIFI_MODE_APSTA)` at `:428`):

```c
bool g_provisioning_mode = !wifi_config_has_sta_credentials();   // file-scope global

ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));   // unchanged
ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
if (!g_provisioning_mode) {
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
}
ESP_ERROR_CHECK(esp_wifi_start());

if (g_provisioning_mode) {
    ESP_LOGW(TAG, "No STA credentials — starting in PROVISIONING (AP idle) mode");
}
```

**Gate the auto-connect in the event handler** (`main.c:67` and `:89`) so the idle STA doesn't try to
join an empty SSID:

```c
case WIFI_EVENT_STA_START:
    if (!g_provisioning_mode) esp_wifi_connect();
    break;
case WIFI_EVENT_STA_DISCONNECTED:
    if (!g_provisioning_mode) esp_wifi_connect();   // existing retry logic
    break;
```

**Start the captive portal** in the provisioning path of `app_main()` (reusing your existing
DHCP-DNS pattern from `:177–222`, but pointing DNS at the AP itself):

```c
if (g_provisioning_mode) {
    esp_netif_ip_info_t ap_ip;
    esp_netif_get_ip_info(s_ap_netif, &ap_ip);                       // 192.168.4.1

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dns_info_t dns = { .ip = { .type = ESP_IPADDR_TYPE_V4 } };
    dns.ip.u_addr.ip4.addr = ap_ip.ip.addr;
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t dns_offer = 1;
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &dns_offer, sizeof(dns_offer));
    esp_netif_dhcps_start(s_ap_netif);

    dns_proxy_set_captive_mode(true, &ap_ip.ip);
    dns_proxy_start();                              // no upstream needed in captive mode
    web_server_set_provisioning_mode(true);
    ESP_LOGI(TAG, "Provisioning: connect to AP '%s' and open any URL", wifi_cfg.ap_ssid);
}
```

> Use a `web_server_set_provisioning_mode(bool)` setter rather than a leaked global — `web_server.c`
> needs the flag for the root handler, the 404 redirect, and the auth guard. `dns_proxy.c` keeps its
> own `s_captive_mode`, so it doesn't need the global.

### 3.3  `dns_proxy.[ch]` — captive mode

```c
// dns_proxy.h
void dns_proxy_set_captive_mode(bool enable, const esp_ip4_addr_t *device_ip);
```

```c
// dns_proxy.c
static bool            s_captive_mode = false;
static esp_ip4_addr_t  s_captive_ip;

void dns_proxy_set_captive_mode(bool enable, const esp_ip4_addr_t *device_ip) {
    s_captive_mode = enable;
    if (device_ip) s_captive_ip = *device_ip;
}
```

In `dns_proxy_task()`, **right after `recvfrom()` and `parse_dns_query()`** (≈`:420`), before the
upstream-forward block, insert:

```c
if (s_captive_mode) {
    uint8_t resp[DNS_PROXY_BUF_SIZE];
    int rlen = (qtype == 1 /* A */)
        ? build_captive_response(buf, len, resp, sizeof(resp), s_captive_ip.addr)
        : build_nodata_response(buf, len, resp, sizeof(resp));   // AAAA & others
    if (rlen > 0)
        sendto(listen_sock, resp, rlen, 0, (struct sockaddr *)&from, from_len);
    dns_log_add(qname, qtype, /*client*/ &from);   // keep logging consistent (optional)
    continue;                                        // skip upstream forwarding
}
```

Helpers (RFC 1035 wire format; the question name is reused via a 0xC00C compression pointer):

```c
// Find end of the question section: 12-byte header + QNAME + 4 (QTYPE+QCLASS)
static int question_end(const uint8_t *buf, int len) {
    int off = 12;
    while (off < len && buf[off] != 0) off += buf[off] + 1;  // walk labels
    return (off < len) ? off + 1 + 4 : -1;                   // null byte + QTYPE/QCLASS
}

static int build_captive_response(const uint8_t *q, int qlen,
                                  uint8_t *out, int out_cap, uint32_t ip_be) {
    int qend = question_end(q, qlen);
    if (qend < 0 || qend + 16 > out_cap) return -1;
    memcpy(out, q, qend);
    out[2] = 0x81; out[3] = 0x80;          // QR=1, RD=1, RA=1, RCODE=0
    out[6] = 0x00; out[7] = 0x01;          // ANCOUNT = 1
    out[8] = out[9] = out[10] = out[11] = 0;// NSCOUNT/ARCOUNT = 0
    int o = qend;
    out[o++] = 0xC0; out[o++] = 0x0C;      // name pointer -> offset 12
    out[o++] = 0x00; out[o++] = 0x01;      // TYPE A
    out[o++] = 0x00; out[o++] = 0x01;      // CLASS IN
    out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x3C; // TTL 60s
    out[o++] = 0x00; out[o++] = 0x04;      // RDLENGTH 4
    memcpy(out + o, &ip_be, 4); o += 4;    // RDATA (network byte order)
    return o;
}

static int build_nodata_response(const uint8_t *q, int qlen, uint8_t *out, int out_cap) {
    int qend = question_end(q, qlen);
    if (qend < 0 || qend > out_cap) return -1;
    memcpy(out, q, qend);
    out[2] = 0x81; out[3] = 0x80;          // QR=1, RA=1, RCODE=0
    out[6] = out[7] = 0x00;                // ANCOUNT = 0  (NODATA)
    out[8] = out[9] = out[10] = out[11] = 0;
    return qend;
}
```

### 3.4  `web_server.c` — scan, captive redirects, conditional auth, restart

**Provisioning flag + auth guard:**

```c
static bool s_provisioning = false;
void web_server_set_provisioning_mode(bool on) { s_provisioning = on; }

// Open during provisioning; require dashboard auth once provisioned.
static esp_err_t guard(httpd_req_t *req) {
    if (s_provisioning) return ESP_OK;
    return check_dashboard_auth(req);   // reuse the mechanism behind /api/auth
}
```

**`GET /api/scan`** (works because mode is APSTA):

```c
static esp_err_t api_scan_handler(httpd_req_t *req) {
    if (guard(req) != ESP_OK) return ESP_FAIL;
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                              .scan_time.active = { .min = 100, .max = 150 } };  // bounded dwell
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {            // blocking but short
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_FAIL;
    }
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);
    // dedup by SSID keeping strongest RSSI, sort desc, build JSON with cJSON:
    //   [{"ssid":"..","rssi":-45,"auth":"WPA2","channel":6}, ...]
    // ... serialize and httpd_resp_send(...)
    return ESP_OK;
}
```

> Note: in the **post-provisioning** dashboard case the STA is connected, so a scan briefly takes the
> single radio off-channel and blips the uplink. Keep the dwell short and warn the user in the UI.

**Captive redirects + root selection:**

```c
static esp_err_t root_handler(httpd_req_t *req) {
    return serve_static_file(req,
        s_provisioning ? "setup.html" : "index.html", "text/html; charset=utf-8");
}

// Catch-all: any unknown path during provisioning -> the setup page.
static esp_err_t captive_404(httpd_req_t *req, httpd_err_code_t err) {
    if (s_provisioning) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}
```

Register in `web_server_init()`:
```c
config.max_uri_handlers = 24;                          // was 16
httpd_register_uri_handler(server, &api_scan);         // GET  /api/scan
httpd_register_uri_handler(server, &setup_uri);        // GET  /setup -> setup.html
httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_404);  // catch-all
```

> The OS probe URLs (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`) are covered by the
> catch-all 404 redirect, so they don't each need a dedicated handler — fewer handlers, more robust.

**`POST /api/config`** — add guard, set the flag, restart from the setup flow:

```c
// at top of api_config_post_handler():
if (guard(req) != ESP_OK) return ESP_FAIL;
// ... existing parse / validate / wifi_config_save / add_ssid_to_history ...
wifi_config_set_provisioned(true);                     // mark provisioned (only here)
httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Saved. Restarting...\"}", -1);
schedule_restart_ms(1000);                              // esp_timer one-shot -> esp_restart()
return ESP_OK;
```

**`POST /api/config/reset`** — erase + clear flag (don't re-write defaults), then restart →
device boots back into provisioning:

```c
wifi_config_erase();          // now also clears "provisioned"
schedule_restart_ms(500);
```

### 3.5  `www/setup.html` *(NEW)* + `www/index.html` / `www/script.js`

- **`setup.html`** — self-contained (inline CSS/JS to limit SPIFFS file count), matching the dashboard
  dark theme: header, **Scan** button → `GET /api/scan` → list with signal bars + lock icons,
  SSID auto-fill, password field, optional AP customization (collapsed), **Save** → `POST /api/config`
  → "Saving… device will restart."
- **`index.html`** — add a "📡 Scan" button beside the STA SSID field in the settings modal.
- **`script.js`** — add `scanNetworks()` calling `/api/scan` and rendering a selectable list that
  fills the SSID field. The settings flow already POSTs to `/api/config` and the device restarts.

---

## §4 — File Change Summary

| File | Action | Description |
|------|--------|-------------|
| `wifi_config_manager.h` | MODIFY | `wifi_config_has_sta_credentials()`, `wifi_config_set_provisioned()`, `NVS_KEY_PROVISIONED` |
| `wifi_config_manager.c` | MODIFY | Implement flag get/set; `erase()` clears it; **`save()` left untouched** |
| `main.c` | MODIFY | Remove first-boot default-save; `g_provisioning_mode`; APSTA-idle branch; gate auto-connect; start captive portal + DHCP-DNS=192.168.4.1 |
| `dns_proxy.h` | MODIFY | `dns_proxy_set_captive_mode()` |
| `dns_proxy.c` | MODIFY | Captive A-record + NODATA response crafting |
| `web_server.c` | MODIFY | `/api/scan`, `web_server_set_provisioning_mode()`, auth `guard()`, captive 404 redirect, root selection, set provisioned + restart, reset→erase, `max_uri_handlers=24` |
| `www/setup.html` | NEW | Provisioning page |
| `www/index.html` | MODIFY | Scan button in settings modal |
| `www/script.js` | MODIFY | `scanNetworks()` + result rendering |

---

## §5 — Captive Portal Mechanics (why each piece is needed)

1. **DHCP advertises 192.168.4.1 as DNS** → clients send DNS to the device (without this the hijack is never hit).
2. **DNS captive mode** → every A query resolves to 192.168.4.1; AAAA returns NODATA so clients fall back to IPv4.
3. **OS probe + catch-all 302** → any HTTP request (probe URL or user-typed) during provisioning redirects to the setup page, triggering the "Sign in to Wi-Fi" banner.
4. On save → `provisioned=1` + reboot → flag is now set → normal APSTA path, captive mode off, DNS proxy forwards upstream as today.

---

## §6 — Failure & Recovery

- **Wrong upstream password:** after reboot the device is `provisioned=1`, so STA fails to connect — but the AP still comes up (APSTA) and the dashboard is reachable at `192.168.4.1`, where the user can re-scan and re-enter credentials. Ensure **AP + web server start regardless of STA result** (they do, since startup doesn't block on connect). Add an **"Upstream: not connected" status** to the dashboard, and optionally **auto-revert to provisioning** (captive portal) after N failed STA attempts.
- **Factory reset:** long-press the GPIO0 button (existing button task) → call the reset path → erase creds + clear flag → reboot into provisioning.

---

## §7 — Security Notes (implement and/or document)

- **Provisioning AP secrecy:** if the provisioning AP is open, the uplink password is POSTed over plaintext HTTP across an unencrypted air link. Use **WPA2 on the provisioning AP** with a per-device default password (MAC-derived or printed on a label).
- **Endpoint auth:** `/api/scan` and `/api/config` are open only while `s_provisioning`; once provisioned they require dashboard auth (`guard()`).
- **Credentials at rest:** NVS is plaintext flash. Note `CONFIG_NVS_ENCRYPTION` + flash encryption as the hardening path (good "security at rest" point for the report).

---

## §8 — Verification Plan

```bash
cd /Users/zainnasseer/development/esp/wifi_repeater
idf.py build
idf.py erase-flash flash monitor   # erase-flash is required to test true first boot
```

1. **First boot:** after `erase-flash`, device logs "PROVISIONING (AP idle)", AP broadcasts, no STA connect attempts.
2. **Captive trigger:** connect a phone → OS shows "Sign in to Wi-Fi" / browser auto-opens the setup page.
3. **DHCP-DNS check:** client's assigned DNS is `192.168.4.1` (confirms §3.2 DHCP option).
4. **Scan:** Scan on the setup page lists nearby networks with RSSI/auth/channel.
5. **Provision:** select network, enter password, save → device reboots into AP+STA and gets internet.
6. **Normal mode:** dashboard works as before; **no captive redirect**, DNS forwards upstream.
7. **Wrong-password recovery:** provision with a bad password → STA fails but AP/dashboard reachable at `192.168.4.1`; re-provision succeeds. *(new)*
8. **Re-scan from dashboard:** settings modal scan populates networks; changing network requires auth. *(auth = new)*
9. **Factory reset:** button long-press / reset API → device returns to **provisioning** (not default creds). *(corrected)*
