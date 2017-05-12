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


#include <unistd.h>
#include <inttypes.h>
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <rpc/object.h>
#include "internal.h"

static const char *rpc_types[] = {
    [RPC_TYPE_NULL] = "null",
    [RPC_TYPE_BOOL] = "bool",
    [RPC_TYPE_UINT64] = "uint64",
    [RPC_TYPE_INT64] = "int64",
    [RPC_TYPE_DOUBLE] = "double",
    [RPC_TYPE_DATE] = "date",
    [RPC_TYPE_STRING] = "string",
    [RPC_TYPE_BINARY] = "binary",
    [RPC_TYPE_FD] = "fd",
    [RPC_TYPE_DICTIONARY] = "dictionary",
    [RPC_TYPE_ARRAY] = "array"
};

static rpc_object_t
rpc_prim_create(rpc_type_t type, union rpc_value val)
{
	struct rpc_object *ro;

	ro = (rpc_object_t)malloc(sizeof(*ro));
	if (ro == NULL)
		abort();

	ro->ro_type = type;
	ro->ro_value = val;
	ro->ro_refcnt = 1;

	return (ro);
}

static size_t
rpc_data_hash(const uint8_t *data, size_t length)
{
	size_t hash = 5381;

	while (length--)
		hash = ((hash << 5) + hash) + data[length];

	return (hash);
}

static void
rpc_create_description (GString *description, rpc_object_t object, unsigned int indent_lvl, bool nested)
{
	int i;
	unsigned int local_indent_lvl = indent_lvl + 1;
	size_t data_length;
	uint8_t *data_ptr;

	if ((indent_lvl > 0) && (!nested))
		g_string_append_printf(description, "%*s", (indent_lvl * 4), "");

	g_string_append_printf(description, "<%s> ", rpc_types[object->ro_type]);

	switch (object->ro_type) {
		case RPC_TYPE_NULL:
			break;

		case RPC_TYPE_BOOL:
			if (object->ro_value.rv_b == true)
				g_string_append(description, "true");
			else
				g_string_append(description, "false");

			break;

		case RPC_TYPE_INT64:
			g_string_append_printf(description, "%" PRId64 "", object->ro_value.rv_i);
			break;

		case RPC_TYPE_FD:
			g_string_append_printf(description, "%u", object->ro_value.rv_fd);
			break;

		case RPC_TYPE_UINT64:
			g_string_append_printf(description, "%" PRIu64 "", object->ro_value.rv_ui);
			break;

		case RPC_TYPE_DOUBLE:
			g_string_append_printf(description, "%f", object->ro_value.rv_d);
			break;

		case RPC_TYPE_DATE:
			g_string_append(description, g_date_time_format(object->ro_value.rv_datetime, "%F %T"));
			break;

		case RPC_TYPE_STRING:
			g_string_append_printf(description, "\"%s\"", rpc_string_get_string_ptr(object));
			break;

		case RPC_TYPE_BINARY:
			data_ptr = (uint8_t *)rpc_data_get_bytes_ptr(object);
			data_length = MIN(object->ro_value.rv_bin.length, 16);

			for (i = 0; i < data_length; i++)
				g_string_append_printf(description, "%02x", data_ptr[i]);

			break;

		case RPC_TYPE_DICTIONARY:
			g_string_append(description, "{\n");
			rpc_dictionary_apply(object, ^(const char *k, rpc_object_t v) {
				g_string_append_printf(description, "%*s%s: ", (local_indent_lvl * 4), "", k);
				rpc_create_description(description, v, local_indent_lvl, true);
				g_string_append(description, ",\n");
				return ((bool)true);
			});
			if (indent_lvl > 0)
				g_string_append_printf(description, "%*s", (indent_lvl * 4), "");

			g_string_append(description, "}");
			break;

		case RPC_TYPE_ARRAY:
			g_string_append(description, "[\n");
			rpc_array_apply(object, ^(size_t idx, rpc_object_t v) {
				g_string_append_printf(
				    description, "%*s%u: ",
				    (local_indent_lvl * 4),
				    "",
				    (unsigned int)idx);

				rpc_create_description(description, v, local_indent_lvl, true);
				g_string_append(description, ",\n");
				return ((bool)true);
			});
			if (indent_lvl > 0)
				g_string_append_printf(description, "%*s", (indent_lvl * 4), "");

			g_string_append(description, "]");
			break;
	}

	if (nested == false)
		g_string_append(description, "\n");
}

