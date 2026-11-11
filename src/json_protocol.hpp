#pragma once
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mvdb::protocol {
// Numbers remain source text: request identifiers must never pass through double.
struct Json {
  char kind=0;
  std::string text,raw;
  std::map<std::string,Json> fields;
};
class Parser {
  std::string_view source;
  size_t pos=0;
  bool request_only=false;
  [[noreturn]] void fail() const {throw std::runtime_error("invalid JSON request");}
  void ws(){while(pos<source.size()&&(source[pos]==' '||source[pos]=='\t'||source[pos]=='\r'||source[pos]=='\n'))++pos;}
  bool eat(char c){if(pos<source.size()&&source[pos]==c){++pos;return true;}return false;}
  unsigned hex4(){unsigned value=0;for(int i=0;i<4;++i){if(pos==source.size())fail();char c=source[pos++];value<<=4;if(c>='0'&&c<='9')value+=c-'0';else if(c>='a'&&c<='f')value+=c-'a'+10;else if(c>='A'&&c<='F')value+=c-'A'+10;else fail();}return value;}
  static void utf8(std::string& out,unsigned c){if(c<0x80)out+=static_cast<char>(c);else if(c<0x800){out+=static_cast<char>(0xc0|(c>>6));out+=static_cast<char>(0x80|(c&63));}else if(c<0x10000){out+=static_cast<char>(0xe0|(c>>12));out+=static_cast<char>(0x80|((c>>6)&63));out+=static_cast<char>(0x80|(c&63));}else {out+=static_cast<char>(0xf0|(c>>18));out+=static_cast<char>(0x80|((c>>12)&63));out+=static_cast<char>(0x80|((c>>6)&63));out+=static_cast<char>(0x80|(c&63));}}
  std::string string(){
    if(!eat('"'))fail();
    std::string out;
    while(pos<source.size()){
      unsigned char c=source[pos++];if(c=='"')return out;if(c<32)fail();
      if(c=='\\'){
        if(pos==source.size())fail();
        char escape=source[pos++];
        switch(escape){case '"':case '\\':case '/':out+=escape;break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
          case 'u':{unsigned u=hex4();if(u>=0xd800&&u<=0xdbff){if(!eat('\\')||!eat('u'))fail();unsigned low=hex4();if(low<0xdc00||low>0xdfff)fail();u=0x10000+((u-0xd800)<<10)+(low-0xdc00);}else if(u>=0xdc00&&u<=0xdfff)fail();utf8(out,u);break;}default:fail();}
      }else if(c<0x80)out+=static_cast<char>(c);
      else {
        unsigned count=0,u=0,min=0;
        if(c>=0xc2&&c<=0xdf){count=1;u=c&31;min=0x80;}else if(c>=0xe0&&c<=0xef){count=2;u=c&15;min=0x800;}else if(c>=0xf0&&c<=0xf4){count=3;u=c&7;min=0x10000;}else fail();
        out+=static_cast<char>(c);for(unsigned i=0;i<count;++i){if(pos==source.size())fail();unsigned char next=source[pos++];if((next&0xc0)!=0x80)fail();u=(u<<6)|(next&63);out+=static_cast<char>(next);}if(u<min||u>0x10ffff||(u>=0xd800&&u<=0xdfff))fail();
      }
    }fail();
  }
  bool digit() const {return pos<source.size()&&source[pos]>='0'&&source[pos]<='9';}
  Json value(unsigned depth,bool capture_raw=false){
    if(depth>128)fail();
    ws();if(pos==source.size())fail();size_t start=pos;Json result;result.kind=source[pos];
    if(result.kind=='"')result.text=string();
    else if(eat('{')){ws();if(!eat('}')){for(;;){ws();auto key=string();ws();if(!eat(':'))fail();auto child=value(depth+1,depth==0&&key=="id");if(!result.fields.emplace(std::move(key),std::move(child)).second)fail();ws();if(eat('}'))break;if(!eat(','))fail();}}}
    else if(eat('[')){ws();if(!eat(']')){for(;;){value(depth+1);ws();if(eat(']'))break;if(!eat(','))fail();}}}
    else if(result.kind=='t'||result.kind=='f'||result.kind=='n'){std::string_view word=result.kind=='t'?"true":result.kind=='f'?"false":"null";if(source.substr(pos,word.size())!=word)fail();pos+=word.size();}
    else {
      result.kind='0';eat('-');if(eat('0')){if(digit())fail();}else {if(!digit())fail();while(digit())++pos;}
      if(eat('.')){if(!digit())fail();while(digit())++pos;}
      if(eat('e')||eat('E')){if(!eat('+'))eat('-');if(!digit())fail();while(digit())++pos;}
    }
    if(!request_only||capture_raw)result.raw=std::string(source.substr(start,pos-start));
    return result;
  }
public:
  explicit Parser(std::string_view input,bool request=false):source(input),request_only(request){}
  Json parse(){auto result=value(0);ws();if(pos!=source.size())fail();return result;}
};
template<class Request>inline auto& required_string(Request& request,const std::string& field){auto it=request.fields.find(field);if(it==request.fields.end()||it->second.kind!='"')throw std::runtime_error("request requires string field: "+field);return it->second.text;}
}
