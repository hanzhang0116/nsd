/*
 * xfrd-tcp.c - XFR (transfer) Daemon TCP system source file. Manages tcp conn.
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include "config.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/uio.h>
#include "nsd.h"
#include "xfrd-tcp.h"
#include "buffer.h"
#include "packet.h"
#include "dname.h"
#include "options.h"
#include "namedb.h"
#include "xfrd.h"
#include "xfrd-disk.h"
#include "util.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

void configure_context(SSL_CTX *ctx)
{
    // Only trust 1.3
    DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** Setting minimum TLS version 1.3 for SSL CTX"));
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION)) {
        DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** Error setting minimum TLS version 1.3 for SSL CTX"));
        SSL_CTX_free(ctx);
    }
//
//    SSL_CTX_set_ecdh_auto(ctx, 1);
//    if (SSL_CTX_set_default_verify_paths(ctx) != 1)
//        DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** Error loading trust store"));
//
//    /* Set the key and cert */
//    if (SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0) {
//        ERR_print_errors_fp(stderr);
//        exit(EXIT_FAILURE);
//    }
//
//    if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0 ) {
//        ERR_print_errors_fp(stderr);
//        exit(EXIT_FAILURE);
//    }
}

SSL_CTX*
create_context()
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    // if using openssl < 1.1.0
//    method = SSLv23_server_method();
    DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** Creating SSL context"));
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** Unable to create SSL ctxt"));
    }
    else {
        configure_context(ctx);
    }
    return ctx;
}


void* create_ssl_fd(void* ssl_ctx, int fd)
{
//#ifdef HAVE_SSL
    SSL* ssl = SSL_new((SSL_CTX*)ssl_ctx);
    if(!ssl) {
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** Unable to create SSL object"));
        return NULL;
    }
    SSL_set_connect_state(ssl);
    (void)SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
    if(!SSL_set_fd(ssl, fd)) {
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** Unable to set SSL_set_fd"));
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
//#else
//    (void)sslctx; (void)fd;
//	return NULL;
//#endif
}

/** setup SSL for comm point */
static int
setup_ssl(struct xfrd_tcp_pipeline* tp, struct xfrd_tcp_set* tcp_set)
{

    // should this be tcp_r fd or both?
    tp->ssl = create_ssl_fd(tcp_set->ssl_ctx, tp->tcp_w->fd);
    if(!tp->ssl) {
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "*** cannot create SSL object"));
        SSL_free(tp->ssl);
        return 0;
    }
    tp->ssl_shake_state = ssl_shake_write;

//#ifdef HAVE_SSL_SET1_HOST
    SSL_set_verify(tp->ssl, SSL_VERIFY_NONE, NULL);
    if(!SSL_set1_host(tp->ssl, NULL)) {
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "*** SSL_set1_host failed"));
        SSL_free(tp->ssl);
        tp->ssl = NULL;
        return 0;
    }

    // Only trust 1.3
    DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** Setting minimum TLS version 1.3 for SSL object"));
    if (!SSL_set_min_proto_version(tp->ssl, TLS1_3_VERSION)) {
        DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** Error setting minimum TLS version 1.3 for SSL object"));
        SSL_free(tp->ssl);
        tp->ssl = NULL;
        return 0;
    }

//
//    if () {
//		/* because we set SSL_VERIFY_PEER, in netevent in
//		 * ssl_handshake, it'll check if the certificate
//		 * verification has succeeded */
//		/* SSL_VERIFY_PEER is set on the sslctx */
//		/* and the certificates to verify with are loaded into
//		 * it with SSL_load_verify_locations or
//		 * SSL_CTX_set_default_verify_paths */
//		/* setting the hostname makes openssl verify the
//		 * host name in the x509 certificate in the
//		 * SSL connection*/
//		if(!SSL_set1_host(cp->ssl, host)) {
//			log_err("SSL_set1_host failed");
//			return 0;
//		}
//	}
//#elif defined(HAVE_X509_VERIFY_PARAM_SET1_HOST)
//    /* openssl 1.0.2 has this function that can be used for
//	 * set1_host like verification */
//	if((SSL_CTX_get_verify_mode(outnet->sslctx)&SSL_VERIFY_PEER)) {
//		X509_VERIFY_PARAM* param = SSL_get0_param(cp->ssl);
//#  ifdef X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS
//		X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
//#  endif
//		if(!X509_VERIFY_PARAM_set1_host(param, host, strlen(host))) {
//			log_err("X509_VERIFY_PARAM_set1_host failed");
//			return 0;
//		}
//	}
//#else
//    (void)host;
//#endif /* HAVE_SSL_SET1_HOST */
    return 1;
}




/* sort tcppipe, first on IP address, for an IPaddresss, sort on num_unused */
static int
xfrd_pipe_cmp(const void* a, const void* b)
{
	const struct xfrd_tcp_pipeline* x = (struct xfrd_tcp_pipeline*)a;
	const struct xfrd_tcp_pipeline* y = (struct xfrd_tcp_pipeline*)b;
	int r;
	if(x == y)
		return 0;
	if(y->ip_len != x->ip_len)
		/* subtraction works because nonnegative and small numbers */
		return (int)y->ip_len - (int)x->ip_len;
	r = memcmp(&x->ip, &y->ip, x->ip_len);
	if(r != 0)
		return r;
	/* sort that num_unused is sorted ascending, */
	if(x->num_unused != y->num_unused) {
		return (x->num_unused < y->num_unused) ? -1 : 1;
	}
	/* different pipelines are different still, even with same numunused*/
	return (uintptr_t)x < (uintptr_t)y ? -1 : 1;
}

struct xfrd_tcp_set* xfrd_tcp_set_create(struct region* region)
{
	int i;
	struct xfrd_tcp_set* tcp_set = region_alloc(region,
		sizeof(struct xfrd_tcp_set));
	memset(tcp_set, 0, sizeof(struct xfrd_tcp_set));
	tcp_set->tcp_count = 0;
	tcp_set->tcp_waiting_first = 0;
	tcp_set->tcp_waiting_last = 0;
	// Set up SSL Context
	tcp_set->ssl_ctx = create_context();
    for(i=0; i<XFRD_MAX_TCP; i++)
		tcp_set->tcp_state[i] = xfrd_tcp_pipeline_create(region, tcp_set);
	tcp_set->pipetree = rbtree_create(region, &xfrd_pipe_cmp);
	return tcp_set;
}

struct xfrd_tcp_pipeline*
xfrd_tcp_pipeline_create(region_type* region, struct xfrd_tcp_set* tcp_set)
{
	int i;
	struct xfrd_tcp_pipeline* tp = (struct xfrd_tcp_pipeline*)
		region_alloc_zero(region, sizeof(*tp));
	tp->num_unused = ID_PIPE_NUM;
	assert(sizeof(tp->unused)/sizeof(tp->unused[0]) == ID_PIPE_NUM);
	for(i=0; i<ID_PIPE_NUM; i++)
		tp->unused[i] = (uint16_t)i;
	tp->tcp_r = xfrd_tcp_create(region, QIOBUFSZ);
	tp->tcp_w = xfrd_tcp_create(region, 512);

	return tp;
}

