#ifndef NET_H
#define NET_H

#include <netinet/in.h>
#include <stddef.h>
#include <sys/socket.h>

/* Formats the IP and port of a socket address ("" and 0 for other families). */
void format_leg_addr(const struct sockaddr_storage *ss, char *ip_out, size_t ip_out_len,
                     int *port_out);

/* IPv4 only. Return 0, or -1 after logging why. */
int resolve_output_addr(const char *host, int port, struct sockaddr_storage *addr,
                        socklen_t *addrlen);
/* "", "0.0.0.0" and "*" mean every interface. */
int resolve_input_bind_addr(const char *host, int port, struct sockaddr_in *sa);

#endif
