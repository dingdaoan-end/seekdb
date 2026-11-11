#include "../src/sql.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <locale.h>
#include <limits>
#include <stdexcept>
#include <cassert>
// Frozen baseline 9c8798a lexer/parser and conversion oracle.
namespace reference {
using namespace mvdb;
[[noreturn]] void error(const std::string& s){throw std::runtime_error(s);}
std::string lower(std::string s){for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
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
}

namespace {
std::string shape(const mvdb::Statement& s){
  std::string out;auto add=[&](const auto& v){auto x=std::to_string(v);out+=std::to_string(x.size())+":"+x;};
  auto text=[&](const std::string& x){out+=std::to_string(x.size())+":"+x;};
  add(s.kind);text(s.table);text(s.index);text(s.setting);text(s.value);add(s.limit);
  add(s.columns.size());for(auto& c:s.columns){text(c.name);add(static_cast<int>(c.type));add(c.width);add(c.primary);}
  add(s.rows.size());for(auto& r:s.rows){add(r.size());for(auto& v:r){text(v.text);add(v.quoted);}}
  add(s.insert_columns.size());for(auto& c:s.insert_columns)text(c);
  add(s.expressions.size());for(auto& e:s.expressions){text(e.column);text(e.alias);add(e.distance);text(e.query);}
  add(s.conditions.size());for(auto& c:s.conditions){text(c.column);text(c.op);text(c.literal);add(c.quoted);}
  add(s.assignments.size());for(auto& a:s.assignments){text(a.first);text(a.second.text);add(a.second.quoted);}
  add(s.order.size());for(auto& o:s.order){text(o.first);add(o.second);}return out;
}
void differential(const std::string& sql){
  bool old_ok=false,new_ok=false;mvdb::Statement old{},fresh{};
  try{old=reference::Parser(sql).run();old_ok=true;}catch(const std::runtime_error&){}
  try{fresh=mvdb::parse(sql);new_ok=true;}catch(const std::runtime_error&){}
  assert(old_ok==new_ok);if(old_ok)assert(shape(old)==shape(fresh));
  bool insert=false;try{insert=mvdb::is_insert(sql);}catch(const std::runtime_error&){}
  if(insert){
    bool stream_ok=false;mvdb::Statement streamed{};
    try{mvdb::parse_insert_rows(sql,[&](const auto& h){streamed=h;},[&](const auto& row){
      std::vector<mvdb::Literal> result;
      for(const auto& v:row){assert(v.text.data()>=sql.data()&&v.text.data()+v.text.size()<=sql.data()+sql.size());
        reference::Token t{std::string(v.text),v.quoted};
        if(v.escaped){t.text.clear();for(size_t i=0;i<v.text.size();++i){t.text+=v.text[i];if(v.text[i]=='\''&&i+1<v.text.size()&&v.text[i+1]=='\'')++i;}}
        result.push_back({t.text,t.quoted});}
      streamed.rows.push_back(std::move(result));});stream_ok=true;}catch(const std::runtime_error&){}
    assert(stream_ok==old_ok);if(old_ok)assert(shape(old)==shape(streamed));
  }
}
}
void sql_stream_regressions(){
  const std::vector<std::string> cases={
    "SELECT id, L2_DISTANCE(v,'[1e-2, +2,0x1p2]') AS d FROM T WHERE id >= -2 AND label='O''Brien' ORDER BY d ASC, id DESC LIMIT +3;;--end",
    "select * from t /*comment*/;", "select l2_distance(v,'[1,2]') from t order by l2_distance limit 0",
    "INSERT INTO T (id,v,label) VALUES (+1,'[1,2]','a''b'),(-2,'[3,4]','中文;--/*') /* end */;;;",
    "insert into t values (1,'')", "insert into t values (1,'a''''b')", "insert into t values (1,'a\\b')",
    "insert into t values (1,'[1]'),(2,'[2]') select * from t", "insert into t values (1,'[1]'),(2,",
    "insert into t values (1,'[1]'),(2,'unterminated", "insert into t values (1) /*unterminated",
    "insert into t values ()", "insert into t values (1,) ", "insert into t values (1),", "insert into t values (1);@",
    "insert into t values (1.2.3)", "insert into t values (1e+)", "select * from t limit 1e3",
    "begin transaction;", "commit;", "rollback;", "set mode='EXACT';",
    "create table t (id int, label varchar(3),v vector(2));", "create vector index idx on t(v)",
    "update t set label='O''Brien',id=-3 where id<>1", "delete from t where id<=2", "", "--comment",
    "SELECT 'name' FROM t", "select * from t; select * from t", "insert into t values ('/*--*/')"
  };
  const std::vector<std::string> tails={"", ";", ";;;/* tail */", ",", ")", "'", "/*", "--comment", "@", " select", "\n"};
  for(auto& sql:cases)for(auto& tail:tails)differential(sql+tail);
  uint32_t random=1234567;const std::string alphabet="'(),;/*-+0eE. @[]_\n";
  for(size_t i=0;i<1200;++i){random=random*1664525u+1013904223u;std::string sql=cases[random%cases.size()];
    random=random*1664525u+1013904223u;auto pos=random%(sql.size()+1);random=random*1664525u+1013904223u;
    sql.insert(pos,1,alphabet[random%alphabet.size()]);differential(sql);}
  std::string vector="[0";for(int i=1;i<12800;++i)vector+=",0";vector+="]";
  std::string sql="insert into t values (1,'"+vector+"')";
  assert(reference::lex(sql).size()==9);differential(sql);
  size_t seen=0;try{mvdb::parse_insert_rows("insert into t values (1),(2),(",[](const auto&){},[&](const auto&){++seen;});assert(false);}catch(const std::runtime_error&){}assert(seen==2);
  std::string wide="insert into t values (0";for(int i=0;i<10000;++i)wide+=",0";wide+=")";
  seen=0;try{mvdb::parse_insert_rows(wide,[](const auto&){},[&](const auto&){++seen;},64);assert(false);}catch(const std::runtime_error&){}assert(seen==0);
  wide="insert into t (id";for(int i=0;i<10000;++i)wide+=",id";wide+=") values (0)";
  seen=0;try{mvdb::parse_insert_rows(wide,[&](const auto&){++seen;},[](const auto&){},64);assert(false);}catch(const std::runtime_error&){}assert(seen==0);
  mvdb::Column one{"v",mvdb::Type::Vector,1,false};std::string oversized="[0";for(int i=0;i<10000;++i)oversized+=",0";oversized+="]";
  try{mvdb::convert(mvdb::LiteralView{oversized,true,false},one);assert(false);}catch(const std::runtime_error&){}
  mvdb::Column label{"s",mvdb::Type::Varchar,5,false};
  assert(std::get<std::string>(mvdb::convert(mvdb::LiteralView{"a''b",true,true},label))=="a'b");
  for(auto text:std::vector<std::string>{std::string("\xc0\xaf",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4)}){
    try{mvdb::convert(mvdb::LiteralView{text,true,false},label);assert(false);}catch(const std::runtime_error&){}
  }
  // The uncommon strtod path must copy only the current numeric component.
  // Long whitespace tails and many hexadecimal/underflow components preserve
  // the frozen converter's values, including the sign of underflowed zero.
  auto conversion_equal=[](const std::string& text,uint32_t dimensions){
    mvdb::Column column{"v",mvdb::Type::Vector,dimensions,false};
    auto expected=std::get<std::vector<float>>(reference::convert(mvdb::Literal{text,true},column));
    auto actual=std::get<std::vector<float>>(mvdb::convert(mvdb::LiteralView{text,true,false},column));
    assert(actual==expected);for(size_t i=0;i<actual.size();++i)assert(std::signbit(actual[i])==std::signbit(expected[i]));
  };
  conversion_equal("[+0x1.8p+2"+std::string(1<<20,' ')+"]",1);
  std::string many_hex="[";const std::vector<std::string> components={"-0x1.8p+2","+0x1p-2","1e-999","-1e-999"};
  for(size_t i=0;i<4096;++i){if(i)many_hex+=",";many_hex+=components[i%components.size()];}many_hex+="]";
  conversion_equal(many_hex,4096);
  try{mvdb::convert(mvdb::LiteralView{"[0x"+std::string(150000,'0')+"'']",true,true},mvdb::Column{"v",mvdb::Type::Vector,1,false});assert(false);}catch(const std::runtime_error&){}
  mvdb::Column col{"v",mvdb::Type::Vector,3,false};
  for(std::string text:{"[+1, -2.5, 0x1p2]","[1e-999, .5, -0]","[nan,1,2]","[1,2,]","[1e999,2,3]","[+-1,2,3]","[0x1 p2,2,3]","[1e-999oops,2,3]","[0x1, 0x2,0x3]"}){
    bool a=false,b=false;mvdb::Value x,y;try{x=reference::convert(mvdb::Literal{text,true},col);a=true;}catch(const std::runtime_error&){}
    try{y=mvdb::convert(mvdb::LiteralView{text,true,false},col);b=true;}catch(const std::runtime_error&){}
    assert(a==b);if(a)assert(x==y);
  }
}
