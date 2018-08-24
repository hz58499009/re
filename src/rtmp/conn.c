/**
 * @file rtmp/conn.c  Real Time Messaging Protocol (RTMP) -- NetConnection
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_net.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_tcp.h>
#include <re_sys.h>
#include <re_odict.h>
#include <re_rtmp.h>
#include "rtmp.h"


#define CONN_CHUNK_ID  (3)
#define CONN_STREAM_ID (0)  /* always zero for netconn */


#define WINDOW_ACK_SIZE 2500000


struct rtmp_conn {
	struct tcp_conn *tc;
	bool is_client;
	uint8_t x1[RTMP_SIG_SIZE];        /* C1 or S1 */
	enum rtmp_handshake_state state;
	struct mbuf *mb;                  /* TCP reassembly buffer */
	struct rtmp_dechunker *dechunk;
	bool term;
	rtmp_estab_h *estabh;
	rtmp_close_h *closeh;
	void *arg;

	/* client specific: */
	char *app;
	char *uri;
};


static int send_amf_command(struct rtmp_conn *conn,
			    unsigned format, uint32_t chunk_id,
			    uint32_t msg_stream_id,
			    const uint8_t *cmd, size_t len);
static int rtmp_chunk_handler(const struct rtmp_header *hdr,
			      const uint8_t *pld, size_t pld_len, void *arg);


static int build_connect(struct mbuf *mb, const char *app, const char *url)
{
	double transaction_id = 1.0;
	int err;

	err  = rtmp_amf_encode_string(mb, "connect");
	err |= rtmp_amf_encode_number(mb, transaction_id);

	err |= rtmp_amf_encode_object_start(mb);
	{
		err |= rtmp_amf_encode_key(mb, "app");
		err |= rtmp_amf_encode_string(mb, app);

		err |= rtmp_amf_encode_key(mb, "flashVer");
		err |= rtmp_amf_encode_string(mb, "LNX 9,0,124,2");

		err |= rtmp_amf_encode_key(mb, "tcUrl");
		err |= rtmp_amf_encode_string(mb, url);

		err |= rtmp_amf_encode_key(mb, "fpad");
		err |= rtmp_amf_encode_boolean(mb, false);

		err |= rtmp_amf_encode_key(mb, "capabilities");
		err |= rtmp_amf_encode_number(mb, 15.0);

		err |= rtmp_amf_encode_key(mb, "audioCodecs");
		err |= rtmp_amf_encode_number(mb, 4071.0);

		err |= rtmp_amf_encode_key(mb, "videoCodecs");
		err |= rtmp_amf_encode_number(mb, 252.0);

		err |= rtmp_amf_encode_key(mb, "videoFunction");
		err |= rtmp_amf_encode_number(mb, 1.0);
	}
	err |= rtmp_amf_encode_object_end(mb);

	return err;
}


static void conn_destructor(void *data)
{
	struct rtmp_conn *conn = data;

	mem_deref(conn->tc);
	mem_deref(conn->mb);
	mem_deref(conn->dechunk);
	mem_deref(conn->uri);
	mem_deref(conn->app);
}


static int reply(struct rtmp_conn *conn, uint64_t transaction_id)
{
	struct mbuf *mb;
	int err;

	mb = mbuf_alloc(256);
	if (!mb)
		return ENOMEM;

	re_printf("[%s] reply: tid=%llu\n",
		  conn->is_client ? "Client" : "Server",
		  transaction_id);

	err  = rtmp_amf_encode_string(mb, "_result");
	err |= rtmp_amf_encode_number(mb, transaction_id);

	err |= rtmp_amf_encode_object_start(mb);
	{
		err |= rtmp_amf_encode_key(mb, "fmsVer");
		err |= rtmp_amf_encode_string(mb, "FMS/3,5,7,7009");

		err |= rtmp_amf_encode_key(mb, "capabilities");
		err |= rtmp_amf_encode_number(mb, 31);

		err |= rtmp_amf_encode_key(mb, "mode");
		err |= rtmp_amf_encode_number(mb, 1);

	}
	err |= rtmp_amf_encode_object_end(mb);


	/* XXX: add Information object */


	err = send_amf_command(conn, 0, CONN_CHUNK_ID, CONN_STREAM_ID,
			       mb->buf, mb->end);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}


