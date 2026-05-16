#ifndef DNS_LOG_H
#define DNS_LOG_H

#include <stddef.h>
#include <stdint.h>

#define DNS_LOG_MAX_ENTRIES 32

typedef struct {
    uint32_t timestamp_s;
    char mac[18];
    char ip[16];
    char domain[128];
    uint16_t qtype;
} dns_log_entry_t;

void dns_log_add(const char *mac, const char *ip, const char *domain, uint16_t qtype);
size_t dns_log_get_latest(dns_log_entry_t *out, size_t max_entries);

#endif // DNS_LOG_H
