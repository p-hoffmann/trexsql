#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/similar_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb::capiv2 {

//----------------------------------------------------------------------------------------------------------------------
// Callback payloads
//----------------------------------------------------------------------------------------------------------------------

struct CV2StorageCallbacks {
	duckdb_v2_storage_attach_callback_fn attach = nullptr;
	duckdb_v2_storage_list_callback_fn list_schemas = nullptr;
	duckdb_v2_storage_list_callback_fn list_tables = nullptr;
	duckdb_v2_storage_bind_callback_fn bind_table = nullptr;
	shared_ptr<CV2UserData> user_data;

	void *UserData() const {
		return user_data ? user_data->GetData() : nullptr;
	}
};

class CV2StorageAttachInfo {
public:
	const CV2StorageCallbacks &callbacks;
	Identifier name;
	string path;
	vector<std::pair<Identifier, Value>> options;

	shared_ptr<CV2UserData> out_catalog_data;
	optional<Identifier> out_default_schema;

	explicit CV2StorageAttachInfo(const CV2StorageCallbacks &callbacks) : callbacks(callbacks) {
	}
};

class CV2StorageListInfo {
public:
	const CV2StorageCallbacks &callbacks;
	void *catalog_data;
	//! Unset when listing schemas.
	optional<Identifier> schema;

	vector<Identifier> out_names;
	identifier_set_t seen;

	CV2StorageListInfo(const CV2StorageCallbacks &callbacks, void *catalog_data)
	    : callbacks(callbacks), catalog_data(catalog_data) {
	}
};

class CV2StorageBindInfo {
public:
	const CV2StorageCallbacks &callbacks;
	void *catalog_data;
	Identifier schema;
	Identifier table;

	bool has_function = false;
	QualifiedName out_function_name;
	vector<Value> out_arguments;
	vector<std::pair<Identifier, Value>> out_named_arguments;
	ColumnList out_columns;

	CV2StorageBindInfo(const CV2StorageCallbacks &callbacks, void *catalog_data, Identifier schema, Identifier table)
	    : callbacks(callbacks), catalog_data(catalog_data), schema(std::move(schema)), table(std::move(table)) {
	}

	void RequireFunction(const char *function_name) const {
		if (!has_function) {
			throw InvalidInputException("%s requires duckdb_v2_storage_bind_info_set_function_name to be called first.",
			                            function_name);
		}
	}
};

static auto Convert(duckdb_v2_storage_attach_info_handle info) -> CV2StorageAttachInfo * {
	return reinterpret_cast<CV2StorageAttachInfo *>(info);
}
static auto Convert(CV2StorageAttachInfo *info) -> duckdb_v2_storage_attach_info_handle {
	return reinterpret_cast<duckdb_v2_storage_attach_info_handle>(info);
}
static auto Convert(duckdb_v2_storage_list_info_handle info) -> CV2StorageListInfo * {
	return reinterpret_cast<CV2StorageListInfo *>(info);
}
static auto Convert(CV2StorageListInfo *info) -> duckdb_v2_storage_list_info_handle {
	return reinterpret_cast<duckdb_v2_storage_list_info_handle>(info);
}
static auto Convert(duckdb_v2_storage_bind_info_handle info) -> CV2StorageBindInfo * {
	return reinterpret_cast<CV2StorageBindInfo *>(info);
}
static auto Convert(CV2StorageBindInfo *info) -> duckdb_v2_storage_bind_info_handle {
	return reinterpret_cast<duckdb_v2_storage_bind_info_handle>(info);
}

