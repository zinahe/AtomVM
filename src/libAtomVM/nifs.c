/***************************************************************************
 *   Copyright 2017 by Davide Bettio <davide@uninstall.it>                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Lesser General Public License as        *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA .        *
 ***************************************************************************/

#include "nifs.h"

#include "context.h"
#include "interop.h"
#include "mailbox.h"
#include "module.h"
#include "scheduler.h"
#include "term.h"
#include "utils.h"
#include "sys.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_NIF_NAME_LEN 32

static const char *const ok_atom = "\x2" "ok";
static const char *const error_atom = "\x5" "error";
static const char *const undefined_atom = "\x9" "undefined";

static void process_echo_mailbox(Context *ctx);
static void process_console_mailbox(Context *ctx);

static term nif_erlang_concat_2(Context *ctx, int argc, term argv[]);
static term nif_erlang_make_ref_0(Context *ctx, int argc, term argv[]);
static term nif_erlang_open_port_2(Context *ctx, int argc, term argv[]);
static term nif_erlang_register_2(Context *ctx, int argc, term argv[]);
static term nif_erlang_send_2(Context *ctx, int argc, term argv[]);
static term nif_erlang_setelement_3(Context *ctx, int argc, term argv[]);
static term nif_erlang_spawn_3(Context *ctx, int argc, term argv[]);
static term nif_erlang_whereis_1(Context *ctx, int argc, term argv[]);
static term nif_erlang_system_time_1(Context *ctx, int argc, term argv[]);
static term nif_erlang_universaltime_0(Context *ctx, int argc, term argv[]);
static term nif_erts_debug_flat_size(Context *ctx, int argc, term argv[]);

static const struct Nif make_ref_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_make_ref_0
};

static const struct Nif open_port_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_open_port_2
};

static const struct Nif register_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_register_2
};

static const struct Nif spawn_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_spawn_3
};

static const struct Nif send_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_send_2
};

static const struct Nif setelement_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_setelement_3
};

static const struct Nif whereis_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_whereis_1
};

static const struct Nif concat_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_concat_2
};

static const struct Nif system_time_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_system_time_1
};

static const struct Nif universaltime_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erlang_universaltime_0
};

static const struct Nif flat_size_nif =
{
    .base.type = NIFFunctionType,
    .nif_ptr = nif_erts_debug_flat_size
};

//Ignore warning caused by gperf generated code
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "nifs_hash.h"
#pragma GCC diagnostic pop

static inline term term_from_atom_string(GlobalContext *glb, AtomString string)
{
    int global_atom_index = globalcontext_insert_atom(glb, string);
    return term_from_atom_index(global_atom_index);
}

const struct Nif *nifs_get(AtomString module, AtomString function, int arity)
{
    char nifname[MAX_NIF_NAME_LEN];

    int module_name_len = atom_string_len(module);
    memcpy(nifname, atom_string_data(module), module_name_len);

    nifname[module_name_len] = ':';

    int function_name_len = atom_string_len(function);
    memcpy(nifname + module_name_len + 1, atom_string_data(function), function_name_len);

    nifname[module_name_len + function_name_len + 1] = '/';
    nifname[module_name_len + function_name_len + 2] = '0' + arity;
    nifname[module_name_len + function_name_len + 3] = 0;

    const NifNameAndNifPtr *nameAndPtr = nif_in_word_set(nifname, strlen(nifname));
    if (!nameAndPtr) {
        return NULL;
    }

    return nameAndPtr->nif;
}

static term nif_erlang_open_port_2(Context *ctx, int argc, term argv[])
{
    if (UNLIKELY(argc != 2)) {
        fprintf(stderr, "wrong arity\n");
        abort();
    }

    term port_name = argv[0];
    term opts = argv[1];

    if (!(term_is_tuple(port_name) && term_get_tuple_arity(port_name) == 2) && !term_is_nonempty_list(opts)) {
        fprintf(stderr, "bad args\n");
        abort();
    }

    term t = term_get_tuple_element(port_name, 1);
    char *driver_name = interop_term_to_string(t);
    if (IS_NULL_PTR(driver_name)) {
        int error_index = globalcontext_insert_atom(ctx->global, error_atom);
        if (error_index < 0) {
            abort();
        }
        return term_from_atom_index(error_index);
    }

    Context *new_ctx = NULL;

    if (!strcmp("echo", driver_name)) {
        new_ctx = context_new(ctx->global);
        new_ctx->native_handler = process_echo_mailbox;

    } else if (!strcmp("console", driver_name)) {
        new_ctx = context_new(ctx->global);
        new_ctx->native_handler = process_console_mailbox;
    }

    if (!new_ctx) {
        new_ctx = platform_open_port(ctx->global, driver_name, opts);
    }

    free(driver_name);

    scheduler_make_waiting(ctx->global, new_ctx);

    return term_from_local_process_id(new_ctx->process_id);
}


