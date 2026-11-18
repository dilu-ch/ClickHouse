#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnsView.h>
#include <gtest/gtest.h>

namespace DB
{
namespace
{

ColumnRawPtrs collect(const ColumnsView & columns)
{
    ColumnRawPtrs result;
    columns.forEach([&](const IColumn * column) { result.push_back(column); });
    return result;
}

TEST(ColumnsView, StorageKinds)
{
    Columns columns;
    ColumnRawPtrs column_ptrs;
    for (size_t i = 0; i != 20; ++i)
    {
        auto column = ColumnUInt64::create();
        column_ptrs.push_back(column.get());
        columns.emplace_back(std::move(column));
    }

    auto expect_columns = [](const ColumnsView & view, const ColumnRawPtrs & expected)
    {
        EXPECT_EQ(collect(view), expected);
        EXPECT_EQ(view.empty(), expected.empty());
        EXPECT_EQ(view.tryGetSingle(), expected.size() == 1 ? expected.front() : nullptr);
    };

    /// Smart pointers.
    expect_columns(columns, column_ptrs);
    VectorWithMemoryTracking<ColumnPtr> tracked_columns(columns.begin(), columns.begin() + 2);
    expect_columns(tracked_columns, {column_ptrs[0], column_ptrs[1]});
    expect_columns(columns.front(), {column_ptrs[0]});
    const Columns no_columns;
    expect_columns(no_columns, {});

    /// Raw pointers.
    expect_columns(column_ptrs, column_ptrs);
    expect_columns(ColumnsView(std::span<const IColumn * const>(column_ptrs.data() + 3, 2)), {column_ptrs[3], column_ptrs[4]});
    expect_columns(column_ptrs[5], {column_ptrs[5]});
    expect_columns(ColumnsView(std::span<const IColumn * const>()), {});

    /// Raw pointers with stack memory, inline and after growing out of the stack memory.
    ColumnRawPtrsWithStackMemory stack_column_ptrs;
    expect_columns(stack_column_ptrs, {});
    for (size_t i = 0; i != column_ptrs.size(); ++i)
    {
        stack_column_ptrs.push_back(column_ptrs[i]);
        expect_columns(stack_column_ptrs, ColumnRawPtrs(column_ptrs.begin(), column_ptrs.begin() + i + 1));
    }
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
    EXPECT_EQ(groups[2].tryGetSingle(), nullptr);
    EXPECT_TRUE(groups[3].empty());
    EXPECT_EQ(groups[3].tryGetSingle(), nullptr);

    /// The source columns can be any view, and a source column may have no children.
    auto non_empty_groups = groupColumns(
        ColumnsView(column_ptrs),
        2,
        [](const IColumn * column, auto && emit)
        {
            if (!column->empty())
                emit(column->size() % 2, column);
        });
    ASSERT_EQ(non_empty_groups.size(), 2u);
    EXPECT_EQ(non_empty_groups[0].tryGetSingle(), column_ptrs[2]);
    EXPECT_EQ(collect(non_empty_groups[1]), (ColumnRawPtrs{column_ptrs[1], column_ptrs[3]}));

    /// Without groups the children of the source columns are not visited.
    size_t visited = 0;
    auto no_groups = groupColumns(source_columns, 0, [&](const IColumn *, auto &&) { ++visited; });
    EXPECT_EQ(no_groups.size(), 0u);
    EXPECT_EQ(visited, 0u);
}

/// The non-null contract of the view is a `chassert`, which aborts only in debug and sanitizer builds: hence a death test.
#ifdef DEBUG_OR_SANITIZER_BUILD

TEST(ColumnsViewDeathTest, NullColumnsAreRejected)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";

    auto column = ColumnUInt64::create();

    ColumnRawPtrs null_raw_ptrs{column.get(), nullptr};
    EXPECT_DEATH(collect(ColumnsView(null_raw_ptrs)), "");

    Columns null_column_ptrs(1);
    EXPECT_DEATH(collect(ColumnsView(null_column_ptrs)), "");

    const IColumn * null_column = nullptr;
    EXPECT_DEATH({ ColumnsView view(null_column); }, "");

    ColumnRawPtrsWithStackMemory null_stack_ptrs;
    null_stack_ptrs.push_back(nullptr);
    EXPECT_DEATH(collect(ColumnsView(null_stack_ptrs)), "");
}

#endif
}
}
