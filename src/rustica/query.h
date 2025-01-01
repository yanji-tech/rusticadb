/*
 * Copyright (c) 2024 燕几（北京）科技有限公司
 *
 * Rustica (runtime) is licensed under Mulan PSL v2. You can use this
 * software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *              http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES
 * OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
 * TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 */

#ifndef RUSTICA_QUERY_H
#define RUSTICA_QUERY_H

#include "postgres.h"
#include "executor/spi.h"
#include "storage/latch.h"

#include "llhttp.h"

#include "wasm_runtime_common.h"

#define RST_WASM_TO_PG_ARGS wasm_exec_env_t exec_env, const wasm_value_t value
#define RST_WASM_TO_PG_RET Datum
#define RST_PG_TO_WASM_ARGS                                          \
    Datum value, wasm_struct_obj_t tuptable, uint32 row, uint32 col, \
        wasm_exec_env_t exec_env, wasm_ref_type_t type
#define RST_PG_TO_WASM_RET wasm_value_t

typedef struct PreparedModule PreparedModule;

typedef struct Context {
    WaitEventSet *wait_set;
    pgsocket fd;

    llhttp_t http_parser;
    llhttp_settings_t http_settings;
    WASMArrayObjectRef current_buf;
    int32_t bytes_view;
    wasm_function_inst_t on_message_begin;
    wasm_function_inst_t on_method;
    wasm_function_inst_t on_method_complete;
    wasm_function_inst_t on_url;
    wasm_function_inst_t on_url_complete;
    wasm_function_inst_t on_version;
    wasm_function_inst_t on_version_complete;
    wasm_function_inst_t on_header_field;
    wasm_function_inst_t on_header_field_complete;
    wasm_function_inst_t on_header_value;
    wasm_function_inst_t on_header_value_complete;
    wasm_function_inst_t on_headers_complete;
    wasm_function_inst_t on_body;
    wasm_function_inst_t on_message_complete;
    wasm_function_inst_t on_error;

    PreparedModule *module;
    wasm_function_inst_t call_as_datum;
    wasm_struct_obj_t queries;
    List *tuple_tables;
} Context;

typedef RST_WASM_TO_PG_RET (*WASM2PGFunc)(RST_WASM_TO_PG_ARGS);
typedef RST_PG_TO_WASM_RET (*PG2WASMFunc)(RST_PG_TO_WASM_ARGS);

typedef struct QueryPlan {
    SPIPlanPtr plan;
    uint32 nargs;
    uint32 nattrs;
    wasm_ref_type_t tuptable_type;
    wasm_ref_type_t array_type;
    wasm_ref_type_t fixed_array_type;
    wasm_ref_type_t ret_type;
    wasm_ref_type_t *ret_field_types;
    WASM2PGFunc *wasm_to_pg_funcs;
    PG2WASMFunc *pg_to_wasm_funcs;
} QueryPlan;

void
rst_init_query_plan(QueryPlan *plan, HeapTuple query_tup, TupleDesc tupdesc);

void
rst_free_query_plan(QueryPlan *plan);

void
rst_init_instance_context(wasm_exec_env_t exec_env);

void
rst_free_instance_context(wasm_exec_env_t exec_env);

int32_t
env_execute_statement(wasm_exec_env_t exec_env, int32_t idx);

WASMArrayObjectRef
env_detoast(wasm_exec_env_t exec_env,
            int32_t tuptable_idx,
            int32_t row,
            int32_t col);

#endif /* RUSTICA_QUERY_H */
