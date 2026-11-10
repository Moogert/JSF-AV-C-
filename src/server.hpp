#pragma once
#include <filesystem>

int run_web_server(const std::filesystem::path &executable,
                   const std::filesystem::path &catalog);
