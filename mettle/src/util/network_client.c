/**
 * @brief Durable multi-transport client connection abtraction
 */

#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdlib.h>
#include <tls.h>

#include <util.h>
#include <util/log.h>
#include <util/network_client.h>

enum network_client_proto {
	network_client_proto_udp,
	network_client_proto_tcp,
	network_client_proto_tls,
};

struct network_client_server {
	char *uri;
	enum network_client_proto proto;
	char *host;
	char **services;
	int num_services;
};

struct network_client {
	uv_loop_t *loop;
	uv_timer_t connect_timer;
	struct network_client_server *servers;
	int num_servers;

	int curr_server, curr_service;
	uint64_t connect_time_s;

	union uv_any_handle conn;
	struct addrinfo *addrinfo;

	struct tls *tls;
	int tls_state;

	enum {
		network_client_connected,
		network_client_resolving,
		network_client_connecting,
		network_client_closed,
	} state;

	network_client_cb_t read_cb;
	void *read_cb_arg;
	network_client_cb_t connect_cb;
	void *connect_cb_arg;
	network_client_cb_t close_cb;
	void *close_cb_arg;
};

struct {
	enum network_client_proto proto;
	const char *str;
} proto_list[] = {
	{network_client_proto_udp, "udp"},
	{network_client_proto_tcp, "tcp"},
	{network_client_proto_tls, "tls"},
};

static const char *proto_to_str(enum network_client_proto proto)
{
	for (int i = 0; i < COUNT_OF(proto_list); i++) {
		if (proto_list[i].proto == proto) {
			return proto_list[i].str;
		}
	}
	return "unknown";
}

static enum network_client_proto str_to_proto(const char *proto)
{
	for (int i = 0; i < COUNT_OF(proto_list); i++) {
		if (!strcasecmp(proto_list[i].str, proto)) {
			return proto_list[i].proto;
		}
	}
	return network_client_proto_tcp;
}

void server_free(struct network_client_server *srv)
{
	free(srv->host);
	free(srv->uri);
	for (int i = 0; i < srv->num_services; i++) {
		free(srv->services[i]);
	}
	free(srv->services);
	memset(srv, 0, sizeof(*srv));
}

static int add_server_service(struct network_client_server *srv, const char *service)
{
	char *service_cpy = strdup(service);
	if (service_cpy == NULL) {
		return -1;
	}
	srv->services = reallocarray(srv->services,
			srv->num_services + 1, sizeof(char *));
	if (srv->services == NULL) {
		return -1;
	}
	srv->services[srv->num_services++] = service_cpy;
	return 0;
}

static int parse_server(struct network_client_server *srv, const char *uri)
{
	int rc = -1;
	char *services = NULL;
	char *proto = NULL;
	char *uri_tmp = strdup(uri);
	char *host = strstr(uri_tmp, "://");
	srv->uri = strdup(uri);

	if (uri_tmp == NULL || srv->uri == NULL) {
		goto out;
	}

	if (host == NULL) {
		proto = "tcp";
		host = uri_tmp;
	} else {
		host[0] = '\0';
		proto = uri_tmp;
		host += 3;
	}

	services = strstr(host, ":");
	if (services) {
		services[0] = '\0';
		services++;
	}

	if (proto == NULL || host == NULL) {
		log_error("failed to parse URI: %s", uri);
		goto out;
	}

	srv->host = strdup(host);
	srv->proto = str_to_proto(proto);

	if (services) {
		char *services_tmp = strdup(services);
		if (!services_tmp) {
			goto out;
		}

		char *service_tmp = services_tmp;
		const char *service;
		while ((service = strsep(&service_tmp, ",")) != NULL) {
			if (add_server_service(srv, service) != 0) {
				free(services_tmp);
				goto out;
			}
		}
	} else {
		log_error("%s service unspecified", proto);
		goto out;
	}

	rc = 0;
out:
	if (rc != 0) {
		server_free(srv);
	}
	free(uri_tmp);

	return rc;
}

int network_client_remove_servers(struct network_client *nc)
{
	if (nc->servers) {
		for (int i = 0; i < nc->num_servers; i++) {
			server_free(&nc->servers[i]);
		}
		free(nc->servers);
		nc->servers = NULL;
		nc->num_servers = 0;
	}
	return 0;
}