void
xfrd_setup_packet(buffer_type* packet,
	uint16_t type, uint16_t klass, const dname_type* dname, uint16_t qid)
{
	/* Set up the header */
	buffer_clear(packet);
	ID_SET(packet, qid);
	FLAGS_SET(packet, 0);
	OPCODE_SET(packet, OPCODE_QUERY);
	QDCOUNT_SET(packet, 1);
	ANCOUNT_SET(packet, 0);
	NSCOUNT_SET(packet, 0);
	ARCOUNT_SET(packet, 0);
	buffer_skip(packet, QHEADERSZ);

	/* The question record. */
	buffer_write(packet, dname_name(dname), dname->name_size);
	buffer_write_u16(packet, type);
	buffer_write_u16(packet, klass);
}

static socklen_t
#ifdef INET6
xfrd_acl_sockaddr(acl_options_type* acl, unsigned int port,
	struct sockaddr_storage *sck)
#else
xfrd_acl_sockaddr(acl_options_type* acl, unsigned int port,
	struct sockaddr_in *sck, const char* fromto)
#endif /* INET6 */
{
	/* setup address structure */
#ifdef INET6
	memset(sck, 0, sizeof(struct sockaddr_storage));
#else
	memset(sck, 0, sizeof(struct sockaddr_in));
#endif
	if(acl->is_ipv6) {
#ifdef INET6
		struct sockaddr_in6* sa = (struct sockaddr_in6*)sck;
		sa->sin6_family = AF_INET6;
		sa->sin6_port = htons(port);
		sa->sin6_addr = acl->addr.addr6;
		return sizeof(struct sockaddr_in6);
#else
		log_msg(LOG_ERR, "xfrd: IPv6 connection %s %s attempted but no \
INET6.", fromto, acl->ip_address_spec);
		return 0;
#endif
	} else {
		struct sockaddr_in* sa = (struct sockaddr_in*)sck;
		sa->sin_family = AF_INET;
		sa->sin_port = htons(port);
		sa->sin_addr = acl->addr.addr;
		return sizeof(struct sockaddr_in);
	}
}

socklen_t
#ifdef INET6
xfrd_acl_sockaddr_to(acl_options_type* acl, struct sockaddr_storage *to)
#else
xfrd_acl_sockaddr_to(acl_options_type* acl, struct sockaddr_in *to)
#endif /* INET6 */
{
	unsigned int port = acl->port?acl->port:(unsigned)atoi(TCP_PORT);
#ifdef INET6
	return xfrd_acl_sockaddr(acl, port, to);
#else
	return xfrd_acl_sockaddr(acl, port, to, "to");
#endif /* INET6 */
}

socklen_t
#ifdef INET6
xfrd_acl_sockaddr_frm(acl_options_type* acl, struct sockaddr_storage *frm)
#else
xfrd_acl_sockaddr_frm(acl_options_type* acl, struct sockaddr_in *frm)
#endif /* INET6 */
{
	unsigned int port = acl->port?acl->port:0;
#ifdef INET6
	return xfrd_acl_sockaddr(acl, port, frm);
#else
	return xfrd_acl_sockaddr(acl, port, frm, "from");
#endif /* INET6 */
}

void
xfrd_write_soa_buffer(struct buffer* packet,
	const dname_type* apex, struct xfrd_soa* soa)
{
	size_t rdlength_pos;
	uint16_t rdlength;
	buffer_write(packet, dname_name(apex), apex->name_size);

	/* already in network order */
	buffer_write(packet, &soa->type, sizeof(soa->type));
	buffer_write(packet, &soa->klass, sizeof(soa->klass));
	buffer_write(packet, &soa->ttl, sizeof(soa->ttl));
	rdlength_pos = buffer_position(packet);
	buffer_skip(packet, sizeof(rdlength));

	/* uncompressed dnames */
	buffer_write(packet, soa->prim_ns+1, soa->prim_ns[0]);
	buffer_write(packet, soa->email+1, soa->email[0]);

	buffer_write(packet, &soa->serial, sizeof(uint32_t));
	buffer_write(packet, &soa->refresh, sizeof(uint32_t));
	buffer_write(packet, &soa->retry, sizeof(uint32_t));
	buffer_write(packet, &soa->expire, sizeof(uint32_t));
	buffer_write(packet, &soa->minimum, sizeof(uint32_t));

	/* write length of RR */
	rdlength = buffer_position(packet) - rdlength_pos - sizeof(rdlength);
	buffer_write_u16_at(packet, rdlength_pos, rdlength);
}

struct xfrd_tcp*
xfrd_tcp_create(region_type* region, size_t bufsize)
{
	struct xfrd_tcp* tcp_state = (struct xfrd_tcp*)region_alloc(
		region, sizeof(struct xfrd_tcp));
	memset(tcp_state, 0, sizeof(struct xfrd_tcp));
	tcp_state->packet = buffer_create(region, bufsize);
	tcp_state->fd = -1;

	return tcp_state;
}

static struct xfrd_tcp_pipeline*
pipeline_find(struct xfrd_tcp_set* set, xfrd_zone_type* zone)
{
	rbnode_type* sme = NULL;
	struct xfrd_tcp_pipeline* r;
	/* smaller buf than a full pipeline with 64kb ID array, only need
	 * the front part with the key info, this front part contains the
	 * members that the compare function uses. */
	enum { keysize = sizeof(struct xfrd_tcp_pipeline) -
		ID_PIPE_NUM*(sizeof(struct xfrd_zone*) + sizeof(uint16_t)) };
	/* void* type for alignment of the struct,
	 * divide the keysize by ptr-size and then add one to round up */
	void* buf[ (keysize / sizeof(void*)) + 1 ];
	struct xfrd_tcp_pipeline* key = (struct xfrd_tcp_pipeline*)buf;
	key->node.key = key;
	key->ip_len = xfrd_acl_sockaddr_to(zone->master, &key->ip);
	key->num_unused = ID_PIPE_NUM;
	/* lookup existing tcp transfer to the master with highest unused */
	if(rbtree_find_less_equal(set->pipetree, key, &sme)) {
		/* exact match, strange, fully unused tcp cannot be open */
		assert(0);
	} 
	if(!sme)
		return NULL;
	r = (struct xfrd_tcp_pipeline*)sme->key;
	/* <= key pointed at, is the master correct ? */
	if(r->ip_len != key->ip_len)
		return NULL;
	if(memcmp(&r->ip, &key->ip, key->ip_len) != 0)
		return NULL;
	/* correct master, is there a slot free for this transfer? */
	if(r->num_unused == 0)
		return NULL;
	return r;
}

/* remove zone from tcp waiting list */
static void
tcp_zone_waiting_list_popfirst(struct xfrd_tcp_set* set, xfrd_zone_type* zone)
{
	assert(zone->tcp_waiting);
	set->tcp_waiting_first = zone->tcp_waiting_next;
	if(zone->tcp_waiting_next)
		zone->tcp_waiting_next->tcp_waiting_prev = NULL;
	else	set->tcp_waiting_last = 0;
	zone->tcp_waiting_next = 0;
	zone->tcp_waiting = 0;
}

