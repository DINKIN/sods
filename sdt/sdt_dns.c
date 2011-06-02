/*
 * Socket over DNS client.
 *
 * Copyright (c) 2009-2011 Michael Santos <michael.santos@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <netdb.h>

#include <ctype.h>

#include "base32.h"
#include "base64.h"

#include "sdt.h"
#include "sdt_servers.h"

int nx = 0;

    int
sdt_dns_init(void)
{
    /*
    if ( (__res != NULL) && (__res.res_init != 0))
        __res.res_init = 0;
        */

    IS_ERR(res_init());

    _res.options &= ~(RES_DNSRCH|RES_DEFNAMES);
    _res.retrans = 3;
    _res.retry = 1;

    return (0);
}

    void
sdt_dns_setopt(int opt, int val)
{
    switch (opt) {
        case SDT_RES_RETRANS:
            _res.retrans = val;
            break;
        case SDT_RES_RETRY:
            _res.retry = val;
            break;
        case SDT_RES_USEVC:
            _res.options |= RES_USEVC;
            if (val > 0)
                _res.options |= RES_STAYOPEN;
            break;
        case SDT_RES_ROTATE:
            _res.options |= RES_ROTATE;
            break;
        case SDT_RES_BLAST:
            _res.options |= RES_BLAST;
            break;
        case SDT_RES_DEBUG:
            _res.options |= RES_DEBUG;
            break;
    }
}

    int
sdt_dns_setns(char *ns)
{
    struct hostent *he = NULL;

    if ( (ns == NULL) || (nx > MAXNS - 1))
        return (-1);

    if ( (he = gethostbyname(ns)) == NULL) {
        warnx("gethostbyname: %s", hstrerror(h_errno));
        return (-1);
    }

    (void)memcpy(&_res.nsaddr_list[nx++].sin_addr, he->h_addr_list[0], he->h_length);
    _res.nscount = nx;

    return (0);
}

    int
sdt_dns_A(SDT_STATE *ss, char *buf, ssize_t n)
{
    char *p = NULL;
    char dn[NS_MAXDNAME];
    char query[NS_MAXDNAME];
    char pkt[NS_PACKETSZ];

    int i = 0;
    int j = 0;

    u_int16_t nonce = 0;

    if (n < 1)
        return (-1);

    (void)memset(dn, 0, sizeof(dn));
    (void)memset(query, 0, sizeof(query));
    (void)memset(pkt, 0, sizeof(pkt));

    nonce = (u_int16_t)arc4random();

    /* Base32 encode the buffer and lowercase the result */
    if (base32_encode_length(n) >= sizeof(dn))
        errx(EXIT_FAILURE, "buffer overflow, biatch!");

    base32_encode_into(buf, n, dn);

    for (p = dn; *p != '\0'; p++) {
        *p = tolower(*p);
        if (++i%NS_MAXLABEL == 0)
            query[j++] = '.';
        query[j++] = *p;
    }
    (void)memcpy(dn, query, sizeof(dn));

    switch (ss->protocol) {
        case PROTO_DYN_FWD:
            (void)snprintf(query, sizeof(query), "%s.%u-%u.id-%u.u.%s-%u.x.%s",
                    dn, nonce, (u_int32_t)ss->sum_up, htonl(ss->sess.id), ss->target, ss->target_port, ss->dname_next(ss));
            break;
        default:
            /* Create the domain name:
             *  $temp_payload.$nonce-$sum_up.id-$id.up.$extension
             */
            (void)snprintf(query, sizeof(query), "%s.%u-%u.id-%u.up.%s",
                    dn, nonce, (u_int32_t)ss->sum_up, htonl(ss->sess.id), ss->dname_next(ss));
            break;
    }

    VERBOSE(2, "A:%s\n", query);
    if (res_search(query, ns_c_in, ns_t_a, (u_char *)&pkt, sizeof(pkt)) < 0) {
        VERBOSE(1, "sdt_dns_A: res_search: %s\n", hstrerror(h_errno));
        return (-1);
    }

    return (0);
}

    char *
