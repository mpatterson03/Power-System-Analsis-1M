// Copyright (c) 2023, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#ifndef MAMBA_SOLV_POOL_HPP
#define MAMBA_SOLV_POOL_HPP

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "solv-cpp/ids.hpp"
#include "solv-cpp/queue.hpp"
#include "solv-cpp/repo.hpp"
#include "solv-cpp/solvable.hpp"

namespace mamba::solv
{
    /**
     * Pool of solvable involved in resolving en environment.
     *
     * The pool contains the solvable (packages) information required from the ``::Solver``.
     * The pool can be reused by multiple solvers to solve differents requirements with the same
     * ecosystem.
     */
    class ObjPool
    {
    public:

        ObjPool();
        ~ObjPool();

        auto raw() -> ::Pool*;
        auto raw() const -> const ::Pool*;

        auto find_string(std::string_view str) const -> std::optional<StringId>;
        auto add_string(std::string_view str) -> StringId;
        auto get_string(StringId id) const -> std::string_view;

        auto find_dependency(StringId name_id, RelationFlag flag, StringId version_id) const
            -> std::optional<DependencyId>;
        auto add_dependency(StringId name_id, RelationFlag flag, StringId version_id) -> DependencyId;
        auto get_dependency_name(DependencyId id) const -> std::string_view;
        auto get_dependency_version(DependencyId id) const -> std::string_view;
        auto get_dependency_relation(DependencyId id) const -> std::string_view;
        auto dependency_to_string(DependencyId id) const -> std::string;

        void create_whatprovides();
        template <typename UnaryFunc>
        void for_each_whatprovides_id(DependencyId dep, UnaryFunc func) const;
        template <typename UnaryFunc>
        void for_each_whatprovides(DependencyId dep, UnaryFunc func) const;
        template <typename UnaryFunc>
        void for_each_whatprovides(DependencyId dep, UnaryFunc func);

        auto select_solvables(const ObjQueue& job) const -> ObjQueue;

        auto add_repo(std::string_view name) -> RepoId;
        auto get_repo(RepoId id) -> ObjRepoView;
        auto get_repo(RepoId id) const -> ObjRepoViewConst;
        auto n_repos() const -> std::size_t;
        void remove_repo(RepoId id, bool reuse_ids);
        template <typename UnaryFunc>
        void for_each_repo_id(UnaryFunc func) const;
        template <typename UnaryFunc>
        void for_each_repo(UnaryFunc func);
        template <typename UnaryFunc>
        void for_each_repo(UnaryFunc func) const;

        auto get_solvable(SolvableId id) const -> ObjSolvableViewConst;
        auto get_solvable(SolvableId id) -> ObjSolvableView;
        template <typename UnaryFunc>
        void for_each_solvable_id(UnaryFunc func) const;
        template <typename UnaryFunc>
        void for_each_solvable(UnaryFunc func) const;
        template <typename UnaryFunc>
        void for_each_solvable(UnaryFunc func);

    private:

        struct PoolDeleter
        {
            void operator()(::Pool* ptr);
        };

        std::unique_ptr<::Pool, ObjPool::PoolDeleter> m_pool;
    };
}

/*******************************
 *  Implementation of ObjPool  *
 *******************************/

#include <solv/pool.h>

namespace mamba::solv
{

    template <typename UnaryFunc>
    void ObjPool::for_each_repo_id(UnaryFunc func) const
    {
        const ::Pool* const pool = raw();
        const ::Repo* repo = nullptr;
        RepoId repo_id = 0;
        FOR_REPOS(repo_id, repo)
        {
            func(repo_id);
        }
    }

    template <typename UnaryFunc>
    void ObjPool::for_each_repo(UnaryFunc func) const
    {
        return for_each_repo_id([this, func](RepoId id) { func(get_repo(id)); });
    }

    template <typename UnaryFunc>
    void ObjPool::for_each_repo(UnaryFunc func)
    {
        return for_each_repo_id([this, func](RepoId id) { func(get_repo(id)); });
    }

    template <typename UnaryFunc>
    void ObjPool::for_each_whatprovides_id(DependencyId dep, UnaryFunc func) const
    {
        auto* const pool = const_cast<::Pool*>(raw());
        SolvableId id = 0;
        ::Id offset = 0;  // Not really an Id
        FOR_PROVIDES(id, offset, dep)
        {
            func(id);
        }
    }

    template <typename UnaryFunc>
    void ObjPool::for_each_whatprovides(DependencyId dep, UnaryFunc func) const
    {
        return for_each_whatprovides_id(dep, [this, func](SolvableId id) { func(get_solvable(id)); });
    }

    template <typename UnaryFunc>
    void ObjPool::for_each_whatprovides(DependencyId dep, UnaryFunc func)
    {
        return for_each_whatprovides_id(dep, [this, func](SolvableId id) { func(get_solvable(id)); });
    }

    template <typename UnaryFunc>
    void ObjPool::for_each_solvable_id(UnaryFunc func) const
    {
        const ::Pool* const pool = raw();
        SolvableId id = 0;
        FOR_POOL_SOLVABLES(id)
        {
            func(id);
        }
    }

    template <typename UnaryFunc>
    void ObjPool::for_each_solvable(UnaryFunc func) const
    {
        return for_each_solvable_id([this, func](SolvableId id) { func(get_solvable(id)); });
    }

    template <typename UnaryFunc>
    void ObjPool::for_each_solvable(UnaryFunc func)
    {
        return for_each_solvable_id([this, func](SolvableId id) { func(get_solvable(id)); });
    }
}
#endif