/* remove zone from tcp pipe write-wait list */
static void
tcp_pipe_sendlist_remove(struct xfrd_tcp_pipeline* tp, xfrd_zone_type* zone)
{
	if(zone->in_tcp_send) {
		if(zone->tcp_send_prev)
			zone->tcp_send_prev->tcp_send_next=zone->tcp_send_next;
		else	tp->tcp_send_first=zone->tcp_send_next;
		if(zone->tcp_send_next)
			zone->tcp_send_next->tcp_send_prev=zone->tcp_send_prev;
		else	tp->tcp_send_last=zone->tcp_send_prev;
		zone->in_tcp_send = 0;
	}
}

/* remove first from write-wait list */
static void
tcp_pipe_sendlist_popfirst(struct xfrd_tcp_pipeline* tp, xfrd_zone_type* zone)
{
	tp->tcp_send_first = zone->tcp_send_next;
	if(tp->tcp_send_first)
		tp->tcp_send_first->tcp_send_prev = NULL;
	else	tp->tcp_send_last = NULL;
	zone->in_tcp_send = 0;
}

/* remove zone from tcp pipe ID map */
static void
tcp_pipe_id_remove(struct xfrd_tcp_pipeline* tp, xfrd_zone_type* zone)
{
	assert(tp->num_unused < ID_PIPE_NUM && tp->num_unused >= 0);
	assert(tp->id[zone->query_id] == zone);
	tp->id[zone->query_id] = NULL;
	tp->unused[tp->num_unused] = zone->query_id;
	/* must remove and re-add for sort order in tree */
	(void)rbtree_delete(xfrd->tcp_set->pipetree, &tp->node);
	tp->num_unused++;
	(void)rbtree_insert(xfrd->tcp_set->pipetree, &tp->node);
}

/* stop the tcp pipe (and all its zones need to retry) */
static void
xfrd_tcp_pipe_stop(struct xfrd_tcp_pipeline* tp)
{
	int i, conn = -1;
    DEBUG(DEBUG_XFRD,1, (LOG_INFO, "*** num unused %d, num skip %d", tp->num_unused, tp->num_skip));
    assert(tp->num_unused < ID_PIPE_NUM); /* at least one 'in-use' */
	assert(ID_PIPE_NUM - tp->num_unused > tp->num_skip); /* at least one 'nonskip' */
	/* need to retry for all the zones connected to it */
	/* these could use different lists and go to a different nextmaster*/
	for(i=0; i<ID_PIPE_NUM; i++) {
		if(tp->id[i] && tp->id[i] != TCP_NULL_SKIP) {
			xfrd_zone_type* zone = tp->id[i];
			conn = zone->tcp_conn;
			zone->tcp_conn = -1;
			zone->tcp_waiting = 0;
			tcp_pipe_sendlist_remove(tp, zone);
			tcp_pipe_id_remove(tp, zone);
			xfrd_set_refresh_now(zone);
		}
	}
	assert(conn != -1);
	/* now release the entire tcp pipe */
	xfrd_tcp_pipe_release(xfrd->tcp_set, tp, conn);
}

static void
tcp_pipe_reset_timeout(struct xfrd_tcp_pipeline* tp)
{
	int fd = tp->handler.ev_fd;
	struct timeval tv;
	tv.tv_sec = xfrd->tcp_set->tcp_timeout;
	tv.tv_usec = 0;
	if(tp->handler_added)
		event_del(&tp->handler);
	memset(&tp->handler, 0, sizeof(tp->handler));
	event_set(&tp->handler, fd, EV_PERSIST|EV_TIMEOUT|EV_READ|
		(tp->tcp_send_first?EV_WRITE:0), xfrd_handle_tcp_pipe, tp);
	if(event_base_set(xfrd->event_base, &tp->handler) != 0)
		log_msg(LOG_ERR, "xfrd tcp: event_base_set failed");
	if(event_add(&tp->handler, &tv) != 0)
		log_msg(LOG_ERR, "xfrd tcp: event_add failed");
	tp->handler_added = 1;
}

/* handle event from fd of tcp pipe */
void
xfrd_handle_tcp_pipe(int ATTR_UNUSED(fd), short event, void* arg)
{
	struct xfrd_tcp_pipeline* tp = (struct xfrd_tcp_pipeline*)arg;
	if((event & EV_WRITE)) {
		tcp_pipe_reset_timeout(tp);
		if(tp->tcp_send_first) {
			DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: event tcp write, zone %s",
				tp->tcp_send_first->apex_str));
			xfrd_tcp_write(tp, tp->tcp_send_first);
		}
	}
	if((event & EV_READ) && tp->handler_added) {
		DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: event tcp read"));
		tcp_pipe_reset_timeout(tp);
		xfrd_tcp_read(tp);
	}
	if((event & EV_TIMEOUT) && tp->handler_added) {
		/* tcp connection timed out */
		DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: event tcp timeout"));
		xfrd_tcp_pipe_stop(tp);
	}
}

/* add a zone to the pipeline, it starts to want to write its query */
static void
pipeline_setup_new_zone(struct xfrd_tcp_set* set, struct xfrd_tcp_pipeline* tp,
	xfrd_zone_type* zone)
{
	/* assign the ID */
	int idx;
	assert(tp->num_unused > 0);
	/* we pick a random ID, even though it is TCP anyway */
	idx = random_generate(tp->num_unused);
	zone->query_id = tp->unused[idx];
	tp->unused[idx] = tp->unused[tp->num_unused-1];
	tp->id[zone->query_id] = zone;
	/* decrement unused counter, and fixup tree */
	(void)rbtree_delete(set->pipetree, &tp->node);
	tp->num_unused--;
	(void)rbtree_insert(set->pipetree, &tp->node);

	/* add to sendlist, at end */
	zone->tcp_send_next = NULL;
	zone->tcp_send_prev = tp->tcp_send_last;
	zone->in_tcp_send = 1;
	if(tp->tcp_send_last)
		tp->tcp_send_last->tcp_send_next = zone;
	else	tp->tcp_send_first = zone;
	tp->tcp_send_last = zone;

	/* is it first in line? */
	if(tp->tcp_send_first == zone) {
		xfrd_tcp_setup_write_packet(tp, zone);
		/* add write to event handler */
		tcp_pipe_reset_timeout(tp);
	}
}