inline rpc_object_t
rpc_retain(rpc_object_t object)
{

	g_atomic_int_inc(&object->ro_refcnt);
	return (object);
}

inline int
rpc_release_impl(rpc_object_t object)
{

	assert(object->ro_refcnt > 0);
	if (g_atomic_int_dec_and_test(&object->ro_refcnt)) {
		switch (object->ro_type) {
		case RPC_TYPE_BINARY:
			if (object->ro_value.rv_bin.copy == true)
				g_free((void *)object->ro_value.rv_bin.ptr);

			break;

		case RPC_TYPE_STRING:
			g_string_free(object->ro_value.rv_str, true);
			break;

		case RPC_TYPE_DATE:
			g_date_time_unref(object->ro_value.rv_datetime);
			break;

		case RPC_TYPE_ARRAY:
			rpc_array_apply(object, ^(size_t idx, rpc_object_t v) {
				rpc_release_impl(v);
				return ((bool)true);
			});
			g_array_free(object->ro_value.rv_list, true);
			break;

		case RPC_TYPE_DICTIONARY:
			g_hash_table_unref(object->ro_value.rv_dict);
			break;

		default:
			break;
		}
		free(object);
                return (0);
	}

        return (object->ro_refcnt);
}

inline rpc_type_t
rpc_get_type(rpc_object_t object)
{

	return (object->ro_type);
}

inline rpc_object_t
rpc_copy(rpc_object_t object)
{
	rpc_object_t tmp;

	switch (object->ro_type) {
	case RPC_TYPE_NULL:
		return (rpc_null_create());

	case RPC_TYPE_BOOL:
		return (rpc_bool_create(object->ro_value.rv_b));

	case RPC_TYPE_INT64:
		return (rpc_int64_create(object->ro_value.rv_i));

	case RPC_TYPE_UINT64:
		return (rpc_uint64_create(object->ro_value.rv_ui));

	case RPC_TYPE_DATE:
		return (rpc_date_create(rpc_date_get_value(object)));

	case RPC_TYPE_DOUBLE:
		return (rpc_double_create(object->ro_value.rv_d));

	case RPC_TYPE_FD:
                return (rpc_fd_create(object->ro_value.rv_fd));

	case RPC_TYPE_STRING:
		return (rpc_string_create(strdup(
		    rpc_string_get_string_ptr(object))));

	case RPC_TYPE_BINARY:
                return rpc_data_create(
                    rpc_data_get_bytes_ptr(object),
                    rpc_data_get_length(object),
                    true);

	case RPC_TYPE_DICTIONARY:
		tmp = rpc_dictionary_create();
		rpc_dictionary_apply(object, ^(const char *k, rpc_object_t v) {
		    rpc_dictionary_set_value(tmp, k, rpc_copy(v));
		    return ((bool)true);
		});
		return (tmp);

	case RPC_TYPE_ARRAY:
		tmp = rpc_array_create();
		rpc_array_apply(object, ^(size_t idx, rpc_object_t v) {
		    rpc_array_set_value(tmp, idx, rpc_copy(v));
		    return ((bool)true);
		});
		return (tmp);
	}

	return (0);
}

inline bool
rpc_equal(rpc_object_t o1, rpc_object_t o2)
{

	return (rpc_hash(o1) == rpc_hash(o2));
}

