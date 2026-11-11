#include "sql.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstring>
#include <locale.h>
#include <immintrin.h>
#include <limits>
#include <stdexcept>

namespace mvdb {
namespace {
[[noreturn]] void error(const std::string& s) {throw std::runtime_error(s);}
std::string lower(std::string s) {for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
struct Token {std::string text;bool quoted=false;};
std::vector<Token> lex(const std::string& sql) {
  std::vector<Token> tokens;
  for(size_t p=0;p<sql.size();) {
    unsigned char c=sql[p];if(std::isspace(c)) {++p;continue;}
    // Comments are whitespace only outside quoted literals. Every path either
    // advances p or fails, including comments at EOF and repeated semicolons.
    if(c=='-'&&p+1<sql.size()&&sql[p+1]=='-'){
      p+=2;while(p<sql.size()&&sql[p]!='\n'&&sql[p]!='\r')++p;continue;
    }
    if(c=='/'&&p+1<sql.size()&&sql[p+1]=='*'){
      auto end=sql.find("*/",p+2);if(end==std::string::npos)error("unterminated SQL comment");p=end+2;continue;
    }
    if(c=='\'') {
      ++p;std::string s;bool closed=false;
      while(p<sql.size()) {char ch=sql[p++];if(ch=='\'') {
        if(p<sql.size()&&sql[p]=='\''){s+='\'';++p;}else{closed=true;break;}
      }else s+=ch;}
      if(!closed)error("unterminated SQL string");
      tokens.push_back({s,true});
    } else if(std::isalpha(c)||c=='_') {
      size_t first=p++;while(p<sql.size()&&(std::isalnum(static_cast<unsigned char>(sql[p]))||sql[p]=='_'))++p;
      tokens.push_back({lower(sql.substr(first,p-first)),false});
    } else if(std::isdigit(c)||c=='+'||c=='-'||c=='.') {
      size_t first=p++;while(p<sql.size()&&(std::isdigit(static_cast<unsigned char>(sql[p]))||sql[p]=='.'))++p;
      if(p<sql.size()&&(sql[p]=='e'||sql[p]=='E')) {++p;if(p<sql.size()&&(sql[p]=='+'||sql[p]=='-'))++p;
        while(p<sql.size()&&std::isdigit(static_cast<unsigned char>(sql[p])))++p;}
      tokens.push_back({sql.substr(first,p-first),false});
    } else {
      std::string s(1,sql[p++]);
      if(p<sql.size()&&((s=="<"&&(sql[p]=='='||sql[p]=='>'))||((s==">"||s=="!")&&sql[p]=='=')))s+=sql[p++];
      if(s.find_first_not_of("(),;*=<>![]")==std::string::npos)tokens.push_back({s,false});else error("invalid SQL character");
    }
  } return tokens;
}
int64_t integer(const std::string& s) {
  int64_t n;const char* first=s.data();if(!s.empty()&&s[0]=='+')++first;
  auto [end,ec]=std::from_chars(first,s.data()+s.size(),n);
  if(ec!=std::errc{}||end!=s.data()+s.size())error("invalid or overflowing BIGINT");
  return n;
}
struct Parser {
  std::vector<Token> tokens;size_t pos=0;
  explicit Parser(const std::string& s):tokens(lex(s)){}
  bool accept(const std::string& s){if(pos<tokens.size()&&!tokens[pos].quoted&&tokens[pos].text==s){++pos;return true;}return false;}
  void need(const std::string& s){if(!accept(s))error("expected "+s);}
  Token take(){if(pos==tokens.size())error("unexpected end of SQL");return tokens[pos++];}
  std::string name(){auto t=take();if(t.quoted||t.text.empty()||t.text.size()>63||(!std::isalpha(static_cast<unsigned char>(t.text[0]))&&t.text[0]!='_'))error("expected identifier of at most 63 bytes");return t.text;}
  Literal literal(){auto t=take();
    if(!t.quoted) {size_t start=(!t.text.empty()&&(t.text[0]=='+'||t.text[0]=='-'))?1:0;
      if(start==t.text.size()||(!std::isdigit(static_cast<unsigned char>(t.text[start]))&&t.text[start]!='.'))error("expected numeric or quoted literal");}
    return {t.text,t.quoted};}
  void where(Statement& s){if(!accept("where"))return;do{Condition c;c.column=name();auto op=take();if(op.quoted)error("expected comparison operator");c.op=op.text;
    if(c.op!="="&&c.op!="!="&&c.op!="<>"&&c.op!="<"&&c.op!=">"&&c.op!="<="&&c.op!=">=")error("invalid comparison");
    auto v=literal();c.literal=v.text;c.quoted=v.quoted;s.conditions.push_back(c);
  }while(accept("and"));}
  Statement run(){Statement s{};
    if(accept("begin")){s.kind=Statement::Begin;accept("transaction");}
    else if(accept("commit"))s.kind=Statement::Commit;
    else if(accept("rollback"))s.kind=Statement::Rollback;
    else if(accept("set")){s.kind=Statement::Set;s.setting=name();need("=");s.value=take().text;}
    else if(accept("create")) {
      if(accept("vector")){s.kind=Statement::CreateIndex;need("index");s.index=name();need("on");s.table=name();need("(");s.value=name();need(")");}
      else{s.kind=Statement::Create;need("table");s.table=name();need("(");do{
        Column c;c.name=name();if(accept("bigint")||accept("int"))c.type=Type::Bigint;
        else {if(accept("varchar"))c.type=Type::Varchar;else if(accept("vector"))c.type=Type::Vector;else error("unsupported column type");
          need("(");auto width=take();if(width.quoted)error("type width must be numeric");auto n=integer(width.text);if(n<=0||n>1048576)error("invalid type width");c.width=static_cast<uint32_t>(n);need(")");}
        if(accept("primary")){need("key");c.primary=true;}s.columns.push_back(c);
      }while(accept(","));need(")");
        // A single integer column is an unambiguous implicit primary key.
        size_t keys=0,integers=0,integer_column=0;
        for(size_t i=0;i<s.columns.size();++i){keys+=s.columns[i].primary;if(s.columns[i].type==Type::Bigint){++integers;integer_column=i;}}
        if(keys==0&&integers==1)s.columns[integer_column].primary=true;
      }
    } else if(accept("insert")){s.kind=Statement::Insert;need("into");s.table=name();
      if(accept("(")){do{s.insert_columns.push_back(name());}while(accept(","));need(")");}
      need("values");do{need("(");std::vector<Literal> row;do{row.push_back(literal());}while(accept(","));need(")");s.rows.push_back(std::move(row));}while(accept(","));
    } else if(accept("update")){s.kind=Statement::Update;s.table=name();need("set");do{auto col=name();need("=");s.assignments.emplace_back(col,literal());}while(accept(","));where(s);}
    else if(accept("delete")){s.kind=Statement::Delete;need("from");s.table=name();where(s);}
    else if(accept("select")){s.kind=Statement::Select;do{
      Expr e;if(accept("*"))e.column="*";
      else if(accept("l2_distance")){e.distance=true;need("(");e.column=name();need(",");auto q=take();if(!q.quoted)error("vector literal must be quoted");e.query=q.text;need(")");}
      else e.column=name();
      if(accept("as"))e.alias=name();else e.alias=e.distance?"l2_distance":e.column;
      s.expressions.push_back(e);
    }while(accept(","));need("from");s.table=name();where(s);
      if(accept("order")){need("by");do{auto col=name();bool desc=accept("desc");if(!desc)accept("asc");s.order.emplace_back(col,desc);}while(accept(","));}
      if(accept("limit")){auto count=take();if(count.quoted)error("LIMIT must be numeric");auto n=integer(count.text);if(n<0)error("negative LIMIT");s.limit=static_cast<uint64_t>(n);}
    }else error("unsupported SQL statement");
    while(accept(";")){} // Permit empty trailing statements, never a second SQL statement.
    if(pos!=tokens.size())error("trailing SQL tokens (multiple statements are unsupported)");
    return s;
  }
};
template<class T>void append(std::string& s,T n){s.append(reinterpret_cast<const char*>(&n),sizeof(n));}
template<class T>T read(const std::string& s,size_t& p){if(p+sizeof(T)>s.size())error("invalid stored row");T n;std::memcpy(&n,s.data()+p,sizeof(n));p+=sizeof(n);return n;}
// Bound float accumulation to 128 terms per lane at the supported 4096D limit.
// Keep FMA in its own target function: AVX2 does not imply FMA support.
#define L2_BODY(ACCUMULATE) \
  __m256 sums[4]={_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps()}; \
  size_t i=0; \
  for(;i+32<=n;i+=32)for(size_t j=0;j<4;++j){ \
    __m256 d=_mm256_sub_ps(_mm256_loadu_ps(a+i+j*8),_mm256_loadu_ps(b+i+j*8)); \
    sums[j]=ACCUMULATE; \
  } \
  for(;i+8<=n;i+=8){ \
    __m256 d=_mm256_sub_ps(_mm256_loadu_ps(a+i),_mm256_loadu_ps(b+i)); \
    size_t j=(i/8)%4;sums[j]=ACCUMULATE; \
  } \
  __m256 merged=_mm256_add_ps(_mm256_add_ps(sums[0],sums[1]),_mm256_add_ps(sums[2],sums[3])); \
  __m128 halves=_mm_add_ps(_mm256_castps256_ps128(merged),_mm256_extractf128_ps(merged,1)); \
  halves=_mm_hadd_ps(halves,halves);halves=_mm_hadd_ps(halves,halves); \
  double result=_mm_cvtss_f32(halves); \
  for(;i<n;++i){double d=static_cast<double>(a[i])-b[i];result+=d*d;} \
  return result;
__attribute__((target("avx2"))) double simd_squared(const float* a,const float* b,size_t n) {
  L2_BODY(_mm256_add_ps(sums[j],_mm256_mul_ps(d,d)))
}
__attribute__((target("avx2,fma"))) double simd_squared_fma(const float* a,const float* b,size_t n) {
  L2_BODY(_mm256_fmadd_ps(d,d,sums[j]))
}
#undef L2_BODY
__attribute__((target("avx2"))) bool simd_extreme(const float* a,size_t n) {
  const auto sign=_mm256_set1_ps(-0.0f),limit=_mm256_set1_ps(1e16f);size_t i=0;
  for(;i+8<=n;i+=8){auto x=_mm256_andnot_ps(sign,_mm256_loadu_ps(a+i));
    if(_mm256_movemask_ps(_mm256_cmp_ps(x,limit,_CMP_NLE_UQ)))return true;}
  for(;i<n;++i)if(!std::isfinite(a[i])||std::abs(a[i])>1e16f)return true;
  return false;
}
}
Statement parse(const std::string& s){return Parser(s).run();}
size_t unicode_length(const std::string& s) {
  size_t count=0;
  for(size_t p=0;p<s.size();) {uint32_t c=static_cast<unsigned char>(s[p++]);unsigned n=0;uint32_t minimum=0;
    if(c<128){}else if(c>=0xc2&&c<=0xdf){c&=31;n=1;minimum=128;}
    else if(c>=0xe0&&c<=0xef){c&=15;n=2;minimum=0x800;}
    else if(c>=0xf0&&c<=0xf4){c&=7;n=3;minimum=0x10000;}else error("invalid UTF-8");
    for(unsigned i=0;i<n;++i){if(p==s.size())error("truncated UTF-8");auto b=static_cast<unsigned char>(s[p++]);if((b&0xc0)!=0x80)error("invalid UTF-8 continuation");c=(c<<6)|(b&63);}
    if(c<minimum||c>0x10ffff||(c>=0xd800&&c<=0xdfff))error("invalid Unicode scalar");
    ++count;
  }return count;
}
Value convert(const Literal& v,const Column& col) {
  if(col.type==Type::Bigint){if(v.quoted)error("BIGINT literal must be numeric");return integer(v.text);}
  if(!v.quoted)error("string/vector literal must be quoted");
  if(col.type==Type::Varchar){if(unicode_length(v.text)>col.width)error("VARCHAR exceeds code point length");return v.text;}
  std::vector<float> out;out.reserve(std::min<size_t>(col.width,v.text.size()/2+1));
  size_t p=0;auto ws=[&]{while(p<v.text.size()&&std::isspace(static_cast<unsigned char>(v.text[p])))++p;};
  ws();if(p==v.text.size()||v.text[p++]!='[')error("invalid VECTOR literal");ws();
  if(p<v.text.size()&&v.text[p]!=']')while(true){
    const char* first=v.text.data()+p;const char* last=v.text.data()+v.text.size();
    const char* number=first;if(number!=last&&*number=='+')++number;
    if(number==last||(number!=first&&(*number=='-'||*number=='+')))error("invalid VECTOR component");
    double x=0;auto parsed=std::from_chars(number,last,x,std::chars_format::general);
    const char* end=parsed.ptr;
    const char* magnitude=number;if(magnitude!=last&&*magnitude=='-')++magnitude;
    const bool hex=last-magnitude>=2&&magnitude[0]=='0'&&(magnitude[1]=='x'||magnitude[1]=='X');
    if(hex||parsed.ec==std::errc::result_out_of_range){
      // Preserve strtod's hexadecimal and underflow-to-zero admission, using
      // an explicit locale only on these uncommon paths. Decimal parsing is
      // allocation-free and independent of the process locale.
      static locale_t c_locale=[] {auto locale=newlocale(LC_NUMERIC_MASK,"C",nullptr);if(!locale)error("cannot create numeric locale");return locale;}();
      char* fallback_end=nullptr;x=strtod_l(first,&fallback_end,c_locale);end=fallback_end;
    }else if(parsed.ec!=std::errc{})error("invalid VECTOR component");
    if(end==first||!std::isfinite(x)||std::abs(x)>std::numeric_limits<float>::max())error("nonfinite or overflowing VECTOR component");
    p=static_cast<size_t>(end-v.text.data());out.push_back(static_cast<float>(x));ws();
    if(p<v.text.size()&&v.text[p]==','){++p;ws();continue;}break;}
  if(p==v.text.size()||v.text[p++]!=']')error("invalid VECTOR separator");
  ws();
  if(p!=v.text.size()||out.size()!=col.width)error("VECTOR dimension mismatch");
  return out;
}
int compare(const Value& a,const Value& b) {
  if(a.index()!=b.index())error("incompatible comparison types");
  if(auto x=std::get_if<int64_t>(&a)){auto y=std::get<int64_t>(b);return (*x>y)-(*x<y);}
  if(auto x=std::get_if<double>(&a)){auto y=std::get<double>(b);return (*x>y)-(*x<y);}
  // Valid UTF-8 has the same unsigned-byte lexicographic and code point order.
  if(auto x=std::get_if<std::string>(&a)){int n=x->compare(std::get<std::string>(b));return (n>0)-(n<0);}
  error("VECTOR comparison is unsupported");
}
std::string quote(const std::string& s) {
  std::string out;out.reserve(s.size()+2);out+='"';constexpr char hex[]="0123456789abcdef";
  for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=static_cast<char>(c);}return out+'"';
}
namespace {
template<class Number>void append_number(std::string& out,Number value){
  char buffer[64];auto result=[&]{if constexpr(std::is_integral_v<Number>)return std::to_chars(buffer,buffer+sizeof(buffer),value);else return std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::general,17);}();
  if(result.ec!=std::errc{})error("cannot serialize number");
  out.append(buffer,result.ptr);
}
}
std::string json(const Value& v){
  std::string out;
  if(auto n=std::get_if<int64_t>(&v)){append_number(out,*n);return out;}
  if(auto n=std::get_if<double>(&v)){append_number(out,*n);return out;}
  if(auto s=std::get_if<std::string>(&v))return quote(*s);
  const auto& vector=std::get<std::vector<float>>(v);out.reserve(vector.size()*12+2);out+='[';
  bool first=true;for(float x:vector){if(!first)out+=',';first=false;append_number(out,static_cast<double>(x));}out+=']';return out;
}
std::string pack_values(const std::vector<Value>& row) {
  std::string s;append(s,static_cast<uint32_t>(row.size()));for(auto& v:row){append(s,static_cast<uint8_t>(v.index()));
    if(auto n=std::get_if<int64_t>(&v))append(s,*n);else if(auto n=std::get_if<double>(&v))append(s,*n);
    else if(auto text=std::get_if<std::string>(&v)){append(s,static_cast<uint32_t>(text->size()));s+=*text;}
    else{auto& a=std::get<std::vector<float>>(v);append(s,static_cast<uint32_t>(a.size()));s.append(reinterpret_cast<const char*>(a.data()),a.size()*4);}}
  return s;
}
std::vector<Value> unpack_values(const std::string& s) {
  size_t p=0;auto n=read<uint32_t>(s,p);if(n>1024)error("corrupt row count");std::vector<Value> out;
  for(uint32_t i=0;i<n;++i){auto tag=read<uint8_t>(s,p);
    if(tag==0)out.emplace_back(read<int64_t>(s,p));else if(tag==1)out.emplace_back(read<double>(s,p));
    else if(tag==2){auto len=read<uint32_t>(s,p);if(len>s.size()-p)error("corrupt string");out.emplace_back(s.substr(p,len));p+=len;}
    else if(tag==3){auto len=read<uint32_t>(s,p);if(len>(s.size()-p)/4)error("corrupt vector");std::vector<float> a(len);std::memcpy(a.data(),s.data()+p,len*4);p+=len*4;out.emplace_back(std::move(a));}
    else error("corrupt value tag");}
  if(p!=s.size())error("trailing stored bytes");
  return out;
}
bool avx2_available(){static const bool available=[] {__builtin_cpu_init();return bool(__builtin_cpu_supports("avx2"));}();return available;}
bool fma_available(){static const bool available=[] {__builtin_cpu_init();return bool(__builtin_cpu_supports("fma"));}();return available;}
bool has_extreme(const float* a,size_t n){
  if(n>4096)return true;
  if(avx2_available())return simd_extreme(a,n);
  for(size_t i=0;i<n;++i)if(!std::isfinite(a[i])||std::abs(a[i])>1e16f)return true;
  return false;
}
double l2_squared_exact(const float* a,const float* b,size_t n){
  double sum=0;for(size_t i=0;i<n;++i){double d=static_cast<double>(a[i])-b[i];sum+=d*d;}return sum;
}
double l2_exact(const float* a,const float* b,size_t n){return std::sqrt(l2_squared_exact(a,b,n));}
double l2_squared_fast(const float* a,const float* b,size_t n,bool safe){
  if(!safe||n>4096||!avx2_available())return l2_squared_exact(a,b,n);
  double sum=fma_available()?simd_squared_fma(a,b,n):simd_squared(a,b,n);
  // Recompute tiny sums to avoid float square underflow (including FTZ mode).
  if(!std::isfinite(sum)||sum<=1e-20)return l2_squared_exact(a,b,n);
  return sum;
}
double l2_fast(const float* a,const float* b,size_t n,bool safe){return std::sqrt(l2_squared_fast(a,b,n,safe));}
double l2(const float* a,const float* b,size_t n){return l2_fast(a,b,n,!has_extreme(a,n)&&!has_extreme(b,n));}
}
