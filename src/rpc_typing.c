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

#include <glib.h>
#include <yaml.h>
#include <rpc/object.h>
#include <rpc/serializer.h>
#include <rpc/typing.h>
#include "internal.h"

static const char *builtin_types[] = {
	"null",
	"bool",
	"uint64",
	"int64",
	"double",
	"date",
	"string",
	"binray",
	"fd",
	"dictionary",
	"array",
	"shmem",
	"error",
	"any",
	NULL
};

static int rpct_read_meta(struct rpct_file *file, rpc_object_t obj);
static struct rpct_member *rpct_read_member(const char *decl, rpc_object_t obj,
    struct rpct_type *type);
static struct rpct_typei *rpct_instantiate_type(const char *decl,
    const char *realm);
static inline bool rpct_type_is_fully_specialized(struct rpct_typei *inst);
static char *rpct_canonical_type(struct rpct_typei *typei);
static int rpct_read_type(const char *realm, const char *decl,
    rpc_object_t obj);
static int rpct_read_func(const char *realm, const char *decl,
    rpc_object_t obj);
static int rpct_read_file(const char *path);
static inline bool rpct_realms_apply(rpct_realm_applier_t applier);
static int rpct_validate_args(struct rpct_function *func, rpc_object_t args);
static int rpct_validate_return(struct rpct_function *func,
    rpc_object_t result);
static int rpct_parse_type(const char *decl, GPtrArray *variables);
static inline int rpct_find_or_load(const char *realm, const char *decl,
    rpc_object_t obj);
static void rpct_constraint_free(struct rpct_constraint *constraint);

static struct rpct_context *context = NULL;

rpc_object_t
rpct_new(const char *decl, const char *realm)
{
	rpc_object_t inst;
	struct rpct_typei *typei;

	typei = rpct_instantiate_type(decl, realm);
	if (typei == NULL)
		return (NULL);

	if (typei->type->clazz == RPC_TYPING_BUILTIN) {
		rpct_typei_free(typei);
		return (NULL);
	}

	inst = rpc_dictionary_create();
	inst->ro_typei = typei;

	return (inst);
}

rpct_class_t
rpct_get_class(rpc_object_t instance)
{

	if ((instance == NULL) || (instance->ro_typei == NULL))
		return (NULL);

	return (instance->ro_typei->type->clazz);
}

char *
rpct_get_type(rpc_object_t instance)
{

	if ((instance == NULL) || (instance->ro_typei == NULL))
		return (NULL);

	return (rpct_canonical_type(instance->ro_typei));
}

const char *
rpct_get_value(rpc_object_t instance)
{

	if ((instance == NULL) || (instance->ro_typei == NULL))
		return (NULL);

	if (rpc_get_type(instance) != RPC_TYPE_DICTIONARY)
		return (NULL);

	return (rpc_dictionary_get_string(instance, RPCT_VALUE_FIELD));
}

void
rpct_struct_set_value(rpc_object_t instance, const char *value)
{

	if ((instance == NULL) || (instance->ro_typei == NULL))
		return;

	if (rpc_get_type(instance) != RPC_TYPE_DICTIONARY)
		return;

	rpc_dictionary_set_string(instance, RPCT_VALUE_FIELD, value);
}

static inline struct rpct_realm *
rpct_find_realm(const char *realm)
{

	return (g_hash_table_lookup(context->realms, realm));
}

static rpct_type_t
rpct_find_type(const char *realm_name, const char *name)
{
	rpct_realm_t realm;

	realm = rpct_find_realm(realm_name);
	if (realm == NULL)
		realm = rpct_find_realm("*");

	if (realm == NULL)
		return (NULL);

	return (g_hash_table_lookup(realm->types, name));

}

static int
rpct_read_meta(struct rpct_file *file, rpc_object_t obj)
{
	int ret;

	ret = rpc_object_unpack(obj, "{ssi}",
	    "version", &file->version,
	    "realm", &file->realm,
	    "description", &file->description);

	return (ret > 0 ? 0 : -1);
}

