#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnsView.h>
#include <gtest/gtest.h>

namespace DB
{
namespace
{

const IColumn * identity(const IColumn * column, const void *)
{
    return column;
}

/// Maps every source column to the column in `context`.
const IColumn * mapToContext(const IColumn *, const void * context)
{
    return static_cast<const IColumn *>(context);
}

ColumnRawPtrs collect(const ColumnsView & columns)
{
    ColumnRawPtrs result;
    columns.forEach([&](const IColumn * column) { result.push_back(column); });
    return result;
}

TEST(ColumnsView, ProjectedViewIntrospection)
{
    auto first_column = ColumnUInt64::create();
    auto second_column = ColumnUInt64::create();
    auto mapped_column = ColumnUInt64::create();
    const IColumn * first_column_ptr = first_column.get();
    const IColumn * mapped_column_ptr = mapped_column.get();

    Columns columns;
    columns.emplace_back(std::move(first_column));
    columns.emplace_back(std::move(second_column));
    ColumnsView source_columns(columns);

    auto projected = source_columns.project(mapToContext, mapped_column_ptr);
    EXPECT_FALSE(projected.empty());
    EXPECT_EQ(projected.size(), 2u);
    EXPECT_FALSE(projected.tryGetSingle().has_value());
    EXPECT_EQ(collect(projected), (ColumnRawPtrs{mapped_column_ptr, mapped_column_ptr}));

    /// A projection of a projection of a single column.
    ColumnsView single_column(columns.front());
    auto projected_single = single_column.project(identity);
    auto projected_twice = projected_single.project(mapToContext, mapped_column_ptr);
    EXPECT_FALSE(projected_twice.empty());
    EXPECT_EQ(projected_twice.size(), 1u);
    ASSERT_TRUE(projected_single.tryGetSingle().has_value());
    EXPECT_EQ(*projected_single.tryGetSingle(), first_column_ptr);
    ASSERT_TRUE(projected_twice.tryGetSingle().has_value());
    EXPECT_EQ(*projected_twice.tryGetSingle(), mapped_column_ptr);

    /// A projection of no columns.
    ColumnRawPtrs no_column_ptrs;
    ColumnsView no_columns(no_column_ptrs);
    auto projected_empty = no_columns.project(identity);
    EXPECT_TRUE(projected_empty.empty());
    EXPECT_EQ(projected_empty.size(), 0u);
    EXPECT_FALSE(projected_empty.tryGetSingle().has_value());
}

TEST(ColumnsView, GroupColumns)
{
    /// Source columns of sizes 0, 1, 2, 3.
    Columns columns;
    ColumnRawPtrs column_ptrs;
    for (size_t size = 0; size != 4; ++size)
    {
        auto column = ColumnUInt64::create();
        for (size_t i = 0; i != size; ++i)
            column->insertValue(i);
        column_ptrs.push_back(column.get());
        columns.emplace_back(std::move(column));
    }

    ColumnsView source_columns(columns);

    /// Group 0: the columns of even size, group 1: the columns of odd size, group 2: all columns, group 3: no columns.
    auto groups = groupColumns(
        source_columns,
        4,
        [](const IColumn * column, auto && emit)
        {
            emit(column->size() % 2, column);
            emit(2, column);
        });
    ASSERT_EQ(groups.size(), 4u);
    EXPECT_EQ(collect(groups[0]), (ColumnRawPtrs{column_ptrs[0], column_ptrs[2]}));
    EXPECT_EQ(collect(groups[1]), (ColumnRawPtrs{column_ptrs[1], column_ptrs[3]}));
    EXPECT_EQ(collect(groups[2]), column_ptrs);
    EXPECT_EQ(groups[2].size(), 4u);
    EXPECT_FALSE(groups[2].tryGetSingle().has_value());
    EXPECT_TRUE(groups[3].empty());
    EXPECT_EQ(groups[3].size(), 0u);
    EXPECT_FALSE(groups[3].tryGetSingle().has_value());

    /// The source columns can be any view, for example a projected one, and a source column may have no children.
    auto non_empty_groups = groupColumns(
        source_columns.project(identity),
        2,
        [](const IColumn * column, auto && emit)
        {
            if (!column->empty())
                emit(column->size() % 2, column);
        });
    ASSERT_EQ(non_empty_groups.size(), 2u);
    ASSERT_TRUE(non_empty_groups[0].tryGetSingle().has_value());
    EXPECT_EQ(*non_empty_groups[0].tryGetSingle(), column_ptrs[2]);
    EXPECT_EQ(collect(non_empty_groups[1]), (ColumnRawPtrs{column_ptrs[1], column_ptrs[3]}));

    /// Without groups the children of the source columns are not visited.
    size_t visited = 0;
    auto no_groups = groupColumns(source_columns, 0, [&](const IColumn *, auto &&) { ++visited; });
    EXPECT_EQ(no_groups.size(), 0u);
    EXPECT_EQ(visited, 0u);
}
}
}
