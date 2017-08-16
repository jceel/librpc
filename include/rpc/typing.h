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
 */

#ifndef LIBRPCT_H
#define LIBRPCT_H

#include <rpc/object.h>

#define	RPCT_CLASS_FIELD	"%class"
#define	RPCT_TYPE_FIELD		"%type"
#define	RPCT_VALUE_FIELD	"%value"

struct rpct_type;
typedef struct rpct_type *rpct_type_t;

struct rpct_member;
typedef struct rpct_member *rpct_member_t;

struct rpct_instance;
typedef struct rpct_instance *rpct_instance_t;

typedef enum {
	RPC_TYPING_STRUCT,
	RPC_TYPING_UNION,
	RPC_TYPING_ENUM,
	RPC_TYPING_TYPEDEF,
	RPC_TYPING_SPECIALIZATION,
	RPC_TYPING_BUILTIN
} rpct_class_t;

struct rpct_error
{
	
};

typedef bool (^rpct_type_applier_t)(rpct_type_t);
typedef bool (^rpct_member_applier_t)(rpct_member_t);

#define	RPCT_TYPE_APPLIER(_fn, _arg)					\
	^(rpct_type_t _type) {						\
		return ((bool)_fn(_arg, _type));			\
	}

#define	RPCT_MEMBER_APPLIER(_fn, _arg)					\
	^(rpct_member_t _member) {					\
		return ((bool)_fn(_arg, _member));			\
	}

/**
 * Initializes RPC type system
 *
 * @return 0 on success, -1 on error
 */
int rpct_init(void);
int rpct_load_types(const char *path);
int rpct_load_types_stream(int fd);

const char *rpct_type_get_name(rpct_type_t type);
const char *rpct_type_get_realm(rpct_type_t type);
const char *rpct_type_get_description(rpct_type_t type);
rpct_type_t rpct_type_get_parent(rpct_type_t type);
bool rpct_type_is_generic(rpct_type_t type);

const char *rpct_member_get_name(rpct_member_t member);
const char *rpct_member_get_description(rpct_member_t member);

void rpct_types_apply(rpct_type_applier_t applier);
void rpct_members_apply(rpct_type_t type, rpct_member_applier_t applier);

rpct_instance_t rpct_new(const char *type, ...);
rpct_instance_t rpct_unpack(rpc_object_t obj);
rpc_object_t rpct_pack(rpct_instance_t instance);
void rpct_free(rpct_instance_t instance);

rpct_class_t rpct_get_class(rpct_instance_t instance);
const char *rpct_get_type(rpct_instance_t instance);
const char *rpct_get_value(rpct_instance_t instance);
void rpct_set_value(rpct_instance_t, const char *value);

rpc_object_t rpct_get_dict(rpct_instance_t instance);

#define	rpct_get(_instance, _type, _key)				\
	rpc_dictionary_get_##_type(rpct_get_dict(_instance), _key)

#define	rpct_set()

#endif //LIBRPCT_H