template <class INFO, class CALLBACK>
static void InvokeStorageCallback(CALLBACK callback, INFO &info, ClientContext &context) {
	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	callback(Convert(&info), Convert(&callback_context), &err_ptr);
	if (err.HasError()) {
		err.ThrowAsException();
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Catalog
//----------------------------------------------------------------------------------------------------------------------

class CV2StorageCatalog;
class CV2StorageSchemaEntry;

[[noreturn]] static void ThrowReadOnly(const Identifier &catalog_name) {
	throw BinderException("Cannot modify \"%s\": databases attached through a C API storage extension are read-only",
	                      catalog_name.GetIdentifierName());
}

//! The table function call that reads a table, bound once to learn the columns and again for every scan.
struct CV2BoundScan {
	TableFunction function;
	unique_ptr<FunctionData> bind_data;
	vector<LogicalType> types;
	vector<Identifier> names;
};

class CV2StorageTableEntry final : public TableCatalogEntry {
public:
	CV2StorageTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	                     CV2StorageBindInfo bound_call)
	    : TableCatalogEntry(catalog, schema, info), columns(std::move(info.columns)), call(std::move(bound_call)) {
	}

	const ColumnList &GetColumns() const override {
		return columns;
	}

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override {
		return nullptr;
	}

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override {
		auto scan = TakePendingScan(context);
		if (!scan) {
			scan = make_uniq<CV2BoundScan>(BindScan(context, call));
		}
		CheckColumns(*scan);
		bind_data = std::move(scan->bind_data);
		return std::move(scan->function);
	}

	TableStorageInfo GetStorageInfo(ClientContext &context) override {
		return TableStorageInfo();
	}

	//! No rowid: a remote table has none, and the scan function may not support projection pushdown.
	virtual_column_map_t GetVirtualColumns() const override {
		return virtual_column_map_t();
	}

	vector<column_t> GetRowIdColumns() const override {
		return vector<column_t>();
	}

	//! Binds the call outside of any query, returning what the scan of `call` produces.
	static CV2BoundScan BindScan(ClientContext &context, const CV2StorageBindInfo &call) {
		vector<unique_ptr<ParsedExpression>> children;
		for (auto &argument : call.out_arguments) {
			children.push_back(ConstantExpression::FromValue(argument));
		}
		for (auto &named_argument : call.out_named_arguments) {
			// The binder recovers a named argument's name from the expression alias.
			auto child = ConstantExpression::FromValue(named_argument.second);
			child->SetAlias(named_argument.first);
			children.push_back(std::move(child));
		}
		TableFunctionRef ref;
		ref.function = make_uniq<FunctionExpression>(call.out_function_name, std::move(children));

		auto binder = Binder::CreateBinder(context);
		auto bound = binder->Bind(static_cast<TableRef &>(ref));
		if (!bound.plan || bound.plan->type != LogicalOperatorType::LOGICAL_GET || !bound.plan->children.empty()) {
			throw BinderException("Table \"%s\".\"%s\": storage extension scan function \"%s\" must be a plain table "
			                      "function, not a macro or a function that rewrites itself into a query",
			                      call.schema.GetIdentifierName(), call.table.GetIdentifierName(),
			                      call.out_function_name.ToString());
		}
		auto &get = bound.plan->Cast<LogicalGet>();
		if (get.function.in_out_function) {
			throw BinderException("Table \"%s\".\"%s\": storage extension scan function \"%s\" must be a plain table "
			                      "function, not a table in-out function",
			                      call.schema.GetIdentifierName(), call.table.GetIdentifierName(),
			                      call.out_function_name.ToString());
		}
		CV2BoundScan result;
		result.function = *get.function.GetDefinition();
		result.bind_data = std::move(get.bind_data);
		result.types = get.returned_types;
		result.names = get.names;
		return result;
	}

	//! Hands a scan bound while looking the table up to the scan of the same query, so it is bound once.
	void SetPendingScan(ClientContext &context, unique_ptr<CV2BoundScan> scan) {
		lock_guard<mutex> guard(pending_lock);
		pending_scan = std::move(scan);
		pending_context = &context;
		pending_query = MetaTransaction::Get(context).GetActiveQuery();
	}

private:
	unique_ptr<CV2BoundScan> TakePendingScan(ClientContext &context) {
		lock_guard<mutex> guard(pending_lock);
		auto scan = std::move(pending_scan);
		bool same_query =
		    pending_context == &context && pending_query == MetaTransaction::Get(context).GetActiveQuery();
		pending_context = nullptr;
		return same_query ? std::move(scan) : nullptr;
	}

	void CheckColumns(const CV2BoundScan &scan) const {
		auto &columns = GetColumns();
		if (columns.LogicalColumnCount() != scan.types.size()) {
			throw BinderException("Table \"%s\": scan function \"%s\" now returns %llu columns, but the table has %llu",
			                      name.GetIdentifierName(), call.out_function_name.ToString(), scan.types.size(),
			                      columns.LogicalColumnCount());
		}
		idx_t i = 0;
		for (auto &column : columns.Logical()) {
			if (column.Type() != scan.types[i]) {
				throw BinderException(
				    "Table \"%s\": column \"%s\" is declared as %s, but scan function \"%s\" returns %s",
				    name.GetIdentifierName(), column.Name().GetIdentifierName(), column.Type().ToString(),
				    call.out_function_name.ToString(), scan.types[i].ToString());
			}
			i++;
		}
	}

	ColumnList columns;
	CV2StorageBindInfo call;
	mutex pending_lock;
	unique_ptr<CV2BoundScan> pending_scan;
	optional_ptr<ClientContext> pending_context;
	transaction_t pending_query = 0;
};

class CV2StorageCatalog final : public Catalog {
public:
	CV2StorageCatalog(AttachedDatabase &db, string type_name, string path, shared_ptr<CV2StorageCallbacks> callbacks,
	                  shared_ptr<CV2UserData> catalog_data, optional<Identifier> default_schema)
	    : Catalog(db), type_name(std::move(type_name)), path(std::move(path)), callbacks(std::move(callbacks)),
	      catalog_data(std::move(catalog_data)), default_schema(std::move(default_schema)) {
	}

	void Initialize(bool load_builtin) override {
	}

	string GetCatalogType() override {
		return type_name;
	}

	optional<Identifier> GetDefaultSchema() const override {
		if (default_schema) {
			return default_schema;
		}
		return Catalog::GetDefaultSchema();
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override {
		ThrowReadOnly(GetName());
	}

	void DropSchema(ClientContext &context, DropInfo &info) override {
		ThrowReadOnly(GetName());
	}

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override {
		ThrowReadOnly(GetName());
	}
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override {
		ThrowReadOnly(GetName());
	}
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override {
		ThrowReadOnly(GetName());
	}
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override {
		ThrowReadOnly(GetName());
	}

	DatabaseSize GetDatabaseSize(ClientContext &context) override {
		return DatabaseSize();
	}

	bool InMemory() override {
		return false;
	}

	string GetDBPath() override {
		return path;
	}

	const CV2StorageCallbacks &Callbacks() const {
		return *callbacks;
	}

	void *CatalogData() const {
		return catalog_data ? catalog_data->GetData() : nullptr;
	}

private:
	//! Loads the schema list on first use. False when it is not loaded and cannot be without a client context.
	bool EnsureSchemas(optional_ptr<ClientContext> context);

	string type_name;
	string path;
	shared_ptr<CV2StorageCallbacks> callbacks;
	shared_ptr<CV2UserData> catalog_data;
	optional<Identifier> default_schema;

	mutex schemas_lock;
	bool schemas_loaded = false;
	//! In the order the callback reported them.
	vector<unique_ptr<CV2StorageSchemaEntry>> schemas;
	identifier_map_t<CV2StorageSchemaEntry *> schemas_by_name;
};

class CV2StorageSchemaEntry final : public SchemaCatalogEntry {
public:
	CV2StorageSchemaEntry(CV2StorageCatalog &catalog, CreateSchemaInfo &info)
	    : SchemaCatalogEntry(catalog, info), storage_catalog(catalog) {
	}

	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override {
		if (type != CatalogType::TABLE_ENTRY) {
			return;
		}
		auto names = TableNames(context);
		if (!names) {
			return;
		}
		for (auto &table_name : *names) {
			optional_ptr<CatalogEntry> entry;
			try {
				entry = GetTable(context, table_name);
			} catch (std::exception &) {
				// A table that fails to bind is left out of listings; looking it up by name reports why.
				continue;
			}
			if (entry) {
				callback(*entry);
			}
		}
	}

	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override {
		if (type != CatalogType::TABLE_ENTRY) {
			return;
		}
		lock_guard<mutex> guard(tables_lock);
		for (auto &table_name : table_names) {
			auto entry = tables.find(table_name);
			if (entry != tables.end() && entry->second) {
				callback(*entry->second);
			}
		}
	}

	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction,
	                                       const EntryLookupInfo &lookup_info) override {
		if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY || !transaction.context) {
			return nullptr;
		}
		return GetTable(*transaction.context, Identifier(lookup_info.GetEntryName()));
	}

	//! Ranks names only: the default would bind every table to suggest one for a typo.
	SimilarCatalogEntry GetSimilarEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override {
		SimilarCatalogEntry result;
		if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY || !transaction.context) {
			return result;
		}
		auto names = TableNames(*transaction.context);
		if (!names) {
			return result;
		}
		for (auto &table_name : *names) {
			auto score = StringUtil::SimilarityRating(table_name.GetIdentifierName(), lookup_info.GetEntryName());
			if (score > result.score) {
				result.score = score;
				result.name = table_name;
			}
		}
		return result;
	}

	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override {
		ThrowReadOnly(catalog.GetName());
	}
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override {
		ThrowReadOnly(catalog.GetName());
	}
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override {
		ThrowReadOnly(catalog.GetName());
	}
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override {
		ThrowReadOnly(catalog.GetName());
	}
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override {
		ThrowReadOnly(catalog.GetName());
	}
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override {
		ThrowReadOnly(catalog.GetName());
	}
	void DropEntry(ClientContext &context, DropInfo &info) override {
		ThrowReadOnly(catalog.GetName());
	}
	void Alter(CatalogTransaction transaction, AlterInfo &info) override {
		ThrowReadOnly(catalog.GetName());
	}

