/*
 * Dynomite - A thin, distributed replication layer for multi non-distributed storages.
 * Copyright (C) 2014 Netflix, Inc.
 */ 

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "dyn_core.h"
#include "dyn_conf.h"
#include "dyn_server.h"
#include "dyn_dnode_peer.h"
#include "dyn_node_snitch.h"
#include "dyn_token.h"

static rstatus_t dnode_peer_pool_update(struct server_pool *pool);

bool is_same_dc(struct server_pool *sp, struct server *peer_node)
{
    return string_compare(&sp->dc, &peer_node->dc) == 0;
}

static void
dnode_peer_ref(struct conn *conn, void *owner)
{
    struct server *peer = owner;

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);
    ASSERT(conn->owner == NULL);

    conn->family = peer->family;
    conn->addrlen = peer->addrlen;
    conn->addr = peer->addr;

    peer->ns_conn_q++;
    TAILQ_INSERT_TAIL(&peer->s_conn_q, conn, conn_tqe);

    conn->owner = owner;

    if (log_loggable(LOG_VVERB)) {
       log_debug(LOG_VVERB, "dyn: ref peer conn %p owner %p into '%.*s", conn, peer,
                   peer->pname.len, peer->pname.data);
    }
}

static void
dnode_peer_unref(struct conn *conn)
{
    struct server *peer;

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);
    ASSERT(conn->owner != NULL);

    peer = conn->owner;
    conn->owner = NULL;

    ASSERT(peer->ns_conn_q != 0);
    peer->ns_conn_q--;
    TAILQ_REMOVE(&peer->s_conn_q, conn, conn_tqe);

    if (log_loggable(LOG_VVERB)) {
       log_debug(LOG_VVERB, "dyn: unref peer conn %p owner %p from '%.*s'", conn, peer,
               peer->pname.len, peer->pname.data);
    }
}

int
dnode_peer_timeout(struct msg *msg, struct conn *conn)
{
    struct server *server;
    struct server_pool *pool;

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    server = conn->owner;
    pool = server->owner;
   int additional_timeout = 0;

   if (conn->same_dc)
       additional_timeout = 200;
   else
       additional_timeout = 5000;

   if (!msg->is_read) //make sure write request has a longer timeout so we almost never want to drop it
       additional_timeout += 20000;

    return pool->timeout + additional_timeout;
}

static bool
dnode_peer_active(struct conn *conn)
{
    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    if (!TAILQ_EMPTY(&conn->imsg_q)) {
        log_debug(LOG_VVERB, "dyn: s %d is active", conn->sd);
        return true;
    }

    if (!TAILQ_EMPTY(&conn->omsg_q)) {
        log_debug(LOG_VVERB, "dyn: s %d is active", conn->sd);
        return true;
    }

    if (conn->rmsg != NULL) {
        log_debug(LOG_VVERB, "dyn: s %d is active", conn->sd);
        return true;
    }

    if (conn->smsg != NULL) {
        log_debug(LOG_VVERB, "dyn: s %d is active", conn->sd);
        return true;
    }

    log_debug(LOG_VVERB, "dyn: s %d is inactive", conn->sd);

    return false;
}

static rstatus_t
dnode_peer_each_set_owner(void *elem, void *data)
{
    struct server *s = elem;
    struct server_pool *sp = data;

    s->owner = sp;

    return DN_OK;
}

static rstatus_t
dnode_peer_add_local(struct server_pool *pool, struct server *peer)
{
    ASSERT(peer != NULL);
    peer->idx = 0; /* this might be psychotic, trying it for now */

    peer->pname = pool->d_addrstr;

    uint8_t *p = pool->d_addrstr.data + pool->d_addrstr.len - 1;
    uint8_t *start = pool->d_addrstr.data;
    string_copy(&peer->name, start, dn_strrchr(p, start, ':') - start);

    //peer->name = pool->d_addrstr;
    peer->port = pool->d_port;

    peer->weight = 0;  /* hacking this out of the way for now */
    peer->rack = pool->rack;
    peer->is_local = true;
    //TODO-jeb might need to copy over tokens, not sure if this is good enough
    peer->tokens = pool->tokens;

    peer->family = pool->d_family;
    peer->addrlen = pool->d_addrlen;
    peer->addr = pool->d_addr;

    peer->ns_conn_q = 0;
    TAILQ_INIT(&peer->s_conn_q);

    peer->next_retry_us = 0LL;
    peer->reconnect_backoff_sec = 1LL;
    peer->failure_count = 0;
    peer->is_seed = 1;
    peer->processed = 0;
    string_copy(&peer->dc, pool->dc.data, pool->dc.len);
    peer->owner = pool;

    log_debug(LOG_VERB, "dyn: transform to local node to peer %"PRIu32" '%.*s'",
            peer->idx, pool->name.len, pool->name.data);

    return DN_OK;
}

void
dnode_peer_deinit(struct array *nodes)
{
    uint32_t i, nnode;

    for (i = 0, nnode = array_n(nodes); i < nnode; i++) {
        struct server *s;

        s = array_pop(nodes);
        IGNORE_RET_VAL(s);
        ASSERT(TAILQ_EMPTY(&s->s_conn_q) && s->ns_conn_q == 0);
    }
    array_deinit(nodes);
}

static rstatus_t
dnode_peer_pool_run(struct server_pool *pool)
{
    ASSERT(array_n(&pool->peers) != 0);
    return vnode_update(pool);
}


static rstatus_t
dnode_peer_each_pool_init(void *elem, void *context)
{
    struct server_pool *sp = (struct server_pool *) elem;
    //struct context *ctx = context;
    struct array *conf_seeds = &sp->conf_pool->dyn_seeds;

    struct array * seeds = &sp->seeds;
    struct array * peers = &sp->peers;
    rstatus_t status;
    uint32_t nseed;

    /* init seeds list */
    nseed = array_n(conf_seeds);
    if(nseed == 0) {
        log_debug(LOG_INFO, "dyn: look like you are running with no seeds deifined. This is ok for running with just one node.");

        // add current node to peers array
        status = array_init(peers, CONF_DEFAULT_PEERS, sizeof(struct server));
        if (status != DN_OK) {
            return status;
        }

        struct server *peer = array_push(peers);
        ASSERT(peer != NULL);
        status = dnode_peer_add_local(sp, peer);
        if (status != DN_OK) {
            dnode_peer_deinit(peers);
        }
        dnode_peer_pool_run(sp);
        return status;
    }

    ASSERT(array_n(seeds) == 0);

    status = array_init(seeds, nseed, sizeof(struct server));
    if (status != DN_OK) {
        return status;
    }

    /* transform conf seeds to seeds */
    status = array_each(conf_seeds, conf_seed_each_transform, seeds);
    if (status != DN_OK) {
        dnode_peer_deinit(seeds);
        return status;
    }
    ASSERT(array_n(seeds) == nseed);

    /* set seed owner */
    status = array_each(seeds, dnode_peer_each_set_owner, sp);
    if (status != DN_OK) {
        dnode_peer_deinit(seeds);
        return status;
    }


    /* initialize peers list = seeds list */
    ASSERT(array_n(peers) == 0);

    // add current node to peers array
    uint32_t peer_cnt = nseed + 1;
    status = array_init(peers, CONF_DEFAULT_PEERS, sizeof(struct server));
    if (status != DN_OK) {
        return status;
    }

    struct server *peer = array_push(peers);
    ASSERT(peer != NULL);
    status = dnode_peer_add_local(sp, peer);
    if (status != DN_OK) {
        dnode_peer_deinit(seeds);
        dnode_peer_deinit(peers);
        return status;
    }

    status = array_each(conf_seeds, conf_seed_each_transform, peers);
    if (status != DN_OK) {
        dnode_peer_deinit(seeds);
        dnode_peer_deinit(peers);
        return status;
    }
    IGNORE_RET_VAL(peer_cnt);
    ASSERT(array_n(peers) == peer_cnt);

    status = array_each(peers, dnode_peer_each_set_owner, sp);
    if (status != DN_OK) {
        dnode_peer_deinit(seeds);
        dnode_peer_deinit(peers);
        return status;
    }

    dnode_peer_pool_run(sp);

    log_debug(LOG_DEBUG, "init %"PRIu32" seeds and peers in pool %"PRIu32" '%.*s'",
            nseed, sp->idx, sp->name.len, sp->name.data);

    return DN_OK;
}

