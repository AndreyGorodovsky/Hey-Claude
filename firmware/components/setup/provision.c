/*
 * The hotspot and the settings page. See provision.h for what it does and
 * what it deliberately leaves out. Ported from the author's earlier project
 * (token_monitor), where its reasoning was worked out on hardware; the
 * comments keep that reasoning.
 *
 * How this file is organised, top to bottom:
 *
 *   1. state        the few variables, and why they need no lock
 *   2. identity     the hotspot's name and its random password
 *   3. the scan     the networks in range, and the one moment it is safe
 *   4. the page     the HTML, escaping, and why pages go out in pieces
 *   5. the save     reading the form, checking it, storing it
 *   6. start        WiFi, the hotspot, DHCP, the web server, the DNS
 *
 * Which task runs what. provision_start() runs in the setup loop's task
 * (main). The page handlers run in a task that esp_http_server, ESP-IDF's
 * web server, creates and owns, one request at a time. That is why the
 * variables below need no lock, and why a handler must not block for long:
 * while one runs, no other request is served.
 */
#include "provision.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "captive_dns.h"
#include "dhcpserver/dhcpserver.h"  /* OFFER_DNS: see the DHCP step below */
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"

static const char *TAG = "provision";

/* A number from a #define as text, at build time: STRINGIFY(32) is "32".
 * Two steps, so that a macro's value is turned into text and not its name.
 * Used for the page's length limits, which are app_config's. */
#define STRINGIFY_(x)       #x
#define STRINGIFY(x)        STRINGIFY_(x)

/* ---- 1. State ------------------------------------------------------------
 *
 * Written once by provision_start() before the web server exists, and only
 * read afterwards, except s_saved, written by the save handler once. */

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;

/* What the page is filled in with. Copied from the settings loaded at boot,
 * which in setup mode are the stored ones. The WiFi password and the token
 * are not copied, only whether they are set, so that no fault in a handler
 * can send either back over the air. */
static struct {
    char ssid[APP_CFG_WIFI_SSID_MAX + 1];
    char server_url[APP_CFG_SERVER_URL_MAX + 1];
    char device_id[APP_CFG_DEVICE_ID_MAX + 1];
    bool has_token;
} s_form;

/* The page's address as text, for the redirect */
static char s_address[16];

/* Set by the save handler after its reply has gone out; read by the setup
 * loop. One writer, one reader, one bool: `volatile` is all it needs. */
static volatile bool s_saved;
/* True while the save handler runs; read by the setup loop. Same reasoning. */
static volatile bool s_busy;

bool provision_saved(void)
{
    return s_saved;
}

bool provision_busy(void)
{
    return s_busy;
}

/* ---- 2. Identity ---------------------------------------------------------- */

/* The characters the hotspot's password is made of. Capitals and digits
 * only, so it reads the same on the screen however the font draws case, and
 * none that are easily confused at a glance: no O or 0, I or 1, S or 5, B or
 * 8, Z or 2. Someone reads this off a small screen and types it into a
 * phone. */
static const char PASS_ALPHABET[] = "ACDEFGHJKLMNPQRTUVWXY34679";
#define PASS_ALPHABET_LEN   (sizeof(PASS_ALPHABET) - 1)
/* make_password() draws 5 bits at a time, which reach only 32 characters;
 * a longer alphabet would quietly never use its end */
_Static_assert(PASS_ALPHABET_LEN <= 32, "make_password draws 5 bits at a time");

/* A new random password for each entry into setup mode. Not derived from
 * the chip's MAC address: that is broadcast in every beacon, so a password
 * made from it could be worked out by anyone in range. */
static void make_password(char *out, size_t cap)
{
    /* esp_random() is the chip's hardware random number generator, truly
     * random once the radio runs, as it does by now. 26 characters is not a
     * power of two, so `random % 26` would favour some; instead 5 bits are
     * taken at a time and any that point past the alphabet are thrown away. */
    size_t i = 0;
    while (i + 1 < cap) {
        uint32_t bits = esp_random();
        for (int shift = 0; shift <= 25 && i + 1 < cap; shift += 5) {
            uint32_t idx = (bits >> shift) & 0x1F;
            if (idx < PASS_ALPHABET_LEN) {
                out[i++] = PASS_ALPHABET[idx];
            }
        }
    }
    out[i] = '\0';
}