void
xfrd_tcp_obtain(struct xfrd_tcp_set* set, xfrd_zone_type* zone)
{
	struct xfrd_tcp_pipeline* tp;
	assert(zone->tcp_conn == -1);
	assert(zone->tcp_waiting == 0);

	if(set->tcp_count < XFRD_MAX_TCP) {
		int i;
		assert(!set->tcp_waiting_first);
		set->tcp_count ++;
		/* find a free tcp_buffer */
		for(i=0; i<XFRD_MAX_TCP; i++) {
			if(set->tcp_state[i]->tcp_r->fd == -1) {
				zone->tcp_conn = i;
				break;
			}
		}
		/** What if there is no free tcp_buffer? return; */
		if (zone->tcp_conn < 0) {
			return;
		}

		tp = set->tcp_state[zone->tcp_conn];
		zone->tcp_waiting = 0;

		/* stop udp use (if any) */
		if(zone->zone_handler.ev_fd != -1)
			xfrd_udp_release(zone);

		if(!xfrd_tcp_open(set, tp, zone)) {
			zone->tcp_conn = -1;
			set->tcp_count --;
			xfrd_set_refresh_now(zone);
			return;
		}
		/* ip and ip_len set by tcp_open */
		tp->node.key = tp;
		tp->num_unused = ID_PIPE_NUM;
		tp->num_skip = 0;
		tp->tcp_send_first = NULL;
		tp->tcp_send_last = NULL;
		memset(tp->id, 0, sizeof(tp->id));
		for(i=0; i<ID_PIPE_NUM; i++) {
			tp->unused[i] = i;
		}

		/* insert into tree */
		(void)rbtree_insert(set->pipetree, &tp->node);
		xfrd_deactivate_zone(zone);
		xfrd_unset_timer(zone);
		pipeline_setup_new_zone(set, tp, zone);
		return;
	}
	/* check for a pipeline to the same master with unused ID */
	if((tp = pipeline_find(set, zone))!= NULL) {
		int i;
		if(zone->zone_handler.ev_fd != -1)
			xfrd_udp_release(zone);
		for(i=0; i<XFRD_MAX_TCP; i++) {
			if(set->tcp_state[i] == tp)
				zone->tcp_conn = i;
		}
		xfrd_deactivate_zone(zone);
		xfrd_unset_timer(zone);
		pipeline_setup_new_zone(set, tp, zone);
		return;
	}

	/* wait, at end of line */
	DEBUG(DEBUG_XFRD,2, (LOG_INFO, "xfrd: max number of tcp "
		"connections (%d) reached.", XFRD_MAX_TCP));
	zone->tcp_waiting_next = 0;
	zone->tcp_waiting_prev = set->tcp_waiting_last;
	zone->tcp_waiting = 1;
	if(!set->tcp_waiting_last) {
		set->tcp_waiting_first = zone;
		set->tcp_waiting_last = zone;
	} else {
		set->tcp_waiting_last->tcp_waiting_next = zone;
		set->tcp_waiting_last = zone;
	}
	xfrd_deactivate_zone(zone);
	xfrd_unset_timer(zone);
}

//int
//xfrd_tcp_open_ssl(struct xfrd_tcp_set* set, struct xfrd_tcp_pipeline* tp,
//              xfrd_zone_type* zone)
//{
//    int fd, family, conn;
//    struct timeval tv;
//    assert(zone->tcp_conn != -1);
//
//    /* if there is no next master, fallback to use the first one */
//    /* but there really should be a master set */
//    if(!zone->master) {
//        zone->master = zone->zone_options->pattern->request_xfr;
//        zone->master_num = 0;
//    }
//
//    DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: SSL zone %s open tcp conn to %s",
//            zone->apex_str, zone->master->ip_address_spec));
//    tp->tcp_r->is_reading = 1;
//    tp->tcp_r->total_bytes = 0;
//    tp->tcp_r->msglen = 0;
//    buffer_clear(tp->tcp_r->packet);
//    tp->tcp_w->is_reading = 0;
//    tp->tcp_w->total_bytes = 0;
//    tp->tcp_w->msglen = 0;
//    tp->connection_established = 0;
//
//    if(zone->master->is_ipv6) {
//#ifdef INET6
//        family = PF_INET6;
//#else
//        xfrd_set_refresh_now(zone);
//		return 0;
//#endif
//    } else {
//        family = PF_INET;
//    }
//    fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
//
//    BIO *web, *out = NULL;
//    long res = 1;
//    if (!(web = BIO_new_ssl_connect(set->ssl_ctx)))
//        DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "*** BIO new ssl connect failed"));
//    BIO_set_conn_hostname(web, "69.172.188.13:853"); // hardcoded
//
//    BIO_get_ssl(web, &tp->ssl);
//    if(!(tp->ssl != NULL))
//        DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "*** BIO BIO_get_ssl failed"));
//
//    out = BIO_new_fp(stdout, BIO_NOCLOSE);
//    if(!(NULL != out))
//        DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "*** BIO BIO_new_fp failed"));
//
//    res = BIO_do_connect(web);
//    if(!(1 == res))
//        DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "*** BIO do connect failed"));
//
//    res = BIO_do_handshake(web);
//    if(!(1 == res))
//        DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "*** BIO do handshake failed"));
//
//
//
//    if(fd == -1) {
//        /* squelch 'Address family not supported by protocol' at low
//         * verbosity levels */
//        if(errno != EAFNOSUPPORT || verbosity > 2)
//            log_msg(LOG_ERR, "xfrd: %s cannot create tcp socket: %s",
//                    zone->master->ip_address_spec, strerror(errno));
//        xfrd_set_refresh_now(zone);
//        return 0;
//    }
//    if(fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
//        log_msg(LOG_ERR, "xfrd: fcntl failed: %s", strerror(errno));
//        close(fd);
//        xfrd_set_refresh_now(zone);
//        return 0;
//    }
//
//    if(xfrd->nsd->outgoing_tcp_mss > 0) {
//#if defined(IPPROTO_TCP) && defined(TCP_MAXSEG)
//        if(setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG,
//                      (void*)&xfrd->nsd->outgoing_tcp_mss,
//                      sizeof(xfrd->nsd->outgoing_tcp_mss)) < 0) {
//            log_msg(LOG_ERR, "xfrd: setsockopt(TCP_MAXSEG)"
//                             "failed: %s", strerror(errno));
//        }
//#else
//        log_msg(LOG_ERR, "setsockopt(TCP_MAXSEG) unsupported");
//#endif
//    }
//
//    tp->ip_len = xfrd_acl_sockaddr_to(zone->master, &tp->ip);
//
//    /* bind it */
//    if (!xfrd_bind_local_interface(fd, zone->zone_options->pattern->
//            outgoing_interface, zone->master, 1)) {
//        close(fd);
//        xfrd_set_refresh_now(zone);
//        return 0;
//    }
//
//    conn = connect(fd, (struct sockaddr*)&tp->ip, tp->ip_len);
//    if (conn == -1 && errno != EINPROGRESS) {
//        log_msg(LOG_ERR, "xfrd: connect %s failed: %s",
//                zone->master->ip_address_spec, strerror(errno));
//        close(fd);
//        xfrd_set_refresh_now(zone);
//        return 0;
//    }
//    tp->tcp_r->fd = fd;
//    tp->tcp_w->fd = fd;
//
//    /* set the tcp pipe event */
//    if(tp->handler_added)
//        event_del(&tp->handler);
//    memset(&tp->handler, 0, sizeof(tp->handler));
//    event_set(&tp->handler, fd, EV_PERSIST|EV_TIMEOUT|EV_READ|EV_WRITE,
//              xfrd_handle_tcp_pipe, tp);
//    if(event_base_set(xfrd->event_base, &tp->handler) != 0)
//        log_msg(LOG_ERR, "xfrd tcp: event_base_set failed");
//    tv.tv_sec = set->tcp_timeout;
//    tv.tv_usec = 0;
//    if(event_add(&tp->handler, &tv) != 0)
//        log_msg(LOG_ERR, "xfrd tcp: event_add failed");
//    tp->handler_added = 1;
//    return 1;
//}


