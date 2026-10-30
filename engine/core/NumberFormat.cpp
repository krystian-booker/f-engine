#include "engine/core/NumberFormat.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace fe {
namespace {
constexpr std::string_view NoBreakSpace = " ";

struct LanguageSeparators {
    std::string_view language;
    NumberSeparators separators;
};

// CLDR's "standard" decimal and group symbols; a no-break space stands in
// for the narrow one some locales use, since more fonts carry it.
constexpr std::array<LanguageSeparators, 14> Table = {{
    {"en", {".", ","}},
    {"de", {",", "."}},
    {"fr", {",", NoBreakSpace}},
    {"es", {",", "."}},
    {"it", {",", "."}},
    {"pt", {",", "."}},
    {"ja", {".", ","}},
    {"zh", {".", ","}},
    {"ko", {".", ","}},
    {"ru", {",", NoBreakSpace}},
    {"pl", {",", NoBreakSpace}},
    {"nl", {",", "."}},
    {"sv", {",", NoBreakSpace}},
    {"tr", {",", "."}},
}};

std::string languageOf(std::string_view locale) {
    std::string language;
    for (const char c : locale) {
        if (c == '-' || c == '_') {
            break;
        }
        language += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return language;
}
} // namespace

NumberSeparators numberSeparators(std::string_view locale) {
    const auto language = languageOf(locale);
    const auto found = std::find_if(Table.begin(), Table.end(), [&](const LanguageSeparators& entry) {
        return entry.language == language;
    });
    return found == Table.end() ? NumberSeparators{} : found->separators;
}

std::string formatNumber(double value, int decimals, std::string_view locale) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("formatNumber needs a finite number");
    }
    if (decimals < 0 || decimals > 20) {
        throw std::invalid_argument("formatNumber takes 0 to 20 decimals");
    }
    // Ties round away from zero (1234.5 -> 1235, as players expect), where
    // printf alone would round them to even; printf then pads the digits and
    // the separators are swapped in afterwards.
    double magnitude = std::abs(value);
    const double scale = std::pow(10.0, decimals);
    if (magnitude * scale < 9007199254740992.0) {
        magnitude = std::floor(magnitude * scale + 0.5) / scale;
    }
    char buffer[400];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, magnitude);
    const std::string_view digits(buffer);
    const auto point = digits.find('.');
    const auto whole = digits.substr(0, point);
    const auto fraction = point == std::string_view::npos ? std::string_view() : digits.substr(point + 1);
    const auto separators = numberSeparators(locale);
    std::string out;
    // A value that rounds to zero at this precision is not negative.
    if (value < 0 && whole.find_first_not_of('0') == std::string_view::npos &&
        fraction.find_first_not_of('0') == std::string_view::npos) {
        // -0 prints as 0
    } else if (value < 0) {
        out += '-';
    }
    for (std::size_t i = 0; i < whole.size(); ++i) {
        if (i > 0 && (whole.size() - i) % 3 == 0) {
            out += separators.group;
        }
        out += whole[i];
    }
    if (!fraction.empty()) {
        out += separators.decimal;
        out += fraction;
    }
    return out;
}
} // namespace fe