/* "Hey-Claude-A3F2": recognisable in a phone's list of networks, with the
 * last two bytes of the hotspot's MAC address so that two devices in one
 * room stay apart. The name may come from the MAC; it is public anyway. */
static void make_ssid(char *out, size_t cap)
{
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        snprintf(out, cap, "Hey-Claude");
        return;
    }
    snprintf(out, cap, "Hey-Claude-%02X%02X", mac[4], mac[5]);
}

/* ---- 3. The scan -----------------------------------------------------------
 *
 * Typing a network's name from memory is the likeliest way to get setup
 * wrong: names are case-sensitive, and a wrong one shows up only minutes
 * later as "network not found". The page offers the names in range instead.
 *
 * When the scan runs is the whole design. A scan visits every channel in
 * turn, and an access point that went with it would stop answering its
 * phone, so it runs before the hotspot exists, while the chip is still a
 * station. A failed scan costs only the list; the field can still be typed
 * into, which a hidden network, never in a scan, needs anyway. */

/* Enough for a crowded block of flats; beyond this a list stops helping */
#define SCAN_MAX_SHOWN      12

static char s_scan[SCAN_MAX_SHOWN][APP_CFG_WIFI_SSID_MAX + 1];
static size_t s_scan_count;

static bool already_listed(const char *ssid)
{
    for (size_t i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan[i], ssid) == 0) {
            return true;
        }
    }
    return false;
}

static void scan_for_networks(void)
{
    /* `true`: wait for the results, so nothing else is under way when the
     * radio changes mode afterwards */
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s; the page will offer no networks",
                 esp_err_to_name(err));
        return;
    }
    /* The driver keeps the results until they are fetched, and fetching
     * frees them; asking for fewer than were found is allowed. They come
     * strongest first, so the nearest networks, likeliest to be the right
     * one, lead the list. */
    uint16_t wanted = SCAN_MAX_SHOWN;
    wifi_ap_record_t *recs = calloc(wanted, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();   /* the driver's copy is freed either way */
        return;
    }
    if (esp_wifi_scan_get_ap_records(&wanted, recs) == ESP_OK) {
        for (uint16_t i = 0; i < wanted && s_scan_count < SCAN_MAX_SHOWN; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            /* A hidden network has an empty name; a mesh or a repeater puts
             * one name on several radios */
            if (ssid[0] != '\0' && !already_listed(ssid)) {
                snprintf(s_scan[s_scan_count++], sizeof(s_scan[0]), "%s", ssid);
            }
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    free(recs);
    ESP_LOGI(TAG, "%u networks in range offered", (unsigned)s_scan_count);
}

/* ---- 4. The page -----------------------------------------------------------
 *
 * Everything is inline: no stylesheet, font or icon from elsewhere. A phone
 * on this hotspot cannot reach the internet, so any such reference would be
 * a wait and a timeout before the page appeared.
 *
 * One rule below is functional: inputs are 16 px. A phone's browser zooms
 * the whole page when a smaller field is touched, and does not zoom back. */
static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Hey Claude setup</title><style>"
    "body{font:16px system-ui,sans-serif;margin:0;padding:24px 18px;"
    "background:#141414;color:#ececec}"
    "h1{font-size:20px;margin:0 0 4px}"
    "p.sub{margin:0 0 24px;color:#9a9a9a;font-size:14px}"
    "label{display:block;margin:0 0 18px}"
    "span{display:block;margin-bottom:6px;font-size:13px;color:#9a9a9a}"
    "input{width:100%;box-sizing:border-box;padding:11px;font-size:16px;"
    "border:1px solid #3a3a3a;border-radius:8px;background:#1f1f1f;color:#ececec}"
    "button{width:100%;padding:13px;font-size:16px;font-weight:600;border:0;"
    "border-radius:8px;background:#d97757;color:#141414}"
    "small{color:#7a7a7a}"
    ".nets{margin:10px 0 0}"
    ".chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:6px}"
    ".net{width:auto;padding:8px 12px;font-size:14px;font-weight:400;"
    "background:#1f1f1f;color:#d4d4d4;border:1px solid #3a3a3a}"
    ".check{display:flex;align-items:center;gap:10px}"
    ".check span{display:inline;margin:0;font-size:15px;color:#ececec}"
    ".check input{width:auto;flex:none}"
    ".bad{background:#3a1f1f;border:1px solid #7a3030;padding:12px;"
    "border-radius:8px;margin-bottom:20px}"
    ".note{background:#1f1f1f;border:1px solid #3a3a3a;padding:12px;"
    "border-radius:8px;margin-bottom:20px;font-size:14px}"
    "a{color:#e8a184}"
    "</style></head><body>";

static const char PAGE_TAIL[] = "</body></html>";

/* Copies `in` into `out` with the characters that mean something in HTML
 * replaced, cutting it short rather than overrunning `out`. Needed wherever
 * a value goes into the page: a network's name is any bytes at all, and one
 * with a double quote in it would otherwise end the attribute early. A cut
 * is acceptable only because the result is a field the person can see and
 * correct before saving. */
static void html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p != '\0'; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        default:   break;
        }
        size_t n = rep ? strlen(rep) : 1;
        if (o + n >= cap) {
            break;
        }
        if (rep) {
            memcpy(out + o, rep, n);
        } else {
            out[o] = *p;
        }
        o += n;
    }
    out[o] = '\0';
}