int
xfrd_tcp_open(struct xfrd_tcp_set* set, struct xfrd_tcp_pipeline* tp,
	xfrd_zone_type* zone)
{
	int fd, family, conn;
	struct timeval tv;
	assert(zone->tcp_conn != -1);

	/* if there is no next master, fallback to use the first one */
	/* but there really should be a master set */
	if(!zone->master) {
		zone->master = zone->zone_options->pattern->request_xfr;
		zone->master_num = 0;
	}

	DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: zone %s open tcp conn to %s",
		zone->apex_str, zone->master->ip_address_spec));
	tp->tcp_r->is_reading = 1;
	tp->tcp_r->total_bytes = 0;
	tp->tcp_r->msglen = 0;
	buffer_clear(tp->tcp_r->packet);
	tp->tcp_w->is_reading = 0;
	tp->tcp_w->total_bytes = 0;
	tp->tcp_w->msglen = 0;
	tp->connection_established = 0;

	if(zone->master->is_ipv6) {
#ifdef INET6
		family = PF_INET6;
#else
		xfrd_set_refresh_now(zone);
		return 0;
#endif
	} else {
		family = PF_INET;
	}
	fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
	if(fd == -1) {
		/* squelch 'Address family not supported by protocol' at low
		 * verbosity levels */
		if(errno != EAFNOSUPPORT || verbosity > 2)
		    log_msg(LOG_ERR, "xfrd: %s cannot create tcp socket: %s",
			zone->master->ip_address_spec, strerror(errno));
		xfrd_set_refresh_now(zone);
		return 0;
	}
	if(fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
		log_msg(LOG_ERR, "xfrd: fcntl failed: %s", strerror(errno));
		close(fd);
		xfrd_set_refresh_now(zone);
		return 0;
	}

	if(xfrd->nsd->outgoing_tcp_mss > 0) {
#if defined(IPPROTO_TCP) && defined(TCP_MAXSEG)
		if(setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG,
			(void*)&xfrd->nsd->outgoing_tcp_mss,
			sizeof(xfrd->nsd->outgoing_tcp_mss)) < 0) {
			log_msg(LOG_ERR, "xfrd: setsockopt(TCP_MAXSEG)"
					"failed: %s", strerror(errno));
		}
#else
		log_msg(LOG_ERR, "setsockopt(TCP_MAXSEG) unsupported");
#endif
	}

	tp->ip_len = xfrd_acl_sockaddr_to(zone->master, &tp->ip);

    DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** in tcp open, about to bind"));


    /* bind it */
	if (!xfrd_bind_local_interface(fd, zone->zone_options->pattern->
		outgoing_interface, zone->master, 1)) {
		close(fd);
		xfrd_set_refresh_now(zone);
		return 0;
        }

	conn = connect(fd, (struct sockaddr*)&tp->ip, tp->ip_len);
	if (conn == -1 && errno != EINPROGRESS) {
		log_msg(LOG_ERR, "xfrd: connect %s failed: %s",
			zone->master->ip_address_spec, strerror(errno));
		close(fd);
		xfrd_set_refresh_now(zone);
		return 0;
	}

    DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** in tcp open, trying to do SSL connect"));

    if(!setup_ssl(tp, set)) {
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "*** Cannot setup SSL on pipeline"));
    } else {
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "*** SSL setup successfully!"));
    }

    BIO *sbio = BIO_new_socket(fd,BIO_NOCLOSE);
    SSL_set_bio(tp->ssl,sbio,sbio);
    int ret, err;
    while(1) {
        ERR_clear_error();
        if( (ret=SSL_do_handshake(tp->ssl)) == 1) {
            DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** SSL handshake worked!"));
            break;
        }
        err = SSL_get_error(tp->ssl, ret);
        if(err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** SSL connect failed in tcp open with return value %d", err));
            DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** Min proto version for ctx is %lx and object is %lx",
                    SSL_CTX_get_min_proto_version(xfrd->tcp_set->ssl_ctx),
                    SSL_get_min_proto_version(tp->ssl)));
            close(fd);
            xfrd_set_refresh_now(zone);
            return 0;
        }
        /* else wants to be called again */
    }



	tp->tcp_r->fd = fd;
	tp->tcp_w->fd = fd;

	/* set the tcp pipe event */
	if(tp->handler_added)
		event_del(&tp->handler);
	memset(&tp->handler, 0, sizeof(tp->handler));
	event_set(&tp->handler, fd, EV_PERSIST|EV_TIMEOUT|EV_READ|EV_WRITE,
		xfrd_handle_tcp_pipe, tp);
	if(event_base_set(xfrd->event_base, &tp->handler) != 0)
		log_msg(LOG_ERR, "xfrd tcp: event_base_set failed");
	tv.tv_sec = set->tcp_timeout;
	tv.tv_usec = 0;
	if(event_add(&tp->handler, &tv) != 0)
		log_msg(LOG_ERR, "xfrd tcp: event_add failed");
	tp->handler_added = 1;
	return 1;
}

void
xfrd_tcp_setup_write_packet(struct xfrd_tcp_pipeline* tp, xfrd_zone_type* zone)
{
	struct xfrd_tcp* tcp = tp->tcp_w;
	assert(zone->tcp_conn != -1);
	assert(zone->tcp_waiting == 0);
	/* start AXFR or IXFR for the zone */
	if(zone->soa_disk_acquired == 0 || zone->master->use_axfr_only ||
		zone->master->ixfr_disabled ||
		/* if zone expired, after the first round, do not ask for
		 * IXFR any more, but full AXFR (of any serial number) */
		(zone->state == xfrd_zone_expired && zone->round_num != 0)) {
		DEBUG(DEBUG_XFRD,1, (LOG_INFO, "request full zone transfer "
						"(AXFR) for %s to %s",
			zone->apex_str, zone->master->ip_address_spec));

		xfrd_setup_packet(tcp->packet, TYPE_AXFR, CLASS_IN, zone->apex,
			zone->query_id);
	} else {
		DEBUG(DEBUG_XFRD,1, (LOG_INFO, "request incremental zone "
						"transfer (IXFR) for %s to %s",
			zone->apex_str, zone->master->ip_address_spec));

		xfrd_setup_packet(tcp->packet, TYPE_IXFR, CLASS_IN, zone->apex,
			zone->query_id);
        	NSCOUNT_SET(tcp->packet, 1);
		xfrd_write_soa_buffer(tcp->packet, zone->apex, &zone->soa_disk);
	}
	/* old transfer needs to be removed still? */
	if(zone->msg_seq_nr)
		xfrd_unlink_xfrfile(xfrd->nsd, zone->xfrfilenumber);
	zone->msg_seq_nr = 0;
	zone->msg_rr_count = 0;
	if(zone->master->key_options && zone->master->key_options->tsig_key) {
		xfrd_tsig_sign_request(tcp->packet, &zone->tsig, zone->master);
	}
	buffer_flip(tcp->packet);
	DEBUG(DEBUG_XFRD,1, (LOG_INFO, "sent tcp query with ID %d", zone->query_id));
	tcp->msglen = buffer_limit(tcp->packet);
	tcp->total_bytes = 0;
}

static void
tcp_conn_ready_for_reading(struct xfrd_tcp* tcp)
{
	tcp->total_bytes = 0;
	tcp->msglen = 0;
	buffer_clear(tcp->packet);
}

