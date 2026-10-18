#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <filesystem>

namespace Dimension
{
    template <class T> using Scope = std::unique_ptr<T>;
    template <class T> using Ref = std::shared_ptr<T>;

    template <class T, class... Args> Scope<T> MakeScope(Args&&... args) { return std::make_unique<T>(std::forward<Args>(args)...); }
    template <class T, class... Args> Ref<T> MakeRef(Args&&... args) { return std::make_shared<T>(std::forward<Args>(args)...); }

    namespace fs = std::filesystem;

    [[noreturn]] void FatalError(std::string_view message, const char* file, int line);
}

#define DIM_ASSERT(cond, msg)                                                   \
    do {                                                                        \
        if (!(cond)) ::Dimension::FatalError(msg, __FILE__, __LINE__);          \
    } while (0)
