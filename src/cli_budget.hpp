#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
namespace mvdb {
struct CliBudget {uint64_t bytes;std::string source;};
inline std::optional<uint64_t> cgroup_limit(const std::filesystem::path& path){
  std::ifstream input(path);std::string token,extra;
  if(!(input>>token)||token=="max"||(input>>extra))return {};
  uint64_t value=0;auto[end,error]=std::from_chars(token.data(),token.data()+token.size(),value);
  if(error!=std::errc{}||end!=token.data()+token.size())return {};
  return value;
}
inline CliBudget default_cli_budget(const std::filesystem::path& root="/sys/fs/cgroup"){
  auto limit=cgroup_limit(root/"memory.max"),high=cgroup_limit(root/"memory.high");
  if(high&&(!limit||*high<*limit))limit=high;
  constexpr uint64_t MiB=1024*1024;
  if(!limit)return {20*MiB,"fallback"};
  // Compute floor(55% * limit) without overflow at uint64_t maximum.
  const auto fraction=(*limit/100)*55+(*limit%100)*55/100;
  return {std::clamp(fraction,4*MiB,24*MiB),"cgroup"};
}
}