static struct rpct_member *
rpct_read_member(const char *decl, rpc_object_t obj,
    struct rpct_type *type)
{
	struct rpct_member *member;
	const char *typedecl;
	const char *description;

	rpc_object_unpack(obj, "{ss}", obj,
	    "type", &typedecl,
	    "description", &description);

	member = g_malloc0(sizeof(*member));
	member->name = g_strdup(decl);
	member->description = g_strdup(description);
	member->origin = type;
	member->type = rpct_instantiate_type(typedecl, type->realm);
	member->constraints = g_hash_table_new_full(g_str_hash, g_str_equal,
	    g_free, (GDestroyNotify)rpct_constraint_free);

	return (member);
}

static struct rpct_typei *
rpct_instantiate_type(const char *decl, const char *realm)
{
	GError *err = NULL;
	GRegex *regex;
	GMatchInfo *match = NULL;
	GPtrArray *splitvars = NULL;
	struct rpct_type *type;
	struct rpct_typei *ret = NULL;
	struct rpct_typei *subtype;
	char *decltype = NULL;
	char *declvars = NULL;

	regex = g_regex_new(INSTANCE_REGEX, 0, G_REGEX_MATCH_NOTEMPTY, &err);
	if (err != NULL)
		goto error;

	if (!g_regex_match(regex, decl, 0, &match))
		goto error;

	if (g_match_info_get_match_count(match) < 1)
		goto error;

	decltype = g_match_info_fetch(match, 1);

	type = rpct_find_type(realm, decltype);
	if (type == NULL)
		goto error;

	ret = g_malloc0(sizeof(*ret));
	ret->type = type;
	ret->specializations = g_ptr_array_new();
	ret->constraints = type->constraints;

	if (type->generic) {
		declvars = g_match_info_fetch(match, 3);
		if (declvars == NULL)
			goto error;

		splitvars = g_ptr_array_new();

		rpct_parse_type(declvars, splitvars);
		if (splitvars->len != type->generic_vars->len)
			goto error;

		for (int i = 0; i < splitvars->len; i++) {
			subtype = rpct_instantiate_type(
			    g_ptr_array_index(splitvars, i), realm);
			g_ptr_array_add(ret->specializations, subtype);
		}
	}

	goto done;

error:	if (ret != NULL) {
		rpct_typei_free(ret);
		ret = NULL;
	}

done:	g_regex_unref(regex);

	if (err != NULL)
		g_error_free(err);

	if (match != NULL)
		g_match_info_free(match);

	if (splitvars != NULL)
		g_ptr_array_free(splitvars, true);

	if (decltype != NULL)
		g_free(decltype);

	if (declvars != NULL)
		g_free(declvars);

	return (ret);
}

static void
rpct_type_free(rpct_type_t type)
{

	g_free(type->realm);
	g_free(type->name);
	g_free(type->description);
	g_ptr_array_free(type->generic_vars, true);
	g_hash_table_unref(type->members);
	g_hash_table_unref(type->constraints);
	g_free(type);
}

static void
rpct_file_free(struct rpct_file *file)
{

	g_free(file->path);
	g_free(file->realm);
	g_free(file->description);
	g_hash_table_unref(file->types);
	g_free(file);
}

static void
rpct_realm_free(rpct_realm_t realm)
{

	g_free(realm->name);
	g_hash_table_unref(realm->types);
	g_hash_table_unref(realm->functions);
	g_free(realm);
}

static void
rpct_function_free(struct rpct_function *function)
{

	g_free(function->name);
	g_free(function->realm);
	g_free(function->description);
	g_hash_table_unref(function->arguments);
	if (function->result != NULL)
		rpct_typei_free(function->result);
	g_free(function);
}

static void
rpct_member_free(struct rpct_member *member)
{

	g_free(member->name);
	g_free(member->description);
	rpct_typei_free(member->type);
	g_hash_table_unref(member->constraints);
	g_free(member);
}

static void
rpct_constraint_free(struct rpct_constraint *constraint)
{

	/* XXX  Implement constraints*/
	g_free(constraint);
}