static int control_send_was(struct rtmp_conn *conn, size_t was)
{
	struct mbuf *mb = mbuf_alloc(4);
	uint32_t chunk_id = RTMP_CHUNK_ID_CONTROL;
	uint32_t timestamp = 0;
	uint32_t timestamp_delta = 0;
	int err;

	(void)mbuf_write_u32(mb, htonl(was));

	err = rtmp_chunker(0, chunk_id, timestamp, timestamp_delta,
			   RTMP_TYPE_WINDOW_ACK_SIZE, CONN_STREAM_ID,
			   mb->buf, mb->end, rtmp_chunk_handler, conn);

	mem_deref(mb);

	return err;
}


static int control_send_set_peer_bw(struct rtmp_conn *conn,
				    size_t was, uint8_t limit_type)
{
	struct mbuf *mb = mbuf_alloc(5);
	uint32_t chunk_id = RTMP_CHUNK_ID_CONTROL;
	uint32_t timestamp = 0;
	uint32_t timestamp_delta = 0;
	int err;

	(void)mbuf_write_u32(mb, htonl(was));
	(void)mbuf_write_u8(mb, limit_type);

	err = rtmp_chunker(0, chunk_id, timestamp, timestamp_delta,
			   RTMP_TYPE_SET_PEER_BANDWIDTH, CONN_STREAM_ID,
			   mb->buf, mb->end, rtmp_chunk_handler, conn);

	mem_deref(mb);

	return err;
}


static int control_send_user_control_msg(struct rtmp_conn *conn)
{
	struct mbuf *mb = mbuf_alloc(6);
	uint32_t chunk_id = RTMP_CHUNK_ID_CONTROL;
	uint32_t timestamp = 0;
	uint32_t timestamp_delta = 0;
	int err;

	(void)mbuf_write_u16(mb, 0);
	(void)mbuf_write_u32(mb, 0);

	err = rtmp_chunker(0, chunk_id, timestamp, timestamp_delta,
			   RTMP_TYPE_USER_CONTROL_MSG, CONN_STREAM_ID,
			   mb->buf, mb->end, rtmp_chunk_handler, conn);

	mem_deref(mb);

	return err;
}


static void client_handle_amf_command(struct rtmp_conn *conn,
				      const struct command_header *cmd_hdr,
				      struct odict *dict)
{

	if (0 == str_casecmp(cmd_hdr->name, "_result")) {

		re_printf("client: Established\n");

		conn->estabh(conn->arg);
	}
	else {
		re_printf("rtmp: client: command not handled (%s)\n",
			  cmd_hdr->name);
	}

}


static void server_handle_amf_command(struct rtmp_conn *conn,
				      const struct command_header *cmd_hdr,
				      struct odict *dict)
{

	if (0 == str_casecmp(cmd_hdr->name, "connect")) {

		control_send_was(conn, WINDOW_ACK_SIZE);

		control_send_set_peer_bw(conn, WINDOW_ACK_SIZE, 2);

		control_send_user_control_msg(conn);

		reply(conn, cmd_hdr->transaction_id);


		conn->estabh(conn->arg);
	}
	else {
		re_printf("rtmp: server: command not handled (%s)\n",
			  cmd_hdr->name);
	}
}


