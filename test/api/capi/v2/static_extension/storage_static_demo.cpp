// A V2 C API extension that registers a storage type from its entrypoint: `ATTACH '' AS d (TYPE storage_demo)` gives a
// database with one schema, `main`, holding one table, `answers`, read through repeat(42, 2).

// Statically linked: calls bind to DuckDB's symbols directly instead of through the vtable.
#define DUCKDB_BUILD_STATIC_EXTENSION
#include "duckdb_extension_v2.h"

#include <cstring>
#include <initializer_list>

namespace {

duckdb_v2_identifier_t DemoIdent(const char *s) {
	return duckdb_v2_identifier_t {s, strlen(s)};
}

void DemoAttach(duckdb_v2_storage_attach_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *) {
}

void DemoListSchemas(duckdb_v2_storage_list_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *err) {
	auto name = DemoIdent("main");
	duckdb_v2_storage_list_info_add_name(info, &name, err);
}

void DemoListTables(duckdb_v2_storage_list_info_handle info, duckdb_v2_context_handle,
                    duckdb_v2_error_info_handle *err) {
	auto name = DemoIdent("answers");
	duckdb_v2_storage_list_info_add_name(info, &name, err);
}

void DemoBindTable(duckdb_v2_storage_bind_info_handle info, duckdb_v2_context_handle context,
                   duckdb_v2_error_info_handle *err) {
	duckdb_v2_identifier_t parts[1] = {DemoIdent("repeat")};
	duckdb_v2_qname_handle function = nullptr;
	if (duckdb_v2_qname_create(parts, 1, &function, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto rc = duckdb_v2_storage_bind_info_set_function_name(info, function, err);
	duckdb_v2_qname_destroy(&function);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, err);
	for (int64_t argument : {int64_t(42), int64_t(2)}) {
		duckdb_v2_value_handle value = nullptr;
		if (duckdb_v2_value_create_bigint(factory, argument, &value, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		duckdb_v2_storage_bind_info_add_argument(info, value, err);
		duckdb_v2_value_destroy(&value);
	}
}

} // namespace

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_v2_extension_handle extension, duckdb_v2_context_handle context,
                            duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	if (duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_storage_extension_handle storage = nullptr;
	if (duckdb_v2_storage_extension_create(factory, &storage, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_storage_extension_set_attach(storage, DemoAttach, err);
	duckdb_v2_storage_extension_set_list_schemas(storage, DemoListSchemas, err);
	duckdb_v2_storage_extension_set_list_tables(storage, DemoListTables, err);
	duckdb_v2_storage_extension_set_bind_table(storage, DemoBindTable, err);
	auto name = DemoIdent("storage_demo");
	duckdb_v2_extension_register_storage_extension(extension, &name, storage, err);
	duckdb_v2_storage_extension_destroy(&storage);
}
