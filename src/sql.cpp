#include "sql.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstring>
#include <locale.h>
#include <limits>
#include <stdexcept>

namespace mvdb {
namespace {
[[noreturn]] void error(const std::string& s) {throw std::runtime_error(s);}
std::string lower(std::string s) {for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
// Token spans borrow the SQL buffer. Only fields retained by a Statement are
// copied; quoted vector components never become SQL tokens.
struct TokenView {
  std::string_view text;bool quoted=false,escaped=false,identifier=false;
  bool equals(std::string_view word)const {
    if(text.size()!=word.size())return false;
    for(size_t i=0;i<word.size();++i)if(static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])))!=word[i])return false;
    return true;
  }
  std::string str()const {
    std::string out(text);
    if(escaped){out.clear();out.reserve(text.size());for(size_t i=0;i<text.size();++i){out+=text[i];if(text[i]=='\''&&i+1<text.size()&&text[i+1]=='\'')++i;}}
    if(identifier)return lower(std::move(out));
    return out;
  }
};
struct Cursor {
  const std::string& sql;size_t pos=0;TokenView token{};bool loaded=false,eof=false;
  explicit Cursor(const std::string& s):sql(s){}
  void load(){
    if(loaded)return;
    loaded=true;
    while(pos<sql.size()){
      unsigned char c=sql[pos];
      if(std::isspace(c)){++pos;continue;}
      if(c=='-'&&pos+1<sql.size()&&sql[pos+1]=='-'){pos+=2;while(pos<sql.size()&&sql[pos]!='\n'&&sql[pos]!='\r')++pos;continue;}
      if(c=='/'&&pos+1<sql.size()&&sql[pos+1]=='*'){auto end=sql.find("*/",pos+2);if(end==std::string::npos)error("unterminated SQL comment");pos=end+2;continue;}
      break;
    }
    if(pos==sql.size()){eof=true;return;}
    size_t first=pos;unsigned char c=sql[pos++];token={};
    if(c=='\''){
      first=pos;
      while(true){
        // Search the bounded byte span in bulk; embedded NUL is literal data.
        const auto quote=sql.find('\'',pos);
        if(quote==std::string::npos)error("unterminated SQL string");
        pos=quote+1;
        if(pos<sql.size()&&sql[pos]=='\''){token.escaped=true;++pos;}
        else break;
      }
      token.quoted=true;token.text=std::string_view(sql).substr(first,pos-first-1);return;
    }
    if(std::isalpha(c)||c=='_'){
      while(pos<sql.size()&&(std::isalnum(static_cast<unsigned char>(sql[pos]))||sql[pos]=='_'))++pos;
      token.identifier=true;
    }else if(std::isdigit(c)||c=='+'||c=='-'||c=='.'){
      while(pos<sql.size()&&(std::isdigit(static_cast<unsigned char>(sql[pos]))||sql[pos]=='.'))++pos;
      if(pos<sql.size()&&(sql[pos]=='e'||sql[pos]=='E')){++pos;if(pos<sql.size()&&(sql[pos]=='+'||sql[pos]=='-'))++pos;while(pos<sql.size()&&std::isdigit(static_cast<unsigned char>(sql[pos])))++pos;}
    }else{
      if(pos<sql.size()&&((c=='<'&&(sql[pos]=='='||sql[pos]=='>'))||((c=='>'||c=='!')&&sql[pos]=='=')))++pos;
      if(std::string_view("(),;*=<>![]").find(c)==std::string_view::npos)error("invalid SQL character");
    }
    token.text=std::string_view(sql).substr(first,pos-first);
  }
  bool done(){load();return eof;}
  const TokenView& peek(){load();return token;}
  TokenView next(){load();if(eof)error("unexpected end of SQL");auto result=token;loaded=false;return result;}
};
int64_t integer(std::string_view s) {
  int64_t n;const char* first=s.data();if(!s.empty()&&s[0]=='+')++first;
  auto [end,ec]=std::from_chars(first,s.data()+s.size(),n);
  if(ec!=std::errc{}||end!=s.data()+s.size())error("invalid or overflowing BIGINT");
  return n;
}
// Short plain decimals need no general-purpose conversion. Both the integer
// (at most nine digits) and 10^fraction are exactly representable as doubles,
// so this division rounds once to double, just like the R9 converter. Keep
// the subsequent float cast: parsing directly to float changes midpoint cases.
bool short_decimal(const char* first,const char* last,double& value,const char*& end){
  const char* p=first;const bool negative=p!=last&&*p=='-';if(negative)++p;
  uint32_t mantissa=0;unsigned digits=0,fraction=0;bool dot=false;
  while(p!=last){const unsigned digit=static_cast<unsigned char>(*p)-'0';
    if(digit<10){if(digits==9)return false;mantissa=mantissa*10+digit;++digits;if(dot)++fraction;++p;}
    else if(*p=='.'&&!dot){dot=true;++p;}else break;
  }
  // Only finish at an actual separator. Exponents, hex and malformed tokens
  // retain the original parser and its exact admission/overflow behavior.
  if(!digits||(p!=last&&*p!=','&&*p!=']'&&!std::isspace(static_cast<unsigned char>(*p))))return false;
  static constexpr double powers[]={1,10,100,1000,10000,100000,1000000,10000000,100000000,1000000000};
  value=static_cast<double>(mantissa)/powers[fraction];if(negative)value=-value;end=p;return true;
}
struct Parser {
  Cursor cursor;
  explicit Parser(const std::string& s):cursor(s){}
  bool accept(std::string_view s){if(!cursor.done()&&!cursor.peek().quoted&&cursor.peek().equals(s)){cursor.next();return true;}return false;}
  void need(std::string_view s){if(!accept(s))error("expected "+std::string(s));}
  TokenView take(){if(cursor.done())error("unexpected end of SQL");return cursor.next();}
  std::string name(){auto t=take();if(t.quoted||t.text.empty()||t.text.size()>63||(!std::isalpha(static_cast<unsigned char>(t.text[0]))&&t.text[0]!='_'))error("expected identifier of at most 63 bytes");return t.str();}
  LiteralView literal_view(){auto t=take();
    if(!t.quoted){size_t start=(!t.text.empty()&&(t.text[0]=='+'||t.text[0]=='-'))?1:0;
      if(start==t.text.size()||(!std::isdigit(static_cast<unsigned char>(t.text[start]))&&t.text[start]!='.'))error("expected numeric or quoted literal");}
    return {t.text,t.quoted,t.escaped};}
  Literal literal(){auto t=literal_view();return {TokenView{t.text,t.quoted,t.escaped,false}.str(),t.quoted};}
  void insert_header(Statement& s,size_t max_columns=SIZE_MAX){s.kind=Statement::Insert;need("into");s.table=name();
    if(accept("(")){do{if(s.insert_columns.size()>=max_columns)error("INSERT width mismatch");s.insert_columns.push_back(name());}while(accept(","));need(")");}need("values");}
  void finish(){while(accept(";")){}if(!cursor.done())error("trailing SQL tokens (multiple statements are unsupported)");}
  void stream_insert(const std::function<void(const Statement&)>& header,const std::function<void(const std::vector<LiteralView>&)>& row,size_t max_columns){
    need("insert");Statement s{};insert_header(s,max_columns);header(s);std::vector<LiteralView> tuple;
    do{need("(");tuple.clear();do{if(tuple.size()>=max_columns)error("INSERT width mismatch");tuple.push_back(literal_view());}while(accept(","));need(")");row(tuple);}while(accept(","));finish();
  }
  void where(Statement& s){if(!accept("where"))return;do{Condition c;c.column=name();auto op=take();if(op.quoted)error("expected comparison operator");c.op=op.str();
    if(c.op!="="&&c.op!="!="&&c.op!="<>"&&c.op!="<"&&c.op!=">"&&c.op!="<="&&c.op!=">=")error("invalid comparison");
    auto v=literal();c.literal=std::move(v.text);c.quoted=v.quoted;s.conditions.push_back(std::move(c));
  }while(accept("and"));}
  Statement run(){Statement s{};
    if(accept("begin")){s.kind=Statement::Begin;accept("transaction");}
    else if(accept("commit"))s.kind=Statement::Commit;
    else if(accept("rollback"))s.kind=Statement::Rollback;
    else if(accept("set")){s.kind=Statement::Set;s.setting=name();need("=");s.value=take().str();}
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
    } else if(accept("insert")){insert_header(s);
      do{need("(");std::vector<Literal> row;do{row.push_back(literal());}while(accept(","));need(")");s.rows.push_back(std::move(row));}while(accept(","));
    } else if(accept("update")){s.kind=Statement::Update;s.table=name();need("set");do{auto col=name();need("=");s.assignments.emplace_back(col,literal());}while(accept(","));where(s);}
    else if(accept("delete")){s.kind=Statement::Delete;need("from");s.table=name();where(s);}
    else if(accept("select")){s.kind=Statement::Select;do{
      Expr e;if(accept("*"))e.column="*";
      else if(accept("l2_distance")){e.distance=true;need("(");e.column=name();need(",");auto q=take();if(!q.quoted)error("vector literal must be quoted");e.query=q.str();need(")");}
      else e.column=name();
      if(accept("as"))e.alias=name();else e.alias=e.distance?"l2_distance":e.column;
      s.expressions.push_back(std::move(e));
    }while(accept(","));need("from");s.table=name();where(s);
      if(accept("order")){need("by");do{auto col=name();bool desc=accept("desc");if(!desc)accept("asc");s.order.emplace_back(col,desc);}while(accept(","));}
      if(accept("limit")){auto count=take();if(count.quoted)error("LIMIT must be numeric");auto n=integer(count.text);if(n<0)error("negative LIMIT");s.limit=static_cast<uint64_t>(n);}
    }else error("unsupported SQL statement");
    while(accept(";")){} // Permit empty trailing statements, never a second SQL statement.
    if(!cursor.done())error("trailing SQL tokens (multiple statements are unsupported)");
    return s;
  }
};
template<class T>void append(std::string& s,T n){s.append(reinterpret_cast<const char*>(&n),sizeof(n));}
template<class T>T read(const std::string& s,size_t& p){if(p+sizeof(T)>s.size())error("invalid stored row");T n;std::memcpy(&n,s.data()+p,sizeof(n));p+=sizeof(n);return n;}

}
Statement parse(const std::string& s){return Parser(s).run();}
bool is_insert(const std::string& s){Cursor c(s);return !c.done()&&!c.peek().quoted&&c.peek().equals("insert");}
void parse_insert_rows(const std::string& s,const std::function<void(const Statement&)>& header,const std::function<void(const std::vector<LiteralView>&)>& row,size_t max_columns){Parser(s).stream_insert(header,row,max_columns);}
static size_t unicode_length_view(std::string_view s) {
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
size_t unicode_length(const std::string& s){return unicode_length_view(s);}
Value convert(const Literal& v,const Column& col) {return convert(LiteralView{v.text,v.quoted,false},col);}
Value convert(const LiteralView& v,const Column& col) {
  if(v.escaped){
    // Apostrophes cannot belong to numeric vector components. Reject before
    // decoding to prevent two simultaneous literal-sized scratch allocations.
    if(col.type==Type::Vector)error("invalid VECTOR component");
    auto decoded=TokenView{v.text,v.quoted,true,false}.str();return convert(LiteralView{decoded,v.quoted,false},col);
  }
  if(col.type==Type::Bigint){if(v.quoted)error("BIGINT literal must be numeric");return integer(v.text);}
  if(!v.quoted)error("string/vector literal must be quoted");
  if(col.type==Type::Varchar){if(unicode_length_view(v.text)>col.width)error("VARCHAR exceeds code point length");return std::string(v.text);}
  std::vector<float> out;out.reserve(std::min<size_t>(col.width,v.text.size()/2+1));
  size_t p=0;auto ws=[&]{while(p<v.text.size()&&std::isspace(static_cast<unsigned char>(v.text[p])))++p;};
  ws();if(p==v.text.size()||v.text[p++]!='[')error("invalid VECTOR literal");ws();
  if(p<v.text.size()&&v.text[p]!=']')while(true){
    const char* first=v.text.data()+p;const char* last=v.text.data()+v.text.size();
    const char* number=first;if(number!=last&&*number=='+')++number;
    if(number==last||(number!=first&&(*number=='-'||*number=='+')))error("invalid VECTOR component");
    double x=0;const char* end=number;
    if(!short_decimal(number,last,x,end)){
      auto parsed=std::from_chars(number,last,x,std::chars_format::general);end=parsed.ptr;
      const char* magnitude=number;if(magnitude!=last&&*magnitude=='-')++magnitude;
      const bool hex=last-magnitude>=2&&magnitude[0]=='0'&&(magnitude[1]=='x'||magnitude[1]=='X');
      if(hex||parsed.ec==std::errc::result_out_of_range){
        // Preserve strtod's hexadecimal and underflow-to-zero admission, using
        // an explicit locale only on these uncommon paths. Decimal parsing is
        // allocation-free and independent of the process locale.
        static locale_t c_locale=[] {auto locale=newlocale(LC_NUMERIC_MASK,"C",nullptr);if(!locale)error("cannot create numeric locale");return locale;}();
        // strtod requires a terminator. Bound its scratch copy to this numeric
        // component, so many hex/underflow values remain linear in input size
        // and a whitespace tail is never copied once per component.
        const char* component_end=first;
        while(component_end!=last&&*component_end!=','&&*component_end!=']'&&
              !std::isspace(static_cast<unsigned char>(*component_end)))++component_end;
        char scratch[64];std::string long_component;const char* bounded=nullptr;
        const auto length=static_cast<size_t>(component_end-first);
        if(length<sizeof(scratch)){std::memcpy(scratch,first,length);scratch[length]='\0';bounded=scratch;}
        else{long_component.assign(first,length);bounded=long_component.c_str();}
        // Never truncate: a late exponent or invalid suffix can change either
        // the value or admission. Only the uncommon long component allocates.
        char* fallback_end=nullptr;x=strtod_l(bounded,&fallback_end,c_locale);end=first+(fallback_end-bounded);
      }else if(parsed.ec!=std::errc{})error("invalid VECTOR component");
    }
    if(end==first||!std::isfinite(x)||std::abs(x)>std::numeric_limits<float>::max())error("nonfinite or overflowing VECTOR component");
    if(out.size()>=col.width)error("VECTOR dimension mismatch");
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

}
