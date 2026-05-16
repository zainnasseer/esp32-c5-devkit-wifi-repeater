#include "dns_log.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static dns_log_entry_t s_entries[DNS_LOG_MAX_ENTRIES];
static size_t s_head = 0;
static size_t s_count = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t copy_len = dst_len - 1;
    if (copy_len > 0) {
        strncpy(dst, src, copy_len);
    }
    dst[copy_len] = '\0';
}

void dns_log_add(const char *mac, const char *ip, const char *domain, uint16_t qtype)
{
    uint32_t timestamp_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    portENTER_CRITICAL(&s_lock);
    dns_log_entry_t *entry = &s_entries[s_head];
    entry->timestamp_s = timestamp_s;
    copy_str(entry->mac, sizeof(entry->mac), mac);
    copy_str(entry->ip, sizeof(entry->ip), ip);
    copy_str(entry->domain, sizeof(entry->domain), domain);
    entry->qtype = qtype;
    s_head = (s_head + 1) % DNS_LOG_MAX_ENTRIES;
    if (s_count < DNS_LOG_MAX_ENTRIES) {
        s_count++;
    }
    portEXIT_CRITICAL(&s_lock);
}

size_t dns_log_get_latest(dns_log_entry_t *out, size_t max_entries)
{
    size_t count = 0;

    if (out == NULL || max_entries == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_lock);
    count = s_count;
    if (count > max_entries) {
        count = max_entries;
    }
    for (size_t i = 0; i < count; i++) {
        size_t index = (s_head + DNS_LOG_MAX_ENTRIES - 1 - i) % DNS_LOG_MAX_ENTRIES;
        out[i] = s_entries[index];
    }
    portEXIT_CRITICAL(&s_lock);

    return count;
}
