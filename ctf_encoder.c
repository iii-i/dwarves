/*
  Copyright (C) 2009 Red Hat Inc.
  Copyright (C) 2009 Arnaldo Carvalho de Melo <acme@redhat.com>

  This program is free software; you can redistribute it and/or modify it
  under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.
*/

#include "dwarves.h"
#include "libctf.h"
#include "ctf.h"
#include "hash.h"
#include "elf_symtab.h"

static int tag__check_id_drift(const struct tag *self,
			       uint16_t core_id, uint16_t ctf_id)
{
	if (ctf_id != core_id) {
		fprintf(stderr, "%s: %s id drift, core: %u, libctf: %d\n",
			__func__, dwarf_tag_name(self->tag), core_id, ctf_id);
		return -1;
	}
	return 0;
}

static int dwarf_to_ctf_type(uint16_t tag)
{
	switch (tag) {
	case DW_TAG_const_type:		return CTF_TYPE_KIND_CONST;
	case DW_TAG_pointer_type:	return CTF_TYPE_KIND_PTR;
	case DW_TAG_restrict_type:	return CTF_TYPE_KIND_RESTRICT;
	case DW_TAG_volatile_type:	return CTF_TYPE_KIND_VOLATILE;
	case DW_TAG_class_type:
	case DW_TAG_structure_type:	return CTF_TYPE_KIND_STR;
	case DW_TAG_union_type:		return CTF_TYPE_KIND_UNION;
	}
	return 0xffff;
}

static int base_type__encode(struct tag *self, uint16_t core_id,
			     struct ctf *ctf)
{
	struct base_type *bt = tag__base_type(self);
	int ctf_id = ctf__add_base_type(ctf, bt->name, bt->bit_size);

	if (ctf_id < 0 || tag__check_id_drift(self, core_id, ctf_id))
		return -1;

	return 0;
}

static int pointer_type__encode(struct tag *self, uint16_t core_id,
				struct ctf *ctf)
{
	int ctf_id = ctf__add_short_type(ctf, dwarf_to_ctf_type(self->tag),
					 self->type, 0);

	if (ctf_id < 0 || tag__check_id_drift(self, core_id, ctf_id))
		return -1;

	return 0;
}

static int typedef__encode(struct tag *self, uint16_t core_id, struct ctf *ctf)
{
	int ctf_id = ctf__add_short_type(ctf, CTF_TYPE_KIND_TYPDEF, self->type,
					 tag__namespace(self)->name);

	if (ctf_id < 0 || tag__check_id_drift(self, core_id, ctf_id))
		return -1;

	return 0;
}

static int fwd_decl__encode(struct tag *self, uint16_t core_id, struct ctf *ctf)
{
	int ctf_id = ctf__add_fwd_decl(ctf, tag__namespace(self)->name);

	if (ctf_id < 0 || tag__check_id_drift(self, core_id, ctf_id))
		return -1;

	return 0;
}

static int structure_type__encode(struct tag *self, uint16_t core_id,
				  struct ctf *ctf)
{
	struct type *type = tag__type(self);
	int64_t position;
	int ctf_id = ctf__add_struct(ctf, dwarf_to_ctf_type(self->tag),
				     type->namespace.name, type->size,
				     type->nr_members, &position);

	if (ctf_id < 0 || tag__check_id_drift(self, core_id, ctf_id))
		return -1;

	const bool is_short = type->size < CTF_SHORT_MEMBER_LIMIT;
	struct class_member *pos;
	type__for_each_member(type, pos) {
		if (is_short)
			ctf__add_short_member(ctf, pos->name, pos->tag.type,
					      pos->bit_offset, &position);
		else
			ctf__add_full_member(ctf, pos->name, pos->tag.type,
					     pos->bit_offset, &position);
	}

	return 0;
}

static uint32_t array_type__nelems(struct tag *self)
{
	int i;
	uint32_t nelem = 1;
	struct array_type *array = tag__array_type(self);

	for (i = array->dimensions - 1; i >= 0; --i)
		nelem *= array->nr_entries[i];

	return nelem;
}

static int array_type__encode(struct tag *self, uint16_t core_id,
			      struct ctf *ctf)
{
	const uint32_t nelems = array_type__nelems(self);
	int ctf_id = ctf__add_array(ctf, self->type, 0, nelems);

	if (ctf_id < 0 || tag__check_id_drift(self, core_id, ctf_id))
		return -1;

	return 0;
}

static int subroutine_type__encode(struct tag *self, uint16_t core_id,
				   struct ctf *ctf)
{
	struct parameter *pos;
	int64_t position;
	struct ftype *ftype = tag__ftype(self);
	int ctf_id = ctf__add_function_type(ctf, self->type, ftype->nr_parms,
					    ftype->unspec_parms, &position);

	if (ctf_id < 0 || tag__check_id_drift(self, core_id, ctf_id))
		return -1;

	ftype__for_each_parameter(ftype, pos)
		ctf__add_parameter(ctf, pos->tag.type, &position);

	return 0;
}

static int enumeration_type__encode(struct tag *self, uint16_t core_id,
				    struct ctf *ctf)
{
	struct type *etype = tag__type(self);
	int64_t position;
	int ctf_id = ctf__add_enumeration_type(ctf, etype->namespace.name,
					       etype->size, etype->nr_members,
					       &position);

	if (ctf_id < 0 || tag__check_id_drift(self, core_id, ctf_id))
		return -1;

	struct enumerator *pos;
	type__for_each_enumerator(etype, pos)
		ctf__add_enumerator(ctf, pos->name, pos->value, &position);

	return 0;
}