static term nif_erlang_register_2(Context *ctx, int argc, term argv[])
{
    if (UNLIKELY((argc != 2) || !term_is_atom(argv[0]) || !term_is_pid(argv[1]))) {
        fprintf(stderr, "bad match\n");
        abort();
    }

    int atom_index = term_to_atom_index(argv[0]);
    int pid = term_to_local_process_id(argv[1]);

    globalcontext_register_process(ctx->global, atom_index, pid);

    return term_nil();
}

static term nif_erlang_whereis_1(Context *ctx, int argc, term argv[])
{
    if (UNLIKELY((argc != 1) || !term_is_atom(argv[0]))) {
        fprintf(stderr, "bad match\n");
        abort();
    }

    int atom_index = term_to_atom_index(argv[0]);

    int local_process_id = globalcontext_get_registered_process(ctx->global, atom_index);
    if (local_process_id) {
        return term_from_local_process_id(local_process_id);
    } else {
        int undefined_index = globalcontext_insert_atom(ctx->global, undefined_atom);
        return term_from_atom_index(undefined_index);
    }
}

static void process_echo_mailbox(Context *ctx)
{
    Message *msg = mailbox_dequeue(ctx);
    term pid = term_get_tuple_element(msg->message, 0);
    term val = term_get_tuple_element(msg->message, 1);

    int local_process_id = term_to_local_process_id(pid);
    Context *target = globalcontext_get_process(ctx->global, local_process_id);
    mailbox_send(target, val);

    free(msg);
}

static void process_console_mailbox(Context *ctx)
{

    Message *msg = mailbox_dequeue(ctx);
    term pid = term_get_tuple_element(msg->message, 0);
    term val = term_get_tuple_element(msg->message, 1);

    int local_process_id = term_to_local_process_id(pid);
    Context *target = globalcontext_get_process(ctx->global, local_process_id);

    char *str = interop_term_to_string(val);
    if (IS_NULL_PTR(str)) {
        free(msg);
        return;
    }

    printf("%s", str);
    free(str);

    //TODO: use globalcontext_get_atom_id(ctx->global, ok_atom);
    int ok_index = globalcontext_insert_atom(ctx->global, ok_atom);
    mailbox_send(target, term_from_atom_index(ok_index));

    free(msg);
}

static term nif_erlang_spawn_3(Context *ctx, int argc, term argv[])
{
    if (UNLIKELY(argc != 3)) {
        fprintf(stderr, "spawn: wrong args count\n");
        abort();
    }

    if (UNLIKELY(!term_is_atom(argv[0]) || !term_is_atom(argv[1]) || !term_is_list(argv[2]))) {
        fprintf(stderr, "spawn: invalid arguments\n");
        abort();
    }

    Context *new_ctx = context_new(ctx->global);

    int mod_atom_index = term_to_atom_index(argv[0]);
    AtomString module_string = (AtomString) valueshashtable_get_value(ctx->global->atoms_ids_table, mod_atom_index, (unsigned long) NULL);
    int func_atom_index = term_to_atom_index(argv[1]);
    AtomString function_string = (AtomString) valueshashtable_get_value(ctx->global->atoms_ids_table, func_atom_index, (unsigned long) NULL);

    Module *found_module = globalcontext_get_module(ctx->global, module_string);
    if (UNLIKELY(!found_module)) {
        int undefined_index = globalcontext_insert_atom(ctx->global, undefined_atom);
        if (undefined_index < 0) {
            abort();
        }
        return term_from_atom_index(undefined_index);
    }

    int label = module_search_exported_function(found_module, function_string, term_list_length(argv[2]));
    //TODO: fail here if no function has been found
    new_ctx->saved_module = found_module;
    new_ctx->saved_ip = found_module->labels[label];
    new_ctx->cp = module_address(found_module->module_index, found_module->end_instruction_ii);

    //TODO: check available registers count
    int reg_index = 0;
    term t = argv[2];
    while (!term_is_nil(t)) {
        term *t_ptr = term_get_list_ptr(t);
        new_ctx->x[reg_index] = memory_copy_term_tree(&new_ctx->heap_ptr, &new_ctx->e, t_ptr[1], 0);
        t = *t_ptr;
        reg_index++;
    }

    return term_from_local_process_id(new_ctx->process_id);
}
static term nif_erlang_send_2(Context *ctx, int argc, term argv[])
{
    if (UNLIKELY(argc != 2)) {
        fprintf(stderr, "send: wrong args count\n");
        abort();
    }

    if (UNLIKELY(!term_is_pid(argv[0]))) {
        fprintf(stderr, "send: invalid arguments\n");
        abort();
    }

    int local_process_id = term_to_local_process_id(argv[0]);
    Context *target = globalcontext_get_process(ctx->global, local_process_id);

    mailbox_send(target, argv[1]);

    return argv[1];
}