static void handle_amf_command(struct rtmp_conn *conn,
			       const uint8_t *cmd, size_t len)
{
	struct mbuf mb = {
		.buf = (uint8_t *)cmd,
		.end = len,
		.size = len,
	};
	struct command_header cmd_hdr;
	struct odict *dict;
	int err;

	err = odict_alloc(&dict, 32);
	if (err)
		return;

	err = rtmp_amf_decode(dict, &mb);
	if (err) {
		re_printf("rtmp: amf decode error (%m)\n", err);
		goto out;
	}

	err = rtmp_command_header_decode(&cmd_hdr, dict);
	if (err) {
		re_printf("could not decode command header (%m)\n", err);
		goto out;
	}

#if 1
	re_printf("[%s] Command: %H\n",
		  conn->is_client ? "Client" : "Server",
		  rtmp_command_header_print, &cmd_hdr);
	re_printf("     %H\n", odict_debug, dict);
#endif

	if (conn->is_client) {
		client_handle_amf_command(conn, &cmd_hdr, dict);
	}
	else {
		server_handle_amf_command(conn, &cmd_hdr, dict);
	}

 out:
	mem_deref(dict);
}


static void rtmp_msg_handler(struct rtmp_message *msg, void *arg)
{
#if 1
	struct rtmp_conn *conn = arg;
	void *p;
	uint32_t val;
	struct mbuf mb = {
		.pos = 0,
		.end = msg->length,
		.size = msg->length,
		.buf = msg->buf
	};
	uint32_t was;
	uint16_t event;
	uint8_t limit;

	if (conn->term)
		return;

	re_printf("[%s] ### recv message: type 0x%02x (%s) (%zu bytes)\n",
		  conn->is_client ? "Client" : "Server",
		  msg->type, rtmp_packet_type_name(msg->type), msg->length);

	switch (msg->type) {

	case RTMP_TYPE_SET_CHUNK_SIZE:
		p = msg->buf;
		val = *(uint32_t *)p;

		val = ntohl(val) & 0x7fffffff;

		re_printf("set chunk size:  %u bytes\n", val);

		rtmp_dechunker_set_chunksize(conn->dechunk, val);
		break;

	case RTMP_TYPE_AMF0:
		handle_amf_command(conn, msg->buf, msg->length);
		break;

	case RTMP_TYPE_WINDOW_ACK_SIZE:
		was = ntohl(mbuf_read_u32(&mb));
		re_printf("[%s] got Window Ack Size from peer: %u\n",
			  conn->is_client ? "Client" : "Server", was);
		break;

	case RTMP_TYPE_SET_PEER_BANDWIDTH:
		was = ntohl(mbuf_read_u32(&mb));
		limit = mbuf_read_u8(&mb);
		re_printf("[%s] got Set Peer Bandwidth from peer:"
			  " was=%u, limit_type=%u\n",
			  conn->is_client ? "Client" : "Server",
			  was, limit);

		control_send_was(conn, WINDOW_ACK_SIZE);

		break;

	case RTMP_TYPE_USER_CONTROL_MSG:
		event = ntohs(mbuf_read_u16(&mb));

		re_printf("[%s] got User Control Message: event_type=%u\n",
			  conn->is_client ? "Client" : "Server",
			  event);
		break;

#if 0
	case RTMP_TYPE_AUDIO:
		++cli->n_audio;
		break;

	case RTMP_TYPE_VIDEO:
		++cli->n_video;
		break;
#endif

	default:
		re_printf("!!! unhandled message: type=%d\n", msg->type);
		break;
	}

#endif
}


static struct rtmp_conn *rtmp_conn_alloc(bool is_client,
					 rtmp_estab_h *estabh,
					 rtmp_close_h *closeh, void *arg)
{
	struct rtmp_conn *conn;
	int err = 0;

	conn = mem_zalloc(sizeof(*conn), conn_destructor);
	if (!conn)
		return NULL;

	conn->is_client = is_client;

	rand_bytes(conn->x1, sizeof(conn->x1));

	conn->state = RTMP_STATE_UNINITIALIZED;

	err = rtmp_dechunker_alloc(&conn->dechunk, rtmp_msg_handler, conn);
	if (err)
		goto out;

	conn->estabh = estabh;
	conn->closeh = closeh;
	conn->arg = arg;

 out:
	if (err)
		return mem_deref(conn);

	return conn;
}