/* The worst case is every character becoming "&quot;", six bytes */
#define ESCAPED_CAP(n)      ((n) * 6 + 1)

/* Pages go out in pieces ("chunks"): fixed text, then a value, then more
 * text, each sent as it is ready, rather than built whole in a buffer.
 * Escaping can make a value six times longer, and a buffer large enough for
 * a page around the longest would not fit comfortably on the server task's
 * stack.
 *
 * An empty piece must never be sent: a piece of length zero is how HTTP
 * marks the end of the page, and the browser would stop reading there. On a
 * device with nothing set yet, an empty value is the usual case, so every
 * value goes through this. The real end is sent with NULL, at the end. */
static void send_value(httpd_req_t *req, const char *s)
{
    if (s[0] != '\0') {
        httpd_resp_sendstr_chunk(req, s);
    }
}

/* Each handler gets `req`, the request, which is both what was asked and
 * where the reply goes. Returning ESP_OK means answered; an error makes the
 * server close the connection. */
static esp_err_t form_get(httpd_req_t *req)
{
    /* Reused for each value in turn, sized for the longest, the server's
     * address. On the server task's stack, which provision_start() sizes
     * for it. */
    char esc[ESCAPED_CAP(APP_CFG_SERVER_URL_MAX)];

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<h1>Hey Claude</h1>"
        "<p class=\"sub\">The WiFi network, and the token for the server.</p>"
        "<form method=\"post\" action=\"/save\">"
        "<label><span>WiFi network</span><input name=\"ssid\" value=\"");
    html_escape(s_form.ssid, esc, sizeof(esc));
    send_value(req, esc);
    httpd_resp_sendstr_chunk(req,
        "\" maxlength=\"" STRINGIFY(APP_CFG_WIFI_SSID_MAX) "\" required autocapitalize=\"off\" "
        "autocorrect=\"off\" spellcheck=\"false\">");

    /* The networks in range, as buttons that fill the field in. Not a
     * <datalist>, which browsers filter against what the field already
     * holds, so that a filled-in field offered only itself. The name goes
     * into a data- attribute, as inert text, and never into the script: a
     * name taken off the air inside a script is a way for anyone nearby to
     * run code in the page. */
    if (s_scan_count > 0) {
        httpd_resp_sendstr_chunk(req,
            "<div class=\"nets\"><small>Networks in range, tap one:</small>"
            "<div class=\"chips\">");
        for (size_t i = 0; i < s_scan_count; i++) {
            html_escape(s_scan[i], esc, sizeof(esc));
            httpd_resp_sendstr_chunk(req, "<button type=\"button\" class=\"net\" data-ssid=\"");
            send_value(req, esc);
            httpd_resp_sendstr_chunk(req, "\">");
            send_value(req, esc);
            httpd_resp_sendstr_chunk(req, "</button>");
        }
        httpd_resp_sendstr_chunk(req, "</div></div>");
    }
    httpd_resp_sendstr_chunk(req, "</label>");

    /* The password field is always empty, and empty keeps the stored one.
     * The box says what empty cannot: that the network has no password. */
    httpd_resp_sendstr_chunk(req,
        "<label><span>WiFi password</span>"
        "<input name=\"pass\" type=\"password\" maxlength=\"" STRINGIFY(APP_CFG_WIFI_PASS_MAX) "\" "
        "placeholder=\"leave empty to keep the current one\" "
        "autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\"></label>"
        "<label class=\"check\"><input name=\"open\" type=\"checkbox\" value=\"1\">"
        "<span>This network has no password</span></label>"
        "<label><span>Device token</span>"
        "<input name=\"token\" type=\"password\" maxlength=\""
        STRINGIFY(APP_CFG_DEVICE_TOKEN_MAX) "\" placeholder=\"");
    httpd_resp_sendstr_chunk(req, s_form.has_token ? "leave empty to keep the current one"
                                                   : "the token the server has for this device");
    httpd_resp_sendstr_chunk(req,
        "\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
        "<small>The server must have the same token for this device, named <b>");
    html_escape(s_form.device_id, esc, sizeof(esc));
    send_value(req, esc);
    httpd_resp_sendstr_chunk(req,
        "</b>, in its DEVICE_TOKENS.</small></label>"
        "<label><span>Server address (optional)</span><input name=\"url\" value=\"");
    html_escape(s_form.server_url, esc, sizeof(esc));
    send_value(req, esc);
    httpd_resp_sendstr_chunk(req,
        "\" maxlength=\"" STRINGIFY(APP_CFG_SERVER_URL_MAX) "\" "
        "placeholder=\"found on the network by itself\" "
        "autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
        "<small>Only if the server is not found by itself: address:port</small></label>"
        "<button type=\"submit\">Save</button></form>"
        "<p><small>Saving restarts the device with the new settings.</small></p>");

    /* One listener per button, reading the name back out of its attribute.
     * Without scripting the buttons do nothing and the field still works. */
    if (s_scan_count > 0) {
        httpd_resp_sendstr_chunk(req,
            "<script>document.querySelectorAll('.net').forEach(function(b){"
            "b.addEventListener('click',function(){"
            "document.getElementsByName('ssid')[0].value=b.dataset.ssid;});});</script>");
    }
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);    /* the end of the page */
    return ESP_OK;
}

