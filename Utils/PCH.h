#ifndef PCH_H
#define PCH_H
#define _LIBCPP_ENABLE_EXPERIMENTAL

// ============================================================================
// 标准库高频头（几乎每个 TU 都会展开，预编译收益最大）
// ============================================================================
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// ============================================================================
// 重量级第三方模板库（entt/yaml-cpp 解析成本极高且被广泛引用）
// ============================================================================
#include <entt/entt.hpp>
#include <yaml-cpp/yaml.h>

// ============================================================================
// 引擎基础工具
// ============================================================================
#include "Logger.h"
#include "Directory.h"
#include "Guid.h"
#include "LazySingleton.h"
#include "Utils.h"
#include "Platform.h"

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif
#endif
