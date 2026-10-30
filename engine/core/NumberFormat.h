#pragma once
#include <string>
#include <string_view>

namespace fe {
// The decimal and digit-group separators of a language, as UTF-8.
struct NumberSeparators {
    std::string_view decimal = ".";
    std::string_view group = ",";
};

// Separators for a BCP 47 tag ("de", "pt-BR", "fr_CA"), chosen by its
// language subtag from a small built-in table (en, de, fr, es, it, pt, ja,
// zh, ko, ru, pl, nl, sv, tr); any other language gets English. Languages
// that group with a space use a no-break space (U+00A0), so a number never
// wraps in the middle.
[[nodiscard]] NumberSeparators numberSeparators(std::string_view locale);

// `value` with `decimals` fraction digits (0 to 20; ties round away from
// zero) and groups of three integer digits, using the
// language's separators: formatNumber(1234567.891, 2, "de") is
// "1.234.567,89". A negative number keeps a leading '-'; -0 prints as 0.
[[nodiscard]] std::string formatNumber(double value, int decimals, std::string_view locale);
} // namespace fe