inline size_t
rpc_hash(rpc_object_t object)
{
	__block size_t hash = 0;

	switch (object->ro_type) {
	case RPC_TYPE_NULL:
		return (0);

	case RPC_TYPE_BOOL:
		return ((size_t)object->ro_value.rv_b);

	case RPC_TYPE_INT64:
		return ((size_t)object->ro_value.rv_i);

	case RPC_TYPE_UINT64:
		return ((size_t)object->ro_value.rv_ui);

	case RPC_TYPE_DOUBLE:
		return ((size_t)object->ro_value.rv_d);

	case RPC_TYPE_FD:
		return ((size_t)object->ro_value.rv_fd);

	case RPC_TYPE_DATE:
		return ((size_t)rpc_date_get_value(object));

	case RPC_TYPE_STRING:
		return (g_string_hash(object->ro_value.rv_str));

	case RPC_TYPE_BINARY:
		return (rpc_data_hash(
                    (uint8_t *)rpc_data_get_bytes_ptr(object),
		    rpc_data_get_length(object)));

	case RPC_TYPE_DICTIONARY:
		rpc_dictionary_apply(object, ^(const char *k, rpc_object_t v) {
		    hash ^= rpc_data_hash((const uint8_t *)k, strlen(k));
		    hash ^= rpc_hash(v);
		    return ((bool)true);
		});
		return (hash);

	case RPC_TYPE_ARRAY:
		rpc_array_apply(object, ^(size_t idx, rpc_object_t v) {
		    hash ^= rpc_hash(v);
		    return ((bool)true);
		});
		return (hash);
	}

	return (0);
}

inline char *
rpc_copy_description(rpc_object_t object)
{
	GString *description;

	description = g_string_new(NULL);
	rpc_create_description(description, object, 0, false);

	return g_string_free(description, false);
}

inline rpc_object_t
rpc_null_create(void)
{
	union rpc_value val;

	return (rpc_prim_create(RPC_TYPE_NULL, val));
}

inline rpc_object_t
rpc_bool_create(bool value)
{
	union rpc_value val;

	val.rv_b = value;
	return (rpc_prim_create(RPC_TYPE_BOOL, val));
}

inline bool
rpc_bool_get_value(rpc_object_t xbool)
{

	if (xbool->ro_type != RPC_TYPE_BOOL)
		return (false);

	return (xbool->ro_value.rv_b);
}

inline rpc_object_t
rpc_int64_create(int64_t value)
{
	union rpc_value val;

	val.rv_i = value;
	return (rpc_prim_create(RPC_TYPE_INT64, val));
}

inline int64_t
rpc_int64_get_value(rpc_object_t xint)
{

	if (xint->ro_type != RPC_TYPE_INT64)
		return (-1);

	return (xint->ro_value.rv_i);
}

inline rpc_object_t
rpc_uint64_create(uint64_t value)
{
	union rpc_value val;

	val.rv_ui = value;
	return (rpc_prim_create(RPC_TYPE_UINT64, val));
}

inline uint64_t
rpc_uint64_get_value(rpc_object_t xuint)
{

	if (xuint->ro_type != RPC_TYPE_UINT64)
		return (0);

	return (xuint->ro_value.rv_ui);
}

inline rpc_object_t
rpc_double_create(double value)
{
	union rpc_value val;

	val.rv_d = value;
	return (rpc_prim_create(RPC_TYPE_DOUBLE, val));
}

inline double
rpc_double_get_value(rpc_object_t xdouble)
{

	if (xdouble->ro_type != RPC_TYPE_DOUBLE)
		return (0);

	return (xdouble->ro_value.rv_d);
}

inline rpc_object_t
rpc_date_create(int64_t interval)
{
	union rpc_value val;

        val.rv_datetime = g_date_time_new_from_unix_utc(interval);
	return (rpc_prim_create(RPC_TYPE_DATE, val));
}

inline
rpc_object_t rpc_date_create_from_current(void)
{
        union rpc_value val;

        val.rv_datetime = g_date_time_new_now_utc();
        return (rpc_prim_create(RPC_TYPE_DATE, val));
}

inline int64_t
rpc_date_get_value(rpc_object_t xdate)
{

	if (xdate->ro_type != RPC_TYPE_DATE)
		return (0);

	return (g_date_time_to_unix(xdate->ro_value.rv_datetime));
}

