#include "test_capi_v2.hpp"

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 storage extension tests: ATTACH ... (TYPE stor) serves a read-only
// catalog whose schemas and tables come from callbacks. The fake remote has
// schemas Sales (Orders, Items, Bad, Ghost, Broken) and hr (people).
//
// Callbacks avoid Catch assertions (a REQUIRE would throw through the C
// boundary) and latch what they see into file-scope statics. Names carry a
// Stor prefix: this directory is a unity build.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

struct StorObserved {
	int attach_calls = 0;
	int list_schema_calls = 0;
	std::map<std::string, int> list_table_calls;
	std::map<std::string, int> bind_calls;
	std::string attach_name;
	std::string attach_path;
	std::map<std::string, std::string> attach_options;
	std::string catalog_data_seen;
	std::string user_data_seen;
	int catalog_data_destroyed = 0;
};
StorObserved stor_observed;

void StorReset() {
	stor_observed = StorObserved();
}

void StorDestroyCatalogData(void *data) {
	stor_observed.catalog_data_destroyed++;
	delete static_cast<std::string *>(data);
}

void StorFail(duckdb_v2_error_info_handle *err, const char *message) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text = Convert(message);
	duckdb_v2_error_info_set_text(*err, &text);
}

void StorAttach(duckdb_v2_storage_attach_info_handle info, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	stor_observed.attach_calls++;
	duckdb_v2_identifier_t name = {nullptr, 0};
	duckdb_v2_storage_attach_info_get_name(info, &name, err);
	stor_observed.attach_name = Convert(name);
	duckdb_v2_str path = {nullptr, 0};
	duckdb_v2_storage_attach_info_get_path(info, &path, err);
	stor_observed.attach_path = Convert(path);

	void *user_data = nullptr;
	duckdb_v2_storage_attach_info_get_user_data(info, &user_data, err);
	stor_observed.user_data_seen = user_data ? *static_cast<std::string *>(user_data) : "";

	idx_t count = 0;
	duckdb_v2_storage_attach_info_get_option_count(info, &count, err);
	std::string default_schema;
	for (idx_t i = 0; i < count; i++) {
		duckdb_v2_identifier_t option_name = {nullptr, 0};
		duckdb_v2_storage_attach_info_get_option_name(info, i, &option_name, err);
		duckdb_v2_value_handle value = nullptr;
		duckdb_v2_storage_attach_info_get_option_value(info, i, &value, err);
		auto rendered = Render(value);
		duckdb_v2_value_destroy(&value);
		stor_observed.attach_options[Convert(option_name)] = rendered;
		if (Convert(option_name) == "schema") {
			default_schema = rendered;
		}
	}
	if (stor_observed.attach_path == "refuse") {
		StorFail(err, "the remote refused the connection");
		return;
	}
	if (!default_schema.empty()) {
		auto schema = Convert(default_schema);
		duckdb_v2_storage_attach_info_set_default_schema(info, &schema, err);
	}
	duckdb_v2_opaque data {new std::string("remote:" + stor_observed.attach_path), StorDestroyCatalogData, nullptr};
	duckdb_v2_storage_attach_info_set_catalog_data(info, &data, err);
}

void StorListSchemas(duckdb_v2_storage_list_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *err) {
	stor_observed.list_schema_calls++;
	// Not valid while listing schemas.
	duckdb_v2_identifier_t schema = {nullptr, 0};
	if (duckdb_v2_storage_list_info_get_schema_name(info, &schema, nullptr) == DUCKDB_V2_ERROR_NONE) {
		StorFail(err, "get_schema_name should fail while listing schemas");
		return;
	}
	for (auto name : {"Sales", "hr", "SALES"}) {
		auto ident = Convert(name);
		duckdb_v2_storage_list_info_add_name(info, &ident, err);
	}
}

void StorListTables(duckdb_v2_storage_list_info_handle info, duckdb_v2_context_handle,
                    duckdb_v2_error_info_handle *err) {
	void *catalog_data = nullptr;
	duckdb_v2_storage_list_info_get_catalog_data(info, &catalog_data, err);
	stor_observed.catalog_data_seen = catalog_data ? *static_cast<std::string *>(catalog_data) : "";

	duckdb_v2_identifier_t schema = {nullptr, 0};
	duckdb_v2_storage_list_info_get_schema_name(info, &schema, err);
	auto schema_name = Convert(schema);
	stor_observed.list_table_calls[schema_name]++;
	std::vector<const char *> tables;
	if (schema_name == "Sales") {
		tables = {"Orders", "Items", "Bad", "Ghost", "Broken", "Lateral"};
	} else if (schema_name == "hr") {
		tables = {"people"};
	}
	for (auto table : tables) {
		auto ident = Convert(table);
		duckdb_v2_storage_list_info_add_name(info, &ident, err);
	}
}

