#include "postgres.h"
#include <string.h>
#include "fmgr.h"
#include "varatt.h"
#include "wasm_export.h"
#include "wasm_memory.h"
#include "aot_emit_aot_file.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(compile_wasm);

void *
noop_realloc(void *ptr, size_t size) {
    return ptr;
}

Datum
compile_wasm(PG_FUNCTION_ARGS) {
    char error_buf[128];
    wasm_module_t wasm_module = NULL;
    AOTCompData *comp_data = NULL;
    AOTCompContext *comp_ctx = NULL;
    AOTCompOption option = { 0 };
    option.opt_level = 3;
    option.size_level = 0;
    option.output_format = AOT_FORMAT_FILE;
    option.bounds_checks = 2;
    option.stack_bounds_checks = 2;
    option.enable_simd = true;
    option.enable_bulk_memory = true;
    option.enable_ref_types = false;
    option.enable_aux_stack_frame = true;
    option.enable_gc = true;
    option.target_arch = "x86_64";

    bytea *wasm = PG_GETARG_BYTEA_PP(0);
    int32 wasm_size = VARSIZE_ANY_EXHDR(wasm);

    MemAllocOption mem_option = { 0 };
    mem_option.allocator.malloc_func = palloc;
    mem_option.allocator.realloc_func = noop_realloc;
    mem_option.allocator.free_func = pfree;
    wasm_runtime_memory_init(Alloc_With_Allocator, &mem_option);
    aot_compiler_init();

    wasm_module = wasm_runtime_load(VARDATA_ANY(wasm),
                                    wasm_size,
                                    error_buf,
                                    sizeof(error_buf));
    comp_data = aot_create_comp_data(wasm_module, option.target_arch, true);
    comp_ctx = aot_create_comp_context(comp_data, &option);
    aot_compile_wasm(comp_ctx);

    void *obj_data = aot_obj_data_create(comp_ctx);
    uint32_t aot_file_size = get_aot_file_size(comp_ctx, comp_data, obj_data);
    bytea *rv = (bytea *)palloc(aot_file_size + VARHDRSZ);
    SET_VARSIZE(rv, aot_file_size + VARHDRSZ);
    aot_emit_aot_file_buf_ex(comp_ctx,
                             comp_data,
                             obj_data,
                             VARDATA(rv),
                             aot_file_size);
    aot_obj_data_destroy(obj_data);
    aot_compiler_destroy();
    wasm_runtime_memory_destroy();
    PG_RETURN_TEXT_P(rv);
}