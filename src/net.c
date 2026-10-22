#include "net.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

void format_leg_addr(const struct sockaddr_storage *ss, char *ip_out, size_t ip_out_len,
                     int *port_out) {
    ip_out[0] = '\0';
    *port_out = 0;
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;
        inet_ntop(AF_INET, &sin->sin_addr, ip_out, ip_out_len);
        *port_out = ntohs(sin->sin_port);
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ss;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ip_out, ip_out_len);
        *port_out = ntohs(sin6->sin6_port);
    }
}

static int resolve_ipv4_addr(const char *host, int port, int ai_flags, const char *context,
                             struct sockaddr_storage *addr, socklen_t *addrlen) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = ai_flags;

    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    int rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0 || !res) {
        relay_logf("getaddrinfo failed for %s %s:%d: %s\n", context, host, port,
                   rc == 0 ? "no address" : gai_strerror(rc));
        return -1;
    }

    *addrlen = (socklen_t)res->ai_addrlen;
    memcpy(addr, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return 0;
}

int resolve_output_addr(const char *host, int port, struct sockaddr_storage *addr,
                        socklen_t *addrlen) {
    return resolve_ipv4_addr(host, port, 0, "output", addr, addrlen);
}

int resolve_input_bind_addr(const char *host, int port, struct sockaddr_in *sa) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((unsigned short)port);

    if (!host || !host[0] || strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
        sa->sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }

    struct sockaddr_storage resolved;
    socklen_t resolved_len = 0;
    if (resolve_ipv4_addr(host, port, AI_PASSIVE, "input", &resolved, &resolved_len) < 0) {
        return -1;
    }

    (void)resolved_len;
    memcpy(sa, &resolved, sizeof *sa);
    return 0;
}