rstatus_t
dnode_peer_init(struct array *server_pool, struct context *ctx)
{
    rstatus_t status;

    status = array_each(server_pool, dnode_peer_each_pool_init, ctx);
    if (status != DN_OK) {
        server_pool_deinit(server_pool);
        return status;
    }

    return DN_OK;
}


static bool
is_conn_secured(struct server_pool *sp, struct server *peer_node)
{
    //ASSERT(peer_server != NULL);
    //ASSERT(sp != NULL);

    // if dc-secured mode then communication only between nodes in different dc is secured
    if (dn_strcmp(sp->secure_server_option.data, CONF_STR_DC) == 0) {
        if (string_compare(&sp->dc, &peer_node->dc) != 0) {
            return true;
        }
    }
    // if rack-secured mode then communication only between nodes in different rack is secured.
    // communication secured between nodes if they are in rack with same name across dcs.
    else if (dn_strcmp(sp->secure_server_option.data, CONF_STR_RACK) == 0) {
        // if not same rack nor dc
        if (string_compare(&sp->rack, &peer_node->rack) != 0
                || string_compare(&sp->dc, &peer_node->dc) != 0) {
            return true;
        }
    }
    // if all then all communication between nodes will be secured.
    else if (dn_strcmp(sp->secure_server_option.data, CONF_STR_ALL) == 0) {
        return true;
    }

    return false;
}

static struct conn *
dnode_peer_conn(struct server *server)
{
    struct server_pool *pool;
    struct conn *conn;

    pool = server->owner;

    if (server->ns_conn_q < 1) {
        conn = conn_get_peer(server, false, pool->data_store);
        if (is_conn_secured(pool, server)) {
            conn->dnode_secured = 1;
            conn->dnode_crypto_state = 0; //need to do a encryption handshake
        }

        conn->same_dc = is_same_dc(pool, server)? 1 : 0;

        return conn;
    }

    /*
     * Pick a server connection from the head of the queue and insert
     * it back into the tail of queue to maintain the lru order
     */
    conn = TAILQ_FIRST(&server->s_conn_q);
    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    TAILQ_REMOVE(&server->s_conn_q, conn, conn_tqe);
    TAILQ_INSERT_TAIL(&server->s_conn_q, conn, conn_tqe);

    return conn;
}

static rstatus_t
dnode_peer_connect(struct context *ctx, struct server *server, struct conn *conn)
{
    rstatus_t status;

    if (log_loggable(LOG_VERB)) {
        log_debug(LOG_VERB, "dnode_peer_connect dyn: connect to peer '%.*s'", server->pname.len,
                server->pname.data);
    }

    if (ctx->admin_opt > 0)
        return DN_OK;

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    if (conn->sd > 0) {
        /* already connected on peer connection */
        return DN_OK;
    }

    conn->sd = socket(conn->family, SOCK_STREAM, 0);
    if (conn->sd < 0) {
        log_error("dyn: socket for peer '%.*s' failed: %s", server->pname.len,
                server->pname.data, strerror(errno));
        status = DN_ERROR;
        goto error;
    }
    log_debug(LOG_WARN, "dnode: connecting to peer '%.*s' on p %d", server->pname.len,
            server->pname.data, conn->sd);


    status = dn_set_nonblocking(conn->sd);
    if (status != DN_OK) {
        log_error("dyn: set nonblock on s %d for peer '%.*s' failed: %s",
                conn->sd,  server->pname.len, server->pname.data,
                strerror(errno));
        goto error;
    }


    if (server->pname.data[0] != '/') {
        status = dn_set_tcpnodelay(conn->sd);
        if (status != DN_OK) {
            log_warn("dyn: set tcpnodelay on s %d for peer '%.*s' failed, ignored: %s",
                    conn->sd, server->pname.len, server->pname.data,
                    strerror(errno));
        }
    }

    status = event_add_conn(ctx->evb, conn);
    if (status != DN_OK) {
        log_error("dyn: event add conn s %d for peer '%.*s' failed: %s",
                conn->sd, server->pname.len, server->pname.data,
                strerror(errno));
        goto error;
    }

    ASSERT(!conn->connecting && !conn->connected);

    status = connect(conn->sd, conn->addr, conn->addrlen);

    if (status != DN_OK) {
        if (errno == EINPROGRESS) {
            conn->connecting = 1;
            log_debug(LOG_DEBUG, "dyn: connecting on s %d to peer '%.*s'",
                    conn->sd, server->pname.len, server->pname.data);
            return DN_OK;
        }

        log_error("dyn: connect on s %d to peer '%.*s' failed: %s", conn->sd,
                server->pname.len, server->pname.data, strerror(errno));

        goto error;
    }


    ASSERT(!conn->connecting);
    conn->connected = 1;
    log_debug(LOG_WARN, "dyn: connected on s %d to peer '%.*s'", conn->sd,
            server->pname.len, server->pname.data);


    return DN_OK;

    error:
    conn->err = errno;
    return status;
}

static void
dnode_peer_ack_err(struct context *ctx, struct conn *conn, struct msg *req)
{
    if ((req->swallow && req->noreply) || // no reply
        (req->swallow && (req->consistency == DC_ONE)) || // dc one
        (req->swallow && ((req->consistency == DC_QUORUM) || (req->consistency == DC_SAFE_QUORUM)) // remote dc request
                      && (!conn->same_dc)) ||
        (req->owner == conn)) // a gossip message that originated on this conn
    {
        log_info("close %s %d swallow req %u:%u len %"PRIu32
                 " type %d", conn_get_type_string(conn), conn->sd, req->id,
                 req->parent_id, req->mlen, req->type);
        req_put(req);
        return;
    }
    struct conn *c_conn = req->owner;
    // At other connections, these responses would be swallowed.
    ASSERT_LOG((c_conn->type == CONN_CLIENT) ||
               (c_conn->type == CONN_DNODE_PEER_CLIENT),
               "conn:%s c_conn:%s, req %d:%d", conn_get_type_string(conn),
               conn_get_type_string(c_conn), req->id, req->parent_id);

    // Create an appropriate response for the request so its propagated up;
    // This response gets dropped in rsp_make_error anyways. But since this is
    // an error path its ok with the overhead.
    struct msg *rsp = msg_get(conn, false, conn->data_store, __FUNCTION__);
    req->done = 1;
    rsp->error = req->error = 1;
    rsp->err = req->err = conn->err;
    rsp->dyn_error = req->dyn_error = PEER_CONNECTION_REFUSE;
    rsp->dmsg = dmsg_get();
    rsp->dmsg->id =  req->id;

    log_info("close %s %d req %u:%u "
             "len %"PRIu32" type %d from c %d%c %s", conn_get_type_string(conn),
             conn->sd, req->id, req->parent_id, req->mlen, req->type,
             c_conn->sd, conn->err ? ':' : ' ',
             conn->err ? strerror(conn->err): " ");
    rstatus_t status =
            conn_handle_response(c_conn, req->parent_id ? req->parent_id : req->id,
                                 rsp);
    IGNORE_RET_VAL(status);
    if (req->swallow)
        req_put(req);
}


