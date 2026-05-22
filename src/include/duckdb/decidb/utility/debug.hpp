#ifndef DEBUG_HPP
#define DEBUG_HPP

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <map>
#include <memory>
#include <algorithm>
#include <type_traits>
#include <tuple>

#include "duckdb/common/enum_util.hpp"

namespace decidb {

extern const char* RED;
extern const char* GREEN;
extern const char* RESET;

// Helper to check for a ToString() member function.
template <typename T, typename = void>
struct HasToString : std::false_type {};

template <typename T>
struct HasToString<T, std::void_t<decltype(std::declval<T>().ToString())>> : std::true_type {};

template <typename T, typename U>
std::pair<T, U> operator+(const std::pair<T, U>& l, const std::pair<T, U>& r) {
    return {l.first + r.first, l.second + r.second};
}

class Profiler {
private:
    std::map<std::string, std::pair<double, int>> clocks;
    std::map<std::string, std::chrono::time_point<std::chrono::high_resolution_clock>> timePoints;
public:
    Profiler();
    void clock(const std::string& label = "");
    void stop(const std::string& label = "");
    void add(const Profiler& pro);
    void print() const;
};

// --- GENERIC SMART POINTER TRAIT ---
template <typename T, typename = void>
struct IsPointerLike : std::false_type {};

template <typename T>
struct IsPointerLike<T, std::void_t<
    typename T::element_type,
    decltype(*std::declval<T>())
>> : std::true_type {};


// Type trait for checking if a type supports default output to std::cout
template <class T, class = void>
struct DefaultIO : std::false_type {};

template <class T>
struct DefaultIO<T, std::void_t<decltype(std::cout << std::declval<T&>())>> : std::true_type {};

// Type trait for checking if a type is a tuple
template <class T, class = void>
struct IsTuple : std::false_type {};

template <class T>
struct IsTuple<T, std::void_t<typename std::tuple_size<T>::type>> : std::true_type {};

// Type trait for checking if a type is iterable (has begin())
template <class T, class = void>
struct Iterable : std::false_type {};

template <class T>
struct Iterable<T, std::void_t<decltype(std::begin(std::declval<T>()))>> : std::true_type {};

template <class T>
struct IsEnum : std::integral_constant<bool, std::is_enum_v<T>> {};

// Function to determine spacing based on type traits
template <class T>
constexpr char Space(const T&) {
    return (Iterable<T>::value || IsTuple<T>::value) ? ' ' : ' ';
}

// Writer struct for formatted output
template <auto& os>
struct Writer {
    template <class T>
    void Impl(T const& t) const {
        // Check for smart pointer behavior first
        if constexpr (IsPointerLike<T>::value) {
            if (t) {
                Impl(*t); // Recurse on the pointed-to object
            } else {
                os << "nullptr";
            }
        } else if constexpr (DefaultIO<T>::value && !HasToString<T>::value) {
            os << t;
        } else if constexpr (HasToString<T>::value) {
            os << t.ToString();
        } else if constexpr (Iterable<T>::value) {
            int i = 0;
            os << "[";
            for (auto&& x : t) {
                ((i++) ? (os << Space(x), Impl(x)) : Impl(x));
            }
            os << "]";
        } else if constexpr (IsTuple<T>::value) {
            std::apply([this](auto const&... args) {
                int i = 0;
                os << "{";
                (((i++) ? (os << ' ', Impl(args)) : Impl(args)), ...);
                os << "}";
            }, t);
        } else if constexpr (IsEnum<T>::value) {
            os << duckdb::EnumUtil::ToString(t);
        }
        else {
            // This static_assert will fail with a more helpful message if no condition is met.
            static_assert(IsPointerLike<T>::value || DefaultIO<T>::value || HasToString<T>::value || Iterable<T>::value || IsTuple<T>::value, "No matching type for print. Type must be a smart pointer, have a ToString() method, be iterable, be a tuple, or support ostream output.");
        }
    }

    template <class F, class... Ts>
    auto& operator()(F const& f, Ts const&... ts) const {
        Impl(f);
        ((os << ' ', Impl(ts)), ...);
        os << '\n';
        return *this;
    }
};

// Debug function to replace deb macro
template <typename... Args>
void debug(const char* file, int line, const char* names, Args&&... args) {
    std::cerr << RED << "File " << file << ", Line " << line << RESET << "\n";
    std::string names_str(names);
    std::replace(names_str.begin(), names_str.end(), ',', ' ');
    std::stringstream ss(names_str);
    auto print = [&ss](auto&& arg) {
        std::string name;
        ss >> name;
        std::cerr << name << " = ";
        decidb::Writer<std::cerr>{}(arg);
    };
    (print(std::forward<Args>(args)), ...);
}

// Assertion function to replace ASSERT macro
inline void assert_func(bool condition, const char* expr) {
    if (!condition) {
        throw std::runtime_error(expr);
    }
}

}

#define SFINAE(x, ...)             \
    template <class, class = void> \
    struct x : std::false_type {}; \
    template <class T>             \
    struct x<T, std::void_t<__VA_ARGS__>> : std::true_type {}

SFINAE(DefaultIO, decltype(std::cout << std::declval<T&>()));
SFINAE(IsTuple, typename std::tuple_size<T>::type);
SFINAE(Iterable, decltype(begin(std::declval<T>())));

template <class T>
constexpr char Space(const T&) {
    return (Iterable<T>::value or IsTuple<T>::value) ? ' ' : ' ';
}

#define deb(...) decidb::debug(__FILE__, __LINE__, #__VA_ARGS__, __VA_ARGS__)
#define ASSERT(expr) decidb::assert_func(expr, #expr)

#endif // DEBUG_HPP