static term nif_erlang_concat_2(Context *ctx, int argc, term argv[])
{
    if (UNLIKELY(argc != 2)) {
        fprintf(stderr, "++: wrong args count\n");
        abort();
    }

    term prepend_list = argv[0];

    if (UNLIKELY(!term_is_nonempty_list(prepend_list))) {
        if (term_is_nil(prepend_list)) {
            return argv[1];

        } else {
            fprintf(stderr, "Argument error\n");
            abort();
        }
    }

    int len = term_list_length(prepend_list);
    memory_ensure_free(ctx, len * 2);

    // GC might have changed all pointers
    prepend_list = argv[0];
    term append_list = argv[1];

    term t = prepend_list;
    term list_begin = term_nil();
    term *prev_term = NULL;

    // TODO: handle impropers list
    while (!term_is_nil(t)) {
        term head = term_get_list_head(t);

        term *new_list_item = term_list_alloc(ctx);

        if (prev_term) {
            prev_term[0] = term_list_from_list_ptr(new_list_item);
        } else {
            list_begin = term_list_from_list_ptr(new_list_item);
        }

        prev_term = new_list_item;
        new_list_item[1] = head;

        t = term_get_list_tail(t);
    }

    if (prev_term) {
        prev_term[0] = append_list;
    }

    return list_begin;
}

term nif_erlang_make_ref_0(Context *ctx, int argc, term argv[])
{
    UNUSED(argv);

    if (UNLIKELY(argc != 0)) {
        fprintf(stderr, "erlang:make_ref: wrong args count\n");
        abort();
    }

    uint64_t ref_ticks = globalcontext_get_ref_ticks(ctx->global);

    return term_from_ref_ticks(ref_ticks, ctx);
}

term nif_erlang_system_time_1(Context *ctx, int argc, term argv[])
{
    UNUSED(ctx);

    if (argc != 1) {
        fprintf(stderr, "erlang:system_time: wrong args count\n");
        abort();
    }

    struct timespec ts;
    sys_time(&ts);

    term minute_atom = term_from_atom_string(ctx->global, "\x6" "minute");
    if (argv[0] == minute_atom) {
        // FIXME: This is not standard, however we cannot hold seconds since 1970 in just 27 bits.
        return term_from_int32(ts.tv_sec / 60);

    } else {
        fprintf(stderr, "nif_erlang_system_time: error, got: %lx\n", argv[0]);
        abort();
    }
}

term nif_erlang_universaltime_0(Context *ctx, int argc, term argv[])
{
    UNUSED(ctx);
    UNUSED(argv);

    if (argc != 0) {
        fprintf(stderr, "system_time_to_universal_time(): wrong args count\n");
        abort();
    }

    // 4 = size of date/time tuple, 3 size of date time tuple
    memory_ensure_free(ctx, 3 + 4 + 4);
    term date_tuple = term_alloc_tuple(3, ctx);
    term time_tuple = term_alloc_tuple(3, ctx);
    term date_time_tuple = term_alloc_tuple(2, ctx);

    struct timespec ts;
    sys_time(&ts);

    struct tm broken_down_time;
    gmtime_r(&ts.tv_sec, &broken_down_time);

    term_put_tuple_element(date_tuple, 0, term_from_int32(1900 + broken_down_time.tm_year));
    term_put_tuple_element(date_tuple, 1, term_from_int32(broken_down_time.tm_mon + 1));
    term_put_tuple_element(date_tuple, 2, term_from_int32(broken_down_time.tm_mday));

    term_put_tuple_element(time_tuple, 0, term_from_int32(broken_down_time.tm_hour));
    term_put_tuple_element(time_tuple, 1, term_from_int32(broken_down_time.tm_min));
    term_put_tuple_element(time_tuple, 2, term_from_int32(broken_down_time.tm_sec));

    term_put_tuple_element(date_time_tuple, 0, date_tuple);
    term_put_tuple_element(date_time_tuple, 1, time_tuple);

    return date_time_tuple;
}

static term nif_erlang_setelement_3(Context *ctx, int argc, term argv[])
{
    if (argc != 3) {
        fprintf(stderr, "setelement: wrong args count\n");
        abort();
    }

    // indexes are 1 based
    int replace_index = term_to_int32(argv[0]) - 1;

    int tuple_size = term_get_tuple_arity(argv[1]);

    if (UNLIKELY((replace_index >= tuple_size) || (replace_index < 0))) {
        fprintf(stderr, "setelement: bad argument: %i\n", replace_index);
        abort();
    }

    memory_ensure_free(ctx, tuple_size + 1);
    term new_tuple = term_alloc_tuple(tuple_size, ctx);

    term old_tuple = argv[1];
    for (int i = 0; i < tuple_size; i++) {
        term_put_tuple_element(new_tuple, i, term_get_tuple_element(old_tuple, i));
    }

    term value = argv[2];
    term_put_tuple_element(new_tuple, replace_index, value);

    return new_tuple;
}

static term nif_erts_debug_flat_size(Context *ctx, int argc, term argv[])
{
    UNUSED(ctx);

    if (UNLIKELY(argc != 1)) {
        fprintf(stderr, "erts_debug:flat_size: wrong args count\n");
        abort();
    }

    unsigned long terms_count;
    int stack_slots;

    memory_estimate_term_memory_usage(argv[0], &terms_count, &stack_slots);

    return term_from_int32(terms_count);
}