static inline bool
rpct_type_is_fully_specialized(struct rpct_typei *inst)
{
	struct rpct_typei *s;

	if (inst->specializations == NULL)
		return (true);

	for (guint i = 0; i < inst->specializations->len; i++) {
		s = g_ptr_array_index(inst->specializations, i);
		if (s == NULL || s->proxy)
			return (false);
	}

	return (true);
}

static inline bool
rpct_type_is_compatible(struct rpct_typei *decl, struct rpct_typei *type)
{

	/* Types from different realms are always incompatible */
	/* XXX global realm */
	if (g_strcmp0(decl->type->realm, type->type->realm) != 0)
		return (false);


}

static int
rpct_parse_type(const char *decl, GPtrArray *variables)
{
	int istart = 0;
	int nesting = 0;
	int groups = 0;

	for (int i = 0; i < strlen(decl); i++) {
		switch (decl[i]) {
		case '<':
			nesting++;
			break;

		case '>':
			nesting--;
			break;

		case ',':
			if (nesting == 0) {
				groups++;
				g_ptr_array_add(variables, g_strndup(
			 	   &decl[istart], (gsize)(i - istart)));
			}
			break;

		default:
			continue;
		}
	}

	return (groups);
}

static char *
rpct_canonical_type(struct rpct_typei *typei)
{
	GString *ret = g_string_new(typei->type->name);

	if (!typei->type->generic)
		return (g_string_free(ret, false));

	g_string_append(ret, "<");

	for (guint i = 0; i < typei->specializations->len; i++) {
		struct rpct_typei *subtype;
		char *substr;

		subtype = g_ptr_array_index(typei->specializations, i);
		substr = rpct_canonical_type(subtype);
		g_string_append(ret, substr);
		g_free(substr);

		if (i < typei->specializations->len)
			g_string_append(ret, ",");
	}

	g_string_append(ret, ">");
	return (g_string_free(ret, false));
}

static inline int
rpct_find_or_load(const char *realm, const char *decl, rpc_object_t obj)
{
	GError *err = NULL;
	GRegex *regex;
	GMatchInfo *match = NULL;
	GPtrArray *splitvars = NULL;
	struct rpct_type *type;
	char *decltype = NULL;
	char *declvars = NULL;
	int ret = -1;

	regex = g_regex_new(INSTANCE_REGEX, 0, G_REGEX_MATCH_NOTEMPTY, &err);
	if (err != NULL)
		goto done;

	if (!g_regex_match(regex, decl, 0, &match))
		goto done;

	if (g_match_info_get_match_count(match) < 1)
		goto done;

	decltype = g_match_info_fetch(match, 1);

	type = rpct_find_type(realm, decltype);

	if (type == NULL) {
		if (rpct_read_type(realm, decltype, obj) != 0)
			goto done;

		type = rpct_find_type(realm, decltype);
	}

	if (type->generic) {
		declvars = g_match_info_fetch(match, 3);

		if (!g_strcmp0(declvars, "")) {
			splitvars = g_ptr_array_new();

			rpct_parse_type(declvars, splitvars);
			if (splitvars->len != type->generic_vars->len)
				goto done;

			for (int i = 0; i < splitvars->len; i++) {
				if (rpct_find_or_load(realm,
				    g_ptr_array_index(splitvars, i), obj) != 0)
					goto done;
			}
		}
	}

	ret = 0;

done:	g_regex_unref(regex);

	if (err != NULL)
		g_error_free(err);

	if (match != NULL)
		g_match_info_free(match);

	if (splitvars != NULL)
		g_ptr_array_free(splitvars, true);

	if (decltype != NULL)
		g_free(decltype);

	if (declvars != NULL)
		g_free(declvars);

	return (ret);
}