inline rpc_object_t
rpc_data_create(const void *bytes, size_t length, bool copy)
{
        union rpc_value value;

        if (copy == true) {
                value.rv_bin.ptr = (uintptr_t)malloc(length);
                memcpy((void *)value.rv_bin.ptr, bytes, length);
        } else
                value.rv_bin.ptr = (uintptr_t)bytes;

	value.rv_bin.copy = copy;
	value.rv_bin.length = length;

        return (rpc_prim_create(
            RPC_TYPE_BINARY,
            value));
}

inline size_t
rpc_data_get_length(rpc_object_t xdata)
{

	if (xdata->ro_type != RPC_TYPE_BINARY)
		return (0);

	return (xdata->ro_value.rv_bin.length);
}

inline const void *
rpc_data_get_bytes_ptr(rpc_object_t xdata)
{

	if (xdata->ro_type != RPC_TYPE_BINARY)
		return (NULL);

	return ((const void *)xdata->ro_value.rv_bin.ptr);
}

inline size_t
rpc_data_get_bytes(rpc_object_t xdata, void *buffer, size_t off,
    size_t length)
{
        size_t cpy_size;
        size_t xdata_length = rpc_data_get_length(xdata);

        if (xdata->ro_type != RPC_TYPE_BINARY)
                return (0);

        if (off > xdata_length)
                return (0);

        cpy_size = MIN(length, xdata_length - off);

        memcpy(
            buffer,
            rpc_data_get_bytes_ptr(xdata) + off,
            cpy_size);

        return (cpy_size);
}

inline rpc_object_t
rpc_string_create(const char *string)
{
	union rpc_value val;
	const char *str;

	str = g_strdup(string);
	val.rv_str = g_string_new(str);
	return (rpc_prim_create(RPC_TYPE_STRING, val));
}

inline rpc_object_t
rpc_string_create_with_format(const char *fmt, ...)
{
	va_list ap;
	union rpc_value val;

	va_start(ap, fmt);
	val.rv_str = g_string_new(NULL);
	g_string_vprintf(val.rv_str, fmt, ap);
	va_end(ap);

	return (rpc_prim_create(RPC_TYPE_STRING, val));
}

inline rpc_object_t
rpc_string_create_with_format_and_arguments(const char *fmt, va_list ap)
{
	union rpc_value val;

	val.rv_str = g_string_new(NULL);
	g_string_vprintf(val.rv_str, fmt, ap);
	return (rpc_prim_create(RPC_TYPE_STRING, val));
}

inline size_t
rpc_string_get_length(rpc_object_t xstring)
{

	if (xstring->ro_type != RPC_TYPE_STRING)
		return (0);

	return (xstring->ro_value.rv_str->len);
}

inline const char *
rpc_string_get_string_ptr(rpc_object_t xstring)
{

	if (xstring->ro_type != RPC_TYPE_STRING)
		return (0);

	return (xstring->ro_value.rv_str->str);
}

inline rpc_object_t
rpc_fd_create(int fd)
{
	union rpc_value val;

	val.rv_fd = fd;
	return (rpc_prim_create(RPC_TYPE_FD, val));
}

inline int
rpc_fd_get_value(rpc_object_t xfd)
{

        if (xfd->ro_type != RPC_TYPE_FD)
                return (0);

        return (xfd->ro_value.rv_fd);
}

inline int
rpc_fd_dup(rpc_object_t xfd)
{

        if (xfd->ro_type != RPC_TYPE_FD)
                return (0);

        return (dup(rpc_fd_get_value(xfd)));
}

inline rpc_object_t
rpc_array_create(void)
{
	union rpc_value val;

	val.rv_list = g_array_new(true, true, sizeof(rpc_object_t));
        return (rpc_prim_create(RPC_TYPE_ARRAY, val));
}

inline rpc_object_t
rpc_array_create_ex(const rpc_object_t *objects, size_t count)
{
	rpc_object_t array_object;
	int i;

	array_object = rpc_array_create();
	for (i = 0; i < count; i++)
		rpc_array_append_value(array_object, objects[i]);

	return array_object;
}