int network_client_add_server(struct network_client *nc, const char *uri)
{
	nc->servers = reallocarray(nc->servers, nc->num_servers + 1,
			sizeof(struct network_client_server));
	if (nc->servers == NULL) {
		return -1;
	}

	if (parse_server(&nc->servers[nc->num_servers], uri) != 0) {
		return -1;
	}

	nc->num_servers++;
	return 0;
}

static struct network_client_server *get_curr_server(struct network_client *nc)
{
	if (nc->servers) {
		return &nc->servers[nc->curr_server];
	} else {
		return NULL;
	}
}

static const char * get_curr_service(struct network_client *nc)
{
	if (nc->servers) {
		return nc->servers[nc->curr_server].services[nc->curr_service];
	} else {
		return 0;
	}
}

static struct network_client_server *choose_next_server(struct network_client *nc)
{
	struct network_client_server *srv = get_curr_server(nc);
	if (srv && nc->curr_service < srv->num_services - 1) {
		nc->curr_service++;
	} else {
		nc->curr_service = 0;
		if (nc->num_servers > 1) {
			nc->curr_server++;
			if (nc->curr_server >= nc->num_servers) {
				nc->curr_server = 0;
			}
		}
	}
	return get_curr_server(nc);
}

/*
 * Callback management
 */

void network_client_set_read_cb(struct network_client *nc,
		network_client_cb_t cb, void *arg)
{
	nc->read_cb = cb;
	nc->read_cb_arg = arg;
}

void network_client_set_connect_cb(struct network_client *nc,
		network_client_cb_t cb, void *arg)
{
	nc->connect_cb = cb;
	nc->connect_cb_arg = arg;
}

void network_client_set_close_cb(struct network_client *nc,
		network_client_cb_t cb, void *arg)
{
	nc->close_cb = cb;
	nc->close_cb_arg = arg;
}

/*
 * Client-side IO
 */
int network_client_read(struct network_client *nc, void *buf, size_t buflen)
{
	return -1;
}

int network_client_write(struct network_client *nc, void *buf, size_t buflen)
{
	log_info("writing %zu bytes", buflen);
	return -1;
}

static void set_closed(struct network_client *nc)
{
	nc->state = network_client_closed;
	if (nc->addrinfo) {
		uv_freeaddrinfo(nc->addrinfo);
		nc->addrinfo = NULL;
	}

	if (nc->tls) {
		tls_free(nc->tls);
		nc->tls_state = 0;
	}

	if (nc->close_cb) {
		nc->close_cb(nc, nc->close_cb_arg);
	}
}

static void on_close(uv_handle_t *handle)
{
	struct network_client *nc = handle->data;
	set_closed(nc);
}

int network_client_close(struct network_client *nc)
{
	if (nc->state != network_client_connected) {
		return -1;
	}
	uv_close((uv_handle_t *)&nc->conn.tcp, on_close);
	return 0;
}

void on_poll(uv_poll_t *req, int status, int events)
{
	struct network_client *nc = req->data;
	if (nc->state == network_client_connecting) {
		if (nc->tls_state == TLS_READ_AGAIN || nc->tls_state == TLS_WRITE_AGAIN) {
			nc->tls_state = tls_connect_socket(nc->tls, 0, NULL);
			if (nc->tls_state == 0) {
				nc->state = network_client_connected;
			} else if (nc->tls_state == -1) {
				log_info("%s", nc->tls_state, tls_error(nc->tls));
				//network_client_close(nc);
			}
		}
	}
}

