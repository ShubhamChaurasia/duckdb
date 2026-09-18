//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/catalog_scan_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_scan_filter.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/function/table_function.hpp"

#include <functional>

namespace duckdb {
class ClientContext;
class LogicalGet;

//! Filter pushdown for the catalog functions: equality and IN constraints on database_name, schema_name and
//! table_name/view_name. The database names select the catalogs, the rest is passed to Catalog::ScanEntries.
struct CatalogFunctionPushdown {
	//! pushdown_complex_filter hook
	static void PushdownComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data,
	                                  vector<unique_ptr<Expression>> &filters);
	//! to_string hook: "Catalog Filters: ..." on the EXPLAIN node
	static InsertionOrderPreservingMap<string> ToString(TableFunctionToStringInput &input);
	//! Scans the entries of 'type' in the named catalogs (all if none), in GetAllSchemas order
	static void ScanEntries(ClientContext &context, const TableFunctionInitInput &input, CatalogType type,
	                        const std::function<void(CatalogEntry &)> &callback);
};

struct CatalogScanBindData : public TableFunctionData {
	//! null: all catalogs
	unique_ptr<vector<string>> databases;
	CatalogScanFilter filter;
	//! table_name or view_name, for EXPLAIN
	string entry_column = "table_name";
};

} // namespace duckdb