int conn_write_ssl(struct xfrd_tcp* tcp, SSL* ssl)
{
    ssize_t sent;
    int res;

    DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** in conn_write_ssl"));

    if(tcp->total_bytes < sizeof(tcp->msglen)) {
        uint16_t sendlen = htons(tcp->msglen);

        // send
        DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** SSL sending write"));
        int request_length = sizeof(tcp->msglen) - tcp->total_bytes;
        sent = SSL_write(ssl,
                          (const char*)&sendlen + tcp->total_bytes,
                          request_length);


        switch(SSL_get_error(ssl,sent)) {
            case SSL_ERROR_NONE:
                if(request_length != sent)
                    DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** Incomplete write in conn_write_ssl"));
                break;
            default:
                DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** generic write problem in conn_write_ssl"));
        }

        if(sent == -1) {
            if(errno == EAGAIN || errno == EINTR) {
                /* write would block, try later */
                return 0;
            } else {
                return -1;
            }
        }

        tcp->total_bytes += sent;
        if(sent > (ssize_t)sizeof(tcp->msglen))
            buffer_skip(tcp->packet, sent-sizeof(tcp->msglen));
        if(tcp->total_bytes < sizeof(tcp->msglen)) {
            /* incomplete write, resume later */
            return 0;
        }
        assert(tcp->total_bytes >= sizeof(tcp->msglen));
    }

    assert(tcp->total_bytes < tcp->msglen + sizeof(tcp->msglen));

    int request_length = buffer_remaining(tcp->packet);
    DEBUG(DEBUG_XFRD, 1, (LOG_INFO, "xfrd: *** SSL sending another write"));

    sent = SSL_write(ssl,
                     buffer_current(tcp->packet),
                     request_length);


    switch(SSL_get_error(ssl,sent)) {
        case SSL_ERROR_NONE:
            if(request_length != sent)
                DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** Incomplete write in conn_write_ssl"));
            break;
        default:
            DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** generic write problem in conn_write_ssl"));
    }


    if(sent == -1) {
        if(errno == EAGAIN || errno == EINTR) {
            /* write would block, try later */
            return 0;
        } else {
            return -1;
        }
    }

    buffer_skip(tcp->packet, sent);
    tcp->total_bytes += sent;

    if(tcp->total_bytes < tcp->msglen + sizeof(tcp->msglen)) {
        /* more to write when socket becomes writable again */
        return 0;
    }

    assert(tcp->total_bytes == tcp->msglen + sizeof(tcp->msglen));
    return 1;

}

int conn_write(struct xfrd_tcp* tcp)
{
	ssize_t sent;

	if(tcp->total_bytes < sizeof(tcp->msglen)) {
		uint16_t sendlen = htons(tcp->msglen);
#ifdef HAVE_WRITEV
		struct iovec iov[2];
		iov[0].iov_base = (uint8_t*)&sendlen + tcp->total_bytes;
		iov[0].iov_len = sizeof(sendlen) - tcp->total_bytes;
		iov[1].iov_base = buffer_begin(tcp->packet);
		iov[1].iov_len = buffer_limit(tcp->packet);
		sent = writev(tcp->fd, iov, 2);
#else /* HAVE_WRITEV */
		sent = write(tcp->fd,
			(const char*)&sendlen + tcp->total_bytes,
			sizeof(tcp->msglen) - tcp->total_bytes);
#endif /* HAVE_WRITEV */

		if(sent == -1) {
			if(errno == EAGAIN || errno == EINTR) {
				/* write would block, try later */
				return 0;
			} else {
				return -1;
			}
		}

		tcp->total_bytes += sent;
		if(sent > (ssize_t)sizeof(tcp->msglen))
			buffer_skip(tcp->packet, sent-sizeof(tcp->msglen));
		if(tcp->total_bytes < sizeof(tcp->msglen)) {
			/* incomplete write, resume later */
			return 0;
		}
#ifdef HAVE_WRITEV
		if(tcp->total_bytes == tcp->msglen + sizeof(tcp->msglen)) {
			/* packet done */
			return 1;
		}
#endif
		assert(tcp->total_bytes >= sizeof(tcp->msglen));
	}

	assert(tcp->total_bytes < tcp->msglen + sizeof(tcp->msglen));

	sent = write(tcp->fd,
		buffer_current(tcp->packet),
		buffer_remaining(tcp->packet));
	if(sent == -1) {
		if(errno == EAGAIN || errno == EINTR) {
			/* write would block, try later */
			return 0;
		} else {
			return -1;
		}
	}

	buffer_skip(tcp->packet, sent);
	tcp->total_bytes += sent;

	if(tcp->total_bytes < tcp->msglen + sizeof(tcp->msglen)) {
		/* more to write when socket becomes writable again */
		return 0;
	}

	assert(tcp->total_bytes == tcp->msglen + sizeof(tcp->msglen));
	return 1;
}

void
xfrd_tcp_write(struct xfrd_tcp_pipeline* tp, xfrd_zone_type* zone)
{
    DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** tcp write!"));
    int ret;
	struct xfrd_tcp* tcp = tp->tcp_w;
	assert(zone->tcp_conn != -1);
	assert(zone == tp->tcp_send_first);
	/* see if for non-established connection, there is a connect error */
	if(!tp->connection_established) {
		/* check for pending error from nonblocking connect */
		/* from Stevens, unix network programming, vol1, 3rd ed, p450 */
		int error = 0;
		socklen_t len = sizeof(error);
		if(getsockopt(tcp->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0){
			error = errno; /* on solaris errno is error */
		}
		if(error == EINPROGRESS || error == EWOULDBLOCK)
			return; /* try again later */
		if(error != 0) {
			log_msg(LOG_ERR, "%s: Could not tcp connect to %s: %s",
				zone->apex_str, zone->master->ip_address_spec,
				strerror(error));
			xfrd_tcp_pipe_stop(tp);
			return;
		}
	}
//	ret = conn_write(tcp);
    DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** zone %s mapping to tcp conn of %s with port %d, attempting SSL write",
            zone->apex_str, zone->master->ip_address_spec, zone->master->port));
    ret = conn_write_ssl(tcp, tp->ssl);
    DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** conn_write_ssl returned %d", ret));
    if(ret == -1) {
		log_msg(LOG_ERR, "xfrd: failed writing tcp %s", strerror(errno));
		xfrd_tcp_pipe_stop(tp);
		return;
	}
	if(tcp->total_bytes != 0 && !tp->connection_established)
		tp->connection_established = 1;
	if(ret == 0) {
		return; /* write again later */
	}
	/* done writing this message */

	/* remove first zone from sendlist */
	tcp_pipe_sendlist_popfirst(tp, zone);

	/* see if other zone wants to write; init; let it write (now) */
	/* and use a loop, because 64k stack calls is a too much */
	while(tp->tcp_send_first) {
		/* setup to write for this zone */
		xfrd_tcp_setup_write_packet(tp, tp->tcp_send_first);
		/* attempt to write for this zone (if success, continue loop)*/
//		ret = conn_write(tcp);
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** zone %s mapping to tcp conn of %s, attempting YET ANOTHER SSL write",
                zone->apex_str, zone->master->ip_address_spec));
		ret = conn_write_ssl(tcp, tp->ssl);
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: *** conn_write_ssl returned %d", ret));
        if(ret == -1) {
			log_msg(LOG_ERR, "xfrd: failed writing tcp %s", strerror(errno));
			xfrd_tcp_pipe_stop(tp);
			return;
		}
		if(ret == 0)
			return; /* write again later */
		tcp_pipe_sendlist_popfirst(tp, tp->tcp_send_first);
	}

	/* if sendlist empty, remove WRITE from event */

	/* listen to READ, and not WRITE events */
	assert(tp->tcp_send_first == NULL);
	tcp_pipe_reset_timeout(tp);
}

