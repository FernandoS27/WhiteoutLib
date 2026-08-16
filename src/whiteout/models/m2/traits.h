#pragma once

#include <type_traits>

namespace whiteout {
namespace m2 {

/// When specialized to true for a type T, forces the visitor to use
/// per-field visit() even if T is trivially copyable.
template <typename T>
inline constexpr bool force_visit_v = false;

struct Sequence;
template <>
inline constexpr bool force_visit_v<Sequence> = true;

// Trivially copyable, but its on-disk size is version-dependent (vanilla
// sections carry no sort fields), so it must go through the per-field visit
// where the version gate lives.
struct SkinSection;
template <>
inline constexpr bool force_visit_v<SkinSection> = true;

/// True when T can be bulk-read/written as raw bytes.
template <typename T>
inline constexpr bool bulk_copyable_v = std::is_trivially_copyable_v<T> && !force_visit_v<T>;

} // namespace m2
} // namespace whiteout
