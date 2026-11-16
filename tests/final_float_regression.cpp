#include "../src/sql.hpp"
#include <bit>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iostream>
#include <limits>
#include <locale.h>
#include <random>
#include <stdexcept>
#include <algorithm>
namespace {
using namespace mvdb;
[[noreturn]] void error(const std::string& s){throw std::runtime_error(s);}
// Frozen 4722 decimal helper and VECTOR conversion (non-VECTOR branches
// omitted). Numeric bodies below are unchanged. The original
// double conversion followed by float cast is deliberate, including midpoints.
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
mvdb::Value frozen_convert(const LiteralView& v,const Column& col) {
  if(v.escaped)error("invalid VECTOR component");
  if(!v.quoted)error("string/vector literal must be quoted");
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
struct Result {std::vector<float> values;std::string error;};
Result read(std::string_view text,uint32_t width,bool old){
  try {auto v=old?frozen_convert(LiteralView{text,true,false},Column{"v",Type::Vector,width}):mvdb::convert(LiteralView{text,true,false},Column{"v",Type::Vector,width});return {std::get<std::vector<float>>(v),{}};}
  catch(const std::exception& e){return {{},e.what()};}
}
size_t checked=0,direct_differences=0;
void check(const std::string& s,uint32_t width=1){
  auto a=read(s,width,true),b=read(s,width,false);
  if(a.error!=b.error||a.values.size()!=b.values.size())throw std::runtime_error("admission mismatch: "+s);
  for(size_t i=0;i<a.values.size();++i)if(std::bit_cast<uint32_t>(a.values[i])!=std::bit_cast<uint32_t>(b.values[i]))throw std::runtime_error("bits mismatch: "+s);
  if(width==1&&a.error.empty()){char* end=nullptr;float direct=strtof(s.c_str()+1,&end);if(*end==']'&&std::bit_cast<uint32_t>(direct)!=std::bit_cast<uint32_t>(a.values[0]))++direct_differences;}
  ++checked;
}
}
void final_float_regression(){
  for(const char* s:{"0","-0","+0","0.0000000000000000000000","-0e22","1e-22","999999999999999e22","1.000000059604644775390625","1.0000000596046448","1.0000000596046447","-1.0000000596046448","3.4028234663852886e38","3.4028234663852887e38","1.1754943508222875e-38","1.401298464324817e-45","1e-9999","-1e-9999","1e9999","0x1.000001p0","nan","NaN(1)","inf","-infinity",".","+","--1","+-1","1e","1e+","1.2.3","1x","1e22x","000000000000000000000001.0"})check(std::string("[")+s+"]");
  check("[1,2]",1);check("[1]",2);check("[1,]",2);check("[1 2]",2);check("[]",0);check("[]",1);check("[1]x");check(std::string("[1\0,2]",7),2);
  for(int e=-25;e<=25;++e){
    for(const char* m:{"1","999999999","999999999999999","1000000080","-1000000080","000000000000001"})check("["+std::string(m)+"e"+std::to_string(e)+"]");
  }
  check("[0x1."+std::string(4096,'0')+"1p-149]");
  check("[0."+std::string(4096,'0')+"1e4097]");
  check("[1"+std::string(4096,'0')+"e-4096x]");
  auto negative_zero=read("[-0]",1,false);assert(std::bit_cast<uint32_t>(negative_zero.values[0])==0x80000000);
  bool escaped=false;try{mvdb::convert(LiteralView{"[1'0]",true,true},Column{"v",Type::Vector,1});}catch(const std::runtime_error& e){escaped=std::string(e.what())=="invalid VECTOR component";}assert(escaped);
  std::mt19937_64 rng(0x47221522);
  for(unsigned i=0;i<2000000;++i){
    auto digits=1+rng()%24;std::string s= rng()%2?"-":"";
    unsigned dot=rng()%(digits+1);
    for(unsigned j=0;j<digits;++j){if(j==dot)s+='.';s+=char('0'+rng()%10);}
    if(rng()%2)s+='e'+std::to_string(static_cast<int>(rng()%151)-75);
    check("["+s+"]");
  }
  // Decimal neighbors of float midpoints expose double rounding. Generate
  // exact binary midpoint values, then nearby 17-digit decimals and signs.
  for(unsigned i=0;i<200000;++i){
    uint32_t bits=0x00800000+uint32_t(rng()%(0x7f7ffffe - 0x00800000));
    float f=std::bit_cast<float>(bits),g=std::bit_cast<float>(bits+1);
    double mid=(double(f)+double(g))/2;
    for(double d:{std::nextafter(mid,0.0),mid,std::nextafter(mid,std::numeric_limits<double>::infinity())}){
      char b[64];auto end=std::to_chars(b,b+64,d,std::chars_format::general,17).ptr;check("["+std::string(b,end)+"]");check("[-"+std::string(b,end)+"]");
    }
  }
  // Vector parsing and text validation remain independent of the decimal path.
  auto utf=mvdb::convert(LiteralView{"\xe4\xb8\xad",true,false},Column{"s",Type::Varchar,1});assert(std::get<std::string>(utf).size()==3);
  bool rejected=false;try{mvdb::convert(LiteralView{"\xc0\x80",true,false},Column{"s",Type::Varchar,2});}catch(...){rejected=true;}assert(rejected);
  std::cout<<"frozen472 components checked="<<checked<<" mismatches=0 direct_strtof_differences="<<direct_differences<<'\n';
}
#ifdef MVDB_FINAL_FLOAT_STANDALONE
int main(){final_float_regression();}
#endif