int
conn_read_ssl(struct xfrd_tcp* tcp, SSL* ssl)
{
    ssize_t received;
    /* receive leading packet length bytes */
    DEBUG(DEBUG_XFRD,1, (LOG_INFO,
            "xfrd: *** in ssl_read total bytes %d and msglen %lu", tcp->total_bytes, sizeof(tcp->msglen)));
    if(tcp->total_bytes < sizeof(tcp->msglen)) {
        ERR_clear_error();

        received = SSL_read(ssl,
                        (char*) &tcp->msglen + tcp->total_bytes,
                        sizeof(tcp->msglen) - tcp->total_bytes);
        if (received <= 0) {
            int err =SSL_get_error(ssl, received);
            DEBUG(DEBUG_XFRD,1, (LOG_INFO,
                    "xfrd: *** xyzzy ssl_read returned error %d", err));

            if(err == SSL_ERROR_ZERO_RETURN) {
                /* EOF */
                return 0;
            }
        }

        if(received == -1) {
            if(errno == EAGAIN || errno == EINTR) {
                /* read would block, try later */
                return 0;
            } else {
#ifdef ECONNRESET
                if (verbosity >= 2 || errno != ECONNRESET)
#endif /* ECONNRESET */
                    log_msg(LOG_ERR, "tcp read %s", strerror(errno));
                return -1;
            }
        } else if(received == 0) {
            /* EOF */
            return -1;
        }



        tcp->total_bytes += received;
        if(tcp->total_bytes < sizeof(tcp->msglen)) {
            /* not complete yet, try later */
            return 0;
        }

        assert(tcp->total_bytes == sizeof(tcp->msglen));
        tcp->msglen = ntohs(tcp->msglen);

        if(tcp->msglen == 0) {
            buffer_set_limit(tcp->packet, tcp->msglen);
            return 1;
        }
        if(tcp->msglen > buffer_capacity(tcp->packet)) {
            log_msg(LOG_ERR, "buffer too small, dropping connection");
            return 0;
        }
        buffer_set_limit(tcp->packet, tcp->msglen);
    }

    assert(buffer_remaining(tcp->packet) > 0);
    ERR_clear_error();

    DEBUG(DEBUG_XFRD,1, (LOG_INFO,
            "xfrd: *** xyzzy doing ssl_read again!"));

    received = SSL_read(ssl, buffer_current(tcp->packet),
                    buffer_remaining(tcp->packet));

    if (received <= 0) {
        int err =SSL_get_error(ssl, received);
        DEBUG(DEBUG_XFRD,1, (LOG_INFO,
                "xfrd: *** xyzzy ssl_read returned error %d", err));

        if(err == SSL_ERROR_ZERO_RETURN) {
            /* EOF */
            return 0;
        }
    }

    if(received == -1) {
        if(errno == EAGAIN || errno == EINTR) {
            /* read would block, try later */
            return 0;
        } else {
#ifdef ECONNRESET
            if (verbosity >= 2 || errno != ECONNRESET)
#endif /* ECONNRESET */
                log_msg(LOG_ERR, "tcp read %s", strerror(errno));
            return -1;
        }
    } else if(received == 0) {
        /* EOF */
        return -1;
    }

    tcp->total_bytes += received;
    buffer_skip(tcp->packet, received);

    if(buffer_remaining(tcp->packet) > 0) {
        /* not complete yet, wait for more */
        return 0;
    }

    /* completed */
    assert(buffer_position(tcp->packet) == tcp->msglen);
    return 1;
}


int
conn_read(struct xfrd_tcp* tcp)
{
	ssize_t received;
	/* receive leading packet length bytes */
	if(tcp->total_bytes < sizeof(tcp->msglen)) {
		received = read(tcp->fd,
			(char*) &tcp->msglen + tcp->total_bytes,
			sizeof(tcp->msglen) - tcp->total_bytes);
		if(received == -1) {
			if(errno == EAGAIN || errno == EINTR) {
				/* read would block, try later */
				return 0;
			} else {
#ifdef ECONNRESET
				if (verbosity >= 2 || errno != ECONNRESET)
#endif /* ECONNRESET */
				log_msg(LOG_ERR, "tcp read sz: %s", strerror(errno));
				return -1;
			}
		} else if(received == 0) {
			/* EOF */
			return -1;
		}
		tcp->total_bytes += received;
		if(tcp->total_bytes < sizeof(tcp->msglen)) {
			/* not complete yet, try later */
			return 0;
		}

		assert(tcp->total_bytes == sizeof(tcp->msglen));
		tcp->msglen = ntohs(tcp->msglen);

		if(tcp->msglen == 0) {
			buffer_set_limit(tcp->packet, tcp->msglen);
			return 1;
		}
		if(tcp->msglen > buffer_capacity(tcp->packet)) {
			log_msg(LOG_ERR, "buffer too small, dropping connection");
			return 0;
		}
		buffer_set_limit(tcp->packet, tcp->msglen);
	}

	assert(buffer_remaining(tcp->packet) > 0);

	received = read(tcp->fd, buffer_current(tcp->packet),
		buffer_remaining(tcp->packet));
	if(received == -1) {
		if(errno == EAGAIN || errno == EINTR) {
			/* read would block, try later */
			return 0;
		} else {
#ifdef ECONNRESET
			if (verbosity >= 2 || errno != ECONNRESET)
#endif /* ECONNRESET */
			log_msg(LOG_ERR, "tcp read %s", strerror(errno));
			return -1;
		}
	} else if(received == 0) {
		/* EOF */
		return -1;
	}

	tcp->total_bytes += received;
	buffer_skip(tcp->packet, received);

	if(buffer_remaining(tcp->packet) > 0) {
		/* not complete yet, wait for more */
		return 0;
	}

	/* completed */
	assert(buffer_position(tcp->packet) == tcp->msglen);
	return 1;
}