DUCKDB_V2_ERROR StorSetFunction(duckdb_v2_storage_bind_info_handle info, const char *function,
                                duckdb_v2_error_info_handle *err) {
	duckdb_v2_identifier_t parts[1] = {Convert(function)};
	duckdb_v2_qname_handle name = nullptr;
	auto rc = duckdb_v2_qname_create(parts, 1, &name, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return rc;
	}
	rc = duckdb_v2_storage_bind_info_set_function_name(info, name, err);
	duckdb_v2_qname_destroy(&name);
	return rc;
}

void StorAddBigint(duckdb_v2_storage_bind_info_handle info, duckdb_v2_context_handle context, int64_t n,
                   duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	auto value = MakeInt64Value(factory, n);
	duckdb_v2_storage_bind_info_add_argument(info, value, err);
	duckdb_v2_value_destroy(&value);
}

void StorDeclareColumn(duckdb_v2_storage_bind_info_handle info, duckdb_v2_context_handle context, const char *name,
                       DUCKDB_V2_LOGICAL_TYPE_ID id, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	auto type = MakeType(factory, id);
	auto column = Convert(name);
	duckdb_v2_storage_bind_info_add_column(info, &column, type, err);
	duckdb_v2_logical_type_destroy(&type);
}

void StorBindTable(duckdb_v2_storage_bind_info_handle info, duckdb_v2_context_handle context,
                   duckdb_v2_error_info_handle *err) {
	duckdb_v2_identifier_t schema = {nullptr, 0};
	duckdb_v2_storage_bind_info_get_schema_name(info, &schema, err);
	duckdb_v2_identifier_t table = {nullptr, 0};
	duckdb_v2_storage_bind_info_get_table_name(info, &table, err);
	auto qualified = Convert(schema) + "." + Convert(table);
	stor_observed.bind_calls[qualified]++;

	if (qualified == "Sales.Orders") {
		// Columns come from binding repeat(5, 3): one BIGINT column named "5".
		StorSetFunction(info, "repeat", err);
		StorAddBigint(info, context, 5, err);
		StorAddBigint(info, context, 3, err);
	} else if (qualified == "Sales.Items") {
		// Declared columns: the table is described without binding repeat.
		StorSetFunction(info, "repeat", err);
		StorAddBigint(info, context, 2, err);
		StorAddBigint(info, context, 2, err);
		StorDeclareColumn(info, context, "item_id", DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	} else if (qualified == "Sales.Bad") {
		// Declared as VARCHAR, but repeat returns BIGINT.
		StorSetFunction(info, "repeat", err);
		StorAddBigint(info, context, 2, err);
		StorAddBigint(info, context, 2, err);
		StorDeclareColumn(info, context, "label", DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, err);
	} else if (qualified == "Sales.Lateral") {
		// range is a table in-out function, which cannot back a table.
		StorSetFunction(info, "range", err);
		StorAddBigint(info, context, 3, err);
	} else if (qualified == "Sales.Broken") {
		StorFail(err, "the remote table is unreadable");
	} else if (qualified == "hr.people") {
		// A named argument.
		StorSetFunction(info, "test_all_types", err);
		duckdb_v2_factory_handle factory = nullptr;
		duckdb_v2_context_get_factory(context, &factory, nullptr);
		auto value = MakeBoolValue(factory, true);
		auto parameter = Convert("use_large_enum");
		duckdb_v2_storage_bind_info_add_named_argument(info, &parameter, value, err);
		duckdb_v2_value_destroy(&value);
	}
	// Ghost: declined.
}

int stor_user_data_destroyed = 0;
void StorDestroyUserData(void *data) {
	stor_user_data_destroyed++;
	delete static_cast<std::string *>(data);
}

duckdb_v2_storage_extension_handle StorCreate(duckdb_v2_factory_handle factory) {
	duckdb_v2_storage_extension_handle storage = nullptr;
	REQUIRE(duckdb_v2_storage_extension_create(factory, &storage, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_storage_extension_set_attach(storage, StorAttach, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_storage_extension_set_list_schemas(storage, StorListSchemas, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_storage_extension_set_list_tables(storage, StorListTables, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_storage_extension_set_bind_table(storage, StorBindTable, nullptr) == DUCKDB_V2_ERROR_NONE);
	return storage;
}

void StorRegister(EnvFixture &fx, const char *type_name = "stor") {
	auto storage = StorCreate(fx.factory);
	duckdb_v2_opaque user_data {new std::string("planted"), StorDestroyUserData, nullptr};
	REQUIRE(duckdb_v2_storage_extension_set_user_data(storage, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name = Convert(type_name);
	REQUIRE(duckdb_v2_instance_register_storage_extension(fx.instance, &name, storage, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_storage_extension_destroy(&storage) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(storage == nullptr);
}

std::vector<int64_t> StorQueryI64(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql, &result, &err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_str text = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &text);
		auto message = Convert(text);
		duckdb_v2_error_info_destroy(&err);
		duckdb_v2_result_destroy(&result);
		FAIL(sql << ": " << message);
	}
	std::vector<int64_t> out;
	while (auto chunk = StepChunk(result)) {
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		duckdb_v2_vector_handle vec = nullptr;
		duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
		duckdb_v2_vector_view view {};
		duckdb_v2_vector_get_view(vec, &view, nullptr);
		for (idx_t i = 0; i < size; i++) {
			out.push_back(static_cast<const int64_t *>(view.data)[SelAt(view.sel, i)]);
		}
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	duckdb_v2_result_destroy(&result);
	return out;
}

std::vector<std::string> StorQueryStrings(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	std::vector<std::string> out;
	while (auto chunk = StepChunk(result)) {
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		duckdb_v2_vector_handle vec = nullptr;
		duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
		duckdb_v2_vector_view view {};
		duckdb_v2_vector_get_view(vec, &view, nullptr);
		for (idx_t i = 0; i < size; i++) {
			auto &bytes = static_cast<const duckdb_v2_bytes *>(view.data)[SelAt(view.sel, i)];
			out.push_back(Convert(Convert(bytes)));
		}
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	duckdb_v2_result_destroy(&result);
	return out;
}

// Runs a statement expected to fail and returns the error text, from execution or from stepping.
std::string StorErrorText(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql, &result, &err);
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	while (rc == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		rc = duckdb_v2_result_step(result, &chunk, &status, &err);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (rc != DUCKDB_V2_ERROR_NONE || status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_result_wait(result, &err);
		}
	}
	std::string message = "<no error>";
	if (rc != DUCKDB_V2_ERROR_NONE && err) {
		duckdb_v2_str text = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &text);
		message = Convert(text);
	}
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_result_destroy(&result);
	return message;
}

bool StorContains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("V2 storage extension: ATTACH serves schemas and tables from callbacks", "[capi_v2][storage_extension]") {
	EnvFixture fx;
	StorReset();
	StorRegister(fx);

	ExecSQL(fx.conn, "ATTACH 'remote-db' AS r (TYPE stor, SCHEMA 'Sales', PASSWORD 'pw')");
	REQUIRE(stor_observed.attach_calls == 1);
	REQUIRE(stor_observed.attach_name == "r");
	REQUIRE(stor_observed.attach_path == "remote-db");
	REQUIRE(stor_observed.user_data_seen == "planted");
	REQUIRE(stor_observed.attach_options.size() == 2);
	REQUIRE(stor_observed.attach_options["password"] == "pw");
	REQUIRE(stor_observed.attach_options["schema"] == "Sales");
	// Nothing is listed until something is looked up.
	REQUIRE(stor_observed.list_schema_calls == 0);

	REQUIRE(StorQueryI64(fx.conn, "SELECT * FROM r.Sales.Orders ORDER BY 1") == std::vector<int64_t> {5, 5, 5});
	REQUIRE(stor_observed.catalog_data_seen == "remote:remote-db");
	// Names resolve case-insensitively; the default schema serves two-part names.
	REQUIRE(StorQueryI64(fx.conn, "SELECT * FROM r.sales.ORDERS ORDER BY 1") == std::vector<int64_t> {5, 5, 5});
	REQUIRE(StorQueryI64(fx.conn, "SELECT * FROM r.Orders ORDER BY 1") == std::vector<int64_t> {5, 5, 5});
	// Projection and filters run over the bound columns.
	REQUIRE(StorQueryI64(fx.conn, "SELECT \"5\" + 1 FROM r.Sales.Orders WHERE \"5\" > 4") ==
	        std::vector<int64_t> {6, 6, 6});
	REQUIRE(StorQueryI64(fx.conn, "SELECT count(*) FROM r.Sales.Orders WHERE \"5\" > 5") == std::vector<int64_t> {0});
	// Declared columns rename the function's output.
	REQUIRE(StorQueryI64(fx.conn, "SELECT item_id FROM r.Sales.Items ORDER BY item_id") == std::vector<int64_t> {2, 2});
	// A named argument reaches the function.
	REQUIRE(StorQueryI64(fx.conn, "SELECT count(*) FROM r.hr.people") == std::vector<int64_t> {3});

	// Each callback ran once per name, however often the name was used.
	REQUIRE(stor_observed.list_schema_calls == 1);
	REQUIRE(stor_observed.list_table_calls["Sales"] == 1);
	REQUIRE(stor_observed.list_table_calls["hr"] == 1);
	REQUIRE(stor_observed.bind_calls["Sales.Orders"] == 1);
	REQUIRE(stor_observed.bind_calls["Sales.Items"] == 1);

	// USE makes the default schema the search path.
	ExecSQL(fx.conn, "USE r");
	REQUIRE(StorQueryI64(fx.conn, "SELECT * FROM Orders ORDER BY 1") == std::vector<int64_t> {5, 5, 5});
	ExecSQL(fx.conn, "USE memory");
}

TEST_CASE("V2 storage extension: listings, unknown tables and errors", "[capi_v2][storage_extension]") {
	EnvFixture fx;
	StorReset();
	StorRegister(fx);
	ExecSQL(fx.conn, "ATTACH 'remote-db' AS r (TYPE stor)");

	// The duplicate SALES spelling is kept once.
	REQUIRE(
	    StorQueryStrings(fx.conn, "SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'r' ORDER BY 1") ==
	    std::vector<std::string> {"Sales", "hr"});
	// Declined (Ghost) and failing (Broken) tables are left out of listings; Bad is listed from its declaration.
	REQUIRE(StorQueryStrings(fx.conn,
	                         "SELECT schema_name || '.' || table_name FROM duckdb_tables() WHERE database_name = 'r' "
	                         "ORDER BY 1") ==
	        std::vector<std::string> {"Sales.Bad", "Sales.Items", "Sales.Orders", "hr.people"});

	REQUIRE(StorContains(StorErrorText(fx.conn, "SELECT * FROM r.Sales.Ghost"), "Ghost"));
	REQUIRE(StorContains(StorErrorText(fx.conn, "SELECT * FROM r.Sales.Nope"), "Nope"));
	REQUIRE(StorContains(StorErrorText(fx.conn, "SELECT * FROM r.Nope.Orders"), "Nope"));
	REQUIRE(StorContains(StorErrorText(fx.conn, "SELECT * FROM r.Sales.Broken"), "the remote table is unreadable"));
	// A failed bind is not cached: the next lookup asks again.
	auto broken_calls = stor_observed.bind_calls["Sales.Broken"];
	StorErrorText(fx.conn, "SELECT * FROM r.Sales.Broken");
	REQUIRE(stor_observed.bind_calls["Sales.Broken"] == broken_calls + 1);
	// Declared columns that do not match the function fail the scan.
	auto bad = StorErrorText(fx.conn, "SELECT * FROM r.Sales.Bad");
	REQUIRE(StorContains(bad, "label"));
	REQUIRE(StorContains(bad, "VARCHAR"));
	REQUIRE(StorContains(StorErrorText(fx.conn, "SELECT * FROM r.Sales.Lateral"), "in-out"));
	// Without a SCHEMA option the default schema is main, which the remote does not have.
	REQUIRE(StorErrorText(fx.conn, "SELECT * FROM r.Orders") != "<no error>");
	// A typo suggests by name.
	REQUIRE(StorContains(StorErrorText(fx.conn, "SELECT * FROM r.Sales.Orderz"), "Orders"));
}

TEST_CASE("V2 storage extension: the catalog is read-only", "[capi_v2][storage_extension]") {
	EnvFixture fx;
	StorReset();
	StorRegister(fx);
	ExecSQL(fx.conn, "ATTACH 'remote-db' AS r (TYPE stor)");

	for (auto sql :
	     {"CREATE TABLE r.Sales.t (i INTEGER)", "CREATE SCHEMA r.s", "CREATE VIEW r.Sales.v AS SELECT 1",
	      "INSERT INTO r.Sales.Orders VALUES (1)", "DELETE FROM r.Sales.Orders", "UPDATE r.Sales.Orders SET \"5\" = 1",
	      "DROP TABLE r.Sales.Orders", "CREATE TABLE r.Sales.t2 AS SELECT 1"}) {
		INFO(sql);
		REQUIRE(StorErrorText(fx.conn, sql) != "<no error>");
	}
	REQUIRE(StorContains(StorErrorText(fx.conn, "CREATE TABLE r.Sales.t (i INTEGER)"), "read-only"));
	// Still readable afterwards.
	REQUIRE(StorQueryI64(fx.conn, "SELECT count(*) FROM r.Sales.Orders") == std::vector<int64_t> {3});
}

TEST_CASE("V2 storage extension: attach failure, detach and re-attach", "[capi_v2][storage_extension]") {
	EnvFixture fx;
	StorReset();
	StorRegister(fx);

	REQUIRE(
	    StorContains(StorErrorText(fx.conn, "ATTACH 'refuse' AS r (TYPE stor)"), "the remote refused the connection"));
	REQUIRE(StorErrorText(fx.conn, "SELECT * FROM r.Sales.Orders") != "<no error>");

	ExecSQL(fx.conn, "ATTACH 'remote-db' AS r (TYPE stor)");
	REQUIRE(StorQueryI64(fx.conn, "SELECT count(*) FROM r.Sales.Orders") == std::vector<int64_t> {3});
	REQUIRE(stor_observed.catalog_data_destroyed == 0);
	ExecSQL(fx.conn, "DETACH r");
	REQUIRE(stor_observed.catalog_data_destroyed == 1);

	// A new ATTACH starts over: everything is listed and bound again.
	ExecSQL(fx.conn, "ATTACH 'remote-db' AS r (TYPE stor)");
	REQUIRE(StorQueryI64(fx.conn, "SELECT count(*) FROM r.Sales.Orders") == std::vector<int64_t> {3});
	REQUIRE(stor_observed.list_schema_calls == 2);
	REQUIRE(stor_observed.bind_calls["Sales.Orders"] == 2);

	// Two databases of the same type are independent.
	ExecSQL(fx.conn, "ATTACH 'other-db' AS o (TYPE STOR)");
	REQUIRE(StorQueryI64(fx.conn, "SELECT count(*) FROM o.hr.people") == std::vector<int64_t> {3});
	REQUIRE(stor_observed.catalog_data_seen == "remote:other-db");
}

TEST_CASE("V2 storage extension: the type prefix of the path selects the extension", "[capi_v2][storage_extension]") {
	EnvFixture fx;
	StorReset();
	StorRegister(fx);
	ExecSQL(fx.conn, "ATTACH 'stor:remote-db' AS r");
	REQUIRE(stor_observed.attach_path == "remote-db");
	REQUIRE(StorQueryI64(fx.conn, "SELECT count(*) FROM r.Sales.Orders") == std::vector<int64_t> {3});
}

TEST_CASE("V2 storage extension: registration", "[capi_v2][storage_extension]") {
	EnvFixture fx;
	StorReset();
	stor_user_data_destroyed = 0;
	{
		EnvFixture inner;
		StorRegister(inner);
		REQUIRE(stor_user_data_destroyed == 0);
	}
	// Destroyed with the instance.
	REQUIRE(stor_user_data_destroyed == 1);

	StorRegister(fx);
	// The type name is taken.
	{
		auto storage = StorCreate(fx.factory);
		auto name = Convert("STOR");
		REQUIRE(duckdb_v2_instance_register_storage_extension(fx.instance, &name, storage, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_storage_extension_destroy(&storage);
	}
	// Every callback is required.
	{
		duckdb_v2_storage_extension_handle storage = nullptr;
		REQUIRE(duckdb_v2_storage_extension_create(fx.factory, &storage, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_storage_extension_set_attach(storage, StorAttach, nullptr) == DUCKDB_V2_ERROR_NONE);
		auto name = Convert("partial");
		REQUIRE(duckdb_v2_instance_register_storage_extension(fx.instance, &name, storage, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_storage_extension_destroy(&storage);
	}
	// A handle registers once.
	{
		auto storage = StorCreate(fx.factory);
		auto name = Convert("first");
		REQUIRE(duckdb_v2_instance_register_storage_extension(fx.instance, &name, storage, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		auto name2 = Convert("second");
		REQUIRE(duckdb_v2_instance_register_storage_extension(fx.instance, &name2, storage, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_storage_extension_destroy(&storage);
	}
	// Null arguments and destroy null-safety.
	duckdb_v2_storage_extension_handle storage = nullptr;
	REQUIRE(duckdb_v2_storage_extension_create(nullptr, &storage, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_storage_extension_create(fx.factory, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_storage_extension_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_storage_extension_destroy(&storage) == DUCKDB_V2_ERROR_NONE);
}

} // namespace test_capi_v2
