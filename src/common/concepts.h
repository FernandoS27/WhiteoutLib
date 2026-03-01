#include <concepts>
#include <span>
#include <type_traits>

namespace whiteout {
namespace common {

template<typename C>
concept TrivialContiguousRange =
    requires(const C& c) {
        std::data(c);
        std::size(c);
    } &&
    std::is_trivially_copyable_v<
        std::remove_cv_t<
            std::remove_pointer_t<decltype(std::data(std::declval<C&>()))>
        >
    >;

template<typename T>
concept BinaryBlob =
    std::is_trivially_copyable_v<T> &&
    std::is_standard_layout_v<T> &&
    !TrivialContiguousRange<T>;

template<typename T>
concept BinaryInteger =
    std::is_integral_v<T> && !std::is_same_v<T, bool>;

template<typename T>
concept BinaryFloat =
    std::is_floating_point_v<T>;

template<typename T>
concept BinaryScalar =
    std::is_trivially_copyable_v<T> &&
    (std::is_scalar_v<T> || std::is_enum_v<T>) &&
    !std::is_same_v<T, bool>;

} // namespace common
} // namespace whiteout