sdt_dns_poll(SDT_STATE *ss, size_t *len)
{
    char query[NS_MAXDNAME];
    char pkt[NS_PACKETSZ];

    char *buf = NULL;
    int buflen = 0;

    u_int16_t nonce = 0;

    (void)memset(pkt, 0, sizeof(pkt));

    nonce = (u_int16_t)arc4random();

    switch (ss->protocol) {
        case PROTO_DYN_FWD:
            (void)snprintf(query, sizeof(query), "%u-%u.id-%u.d.%s-%u.x.%s",
                    (u_int32_t)ss->sum, nonce, htonl(ss->sess.id), ss->target, ss->target_port, ss->dname_next(ss));
            break;
        default:
            /* Create the TXT query:
             *  $sum-$nonce.id-$id.down.$extension
             */
            (void)snprintf(query, sizeof(query), "%u-%u.id-%u.down.%s",
                    (u_int32_t)ss->sum, nonce, htonl(ss->sess.id), ss->dname_next(ss));
            break;
    }

    VERBOSE(2, "POLL:%s\n", query);

    *len = 0;
    if ( (buflen = res_search(query, ns_c_in, ss->type, (u_char *)&pkt, sizeof(pkt))) < 0) {
        VERBOSE(1, "sdt_dns_poll: res_search: %s\n", hstrerror(h_errno));
        ss->pollfail++;
        return (NULL);
    }

    buf = sdt_dns_parse(ss, pkt, &buflen);
    *len = buflen;

    return (buf);
}

/* Retrieve the answer and base64
 * decode it
 */
    char *