static void
dnode_peer_failure(struct context *ctx, struct server *server)
{
    struct server_pool *pool = server->owner;
    int64_t now;
    rstatus_t status;

    server->failure_count++;

    log_notice("dyn: peer '%.*s' failure count %"PRIu32" ",
              server->pname.len, server->pname.data, server->failure_count);

	if (server->failure_count < pool->server_failure_limit) {
		return;
	}

    now = dn_usec_now();
    if (now < 0) {
        return;
    }

	stats_pool_set_ts(ctx, pool, peer_ejected_at, now);

    log_notice("dyn: update peer pool %"PRIu32" '%.*s' for peer '%.*s' "
             "for next %"PRIu32" secs", pool->idx, pool->name.len,
             pool->name.data, server->pname.len, server->pname.data,
             WAIT_BEFORE_UPDATE_PEERS_IN_MILLIS / 1000 );

    stats_pool_incr(ctx, pool, peer_ejects);

    //server->failure_count = 0;
    server->next_retry_us = now + (server->reconnect_backoff_sec * 1000 * 1000);
    server->reconnect_backoff_sec =
        (2 * server->reconnect_backoff_sec) > MAX_WAIT_BEFORE_RECONNECT_IN_SECS ?
                                                MAX_WAIT_BEFORE_RECONNECT_IN_SECS :
                                                2 * server->reconnect_backoff_sec;
    // Mark the peer as down
    server->state = DOWN;
    status = dnode_peer_pool_update(pool);
    if (status != DN_OK) {
        log_error("dyn: updating peer pool %"PRIu32" '%.*s' failed: %s", pool->idx,
                  pool->name.len, pool->name.data, strerror(errno));
    }
}

static void
dnode_peer_close_stats(struct context *ctx, struct conn *conn)
{
    struct server *server = conn->owner;
    if (conn->connected) {
        stats_pool_decr(ctx, server->owner, peer_connections);
    }

    if (conn->eof) {
        stats_pool_incr(ctx, server->owner, peer_eof);
        return;
    }

    switch (conn->err) {
    case ETIMEDOUT:
        if (conn->same_dc)
            stats_pool_incr(ctx, server->owner, peer_timedout);
        else
            stats_pool_incr(ctx, server->owner, remote_peer_timedout);
        break;
    case EPIPE:
    case ECONNRESET:
    case ECONNABORTED:
    case ECONNREFUSED:
    case ENOTCONN:
    case ENETDOWN:
    case ENETUNREACH:
    case EHOSTDOWN:
    case EHOSTUNREACH:
    default:
        stats_pool_incr(ctx, server->owner, peer_err);
        break;
    }
}


static void
dnode_peer_close(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    struct msg *msg, *nmsg; /* current and next message */

    struct server *server = conn->owner;

    log_debug(LOG_WARN, "dyn: dnode_peer_close on peer '%.*s'", server->pname.len,
            server->pname.data);

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    dnode_peer_close_stats(ctx, conn);

    if (conn->sd < 0) {
        dnode_peer_failure(ctx, conn->owner);
        conn_unref(conn);
        conn_put(conn);
        return;
    }
    uint32_t out_counter = 0;
    for (msg = TAILQ_FIRST(&conn->omsg_q); msg != NULL; msg = nmsg) {
        nmsg = TAILQ_NEXT(msg, s_tqe);

        /* dequeue the message (request) from server outq */
        conn_dequeue_outq(ctx, conn, msg);
        dnode_peer_ack_err(ctx, conn, msg);
        out_counter++;
    }

    ASSERT(TAILQ_EMPTY(&conn->omsg_q));

    uint32_t in_counter = 0;
    for (msg = TAILQ_FIRST(&conn->imsg_q); msg != NULL; msg = nmsg) {
        nmsg = TAILQ_NEXT(msg, s_tqe);

        /* dequeue the message (request) from server inq */
        conn_dequeue_inq(ctx, conn, msg);
        // We should also remove the msg from the timeout rbtree.
        // for outq, its already taken care of
        msg_tmo_delete(msg);
        dnode_peer_ack_err(ctx, conn, msg);
        in_counter++;

        stats_pool_incr(ctx, server->owner, peer_dropped_requests);
    }

    ASSERT(TAILQ_EMPTY(&conn->imsg_q));

    log_warn("close %s %d Dropped %u outqueue & %u inqueue requests",
             conn_get_type_string(conn), conn->sd, out_counter, in_counter);
    msg = conn->rmsg;
    if (msg != NULL) {
        conn->rmsg = NULL;

        ASSERT(!msg->request);
        ASSERT(msg->peer == NULL);

        rsp_put(msg);

        log_debug(LOG_INFO, "dyn: close s %d discarding rsp %"PRIu64" len %"PRIu32" "
                "in error", conn->sd, msg->id, msg->mlen);
    }

    ASSERT(conn->smsg == NULL);

    dnode_peer_failure(ctx, conn->owner);

    conn_unref(conn);

    status = close(conn->sd);
    if (status < 0) {
        log_error("dyn: close s %d failed, ignored: %s", conn->sd, strerror(errno));
    }
    conn->sd = -1;

    conn_put(conn);
}



static rstatus_t
dnode_peer_each_preconnect(void *elem, void *data)
{
    rstatus_t status;
    struct server *peer;
    struct server_pool *sp;
    struct conn *conn;

    peer = elem;
    sp = peer->owner;

    if (peer->is_local)  //don't bother to connect if it is a self-connection
        return DN_OK;

    conn = dnode_peer_conn(peer);
    if (conn == NULL) {
        return DN_ENOMEM;
    }

    status = dnode_peer_connect(sp->ctx, peer, conn);
    if (status != DN_OK) {
        log_warn("dyn: connect to peer '%.*s' failed, ignored: %s",
                peer->pname.len, peer->pname.data, strerror(errno));
        dnode_peer_close(sp->ctx, conn);
    }

    return DN_OK;
}

static rstatus_t
dnode_peer_each_disconnect(void *elem, void *data)
{
    struct server *server;
    struct server_pool *pool;

    server = elem;
    pool = server->owner;

    //TODOs: fixe me not to use s_conn_q to distinguish server pool and peer pool
    while (!TAILQ_EMPTY(&server->s_conn_q)) {
        struct conn *conn;

        ASSERT(server->ns_conn_q > 0);

        conn = TAILQ_FIRST(&server->s_conn_q);
        conn_close(pool->ctx, conn);
    }

    return DN_OK;
}

static void
dnode_peer_close_socket(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    if (log_loggable(LOG_VERB)) {
        log_debug(LOG_VERB, "In dnode_peer_close_socket");
    }

    if (conn != NULL) {
        status = close(conn->sd);
        if (status < 0) {
            log_error("dyn: close s %d failed, ignored: %s", conn->sd, strerror(errno));
        }
    }

    conn->dnode_crypto_state = 0;
    conn->sd = -1;
}


rstatus_t
dnode_peer_forward_state(void *rmsg)
{
    rstatus_t status;
    struct ring_msg *msg = rmsg;
    struct server_pool *sp = msg->sp;

    log_debug(LOG_VVERB, "dnode_peer_forward_state: forwarding");

    //we assume one mbuf is enough for now - will enhance with multiple mbufs later
    struct mbuf *mbuf = mbuf_get();
    if (mbuf == NULL) {
        log_debug(LOG_VVERB, "Too bad, not enough memory!");
        return DN_ENOMEM;
    }

    mbuf_copy(mbuf, msg->data, msg->len);

    struct array *peers = &sp->peers;
    uint32_t nelem;
    nelem = array_n(peers);

    //pick a random peer
    int ran_index = rand() % nelem;

    if (ran_index == 0)
       ran_index += 1;

    struct server *peer = (struct server *) array_get(peers, ran_index);

    //log_debug(LOG_VVERB, "Gossiping to node  '%.*s'", peer->name.len, peer->name.data);

    struct conn * conn = dnode_peer_conn(peer);
    if (conn == NULL) {
        //running out of connection due to memory exhaust
        log_debug(LOG_ERR, "Unable to obtain a connection object");
        return DN_ERROR;
    }

    status = dnode_peer_connect(sp->ctx, peer, conn);
    if (status != DN_OK ) {
        dnode_peer_close(sp->ctx, conn);
        log_debug(LOG_ERR, "Error happened in connecting on conn %d", conn->sd);
        return DN_ERROR;
    }

    dnode_peer_gossip_forward(sp->ctx, conn, sp->data_store, mbuf);

    //free this as nobody else will do
    //mbuf_put(mbuf);

    return status;
}


