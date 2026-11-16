#pragma once
#include <cstdint>
#include <functional>
#include <string_view>
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
// Borrowed while the original SQL string stays alive and unchanged. Doubled quotes
// are decoded by convert() only on the uncommon escaped-literal path.
struct LiteralView {
  std::string_view text;bool quoted,escaped;
  LiteralView(std::string_view t,bool q,bool e):text(t),quoted(q),escaped(e){}
};
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
Statement parse(std::string_view);
// Reuse a validated SELECT shape. Only unescaped quoted literals may vary;
// structural mismatches leave the statement untouched and use the full parser.
bool reuse_select(std::string_view pattern,std::string_view sql,Statement&);
bool is_insert(std::string_view);
// Callbacks may run before a later syntax error. The caller must provide
// statement rollback and must never retry a materializing parser after mutation.
void parse_insert_rows(std::string_view,const std::function<void(const Statement&)>&,
                       const std::function<void(const std::vector<LiteralView>&)>&,size_t max_columns=SIZE_MAX);
size_t unicode_length(const std::string&);
Value convert(const Literal&,const Column&);
Value convert(const LiteralView&,const Column&);
// Statement-local reuse; no view or pointer escapes the destination Value.
void convert_into(const LiteralView&,const Column&,Value&);
int compare(const Value&,const Value&);
std::string json(const Value&);
std::string quote(const std::string&);
std::string pack_values(const std::vector<Value>&);
void pack_values_into(const std::vector<Value>&,std::string&);
std::vector<Value> unpack_values(const std::string&);
double l2(const float*,const float*,size_t);
// Precheck each stored vector once and the query once. safe=true requires both
// has_extreme checks to be false; false always uses the widened double kernel.
bool has_extreme(const float*,size_t);
double l2_fast(const float*,const float*,size_t,bool safe);
double l2_exact(const float*,const float*,size_t);
double l2_squared_fast(const float*,const float*,size_t,bool safe);
double l2_squared_exact(const float*,const float*,size_t);
// Four physically adjacent 128D rows; safe requires all rows and query to
// pass has_extreme. Results match four full l2_squared_fast calls bit for bit.
void l2_squared_batch4_128(const float* rows,const float* query,double* out,bool safe);
struct BoundedDistance {double squared;bool pruned;};
// Takes an exact squared-distance heap threshold; rounds the conservative
// margin upward to float. Overflow/nonfinite input disables pruning.
double l2_bounded_cutoff(double exact_squared);
// The caller prechecks both vectors as for l2_squared_fast, and passes the
// inflated cutoff above. Only 128D and prefix 32/64 are supported. A pruned
// result contains the partial squared distance; survivors match the old kernel.
BoundedDistance l2_squared_bounded_128(const float*,const float*,double cutoff,bool safe,size_t prefix=32);
bool avx2_available();
bool fma_available();
}
