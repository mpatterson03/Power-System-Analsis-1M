// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#ifndef MAMBA_PROBLEMS_GRAPH_HPP
#define MAMBA_PROBLEMS_GRAPH_HPP

#include <string>
#include <utility>
#include <vector>
#include <optional>

#include "match_spec.hpp"
#include "union_util.hpp"

extern "C"
{
#include "solv/queue.h"
#include "solv/solver.h"
}

namespace mamba
{
    template <class T, class U>
    class MGraph
    {
    public:
        using node = T;
        using node_id = size_t;
        using node_list = std::vector<node>;
        using edge_info = U;
        using edge_list = std::vector<std::pair<node_id, edge_info>>;
        using adjacency_list = std::vector<edge_list>;
        using cycle_list = std::vector<node_list>;

        const node_list& get_node_list() const;
        const node& get_node(node_id id) const;
        const adjacency_list& get_adj_list() const;
        const edge_list& get_edge_list(node_id id) const;

        node_id add_node(const node& value);
        node_id add_node(node&& value);
        void add_edge(node_id from, node_id to, edge_info info);

        template <class V>
        void update_node(node_id id, V&& value);

        template <class Y>
        bool update_edge_if_present(node_id from, node_id to, Y&& info);

    private:
        template <class V>
        node_id add_node_impl(V&& value);

        node_list m_node_list;
        adjacency_list m_adjacency_list;
    };

    /************************
     * graph implementation *
     ************************/

    template <class T, class U>
    inline auto MGraph<T, U>::get_adj_list() const -> const adjacency_list&
    {
        return m_adjacency_list;
    }

    template <class T, class U>
    inline auto MGraph<T, U>::get_node_list() const -> const node_list&
    {
        return m_node_list;
    }

    template <class T, class U>
    inline auto MGraph<T, U>::get_node(node_id id) const -> const node&
    {
        return m_node_list[id];
    }


    template <class T, class U>
    inline auto MGraph<T, U>::get_edge_list(node_id id) const -> const edge_list&
    {
        return m_adjacency_list[id];
    }

    template <class T, class U>
    inline auto MGraph<T, U>::add_node(const node& value) -> node_id
    {
        return add_node_impl(value);
    }

    template <class T, class U>
    inline auto MGraph<T, U>::add_node(node&& value) -> node_id
    {
        return add_node_impl(std::move(value));
    }

    template <class T, class U>
    inline void MGraph<T, U>::add_edge(node_id from, node_id to, edge_info info)
    {
        m_adjacency_list[from].push_back(std::make_pair(to, info));
    }


    template <class T, class U>
    template <class V>
    inline auto MGraph<T, U>::add_node_impl(V&& value) -> node_id
    {
        m_node_list.push_back(std::forward<V>(value));
        m_adjacency_list.push_back(edge_list());
        return m_node_list.size() - 1u;
    }


    template <class T, class U>
    template <class V>
    inline void MGraph<T, U>::update_node(node_id id, V&& value)
    {
        m_node_list[id].add(value);
    }

    template <class T, class U>
    template <class Y>
    inline bool MGraph<T, U>::update_edge_if_present(node_id from, node_id to, Y&& value)
    {   
        std::vector<std::pair<node_id, edge_info>> edge_list = m_adjacency_list[from];
        for (auto it = edge_list.begin(); it != edge_list.end(); ++it) 
        {
            if (it->first == to) 
            {
                it->second.add(value);
                return true;
            }
        }
        return false;
    }

}  // namespace mamba

#endif  // MAMBA_PROBLEMS_GRAPH_HPP