rstatus_t
dnode_peer_handshake_announcing(void *rmsg)
{
    rstatus_t status;
    struct ring_msg *msg = rmsg;
    struct server_pool *sp = msg->sp;
    log_debug(LOG_VVERB, "dyn: handshaking peers");
    struct array *peers = &sp->peers;

    uint32_t i,nelem;
    nelem = array_n(peers);

    //we assume one mbuf is enough for now - will enhance with multiple mbufs later
    struct mbuf *mbuf = mbuf_get();
    if (mbuf == NULL) {
        log_debug(LOG_VVERB, "Too bad, not enough memory!");
        return DN_ENOMEM;
    }

    //annoucing myself by sending msg: 'dc$rack$token,started_ts,node_state,node_dns'
    mbuf_write_string(mbuf, &sp->dc);
    mbuf_write_char(mbuf, '$');
    mbuf_write_string(mbuf, &sp->rack);
    mbuf_write_char(mbuf, '$');
    struct dyn_token *token = (struct dyn_token *) array_get(&sp->tokens, 0);
    if (token == NULL) {
        log_debug(LOG_VVERB, "Why? This should not be null!");
        mbuf_put(mbuf);
        return DN_ERROR;
    }

    mbuf_write_uint32(mbuf, token->mag[0]);
    mbuf_write_char(mbuf, ',');
    int64_t cur_ts = (int64_t)time(NULL);
    mbuf_write_uint64(mbuf, cur_ts);
    mbuf_write_char(mbuf, ',');
    mbuf_write_uint8(mbuf, sp->ctx->dyn_state);
    mbuf_write_char(mbuf, ',');

    char *broadcast_addr = get_broadcast_address(sp);
    mbuf_write_bytes(mbuf, broadcast_addr, dn_strlen(broadcast_addr));

    //for each peer, send a registered msg
    for (i = 0; i < nelem; i++) {
        struct server *peer = (struct server *) array_get(peers, i);
        if (peer->is_local)
            continue;

        log_debug(LOG_VVERB, "Gossiping to node  '%.*s'", peer->name.len, peer->name.data);

        struct conn * conn = dnode_peer_conn(peer);
        if (conn == NULL) {
            //running out of connection due to memory exhaust
            log_debug(LOG_DEBUG, "Unable to obtain a connection object");
            return DN_ERROR;
        }


        status = dnode_peer_connect(sp->ctx, peer, conn);
        if (status != DN_OK ) {
            dnode_peer_close(sp->ctx, conn);
            log_debug(LOG_DEBUG, "Error happened in connecting on conn %d", conn->sd);
            return DN_ERROR;
        }

        //conn->

        dnode_peer_gossip_forward(sp->ctx, conn, sp->data_store, mbuf);
        //peer_gossip_forward1(sp->ctx, conn, sp->data_store, &data);
    }

    //free this as nobody else will do
    //mbuf_put(mbuf);

    return DN_OK;
}


static void
dnode_peer_relink_conn_owner(struct server_pool *sp)
{
    struct array *peers = &sp->peers;

    uint32_t i,nelem;
    nelem = array_n(peers);
    for (i = 0; i < nelem; i++) {
        struct server *peer = (struct server *) array_get(peers, i);
        struct conn *conn, *nconn;
        for (conn = TAILQ_FIRST(&peer->s_conn_q); conn != NULL;
                conn = nconn) {
            nconn = TAILQ_NEXT(conn, conn_tqe);
            conn->owner = peer; //re-link to the owner in case of an resize/allocation
        }
    }

}


static rstatus_t
dnode_peer_add_node(struct server_pool *sp, struct node *node)
{
    rstatus_t status;
    struct array *peers = &sp->peers;
    struct server *s = array_push(peers);

    s->owner = sp;
    s->idx = array_idx(peers, s);

    //log_debug(LOG_VERB, "node rack_name         : '%.*s'", node->rack.len, node->rack.data);
    //log_debug(LOG_VERB, "node dc_name        : '%.*s'", node->dc.len, node->dc.data);
    //log_debug(LOG_VERB, "node address          : '%.*s'", node->pname.len, node->pname.data);
    //log_debug(LOG_VERB, "node ip         : '%.*s'", node->name.len, node->name.data);


    string_copy(&s->pname, node->pname.data, node->pname.len);
    string_copy(&s->name, node->name.data, node->name.len);
    string_copy(&s->rack, node->rack.data, node->rack.len);
    string_copy(&s->dc, node->dc.data, node->dc.len);

    s->port = (uint16_t) node->port;
    s->is_local = node->is_local;
    s->state = node->state;
    s->processed = 0;

    array_init(&s->tokens, 1, sizeof(struct dyn_token));
    struct dyn_token *src_token = &node->token;
    struct dyn_token *dst_token = array_push(&s->tokens);
    copy_dyn_token(src_token, dst_token);

    struct sockinfo  *info =  dn_alloc(sizeof(*info)); //need to free this
    dn_resolve(&s->name, s->port, info);
    s->family = info->family;
    s->addrlen = info->addrlen;
    s->addr = (struct sockaddr *)&info->addr;  //TODOs: fix this by copying, not reference
    s->ns_conn_q = 0;
    TAILQ_INIT(&s->s_conn_q);

    s->next_retry_us = 0LL;
    s->reconnect_backoff_sec = 1LL;
    s->failure_count = 0;
    s->is_seed = node->is_seed;

    log_debug(LOG_VERB, "add a node to peer %"PRIu32" '%.*s'",
            s->idx, s->pname.len, s->pname.data);

    dnode_peer_relink_conn_owner(sp);

    status = dnode_peer_pool_run(sp);
    if (status != DN_OK)
        return status;

    status = dnode_peer_each_preconnect(s, NULL);

    return status;
}

rstatus_t
dnode_peer_add(void *rmsg)
{
    rstatus_t status;
    struct ring_msg *msg = rmsg;
    struct server_pool *sp = msg->sp;
    struct node *node = array_get(&msg->nodes, 0);
    log_debug(LOG_VVERB, "dyn: peer has an added message '%.*s'", node->name.len, node->name.data);
    status = dnode_peer_add_node(sp, node);

    return status;
}

/*
rstatus_t
dnode_peer_add(struct server_pool *sp, struct node *node)
{
    rstatus_t status;

    log_debug(LOG_VVERB, "dyn: peer has an added message '%.*s'", node->name.len, node->name.data);
    status = dnode_peer_add_node(sp, node);

    return status;
}
*/

rstatus_t
dnode_peer_replace(void *rmsg)
{
    //rstatus_t status;
    struct ring_msg *msg = rmsg;
    struct server_pool *sp = msg->sp;
    struct node *node = array_get(&msg->nodes, 0);
    log_debug(LOG_VVERB, "dyn: peer has a replaced message '%.*s'", node->name.len, node->name.data);
    struct array *peers = &sp->peers;
    struct server *s = NULL;

    uint32_t i,nelem;
    //bool node_exist = false;
    //TODOs: use hash table here
    for (i=1, nelem = array_n(peers); i< nelem; i++) {
        struct server * peer = (struct server *) array_get(peers, i);
        if (string_compare(&peer->rack, &node->rack) == 0) {
            //TODOs: now only compare 1st token and support vnode later - use hash string on a tokens for comparison
            struct dyn_token *ptoken = (struct dyn_token *) array_get(&peer->tokens, 0);
            struct dyn_token *ntoken = &node->token;

            if (cmp_dyn_token(ptoken, ntoken) == 0) {
                s = peer; //found a node to replace
            }
        }
    }


    if (s != NULL) {
        log_debug(LOG_INFO, "Found an old node to replace '%.*s'", s->name.len, s->name.data);
        log_debug(LOG_INFO, "Replace with address '%.*s'", node->name.len, node->name.data);

        string_deinit(&s->pname);
        string_deinit(&s->name);
        string_copy(&s->pname, node->pname.data, node->pname.len);
        string_copy(&s->name, node->name.data, node->name.len);

        //TODOs: need to free the previous s->addr?
        //if (s->addr != NULL) {
        //   dn_free(s->addr);
        //}

        struct sockinfo  *info =  dn_alloc(sizeof(*info)); //need to free this
        dn_resolve(&s->name, s->port, info);
        s->family = info->family;
        s->addrlen = info->addrlen;
        s->addr = (struct sockaddr *)&info->addr;  //TODOs: fix this by copying, not reference


        dnode_peer_each_disconnect(s, NULL);
        dnode_peer_each_preconnect(s, NULL);
    } else {
        log_debug(LOG_INFO, "Unable to find any node matched the token");
    }

    return DN_OK;
}