static int
rpct_read_type(const char *realm, const char *decl, rpc_object_t obj)
{
	struct rpct_type *type;
	struct rpct_type *parent = NULL;
	const char *inherits = NULL;
	const char *description = NULL;
	const char *decltype, *declname, *declvars;
	GError *err = NULL;
	GRegex *regex;
	GMatchInfo *match;
	rpc_object_t members = NULL;
	rpc_object_t decl_obj = NULL;
	rpct_realm_t realm_obj;

	decl_obj = rpc_dictionary_get_value(obj, decl);

	rpc_object_unpack(decl_obj, "{ssv}",
	    "inherits", &inherits,
	    "description", &description,
	    "members", &members);

	if (inherits != NULL) {
		if (rpct_find_or_load(realm, inherits, obj) != 0)
			return (-1);

		parent = rpct_find_type(realm, inherits);
		if (parent == NULL)
			return (-1);
	}

	regex = g_regex_new(TYPE_REGEX, 0, G_REGEX_MATCH_NOTEMPTY, &err);
	if (err != NULL) {
		g_error_free(err);
		return (-1);
	}

	if (!g_regex_match(regex, decl, 0, &match)) {
		g_regex_unref(regex);
		return (-1);
	}

	if (g_match_info_get_match_count(match) < 2) {
		g_regex_unref(regex);
		g_match_info_free(match);
		return (-1);
	}

	decltype = g_match_info_fetch(match, 1);
	declname = g_match_info_fetch(match, 2);
	declvars = g_match_info_fetch(match, 3);

	type = g_malloc0(sizeof(*type));
	type->name = g_strdup(declname);
	type->realm = g_strdup(realm);
	type->parent = parent;
	type->members = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	    (GDestroyNotify)rpct_member_free);
	type->constraints = g_hash_table_new_full(g_str_hash, g_str_equal,
	    g_free, (GDestroyNotify)rpct_constraint_free);
	type->description = g_strdup(description);
	type->generic_vars = g_ptr_array_new_with_free_func(g_free);

	if (g_strcmp0(decltype, "struct") == 0)
		type->clazz = RPC_TYPING_STRUCT;

	else if (g_strcmp0(decltype, "union") == 0)
		type->clazz = RPC_TYPING_UNION;

	else if (g_strcmp0(decltype, "enum") == 0)
		type->clazz = RPC_TYPING_ENUM;

	else if (g_strcmp0(decltype, "type") == 0)
		type->clazz = RPC_TYPING_TYPEDEF;

	else
		g_assert_not_reached();

	if (declvars)
		rpct_parse_type(declvars, type->generic_vars);

	/* Pull inherited members (if any) */
	if (parent != NULL) {
		GHashTableIter iter;
		gpointer key;
		gpointer value;

		g_hash_table_iter_init(&iter, parent->members);
		while (g_hash_table_iter_next(&iter, &key, &value))
			g_hash_table_insert(type->members, key, value);
	}

	/* Read member list */
	if (members != NULL) {
		if (rpc_dictionary_apply(members, ^(const char *key,
		    rpc_object_t value) {
			struct rpct_member *m;

			m = rpct_read_member(key, value, type);
			if (m != NULL) {
				g_hash_table_insert(type->members,
				    g_strdup(key), m);
				return ((bool)true);
			}

			return ((bool)false);
		})) {
			rpct_type_free(type);
			return (-1);
		}
	}

	/*XXX Add constraints support */

	realm_obj = rpct_find_realm(realm);
	g_hash_table_insert(realm_obj->types, g_strdup(declname), type);
	return (0);
}

