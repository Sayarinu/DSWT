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

/* ---- Networks the Wi-Fi setup screen lists (LUNE-875) --------------------- */

#define CP_WIFI_SSID_MAX 32
/* A WPA2 passphrase has 8 to 63 printable ASCII characters; 64 hexadecimal
 * digits are the raw pre-shared key. */
#define CP_WIFI_KEY_MAX 64

enum CpWifiSecurity {
    CP_WIFI_SECURITY_OPEN,
    CP_WIFI_SECURITY_WEP,
    CP_WIFI_SECURITY_WPA2,      /* WPA2 personal, and WPA2/WPA3 mixed */
    CP_WIFI_SECURITY_WPA,       /* WPA with TKIP only: DSWiFi cannot join it */
    CP_WIFI_SECURITY_ENTERPRISE /* 802.1X: needs a login the companion cannot give */
};

/* Why a network cannot be joined. */
enum CpWifiUnsupported {
    CP_WIFI_SUPPORTED,
    CP_WIFI_UNSUPPORTED_TKIP,
    CP_WIFI_UNSUPPORTED_ENTERPRISE,
    CP_WIFI_UNSUPPORTED_NEEDS_DSI /* WPA2 in DS mode: the DS driver has open and WEP only */
};

typedef struct CpWifiNetwork {
    char ssid[CP_WIFI_SSID_MAX + 1]; /* the broadcast name's bytes, NUL-terminated */
    uint8_t security;                /* enum CpWifiSecurity */
    uint8_t bars;                    /* signal, 0 to 3 */
} CpWifiNetwork;

/* DSWiFi's Wifi_ApSecurityType values (0 open, 1 WEP, 2 WPA, 3 WPA2),
 * without its headers, and whether the access point asks for 802.1X. */
enum CpWifiSecurity CpWifi_Security(int apSecurityType, int auth8021x);
/* Signal bars from DSWiFi's RSSI: dBm in DSi mode, 0-255 on the DS driver.
 * 0 means no beacon for a while. */
unsigned CpWifi_Bars(int rssi, int dsiMode);
/* Whether the companion can join a network of this security. */
enum CpWifiUnsupported CpWifi_Unsupported(unsigned security, int wpa2Capable);

/* A key the player typed, for a network of this security: WPA2 8-63
 * printable ASCII characters or 64 hex digits; WEP 5 or 13 characters, or
 * 10 or 26 hex digits; open networks take none. */
int CpWifi_KeyValid(unsigned security, const char *key);
/* The bytes DSWiFi takes for that key (Wifi_ConnectSecureAP): WEP hex digits
 * become 5 or 13 bytes, anything else is passed as typed. Returns the length,
 * or -1 for an invalid key. */
int CpWifi_KeyBytes(unsigned security, const char *key, unsigned char out[CP_WIFI_KEY_MAX]);

/* How far a connection attempt got before it ended. */
enum CpWifiStage {
    CP_WIFI_STAGE_NONE,
    CP_WIFI_STAGE_SEARCHING,
    CP_WIFI_STAGE_AUTHENTICATING,
    CP_WIFI_STAGE_ASSOCIATING, /* includes the WPA2 handshake */
    CP_WIFI_STAGE_DHCP,
    CP_WIFI_STAGE_ASSOCIATED   /* associated, but without a usable IPv4 address */
};

/* Why an attempt to join one named network failed. */
enum CpWifiFailure {
    CP_WIFI_FAILURE_NONE,
    CP_WIFI_FAILURE_NOT_FOUND,  /* the network was not in range */
    CP_WIFI_FAILURE_REJECTED,   /* it ended while authenticating: most often a wrong password */
    CP_WIFI_FAILURE_NO_ADDRESS, /* joined, but DHCP gave no IPv4 address */
    CP_WIFI_FAILURE_TIMEOUT     /* found, but it never got past searching */
};
enum CpWifiFailure CpWifi_Failure(int notFound, unsigned furthestStage);

#endif
