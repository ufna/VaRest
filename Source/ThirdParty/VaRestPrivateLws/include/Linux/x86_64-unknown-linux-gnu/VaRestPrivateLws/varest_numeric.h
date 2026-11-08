/* Private same-source numeric TCP route. No Engine LWS ABI or custody authority. */
#ifndef VAREST_PRIVATE3_NUMERIC_H
#define VAREST_PRIVATE3_NUMERIC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lws_client_connect_info;
struct lws;
struct varest_private3_tcp_route {
 uint32_t kind; /* 1 direct,2 HTTP CONNECT,3 SOCKS; selected from actual vhost. */
 uint32_t allowed_families; /* bit1 IPv4,bit2 IPv6 from actual vhost policy. */
 uint32_t family; /* 4 or6; zero means endpoint identity only, never a connect. */
 uint32_t scope;
 uint16_t port; /* host order, physical port (proxy when configured). */
 uint8_t address[16];
 char physical_host[256]; /* exact selected resolver input, excludes proxy auth. */
};
/* Pure selection/copy only. No DNS, TCP, credential, callback or object creation. */
__attribute__((visibility("hidden"))) int
varest_private3_get_tcp_route(const struct lws_client_connect_info *, struct varest_private3_tcp_route *);
__attribute__((visibility("hidden"))) int
varest_private3_tcp_is_connected(const struct lws *);
#ifdef __cplusplus
}
#endif
#endif