static int
rpct_read_func(const char *realm, const char *decl, rpc_object_t obj)
{
	int ret = -1;
	struct rpct_function *func = NULL;
	GError *err = NULL;
	GRegex *regex = NULL;
	GMatchInfo *match = NULL;
	const char *name;
	const char *description;
	const char *returns_type;
	rpc_object_t decl_obj;
	rpc_object_t args;
	rpc_object_t returns;
	rpct_realm_t realm_obj;

	decl_obj = rpc_dictionary_get_value(obj, decl);

	rpc_object_unpack(decl_obj, "{ssvv}",
	    "description", &description,
	    "arguments", &args,
	    "return", &returns);

	regex = g_regex_new(FUNC_REGEX, 0, G_REGEX_MATCH_NOTEMPTY, &err);
	if (err != NULL)
		goto error;

	if (!g_regex_match(regex, decl, 0, &match))
		goto error;

	if (g_match_info_get_match_count(match) < 1)
		goto error;

	name = g_match_info_fetch(match, 1);
	func = g_malloc0(sizeof(*func));
	func->arguments = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	    (GDestroyNotify)rpct_typei_free);

	if (args != NULL) {
		if (rpc_array_apply(args, ^(size_t idx, rpc_object_t i) {
			const char *arg_name;
			const char* arg_type;
			struct rpct_typei *arg_inst;

			arg_name = rpc_dictionary_get_string(i, "name");
			if (arg_name == NULL)
				return ((bool)false);

			arg_type = rpc_dictionary_get_string(i, "type");
			if (arg_type == NULL)
				return ((bool)false);

			if (rpct_find_or_load(realm, arg_type, obj) != 0)
				return ((bool)false);

			arg_inst = rpct_instantiate_type(arg_type, realm);
			if (arg_inst == NULL)
				return ((bool)false);

			g_hash_table_insert(func->arguments, g_strdup(arg_name),
			    arg_inst);

			return ((bool)true);
		}))
			goto error;
	}

	if (returns != NULL) {
		returns_type = rpc_string_get_string_ptr(returns);
		if (rpct_find_or_load(realm, returns_type, obj) != 0)
			goto error;

		func->result = rpct_instantiate_type(returns_type, realm);
		if (func->result == NULL)
			goto error;
	}

	func->name = g_strdup(name);
	func->realm = g_strdup(realm);
	func->description = g_strdup(description);

	realm_obj = rpct_find_realm(realm);
	g_hash_table_insert(realm_obj->functions, g_strdup(name), func);
	ret = 0;
	goto done;

error:	if (func != NULL) {
		g_hash_table_unref(func->arguments);
		g_free(func);
	}

done:	if (err != NULL)
		g_error_free(err);

	if (regex != NULL)
		g_regex_unref(regex);

	if (match != NULL)
		g_match_info_free(match);

	return (ret);
}

static int
rpct_read_file(const char *path)
{
	struct rpct_file *file;
	struct rpct_realm *realm;
	char *contents;
	size_t length;
	rpc_object_t obj;
	GError *err = NULL;
	int ret = 0;

	if (!g_file_get_contents(path, &contents, &length, &err))
		return (-1);

	obj = rpc_serializer_load("yaml", contents, length);
	g_free(contents);

	if (obj == NULL)
		return (-1);

	file = g_malloc0(sizeof(*file));
	rpct_read_meta(file, rpc_dictionary_get_value(obj, "meta"));
	file->path = g_strdup(path);
	file->types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	    NULL);

	if (rpct_find_realm(file->realm) == NULL) {
		realm = g_malloc0(sizeof(*realm));
		realm->name = g_strdup(file->realm);
		realm->types= g_hash_table_new_full(g_str_hash, g_str_equal,
		    g_free, (GDestroyNotify)rpct_type_free);
		realm->functions= g_hash_table_new_full(g_str_hash, g_str_equal,
		    g_free, (GDestroyNotify)rpct_function_free);
		g_hash_table_insert(context->realms, g_strdup(realm->name),
		    realm);
	}

	if (rpc_dictionary_apply(obj, ^(const char *key,
	    rpc_object_t v __unused) {
		if (g_strcmp0(key, "meta") == 0)
			return ((bool)true);

		if (g_str_has_prefix(key, "function")) {
			if (rpct_read_func(file->realm, key, obj) != 0)
				return ((bool)false);
			return ((bool)true);
		}

		if (rpct_find_or_load(file->realm, key, obj) != 0)
			return ((bool)false);
		return ((bool)true);
	}))
		ret = -1;

	rpc_release(obj);
	return (ret);
}

static int
rpct_validate_obj(struct rpct_typei *typei, rpc_object_t obj)
{

}

