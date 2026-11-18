#pragma once

#include <Columns/IColumn_fwd.h>
#include <base/defines.h>

#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace DB
{

/// Non-owning borrowed view over source columns.
/// The referenced storage must outlive the call that receives this view.
/// Do not store `ColumnsView` or pointers obtained from it.
/// The view is expected to yield non-null `IColumn` pointers.
class ColumnsView
{
public:
    /// Mappers are evaluated lazily and can run multiple times while traversing nested views.
    /// They must be cheap and pure, and must not return pointers to temporary objects.
    using Mapper = const IColumn * (*)(const IColumn *, const void *);

    ColumnsView(const Columns & columns_) // NOLINT(google-explicit-constructor)
        : storage(ColumnPtrRange{columns_.data(), columns_.size()})
    {
    }

    ColumnsView(Columns &&) = delete;

    ColumnsView(const ColumnPtr & column_) // NOLINT(google-explicit-constructor)
        : storage(ColumnPtrRange{&column_, 1})
    {
    }

    ColumnsView(ColumnPtr &&) = delete;

    ColumnsView(const VectorWithMemoryTracking<ColumnPtr> & columns_) // NOLINT(google-explicit-constructor)
        : storage(ColumnPtrRange{columns_.data(), columns_.size()})
    {
    }

    ColumnsView(VectorWithMemoryTracking<ColumnPtr> &&) = delete;

    ColumnsView(const ColumnRawPtrs & columns_) // NOLINT(google-explicit-constructor)
        : storage(RawPtrRange{columns_.data(), columns_.size()})
    {
    }

    ColumnsView(ColumnRawPtrs &&) = delete;

    explicit ColumnsView(std::span<const IColumn * const> columns_)
        : storage(RawPtrRange{columns_.data(), columns_.size()})
    {
    }

    ColumnsView(const IColumn * column_) // NOLINT(google-explicit-constructor)
        : storage(SingleRawPtr{column_})
    {
    }

    ColumnsView(const ColumnsView &) = default;
    /// A projected view points to its base, so `view = view.project(...)` would make the view its own base.
    ColumnsView & operator=(const ColumnsView &) = delete;

    /// The mapper must return a non-null `IColumn` pointer.
    /// To select the children of the source columns by key, where some source columns may have no child for a key, use
    /// `groupColumns`.
    ColumnsView project(Mapper mapper_, const void * context_ = nullptr) const &
    {
        return ColumnsView(ProjectedRange{this, mapper_, context_});
    }

    ColumnsView project(Mapper, const void * = nullptr) const && = delete;

    /// Calls `callback(column)` for every column of the view, in order.
    template <typename Callback>
    void forEach(Callback && callback) const
    {
        static_assert(std::is_void_v<std::invoke_result_t<Callback &, const IColumn *>>, "The callback of forEach must return void");

        if (forEachStoredColumn(callback))
            return;

        using CallbackType = std::remove_reference_t<Callback>;
        forEachImpl(
            [](const IColumn * column, void * context) { (*static_cast<CallbackType *>(context))(column); },
            const_cast<void *>(static_cast<const void *>(std::addressof(callback))));
    }

    /// The only column of the view, or `nullptr` if the view doesn't have exactly one column.
    [[nodiscard]] const IColumn * tryGetSingle() const
    {
        if (const auto * projected = std::get_if<ProjectedRange>(&storage))
        {
            const IColumn * base_column = projected->base->tryGetSingle();
            return base_column ? projected->mapper(base_column, projected->context) : nullptr;
        }

        if (const auto * single = std::get_if<SingleRawPtr>(&storage))
            return single->column;

        if (const auto * range = std::get_if<ColumnPtrRange>(&storage))
            return range->num_columns == 1 ? range->columns[0].get() : nullptr;

        const auto & range = std::get<RawPtrRange>(storage);
        return range.num_columns == 1 ? range.columns[0] : nullptr;
    }

    [[nodiscard]] bool empty() const
    {
        if (const auto * projected = std::get_if<ProjectedRange>(&storage))
            return projected->base->empty();

        if (std::holds_alternative<SingleRawPtr>(storage))
            return false;

        if (const auto * range = std::get_if<ColumnPtrRange>(&storage))
            return range->num_columns == 0;

        return std::get<RawPtrRange>(storage).num_columns == 0;
    }

private:
    struct ColumnPtrRange
    {
        const ColumnPtr * columns = nullptr;
        size_t num_columns = 0;
    };

    struct RawPtrRange
    {
        const IColumn * const * columns = nullptr;
        size_t num_columns = 0;
    };

    struct SingleRawPtr
    {
        const IColumn * column = nullptr;
    };

    struct ProjectedRange
    {
        const ColumnsView * base;
        Mapper mapper;
        const void * context;
    };

    using Storage = std::variant<RawPtrRange, ColumnPtrRange, SingleRawPtr, ProjectedRange>;

    explicit ColumnsView(ProjectedRange projected_)
        : storage(projected_)
    {
    }

    /// Calls `callback` for every column if the view stores its columns, and returns false if it is a projected view.
    template <typename Callback>
    bool forEachStoredColumn(Callback & callback) const
    {
        if (const auto * range = std::get_if<ColumnPtrRange>(&storage))
        {
            for (size_t i = 0; i != range->num_columns; ++i)
                callback(range->columns[i].get());
            return true;
        }

        if (const auto * range = std::get_if<RawPtrRange>(&storage))
        {
            for (size_t i = 0; i != range->num_columns; ++i)
                callback(range->columns[i]);
            return true;
        }

        if (const auto * single = std::get_if<SingleRawPtr>(&storage))
        {
            callback(single->column);
            return true;
        }

        return false;
    }

    using ForEachCallback = void (*)(const IColumn *, void *);

    /// Type-erased traversal, so that nested projections don't instantiate a template per nesting level.
    void forEachImpl(ForEachCallback callback, void * callback_context) const
    {
        if (const auto * projected = std::get_if<ProjectedRange>(&storage))
        {
            struct ProjectedCallbackContext
            {
                Mapper mapper;
                const void * mapper_context;
                ForEachCallback callback;
                void * callback_context;
            };

            ProjectedCallbackContext context{projected->mapper, projected->context, callback, callback_context};
            projected->base->forEachImpl(
                [](const IColumn * column, void * opaque_context)
                {
                    const auto & typed_context = *static_cast<const ProjectedCallbackContext *>(opaque_context);
                    typed_context.callback(typed_context.mapper(column, typed_context.mapper_context), typed_context.callback_context);
                },
                &context);
            return;
        }

        auto invoke = [&](const IColumn * column) { callback(column, callback_context); };
        forEachStoredColumn(invoke);
    }

    Storage storage;
};

/// Children of source columns grouped by key, for example the columns of every `JSON` path in the source `JSON` columns.
/// Unlike `ColumnsView`, it owns its storage: every group is a `ColumnsView` into one buffer of this object.
/// The source columns must outlive this object, and the views must not outlive it. Built by `groupColumns`.
class GroupedColumns
{
public:
    /// The number of groups.
    [[nodiscard]] size_t size() const { return group_offsets.size() - 1; }

    /// The columns of `group`, in the order of the source columns.
    ColumnsView operator[](size_t group) const &
    {
        chassert(group < size());
        return ColumnsView(
            std::span<const IColumn * const>(columns.data() + group_offsets[group], group_offsets[group + 1] - group_offsets[group]));
    }

    ColumnsView operator[](size_t) const && = delete;

private:
    explicit GroupedColumns(size_t num_groups)
        : group_offsets(num_groups + 1, 0)
    {
    }

    template <typename Children>
    friend GroupedColumns groupColumns(const ColumnsView & source_columns, size_t num_groups, Children && children);

    /// The columns of group `i` are in [group_offsets[i], group_offsets[i + 1]).
    VectorWithMemoryTracking<const IColumn *> columns;
    VectorWithMemoryTracking<size_t> group_offsets;
};

/// Groups the children of the source columns by key in one pass over the children that exist.
/// `children(source_column, emit)` must call `emit(group, child)` for every child of `source_column` that belongs to a
/// group, with `group` < `num_groups` and non-null `child`. A source column may have children in any number of groups.
/// Every group keeps the order of the source columns.
///
/// Use it when the children are selected by key (e.g. by name in a hash map) for many keys: looking up every key in every
/// source costs keys * sources lookups, mostly misses when every source has only a few of the keys, while grouping costs
/// one lookup per existing child. The groups are materialized, so traversing them doesn't repeat the lookups.
template <typename Children>
GroupedColumns groupColumns(const ColumnsView & source_columns, size_t num_groups, Children && children)
{
    GroupedColumns result(num_groups);
    if (num_groups == 0)
        return result;

    /// Collect the children with their groups in one pass, so that `children` (usually a lookup per child) runs once
    /// per child, and then put them into the groups with a stable counting sort.
    VectorWithMemoryTracking<std::pair<size_t, const IColumn *>> children_with_groups;
    /// Usually every group has a child in at least one source column.
    children_with_groups.reserve(num_groups);
    source_columns.forEach(
        [&](const IColumn * source_column)
        {
            children(
                source_column,
                [&](size_t group, const IColumn * child)
                {
                    chassert(group < num_groups);
                    chassert(child);
                    children_with_groups.emplace_back(group, child);
                });
        });

    /// Count the children of every group, then turn the counts into the ends of the groups.
    auto & offsets = result.group_offsets;
    for (const auto & [group, _] : children_with_groups)
        ++offsets[group];
    for (size_t group = 1; group != num_groups; ++group)
        offsets[group] += offsets[group - 1];
    offsets[num_groups] = children_with_groups.size();

    /// Fill backwards, so that every group keeps the order of the source columns and every offset ends at the start of its group.
    result.columns.resize(children_with_groups.size());
    for (auto it = children_with_groups.rbegin(); it != children_with_groups.rend(); ++it)
        result.columns[--offsets[it->first]] = it->second;

    return result;
}
}
