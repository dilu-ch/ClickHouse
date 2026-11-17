#include <Core/Settings.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Storages/StorageView.h>
#include <TableFunctions/ITableFunction.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/TableFunctionView.h>
#include <TableFunctions/registerTableFunctions.h>


namespace DB
{
namespace Setting
{
}

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}


const ASTSelectWithUnionQuery & TableFunctionView::getSelectQuery() const
{
    return *create.select;
}

VectorWithMemoryTracking<size_t> TableFunctionView::skipAnalysisForArguments(const QueryTreeNodePtr &, ContextPtr) const
{
    return {0};
}

void TableFunctionView::parseArguments(const ASTPtr & ast_function, ContextPtr /*context*/)
{
    const auto * function = ast_function->as<ASTFunction>();
    if (function)
    {
        if (auto * select = function->tryGetQueryArgument())
        {
            create.set(create.select, select->clone());
            return;
        }
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS, "Table function '{}' requires a query argument.", getName());
}

namespace
{
    /// The literals a `WITH` clause binds to a name, visible in the query and its subqueries.
    using LiteralsByAlias = std::unordered_map<String, ASTPtr>;

    void collectLiteralAliases(const ASTSelectQuery & select, LiteralsByAlias & literals)
    {
        const auto with = select.with();
        if (!with)
            return;
        for (const auto & child : with->children)
            if (const auto * literal = child->as<ASTLiteral>(); literal && !literal->alias.empty())
                literals[literal->alias] = child;
    }

    /// The function with the arguments bound by `WITH` written out, so that it parses as it runs.
    ASTPtr withLiteralsSubstituted(const ASTPtr & table_function, const LiteralsByAlias & literals)
    {
        if (literals.empty())
            return table_function;

        auto result = table_function->clone();
        auto & function = result->as<ASTFunction &>();
        if (!function.arguments)
            return result;
        for (auto & argument : function.arguments->children)
        {
            const auto * identifier = argument->as<ASTIdentifier>();
            if (!identifier)
                continue;
            if (const auto it = literals.find(identifier->name()); it != literals.end())
            {
                argument = it->second->clone();
                argument->as<ASTLiteral &>().alias.clear();
            }
        }
        return result;
    }

    /// The query is not analyzed: a carrier such as `remote` may run it where tables unknown here exist.
    void checkAccessToCreateTableTargets(const IAST & ast, const ContextPtr & context, LiteralsByAlias literals)
    {
        if (const auto * select = ast.as<ASTSelectQuery>())
            collectLiteralAliases(*select, literals);

        if (const auto * table_expression = ast.as<ASTTableExpression>(); table_expression && table_expression->table_function)
        {
            ITableFunction::checkAccessToCreateTableTarget(withLiteralsSubstituted(table_expression->table_function, literals), context);
            return;
        }
        for (const auto & child : ast.children)
            checkAccessToCreateTableTargets(*child, context, literals);
    }
}

void TableFunctionView::checkAccessToCreateTable(ContextPtr context) const
{
    /// A carrier may persist the query and run it under another identity, so the table functions it reads are targets too.
    checkAccessToCreateTableTargets(*create.select, context, {});
}

ColumnsDescription TableFunctionView::getActualTableStructure(ContextPtr context, bool /*is_insert_query*/) const
{
    chassert(create.select);
    chassert(create.children.size() == 1);
    chassert(create.children[0]->as<ASTSelectWithUnionQuery>());

    SharedHeader sample_block = InterpreterSelectQueryAnalyzer::getSampleBlock(create.children[0], context);

    return ColumnsDescription(sample_block->getNamesAndTypesList());
}

StoragePtr TableFunctionView::executeImpl(
    const ASTPtr & /*ast_function*/, ContextPtr context, const std::string & table_name, ColumnsDescription /*cached_columns*/, bool is_insert_query) const
{
    auto columns = getActualTableStructure(context, is_insert_query);
    auto res = std::make_shared<StorageView>(StorageID(getDatabaseName(), table_name), create, columns, "");
    res->startup();
    return res;
}

void registerTableFunctionView(TableFunctionFactory & factory)
{
    factory.registerFunction<TableFunctionView>({.description = R"DOCS_MD(
Turns a subquery into a table. The function implements views (see [CREATE VIEW](/reference/statements/create/view)). The resulting table does not store data, but only stores the specified `SELECT` query. When reading from the table, ClickHouse executes the query and deletes all unnecessary columns from the result.

## Syntax {#syntax}

```sql
view(subquery)
```

## Arguments {#arguments}

- `subquery` — `SELECT` query.

## Returned value {#returned-value}

- A table.

## Examples {#examples}

Input table:

```text
┌─id─┬─name─────┬─days─┐
│  1 │ January  │   31 │
│  2 │ February │   29 │
│  3 │ March    │   31 │
│  4 │ April    │   30 │
└────┴──────────┴──────┘
```

```sql title="Query"
SELECT * FROM view(SELECT name FROM months);
```

```text title="Response"
┌─name─────┐
│ January  │
│ February │
│ March    │
│ April    │
└──────────┘
```

You can use the `view` function as a parameter of the [remote](/reference/functions/table-functions/remote) and [cluster](/reference/functions/table-functions/cluster) table functions:

```sql title="Query"
SELECT * FROM remote(`127.0.0.1`, view(SELECT a, b, c FROM table_name));
```

```sql title="Query"
SELECT * FROM cluster(`cluster_name`, view(SELECT a, b, c FROM table_name));
```

## Related {#related}

- [View Table Engine](/reference/engines/table-engines/special/view)
)DOCS_MD", .category = FunctionDocumentation::Category::TableFunction}, SecretArgumentsSpec{}, {.allow_readonly = true});
}

}
