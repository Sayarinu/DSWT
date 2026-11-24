#ifndef COMPANION_PLATFORM_NDS_WIFI_H
#define COMPANION_PLATFORM_NDS_WIFI_H

#include <stddef.h>
#include <stdint.h>

/* Wi-Fi bring-up with DSWiFi. In DSi mode this uses the DSi's own Wi-Fi
 * (WPA2 capable); in DS mode DSWiFi falls back to the DS driver (open and WEP
 * only). No UI: the Wi-Fi setup screen (LUNE-875) drives this. */

enum CpWifiStatus {
    CP_WIFI_OFF,         /* not initialised, or initialisation failed */
    CP_WIFI_IDLE,        /* on, not connected or connecting */
    CP_WIFI_CONNECTING,  /* searching, authenticating, associating or DHCP */
    CP_WIFI_CONNECTED,   /* associated with an IPv4 address */
    CP_WIFI_FAILED       /* the last connection attempt failed or timed out */
};

/* Start the driver and enable the radio. Zero on success. The caller has
 * already started libnds's systemCounter, which times connections. */
int CpWifi_Init(void);
/* Nonzero when DSWiFi runs the DSi driver: WIFI_ATTEMPT_DSI_MODE falls back
 * to the DS driver only outside DSi mode. */
int CpWifi_DsiMode(void);
/* The console's MAC address, valid after CpWifi_Init. */
void CpWifi_Mac(unsigned char mac[6]);

/* Connect with the console's saved connections (Wifi_AutoConnect). On a DSi or
 * 3DS in DSi mode this includes the WPA2 slots. Poll CpWifi_Status. */
int CpWifi_AutoConnect(void);
/* Connect to a network by name with a typed passphrase (an empty one for an
 * open network). The network must be in range; it is looked up by a scan of
 * up to scanMs milliseconds. */
int CpWifi_ConnectPassphrase(const char *ssid, const char *passphrase, unsigned scanMs);

/* Poll the connection; a connection that takes longer than the timeout given
 * to CpWifi_SetTimeout (45 s by default) counts as failed. */
enum CpWifiStatus CpWifi_Status(void);
void CpWifi_SetTimeout(unsigned ms);
/* DSWiFi's association state string, for diagnostics. */
const char *CpWifi_StatusText(void);
/* The IPv4 address in network order, 0 while not connected. */
uint32_t CpWifi_Address(void);
void CpWifi_Disconnect(void);

#endif