static void set_state(struct rtmp_conn *conn, enum rtmp_handshake_state state)
{
	if (!conn)
		return;

	re_printf("[%s] set state: %d (%s)\n",
		  conn->is_client ? "Client" : "Server",
		  state, rtmp_handshake_name(state));

	conn->state = state;
}


static int send_packet(struct rtmp_conn *conn,
		       const uint8_t *pkt, size_t len)
{
	struct mbuf *mb = mbuf_alloc(2048);
	int err;

	if (!conn || !pkt || !len)
		return EINVAL;

	err = mbuf_write_mem(mb, pkt, len);
	if (err)
		goto out;

	mb->pos = 0;

	re_printf("[%s] send packet (%zu bytes)\n",
		  conn->is_client ? "Client" : "Server", mb->end);

	err = tcp_send(conn->tc, mb);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}


static int handshake_start(struct rtmp_conn *conn)
{
	uint8_t x0 = RTMP_PROTOCOL_VERSION;
	int err;

	err = send_packet(conn, &x0, sizeof(x0));
	if (err)
		goto out;

	err = send_packet(conn, conn->x1, sizeof(conn->x1));
	if (err)
		return err;

	set_state(conn, RTMP_STATE_VERSION_SENT);

 out:
	return err;
}


static void conn_close(struct rtmp_conn *conn, int err)
{
	re_printf("rtmp: connection closed (%m)\n", err);

	conn->tc = mem_deref(conn->tc);

	conn->term = true;

	if (conn->closeh)
		conn->closeh(err, conn->arg);
}


static void tcp_estab_handler(void *arg)
{
	struct rtmp_conn *conn = arg;
	int err = 0;

	re_printf("[%s] TCP established\n",
		  conn->is_client ? "Client" : "Server");

	if (conn->is_client) {

		err = handshake_start(conn);
		if (err)
			goto out;
	}

 out:
	if (err) {
		conn_close(conn, err);
	}
}


static int rtmp_chunk_handler(const struct rtmp_header *hdr,
			      const uint8_t *pld, size_t pld_len, void *arg)
{
	struct rtmp_conn *conn = arg;
	struct mbuf *mb;
	int err;

	mb = mbuf_alloc(1024);
	if (!mb)
		return ENOMEM;

	err  = rtmp_header_encode(mb, hdr);
	err |= mbuf_write_mem(mb, pld, pld_len);
	if (err)
		goto out;

	mb->pos = 0;

	err = tcp_send(conn->tc, mb);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}


static int send_amf_command(struct rtmp_conn *conn,
			    unsigned format, uint32_t chunk_id,
			    uint32_t msg_stream_id,
			    const uint8_t *cmd, size_t len)
{
	uint32_t timestamp = 0;
	int err;

	if (!conn || !cmd || !len)
		return EINVAL;

	re_printf("[%s] send AMF command: [fmt=%u, chunk=%u, stream=%u]"
		  " %zu bytes\n",
		  conn->is_client ? "Client" : "Server",
		  format, chunk_id, msg_stream_id, len);

	err = rtmp_chunker(format, chunk_id,
			   timestamp, 0,
			   RTMP_TYPE_AMF0, msg_stream_id,
			   cmd, len,
			   rtmp_chunk_handler, conn);
	if (err)
		return err;

	return err;
}


static int send_connect(struct rtmp_conn *conn)
{
	struct mbuf *mb;
	int err = 0;

	mb = mbuf_alloc(512);

	err = build_connect(mb, conn->app, conn->uri);
	if (err)
		goto out;

	err = send_amf_command(conn, 0,
			       CONN_CHUNK_ID, CONN_STREAM_ID,
			       mb->buf, mb->end);
	if (err) {
		re_printf("rtmp: failed to send AMF command (%m)\n", err);
		goto out;
	}

 out:
	mem_deref(mb);

	return err;
}