void
xfrd_tcp_read(struct xfrd_tcp_pipeline* tp)
{
	xfrd_zone_type* zone;
	struct xfrd_tcp* tcp = tp->tcp_r;
	int ret;
	enum xfrd_packet_result pkt_result;
//	ret = conn_read(tcp);
	ret = conn_read_ssl(tcp, tp->ssl);
    DEBUG(DEBUG_XFRD,1, (LOG_INFO,
            "xfrd: *** xyzzy ssl conn_read returned %d", ret));
	if(ret == -1) {
		DEBUG(DEBUG_XFRD,1, (LOG_INFO,
			"xfrd: *** xyzzy ssl conn_read returned -1, stopping pipe"));
		xfrd_tcp_pipe_stop(tp);
		return;
	}
	if(ret == 0)
		return;
	/* completed msg */
	buffer_flip(tcp->packet);
	/* see which ID number it is, if skip, handle skip, NULL: warn */
	if(tcp->msglen < QHEADERSZ) {
		/* too short for DNS header, skip it */
		DEBUG(DEBUG_XFRD,1, (LOG_INFO,
			"xfrd: tcp skip response that is too short"));
		tcp_conn_ready_for_reading(tcp);
		return;
	}
	zone = tp->id[ID(tcp->packet)];
	if(!zone || zone == TCP_NULL_SKIP) {
		/* no zone for this id? skip it */
		DEBUG(DEBUG_XFRD,1, (LOG_INFO,
			"xfrd: tcp skip response with %s ID",
			zone?"set-to-skip":"unknown"));
		tcp_conn_ready_for_reading(tcp);
		return;
	}
	assert(zone->tcp_conn != -1);

	/* handle message for zone */
	pkt_result = xfrd_handle_received_xfr_packet(zone, tcp->packet);
	/* setup for reading the next packet on this connection */
	tcp_conn_ready_for_reading(tcp);
	switch(pkt_result) {
		case xfrd_packet_more:
			/* wait for next packet */
			break;
		case xfrd_packet_newlease:
			/* set to skip if more packets with this ID */
			tp->id[zone->query_id] = TCP_NULL_SKIP;
			tp->num_skip++;
			/* fall through to remove zone from tp */
			/* fallthrough */
		case xfrd_packet_transfer:
			if(zone->zone_options->pattern->multi_master_check) {
				xfrd_tcp_release(xfrd->tcp_set, zone);
				xfrd_make_request(zone);
				break;
			}
			xfrd_tcp_release(xfrd->tcp_set, zone);
			assert(zone->round_num == -1);
			break;
		case xfrd_packet_notimpl:
			xfrd_disable_ixfr(zone);
			xfrd_tcp_release(xfrd->tcp_set, zone);
			/* query next server */
			xfrd_make_request(zone);
			break;
		case xfrd_packet_bad:
		case xfrd_packet_tcp:
		default:
			/* set to skip if more packets with this ID */
			tp->id[zone->query_id] = TCP_NULL_SKIP;
			tp->num_skip++;
			xfrd_tcp_release(xfrd->tcp_set, zone);
			/* query next server */
			xfrd_make_request(zone);
			break;
	}
}

void
xfrd_tcp_release(struct xfrd_tcp_set* set, xfrd_zone_type* zone)
{
	int conn = zone->tcp_conn;
	struct xfrd_tcp_pipeline* tp = set->tcp_state[conn];
	DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: zone %s released tcp conn to %s",
		zone->apex_str, zone->master->ip_address_spec));
	assert(zone->tcp_conn != -1);
	assert(zone->tcp_waiting == 0);
	zone->tcp_conn = -1;
	zone->tcp_waiting = 0;

	/* remove from tcp_send list */
	tcp_pipe_sendlist_remove(tp, zone);
	/* remove it from the ID list */
	if(tp->id[zone->query_id] != TCP_NULL_SKIP)
		tcp_pipe_id_remove(tp, zone);
	DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: released tcp pipe now %d unused",
		tp->num_unused));
	/* if pipe was full, but no more, then see if waiting element is
	 * for the same master, and can fill the unused ID */
	if(tp->num_unused == 1 && set->tcp_waiting_first) {
#ifdef INET6
		struct sockaddr_storage to;
#else
		struct sockaddr_in to;
#endif
		socklen_t to_len = xfrd_acl_sockaddr_to(
			set->tcp_waiting_first->master, &to);
		if(to_len == tp->ip_len && memcmp(&to, &tp->ip, to_len) == 0) {
			/* use this connection for the waiting zone */
			zone = set->tcp_waiting_first;
			assert(zone->tcp_conn == -1);
			zone->tcp_conn = conn;
			tcp_zone_waiting_list_popfirst(set, zone);
			if(zone->zone_handler.ev_fd != -1)
				xfrd_udp_release(zone);
			xfrd_unset_timer(zone);
			pipeline_setup_new_zone(set, tp, zone);
			return;
		}
		/* waiting zone did not go to same server */
	}

	/* if all unused, or only skipped leftover, close the pipeline */
	if(tp->num_unused >= ID_PIPE_NUM || tp->num_skip >= ID_PIPE_NUM - tp->num_unused)
		xfrd_tcp_pipe_release(set, tp, conn);
}

void
xfrd_tcp_pipe_release(struct xfrd_tcp_set* set, struct xfrd_tcp_pipeline* tp,
	int conn)
{
	DEBUG(DEBUG_XFRD,1, (LOG_INFO, "xfrd: tcp pipe released"));
	/* one handler per tcp pipe */
	if(tp->handler_added)
		event_del(&tp->handler);
	tp->handler_added = 0;

    /* close SSL */
    DEBUG(DEBUG_XFRD,1, (LOG_INFO, "*** Shutting down SSL"));
    int ret = SSL_shutdown(tp->ssl);
    //error handling here if r < 0
    if(!ret)
    {
        DEBUG(DEBUG_XFRD,1, (LOG_INFO, "*** SSL shutdown ran into error with return value %d", ret));
        if (!SSL_shutdown(tp->ssl)) {
            DEBUG(DEBUG_XFRD,1, (LOG_INFO, "*** SSL shutdown ran into error AGAIN, giving up.."));
        }
    }
    SSL_free(tp->ssl);
    tp->ssl = NULL;

	/* fd in tcp_r and tcp_w is the same, close once */
	if(tp->tcp_r->fd != -1)
		close(tp->tcp_r->fd);
	tp->tcp_r->fd = -1;
	tp->tcp_w->fd = -1;

	/* remove from pipetree */
	(void)rbtree_delete(xfrd->tcp_set->pipetree, &tp->node);

	/* a waiting zone can use the free tcp slot (to another server) */
	/* if that zone fails to set-up or connect, we try to start the next
	 * waiting zone in the list */
	while(set->tcp_count == XFRD_MAX_TCP && set->tcp_waiting_first) {
		int i;

		/* pop first waiting process */
		xfrd_zone_type* zone = set->tcp_waiting_first;
		/* start it */
		assert(zone->tcp_conn == -1);
		zone->tcp_conn = conn;
		tcp_zone_waiting_list_popfirst(set, zone);

		/* stop udp (if any) */
		if(zone->zone_handler.ev_fd != -1)
			xfrd_udp_release(zone);
		if(!xfrd_tcp_open(set, tp, zone)) {
			zone->tcp_conn = -1;
			xfrd_set_refresh_now(zone);
			/* try to start the next zone (if any) */
			continue;
		}
		/* re-init this tcppipe */
		/* ip and ip_len set by tcp_open */
		tp->node.key = tp;
		tp->num_unused = ID_PIPE_NUM;
		tp->num_skip = 0;
		tp->tcp_send_first = NULL;
		tp->tcp_send_last = NULL;
		memset(tp->id, 0, sizeof(tp->id));
		for(i=0; i<ID_PIPE_NUM; i++) {
			tp->unused[i] = i;
		}

		/* insert into tree */
		(void)rbtree_insert(set->pipetree, &tp->node);
		/* setup write */
		xfrd_unset_timer(zone);
		pipeline_setup_new_zone(set, tp, zone);
		/* started a task, no need for cleanups, so return */
		return;
	}
	/* no task to start, cleanup */
	assert(!set->tcp_waiting_first);
	set->tcp_count --;
	assert(set->tcp_count >= 0);

}