/*
rstatus_t
dnode_peer_replace(struct server_pool *sp, struct node *node)
{
    //rstatus_t status;
    log_debug(LOG_VVERB, "dyn: peer has a replaced message '%.*s'", node->name.len, node->name.data);
    struct array *peers = &sp->peers;
    struct server *s = NULL;

    uint32_t i,nelem;
    //bool node_exist = false;
    //TODOs: use hash table here
    for (i=1, nelem = array_n(peers); i< nelem; i++) {
        struct server * peer = (struct server *) array_get(peers, i);
        if (string_compare(&peer->rack, &node->rack) == 0) {
            //TODOs: now only compare 1st token and support vnode later - use hash string on a tokens for comparison
            struct dyn_token *ptoken = (struct dyn_token *) array_get(&peer->tokens, 0);
            struct dyn_token *ntoken = &node->token;

            if (cmp_dyn_token(ptoken, ntoken) == 0) {
                s = peer; //found a node to replace
            }
        }
    }


    if (s != NULL) {
        log_debug(LOG_INFO, "Found an old node to replace '%.*s'", s->name.len, s->name.data);
        log_debug(LOG_INFO, "Replace with address '%.*s'", node->name.len, node->name.data);

        string_deinit(&s->pname);
        string_deinit(&s->name);
        string_copy(&s->pname, node->pname.data, node->pname.len);
        string_copy(&s->name, node->name.data, node->name.len);

        //TODOs: need to free the previous s->addr?
        //if (s->addr != NULL) {
        //   dn_free(s->addr);
        //}

        struct sockinfo  *info =  dn_alloc(sizeof(*info)); //need to free this
        dn_resolve(&s->name, s->port, info);
        s->family = info->family;
        s->addrlen = info->addrlen;
        s->addr = (struct sockaddr *)&info->addr;  //TODOs: fix this by copying, not reference


        dnode_peer_each_disconnect(s, NULL);
        dnode_peer_each_preconnect(s, NULL);
    } else {
        log_debug(LOG_INFO, "Unable to find any node matched the token");
    }

    return DN_OK;
}
*/


void
dnode_peer_connected(struct context *ctx, struct conn *conn)
{
    struct server *server = conn->owner;

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);
    ASSERT(conn->connecting && !conn->connected);

    stats_pool_incr(ctx, server->owner, peer_connections);

    conn->connecting = 0;
    conn->connected = 1;

        if (log_loggable(LOG_INFO)) {
       log_debug(LOG_INFO, "dyn: peer connected on sd %d to server '%.*s'", conn->sd,
              server->pname.len, server->pname.data);
        }
}

static void
dnode_peer_ok(struct context *ctx, struct conn *conn)
{
    struct server *server = conn->owner;

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);
    ASSERT(conn->connected);

    log_debug(LOG_VERB, "dyn: reset peer '%.*s' failure count from %"PRIu32
            " to 0", server->pname.len, server->pname.data,
            server->failure_count);
    server->failure_count = 0;
    server->next_retry_us = 0LL;
    server->reconnect_backoff_sec = 1LL;
}

static rstatus_t
dnode_peer_pool_update(struct server_pool *pool)
{
    int64_t now;

    now = dn_msec_now();
    if (now < 0) {
        return DN_ERROR;
    }

    if (now <= pool->next_rebuild) {
        return DN_OK;
    }

    pool->next_rebuild = now + WAIT_BEFORE_UPDATE_PEERS_IN_MILLIS;
    return dnode_peer_pool_run(pool);

}

static struct dyn_token *
dnode_peer_pool_hash(struct server_pool *pool, uint8_t *key, uint32_t keylen)
{
    ASSERT(array_n(&pool->peers) != 0);
    ASSERT(key != NULL && keylen != 0);

    struct dyn_token *token = dn_alloc(sizeof(struct dyn_token));
    if (token == NULL) {
        return NULL;
    }
    init_dyn_token(token);

    rstatus_t status = pool->key_hash((char *)key, keylen, token);
    if (status != DN_OK) {
        dn_free(token);
        return NULL;
    }

    return token;
}

static struct server *
dnode_peer_pool_reroute_server(struct server_pool *pool, struct rack *rack, uint8_t *key, uint32_t keylen)
{
    uint32_t pos = 0;
    struct server *server = NULL;
    struct continuum *entry;

    if (rack->ncontinuum > 1) {
        do {
            pos = pos % rack->ncontinuum;
            entry = rack->continuum + pos;
            server = array_get(&pool->peers, entry->index);
            pos++;
        } while (server->state == DOWN && pos < rack->ncontinuum);
    }

    //TODOs: pick another server in another rack of the same DC if we don't have any good server
    return server;
}

static struct server *
dnode_peer_for_key_on_rack(struct server_pool *pool, struct rack *rack,
                           uint8_t *key, uint32_t keylen)
{
    struct server *server;
    uint32_t idx;
    struct dyn_token *token = NULL;

    ASSERT(array_n(&pool->peers) != 0);

    if (keylen == 0) {
        idx = 0; //for no argument command
    } else {
        token = dnode_peer_pool_hash(pool, key, keylen);
        //print_dyn_token(token, 1);
        idx = vnode_dispatch(rack->continuum, rack->ncontinuum, token);
        //loga("found idx %d for rack '%.*s' ", idx, rack->name->len, rack->name->data);

        //TODOs: should reuse the token
        if (token != NULL) {
            deinit_dyn_token(token);
            dn_free(token);
        }
    }

    ASSERT(idx < array_n(&pool->peers));

    server = array_get(&pool->peers, idx);

    if (server->state == DOWN) {
        if (!is_same_dc(pool, server)) {
            //pick another reroute server in the server DC
            server = dnode_peer_pool_reroute_server(pool, rack, key, keylen);
        }
    }

    if (log_loggable(LOG_VERB)) {
        log_debug(LOG_VERB, "dyn: key '%.*s' on dist %d maps to server '%.*s'", keylen,
                key, pool->dist_type, server->pname.len, server->pname.data);
    }

    return server;
}

struct server*
dnode_peer_pool_server(struct context *ctx, struct server_pool *pool,
                     struct rack *rack, uint8_t *key, uint32_t keylen,
                     uint8_t msg_type)
{
    rstatus_t status;
    struct server *server;

    log_debug(LOG_VERB, "Entering dnode_peer_pool_conn ................................");

    status = dnode_peer_pool_update(pool);
    if (status != DN_OK) {
        loga("status is not OK");
        return NULL;
    }

    if (msg_type == 1) {  //always local
        server = array_get(&pool->peers, 0);
    } else {
        /* from a given {key, keylen} pick a server from pool */
        server = dnode_peer_for_key_on_rack(pool, rack, key, keylen);
        if (server == NULL) {
            log_debug(LOG_VERB, "What? There is no such server in rack '%.*s' for key '%.*s'",
                    rack->name, keylen, key);
            return NULL;
        }
    }
    return server;
}

struct conn *
dnode_peer_pool_server_conn(struct context *ctx, struct server *server)
{
    // This peer server is marked as down. lets check if it is time to connect
    if (server->state == DOWN) {
        int64_t now = dn_usec_now();
        static int64_t next_log = 0; // Log every 1 sec
        if (server->next_retry_us && (now > server->next_retry_us)) {
            server->state = NORMAL;
            server->next_retry_us = 0;
            server->failure_count = 0;
        } else {
            if (now > next_log) {
                log_warn("Detecting peer '%.*s' is set with state Down",
                         server->pname.len, server->pname.data);
                next_log = now + 1000 * 1000;
            }
            return NULL;
        }
    }
    /* pick a connection to a given server */
    struct conn *conn = dnode_peer_conn(server);
    if (conn == NULL) {
        return NULL;
    }

    if (server->is_local) {
        return conn; //Don't bother to connect
    }

    if (server->state == RESET) {
        log_debug(LOG_WARN, "Detecting peer '%.*s' is set with state Reset", server->name);

        dnode_peer_close_socket(ctx, conn);
        if (dnode_peer_connect(ctx, server, conn) != DN_OK) {
            conn->err = EHOSTDOWN;
            dnode_peer_close(ctx, conn);
            return NULL;
        }

        server->state = NORMAL;
        log_debug(LOG_WARN, "after setting back server's state to NORMAL");
        return conn;
    }

    if (dnode_peer_connect(ctx, server, conn) != DN_OK) {
        dnode_peer_close(ctx, conn);
        return NULL;
    }

    return conn;
}