static int handshake_done(struct rtmp_conn *conn)
{
	int err = 0;

	re_printf("[%s] ** handshake done **\n",
		  conn->is_client ? "Client" : "Server");

	if (conn->is_client) {

		err = send_connect(conn);
	}

	return err;
}


static int client_handle_packet(struct rtmp_conn *conn, struct mbuf *mb)
{
	uint8_t s0;
	uint8_t s1[RTMP_SIG_SIZE];
	uint8_t s2[RTMP_SIG_SIZE];
	uint8_t c2[RTMP_SIG_SIZE];
	int err = 0;

	switch (conn->state) {

	case RTMP_STATE_VERSION_SENT:
		if (mbuf_get_left(mb) < (1+RTMP_SIG_SIZE))
			return ENODATA;

		s0 = mbuf_read_u8(mb);
		if (s0 != RTMP_PROTOCOL_VERSION) {
			re_printf("rtmp: handshake: illegal version %u\n", s0);
			return EPROTO;
		}

		err = mbuf_read_mem(mb, s1, sizeof(s1));
		if (err)
			return err;

		memcpy(c2, s1, sizeof(c2));

		err = send_packet(conn, c2, sizeof(c2));
		if (err)
			return err;

		set_state(conn, RTMP_STATE_ACK_SENT);
		break;

	case RTMP_STATE_ACK_SENT:
		if (mbuf_get_left(mb) < RTMP_SIG_SIZE)
			return ENODATA;

		err = mbuf_read_mem(mb, s2, sizeof(s2));
		if (err)
			return err;

		/* XXX: compare */

		set_state(conn, RTMP_STATE_HANDSHAKE_DONE);

		handshake_done(conn);
		break;

	case RTMP_STATE_HANDSHAKE_DONE:
		err = rtmp_dechunker_receive(conn->dechunk, mb);
		if (err)
			return err;
		break;

	default:
		re_printf("[%s] unhandled state %d\n",
			  conn->is_client ? "Client" : "Server",
			  conn->state);
		return EPROTO;
	}

	return 0;
}


static int server_handle_packet(struct rtmp_conn *conn, struct mbuf *mb)
{
	uint8_t c0;
	uint8_t s2[RTMP_SIG_SIZE];
	uint8_t c1[RTMP_SIG_SIZE];
	uint8_t c2[RTMP_SIG_SIZE];
	int err = 0;

	switch (conn->state) {

	case RTMP_STATE_UNINITIALIZED:
		if (mbuf_get_left(mb) < (1+RTMP_SIG_SIZE))
			return ENODATA;

		c0 = mbuf_read_u8(mb);
		if (c0 != RTMP_PROTOCOL_VERSION) {
			re_printf("rtmp: handshake: illegal version %u\n", c0);
			return EPROTO;
		}

		err = mbuf_read_mem(mb, c1, sizeof(c1));
		if (err)
			return err;

		err = handshake_start(conn);
		break;

	case RTMP_STATE_VERSION_SENT:
		if (mbuf_get_left(mb) < (RTMP_SIG_SIZE))
			return ENODATA;

		err = mbuf_read_mem(mb, c2, sizeof(c2));
		if (err)
			return err;

		/* XXX memcpy(c2, s1, sizeof(c2)); */

		err = send_packet(conn, s2, sizeof(s2));
		if (err)
			return err;

		set_state(conn, RTMP_STATE_ACK_SENT);
		set_state(conn, RTMP_STATE_HANDSHAKE_DONE);

		handshake_done(conn);
		break;

	case RTMP_STATE_HANDSHAKE_DONE:
		err = rtmp_dechunker_receive(conn->dechunk, mb);
		if (err)
			return err;
		break;

	default:
		re_printf("[%s] unhandled state %d\n",
			  conn->is_client ? "Client" : "Server",
			  conn->state);
		return EPROTO;
	}

	return 0;
}


