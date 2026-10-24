#include "loader/astroDlssOutput.h"

#include <cstdio>
#include <vector>

using namespace Loader::AstroDlssOutput;

int main() {
  int failures = 0;
  const auto check = [&](bool ok, const char *message) {
    if (!ok) {
      std::printf("FAIL: %s\n", message);
      ++failures;
    }
  };
  for (const auto mode : {"on", "1"}) {
    check(Requested(mode, ""), "SR defaults to 4K");
    check(Requested(mode, "4k") && Requested(mode, "3840x2160"), "explicit 4K");
    check(!Requested(mode, "game"), "game output opt-out");
  }
  for (const auto mode : {"", "off", "0", "dlaa", "invalid"})
    check(!Requested(mode, "4k"), "no patch without an SR mode");
  check(!Requested("on", "invalid"),
        "unknown output does not patch guest code");
  for (const auto mode :
       {"quality", "balanced", "performance", "ultra-performance"}) {
    check(Common::ParseDlssSrRequest(mode) == Common::DlssSrRequest::Invalid,
          "SDK quality-mode names are rejected as configuration values");
    check(!Requested(mode, "4k"),
          "rejected mode does not patch the output grid");
  }

  using Common::DlssSrInputRange;
  // Fixture ranges overlap, as SDK dynamic-resolution ranges can. The actual
  // grid must select the closest eligible recommendation, not a user label.
  std::array<DlssSrInputRange, 4> ranges{{{960, 540, 3328, 1872, 2560, 1440},
                                          {960, 540, 2560, 1440, 2227, 1253},
                                          {960, 540, 2560, 1440, 1920, 1080},
                                          {640, 360, 1920, 1080, 1280, 720}}};
  check(Common::SelectDlssSrInputRange(ranges, 1920, 1080) == 2,
        "1080p patch selects the SDK performance recommendation");
  check(Common::SelectDlssSrInputRange(ranges, 2432, 1368) == 0,
        "1368p patch selects the closest quality recommendation");
  check(Common::SelectDlssSrInputRange(ranges, 3328, 1872) == 0,
        "1872p patch uses a mode that accepts its actual input");
  ranges[2] = {};
  check(Common::SelectDlssSrInputRange(ranges, 1920, 1080) == 1,
        "a failed SDK query is excluded from selection");
  check(!Common::SelectDlssSrInputRange(ranges, 3840, 2160),
        "unsupported sizes retain game TAA");
  check(!Common::SelectDlssSrInputRange(ranges, 0, 1080),
        "zero-sized input is rejected");
  check(Common::ParseDlssSrRequest("dlaa") == Common::DlssSrRequest::Dlaa,
        "explicit legacy DLAA remains available");

  std::vector<uint8_t> original(256, 0xcc);
  std::copy(ResourceSignature.begin(), ResourceSignature.end(),
            original.begin() + 16);
  std::copy(CameraSignature.begin(), CameraSignature.end(),
            original.begin() + 96);
  auto code = original;
  check(Apply(code) == Result::Applied, "recognized executable is patched");
  auto expected = original;
  const std::array<uint8_t, 7> high{0xc6, 0x81, 0x50, 0x01, 0x00, 0x00, 0x00};
  std::copy(high.begin(), high.end(), expected.begin() + 28);
  expected[98] = 0x70;
  expected[99] = 0x08; // mov r10d,2160
  expected[143] = 0;
  expected[144] = 0x0f; // mov r11d,3840
  check(code == expected,
        "scene dimensions, preset, surrounding code and flags are preserved");

  for (const auto offset : {size_t{16}, size_t{96}, size_t{120}}) {
    code = original;
    code[offset] ^= 1;
    const auto before = code;
    check(Apply(code) == Result::MissingSignature && code == before,
          "mismatched versions are rejected without partial writes");
  }
  for (const bool duplicate_resource : {false, true}) {
    code = original;
    if (duplicate_resource)
      code.insert(code.end(), ResourceSignature.begin(),
                  ResourceSignature.end());
    else
      code.insert(code.end(), CameraSignature.begin(), CameraSignature.end());
    const auto before = code;
    check(Apply(code) == Result::AmbiguousSignature && code == before,
          "ambiguous signatures are rejected without partial writes");
  }
  code.assign(CameraSignature.begin(), CameraSignature.end() - 1);
  const auto truncated = code;
  check(Apply(code) == Result::MissingSignature && code == truncated,
        "truncated segment is rejected");
  check(Apply({}) == Result::MissingSignature, "empty segment is rejected");
  std::printf("AstroDlssOutputTests: %s\n", failures ? "failed" : "passed");
  return failures ? 1 : 0;
}
