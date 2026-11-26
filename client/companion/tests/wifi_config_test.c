/* wifi.cfg: the network the Wi-Fi settings store. Its text round-trips,
 * malformed files are refused whole, and it is written through a .tmp that
 * a power cut may leave behind. */
#include "platform/wifi_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char sDir[64], sPath[128], sTmp[160];

static int Parse(const char *text, CpWifiConfig *out)
{
    return CpWifiConfig_Parse(text, strlen(text), out);
}

static void WriteText(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    fputs(text, file);
    fclose(file);
}

static int Exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static CpWifiConfig Network(unsigned security, const char *ssid, const char *key)
{
    CpWifiConfig config;
    memset(&config, 0, sizeof(config));
    config.security = security;
    snprintf(config.ssid, sizeof(config.ssid), "%s", ssid);
    snprintf(config.key, sizeof(config.key), "%s", key);
    return config;
}

static void TestParse(void)
{
    CpWifiConfig config;
    assert(Parse("version=1\nsecurity=wpa2\nssid=Home Net\nkey=correct horse\n", &config) == 0);
    assert(config.security == CP_WIFI_SECURITY_WPA2 && !strcmp(config.ssid, "Home Net")
        && !strcmp(config.key, "correct horse"));
    /* Comments, blank lines, CRLF, no final newline, unknown names, any order. */
    assert(Parse("# saved\r\n\r\nssid=Cafe\r\ncolour=blue\r\nkey=abcde\r\nsecurity=wep\r\nversion=1", &config) == 0);
    assert(config.security == CP_WIFI_SECURITY_WEP && !strcmp(config.ssid, "Cafe") && !strcmp(config.key, "abcde"));
    /* An open network needs no key. */
    assert(Parse("version=1\nsecurity=open\nssid=Guest\n", &config) == 0 && config.key[0] == 0);
    /* Values keep their spaces, '=' and '#'; escapes give back any byte. */
    assert(Parse("version=1\nsecurity=wpa2\nssid= a=b #c \nkey=pass word=#1\n", &config) == 0);
    assert(!strcmp(config.ssid, " a=b #c ") && !strcmp(config.key, "pass word=#1"));
    assert(Parse("version=1\nsecurity=wpa2\nssid=tab\\x09\\\\end\nkey=12345678\n", &config) == 0);
    assert(!strcmp(config.ssid, "tab\t\\end"));
    /* UTF-8 names are bytes like any other. */
    assert(Parse("version=1\nsecurity=wpa2\nssid=Caf\xc3\xa9\nkey=12345678\n", &config) == 0);
    assert(!strcmp(config.ssid, "Caf\xc3\xa9"));
}

