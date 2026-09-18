#include "catch.hpp"
#include "duckdb/catalog/catalog_scan_filter.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "test_helpers.hpp"

#include <algorithm>

using namespace duckdb;

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
//! Names of the entries of 'type' (a TABLE_ENTRY scan also yields views)
static vector<string> Names(const vector<reference<CatalogEntry>> &entries,
                            CatalogType type = CatalogType::TABLE_ENTRY) {
	vector<string> result;
	for (auto &entry : entries) {
		if (entry.get().type == type) {
			result.push_back(entry.get().name.GetIdentifierName());
		}
	}
	return result;
}

static vector<string> Column(QueryResult &result, idx_t col = 0) {
	vector<string> values;
	for (idx_t row = 0; row < result.RowCount(); row++) {
		values.push_back(result.GetValue(col, row).ToString());
	}
	return values;
}

static vector<string> Names(const unique_ptr<vector<string>> &level) {
	return level ? *level : vector<string>();
}

static CatalogScanFilter Filter(const vector<string> *schemas, const vector<string> *tables) {
	CatalogScanFilter filter;
	if (schemas) {
		filter.schemas = make_uniq<vector<string>>(*schemas);
	}
	if (tables) {
		filter.tables = make_uniq<vector<string>>(*tables);
	}
	return filter;
}

//===--------------------------------------------------------------------===//
// Default ScanEntries
//===--------------------------------------------------------------------===//
TEST_CASE("Default ScanEntries ignores the filter", "[api][catalog]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS a"));
	REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA a.s1; CREATE SCHEMA a.s2"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a.s1.t1(x INT); CREATE TABLE a.s1.t2(x INT); CREATE TABLE a.s2.t3(x INT)"));
	REQUIRE_NO_FAIL(con.Query("CREATE VIEW a.s1.v1 AS SELECT 1"));
	auto &context = *con.context;
	context.transaction.BeginTransaction();
	auto &catalog_a = Catalog::GetCatalog(context, Identifier("a"));
	auto scan = [&](const CatalogScanFilter &filter, CatalogType type = CatalogType::TABLE_ENTRY) {
		vector<reference<CatalogEntry>> entries;
		catalog_a.ScanEntries(context, type, filter, [&](CatalogEntry &entry) { entries.push_back(entry); });
		return Names(entries, type);
	};
	const vector<string> s2 {"s2"}, t1 {"t1"}, missing {"nope"};
	const vector<string> everything {"t1", "t2", "t3"};

	SECTION("unconstrained") {
		REQUIRE(scan(CatalogScanFilter()) == everything);
		REQUIRE(scan(CatalogScanFilter(), CatalogType::VIEW_ENTRY) == vector<string> {"v1"});
	}

	SECTION("named schemas and tables") {
		REQUIRE(scan(Filter(&s2, nullptr)) == everything);
		REQUIRE(scan(Filter(nullptr, &t1)) == everything);
		REQUIRE(scan(Filter(&s2, &t1)) == everything);
		REQUIRE(scan(Filter(&missing, &missing)) == everything);
	}

	context.transaction.Commit();
}

//===--------------------------------------------------------------------===//
// ScanEntries override
//===--------------------------------------------------------------------===//
//! Scans only the named schemas, ignores the table names, records the filters it receives
struct RecordingCatalog : public DuckCatalog {
	using DuckCatalog::DuckCatalog;

	struct Received {
		vector<string> schemas, tables;
		bool had_schemas, had_tables;
		bool IsNarrowed() const {
			return had_schemas || had_tables;
		}
	};
	static vector<Received> received;

	void ScanEntries(ClientContext &context, CatalogType type, const CatalogScanFilter &filter,
	                 const std::function<void(CatalogEntry &)> &callback) override {
		received.push_back(Received {Names(filter.schemas), Names(filter.tables), filter.schemas != nullptr,
		                             filter.tables != nullptr});
		if (!filter.schemas) {
			Catalog::ScanEntries(context, type, filter, callback);
			return;
		}
		auto names = *filter.schemas;
		std::sort(names.begin(), names.end());
		for (auto &name : names) {
			auto schema = GetSchema(context, Identifier(name), OnEntryNotFound::RETURN_NULL);
			if (schema) {
				schema->Scan(context, type, callback);
			}
		}
	}
};
vector<RecordingCatalog::Received> RecordingCatalog::received;