inline void
rpc_array_set_value(rpc_object_t array, size_t index, rpc_object_t value)
{
	rpc_array_steal_value(array, index, value);
	rpc_retain(value);
}

inline void
rpc_array_steal_value(rpc_object_t array, size_t index, rpc_object_t value)
{
	rpc_object_t *ro;
	union rpc_value null_value;
	int i;

	if (array->ro_type != RPC_TYPE_ARRAY)
		abort();

	for (i = (int)(index - array->ro_value.rv_list->len); i >= 0; i--) {
		rpc_array_append_value(
		    array,
		    rpc_prim_create(RPC_TYPE_NULL, null_value)
		);
	}

	ro = &g_array_index(array->ro_value.rv_list, rpc_object_t, index);
	rpc_release_impl(*ro);
	*ro = value;
}

inline void
rpc_array_remove_index(rpc_object_t array, size_t index)
{
	if (array->ro_type != RPC_TYPE_ARRAY)
		abort();

	if (rpc_array_get_count(array) >= index)
		abort();

	g_array_remove_index(array->ro_value.rv_list, (guint)index);
}


inline void
rpc_array_append_value(rpc_object_t array, rpc_object_t value)
{

        rpc_array_append_stolen_value(array, value);

	rpc_retain(value);
}

inline void
rpc_array_append_stolen_value(rpc_object_t array, rpc_object_t value)
{

        if (array->ro_type != RPC_TYPE_ARRAY)
                abort();

	g_array_append_val(array->ro_value.rv_list, value);
}

inline rpc_object_t
rpc_array_get_value(rpc_object_t array, size_t index)
{
        if (array->ro_type != RPC_TYPE_ARRAY)
                return (0);

        if (index >= array->ro_value.rv_list->len)
                return (0);

        return (g_array_index(array->ro_value.rv_list, rpc_object_t, index));
}

inline size_t rpc_array_get_count(rpc_object_t array)
{
	if (array->ro_type != RPC_TYPE_ARRAY)
		return (0);

	return (array->ro_value.rv_list->len);
}

inline bool
rpc_array_apply(rpc_object_t array, rpc_array_applier_t applier)
{
	bool flag = false;
	size_t i = 0;

	for (i = 0; i < array->ro_value.rv_list->len; i++) {
		if (!applier(i, g_array_index(array->ro_value.rv_list,
		    rpc_object_t, i))) {
			flag = true;
			break;
		}
	}

	return (flag);
}

inline void
rpc_array_set_bool(rpc_object_t array, size_t index, bool value)
{

	rpc_array_steal_value(array, index, rpc_bool_create(value));
}

inline void
rpc_array_set_int64(rpc_object_t array, size_t index, int64_t value)
{

	rpc_array_steal_value(array, index, rpc_int64_create(value));
}

inline void
rpc_array_set_uint64(rpc_object_t array, size_t index, uint64_t value)
{

	rpc_array_steal_value(array, index, rpc_uint64_create(value));
}

inline void
rpc_array_set_double(rpc_object_t array, size_t index, double value)
{

	rpc_array_steal_value(array, index, rpc_double_create(value));
}

inline void
rpc_array_set_date(rpc_object_t array, size_t index, int64_t value)
{

	rpc_array_steal_value(array, index, rpc_date_create(value));
}

inline void
rpc_array_set_data(rpc_object_t array, size_t index, const void *bytes,
    size_t length)
{

	rpc_array_steal_value(array, index, rpc_data_create(bytes, length, false));
}

inline void
rpc_array_set_string(rpc_object_t array, size_t index,
    const char *value)
{

	rpc_array_steal_value(array, index, rpc_string_create(value));
}

inline void
rpc_array_set_fd(rpc_object_t array, size_t index, int value)
{

	rpc_array_steal_value(array, index, rpc_fd_create(value));
}

inline bool
rpc_array_get_bool(rpc_object_t array, size_t index)
{

	return (rpc_bool_get_value(rpc_array_get_value(array, index)));
}

