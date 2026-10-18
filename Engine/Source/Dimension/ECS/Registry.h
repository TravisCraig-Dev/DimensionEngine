#pragma once
#include "Dimension/ECS/ComponentPool.h"
#include <array>
#include <tuple>

namespace Dimension
{
    template <class... Ts> class View;

    // Owns entities and their components. Component storage is one sparse set per type.
    //
    // Note: adding a component of type T may reallocate T's storage, which invalidates
    // references to other T components. Re-fetch after structural changes.
    class Registry
    {
    public:
        Registry() = default;
        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;
        Registry(Registry&&) noexcept = default;
        Registry& operator=(Registry&&) noexcept = default;

        Entity Create()
        {
            uint32_t index;
            if (!m_FreeList.empty())
            {
                index = m_FreeList.back();
                m_FreeList.pop_back();
            }
            else
            {
                index = (uint32_t)m_Generations.size();
                m_Generations.push_back(0);
                m_AliveFlags.push_back(false);
            }
            m_AliveFlags[index] = true;
            ++m_AliveCount;
            return { index, m_Generations[index] };
        }

        void Destroy(Entity e)
        {
            if (!Valid(e)) return;
            for (auto& pool : m_Pools)
                if (pool) pool->Remove(e.Index);
            m_AliveFlags[e.Index] = false;
            ++m_Generations[e.Index];
            m_FreeList.push_back(e.Index);
            --m_AliveCount;
        }

        bool Valid(Entity e) const
        {
            return e.Index < m_Generations.size() && m_AliveFlags[e.Index] && m_Generations[e.Index] == e.Generation;
        }

        // Rebuilds a full handle from a raw index (used by views and GPU picking).
        Entity EntityFromIndex(uint32_t index) const
        {
            if (index >= m_Generations.size() || !m_AliveFlags[index]) return Entity::Null();
            return { index, m_Generations[index] };
        }

        size_t AliveCount() const { return m_AliveCount; }

        template <class T, class... Args>
        T& Emplace(Entity e, Args&&... args)
        {
            DIM_ASSERT(Valid(e), "Emplace on invalid entity");
            return Pool<T>().Emplace(e.Index, std::forward<Args>(args)...);
        }

        template <class T, class... Args>
        T& EmplaceOrReplace(Entity e, Args&&... args)
        {
            if (T* existing = TryGet<T>(e))
            {
                *existing = T{ std::forward<Args>(args)... };
                return *existing;
            }
            return Emplace<T>(e, std::forward<Args>(args)...);
        }

        template <class T> void Remove(Entity e)
        {
            if (Valid(e))
                if (auto* pool = FindPool<T>()) pool->Remove(e.Index);
        }

        template <class T> bool Has(Entity e) const
        {
            auto* pool = FindPool<T>();
            return pool && Valid(e) && pool->Contains(e.Index);
        }

        template <class T> T& Get(Entity e)
        {
            T* c = TryGet<T>(e);
            DIM_ASSERT(c, "Entity does not have the requested component");
            return *c;
        }

        template <class T> T* TryGet(Entity e)
        {
            auto* pool = FindPool<T>();
            return (pool && Valid(e)) ? pool->TryGet(e.Index) : nullptr;
        }
        template <class T> const T* TryGet(Entity e) const { return const_cast<Registry*>(this)->TryGet<T>(e); }
        template <class T> const T& Get(Entity e) const { return const_cast<Registry*>(this)->Get<T>(e); }

        // Type-erased access for reflection-driven code (inspector, serializer).
        void* GetRaw(ComponentTypeId type, Entity e)
        {
            if (type >= m_Pools.size() || !m_Pools[type] || !Valid(e)) return nullptr;
            return m_Pools[type]->GetRaw(e.Index);
        }
        void RemoveRaw(ComponentTypeId type, Entity e)
        {
            if (type < m_Pools.size() && m_Pools[type] && Valid(e)) m_Pools[type]->Remove(e.Index);
        }

        template <class... Ts> View<Ts...> ViewOf();

        // Calls fn(Entity) for every live entity.
        template <class F> void EachEntity(F&& fn) const
        {
            for (uint32_t i = 0; i < (uint32_t)m_Generations.size(); ++i)
                if (m_AliveFlags[i]) fn(Entity{ i, m_Generations[i] });
        }