/* Browsers ask for /favicon.ico by themselves. "Nothing here, and that is
 * fine" keeps two warnings a page out of the log. */
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* Anything else is sent to the page. This is the web half of the captive
 * portal: a phone's internet check lands here (captive_dns.h), and a
 * redirect is what tells it there is a sign-in page to open. A 404 would make
 * it decide the network is broken instead. Absolute, because a relative
 * address would be taken against the name the phone asked for, and come
 * straight back here. */
static esp_err_t redirect_get(httpd_req_t *req)
{
    char location[32];
    snprintf(location, sizeof(location), "http://%s/", s_address);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, "<!doctype html><meta charset=\"utf-8\">"
                                   "<p><a href=\"/\">Hey Claude setup</a></p>");
}

/* ---- 5. The save ----------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A form arrives as key=value pairs joined by '&', with spaces as '+' and
 * other characters as %XX. ESP-IDF's httpd_query_key_value() finds a pair
 * but leaves its value encoded, so a password "a b&c" would arrive as
 * "a+b%26c". This finds and decodes.
 *
 * Returns 0 when found, -1 when absent, -2 when too long for `out`. Never
 * cuts a value short: these are on their way into credentials. The search
 * looks only at the start of each pair, so "pass" cannot match the end of
 * "bypass". */