static void tcp_recv_handler(struct mbuf *mb_pkt, void *arg)
{
	struct rtmp_conn *conn = arg;
	int err;

#if 1
	re_printf("[%s] tcp recv %zu bytes\n",
		  conn->is_client ? "Client" : "Server",
		  mbuf_get_left(mb_pkt));
#endif

	/* re-assembly of fragments */
	if (conn->mb) {
		size_t pos;

		pos = conn->mb->pos;

		conn->mb->pos = conn->mb->end;

		err = mbuf_write_mem(conn->mb,
				     mbuf_buf(mb_pkt), mbuf_get_left(mb_pkt));
		if (err)
			goto out;

		conn->mb->pos = pos;
	}
	else {
		conn->mb = mem_ref(mb_pkt);
	}

	while (mbuf_get_left(conn->mb) > 0) {

		size_t pos;

		pos = conn->mb->pos;

		if (conn->is_client)
			err = client_handle_packet(conn, conn->mb);
		else
			err = server_handle_packet(conn, conn->mb);
		if (err) {

			/* rewind */
			conn->mb->pos = pos;

			if (err == ENODATA) {
				re_printf(".. wait for more data"
					  " (%zu bytes in buffer)\n",
					  conn->mb->end - conn->mb->pos);
				err = 0;
			}
			break;
		}

		if (conn->mb->pos >= conn->mb->end) {
			conn->mb = mem_deref(conn->mb);
			break;
		}
	}

 out:
	if (err) {
		re_printf("ERROR!\n");
		conn_close(conn, err);
	}

}


static void tcp_close_handler(int err, void *arg)
{
	struct rtmp_conn *conn = arg;

	re_printf("TCP connection closed (%m)\n", err);

	conn_close(conn, err);
}


int rtmp_connect(struct rtmp_conn **connp, const char *uri,
		 rtmp_estab_h *estabh, rtmp_close_h *closeh, void *arg)
{
	struct rtmp_conn *conn;
	struct pl pl_addr;
	struct pl pl_port;
	struct pl pl_app;
	struct pl pl_stream;
	struct sa addr;
	char *stream = NULL;
	int err = 0;

	if (!connp || !uri)
		return EINVAL;

	if (re_regex(uri, strlen(uri), "rtmp://[^:/]+:[0-9]+/[^/]+/[^]+",
		     &pl_addr, &pl_port, &pl_app, &pl_stream)) {
		re_printf("invalid uri '%s'\n", uri);
		return EINVAL;
	}

	err = sa_set(&addr, &pl_addr, pl_u32(&pl_port));
	if (err)
		return err;

	conn = rtmp_conn_alloc(true, estabh, closeh, arg);
	if (!conn)
		return ENOMEM;

	re_printf("  addr:   %r\n", &pl_addr);
	re_printf("  port:   %r\n", &pl_port);
	re_printf("  app:    %r\n", &pl_app);
	re_printf("  stream: %r\n", &pl_stream);

	err |= pl_strdup(&conn->app,         &pl_app);
	err |= pl_strdup(&stream, &pl_stream);
	if (err)
		goto out;

	err = str_dup(&conn->uri, uri);
	if (err)
		goto out;

	err = tcp_connect(&conn->tc, &addr, tcp_estab_handler,
			  tcp_recv_handler, tcp_close_handler, conn);
	if (err)
		goto out;

 out:
	mem_deref(stream);

	if (err)
		mem_deref(conn);
	else
		*connp = conn;

	return err;
}


int rtmp_accept(struct rtmp_conn **connp, struct tcp_sock *ts,
		rtmp_estab_h *estabh, rtmp_close_h *closeh, void *arg)
{
	struct rtmp_conn *conn;
	int err;

	if (!connp || !ts)
		return EINVAL;

	conn = rtmp_conn_alloc(false, estabh, closeh, arg);
	if (!conn)
		return ENOMEM;

	err = tcp_accept(&conn->tc, ts, tcp_estab_handler,
			 tcp_recv_handler, tcp_close_handler, conn);
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(conn);
	else
		*connp = conn;

	return err;
}
