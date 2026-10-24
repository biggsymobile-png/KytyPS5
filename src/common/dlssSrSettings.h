#ifndef KYTY_COMMON_DLSS_SR_SETTINGS_H_
#define KYTY_COMMON_DLSS_SR_SETTINGS_H_

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace Common {

enum class DlssSrRequest { Off, Automatic, Dlaa, Invalid };

inline DlssSrRequest ParseDlssSrRequest(std::string_view value) {
	if (value.empty() || value == "off" || value == "0") return DlssSrRequest::Off;
	if (value == "on" || value == "1") return DlssSrRequest::Automatic;
	if (value == "dlaa") return DlssSrRequest::Dlaa; // legacy explicit native-grid mode
	return DlssSrRequest::Invalid;
}

struct DlssSrInputRange {
	uint32_t min_width = 0, min_height = 0, max_width = 0, max_height = 0;
	uint32_t optimal_width = 0, optimal_height = 0;
};

// Select the closest SDK recommendation among modes that actually accept the
// game's input grid. A failed SDK query leaves an empty, ineligible range.
inline std::optional<size_t> SelectDlssSrInputRange(std::span<const DlssSrInputRange> ranges,
                                                    uint32_t width, uint32_t height) {
	std::optional<size_t> best;
	double                best_distance = std::numeric_limits<double>::infinity();
	if (width == 0 || height == 0) return best;
	for (size_t i = 0; i < ranges.size(); ++i) {
		const auto& r = ranges[i];
		if (width < r.min_width || height < r.min_height || width > r.max_width ||
		    height > r.max_height || r.optimal_width == 0 || r.optimal_height == 0)
			continue;
		const double dx       = (double(width) - r.optimal_width) / width;
		const double dy       = (double(height) - r.optimal_height) / height;
		const double distance = dx * dx + dy * dy;
		if (distance < best_distance) {
			best          = i;
			best_distance = distance;
		}
	}
	return best;
}

} // namespace Common

#endif