static void tag__encode_ctf(struct tag *self, uint16_t core_id, struct ctf *ctf)
{
	switch (self->tag) {
	case DW_TAG_base_type:
		base_type__encode(self, core_id, ctf);
		break;
	case DW_TAG_const_type:
	case DW_TAG_pointer_type:
	case DW_TAG_restrict_type:
	case DW_TAG_volatile_type:
		pointer_type__encode(self, core_id, ctf);
		break;
	case DW_TAG_typedef:
		typedef__encode(self, core_id, ctf);
		break;
	case DW_TAG_structure_type:
	case DW_TAG_union_type:
	case DW_TAG_class_type:
		if (tag__type(self)->declaration)
			fwd_decl__encode(self, core_id, ctf);
		else
			structure_type__encode(self, core_id, ctf);
		break;
	case DW_TAG_array_type:
		array_type__encode(self, core_id, ctf);
		break;
	case DW_TAG_subroutine_type:
		subroutine_type__encode(self, core_id, ctf);
		break;
	case DW_TAG_enumeration_type:
		enumeration_type__encode(self, core_id, ctf);
		break;
	}
}

#define HASHADDR__BITS 8
#define HASHADDR__SIZE (1UL << HASHADDR__BITS)
#define hashaddr__fn(key) hash_64(key, HASHADDR__BITS)

static struct function *hashaddr__find_function(const struct hlist_head hashtable[],
						const uint64_t addr)
{
	struct function *function;
	struct hlist_node *pos;
	uint16_t bucket = hashaddr__fn(addr);
	const struct hlist_head *head = &hashtable[bucket];

	hlist_for_each_entry(function, pos, head, tool_hnode) {
		if (function->lexblock.low_pc == addr)
			return function;
	}

	return NULL;
}

static struct variable *hashaddr__find_variable(const struct hlist_head hashtable[],
						const uint64_t addr)
{
	struct variable *variable;
	struct hlist_node *pos;
	uint16_t bucket = hashaddr__fn(addr);
	const struct hlist_head *head = &hashtable[bucket];

	hlist_for_each_entry(variable, pos, head, tool_hnode) {
		if (variable->addr == addr)
			return variable;
	}

	return NULL;
}

int cu__encode_ctf(struct cu *self)
{
	int err = -1;
	struct ctf *ctf = ctf__new(self->filename, self->elf);

	if (ctf == NULL)
		goto out;

	ctf__set_strings(ctf, &strings->gb);

	uint32_t id;
	struct tag *pos;
	cu__for_each_type(self, id, pos)
		tag__encode_ctf(pos, id, ctf);

	struct hlist_head hash_addr[HASHADDR__SIZE];

	for (id = 0; id < HASHADDR__SIZE; ++id)
		INIT_HLIST_HEAD(&hash_addr[id]);

	struct function *function;
	cu__for_each_function(self, id, function) {
		uint64_t addr = function->lexblock.low_pc;
		struct hlist_head *head = &hash_addr[hashaddr__fn(addr)];
		hlist_add_head(&function->tool_hnode, head);
	}

	cu__cache_symtab(self);

	uint64_t addr;
	GElf_Sym sym;
	const char *sym_name;
	cu__for_each_cached_symtab_entry(self, id, sym, sym_name) {
		if (ctf__ignore_symtab_function(&sym, sym_name))
			continue;

		addr = elf_sym__value(&sym);
		int64_t position;
		function = hashaddr__find_function(hash_addr, addr);
		if (function == NULL) {
			fprintf(stderr,
				"function %4d: %-20s %#llx %5u NOT FOUND!\n",
				id, sym_name,
				(unsigned long long)addr, elf_sym__size(&sym));
			err = ctf__add_function(ctf, 0, 0, 0, &position);
			if (err != 0)
				goto out_err_ctf;
			continue;
		}

		const struct ftype *ftype = &function->proto;
		err = ctf__add_function(ctf, function->proto.tag.type,
					ftype->nr_parms,
					ftype->unspec_parms, &position);

		if (err != 0)
			goto out_err_ctf;

		struct parameter *pos;
		ftype__for_each_parameter(ftype, pos)
			ctf__add_function_parameter(ctf, pos->tag.type, &position);
	}

	for (id = 0; id < HASHADDR__SIZE; ++id)
		INIT_HLIST_HEAD(&hash_addr[id]);

	struct variable *var;
	cu__for_each_variable(self, id, pos) {
		var = tag__variable(pos);
		if (var->location != LOCATION_GLOBAL)
			continue;
		struct hlist_head *head = &hash_addr[hashaddr__fn(var->addr)];
		hlist_add_head(&var->tool_hnode, head);
	}

	cu__for_each_cached_symtab_entry(self, id, sym, sym_name) {
		if (ctf__ignore_symtab_object(&sym, sym_name))
			continue;
		addr = elf_sym__value(&sym);

		var = hashaddr__find_variable(hash_addr, addr);
		if (var == NULL) {
			fprintf(stderr,
				"variable %4d: %-20s %#llx %5u NOT FOUND!\n",
				id, sym_name, (unsigned long long)addr,
				elf_sym__size(&sym));
			err = ctf__add_object(ctf, 0);
			if (err != 0)
				goto out_err_ctf;
			continue;
		}

		err = ctf__add_object(ctf, var->tag.type);
		if (err != 0)
			goto out_err_ctf;
	}

	ctf__encode(ctf, CTF_FLAGS_COMPR);

	err = 0;
out_delete:
	ctf__delete(ctf);
out:
	return err;
out_err_ctf:
	fprintf(stderr,
		"%4d: %-20s %#llx %5u failed encoding, "
		"ABORTING!\n", id, sym_name,
		(unsigned long long)addr, elf_sym__size(&sym));
	goto out_delete;
}
