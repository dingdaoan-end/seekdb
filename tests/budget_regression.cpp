#include "cli_budget.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <unistd.h>
int main(){
  using namespace mvdb;constexpr uint64_t MiB=1024*1024;
  auto root=std::filesystem::temp_directory_path()/("mvdb-budget-"+std::to_string(getpid()));
  std::filesystem::create_directories(root);
  auto set=[&](const char* name,const char* value){std::ofstream(root/name)<<value;};
  assert(default_cli_budget(root).bytes==10*MiB);
  set("memory.max","max\n");set("memory.high","max\n");
  assert(default_cli_budget(root).bytes==10*MiB);
  set("memory.max","33554432\n");assert(default_cli_budget(root).bytes==10*MiB);
  set("memory.high","16777216\n");assert(default_cli_budget(root).bytes==9227468);
  set("memory.high","max\n");set("memory.max","1048576\n");assert(default_cli_budget(root).bytes==4*MiB);
  set("memory.max","67108864\n");assert(default_cli_budget(root).bytes==10*MiB);
  set("memory.max","18446744073709551615\n");assert(default_cli_budget(root).bytes==10*MiB);
  set("memory.max","invalid\n");assert(default_cli_budget(root).bytes==10*MiB);
  set("memory.max","max\n");set("memory.high","33554432\n");assert(default_cli_budget(root).bytes==10*MiB);
  set("memory.high","0\n");assert(default_cli_budget(root).bytes==4*MiB);
  set("memory.high","-1\n");assert(default_cli_budget(root).source=="fallback");
  std::filesystem::remove_all(root);
  std::cout<<"budget_regression: 11 cgroup max/high, fallback, clamp and overflow cases PASS\n";
}
