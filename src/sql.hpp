#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace mvdb {
using Value=std::variant<int64_t,double,std::string,std::vector<float>>;
enum class Type { Bigint, Varchar, Vector };
struct Column {std::string name;Type type;uint32_t width=0;bool primary=false;};
struct Expr {std::string column,alias;bool distance=false;std::string query;};
struct Condition {std::string column,op;std::string literal;bool quoted=false;};
struct Literal {std::string text;bool quoted=false;};
struct Statement {
  enum Kind {Create,Insert,Select,Update,Delete,Begin,Commit,Rollback,CreateIndex,Set} kind;
  std::string table,index,setting,value;
  std::vector<Column> columns;
  std::vector<std::vector<Literal>> rows;
  std::vector<std::string> insert_columns;
  std::vector<Expr> expressions;
  std::vector<Condition> conditions;
  std::vector<std::pair<std::string,Literal>> assignments;
  std::vector<std::pair<std::string,bool>> order;
  uint64_t limit=UINT64_MAX;
};
Statement parse(const std::string&);
size_t unicode_length(const std::string&);
Value convert(const Literal&,const Column&);
int compare(const Value&,const Value&);
std::string json(const Value&);
std::string quote(const std::string&);
std::string pack_values(const std::vector<Value>&);
std::vector<Value> unpack_values(const std::string&);
double l2(const float*,const float*,size_t);
// Precheck each stored vector once and the query once. safe=true requires both
// has_extreme checks to be false; false always uses the widened double kernel.
bool has_extreme(const float*,size_t);
double l2_fast(const float*,const float*,size_t,bool safe);
double l2_exact(const float*,const float*,size_t);
bool avx2_available();
bool fma_available();
}