private:
	//! The listed table names, loaded on first use; null when they cannot be loaded.
	optional_ptr<const vector<Identifier>> TableNames(ClientContext &context) {
		{
			lock_guard<mutex> guard(tables_lock);
			if (tables_loaded) {
				return &table_names;
			}
		}
		// Callbacks run unlocked: they may query the catalog of this very database.
		CV2StorageListInfo info(storage_catalog.Callbacks(), storage_catalog.CatalogData());
		info.schema = name;
		InvokeStorageCallback(storage_catalog.Callbacks().list_tables, info, context);

		lock_guard<mutex> guard(tables_lock);
		if (!tables_loaded) {
			table_names = std::move(info.out_names);
			for (auto &table_name : table_names) {
				listed.insert(table_name);
			}
			tables_loaded = true;
		}
		return &table_names;
	}

	optional_ptr<CatalogEntry> GetTable(ClientContext &context, const Identifier &requested) {
		if (!TableNames(context)) {
			return nullptr;
		}
		Identifier table_name;
		{
			lock_guard<mutex> guard(tables_lock);
			auto listed_entry = listed.find(requested);
			if (listed_entry == listed.end()) {
				return nullptr;
			}
			// The listed spelling, not the requested one, is what the extension knows the table by.
			table_name = *listed_entry;
			auto entry = tables.find(table_name);
			if (entry != tables.end()) {
				return entry->second.get();
			}
		}

		CV2StorageBindInfo call(storage_catalog.Callbacks(), storage_catalog.CatalogData(), name, table_name);
		InvokeStorageCallback(storage_catalog.Callbacks().bind_table, call, context);

		unique_ptr<CV2StorageTableEntry> table;
		if (call.has_function) {
			unique_ptr<CV2BoundScan> scan;
			ColumnList columns;
			if (call.out_columns.LogicalColumnCount() > 0) {
				columns = call.out_columns.Copy();
			} else {
				scan = make_uniq<CV2BoundScan>(CV2StorageTableEntry::BindScan(context, call));
				for (idx_t i = 0; i < scan->types.size(); i++) {
					columns.AddColumn(ColumnDefinition(scan->names[i], scan->types[i]));
				}
			}
			CreateTableInfo create_info(*this, table_name);
			create_info.columns = std::move(columns);
			table = make_uniq<CV2StorageTableEntry>(catalog, *this, create_info, std::move(call));
			if (scan) {
				table->SetPendingScan(context, std::move(scan));
			}
		}

		lock_guard<mutex> guard(tables_lock);
		auto entry = tables.find(table_name);
		if (entry != tables.end()) {
			// Another connection bound it first.
			return entry->second.get();
		}
		auto result = table.get();
		tables[table_name] = std::move(table);
		return result;
	}

	CV2StorageCatalog &storage_catalog;

	mutex tables_lock;
	bool tables_loaded = false;
	vector<Identifier> table_names;
	identifier_set_t listed;
	//! A null entry caches a listed table the bind callback declined.
	identifier_map_t<unique_ptr<CV2StorageTableEntry>> tables;
};

