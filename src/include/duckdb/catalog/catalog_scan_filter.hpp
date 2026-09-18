//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_scan_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! The schema and entry names a query filtered on; null means unconstrained. See Catalog::ScanEntries.
struct CatalogScanFilter {
	unique_ptr<vector<string>> schemas;
	unique_ptr<vector<string>> tables;
};

} // namespace duckdb