sdt_dns_parse(SDT_STATE *ss, char *pkt, int *pktlen)
{
    ns_msg nsh;
    ns_rr rr;
    char *buf = NULL;
    u_int16_t rrlen = 0;
    int type = 0;

    if (ns_initparse((u_char *)pkt, *pktlen, &nsh) < 0) {
        VERBOSE(1, "Invalid response in record\n");
        return (NULL);
    }

#if 0
    for (i = 0; i < ns_msg_count(nsh, ns_s_an); i++) {
        if (ns_parserr(&nsh, ns_s_an, i, &rr)) {
#endif
        if (ns_parserr(&nsh, ns_s_an, 0, &rr)) {
            VERBOSE(1, "ns_parserr\n");
            *pktlen = 0;
            return (NULL);
        }

        if (ns_rr_type(rr) != ss->type) {
            VERBOSE(1, "ns_rr_type != ns_t_txt\n");
            /* continue; */
            *pktlen = 0;
            return (NULL);
        }

        type = ns_rr_type(rr);
        rrlen = ns_rr_rdlen(rr);

        if (rrlen > *pktlen)
            return (NULL);

        switch (type) {
            case ns_t_txt:
                buf = sdt_dns_dec_TXT(ss, (u_char *)ns_rr_rdata(rr), &rrlen);
                break;
            case ns_t_cname:
                buf = sdt_dns_dec_CNAME(ss, (u_char *)ns_rr_rdata(rr), &rrlen);
                break;
            case ns_t_null:
                buf = sdt_dns_dec_NULL(ss, (u_char *)ns_rr_rdata(rr), &rrlen);
                break;
        }
#if 0
    }
#endif

    *pktlen = rrlen;
    return (buf);
}


    char *
sdt_dns_dec_TXT(SDT_STATE *ss, u_char *data, u_int16_t *n)
{
    u_char *p = NULL;
    u_char *dp = NULL;
    char *buf = NULL;
    size_t len = 0;

    char *out = NULL;
    size_t outlen = 0;

    IS_NULL(buf = calloc(NS_PACKETSZ, 1));
    dp = p = data;

    for ( ; p - dp < *n ; p += *p+1) {
        char *lf = NULL;

        struct {
            u_int8_t len;
            char data[256];
        } rec;


        if (*p == 0)
            continue;

        rec.len = *p;
        (void)memset(rec.data, 0, sizeof(rec.data));
        (void)memcpy(rec.data, p+1, *p);

        /* Remove base64 linefeeds used for formatting */
        while ( (lf = strchr(rec.data, 0x0A)) != NULL)
            (void)memmove(lf, lf+1, strlen(lf));

        if (base64_decode_alloc(rec.data, strlen(rec.data), &out, &outlen) == 0) {
            VERBOSE(0, "Invalid base64 encoded packet\n");
            free(buf);
            return (NULL);
        }


        if (out == NULL)
            err(EXIT_FAILURE, "Could not allocate memory");

        if (len + outlen >= NS_PACKETSZ)
            errx(EXIT_FAILURE, "buffer overflow v2, biatch!");

        (void)memcpy(buf + len, out, outlen);

        len += outlen;
        free (out);
    }
    *n = len;
    return (buf);
}

    char *
sdt_dns_dec_CNAME(SDT_STATE *ss, u_char *data, u_int16_t *n)
{
    char *p = NULL;
    char *buf = NULL;
    char b32[NS_PACKETSZ];

    IS_NULL(buf = calloc(NS_PACKETSZ, 1));

    if (dn_expand( (const u_char *)data, (const u_char *)data + *n,
                (const u_char *)data, b32, NS_PACKETSZ) < 0) {
        *n = 0;
        return (NULL);
    }

    while ( (p = strchr(b32, '.')) != NULL)
        (void)memmove(p, p+1, strlen(p));

    *n = base32_decode_into(b32, NS_PACKETSZ, buf);
    return (buf);
}

    char *
sdt_dns_dec_NULL(SDT_STATE *ss, u_char *data, u_int16_t *n)
{
    char *out = NULL;
    size_t outlen = 0;
    char *lf = NULL;

    if (*data == 0)
        return (NULL);

    /* Remove base64 linefeeds used for formatting */
    while ( (lf = strchr((char *)data, 0x0A)) != NULL)
        (void)memmove(lf, lf+1, strlen(lf));

    if (base64_decode_alloc((char *)data, *n, &out, &outlen) == 0) {
        VERBOSE(0, "Invalid base64 encoded packet\n");
        return (NULL);
    }

    if (out == NULL)
        err(EXIT_FAILURE, "Could not allocate memory");

    if (outlen >= NS_PACKETSZ)
        errx(EXIT_FAILURE, "buffer overflow v2, biatch!");

    *n = outlen;
    return (out);
}


    int
sdt_dns_parsens(SDT_STATE *ss, char *buf)
{
    char *serv = NULL;
    char *p = NULL;
    SDS_SERV *ds = NULL;
    int i = 0;
    int n = 0;
    int ns = 0;

    IS_NULL(p = serv = strdup(buf));

    if (strcasecmp(serv, "random") == 0) {
        for (i = 0; i < MAXNS; i++) {
            ds = dnsserv + (arc4random()%((sizeof(dnsserv)/sizeof(SDS_SERV))-1));
            (void)sdt_dns_setns(ds->addr);
            VERBOSE(1, "Using %s (%s) = %s\n", ds->name, ds->descr,
                    ds->addr);
        }

        return (0);
    }

    if ( (p = strchr(serv, ':')) != NULL) {
        *p++ = '\0';
        ns = atoi(p);
    }

    for (ds = dnsserv; ds->name && i < MAXNS; ds++) {
        if (strcasecmp(serv, ds->name) == 0) {
            if (p && (n++ != ns))
                continue;
            VERBOSE(1, "Using %s (%s) = %s\n", ds->name, ds->descr,
                    ds->addr);
            (void)sdt_dns_setns(ds->addr);
            i++;
        }
    }

    if (i == 0) {
        if (sdt_dns_setns(optarg) < 0)
            return (-1);
    }

    free (serv);
    return (0);
}

    void
sdt_dns_print_servers(SDT_STATE *ss)
{
    SDS_SERV *ds = NULL;
    int ns = 0;
    char *last = NULL;

    for (ds = dnsserv; ds->name; ds++, ns++) {
        if (last && (strcmp(last, ds->name) != 0))
            ns = 0;
        (void)fprintf(stderr, "%s:%d (%s) = %s\n", ds->name, ns, ds->descr,
                ds->addr);
        last = ds->name;
    }
}


/* Strategies for iterating through multiple
 * domain names */
    char *
sdt_dns_dn_roundrobin(void *state)
{
    SDT_STATE *ss = state;
    return (ss->dname[ss->dname_iterator++ % ss->dname_max]);
}

    char *
sdt_dns_dn_random(void *state)
{
    SDT_STATE *ss = state;
    return (ss->dname[arc4random() % ss->dname_max]);
}