bool CV2StorageCatalog::EnsureSchemas(optional_ptr<ClientContext> context) {
	{
		lock_guard<mutex> guard(schemas_lock);
		if (schemas_loaded) {
			return true;
		}
	}
	if (!context) {
		return false;
	}
	CV2StorageListInfo info(*callbacks, CatalogData());
	InvokeStorageCallback(callbacks->list_schemas, info, *context);

	lock_guard<mutex> guard(schemas_lock);
	if (!schemas_loaded) {
		for (auto &schema_name : info.out_names) {
			CreateSchemaInfo create_info;
			create_info.SetSchema(schema_name);
			create_info.internal = false;
			auto schema = make_uniq<CV2StorageSchemaEntry>(*this, create_info);
			schemas_by_name[schema_name] = schema.get();
			schemas.push_back(std::move(schema));
		}
		schemas_loaded = true;
	}
	return true;
}

optional_ptr<SchemaCatalogEntry> CV2StorageCatalog::LookupSchema(CatalogTransaction transaction,
                                                                 const EntryLookupInfo &schema_lookup,
                                                                 OnEntryNotFound if_not_found) {
	optional_ptr<SchemaCatalogEntry> result;
	if (EnsureSchemas(transaction.context)) {
		lock_guard<mutex> guard(schemas_lock);
		auto entry = schemas_by_name.find(Identifier(schema_lookup.GetEntryName()));
		if (entry != schemas_by_name.end()) {
			result = entry->second;
		}
	}
	if (!result && if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException(schema_lookup.GetErrorContext(), "Schema with name \"%s\" does not exist in \"%s\"",
		                       schema_lookup.GetEntryName(), GetName().GetIdentifierName());
	}
	return result;
}

