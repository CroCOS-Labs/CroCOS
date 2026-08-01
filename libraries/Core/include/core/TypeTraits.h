//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_TYPETRAITS_H
#define CROCOS_TYPETRAITS_H
#include "stdint.h"
#include "stddef.h"

template <typename T>
struct is_void_t {
    static const bool value = false;
};

template <>
struct is_void_t<void> {
    static const bool value = true;
};

template <typename T>
constexpr bool is_void_v = is_void_t<T>::value;

template <typename T>
concept is_void = is_void_v<T>;

constexpr size_t RequiredBits(size_t value) {
    size_t bits = 0;
    while (value > 0) {
        value >>= 1;
        bits++;
    }
    return bits;
}

// Template to choose the smallest unsigned integer type
template <size_t Bits>
struct SmallestUInt;

template <> struct SmallestUInt<0> { using Type = uint8_t; };
template <> struct SmallestUInt<1> { using Type = uint8_t; };
template <> struct SmallestUInt<8> { using Type = uint8_t; };
template <> struct SmallestUInt<16> { using Type = uint16_t; };
template <> struct SmallestUInt<32> { using Type = uint32_t; };
template <> struct SmallestUInt<64> { using Type = uint64_t; };

// Helper type alias
template <size_t Bits>
using SmallestUInt_t = typename SmallestUInt<
        (Bits <= 8) ? 8 :
        (Bits <= 16) ? 16 :
        (Bits <= 32) ? 32 :
        64
        >::Type;

// Template to choose the smallest unsigned integer type
template <size_t Bits>
struct SmallestUInt;
template <size_t Bits>
struct SmallestInt;

template <> struct SmallestInt<0> { using Type = int8_t; };
template <> struct SmallestInt<1> { using Type = int8_t; };
template <> struct SmallestInt<8> { using Type = int8_t; };
template <> struct SmallestInt<16> { using Type = int16_t; };
template <> struct SmallestInt<32> { using Type = int32_t; };
template <> struct SmallestInt<64> { using Type = int64_t; };

// Helper type alias
template <size_t Bits>
using SmallestInt_t = typename SmallestInt<
        (Bits <= 8) ? 8 :
        (Bits <= 16) ? 16 :
        (Bits <= 32) ? 32 :
        64
        >::Type;

template <typename T>
struct is_trivially_copyable {
    static constexpr bool value = __is_trivially_copyable(T);
};

static constexpr size_t constexpr_strlen(const char * str) {
    size_t out = 0;
    while(str[out] != 0){
        out++;
    }
    return out;
}

#if !(defined(CORE_LIBRARY_TESTING) || defined(HOSTED))
static constexpr size_t strlen(const char * str) {
    return constexpr_strlen(str);
}
#else
// When testing with standard library, use system strlen
#include <cstring>
#endif

static constexpr size_t find(const char * str, const char * substr) {
    size_t strIndex = 0;
    while(str[strIndex] != 0){
        size_t subStrIndex = 0;
        for(; substr[subStrIndex] != 0; subStrIndex++){
            if(substr[subStrIndex] != str[strIndex + subStrIndex]){
                break;
            }
        }
        if(subStrIndex == constexpr_strlen(substr)){
            break;
        }
        strIndex++;
    }
    return strIndex;
}

static constexpr size_t rfind(const char * str, const char * substr) {
    size_t strIndex = constexpr_strlen(str) - 1;
    while(strIndex < (size_t)-1){
        size_t subStrIndex = 0;
        for(; substr[subStrIndex] != 0; subStrIndex++){
            if(substr[subStrIndex] != str[strIndex + subStrIndex]){
                break;
            }
        }
        if(subStrIndex == constexpr_strlen(substr)){
            break;
        }
        strIndex--;
    }
    return strIndex;
}

template <typename T>
inline constexpr bool is_trivially_copyable_v = is_trivially_copyable<T>::value;

template <typename T>
struct TypeName {
private:
    static constexpr const char* full_name() {
#if defined(__clang__)
        return __PRETTY_FUNCTION__;
#elif defined(__GNUC__)
        return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
        return __FUNCSIG__;
#else
        return "Unsupported compiler";
#endif
    }

public:
#if defined(__clang__)
    static constexpr char * prefix = (char*)"[T = ";
    static constexpr char * suffix = (char*)"]";
#elif defined(__GNUC__)
    static constexpr char * prefix = (char*)"[with T = ";
    static constexpr char * suffix = (char*)"]";
#else
#error("Unsupported compiler")
#endif

    constexpr static size_t startIndex = find(full_name(), prefix) + constexpr_strlen(prefix);
    constexpr static size_t endIndex = rfind(full_name(), suffix);
    constexpr static size_t typeNameSize = endIndex - startIndex + 1; //includes space for the null terminator

    static constexpr const char* name() {
        const char * fullName = full_name();
        //A small hack to prevent clangd from complaining about TypeName due to it pretending to be GCC
        constexpr size_t clampedSize = typeNameSize < 10000 ? typeNameSize : 10000;
        static char output[clampedSize];
        for(size_t i = 0; i < clampedSize - 1; i++){
            output[i] = fullName[startIndex + i];
        }
        output[clampedSize - 1] = 0;

        return output;
    }
};

template <typename T>
constexpr const char* type_name() {
    return TypeName<T>::name();
}

template <typename T>
constexpr bool is_enum_v = __is_enum(T);

// Wrapped in a variable template rather than used inline: GCC 15 rejects a
// built-in trait appearing directly in a function signature ("use library traits
// instead"), which bites `requires __is_constructible(T)` constraints on
// constructors. Clang accepts them, so this only shows up on the cross build.
template <typename T, typename... Args>
constexpr bool is_constructible_v = __is_constructible(T, Args...);

template <typename T>
constexpr bool is_default_constructible_v = __is_constructible(T);

template <typename T>
using degrade_enum = __underlying_type(T);

template <bool isEnum, typename T>
struct _underlying_type;

template <typename T>
struct _underlying_type<true, T>
{
    using type = degrade_enum<T>;
};

template <typename T>
struct _underlying_type<false, T> {
    using type = T;
};

template <typename T>
using underlying_type_t = _underlying_type<is_enum_v<T>, T>::type;
#endif //CROCOS_TYPETRAITS_H