inline int64_t
rpc_array_get_int64(rpc_object_t array, size_t index)
{

	return (rpc_int64_get_value(rpc_array_get_value(array, index)));
}

inline uint64_t
rpc_array_get_uint64(rpc_object_t array, size_t index)
{

	return (rpc_uint64_get_value(rpc_array_get_value(array, index)));
}

inline double
rpc_array_get_double(rpc_object_t array, size_t index)
{

	return (rpc_double_get_value(rpc_array_get_value(array, index)));
}

inline int64_t
rpc_array_get_date(rpc_object_t array, size_t index)
{

	return (rpc_date_get_value(rpc_array_get_value(array, index)));
}

inline const void *rpc_array_get_data(rpc_object_t array, size_t index,
    size_t *length)
{
        rpc_object_t xdata;

        if ((xdata = rpc_array_get_value(array, index)) == 0)
                return (0);

	if (length != NULL)
		*length = xdata->ro_value.rv_bin.length;

        return rpc_data_get_bytes_ptr(xdata);
}

inline const char *
rpc_array_get_string(rpc_object_t array, size_t index)
{

	return rpc_string_get_string_ptr(rpc_array_get_value(array, index));
}

inline int rpc_array_dup_fd(rpc_object_t array, size_t index)
{

        return (rpc_fd_dup(rpc_array_get_value(array, index)));
}

inline rpc_object_t
rpc_dictionary_create(void)
{
	union rpc_value val;

	val.rv_dict = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	    (GDestroyNotify)rpc_release_impl);

	return (rpc_prim_create(RPC_TYPE_DICTIONARY, val));
}

inline rpc_object_t
rpc_dictionary_create_ex(const char * const *keys, const rpc_object_t *values,
    size_t count, bool steal)
{
	rpc_object_t object;
	int i;
	void (*setter_fn)(rpc_object_t, const char *, rpc_object_t);

	setter_fn = steal ? &rpc_dictionary_steal_value : &rpc_dictionary_set_value;
	object = rpc_dictionary_create();

	for (i = 0; i < count; i++)
		setter_fn(object, keys[i], values[i]);

	return (object);
}

inline void
rpc_dictionary_set_value(rpc_object_t dictionary, const char *key,
    rpc_object_t value)
{

	rpc_dictionary_steal_value(dictionary, key, value);

	rpc_retain(value);
}

inline void
rpc_dictionary_steal_value(rpc_object_t dictionary, const char *key,
    rpc_object_t value)
{

	if (dictionary->ro_type != RPC_TYPE_DICTIONARY)
		abort();

	g_hash_table_insert(dictionary->ro_value.rv_dict, (gpointer)key, value);
}

inline void
rpc_dictionary_remove_key(rpc_object_t dictionary, const char *key)
{
	if (dictionary->ro_type != RPC_TYPE_DICTIONARY)
		abort();

	g_hash_table_remove(dictionary->ro_value.rv_dict, key);
}

inline rpc_object_t
rpc_dictionary_get_value(rpc_object_t dictionary,
    const char *key)
{

	return ((rpc_object_t)g_hash_table_lookup(
	    dictionary->ro_value.rv_dict, key));
}

inline size_t
rpc_dictionary_get_count(rpc_object_t dictionary)
{

	return ((size_t)g_hash_table_size(dictionary->ro_value.rv_dict));
}

inline bool
rpc_dictionary_apply(rpc_object_t dictionary, rpc_dictionary_applier_t applier)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, dictionary->ro_value.rv_dict);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (!applier((const char *)key, (rpc_object_t)value))
			break;
	}

	return (true);
}

inline bool
rpc_dictionary_has_key(rpc_object_t dictionary, const char *key)
{

	return (g_hash_table_lookup(dictionary->ro_value.rv_dict, key) != NULL);
}

inline void
rpc_dictionary_set_bool(rpc_object_t dictionary, const char *key, bool value)
{

	rpc_dictionary_steal_value(dictionary, key, rpc_bool_create(value));
}

