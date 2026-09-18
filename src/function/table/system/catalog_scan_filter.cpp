#include "duckdb/function/table/catalog_scan_filter.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Extraction
//===--------------------------------------------------------------------===//
//! lower(x) or upper(x): names are matched case-insensitively, so x is the column
static bool IsCaseFold(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	auto &name = function.Function().GetName().GetIdentifierName();
	return (name == "lower" || name == "upper") && function.GetChildren().size() == 1;
}

//! The constraint list for a name column of 'get', or nullptr
static unique_ptr<vector<string>> *NameTargetOf(const LogicalGet &get, CatalogScanBindData &data,
                                                const Expression &expr_p) {
	auto &expr = IsCaseFold(expr_p) ? *expr_p.Cast<BoundFunctionExpression>().GetChildren()[0] : expr_p;
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return nullptr;
	}
	auto &binding = expr.Cast<BoundColumnRefExpression>().Binding();
	if (binding.table_index != get.table_index) {
		return nullptr;
	}
	auto &name = get.GetColumnName(get.GetColumnIds()[binding.column_index]).GetIdentifierName();
	if (name == "database_name") {
		return &data.databases;
	}
	if (name == "schema_name") {
		return &data.filter.schemas;
	}
	if (name == "table_name" || name == "view_name") {
		data.entry_column = name;
		return &data.filter.tables;
	}
	return nullptr;
}

//! A non-NULL VARCHAR constant
static bool TryGetStringConstant(const Expression &expr, string &result) {
	if (expr.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		return false;
	}
	auto &value = expr.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	result = StringValue::Get(value);
	return true;
}

//! Adds a name if no case-insensitive match is present
static void Constrain(unique_ptr<vector<string>> &target, const string &value) {
	if (!target) {
		target = make_uniq<vector<string>>();
	}
	for (auto &present : *target) {
		if (StringUtil::CIEquals(present, value)) {
			return;
		}
	}
	target->push_back(value);
}

static void TryConstrainEquality(const LogicalGet &get, CatalogScanBindData &data, const Expression &column_side,
                                 const Expression &constant_side) {
	auto target = NameTargetOf(get, data, column_side);
	string value;
	if (target && TryGetStringConstant(constant_side, value)) {
		Constrain(*target, value);
	}
}

//! Collects name = 'constant' and name IN (...) constraints below AND
static void ExtractConstraints(const LogicalGet &get, const Expression &expr, CatalogScanBindData &data) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		if (conjunction.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			return;
		}
		for (auto &child : conjunction.GetChildren()) {
			ExtractConstraints(get, *child, data);
		}
		return;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL ||
		    !BoundComparisonExpression::IsComparison(expr)) {
			return;
		}
		auto &comparison = expr.Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		TryConstrainEquality(get, data, left, right);
		TryConstrainEquality(get, data, right, left);
		return;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		if (expr.GetExpressionType() != ExpressionType::COMPARE_IN) {
			return;
		}
		auto &in = expr.Cast<BoundOperatorExpression>();
		if (in.GetChildren().empty()) {
			return;
		}
		auto target = NameTargetOf(get, data, *in.GetChildren()[0]);
		if (!target) {
			return;
		}
		vector<string> values;
		for (idx_t i = 1; i < in.GetChildren().size(); i++) {
			string value;
			if (!TryGetStringConstant(*in.GetChildren()[i], value)) {
				return;
			}
			values.push_back(std::move(value));
		}
		for (auto &value : values) {
			Constrain(*target, value);
		}
		return;
	}
	default:
		return;
	}
}

//! Not filter_pushdown: table filters only carry bare column-constant comparisons, so lower(schema_name) = 's' (the
//! SHOW TABLES rewrite) would not reach the function. The filters are left in place.
void CatalogFunctionPushdown::PushdownComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data,
                                                    vector<unique_ptr<Expression>> &filters) {
	auto &data = bind_data->Cast<CatalogScanBindData>();
	for (auto &expr : filters) {
		ExtractConstraints(get, *expr, data);
	}
}

//===--------------------------------------------------------------------===//
// EXPLAIN
//===--------------------------------------------------------------------===//
static void RenderLevel(const char *column, const unique_ptr<vector<string>> &values, vector<string> &out) {
	if (!values) {
		return;
	}
	vector<string> quoted;
	for (auto &value : *values) {
		quoted.push_back("'" + value + "'");
	}
	out.push_back(string(column) + " IN (" + StringUtil::Join(quoted, ", ") + ")");
}

InsertionOrderPreservingMap<string> CatalogFunctionPushdown::ToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	result["Function"] = StringUtil::Upper(input.table_function.name.GetIdentifierName());
	auto &data = input.bind_data->Cast<CatalogScanBindData>();
	auto &filter = data.filter;
	if (!data.databases && !filter.schemas && !filter.tables) {
		return result;
	}
	vector<string> parts;
	RenderLevel("database_name", data.databases, parts);
	RenderLevel("schema_name", filter.schemas, parts);
	RenderLevel(data.entry_column.c_str(), filter.tables, parts);
	result["Catalog Filters"] = StringUtil::Join(parts, " AND ");
	return result;
}

//===--------------------------------------------------------------------===//
// Enumeration
//===--------------------------------------------------------------------===//
void CatalogFunctionPushdown::ScanEntries(ClientContext &context, const TableFunctionInitInput &input, CatalogType type,
                                          const std::function<void(CatalogEntry &)> &callback) {
	auto &data = input.bind_data->Cast<CatalogScanBindData>();
	vector<reference<Catalog>> catalogs;
	if (data.databases) {
		for (auto &name : *data.databases) {
			auto catalog = Catalog::GetCatalogEntry(context, Identifier(name));
			if (catalog && catalog->GetAttached().GetVisibility() != AttachVisibility::HIDDEN) {
				catalogs.push_back(*catalog);
			}
		}
	} else {
		for (auto &database : DatabaseManager::Get(context).GetDatabases(context)) {
			if (database->GetVisibility() == AttachVisibility::HIDDEN) {
				continue;
			}
			catalogs.push_back(database->GetCatalog());
		}
	}
	// same order as GetAllSchemas
	std::sort(catalogs.begin(), catalogs.end(),
	          [](reference<Catalog> a, reference<Catalog> b) { return a.get().GetName() < b.get().GetName(); });
	for (auto &catalog : catalogs) {
		catalog.get().ScanEntries(context, type, data.filter, callback);
	}
}

} // namespace duckdb