void CV2StorageCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	EnsureSchemas(&context);
	vector<reference<SchemaCatalogEntry>> snapshot;
	{
		lock_guard<mutex> guard(schemas_lock);
		for (auto &schema : schemas) {
			snapshot.push_back(*schema);
		}
	}
	for (auto &schema : snapshot) {
		callback(schema.get());
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Transactions: the catalog is read-only, so a transaction holds nothing.
//----------------------------------------------------------------------------------------------------------------------

class CV2StorageTransactionManager final : public TransactionManager {
public:
	explicit CV2StorageTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override {
		auto transaction = make_uniq<Transaction>(*this, context);
		auto &result = *transaction;
		lock_guard<mutex> guard(lock);
		transactions[result] = std::move(transaction);
		return result;
	}

	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override {
		RemoveTransaction(transaction);
		return ErrorData();
	}

	void RollbackTransaction(Transaction &transaction) override {
		RemoveTransaction(transaction);
	}

	void Checkpoint(ClientContext &context, bool force) override {
	}

private:
	void RemoveTransaction(Transaction &transaction) {
		lock_guard<mutex> guard(lock);
		transactions.erase(transaction);
	}

	mutex lock;
	reference_map_t<Transaction, unique_ptr<Transaction>> transactions;
};

//----------------------------------------------------------------------------------------------------------------------
// Storage extension
//----------------------------------------------------------------------------------------------------------------------

struct CV2StorageExtensionInfo final : public StorageExtensionInfo {
	string type_name;
	shared_ptr<CV2StorageCallbacks> callbacks;
};

static unique_ptr<Catalog> CV2StorageAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                            AttachedDatabase &db, const string &name, AttachInfo &info,
                                            AttachOptions &options) {
	auto &extension_info = static_cast<CV2StorageExtensionInfo &>(*storage_info);
	auto &callbacks = *extension_info.callbacks;

	CV2StorageAttachInfo attach(callbacks);
	attach.name = Identifier(name);
	attach.path = info.path;
	for (auto &option : options.options) {
		attach.options.emplace_back(Identifier(StringUtil::Lower(option.first)), option.second);
	}
	std::sort(attach.options.begin(), attach.options.end(),
	          [](const std::pair<Identifier, Value> &a, const std::pair<Identifier, Value> &b) {
		          return a.first.GetIdentifierName() < b.first.GetIdentifierName();
	          });
	InvokeStorageCallback(callbacks.attach, attach, context);

	return make_uniq<CV2StorageCatalog>(db, extension_info.type_name, info.path, extension_info.callbacks,
	                                    std::move(attach.out_catalog_data), std::move(attach.out_default_schema));
}