static void TestRefusals(void)
{
    static const char *const bad[] = {
        "",                                                        /* nothing */
        "security=wpa2\nssid=a\nkey=12345678\n",                   /* no version */
        "version=2\nsecurity=wpa2\nssid=a\nkey=12345678\n",        /* a future version */
        "version=1\nssid=a\nkey=12345678\n",                       /* no security */
        "version=1\nsecurity=wpa\nssid=a\nkey=12345678\n",         /* TKIP is never stored */
        "version=1\nsecurity=wpa2\nkey=12345678\n",                /* no ssid */
        "version=1\nsecurity=wpa2\nssid=\nkey=12345678\n",         /* an empty ssid */
        "version=1\nsecurity=wpa2\nssid=a\n",                      /* WPA2 without a key */
        "version=1\nsecurity=wpa2\nssid=a\nkey=short\n",           /* too short for WPA2 */
        "version=1\nsecurity=wep\nssid=a\nkey=123456\n",           /* not a WEP length */
        "version=1\nsecurity=open\nssid=a\nkey=12345678\n",        /* a key for an open network */
        "version=1\nsecurity=wpa2\nssid=a\nssid=b\nkey=12345678\n", /* a repeated name */
        "version=1\nversion=1\nsecurity=wpa2\nssid=a\nkey=12345678\n",
        "version=1\nsecurity=wpa2\nssid=a\nkey=12345678\ngarbage\n", /* a line without '=' */
        "version=1\nsecurity=wpa2\nssid=123456789012345678901234567890123\nkey=12345678\n", /* 33 bytes */
        "version=1\nsecurity=wpa2\nssid=a\\x00b\nkey=12345678\n",  /* an escaped NUL */
        "version=1\nsecurity=wpa2\nssid=a\\q\nkey=12345678\n",     /* an unknown escape */
        "version=1\nsecurity=wpa2\nssid=a\\x4\nkey=12345678\n",    /* a short escape */
        "version=1\nsecurity=wpa2\nssid=a\tb\nkey=12345678\n",     /* a raw control byte */
        "version=1\nsecurity=wpa2\nssid=a\nkey=pass\\x0aword\n",   /* a key that is not printable */
        "version=1\nsecurity=wpa2\nssid=a\nkey=\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\n", /* a non-ASCII key */
    };
    CpWifiConfig config;
    char big[CP_WIFI_CONFIG_FILE_MAX + 64], line[600];
    unsigned i;
    for (i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        memset(&config, 0x5A, sizeof(config));
        if (Parse(bad[i], &config) == 0) {
            fprintf(stderr, "accepted case %u\n", i);
            assert(0);
        }
    }
    /* An embedded NUL, a file past the limit and an overlong line. */
    assert(CpWifiConfig_Parse("version=1\0\nsecurity=open\nssid=a\n", 31, &config) != 0);
    memset(big, '#', sizeof(big));
    memcpy(big, "version=1\nsecurity=open\nssid=a\n", 31);
    big[31] = '#';
    assert(CpWifiConfig_Parse(big, sizeof(big), &config) != 0);
    memset(line, 'a', sizeof(line));
    memcpy(line, "version=1\nsecurity=open\nssid=a\n#", 32);
    line[sizeof(line) - 1] = 0;
    assert(Parse(line, &config) != 0);
}

static void TestFormat(void)
{
    static const char ssids[][CP_WIFI_SSID_MAX + 1] = {
        "Home", " spaced ", "back\\slash", "tab\tnew\nline\x7f", "12345678901234567890123456789012",
    };
    char text[CP_WIFI_CONFIG_FILE_MAX + 1];
    unsigned i;
    for (i = 0; i < sizeof(ssids) / sizeof(*ssids); ++i) {
        CpWifiConfig in = Network(CP_WIFI_SECURITY_WPA2, ssids[i], "p@ss w0rd\\=#"), out;
        size_t size = CpWifiConfig_Format(&in, text, sizeof(text));
        assert(size > 0 && size == strlen(text));
        assert(strstr(text, "plain text"));
        assert(CpWifiConfig_Parse(text, size, &out) == 0);
        assert(out.security == in.security && !strcmp(out.ssid, in.ssid) && !strcmp(out.key, in.key));
    }
    {
        CpWifiConfig open = Network(CP_WIFI_SECURITY_OPEN, "Guest", ""), hex, out;
        char key[65];
        memset(key, 'a', 64);
        key[64] = 0;
        hex = Network(CP_WIFI_SECURITY_WPA2, "Hex", key);
        assert(CpWifiConfig_Format(&open, text, sizeof(text)) > 0 && CpWifiConfig_Parse(text, strlen(text), &out) == 0);
        assert(out.security == CP_WIFI_SECURITY_OPEN);
        /* A raw 64-hex-digit WPA2 key is neither stored nor read back:
         * DSWiFi treats every key as a passphrase. */
        assert(CpWifiConfig_Format(&hex, text, sizeof(text)) == 0);
        snprintf(text, sizeof(text), "version=1\nsecurity=wpa2\nssid=Hex\nkey=%s\n", key);
        assert(CpWifiConfig_Parse(text, strlen(text), &out) != 0);
    }
    {
        /* What cannot be stored is refused, not written half. */
        CpWifiConfig noName = Network(CP_WIFI_SECURITY_WPA2, "", "12345678");
        CpWifiConfig badKey = Network(CP_WIFI_SECURITY_WPA2, "a", "short");
        CpWifiConfig tkip = Network(CP_WIFI_SECURITY_WPA, "a", "12345678");
        assert(CpWifiConfig_Format(&noName, text, sizeof(text)) == 0);
        assert(CpWifiConfig_Format(&badKey, text, sizeof(text)) == 0);
        assert(CpWifiConfig_Format(&tkip, text, sizeof(text)) == 0);
        assert(CpWifiConfig_Format(&noName, text, 8) == 0);
    }
}