inline void
rpc_dictionary_set_int64(rpc_object_t dictionary, const char *key,
    int64_t value)
{

	rpc_dictionary_steal_value(dictionary, key, rpc_int64_create(value));
}

inline void
rpc_dictionary_set_uint64(rpc_object_t dictionary, const char *key,
    uint64_t value)
{

	rpc_dictionary_steal_value(dictionary, key, rpc_uint64_create(value));
}

inline void
rpc_dictionary_set_double(rpc_object_t dictionary, const char *key,
    double value)
{

	rpc_dictionary_steal_value(dictionary, key, rpc_double_create(value));
}

inline void
rpc_dictionary_set_date(rpc_object_t dictionary, const char *key,
    int64_t value)
{

	rpc_dictionary_steal_value(dictionary, key, rpc_date_create(value));
}

inline void
rpc_dictionary_set_data(rpc_object_t dictionary, const char *key,
    const void *value, size_t length)
{

	rpc_dictionary_steal_value(dictionary, key, rpc_data_create(value, length, false));
}

inline void
rpc_dictionary_set_string(rpc_object_t dictionary, const char *key,
    const char *value)
{

	rpc_dictionary_steal_value(dictionary, key, rpc_string_create(value));
}

inline void
rpc_dictionary_set_fd(rpc_object_t dictionary, const char *key, int value)
{

	rpc_dictionary_steal_value(dictionary, key, rpc_fd_create(value));
}

inline bool
rpc_dictionary_get_bool(rpc_object_t dictionary, const char *key)
{
	rpc_object_t xbool;

	xbool = rpc_dictionary_get_value(dictionary, key);
	return ((xbool != NULL) ? rpc_bool_get_value(xbool) : false);
}

inline int64_t
rpc_dictionary_get_int64(rpc_object_t dictionary, const char *key)
{
	rpc_object_t xint;

	xint = rpc_dictionary_get_value(dictionary, key);
	return ((xint != NULL) ? rpc_int64_get_value(xint) : 0);
}

inline uint64_t
rpc_dictionary_get_uint64(rpc_object_t dictionary, const char *key)
{
	rpc_object_t xuint;

	xuint = rpc_dictionary_get_value(dictionary, key);
	return ((xuint != NULL) ? rpc_uint64_get_value(xuint) : 0);
}

inline double
rpc_dictionary_get_double(rpc_object_t dictionary, const char *key)
{
	rpc_object_t xdouble;

	xdouble = rpc_dictionary_get_value(dictionary, key);
	return ((xdouble != NULL) ? rpc_double_get_value(xdouble) : 0);
}

inline int64_t
rpc_dictionary_get_date(rpc_object_t dictionary, const char *key)
{
	rpc_object_t xdate;

	xdate = rpc_dictionary_get_value(dictionary, key);
	return ((xdate != NULL) ? rpc_date_get_value(xdate) : false);
}

inline const void *
rpc_dictionary_get_data(rpc_object_t dictionary, const char *key,
    size_t *length)
{
	rpc_object_t xdata;

	if ((xdata = rpc_dictionary_get_value(dictionary, key)) == NULL)
		return (NULL);

	if (length != NULL)
		*length = xdata->ro_value.rv_bin.length;

	return rpc_data_get_bytes_ptr(xdata);
}

inline const char *
rpc_dictionary_get_string(rpc_object_t dictionary, const char *key)
{
	rpc_object_t xstring;

	xstring = rpc_dictionary_get_value(dictionary, key);
	return ((xstring != NULL) ? rpc_string_get_string_ptr(xstring) : NULL);
}

inline int
rpc_dictionary_get_fd(rpc_object_t dictionary, const char *key)
{
	rpc_object_t xfd;

	xfd = rpc_dictionary_get_value(dictionary, key);
	return ((xfd != NULL) ? rpc_fd_get_value(xfd) : 0);
}

inline int
rpc_dictionary_dup_fd(rpc_object_t dictionary, const char *key)
{
	rpc_object_t xfd;

	xfd = rpc_dictionary_get_value(dictionary, key);
	return (xfd != NULL ? rpc_fd_dup(xfd) : 0);
}