static rstatus_t
dnode_peer_pool_each_preconnect(void *elem, void *data)
{
    rstatus_t status;
    struct server_pool *sp = elem;

    if (!sp->preconnect) {
        return DN_OK;
    }

    status = array_each(&sp->peers, dnode_peer_each_preconnect, NULL);
    if (status != DN_OK) {
        return status;
    }

    return DN_OK;
}

rstatus_t
dnode_peer_pool_preconnect(struct context *ctx)
{
    rstatus_t status;

    status = array_each(&ctx->pool, dnode_peer_pool_each_preconnect, NULL);
    if (status != DN_OK) {
        return status;
    }

    return DN_OK;
}

static rstatus_t
dnode_peer_pool_each_disconnect(void *elem, void *data)
{
    rstatus_t status;
    struct server_pool *sp = elem;

    status = array_each(&sp->peers, dnode_peer_each_disconnect, NULL);
    if (status != DN_OK) {
        return status;
    }

    return DN_OK;
}

static void
dnode_peer_pool_disconnect(struct context *ctx)
{
    array_each(&ctx->pool, dnode_peer_pool_each_disconnect, NULL);
}

void
dnode_peer_pool_deinit(struct array *server_pool)
{
    uint32_t i, npool;

    for (i = 0, npool = array_n(server_pool); i < npool; i++) {
        struct server_pool *sp;

        sp = array_pop(server_pool);
        ASSERT(sp->p_conn == NULL);
        //fixe me to use different variables
        ASSERT(TAILQ_EMPTY(&sp->c_conn_q) && sp->dn_conn_q == 0);


        dnode_peer_deinit(&sp->peers);
        array_each(&sp->datacenters, datacenter_destroy, NULL);
        array_deinit(&sp->datacenters);

        log_debug(LOG_DEBUG, "dyn: deinit peer pool %"PRIu32" '%.*s'", sp->idx,
                sp->name.len, sp->name.data);
    }

    //array_deinit(server_pool);

    log_debug(LOG_DEBUG, "deinit %"PRIu32" peer pools", npool);
}


static struct msg *
dnode_rsp_recv_next(struct context *ctx, struct conn *conn, bool alloc)
{
    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    conn->last_received = time(NULL);

    return rsp_recv_next(ctx, conn, alloc);
}

static bool
dnode_rsp_filter(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg;

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    if (msg_empty(msg)) {
        ASSERT(conn->rmsg == NULL);
        log_debug(LOG_VERB, "dyn: filter empty rsp %"PRIu64" on s %d", msg->id,
                conn->sd);
        rsp_put(msg);
        return true;
    }

    pmsg = TAILQ_FIRST(&conn->omsg_q);
    if (pmsg == NULL) {
        log_debug(LOG_INFO, "dyn: filter stray rsp %"PRIu64" len %"PRIu32" on s %d noreply %d",
                msg->id, msg->mlen, conn->sd, msg->noreply);
        rsp_put(msg);
        return true;
    }
    ASSERT(pmsg->peer == NULL);
    ASSERT(pmsg->request && !pmsg->done);

    return false;
}

static void
dnode_rsp_forward_stats(struct context *ctx, struct server *server, struct msg *msg)
{
    ASSERT(!msg->request);
    stats_pool_incr(ctx, server->owner, peer_responses);
    stats_pool_incr_by(ctx, server->owner, peer_response_bytes, msg->mlen);
}

static void
dnode_rsp_swallow(struct context *ctx, struct conn *peer_conn,
                  struct msg *req, struct msg *rsp)
{
    conn_dequeue_outq(ctx, peer_conn, req);
    req->done = 1;
    log_debug(LOG_VERB, "conn %p swallow %p", peer_conn, req);
    if (rsp) {
        log_debug(LOG_INFO, "dyn: swallow rsp %"PRIu64" len %"PRIu32" of req "
                  "%"PRIu64" on s %d", rsp->id, rsp->mlen, req->id,
                  peer_conn->sd);
        rsp_put(rsp);
    }
    req_put(req);
}

/* Description: link data from a peer connection to a client-facing connection
 * peer_conn: a peer connection
 * msg      : msg with data from the peer connection after parsing
 */
static void
dnode_rsp_forward_match(struct context *ctx, struct conn *peer_conn, struct msg *rsp)
{
    rstatus_t status;
    struct msg *req;
    struct conn *c_conn;

    req = TAILQ_FIRST(&peer_conn->omsg_q);
    c_conn = req->owner;

    /* if client consistency is dc_one forward the response from only the
       local node. Since dyn_dnode_peer is always a remote node, drop the rsp */
    if (req->consistency == DC_ONE) {
        if (req->swallow) {
            dnode_rsp_swallow(ctx, peer_conn, req, rsp);
            return;
        }
        //log_warn("req %d:%d with DC_ONE consistency is not being swallowed");
    }

    /* if client consistency is dc_quorum or dc_safe_quorum, forward the response from only the
       local region/DC. */
    if (((req->consistency == DC_QUORUM) || (req->consistency == DC_SAFE_QUORUM))
        && !peer_conn->same_dc) {
        if (req->swallow) {
            dnode_rsp_swallow(ctx, peer_conn, req, rsp);
            return;
        }
    }

    log_debug(LOG_DEBUG, "DNODE RSP RECEIVED %s %d dmsg->id %u req %u:%u rsp %u:%u, ",
              conn_get_type_string(peer_conn),
              peer_conn->sd, rsp->dmsg->id,
              req->id, req->parent_id, rsp->id, rsp->parent_id);
    ASSERT(req != NULL && req->peer == NULL);
    ASSERT(req->request && !req->done);

    if (log_loggable(LOG_VVERB)) {
        loga("Dumping content for response:   ");
        msg_dump(rsp);

        loga("rsp id %d", rsp->id);

        loga("Dumping content for request:");
        msg_dump(req);

        loga("req id %d", req->id);
    }

    conn_dequeue_outq(ctx, peer_conn, req);
    req->done = 1;

    log_info("c_conn:%p %d:%d <-> %d:%d", c_conn, req->id, req->parent_id,
             rsp->id, rsp->parent_id);
    /* establish rsp <-> req (response <-> request) link */
    req->peer = rsp;
    rsp->peer = req;

    rsp->pre_coalesce(rsp);

    ASSERT_LOG((c_conn->type == CONN_CLIENT) ||
               (c_conn->type == CONN_DNODE_PEER_CLIENT),
               "c_conn type %s", conn_get_type_string(c_conn));

    dnode_rsp_forward_stats(ctx, peer_conn->owner, rsp);
    // c_conn owns respnse now
    status = conn_handle_response(c_conn, req->parent_id ? req->parent_id : req->id,
                                  rsp);
    IGNORE_RET_VAL(status);
    if (req->swallow) {
        log_info("swallow request %d:%d", req->id, req->parent_id);
        req_put(req);
    }
}

/* There are chances that the request to the remote peer or its response got dropped.
 * Hence we may not always receive a response to the request at the head of the FIFO.
 * Hence what we do is we mark that request as errored and move on the next one
 * in the outgoing queue. This works since we always have message ids in monotonically
 * increasing order.
 */