static int
rpct_validate_args(struct rpct_function *func, rpc_object_t args)
{

	rpc_array_apply(args, ^(size_t idx, rpc_object_t i) {
		return ((bool)true);
	});
}

static int
rpct_validate_return(struct rpct_function *func, rpc_object_t result)
{

}

static inline bool
rpct_realms_apply(rpct_realm_applier_t applier)
{
	GHashTableIter iter;
	rpct_realm_t value;
	char *key;
	bool flag = false;

	g_hash_table_iter_init(&iter, context->realms);
	while (g_hash_table_iter_next(&iter, (gpointer *)&key,
	    (gpointer *)&value))
		if (!applier(value)) {
			flag = true;
			break;
		}

	return (flag);
}

int
rpct_init(void)
{
	rpct_type_t type;
	rpct_realm_t realm;
	const char **b;

	context = g_malloc0(sizeof(*context));
	context->files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	    (GDestroyNotify)rpct_file_free);
	context->realms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	    (GDestroyNotify)rpct_realm_free);

	realm = g_malloc(sizeof(*realm));
	realm->name = g_strdup("*");
	realm->types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	    (GDestroyNotify)rpct_type_free);

	for (b = builtin_types; *b != NULL; b++) {
		type = g_malloc0(sizeof(*type));
		type->name = g_strdup(*b);
		type->clazz = RPC_TYPING_BUILTIN;
		type->realm = g_strdup(realm->name);
		type->members = g_hash_table_new_full(g_str_hash, g_str_equal,
		    g_free, (GDestroyNotify)rpct_member_free);
		type->constraints = g_hash_table_new_full(g_str_hash,
		    g_str_equal, g_free, (GDestroyNotify)rpct_constraint_free);
		type->description = g_strdup_printf("builtin %s type", *b);
		type->generic_vars = g_ptr_array_new();
	}
	return (0);
}

void
rpct_free(void)
{

	g_hash_table_unref(context->files);
	g_hash_table_unref(context->realms);
	g_free(context);
}

void
rpct_typei_free(struct rpct_typei *inst)
{

	if (inst->specializations != NULL) {
		for (int i = 0; i < inst->specializations->len; i++) {
			rpct_typei_free(
			    g_ptr_array_index(inst->specializations, i));
		}
	}
	g_free(inst);
}

int
rpct_load_types(const char *path)
{

	return (rpct_read_file(path));
}

const char *
rpct_type_get_name(rpct_type_t type)
{

	return (type->name);
}

const char *
rpct_type_get_realm(rpct_type_t type)
{

	return (type->realm);
}

const char *
rpct_type_get_description(rpct_type_t type)
{

	return (type->description);
}

rpct_type_t
rpct_type_get_parent(rpct_type_t type)
{

	return (type->parent);
}

bool
rpct_type_is_generic(rpct_type_t type)
{

	return (type->generic);
}

const char *
rpct_member_get_name(rpct_member_t member)
{

	return (member->name);
}

const char *
rpct_member_get_description(rpct_member_t member)
{

	return (member->description);
}

bool
rpct_types_apply(rpct_type_applier_t applier)
{

	return (rpct_realms_apply(^(rpct_realm_t realm) {
		GHashTableIter iter;
		char *key;
		rpct_type_t value;

		g_hash_table_iter_init(&iter, realm->types);
		while (g_hash_table_iter_next(&iter, (gpointer *)&key,
		    (gpointer *)&value))
			if (!applier(value))
				return ((bool)false);

		return ((bool)true);
	}));
}

bool
rpct_members_apply(rpct_type_t type, rpct_member_applier_t applier)
{
	GHashTableIter iter;
	char *key;
	struct rpct_member *value;
	bool flag = false;

	g_hash_table_iter_init(&iter, type->members);
	while (g_hash_table_iter_next(&iter, (gpointer *)&key,
	    (gpointer *)&value))
		if (!applier(value)) {
			flag = true;
			break;
		}

	return (flag);
}