static int form_field(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p != '\0') {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p != '\0' && *p != '&') {
                char c = *p++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    /* p[1] is read only once p[0] is a valid digit, so a '%'
                     * at the very end never reads past the terminating NUL.
                     * A malformed escape passes through as a plain '%'. */
                    int hi = hexval(p[0]);
                    int lo = hi < 0 ? -1 : hexval(p[1]);
                    if (lo >= 0) {
                        c = (char)((hi << 4) | lo);
                        p += 2;
                    }
                }
                if (o + 1 >= cap) {
                    return -2;
                }
                out[o++] = c;
            }
            out[o] = '\0';
            return 0;
        }
        const char *amp = strchr(p, '&');
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    return -1;
}

/* Wraps a short, fixed piece of HTML in the shared head and tail */
static esp_err_t send_page(httpd_req_t *req, const char *body_html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, body_html);
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* Says why a form was not accepted, with a way back. `problem` is the
 * device's own wording, never anything the phone sent, so it needs no
 * escaping. */
static esp_err_t send_problem(httpd_req_t *req, const char *problem)
{
    char page[384];
    snprintf(page, sizeof(page),
             "<h1>Not saved</h1><div class=\"bad\">%s</div>"
             "<p><a href=\"/\">Back to the form</a></p>", problem);
    ESP_LOGW(TAG, "a form was not accepted: %s", problem);
    httpd_resp_set_status(req, "400 Bad Request");
    return send_page(req, page);
}

/* The form, percent-encoded at worst: every byte of the four values as %XX,
 * plus the names. A larger body did not come from this page. Static, not on
 * the stack: the server runs one handler at a time, so one copy serves. */
static char s_body[(APP_CFG_WIFI_SSID_MAX + APP_CFG_WIFI_PASS_MAX + APP_CFG_DEVICE_TOKEN_MAX
                    + APP_CFG_SERVER_URL_MAX) * 3 + 64];