static void
dnode_rsp_forward(struct context *ctx, struct conn *peer_conn, struct msg *rsp)
{
    struct msg *req;
    struct conn *c_conn;

    ASSERT(peer_conn->type == CONN_DNODE_PEER_SERVER);

    /* response from a peer implies that peer is ok and heartbeating */
    dnode_peer_ok(ctx, peer_conn);

    /* dequeue peer message (request) from peer conn */
    while (true) {
        req = TAILQ_FIRST(&peer_conn->omsg_q);
        log_debug(LOG_VERB, "dnode_rsp_forward entering req %p rsp %p...", req, rsp);
        c_conn = req->owner;

        if (req->request_send_time) {
            struct stats *st = ctx->stats;
            uint64_t delay = dn_usec_now() - req->request_send_time;
            if (!peer_conn->same_dc)
                histo_add(&st->cross_region_latency_histo, delay);
            else
                histo_add(&st->cross_zone_latency_histo, delay);
        }

        if (req->id == rsp->dmsg->id) {
            dnode_rsp_forward_match(ctx, peer_conn, rsp);
            return;
        }
        // Report a mismatch and try to rectify
        log_error("MISMATCH: dnode %s %d rsp_dmsg_id %u req %u:%u dnode rsp %u:%u",
                  conn_get_type_string(peer_conn),
                  peer_conn->sd, rsp->dmsg->id, req->id, req->parent_id, rsp->id,
                  rsp->parent_id);
        if (c_conn && conn_to_ctx(c_conn))
            stats_pool_incr(conn_to_ctx(c_conn), c_conn->owner,
                    peer_mismatch_requests);

        // TODO : should you be worried about message id getting wrapped around to 0?
        if (rsp->dmsg->id < req->id) {
            // We received a response from the past. This indeed proves out of order
            // responses. A blunder to the architecture. Log it and drop the response.
            log_error("MISMATCH: received response from the past. Dropping it");
            rsp_put(rsp);
            return;
        }

        if (req->consistency == DC_ONE) {
            if (req->swallow) {
                // swallow the request and move on the next one
                dnode_rsp_swallow(ctx, peer_conn, req, NULL);
                continue;
            }
            log_warn("req %d:%d with DC_ONE consistency is not being swallowed");
        }

        if (((req->consistency == DC_QUORUM) || (req->consistency == DC_SAFE_QUORUM))
            && !peer_conn->same_dc) {
            if (req->swallow) {
                // swallow the request and move on the next one
                dnode_rsp_swallow(ctx, peer_conn, req, NULL);
                continue;
            }
        }

        log_error("MISMATCHED DNODE RSP RECEIVED %s %d dmsg->id %u req %u:%u rsp %u:%u, skipping....",
                  conn_get_type_string(peer_conn),
                 peer_conn->sd, rsp->dmsg->id,
                 req->id, req->parent_id, rsp->id, rsp->parent_id);
        ASSERT(req != NULL && req->peer == NULL);
        ASSERT(req->request && !req->done);

        if (log_loggable(LOG_VVERB)) {
            loga("skipping req:   ");
            msg_dump(req);
        }


        conn_dequeue_outq(ctx, peer_conn, req);
        req->done = 1;

        // Create an appropriate response for the request so its propagated up;
        struct msg *err_rsp = msg_get(peer_conn, false, peer_conn->data_store,
                                      __FUNCTION__);
        err_rsp->error = req->error = 1;
        err_rsp->err = req->err = BAD_FORMAT;
        err_rsp->dyn_error = req->dyn_error = BAD_FORMAT;
        err_rsp->dmsg = dmsg_get();
        err_rsp->dmsg->id = req->id;
        log_debug(LOG_VERB, "%p <-> %p", req, err_rsp);
        /* establish err_rsp <-> req (response <-> request) link */
        req->peer = err_rsp;
        err_rsp->peer = req;

        log_error("Peer connection s %d skipping request %u:%u, dummy err_rsp %u:%u",
                 peer_conn->sd, req->id, req->parent_id, err_rsp->id, err_rsp->parent_id);
        rstatus_t status =
            conn_handle_response(c_conn, req->parent_id ? req->parent_id : req->id,
                                err_rsp);
        IGNORE_RET_VAL(status);
        if (req->swallow) {
                log_debug(LOG_INFO, "swallow request %d:%d", req->id, req->parent_id);
            req_put(req);
        }
    }
}

static void
dnode_rsp_recv_done(struct context *ctx, struct conn *conn,
                    struct msg *msg, struct msg *nmsg)
{
    log_debug(LOG_VERB, "dnode_rsp_recv_done entering ...");

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);
    ASSERT(msg != NULL && conn->rmsg == msg);
    ASSERT(!msg->request);
    ASSERT(msg->owner == conn);
    ASSERT(nmsg == NULL || !nmsg->request);

    if (log_loggable(LOG_VVERB)) {
       loga("Dumping content for msg:   ");
       msg_dump(msg);

       if (nmsg != NULL) {
          loga("Dumping content for nmsg :");
          msg_dump(nmsg);
       }
    }

    /* enqueue next message (response), if any */
    conn->rmsg = nmsg;

    if (dnode_rsp_filter(ctx, conn, msg)) {
        return;
    }
    dnode_rsp_forward(ctx, conn, msg);
}



//TODOs: fix this in using dmsg_write with encrypted msgs
//         It is not in use now.
/*
void
dnode_rsp_gos_syn(struct context *ctx, struct conn *p_conn, struct msg *msg)
{
    rstatus_t status;
    struct msg *pmsg;

    //ASSERT(p_conn->type == CONN_DNODE_PEER_CLIENT);

    //add messsage
    struct mbuf *nbuf = mbuf_get();
    if (nbuf == NULL) {
        log_debug(LOG_ERR, "Error happened in calling mbuf_get");
        return;  //TODOs: need to address this further
    }

    msg->done = 1;

    //TODOs: need to free the old msg object
    pmsg = msg_get(p_conn, 0, msg->redis);
    if (pmsg == NULL) {
        mbuf_put(nbuf);
        return;
    }

    pmsg->done = 1;
    // establish msg <-> pmsg (response <-> request) link
    msg->peer = pmsg;
    pmsg->peer = msg;
    pmsg->pre_coalesce(pmsg);
    pmsg->owner = p_conn;

    //dyn message's meta data
    uint64_t msg_id = msg->dmsg->id;
    uint8_t type = GOSSIP_SYN_REPLY;
    struct string data = string("SYN_REPLY_OK");

    dmsg_write(nbuf, msg_id, type, p_conn, 0);
    mbuf_insert(&pmsg->mhdr, nbuf);

    //dnode_rsp_recv_done(ctx, p_conn, msg, pmsg);
    //should we do this?
    //conn_dequeue_outq(ctx, s_conn, pmsg);



     //p_conn->enqueue_outq(ctx, p_conn, pmsg);
     //if (TAILQ_FIRST(&p_conn->omsg_q) != NULL && req_done(p_conn, TAILQ_FIRST(&p_conn->omsg_q))) {
     //   status = event_add_out(ctx->evb, p_conn);
     //   if (status != DN_OK) {
     //      p_conn->err = errno;
     //   }
     //}


    if (TAILQ_FIRST(&p_conn->omsg_q) != NULL && req_done(p_conn, TAILQ_FIRST(&p_conn->omsg_q))) {
        status = event_add_out(ctx->evb, p_conn);
        if (status != DN_OK) {
            p_conn->err = errno;
        }
    }

    //dnode_rsp_forward_stats(ctx, s_conn->owner, msg);
}

*/
static struct msg *
dnode_req_send_next(struct context *ctx, struct conn *conn)
{
    rstatus_t status;

    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    uint32_t now = time(NULL);
    //throttling the sending traffics here
    if (!conn->same_dc) {
        if (conn->last_sent != 0) {
            uint32_t elapsed_time = now - conn->last_sent;
            uint32_t earned_tokens = elapsed_time * msgs_per_sec();
            conn->avail_tokens = (conn->avail_tokens + earned_tokens) < msgs_per_sec()?
                    conn->avail_tokens + earned_tokens : msgs_per_sec();

        }

        conn->last_sent = now;
        if (conn->avail_tokens > 0) {
            conn->avail_tokens--;
            return req_send_next(ctx, conn);
        }

        //requeue
        status = event_add_out(ctx->evb, conn);
        IGNORE_RET_VAL(status);

        return NULL;
    }

    conn->last_sent = now;
    return req_send_next(ctx, conn);
}