static unique_ptr<TransactionManager> CV2StorageCreateTransactionManager(optional_ptr<StorageExtensionInfo>,
                                                                         AttachedDatabase &db, Catalog &) {
	return make_uniq<CV2StorageTransactionManager>(db);
}

class CV2StorageExtension {
public:
	explicit CV2StorageExtension(DatabaseInstance &db) : db(db), callbacks(make_shared_ptr<CV2StorageCallbacks>()) {
	}

	void Register(DatabaseInstance &target, const string &type_name) {
		CheckRegistrationTarget(db, target, "storage extension");
		if (!callbacks->attach || !callbacks->list_schemas || !callbacks->list_tables || !callbacks->bind_table) {
			throw InvalidInputException(
			    "The attach, list-schemas, list-tables and bind-table callbacks must all be set "
			    "for the storage extension.");
		}
		if (type_name.empty()) {
			throw InvalidInputException("The storage type name cannot be empty.");
		}
		if (registered) {
			throw InvalidInputException("The storage extension is already registered.");
		}
		auto &config = DBConfig::GetConfig(target);
		if (StorageExtension::Find(config, type_name)) {
			throw InvalidInputException("A storage extension of type \"%s\" is already registered.", type_name);
		}

		auto info = make_shared_ptr<CV2StorageExtensionInfo>();
		info->type_name = type_name;
		// A copy: setters called after registration must not reach the registered extension.
		info->callbacks = make_shared_ptr<CV2StorageCallbacks>(*callbacks);

		auto extension = make_shared_ptr<StorageExtension>();
		extension->attach = CV2StorageAttach;
		extension->create_transaction_manager = CV2StorageCreateTransactionManager;
		extension->storage_info = std::move(info);
		StorageExtension::Register(config, type_name, std::move(extension));
		registered = true;
	}

	DatabaseInstance &db;
	shared_ptr<CV2StorageCallbacks> callbacks;
	bool registered = false;
};