static esp_err_t handle_save(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= sizeof(s_body)) {
        return send_problem(req, "The form arrived empty or far too large.");
    }
    /* One receive can return less than was sent, so this reads until the
     * whole body is in. A timeout means a slow phone, and is retried a few
     * times; a phone that announced a body and then went away (its screen
     * locked, or it walked out of range) would otherwise hold the server,
     * which serves one request at a time, for as long as the socket lived. */
    int received = 0;
    int timeouts = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, s_body + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                ESP_LOGW(TAG, "the phone stopped sending the form");
                return ESP_FAIL;
            }
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;            /* the connection is gone; nobody to answer */
        }
        received += r;
        timeouts = 0;
    }
    s_body[received] = '\0';

    char ssid[APP_CFG_WIFI_SSID_MAX + 1];
    char pass[APP_CFG_WIFI_PASS_MAX + 1];
    char token[APP_CFG_DEVICE_TOKEN_MAX + 1];
    char url[APP_CFG_SERVER_URL_MAX + 1];
    char open_box[4];
    if (form_field(s_body, "ssid", ssid, sizeof(ssid)) != 0
        || form_field(s_body, "pass", pass, sizeof(pass)) != 0
        || form_field(s_body, "token", token, sizeof(token)) != 0
        || form_field(s_body, "url", url, sizeof(url)) != 0) {
        /* The page limits each field's length, so this is a form from
         * elsewhere, or a browser that ignored the limits */
        return send_problem(req, "A field is missing or too long.");
    }
    /* A box left unticked is not sent at all; present means ticked */
    bool open_network = form_field(s_body, "open", open_box, sizeof(open_box)) == 0;

    if (ssid[0] == '\0') {
        return send_problem(req, "The WiFi network is needed.");
    }
    if (open_network && pass[0] != '\0') {
        return send_problem(req,
            "The box says the network has no password, but a password was typed. "
            "Untick the box to use it, or clear the password.");
    }
    /* "Empty keeps the stored password" is right while the network stays
     * the same, and a trap when it changes: the new network would be tried
     * with the old one's password, and fail looking like a wrong name. On a
     * device with no network stored, empty means an open network, since
     * there is nothing to keep. */
    if (!open_network && pass[0] == '\0' && s_form.ssid[0] != '\0'
        && strcmp(ssid, s_form.ssid) != 0) {
        return send_problem(req,
            "The WiFi network changed, so its password is needed too. An empty "
            "password keeps the one already stored, which is the old network's.");
    }
    if (token[0] == '\0' && !s_form.has_token) {
        return send_problem(req, "The device token is needed.");
    }

    /* Only what changes. A password left empty and a token left empty are
     * not in the list, and so stay as they are. */
    app_config_edit_t edits[4];
    size_t n = 0;
    edits[n++] = (app_config_edit_t){ "wifi_ssid", ssid };
    if (open_network) {
        edits[n++] = (app_config_edit_t){ "wifi_pass", NULL };
    } else if (pass[0] != '\0') {
        edits[n++] = (app_config_edit_t){ "wifi_pass", pass };
    }
    if (token[0] != '\0') {
        edits[n++] = (app_config_edit_t){ "device_token", token };
    }
    edits[n++] = (app_config_edit_t){ "server_url", url[0] ? url : NULL };

    /* Nothing above has written anything. The same checks as the console
     * now run on every value, and all are stored or none. Whether each
     * secret was typed is logged, never the secret. */
    ESP_LOGI(TAG, "form: network \"%s\", password %s, token %s, server %s",
             ssid,
             open_network ? "none" : pass[0] ? "typed" : "kept",
             token[0] ? "typed" : "kept",
             url[0] ? url : "found by itself");
    const char *bad_key = NULL;
    const char *reason = NULL;
    esp_err_t err = app_config_store(edits, n, &bad_key, &reason);
    if (err == ESP_ERR_INVALID_ARG) {
        char msg[192];
        snprintf(msg, sizeof(msg), "%s %s.", app_config_key_label(bad_key), reason);
        return send_problem(req, msg);
    }
    if (err != ESP_OK) {
        /* Said on the page, not only in the log: someone who typed a
         * password and was told nothing would assume it worked */
        char msg[160];
        snprintf(msg, sizeof(msg), "Storing the settings failed (%s). The device "
                 "keeps its previous settings.", esp_err_to_name(err));
        return send_problem(req, msg);
    }

    send_page(req,
        "<h1>Saved</h1>"
        "<div class=\"note\">The device is restarting with these settings. Its "
        "hotspot disappears in a few seconds; that is the restart, and the phone "
        "goes back to its usual network.</div>"
        "<p>A WiFi password or a token that is refused brings the device back to "
        "setup by itself within a minute, saying why on its screen. If it stays on "
        "Connecting, hold its BOOT button for 3 seconds to come back here.</p>");

    /* Only now that the reply has gone: the setup loop restarts within a
     * second of seeing this, and set earlier, the restart could beat the page
     * to the phone */
    s_saved = true;
    return ESP_OK;
}

/* The save, with the setup loop told it is under way, whichever way it
 * ends */
static esp_err_t save_post(httpd_req_t *req)
{
    s_busy = true;
    esp_err_t err = handle_save(req);
    s_busy = false;
    return err;
}

/* ---- 6. Start --------------------------------------------------------------- */

/* Logged so that the console shows a phone joining: "the page does not
 * load" and "the phone never joined" look the same from the desk, and need
 * different fixes */
static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a phone joined the hotspot");
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "a phone left the hotspot");
    }
}

/* Starts WiFi as a station, only for the scan */
static esp_err_t scan_as_station(void)
{
    /* The station's network interface: the scan does not use it, but the
     * driver's default handlers expect one */
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    /* RAM: by default the driver saves its settings to flash, in a
     * namespace of its own. The hotspot's password lives for minutes and
     * must not outlast them there, readable by anyone with a USB cable. */
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    scan_for_networks();
    return esp_wifi_stop();
}

/* Raises the hotspot. `out` has its name and password. */
static esp_err_t raise_hotspot(const provision_info_t *out)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                               on_ap_event, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                         on_ap_event, NULL);
    }
    if (err != ESP_OK) {
        return err;
    }
    /* = { 0 } leaves every field not set here at its default, and the name
     * and password padded with NULs after the copies */
    wifi_config_t ap = { 0 };
    size_t ssid_len = strlen(out->ssid);
    memcpy(ap.ap.ssid, out->ssid, ssid_len);
    ap.ap.ssid_len = (uint8_t)ssid_len;
    memcpy(ap.ap.password, out->password, strlen(out->password));
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;           /* a phone, and one spare */
    /* WPA2 with the password on the screen. An open hotspot would let anyone
     * in range read the page, and so the network's name, or store settings
     * that send the device to a server of their own. */
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK) err = esp_wifi_start();
    return err;
}