static void
dnode_req_send_done(struct context *ctx, struct conn *conn, struct msg *msg)
{
    if (log_loggable(LOG_DEBUG)) {
       log_debug(LOG_VERB, "dnode_req_send_done entering!!!");
    }
    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);
    // crashes because dmsg is NULL :(
    /*log_debug(LOG_DEBUG, "DNODE REQ SEND %s %d dmsg->id %u",
              conn_get_type_string(conn), conn->sd, msg->dmsg->id);*/

    msg->request_send_time = dn_usec_now();
    req_send_done(ctx, conn, msg);
}

static void
dnode_req_peer_enqueue_imsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);
    msg->request_inqueue_enqueue_time_us = dn_usec_now();

    if (!msg->noreply) {
        msg_tmo_insert(msg, conn);
    }
    TAILQ_INSERT_TAIL(&conn->imsg_q, msg, s_tqe);
    log_debug(LOG_VERB, "conn %p enqueue inq %d:%d", conn, msg->id, msg->parent_id);

    conn->imsg_count++;
    struct server_pool *pool = (struct server_pool *) array_get(&ctx->pool, 0);
    if (conn->same_dc) {
        histo_add(&ctx->stats->peer_in_queue, conn->imsg_count);
        stats_pool_incr(ctx, pool, peer_in_queue);
        stats_pool_incr_by(ctx, pool, peer_in_queue_bytes, msg->mlen);
    } else {
        histo_add(&ctx->stats->remote_peer_in_queue, conn->imsg_count);
        stats_pool_incr(ctx, pool, remote_peer_in_queue);
        stats_pool_incr_by(ctx, pool, remote_peer_in_queue_bytes, msg->mlen);
    }

}

static void
dnode_req_peer_dequeue_imsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);
    int64_t delay = 0;
    if (msg->request_inqueue_enqueue_time_us) {
        delay = dn_usec_now() - msg->request_inqueue_enqueue_time_us;
        if (conn->same_dc)
            histo_add(&ctx->stats->cross_zone_queue_wait_time_histo, delay);
        else
            histo_add(&ctx->stats->cross_region_queue_wait_time_histo, delay);
    }
    TAILQ_REMOVE(&conn->imsg_q, msg, s_tqe);
    log_debug(LOG_VERB, "conn %p dequeue inq %d:%d", conn, msg->id, msg->parent_id);

    conn->imsg_count--;
    struct server_pool *pool = (struct server_pool *) array_get(&ctx->pool, 0);
    if (conn->same_dc) {
        histo_add(&ctx->stats->peer_in_queue, conn->imsg_count);
        stats_pool_decr(ctx, pool, peer_in_queue);
        stats_pool_decr_by(ctx, pool, peer_in_queue_bytes, msg->mlen);
    } else {
        histo_add(&ctx->stats->remote_peer_in_queue, conn->imsg_count);
        stats_pool_decr(ctx, pool, remote_peer_in_queue);
        stats_pool_decr_by(ctx, pool, remote_peer_in_queue_bytes, msg->mlen);
    }
}

static void
dnode_req_peer_enqueue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    TAILQ_INSERT_TAIL(&conn->omsg_q, msg, s_tqe);
    log_debug(LOG_VERB, "conn %p enqueue outq %d:%d", conn, msg->id, msg->parent_id);

    //use only the 1st pool
    conn->omsg_count++;
    struct server_pool *pool = (struct server_pool *) array_get(&ctx->pool, 0);
    if (conn->same_dc) {
        histo_add(&ctx->stats->peer_out_queue, conn->omsg_count);
        stats_pool_incr(ctx, pool, peer_out_queue);
        stats_pool_incr_by(ctx, pool, peer_out_queue_bytes, msg->mlen);
    } else {
        histo_add(&ctx->stats->remote_peer_out_queue, conn->omsg_count);
        stats_pool_incr(ctx, pool, remote_peer_out_queue);
        stats_pool_incr_by(ctx, pool, remote_peer_out_queue_bytes, msg->mlen);
    }
}

static void
dnode_req_peer_dequeue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT(conn->type == CONN_DNODE_PEER_SERVER);

    msg_tmo_delete(msg);

    TAILQ_REMOVE(&conn->omsg_q, msg, s_tqe);
    log_debug(LOG_VVERB, "conn %p dequeue outq %p", conn, msg);

    //use the 1st pool
    conn->omsg_count--;
    struct server_pool *pool = (struct server_pool *) array_get(&ctx->pool, 0);
    if (conn->same_dc) {
        histo_add(&ctx->stats->peer_out_queue, conn->omsg_count);
        stats_pool_decr(ctx, pool, peer_out_queue);
        stats_pool_decr_by(ctx, pool, peer_out_queue_bytes, msg->mlen);
    } else {
        histo_add(&ctx->stats->remote_peer_out_queue, conn->omsg_count);
        stats_pool_decr(ctx, pool, remote_peer_out_queue);
        stats_pool_decr_by(ctx, pool, remote_peer_out_queue_bytes, msg->mlen);
    }
}


struct conn_ops dnode_peer_ops = {
    msg_recv,
    dnode_rsp_recv_next,
    dnode_rsp_recv_done,
    msg_send,
    dnode_req_send_next,
    dnode_req_send_done,
    dnode_peer_close,
    dnode_peer_active,
    dnode_peer_ref,
    dnode_peer_unref,
    dnode_req_peer_enqueue_imsgq,
    dnode_req_peer_dequeue_imsgq,
    dnode_req_peer_enqueue_omsgq,
    dnode_req_peer_dequeue_omsgq,
    conn_cant_handle_response
};

void
init_dnode_peer_conn(struct conn *conn)
{
    conn->type = CONN_DNODE_PEER_SERVER;
    conn->ops = &dnode_peer_ops;
}

static int
rack_name_cmp(const void *t1, const void *t2)
{
    const struct rack *s1 = t1, *s2 = t2;

    return string_compare(s1->name, s2->name);
}

// The idea here is to have a designated rack in each remote region to replicate
// data to. This is used to replicate writes to remote regions
static void
preselect_remote_rack_for_replication_each(void *elem, void *data)
{
    struct server_pool *sp = elem;
    uint32_t dc_cnt = array_n(&sp->datacenters);
    uint32_t dc_index;
    uint32_t my_rack_index = 0;
    for(dc_index = 0; dc_index < dc_cnt; dc_index++) {
        struct datacenter *dc = array_get(&sp->datacenters, dc_index);
        // sort the racks.
        array_sort(&dc->racks, rack_name_cmp);
        if (string_compare(dc->name, &sp->dc) != 0)
            continue;

        // if the dc is a local dc, get the rack_idx
        uint32_t rack_index;
        uint32_t rack_cnt = array_n(&dc->racks);
        for(rack_index = 0; rack_index < rack_cnt; rack_index++) {
            struct rack *rack = array_get(&dc->racks, rack_index);
            if (string_compare(rack->name, &sp->rack) == 0) {
                my_rack_index = rack_index;
                log_notice("my rack index %u", my_rack_index);
                break;
            }
        }
    }

    for(dc_index = 0; dc_index < dc_cnt; dc_index++) {
        struct datacenter *dc = array_get(&sp->datacenters, dc_index);
        dc->preselected_rack_for_replication = NULL;

        // Nothing to do for local DC, continue;
        if (string_compare(dc->name, &sp->dc) == 0)
            continue;

        // if no racks, keep preselected_rack_for_replication as NULL
        uint32_t rack_cnt = array_n(&dc->racks);
        if (rack_cnt == 0)
            continue;

        // if the dc is a remote dc, get the rack at rack_idx
        // use that as preselected rack for replication
        uint32_t this_rack_index = my_rack_index % rack_cnt;
        dc->preselected_rack_for_replication = array_get(&dc->racks,
                                                         this_rack_index);
        log_notice("Selected rack %.*s for replication to remote region %.*s",
                   dc->preselected_rack_for_replication->name->len,
                   dc->preselected_rack_for_replication->name->data,
                   dc->name->len, dc->name->data);
    }
}

void
preselect_remote_rack_for_replication(struct context *ctx)
{
    array_each(&ctx->pool, preselect_remote_rack_for_replication_each, NULL);
}