static auto Convert(duckdb_v2_storage_extension_handle storage) -> CV2StorageExtension * {
	return reinterpret_cast<CV2StorageExtension *>(storage);
}
static auto Convert(CV2StorageExtension *storage) -> duckdb_v2_storage_extension_handle {
	return reinterpret_cast<duckdb_v2_storage_extension_handle>(storage);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_storage_extension_create(duckdb_v2_factory_handle factory,
                                                   duckdb_v2_storage_extension_handle *storage,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(storage);
	*storage = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2StorageExtension>(Convert(factory)->GetDatabase());
		*storage = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_extension_set_attach(duckdb_v2_storage_extension_handle storage,
                                                       duckdb_v2_storage_attach_callback_fn callback,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(storage);
	return WithErrorHandler(err, [&]() { Convert(storage)->callbacks->attach = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_storage_extension_set_list_schemas(duckdb_v2_storage_extension_handle storage,
                                                             duckdb_v2_storage_list_callback_fn callback,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(storage);
	return WithErrorHandler(err, [&]() { Convert(storage)->callbacks->list_schemas = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_storage_extension_set_list_tables(duckdb_v2_storage_extension_handle storage,
                                                            duckdb_v2_storage_list_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(storage);
	return WithErrorHandler(err, [&]() { Convert(storage)->callbacks->list_tables = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_storage_extension_set_bind_table(duckdb_v2_storage_extension_handle storage,
                                                           duckdb_v2_storage_bind_callback_fn callback,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(storage);
	return WithErrorHandler(err, [&]() { Convert(storage)->callbacks->bind_table = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_storage_extension_set_user_data(duckdb_v2_storage_extension_handle storage,
                                                          duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(storage);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(storage)->callbacks->user_data =
		    duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_attach_info_get_user_data(duckdb_v2_storage_attach_info_handle info, void **data,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->callbacks.UserData(); });
}

DUCKDB_V2_ERROR duckdb_v2_storage_attach_info_get_name(duckdb_v2_storage_attach_info_handle info,
                                                       duckdb_v2_identifier_t *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	*name = duckdb_v2_identifier_t {nullptr, 0};
	return WithErrorHandler(err, [&]() { *name = Convert(Convert(info)->name); });
}

DUCKDB_V2_ERROR duckdb_v2_storage_attach_info_get_path(duckdb_v2_storage_attach_info_handle info, duckdb_v2_str *path,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(path);
	*path = duckdb_v2_str {nullptr, 0};
	return WithErrorHandler(err, [&]() { *path = Convert(Convert(info)->path); });
}

DUCKDB_V2_ERROR duckdb_v2_storage_attach_info_get_option_count(duckdb_v2_storage_attach_info_handle info, idx_t *count,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->options.size(); });
}

static auto GetAttachOption(duckdb_v2_storage_attach_info_handle info, idx_t index)
    -> std::pair<duckdb::Identifier, duckdb::Value> & {
	auto &options = Convert(info)->options;
	if (index >= options.size()) {
		throw duckdb::InvalidInputException("Option index %llu is out of range: the ATTACH has %llu options.", index,
		                                    options.size());
	}
	return options[index];
}

DUCKDB_V2_ERROR duckdb_v2_storage_attach_info_get_option_name(duckdb_v2_storage_attach_info_handle info, idx_t index,
                                                              duckdb_v2_identifier_t *name,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	*name = duckdb_v2_identifier_t {nullptr, 0};
	return WithErrorHandler(err, [&]() { *name = Convert(GetAttachOption(info, index).first); });
}

DUCKDB_V2_ERROR duckdb_v2_storage_attach_info_get_option_value(duckdb_v2_storage_attach_info_handle info, idx_t index,
                                                               duckdb_v2_value_handle *value,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(value);
	*value = nullptr;
	return WithErrorHandler(err, [&]() { *value = Convert(new duckdb::Value(GetAttachOption(info, index).second)); });
}

DUCKDB_V2_ERROR duckdb_v2_storage_attach_info_set_catalog_data(duckdb_v2_storage_attach_info_handle info,
                                                               duckdb_v2_opaque *data,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(info)->out_catalog_data = duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_attach_info_set_default_schema(duckdb_v2_storage_attach_info_handle info,
                                                                 const duckdb_v2_identifier_t *schema,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(schema);
	return WithErrorHandler(err, [&]() {
		auto name = duckdb::Identifier(ConvertIdentifierName(schema));
		if (name.empty()) {
			throw duckdb::InvalidInputException("The default schema cannot be empty.");
		}
		Convert(info)->out_default_schema = std::move(name);
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_list_info_get_user_data(duckdb_v2_storage_list_info_handle info, void **data,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->callbacks.UserData(); });
}

DUCKDB_V2_ERROR duckdb_v2_storage_list_info_get_catalog_data(duckdb_v2_storage_list_info_handle info, void **data,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->catalog_data; });
}

DUCKDB_V2_ERROR duckdb_v2_storage_list_info_get_schema_name(duckdb_v2_storage_list_info_handle info,
                                                            duckdb_v2_identifier_t *schema,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(schema);
	*schema = duckdb_v2_identifier_t {nullptr, 0};
	return WithErrorHandler(err, [&]() {
		auto &list = *Convert(info);
		if (!list.schema) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_storage_list_info_get_schema_name is only valid when listing the tables of a schema.");
		}
		*schema = Convert(*list.schema);
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_list_info_add_name(duckdb_v2_storage_list_info_handle info,
                                                     const duckdb_v2_identifier_t *name,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() {
		auto &list = *Convert(info);
		auto identifier = duckdb::Identifier(ConvertIdentifierName(name));
		if (identifier.empty()) {
			throw duckdb::InvalidInputException("Schema and table names cannot be empty.");
		}
		if (list.seen.insert(identifier).second) {
			list.out_names.push_back(std::move(identifier));
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_bind_info_get_user_data(duckdb_v2_storage_bind_info_handle info, void **data,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->callbacks.UserData(); });
}

DUCKDB_V2_ERROR duckdb_v2_storage_bind_info_get_catalog_data(duckdb_v2_storage_bind_info_handle info, void **data,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->catalog_data; });
}

DUCKDB_V2_ERROR duckdb_v2_storage_bind_info_get_schema_name(duckdb_v2_storage_bind_info_handle info,
                                                            duckdb_v2_identifier_t *schema,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(schema);
	*schema = duckdb_v2_identifier_t {nullptr, 0};
	return WithErrorHandler(err, [&]() { *schema = Convert(Convert(info)->schema); });
}

DUCKDB_V2_ERROR duckdb_v2_storage_bind_info_get_table_name(duckdb_v2_storage_bind_info_handle info,
                                                           duckdb_v2_identifier_t *table,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(table);
	*table = duckdb_v2_identifier_t {nullptr, 0};
	return WithErrorHandler(err, [&]() { *table = Convert(Convert(info)->table); });
}

DUCKDB_V2_ERROR duckdb_v2_storage_bind_info_set_function_name(duckdb_v2_storage_bind_info_handle info,
                                                              duckdb_v2_qname_handle name,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() {
		auto &call = *Convert(info);
		call.out_function_name = *Convert(name);
		call.has_function = true;
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_bind_info_add_argument(duckdb_v2_storage_bind_info_handle info,
                                                         duckdb_v2_value_handle value,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &call = *Convert(info);
		call.RequireFunction("duckdb_v2_storage_bind_info_add_argument");
		call.out_arguments.push_back(*Convert(value));
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_bind_info_add_named_argument(duckdb_v2_storage_bind_info_handle info,
                                                               const duckdb_v2_identifier_t *name,
                                                               duckdb_v2_value_handle value,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &call = *Convert(info);
		call.RequireFunction("duckdb_v2_storage_bind_info_add_named_argument");
		call.out_named_arguments.emplace_back(duckdb::Identifier(ConvertIdentifierName(name)), *Convert(value));
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_bind_info_add_column(duckdb_v2_storage_bind_info_handle info,
                                                       const duckdb_v2_identifier_t *name,
                                                       duckdb_v2_logical_type_handle type,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(type);
	return WithErrorHandler(err, [&]() {
		auto column_name = duckdb::Identifier(ConvertIdentifierName(name));
		if (column_name.empty()) {
			throw duckdb::InvalidInputException("Column names cannot be empty.");
		}
		auto &columns = Convert(info)->out_columns;
		if (columns.ColumnExists(column_name)) {
			throw duckdb::InvalidInputException("Column \"%s\" is declared twice.", column_name.GetIdentifierName());
		}
		auto type_ref = Convert(type);
		const duckdb::LogicalType &column_type = *type_ref;
		columns.AddColumn(duckdb::ColumnDefinition(std::move(column_name), column_type));
	});
}

DUCKDB_V2_ERROR duckdb_v2_instance_register_storage_extension(duckdb_v2_instance_handle instance,
                                                              const duckdb_v2_identifier_t *name,
                                                              duckdb_v2_storage_extension_handle storage,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(storage);
	return WithErrorHandler(err, [&]() {
		auto &db = *Convert(instance)->GetDatabase().instance;
		Convert(storage)->Register(db, duckdb::string(ConvertIdentifierName(name)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_storage_extension(duckdb_v2_extension_handle extension,
                                                               const duckdb_v2_identifier_t *name,
                                                               duckdb_v2_storage_extension_handle storage,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(storage);
	return WithErrorHandler(err, [&]() {
		auto &db = GetExtensionLoader(extension).GetDatabaseInstance();
		Convert(storage)->Register(db, duckdb::string(ConvertIdentifierName(name)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_storage_extension_destroy(duckdb_v2_storage_extension_handle *storage) {
	return WithErrorHandler(nullptr, [&]() {
		if (!storage) {
			return;
		}
		if (*storage) {
			delete Convert(*storage);
			*storage = nullptr;
		}
	});
}