struct RecordingStorageExtension : public StorageExtension {
	RecordingStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, RecordingCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

TEST_CASE("ScanEntries override", "[api][catalog]") {
	DBConfig config;
	StorageExtension::Register(config, "recording", make_shared_ptr<RecordingStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS rec (TYPE recording)"));
	REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA rec.s1; CREATE SCHEMA rec.s2"));
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE rec.s1.t1(x INT); CREATE TABLE rec.s1.t2(x INT); CREATE TABLE rec.s2.t3(x INT)"));
	RecordingCatalog::received.clear();

	SECTION("filter reaches the catalog") {
		auto result = con.Query("SELECT table_name FROM duckdb_tables() WHERE database_name = 'rec' AND schema_name = "
		                        "'s1' AND table_name IN ('t1', 'nope') ORDER BY 1");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(Column(*result) == vector<string> {"t1"});
		REQUIRE(RecordingCatalog::received.size() == 1);
		auto &f = RecordingCatalog::received[0];
		REQUIRE(f.schemas == vector<string> {"s1"});
		REQUIRE(f.tables == vector<string> {"t1", "nope"});
	}

	SECTION("ignored table names") {
		auto result = con.Query("SELECT table_name FROM duckdb_tables() WHERE database_name = 'rec' AND schema_name = "
		                        "'s1' AND table_name = 't1'");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(Column(*result) == vector<string> {"t1"});
	}

	SECTION("only the named schema") {
		auto result = con.Query("SELECT table_name FROM duckdb_tables() WHERE database_name = 'rec' AND schema_name = "
		                        "'s1' ORDER BY 1");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(Column(*result) == vector<string> {"t1", "t2"});
		REQUIRE(RecordingCatalog::received.size() == 1);
		REQUIRE(RecordingCatalog::received[0].schemas == vector<string> {"s1"});
	}

	SECTION("database only") {
		auto result = con.Query("SELECT count(*) FROM duckdb_tables() WHERE database_name = 'rec'");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(Column(*result) == vector<string> {"3"});
		REQUIRE(RecordingCatalog::received.size() == 1);
		REQUIRE(!RecordingCatalog::received[0].IsNarrowed());
	}

	SECTION("other database") {
		auto result = con.Query("SELECT count(*) FROM duckdb_tables() WHERE database_name = 'memory'");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(RecordingCatalog::received.empty());
	}

	SECTION("hidden database") {
		REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS h (HIDDEN true); CREATE TABLE h.t9(x INT)"));
		auto result = con.Query("SELECT count(*) FROM duckdb_tables() WHERE database_name IN ('h', 'rec')");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(Column(*result) == vector<string> {"3"});
		result = con.Query("SELECT count(*) FROM duckdb_tables() WHERE table_name = 't9'");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(Column(*result) == vector<string> {"0"});
	}

	SECTION("WITH ORDINALITY") {
		REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS aaa; CREATE TABLE aaa.t0(x INT)"));
		auto result =
		    con.Query("SELECT database_name FROM duckdb_tables() WITH ORDINALITY AS t(database_name, "
		              "database_oid, schema_name, schema_oid, table_name, table_oid, comment, tags, internal, "
		              "temporary, has_primary_key, estimated_size, column_count, index_count, "
		              "check_constraint_count, sql, ordinality) WHERE database_name IN ('aaa', 'rec') ORDER BY "
		              "ordinality");
		REQUIRE_NO_FAIL(*result);
		auto names = Column(*result);
		REQUIRE(names.size() == 4);
		REQUIRE(names[0] == "aaa");
		REQUIRE(names[1] == "rec");
	}
}
