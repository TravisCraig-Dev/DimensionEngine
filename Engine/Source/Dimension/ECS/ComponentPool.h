#pragma once
#include "Dimension/Core/Base.h"
#include "Dimension/ECS/Entity.h"
#include <cassert>

namespace Dimension
{
    // Type-erased sparse set. Maps entity index -> dense slot. Components of one type
    // live contiguously in the dense array, so iteration is cache friendly.
    class IComponentPool
    {
    public:
        static constexpr uint32_t kEmpty = std::numeric_limits<uint32_t>::max();

        virtual ~IComponentPool() = default;
        virtual void Remove(uint32_t entityIndex) = 0;
        virtual void* GetRaw(uint32_t entityIndex) = 0;
        virtual Scope<IComponentPool> Clone() const = 0;

        bool Contains(uint32_t entityIndex) const
        {
            return entityIndex < m_Sparse.size() && m_Sparse[entityIndex] != kEmpty;
        }
        size_t Size() const { return m_Dense.size(); }
        const std::vector<uint32_t>& DenseEntities() const { return m_Dense; }

    protected:
        uint32_t AddSlot(uint32_t entityIndex)
        {
            if (entityIndex >= m_Sparse.size()) m_Sparse.resize(size_t(entityIndex) + 1, kEmpty);
            m_Sparse[entityIndex] = (uint32_t)m_Dense.size();
            m_Dense.push_back(entityIndex);
            return m_Sparse[entityIndex];
        }

        std::vector<uint32_t> m_Sparse;
        std::vector<uint32_t> m_Dense;
    };

    template <class T>
    class ComponentPool final : public IComponentPool
    {
    public:
        template <class... Args>
        T& Emplace(uint32_t entityIndex, Args&&... args)
        {
            assert(!Contains(entityIndex));
            AddSlot(entityIndex);
            if constexpr (std::is_aggregate_v<T>)
                return m_Data.emplace_back(T{ std::forward<Args>(args)... });
            else
                return m_Data.emplace_back(std::forward<Args>(args)...);
        }

        T& Get(uint32_t entityIndex)
        {
            assert(Contains(entityIndex));
            return m_Data[m_Sparse[entityIndex]];
        }

        T* TryGet(uint32_t entityIndex) { return Contains(entityIndex) ? &m_Data[m_Sparse[entityIndex]] : nullptr; }

        void Remove(uint32_t entityIndex) override
        {
            if (!Contains(entityIndex)) return;
            uint32_t slot = m_Sparse[entityIndex];
            uint32_t last = (uint32_t)m_Dense.size() - 1;
            if (slot != last)
            {
                m_Data[slot] = std::move(m_Data[last]);
                m_Dense[slot] = m_Dense[last];
                m_Sparse[m_Dense[slot]] = slot;
            }
            m_Data.pop_back();
            m_Dense.pop_back();
            m_Sparse[entityIndex] = kEmpty;
        }

        void* GetRaw(uint32_t entityIndex) override { return TryGet(entityIndex); }

        Scope<IComponentPool> Clone() const override
        {
            if constexpr (std::is_copy_constructible_v<T>)
            {
                auto copy = MakeScope<ComponentPool<T>>();
                copy->m_Sparse = m_Sparse;
                copy->m_Dense = m_Dense;
                copy->m_Data = m_Data;
                return copy;
            }
            else
            {
                assert(false && "Component type is not copyable");
                return MakeScope<ComponentPool<T>>();
            }
        }

        std::vector<T>& Data() { return m_Data; }

    private:
        std::vector<T> m_Data;
    };
}