        // Deep copy, preserving entity handles. Used to snapshot the edit-time scene
        // when entering play mode, so Entity references inside components stay valid.
        Registry Clone() const
        {
            Registry r;
            r.m_Generations = m_Generations;
            r.m_AliveFlags = m_AliveFlags;
            r.m_FreeList = m_FreeList;
            r.m_AliveCount = m_AliveCount;
            r.m_Pools.resize(m_Pools.size());
            for (size_t i = 0; i < m_Pools.size(); ++i)
                if (m_Pools[i]) r.m_Pools[i] = m_Pools[i]->Clone();
            return r;
        }

        void Clear() { *this = Registry(); }

        template <class T> ComponentPool<T>& Pool()
        {
            ComponentTypeId id = GetComponentTypeId<T>();
            if (id >= m_Pools.size()) m_Pools.resize(size_t(id) + 1);
            if (!m_Pools[id]) m_Pools[id] = MakeScope<ComponentPool<T>>();
            return static_cast<ComponentPool<T>&>(*m_Pools[id]);
        }

        template <class T> ComponentPool<T>* FindPool() const
        {
            ComponentTypeId id = GetComponentTypeId<T>();
            return id < m_Pools.size() ? static_cast<ComponentPool<T>*>(m_Pools[id].get()) : nullptr;
        }

    private:
        std::vector<uint32_t> m_Generations;
        std::vector<bool> m_AliveFlags;
        std::vector<uint32_t> m_FreeList;
        std::vector<Scope<IComponentPool>> m_Pools;
        size_t m_AliveCount = 0;
    };

    // Iterates entities that have all of Ts. Drives iteration from the smallest pool.
    //   for (auto [e, transform, sprite] : registry.ViewOf<Transform, Sprite>()) { ... }
    // Do not add/remove Ts components while range-iterating; use Each() for that,
    // which iterates backwards and tolerates removing the current entity's components.
    template <class... Ts>
    class View
    {
    public:
        explicit View(Registry& registry) : m_Registry(&registry), m_Pools{ registry.FindPool<Ts>()... }
        {
            m_Lead = nullptr;
            for (IComponentPool* p : m_PoolsErased())
            {
                if (!p) { m_Lead = nullptr; m_Empty = true; return; }
                if (!m_Lead || p->Size() < m_Lead->Size()) m_Lead = p;
            }
        }

        template <class F> void Each(F&& fn)
        {
            if (m_Empty || !m_Lead) return;
            for (size_t i = m_Lead->Size(); i-- > 0;)
            {
                if (i >= m_Lead->Size()) continue;
                uint32_t idx = m_Lead->DenseEntities()[i];
                if (!AllContain(idx)) continue;
                fn(m_Registry->EntityFromIndex(idx), std::get<ComponentPool<Ts>*>(m_Pools)->Get(idx)...);
            }
        }

        size_t SizeHint() const { return m_Lead ? m_Lead->Size() : 0; }

        class Iterator
        {
        public:
            Iterator(View* view, size_t pos) : m_View(view), m_Pos(pos) { Skip(); }
            auto operator*() const
            {
                uint32_t idx = m_View->m_Lead->DenseEntities()[m_Pos];
                return std::tuple<Entity, Ts&...>(m_View->m_Registry->EntityFromIndex(idx),
                                                  std::get<ComponentPool<Ts>*>(m_View->m_Pools)->Get(idx)...);
            }
            Iterator& operator++() { ++m_Pos; Skip(); return *this; }
            bool operator!=(const Iterator& o) const { return m_Pos != o.m_Pos; }

        private:
            void Skip()
            {
                while (m_Pos < End() && !m_View->AllContain(m_View->m_Lead->DenseEntities()[m_Pos])) ++m_Pos;
            }
            size_t End() const { return m_View->m_Lead ? m_View->m_Lead->Size() : 0; }
            View* m_View;
            size_t m_Pos;
        };

        Iterator begin() { return Iterator(this, 0); }
        Iterator end() { return Iterator(this, (m_Empty || !m_Lead) ? 0 : m_Lead->Size()); }

    private:
        std::array<IComponentPool*, sizeof...(Ts)> m_PoolsErased() const
        {
            return { static_cast<IComponentPool*>(std::get<ComponentPool<Ts>*>(m_Pools))... };
        }
        bool AllContain(uint32_t idx) const
        {
            return (std::get<ComponentPool<Ts>*>(m_Pools)->Contains(idx) && ...);
        }

        Registry* m_Registry;
        std::tuple<ComponentPool<Ts>*...> m_Pools;
        IComponentPool* m_Lead = nullptr;
        bool m_Empty = false;
    };

    template <class... Ts> View<Ts...> Registry::ViewOf() { return View<Ts...>(*this); }
}
