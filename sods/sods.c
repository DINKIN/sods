/*
 * Socket over DNS server.
 *
 * Copyright (c) 2009-2015 Michael Santos <michael.santos@gmail.com>
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
#include "sods.h"

int sds_parse_forward(SDS_STATE *ss, char *buf);
void sds_print_forward(SDS_STATE *ss);

    int
main (int argc, char *argv[])
{
    SDS_STATE *ss = NULL;

    int ch = 0;
    int di = 0;

    (void)signal(SIGPIPE, SIG_IGN);

    IS_NULL(ss = calloc(1, sizeof(SDS_STATE)));

    (void)memset(&ss->local, 0, sizeof(struct sockaddr_in));
    ss->local.sin_family = AF_INET;
    ss->local.sin_port = htons(53);
    ss->local.sin_addr.s_addr = INADDR_ANY;

    ss->maxconn = 16;
    ss->maxtimeout = 120;

    ss->run = &sds_sock_loop;
    ss->handler = &sds_handler;
    ss->decapsulate = &sds_decapsulate;
    ss->cleanup = &sds_cleanup;

    while ( (ch = getopt(argc, argv, "c:Dd:g:hi:L:p:u:vx:")) != -1) {
        switch (ch) {
            case 'c':
                ss->maxconn = (u_int32_t)atoi(optarg);
                break;
            case 'D':   /* daemonize */
                ss->daemon = 1;
                break;
            case 'd':   /* chroot directory */
                IS_NULL(ss->proc.chroot = strdup(optarg));
                break;
            case 'g':
                IS_NULL(ss->proc.group = strdup(optarg));
                break;
            case 'i':
                if (inet_aton(optarg, &ss->local.sin_addr) != 1) {
                    warnx("Invalid IP address: %s", optarg);
                    usage(ss);
                }
                break;
            case 'L': /* <session id>:<remote address>:<remote port> */
                if (sds_parse_forward(ss, optarg) < 0) {
                    warnx("Invalid forward argument: %s", optarg);
                    usage(ss);
                }
                break;
            case 'p':
                ss->local.sin_port = htons((in_port_t)atoi(optarg));
                break;
            case 'u':
                IS_NULL(ss->proc.user = strdup(optarg));
                break;
            case 'v':
                ss->verbose++;
                break;
            case 'x':
                ss->maxtimeout = (u_int32_t)atoi(optarg);
                break;
            case 'h':
            default:
                usage(ss);
        }
    }

    argc -= optind;
    argv += optind;

    if ( (argc == 0) || (argc >= MAXDNAMELIST))
        usage(ss);

    ss->dn_max = argc;
    IS_NULL(ss->dn = calloc(argc, sizeof(char *)));
    for ( di = 0; di < argc; di++) {
        if (strlen(argv[di]) > NS_MAXCDNAME - 1)
            usage(ss);
            IS_NULL(ss->dn[di] = strdup(argv[di]));
    }

    if (ss->fwd == NULL)
        (void)sds_parse_forward(ss, "127.0.0.1:22");

    sds_print_forward(ss);
    ss->run(ss);

    exit (EXIT_SUCCESS);
}

    int
sds_parse_forward(SDS_STATE *ss, char *buf)
{
    struct hostent *he = NULL;
    char *dst = NULL;
    char *port = NULL;

    SDS_FWD *fw = NULL;

    if (ss->fwds > MAXFWDS)
        return 0;

    IS_NULL(dst = strdup(buf));

    if ( (port = strchr(dst, ':')) == NULL) {
        free(dst);
        return -1;
    }
    *port++ = '\0';

    if (strlen(dst) > 128) {
        free(dst);
        return -1;
    }

    if ( (he = gethostbyname(dst)) == NULL) {
        warnx("gethostbyname: %s", hstrerror(h_errno));
        free(dst);
        return -1;
    }

    if (ss->fwd == NULL)
        IS_NULL(ss->fwd = calloc(1, sizeof(SDS_FWD)));
    else
        IS_NULL(ss->fwd = realloc(ss->fwd, sizeof(SDS_FWD) * (ss->fwds + 1)));

    fw = ss->fwd + ss->fwds;

    (void)memset(&fw->sa, 0, sizeof(struct sockaddr_in));
    (void)memcpy(&fw->sa.sin_addr, he->h_addr_list[0], sizeof(fw->sa.sin_addr));
    fw->sa.sin_port = htons((in_port_t)atoi(port));

    ss->fwds++;
    free(dst);
    return 0;
}

    void
sds_print_forward(SDS_STATE *ss)
{
    int i = 0;
    SDS_FWD *fw = NULL;

    (void)fprintf(stdout, "Forwarded sessions = %u\n", (u_int32_t)ss->fwds);

    for (i = 0; i < ss->fwds; i++) {
        fw = ss->fwd + i;
        (void)fprintf(stdout, "Forward #%u: %s:%u\n",
                (u_int32_t)i, inet_ntoa(fw->sa.sin_addr),
                ntohs(fw->sa.sin_port));
    }
}

    void
sds_timestamp(void)
{
    char outstr[200] = {0};
    time_t t;
    struct tm *tmp = NULL;

    t = time(NULL);
    tmp = localtime(&t);
    if (tmp == NULL) {
        perror("localtime");
        exit(EXIT_FAILURE);
    }

    if (strftime(outstr, sizeof(outstr), "%Y-%m-%d %H:%M:%S ", tmp) == 0) {
        (void)fprintf(stderr, "strftime returned 0");
        exit(EXIT_FAILURE);
    }

    (void)fprintf(stderr, "%s", outstr);
}

    void
usage(SDS_STATE *ss)
{
    (void)fprintf(stderr, "%s, %s\n", SDS_PROGNAME, SDS_VERSION);
    (void)fprintf(stderr,
            "usage: sods <option> <domain name>\n"
            "             -c     max connections\n"
            "             -D     run as a daemon\n"
            "             -d     chroot directory\n"
            "             -g     unprivileged group\n"
            "             -i     local IP address\n"
            "             -L     forward socket: <host>:<port>\n"
            "             -p     port\n"
            "             -u     unprivileged user\n"
            "             -v     verbose\n"
            "             -x     connection timeout\n"
            );
    exit (EXIT_FAILURE);
}
