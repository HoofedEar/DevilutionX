#pragma once

#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace devilution {

namespace detail {

/**
 * @brief Passes a `char` array on as a C string, ending at its first NUL, and anything else unchanged.
 *
 * Fixed-size buffers such as `Player::_pName` hold a name followed by NULs. libstdc++ formats a `char`
 * array up to the first NUL, but libc++ (Emscripten, Apple, Android) formats the whole array, NULs
 * included, and text rendering stops at the first of those, cutting off the rest of the message.
 */
template <typename T>
decltype(auto) AsFormatArg(T &arg)
{
	if constexpr (std::is_array_v<T> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<T>>, char>)
		return std::string_view(static_cast<const char *>(arg));
	else
		return (arg);
}

/** @brief `std::vformat` with `char` arrays formatted as C strings, see `AsFormatArg`. */
template <typename... Args>
std::string VFormat(std::string_view fmt, Args &...args)
{
	// make_format_args only takes lvalues, so the converted arguments need somewhere to live
	std::tuple<decltype(AsFormatArg(args))...> formatArgs(AsFormatArg(args)...);
	return std::apply([fmt](auto &...formatArg) { return std::vformat(fmt, std::make_format_args(formatArg...)); }, formatArgs);
}

} // namespace detail

/**
 * @brief Formats using a format string that is only known at runtime, such as a translation.
 *
 * Unlike `std::format`, the format string is not checked at compile time.
 * An invalid format string results in `std::format_error` being thrown
 * (or program termination if exceptions are disabled).
 */
template <typename... Args>
[[nodiscard]] std::string FormatRuntime(std::string_view fmt, Args &&...args)
{
	return detail::VFormat(fmt, args...);
}

} // namespace devilution