static void TestStore(void)
{
    CpWifiConfig first = Network(CP_WIFI_SECURITY_WPA2, "First", "first-pass"),
        second = Network(CP_WIFI_SECURITY_WEP, "Second", "0123456789"), out;
    assert(CpWifiConfig_Load(sPath, &out) == 1);
    assert(CpWifiConfig_Save(sPath, &first) == 0);
    assert(Exists(sPath) && !Exists(sTmp));
    assert(CpWifiConfig_Load(sPath, &out) == 0 && !strcmp(out.ssid, "First") && !strcmp(out.key, "first-pass"));
    /* Saving again replaces it, as FAT needs: remove, then rename. */
    assert(CpWifiConfig_Save(sPath, &second) == 0);
    assert(CpWifiConfig_Load(sPath, &out) == 0 && !strcmp(out.ssid, "Second") && out.security == CP_WIFI_SECURITY_WEP);
    assert(!Exists(sTmp));

    /* A power cut while the .tmp was written: wifi.cfg still holds the
     * previous network and the half file is ignored. */
    WriteText(sTmp, "version=1\nsecurity=wpa2\nssid=Ha");
    assert(CpWifiConfig_Load(sPath, &out) == 0 && !strcmp(out.ssid, "Second"));
    /* A power cut between remove and rename: only the complete .tmp is left. */
    remove(sPath);
    WriteText(sTmp, "version=1\nsecurity=wpa2\nssid=Third\nkey=third-pass\n");
    assert(CpWifiConfig_Load(sPath, &out) == 0 && !strcmp(out.ssid, "Third"));
    /* A half .tmp alone is not a network. */
    WriteText(sTmp, "version=1\nsecurity=wpa2\nssid=Th");
    assert(CpWifiConfig_Load(sPath, &out) == -1);
    /* A damaged wifi.cfg is reported, not used. */
    WriteText(sPath, "version=1\nsecurity=wpa2\nssid=x\nkey=");
    assert(CpWifiConfig_Load(sPath, &out) == -1);

    /* Forget removes both files; forgetting nothing is fine. */
    assert(CpWifiConfig_Forget(sPath) == 0);
    assert(!Exists(sPath) && !Exists(sTmp));
    assert(CpWifiConfig_Load(sPath, &out) == 1);
    assert(CpWifiConfig_Forget(sPath) == 0);

    /* A destination rename cannot replace, as on FAT (here an empty folder
     * in its place): it is removed first, then the .tmp renamed. */
    assert(mkdir(sPath, 0700) == 0);
    assert(CpWifiConfig_Save(sPath, &first) == 0);
    assert(CpWifiConfig_Load(sPath, &out) == 0 && !strcmp(out.ssid, "First") && !Exists(sTmp));
    assert(CpWifiConfig_Forget(sPath) == 0);

    /* A folder that does not exist: the save fails and leaves nothing. */
    {
        char missing[200];
        snprintf(missing, sizeof(missing), "%s/no/such/wifi.cfg", sDir);
        assert(CpWifiConfig_Save(missing, &first) != 0);
    }
}

static void TestWipe(void)
{
    char secret[16] = "hunter2hunter2";
    unsigned i;
    CpWifiConfig_Wipe(secret, sizeof(secret));
    for (i = 0; i < sizeof(secret); ++i) assert(secret[i] == 0);
}

int main(void)
{
    char template[] = "/tmp/dswt-wifi-XXXXXX";
    assert(mkdtemp(template));
    snprintf(sDir, sizeof(sDir), "%s", template);
    snprintf(sPath, sizeof(sPath), "%s/wifi.cfg", sDir);
    snprintf(sTmp, sizeof(sTmp), "%s.tmp", sPath);
    TestParse();
    TestRefusals();
    TestFormat();
    TestStore();
    TestWipe();
    remove(sPath);
    remove(sTmp);
    rmdir(sDir);
    puts("wifi_config_test: ok");
    return 0;
}
