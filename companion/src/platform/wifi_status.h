#ifndef COMPANION_PLATFORM_WIFI_STATUS_H
#define COMPANION_PLATFORM_WIFI_STATUS_H

#include <stdint.h>

enum CpWifiStatus {
    CP_WIFI_OFF,         /* not initialised, or initialisation failed */
    CP_WIFI_IDLE,        /* on, not connected or connecting */
    CP_WIFI_CONNECTING,  /* searching, authenticating, associating or DHCP */
    CP_WIFI_CONNECTED,   /* associated with a usable IPv4 address */
    CP_WIFI_FAILED       /* the last connection attempt failed or timed out */
};

/* One poll of a connection attempt, read from the driver. */
typedef struct CpWifiPoll {
    int pending;         /* a connection attempt is under way */
    int associated;      /* the driver reports ASSOCSTATUS_ASSOCIATED */
    int cannotConnect;   /* the driver reports ASSOCSTATUS_CANNOTCONNECT */
    uint32_t ipv4;       /* Wifi_GetIP(): 0 or INADDR_NONE when there is none */
    uint32_t elapsedMs, timeoutMs;
} CpWifiPoll;

/* Nonzero for an IPv4 address the sockets can use. */
int CpWifi_UsableIPv4(uint32_t);

/* What a poll means. DSWiFi reports ASSOCIATED once IPv4 or IPv6 is ready,
 * and on a dual-stack network IPv6 can come first; the companion's sockets
 * are IPv4 only, so it stays CONNECTING until IPv4 is there, within the
 * same timeout. FAILED means the caller should end the attempt. */
enum CpWifiStatus CpWifi_Decide(const CpWifiPoll *);

#endif