static void connect_tcp_cb(uv_connect_t *req, int status)
{
	struct network_client *nc = req->data;
	struct network_client_server *srv = get_curr_server(nc);

	if (status != 0) {
		log_info("failed to connect to '%s://%s:%s': %s",
				proto_to_str(srv->proto), srv->host, get_curr_service(nc),
				uv_strerror(status));
		set_closed(nc);
		return;
	}

	if (srv->proto == network_client_proto_tcp) {
		nc->state = network_client_connected;
		if (nc->connect_cb) {
			nc->connect_cb(nc, nc->connect_cb_arg);
		}
	} else {
		nc->tls = tls_client();
		if (nc->tls == NULL) {
			log_error("could not allocate TLS client");
			set_closed(nc);
			return;
		}

		int socket;
		if (uv_fileno((uv_handle_t *)req->handle, &socket) == 0) {
			nc->tls_state = tls_connect_socket(nc->tls, socket, srv->host);
		} else {
			log_error("could not find file descriptor");
			return;
		}

		uv_poll_t poll_req = { .data = nc };
		uv_poll_init(nc->loop, &poll_req, socket);
		uv_poll_start(&poll_req, UV_READABLE, on_poll);
	}
}

static int connect_tcp(struct network_client *nc, struct addrinfo *addrinfo)
{
	uv_connect_t req  = { .data = nc };
	uv_tcp_init(nc->loop, &nc->conn.tcp);
	nc->conn.tcp.data = nc;
	return uv_tcp_connect(&req, &nc->conn.tcp,
			(const struct sockaddr *)addrinfo->ai_addr, connect_tcp_cb);
}

static int connect_udp(struct network_client *nc, struct addrinfo *addrinfo)
{
	uv_udp_init(nc->loop, &nc->conn.udp);

	if (nc->connect_cb) {
		nc->connect_cb(nc, nc->connect_cb_arg);
	}
	return 0;
}

void resolving_cb(uv_getaddrinfo_t *req, int status, struct addrinfo *addrinfo)
{
	struct network_client *nc = req->data;
	struct network_client_server *srv = get_curr_server(nc);

	if (status < 0) {
		log_info("could not resolve '%s://%s:%s': %s",
				proto_to_str(srv->proto), srv->host, get_curr_service(nc),
				uv_strerror(status));
		set_closed(nc);
		return;
	}

	nc->addrinfo = addrinfo;

	switch (srv->proto) {

		case network_client_proto_udp:
			if (connect_udp(nc, addrinfo) == 0) {
				nc->state = network_client_connected;
			} else {
				set_closed(nc);
			}
			break;

		case network_client_proto_tls:
		case network_client_proto_tcp:
			if (connect_tcp(nc, addrinfo) == 0) {
				nc->state = network_client_connecting;
			} else {
				set_closed(nc);
			}
			break;
	}
}

static void reconnect_cb(uv_timer_t *timer)
{
	struct network_client *nc = timer->data;

	if (nc->state != network_client_closed || nc->num_servers == 0) {
		return;
	}

	struct network_client_server *srv = choose_next_server(nc);
	const char *service = get_curr_service(nc);

	log_info("connecting to %s://%s:%s",
			proto_to_str(srv->proto), srv->host, service);

	uv_getaddrinfo_t req = { .data = nc };

	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_flags = AI_CANONNAME,
	};

	if (srv->proto == network_client_proto_udp) {
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
	} else {
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
	}

	nc->state = network_client_resolving;
	uv_getaddrinfo(nc->loop, &req, resolving_cb, srv->host, service, &hints);
}

int network_client_start(struct network_client *nc)
{
	if (uv_timer_init(nc->loop, &nc->connect_timer) != 0) {
		return -1;
	}

	return uv_timer_start(&nc->connect_timer, reconnect_cb, 0, 1000);
}

int network_client_stop(struct network_client *nc)
{
	uv_timer_stop(&nc->connect_timer);
	return 0;
}

void network_client_free(struct network_client *nc)
{
	if (nc) {
		network_client_stop(nc);
		network_client_remove_servers(nc);
		free(nc);
	}
}

struct network_client * network_client(uv_loop_t *loop)
{
	struct network_client *nc = calloc(1, sizeof(*nc));
	if (!nc) {
		return NULL;
	}

	/*
	 * Start libtls
	 */
	tls_init();

	if (loop == NULL) {
		loop = uv_default_loop();
		if (!loop) {
			goto err;
		}
	}

	nc->loop = loop;

	uv_timer_init(nc->loop, &nc->connect_timer);
	nc->connect_timer.data = nc;

	nc->state = network_client_closed;

	return nc;

err:
	network_client_free(nc);
	return NULL;
}