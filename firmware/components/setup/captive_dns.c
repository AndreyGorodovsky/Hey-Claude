/*
 * The DNS responder behind the captive portal. See captive_dns.h for why.
 * Ported from the author's earlier project (token_monitor).
 *
 * A DNS message, which is all this file needs to know:
 *
 *   bytes  0-1   id            copied back unchanged, so the phone can match
 *                              the answer to its question
 *   bytes  2-3   flags         replaced with "this is a reply"
 *   bytes  4-5   qdcount       number of questions, 1 in practice
 *   bytes  6-7   ancount       answers; 0 in a question, 1 in the reply
 *   bytes  8-11  nscount, arcount
 *   byte  12..   the question: a name, then 2 bytes of type, 2 of class
 *
 * The name is written as labels, each with its length in front, rather than
 * with dots: "captive.apple.com" arrives as 7 'c','a','p','t','i','v','e'
 * 5 'a','p','p','l','e' 3 'c','o','m' 0. The name itself is never read, since
 * every name gets the same answer, but it has to be walked to find where
 * the question ends, because the answer goes after it.
 *
 * The reply is the question sent back, plus one answer that points at the
 * name already in the message instead of repeating it: 0xC00C, where the
 * top two bits mark a pointer and 0x00C is offset 12, the start of the
 * question.
 *
 * Context: captive_dns_start() runs in the setup loop's task; the responder
 * runs in a task of its own until the restart that ends setup mode, which
 * is the only way it stops.
 */
#include "captive_dns.h"

#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"           /* the socket interface of lwIP, the TCP/IP stack */

static const char *TAG = "captive_dns";

#define DNS_PORT            53
#define DNS_HEADER_LEN      12
/* DNS over UDP is limited to 512 bytes, so no question is larger */
#define DNS_BUF_LEN         512

/* Recorded in ARCHITECTURE.md's task table. In internal RAM, as every task
 * stack is: the largest local is the 512-byte message buffer. */
#define TASK_STACK          3072
#define TASK_PRIO           4

static int s_sock = -1;
static uint32_t s_answer_ip;
static uint32_t s_answered;         /* questions answered, for the log */

/* Returns the offset just past the name that starts at `pos`, or -1 if it
 * runs off the end of the message. The bounds check is the whole point: the
 * message comes straight off the network, and a length byte claiming 200
 * characters in a 40-byte message must not be followed past the buffer. */
static int skip_name(const uint8_t *buf, int len, int pos)
{
    while (pos < len) {
        uint8_t label = buf[pos];
        if (label == 0) {
            return pos + 1;                 /* the empty label ends the name */
        }
        if ((label & 0xC0) == 0xC0) {
            /* A pointer, two bytes, which also ends the name */
            return (pos + 2 <= len) ? pos + 2 : -1;
        }
        if (label > 63) {
            return -1;                      /* neither a length nor a pointer */
        }
        pos += 1 + label;
    }
    return -1;
}

static void dns_task(void *arg)
{
    uint8_t buf[DNS_BUF_LEN];
    struct sockaddr_in from;

    for (;;) {
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < DNS_HEADER_LEN) {
            continue;                       /* an error, or too short to be a question */
        }

        /* Only questions, and only one at a time. Not answering answers
         * keeps two of these from talking to each other for ever. */
        bool is_response = (buf[2] & 0x80) != 0;
        int qdcount = (buf[4] << 8) | buf[5];
        if (is_response || qdcount != 1) {
            continue;
        }
        int qend = skip_name(buf, n, DNS_HEADER_LEN);
        if (qend < 0 || qend + 4 > n) {
            continue;                       /* a cut-off or malformed name */
        }
        int qtype = (buf[qend] << 8) | buf[qend + 1];
        qend += 4;                          /* past the type and the class */

        /* Only a request for an IPv4 address (type A, 1) gets one. Anything
         * else, IPv6 (AAAA) above all, which phones ask for constantly, gets
         * a reply with no answer: an empty, correct reply makes the phone
         * move on at once, where no reply leaves it waiting for a timeout. */
        bool answer_it = qtype == 1 && qend + 16 <= DNS_BUF_LEN;

        buf[2] = 0x84;                      /* a reply, authoritative */
        buf[3] = 0x00;                      /* no error */
        buf[6] = 0x00;
        buf[7] = answer_it ? 0x01 : 0x00;   /* number of answers */
        buf[8] = buf[9] = buf[10] = buf[11] = 0x00;
        int out_len = qend;
        if (answer_it) {
            uint8_t *a = buf + qend;
            a[0] = 0xC0; a[1] = 0x0C;       /* the name: a pointer to offset 12 */
            a[2] = 0x00; a[3] = 0x01;       /* type A */
            a[4] = 0x00; a[5] = 0x01;       /* class IN, the internet */
            /* Time to keep the answer: none. It is true only while this
             * hotspot exists, and a phone that kept it would go on sending
             * some real site's traffic here afterwards. */
            a[6] = a[7] = a[8] = a[9] = 0x00;
            a[10] = 0x00; a[11] = 0x04;     /* four bytes of address follow */
            memcpy(a + 12, &s_answer_ip, 4);
            out_len = qend + 16;
        }
        sendto(s_sock, buf, out_len, 0, (struct sockaddr *)&from, from_len);

        /* The first, then every fiftieth: evidence that the phone asks this
         * device and not a resolver it remembered from another network,
         * without burying the log, since a phone asks dozens a minute */
        if (s_answered % 50 == 0) {
            ESP_LOGI(TAG, "answered %lu questions (the latest of type %d)",
                     (unsigned long)(s_answered + 1), qtype);
        }
        s_answered++;
    }
}

esp_err_t captive_dns_start(uint32_t ip)
{
    s_answer_ip = ip;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "no socket (errno %d)", errno);
        return ESP_FAIL;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "port %d not available (errno %d)", DNS_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    if (xTaskCreate(dns_task, "captive_dns", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
