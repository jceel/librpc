/*
 * Copyright 2015-2017 Two Pore Guys, Inc.
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdlib.h>
#include <errno.h>
#include <rpc/object.h>
#include <rpc/connection.h>
#include <glib.h>
#include <glib/gprintf.h>
#include "linker_set.h"
#include "internal.h"
#include "serializer/msgpack.h"

#define	DEFAULT_RPC_TIMEOUT	60
#define	MAX_FDS			128

static rpc_object_t rpc_new_id(void);
static rpc_object_t rpc_pack_frame(const char *, const char *, rpc_object_t,
    rpc_object_t);
static struct rpc_call *rpc_call_alloc(rpc_connection_t, rpc_object_t);
static int rpc_send_frame(rpc_connection_t, rpc_object_t);
static void on_rpc_call(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_rpc_response(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_rpc_fragment(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_rpc_continue(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_rpc_end(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_rpc_abort(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_rpc_error(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_events_event(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_events_event_burst(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_events_subscribe(rpc_connection_t, rpc_object_t, rpc_object_t);
static void on_events_unsubscribe(rpc_connection_t, rpc_object_t, rpc_object_t);
static void *rpc_event_worker(void *);
static int rpc_call_wait_locked(rpc_call_t);
static gboolean rpc_call_timeout(gpointer user_data);

struct message_handler
{
    const char *namespace;
    const char *name;
    void (*handler)(rpc_connection_t conn, rpc_object_t args, rpc_object_t id);
};

static const struct message_handler handlers[] = {
    { "rpc", "call", on_rpc_call },
    { "rpc", "response", on_rpc_response },
    { "rpc", "fragment", on_rpc_fragment },
    { "rpc", "continue", on_rpc_continue },
    { "rpc", "end", on_rpc_end },
    { "rpc", "abort", on_rpc_abort },
    { "rpc", "error", on_rpc_error },
    { "events", "event", on_events_event },
    { "events", "event_burst", on_events_event_burst },
    { "events", "subscribe", on_events_subscribe },
    { "events", "unsubscribe", on_events_unsubscribe },
    { NULL }
};

static size_t
rpc_serialize_fds(rpc_object_t obj, int *fds, size_t *nfds, size_t idx)
{
	__block size_t counter = idx;

	switch (rpc_get_type(obj)) {
	case RPC_TYPE_FD:
		fds[counter++] = obj->ro_value.rv_fd;
		obj->ro_value.rv_fd = (int)idx;
		break;

	case RPC_TYPE_ARRAY:
		rpc_array_apply(obj, ^(size_t aidx, rpc_object_t i) {
		    counter += rpc_serialize_fds(i, fds, nfds, idx);
		    return ((bool)true);
		});
		break;

	case RPC_TYPE_DICTIONARY:
		rpc_dictionary_apply(obj, ^(const char *name, rpc_object_t i) {
		    counter += rpc_serialize_fds(i, fds, nfds, idx);
		    return ((bool)true);
		});
		break;

	default:
		break;
	}

	return (counter);
}

static void
rpc_restore_fds(rpc_object_t obj, int *fds, size_t nfds)
{

	switch (rpc_get_type(obj)) {
		case RPC_TYPE_FD:
			obj->ro_value.rv_fd = fds[obj->ro_value.rv_fd];
			break;

		case RPC_TYPE_ARRAY:
			rpc_array_apply(obj, ^(size_t idx, rpc_object_t item) {
				rpc_restore_fds(item, fds, nfds);
				return ((bool)true);
			});
			break;

		case RPC_TYPE_DICTIONARY:
			rpc_dictionary_apply(obj, ^(const char *key,
			    rpc_object_t value) {
			    	rpc_restore_fds(value, fds, nfds);
			    	return ((bool)true);
			});
			break;

		default:
			break;
	}
}

static void *
rpc_event_worker(void *arg)
{
	struct rpc_subscription *sub;
	const char *name;
	rpc_object_t event;
	rpc_object_t data;
	rpc_connection_t conn = arg;

	for (;;) {
		event = g_async_queue_pop(conn->rco_event_queue);
		if (event == NULL)
			break;

		name = rpc_dictionary_get_string(event, "name");
		data = rpc_dictionary_get_value(event, "args");
		sub = g_hash_table_lookup(conn->rco_subscriptions, name);

		if (sub != NULL) {
			GList *iter;

			for (iter = g_list_first(sub->rsu_handlers); iter != NULL;
			     iter = g_list_next(iter)) {
				rpc_handler_t handler = iter->data;
				handler(name, data);
			}
		}

		conn->rco_event_handler(name, data);
	}

	return (NULL);
}

static rpc_object_t
rpc_pack_frame(const char *ns, const char *name, rpc_object_t id,
    rpc_object_t args)
{
	rpc_object_t obj;

	obj = rpc_dictionary_create();
	rpc_dictionary_set_string(obj, "namespace", ns);
	rpc_dictionary_set_string(obj, "name", name);
	rpc_dictionary_set_value(obj, "id", id ? id : rpc_null_create());
	rpc_dictionary_set_value(obj, "args", args);
	return (obj);
}

static void
on_rpc_call(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{
	struct rpc_inbound_call *call;

	if (conn->rco_server == NULL) {
		rpc_connection_send_err(conn, id, ENOTSUP, "Not supported");
		return;
	}

	call = g_malloc0(sizeof(*call));
	call->ric_conn = conn;
	call->ric_id = id;
	call->ric_args = rpc_dictionary_get_value(args, "args");
	call->ric_name = rpc_dictionary_get_string(args, "method");
	g_mutex_init(&call->ric_mtx);
	g_cond_init(&call->ric_cv);
	g_hash_table_insert(conn->rco_inbound_calls,
	    (gpointer)rpc_string_get_string_ptr(id), call);

	rpc_server_dispatch(conn->rco_server, call);
}

static void
on_rpc_response(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{
	rpc_call_t call;

	call = g_hash_table_lookup(conn->rco_calls,
	    rpc_string_get_string_ptr(id));
	if (call == NULL) {
		return;
	}

	g_mutex_lock(&call->rc_mtx);
	call->rc_status = RPC_CALL_DONE;
	call->rc_result = args;

	g_cond_broadcast(&call->rc_cv);
	g_mutex_unlock(&call->rc_mtx);
}

static void
on_rpc_fragment(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{
	rpc_call_t call;
	rpc_object_t payload;
	uint64_t seqno;

	call = g_hash_table_lookup(conn->rco_calls,
	    rpc_string_get_string_ptr(id));
	if (call == NULL) {
		return;
	}

	seqno = rpc_dictionary_get_uint64(args, "seqno");
	payload = rpc_dictionary_get_value(args, "fragment");

	g_mutex_lock(&call->rc_mtx);
	call->rc_status = RPC_CALL_MORE_AVAILABLE;
	call->rc_result = payload;
	call->rc_seqno = seqno;

	g_cond_broadcast(&call->rc_cv);
	g_mutex_unlock(&call->rc_mtx);
}

static void
on_rpc_continue(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{
	struct rpc_inbound_call *call;

	call = g_hash_table_lookup(conn->rco_inbound_calls,
	    rpc_string_get_string_ptr(id));
	if (call == NULL) {
		return;
	}

	g_mutex_lock(&call->ric_mtx);
	call->ric_consumer_seqno++;
	g_cond_broadcast(&call->ric_cv);
	g_mutex_unlock(&call->ric_mtx);
}

static void
on_rpc_end(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{
	rpc_call_t call;

	call = g_hash_table_lookup(conn->rco_calls,
	    rpc_string_get_string_ptr(id));
	if (call == NULL) {
		return;
	}

	g_mutex_lock(&call->rc_mtx);
	call->rc_status = RPC_CALL_DONE;
	call->rc_result = NULL;

	g_cond_broadcast(&call->rc_cv);
	g_mutex_unlock(&call->rc_mtx);
}

static void
on_rpc_abort(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{
	rpc_call_t call;

	call = g_hash_table_lookup(conn->rco_calls,
	    rpc_string_get_string_ptr(id));
	if (call == NULL) {
		return;
	}
}

static void
on_rpc_error(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{
	rpc_call_t call;

	call = g_hash_table_lookup(conn->rco_calls,
	    rpc_string_get_string_ptr(id));
	if (call == NULL) {
		return;
	}

	g_mutex_lock(&call->rc_mtx);
	call->rc_status = RPC_CALL_ERROR;
	call->rc_result = args;

	g_cond_broadcast(&call->rc_cv);
	g_mutex_unlock(&call->rc_mtx);
}

static void
on_events_event(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{

	g_async_queue_push(conn->rco_event_queue, args);
}

static void
on_events_event_burst(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{

	rpc_array_apply(args, ^(size_t idx, rpc_object_t value) {
	    g_async_queue_push(conn->rco_event_queue, value);
	    return ((bool)true);
	});
}

static void
on_events_subscribe(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{
	g_autoptr(GMutexLocker) mtx = g_mutex_locker_new(&conn->rco_subscription_mtx);

	rpc_array_apply(args, ^(size_t index, rpc_object_t value) {
	    const char *name = rpc_string_get_string_ptr(value);
	    int *refcount;

	    refcount = g_hash_table_lookup(conn->rco_subscriptions, name);
	    if (refcount == NULL) {
		    refcount = g_malloc0(sizeof(int));
		    g_hash_table_insert(conn->rco_subscriptions, (gpointer)name,
			refcount);
	    }
	    (*refcount)++;
	    return ((bool)true);
	});

}

static void
on_events_unsubscribe(rpc_connection_t conn, rpc_object_t args, rpc_object_t id)
{
	g_autoptr(GMutexLocker) mtx = g_mutex_locker_new(&conn->rco_subscription_mtx);

	rpc_array_apply(args, ^(size_t index, rpc_object_t value) {
	    const char *name = rpc_string_get_string_ptr(value);
	    int *refcount;

	    refcount = g_hash_table_lookup(conn->rco_subscriptions, name);
	    if (refcount == NULL)
		    return ((bool)true);

	    (*refcount)--;
	    return ((bool)true);
	});
}

static int
rpc_recv_msg(struct rpc_connection *conn, const void *frame, size_t len,
    int *fds, size_t nfds, struct rpc_credentials *creds)
{
	rpc_object_t msg;

	debugf("received frame: addr=%p, len=%zu", frame, len);

	msg = rpc_msgpack_deserialize(frame, len);
	if (msg == NULL)
		return (-1);

	if (rpc_get_type(msg) != RPC_TYPE_DICTIONARY) {
		rpc_release(msg);
		return (-1);
	}

	rpc_connection_dispatch(conn, msg);
	return (0);
}

static int
rpc_close(struct rpc_connection *conn)
{

	return (0);
}

static struct rpc_call *
rpc_call_alloc(rpc_connection_t conn, rpc_object_t id)
{
	struct rpc_call *call;

	call = calloc(1, sizeof(*call));
	call->rc_conn = conn;
	call->rc_id = id != NULL ? id : rpc_new_id();
	g_mutex_init(&call->rc_mtx);
	g_cond_init(&call->rc_cv);

	return (call);
}

static void
rpc_call_destroy(struct rpc_call *call)
{

	g_free(call);
}

static int
rpc_send_frame(rpc_connection_t conn, rpc_object_t frame)
{
	void *buf;
	int fds[MAX_FDS];
	size_t len, nfds = 0;
	g_autoptr(GMutexLocker) mtx = g_mutex_locker_new(&conn->rco_send_mtx);

	if (rpc_msgpack_serialize(frame, &buf, &len) != 0)
		return (-1);

	rpc_serialize_fds(frame, fds, &nfds, 0);
	rpc_release(frame);
	return (conn->rco_send_msg(conn->rco_arg, buf, len, fds, nfds));
}

void
rpc_connection_send_err(rpc_connection_t conn, rpc_object_t id, int code,
    const char *descr, ...)
{
	rpc_object_t args;
	rpc_object_t frame;
	va_list ap;
	char *str;

	va_start(ap, descr);
	g_vasprintf(&str, descr, ap);
	va_end(ap);

	args = rpc_dictionary_create();
	rpc_dictionary_set_int64(args, "code", code);
	rpc_dictionary_set_string(args, "message", str);
	frame = rpc_pack_frame("rpc", "error", id, args);

	if (rpc_send_frame(conn, frame) != 0) {

	}

done:
	g_free(str);
}

static int
rpc_call_wait_locked(rpc_call_t call)
{
	while (call->rc_status == RPC_CALL_IN_PROGRESS)
		g_cond_wait(&call->rc_cv, &call->rc_mtx);

	return (0);
}

static gboolean
rpc_call_timeout(gpointer user_data)
{
	rpc_call_t call = user_data;

	g_mutex_lock(&call->rc_mtx);
	call->rc_status = RPC_CALL_ERROR;
	call->rc_result = rpc_dictionary_create();
	rpc_dictionary_set_uint64(call->rc_result, "code", ETIMEDOUT);
	rpc_dictionary_set_string(call->rc_result, "message", "Call timed out");
	g_cond_broadcast(&call->rc_cv);
	g_mutex_unlock(&call->rc_mtx);

	return (false);
}

void
rpc_connection_send_errx(rpc_connection_t conn, rpc_object_t id,
    rpc_object_t err)
{

}

void
rpc_connection_send_response(rpc_connection_t conn, rpc_object_t id,
    rpc_object_t response)
{
	rpc_object_t frame;

	frame = rpc_pack_frame("rpc", "response", id, response);
	rpc_send_frame(conn, frame);
}

void
rpc_connection_send_fragment(rpc_connection_t conn, rpc_object_t id,
    int64_t seqno, rpc_object_t fragment)
{
	rpc_object_t frame;
	rpc_object_t args;

	args = rpc_dictionary_create();
	rpc_dictionary_set_int64(args, "seqno", seqno);
	rpc_dictionary_set_value(args, "fragment", fragment);
	frame = rpc_pack_frame("rpc", "fragment", id, args);
	rpc_send_frame(conn, frame);
}

void
rpc_connection_send_end(rpc_connection_t conn, rpc_object_t id, int64_t seqno)
{
	rpc_object_t frame;
	rpc_object_t args;

	args = rpc_dictionary_create();
	rpc_dictionary_set_int64(args, "seqno", seqno);
	frame = rpc_pack_frame("rpc", "end", id, args);
	rpc_send_frame(conn, frame);
}

void
rpc_connection_close_inbound_call(struct rpc_inbound_call *call)
{
	rpc_connection_t conn = call->ric_conn;


}

static void
rpc_answer_call(rpc_call_t call)
{
	//if (call->rc_callback != NULL)
	//	call->rc_callback(call->rc_result);
}

static rpc_object_t
rpc_new_id(void)
{
	char *str = g_uuid_string_random();
	rpc_object_t ret = rpc_string_create(str);

	g_free(str);
	return (ret);
}

rpc_connection_t
rpc_connection_alloc(rpc_server_t server)
{
	struct rpc_connection *conn = NULL;

	conn = g_malloc0(sizeof(*conn));
	conn->rco_server = server;
	conn->rco_calls = g_hash_table_new(g_str_hash, g_str_equal);
	conn->rco_inbound_calls = g_hash_table_new(g_str_hash, g_str_equal);
	conn->rco_subscriptions = g_hash_table_new(g_str_hash, g_str_equal);
	conn->rco_rpc_timeout = DEFAULT_RPC_TIMEOUT;
	conn->rco_recv_msg = &rpc_recv_msg;
	conn->rco_close = &rpc_close;
	g_mutex_init(&conn->rco_send_mtx);

	return (conn);
}

rpc_connection_t
rpc_connection_create(const char *uri, int flags)
{
	const struct rpc_transport *transport;
	struct rpc_connection *conn = NULL;
	char *scheme = NULL;

	scheme = g_uri_parse_scheme(uri);
	transport = rpc_find_transport(scheme);

	if (transport == NULL) {
		errno = ENXIO;
		goto fail;
	}

	conn = g_malloc0(sizeof(*conn));
	conn->rco_uri = uri;
	conn->rco_calls = g_hash_table_new(g_str_hash, g_str_equal);
	conn->rco_inbound_calls = g_hash_table_new(g_str_hash, g_str_equal);
	conn->rco_subscriptions = g_hash_table_new(g_str_hash, g_str_equal);
	conn->rco_rpc_timeout = DEFAULT_RPC_TIMEOUT;
	conn->rco_rpc_timeout = DEFAULT_RPC_TIMEOUT;
	conn->rco_recv_msg = &rpc_recv_msg;
	conn->rco_close = &rpc_close;
	g_mutex_init(&conn->rco_send_mtx);
	g_mutex_init(&conn->rco_subscription_mtx);

	if (transport->connect(conn, uri, NULL) != 0)
		goto fail;

	return (conn);
fail:
	g_free(conn);
	g_free(scheme);
	return (NULL);
}

int
rpc_connection_close(rpc_connection_t conn)
{
	conn->rco_abort(conn->rco_arg);
	g_hash_table_destroy(conn->rco_calls);
	g_hash_table_destroy(conn->rco_inbound_calls);
	g_hash_table_destroy(conn->rco_subscriptions);
	g_free(conn);
	return (0);
}

void
rpc_connection_dispatch(rpc_connection_t conn, rpc_object_t frame)
{
	rpc_object_t id;
	const struct message_handler *h;
	const char *namespace;
	const char *name;

	id = rpc_dictionary_get_value(frame, "id");
	namespace = rpc_dictionary_get_string(frame, "namespace");
	name = rpc_dictionary_get_string(frame, "name");

	if (id == NULL || namespace == NULL || name == NULL) {
		rpc_connection_send_err(conn, id, EINVAL, "Malformed request");
		return;
	}

	debugf("inbound call: namespace=%s, name=%s, id=%s", namespace, name,
	    rpc_string_get_string_ptr(id));

	for (h = &handlers[0]; h->namespace != NULL; h++) {
		if (g_strcmp0(namespace, h->namespace))
			continue;

		if (g_strcmp0(name, h->name))
			continue;

		h->handler(conn, rpc_dictionary_get_value(frame, "args"), id);
		return;
	}

	rpc_connection_send_err(conn, id, ENXIO, "No request handler found");
}

int
rpc_connection_subscribe_event(rpc_connection_t conn, const char *name)
{
	rpc_object_t frame;
	rpc_object_t args, str;

	str = rpc_string_create(name);
	args = rpc_array_create_ex(&str, 1);
	frame = rpc_pack_frame("events", "subscribe", NULL, args);

	if (rpc_send_frame(conn, frame) != 0)
		return (-1);

	return (0);
}

int
rpc_connection_unsubscribe_event(rpc_connection_t conn, const char *name)
{
	rpc_object_t frame;
	rpc_object_t args, str;

	str = rpc_string_create(name);
	args = rpc_array_create_ex(&str, 1);
	frame = rpc_pack_frame("events", "unsubscribe", NULL, args);

	if (rpc_send_frame(conn, frame) != 0)
		return (-1);

	rpc_release(str);
	rpc_release(args);
	return (0);
}

rpc_object_t
rpc_connection_call_sync(rpc_connection_t conn, const char *method, ...)
{
	rpc_call_t call;
	rpc_object_t args;
	rpc_object_t i;
	va_list ap;

	args = rpc_array_create();
	va_start(ap, method);

	for (;;) {
		i = va_arg(ap, rpc_object_t);
		if (i == NULL)
			break;

		rpc_array_append_value(args, i);
	}

	va_end(ap);
	call = rpc_connection_call(conn, method, args);
	rpc_call_wait(call);
	return (rpc_call_result(call));
}

rpc_call_t
rpc_connection_call(rpc_connection_t conn, const char *name, rpc_object_t args)
{
	struct rpc_call *call;
	rpc_object_t id = rpc_new_id();
	rpc_object_t payload = rpc_dictionary_create();
	rpc_object_t frame;

	call = rpc_call_alloc(conn, NULL);
	call->rc_id = id;
	call->rc_type = "call";
	call->rc_method = name;
	call->rc_args = args;

	rpc_dictionary_set_string(payload, "method", name);
	rpc_dictionary_set_value(payload, "args", args != NULL ? args : rpc_array_create());
	frame = rpc_pack_frame("rpc", "call", call->rc_id,  payload);

	g_mutex_lock(&call->rc_mtx);
	g_hash_table_insert(conn->rco_calls,
	    (gpointer)rpc_string_get_string_ptr(id), call);

	if (rpc_send_frame(conn, frame) != 0) {
		g_mutex_unlock(&call->rc_mtx);
		rpc_call_destroy(call);
		return (NULL);
	}

	call->rc_timeout = g_timeout_source_new_seconds(conn->rco_rpc_timeout);
	g_source_set_callback(call->rc_timeout, &rpc_call_timeout, call, NULL);
	g_source_attach(call->rc_timeout, conn->rco_client->rci_g_context);

	call->rc_status = RPC_CALL_IN_PROGRESS;
	g_mutex_unlock(&call->rc_mtx);
	return (call);
}

int
rpc_connection_send_event(rpc_connection_t conn, const char *name,
    rpc_object_t args)
{
	rpc_object_t frame;
	rpc_object_t event;
	const char *names[] = {"name", "args"};
	const rpc_object_t values[] = {
	    rpc_string_create(name),
	    args
	};

	event = rpc_dictionary_create_ex(names, values, 2, true);
	frame = rpc_pack_frame("events", "event", NULL, event);

	if (rpc_send_frame(conn, frame) != 0)
		return (-1);

	return (0);
}

void
rpc_connection_set_event_handler(rpc_connection_t conn, rpc_handler_t handler)
{

	conn->rco_event_handler = handler;
}

int
rpc_call_wait(rpc_call_t call)
{
	int ret;

	g_mutex_lock(&call->rc_mtx);
	ret = rpc_call_wait_locked(call);
	g_mutex_unlock(&call->rc_mtx);

	return (ret);
}

int
rpc_call_continue(rpc_call_t call, bool sync)
{
	rpc_object_t frame;
	int64_t seqno;

	g_mutex_lock(&call->rc_mtx);
	seqno = call->rc_seqno + 1;
	frame = rpc_pack_frame("rpc", "continue", call->rc_id,
	    rpc_int64_create(seqno));

	if (rpc_send_frame(call->rc_conn, frame) != 0) {

	}

	call->rc_status = RPC_CALL_IN_PROGRESS;

	if (sync) {
		rpc_call_wait_locked(call);
		g_mutex_unlock(&call->rc_mtx);
		return (rpc_call_success(call));
	}

	g_mutex_unlock(&call->rc_mtx);
	return (0);
}

int
rpc_call_abort(rpc_call_t call)
{
	rpc_object_t frame;

	frame = rpc_pack_frame("rpc", "abort", call->rc_id, rpc_null_create());
	if (rpc_send_frame(call->rc_conn, frame) != 0) {

	}

	return (0);
}

inline int
rpc_call_timedwait(rpc_call_t call, const struct timespec *ts)
{

}

int
rpc_call_success(rpc_call_t call)
{

	return (call->rc_status == RPC_CALL_DONE);
}

int
rpc_call_status(rpc_call_t call)
{

	return (call->rc_status);
}

inline rpc_object_t
rpc_call_result(rpc_call_t call)
{

	return (call->rc_result);
}

void
rpc_call_free(rpc_call_t call)
{

	g_hash_table_remove(call->rc_conn->rco_calls,
	    (gpointer)rpc_string_get_string_ptr(call->rc_id));
	g_free(call);
}