/* Has the hotspot's DHCP server, which gives a joining phone its address,
 * also tell it to ask this device for names. Without it a phone keeps a
 * public DNS server it cannot reach, and the sign-in page never opens. The
 * DHCP server must be stopped while this changes, or the change is accepted
 * and ignored. Nothing here is needed for the page itself, which works at
 * the address on the screen, except restarting the DHCP server. */
static esp_err_t offer_dns(uint32_t ip)
{
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip;
    if (esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        ESP_LOGW(TAG, "the DNS server could not be set");
    }
    uint8_t offer = OFFER_DNS;
    if (esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                               &offer, sizeof(offer)) != ESP_OK) {
        ESP_LOGW(TAG, "the DNS server could not be offered");
    }
    return esp_netif_dhcps_start(s_ap_netif);
}

static esp_err_t start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* The form's handler holds a 763-byte escaping buffer and a chunk being
     * sent at once; the default 4 KB is close, and an overflow restarts the
     * chip out of setup mode with no word on the screen */
    cfg.stack_size = 6144;
    /* A phone that leaves connections open must not lock everyone out */
    cfg.lru_purge_enable = true;
    /* Without it the catch-all route below would match only a path named "*" */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* Copied by the server, so locals would do. The catch-all is registered
     * last: routes are tried in order, and first it would swallow the page
     * itself into a redirect to itself. */
    const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = form_get },
        { .uri = "/save",        .method = HTTP_POST, .handler = save_post },
        { .uri = "/favicon.ico", .method = HTTP_GET,  .handler = favicon_get },
        { .uri = "/*",           .method = HTTP_GET,  .handler = redirect_get },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]) && err == ESP_OK; i++) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
    }
    return err;
}

esp_err_t provision_start(provision_info_t *out)
{
    memset(out, 0, sizeof(*out));
    const app_config_t *cfg = app_config_get();
    snprintf(s_form.ssid, sizeof(s_form.ssid), "%s", cfg->wifi_ssid);
    snprintf(s_form.server_url, sizeof(s_form.server_url), "%s", cfg->server_url);
    snprintf(s_form.device_id, sizeof(s_form.device_id), "%s", cfg->device_id);
    s_form.has_token = cfg->device_token[0] != '\0';

    /* Not ESP_ERROR_CHECK anywhere here: these allocate, and a failure is
     * returned so that the screen can say so, instead of a crash */
    esp_err_t err = scan_as_station();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi did not start: %s", esp_err_to_name(err));
        return err;
    }
    make_ssid(out->ssid, sizeof(out->ssid));
    make_password(out->password, sizeof(out->password));
    err = raise_hotspot(out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the hotspot did not start: %s", esp_err_to_name(err));
        return err;
    }

    /* The address is asked of the interface, not assumed, because it goes
     * on the screen as an instruction. If it cannot be read, the default is
     * used for the number as well as the text, so that the screen, the DHCP
     * server and the DNS answers agree. */
    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(s_ap_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        ip.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    }
    snprintf(out->address, sizeof(out->address), IPSTR, IP2STR(&ip.ip));
    snprintf(s_address, sizeof(s_address), "%s", out->address);

    err = offer_dns(ip.ip.addr);
    if (err == ESP_OK) {
        err = start_server();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the page could not be served: %s", esp_err_to_name(err));
        return err;
    }
    /* Last, and optional: without it the page is still at the address */
    if (captive_dns_start(ip.ip.addr) != ESP_OK) {
        ESP_LOGW(TAG, "no captive portal; the page is at http://%s/", out->address);
    }
    /* The hotspot's password is logged, unlike every other secret here: it
     * is random, lives for minutes, and is on the screen in front of whoever
     * reads this console */
    ESP_LOGI(TAG, "join \"%s\" with password %s, then open http://%s/",
             out->ssid, out->password, out->address);
    return ESP_OK;
}
