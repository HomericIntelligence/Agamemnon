#pragma once

#include <string_view>

namespace agamemnon {

constexpr std::string_view kProjectName{"Agamemnon"};
constexpr std::string_view kVersion{"0.1.0"};
constexpr int kVersionMajor{0};
constexpr int kVersionMinor{1};
constexpr int kVersionPatch{0};

const char* get_version();
const char* get_project_name();

}  // namespace agamemnon
