#include "engine.hpp"
#include "cli_budget.hpp"
#include "json_protocol.hpp"
#include <csignal>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <charconv>
#include <cmath>
#include <string>
#include <type_traits>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio_ext.h>
#include <unistd.h>
namespace {
volatile std::sig_atomic_t stop_requested=0;
// A handler must not enter the engine, allocate memory, or perform stream I/O.
void request_stop(int){stop_requested=1;}
void install_signal_handlers(){
  struct sigaction action{};action.sa_handler=request_stop;::sigemptyset(&action.sa_mask);
  action.sa_flags=0; // In particular, do not restart an idle stdin read.
  if(::sigaction(SIGTERM,&action,nullptr)<0||::sigaction(SIGINT,&action,nullptr)<0)throw std::runtime_error("cannot install termination signal handlers");
}
bool output_ok=true;
size_t atomic_pipe_limit=0;
void detect_atomic_pipe_output(){
  struct stat status{};
  if(::fstat(STDOUT_FILENO,&status)<0||!S_ISFIFO(status.st_mode))return;
  int flags=::fcntl(STDOUT_FILENO,F_GETFL);
  if(flags<0||(flags&O_NONBLOCK))return;
  long bytes=::fpathconf(STDOUT_FILENO,_PC_PIPE_BUF);
  if(bytes>0)atomic_pipe_limit=std::min<size_t>(4096,static_cast<size_t>(bytes));
}
void emit_buffer(std::string_view text){
  if(!output_ok)return;
  if(atomic_pipe_limit&&text.size()<=atomic_pipe_limit&&::__fpending(stdout)==0){
    // With an empty 64 KiB stdio buffer, this record reaches the blocking
    // pipe in one write <= PIPE_BUF. Linux either commits the entire record,
    // or EINTR reports zero bytes. Retain the record and replay only that case.
    for(;;){
      errno=0;auto written=std::fwrite(text.data(),1,text.size(),stdout);int write_error=errno;
      bool staging_error=std::ferror(stdout)!=0;
      if(written!=text.size()||staging_error){
        if(write_error==EINTR&&(written==0||written==text.size())){std::clearerr(stdout);::__fpurge(stdout);continue;}
        output_ok=false;return;
      }
      errno=0;int flushed=std::fflush(stdout);int flush_error=errno;
      if(flushed==0)return;
      if(flush_error==EINTR){std::clearerr(stdout);::__fpurge(stdout);continue;}
      output_ok=false;return;
    }
  }
  // glibc may discard a buffered unwritten tail after an interrupted fflush.
  // Defer our termination handlers until this complete record is flushed.
  // A stalled reader can therefore delay shutdown until it drains the pipe,
  // just as the old raw-write loop waited while retrying a partial record.
  sigset_t blocked,previous;::sigemptyset(&blocked);::sigaddset(&blocked,SIGTERM);::sigaddset(&blocked,SIGINT);
  if(::sigprocmask(SIG_BLOCK,&blocked,&previous)<0)throw std::runtime_error("cannot protect protocol output");
  auto written=std::fwrite(text.data(),1,text.size(),stdout);
  bool complete=written==text.size();
  if(complete)complete=std::fflush(stdout)==0;
  // An unexpected I/O failure is fatal to this transport. Do not retry fflush
  // or append another JSON record to a stream whose progress is uncertain.
  if(!complete)output_ok=false;
  if(::sigprocmask(SIG_SETMASK,&previous,nullptr)<0)output_ok=false;
}
// Read chunks rather than individual bytes. poll's bounded wait also covers a
// signal arriving between the flag check and the blocking syscall. Keep any
// extra complete records for the next request, and accept a final EOF record.
class InputLines {
  std::string buffered;
  size_t begin=0,scan=0;
  bool eof=false;
public:
  bool next(std::string& line){
    while(!stop_requested){
      auto end=buffered.find('\n',scan);
      if(end!=std::string::npos){line.assign(buffered,begin,end-begin);begin=end+1;scan=begin;
        if(begin==buffered.size()){buffered.clear();begin=scan=0;}return true;}
      scan=buffered.size();
      if(eof){if(begin==buffered.size())return false;line.assign(buffered,begin,std::string::npos);buffered.clear();begin=scan=0;return true;}
      if(begin){buffered.erase(0,begin);scan-=begin;begin=0;}
      struct pollfd input{STDIN_FILENO,POLLIN,0};
      int ready=::poll(&input,1,100);
      if(stop_requested)return false;
      if(ready<0){if(errno==EINTR)continue;throw std::runtime_error("stdin poll failed");}
      if(!ready)continue;
      if(input.revents&POLLNVAL)throw std::runtime_error("invalid stdin descriptor");
      char chunk[64*1024];auto count=::read(STDIN_FILENO,chunk,sizeof(chunk));
      if(count<0){if(errno==EINTR||errno==EAGAIN)continue;throw std::runtime_error("stdin read failed");}
      if(count==0)eof=true;else buffered.append(chunk,static_cast<size_t>(count));
    }
    return false;
  }
};
uint64_t proc_value(const char* path,const std::string& key){std::ifstream file(path);std::string line;while(std::getline(file,line))if(line.starts_with(key+":")){std::istringstream value(line.substr(key.size()+1));uint64_t number=0;value>>number;return number;}return 0;}
uint64_t positive_bytes(const std::string& value){uint64_t bytes=0;auto[end,error]=std::from_chars(value.data(),value.data()+value.size(),bytes);if(error!=std::errc{}||end!=value.data()+value.size()||bytes==0)throw std::runtime_error("memory budget must be a positive integer byte count");return bytes;}
// Buffer up to the public transport limit so a serialization failure never emits
// half a JSON response. No temporary files or extra disk writes are required.
class Response {
  std::string data;
  static constexpr size_t limit=16*1024*1024;
public:
  Response(){data.reserve(16384);}
  void clear(){
    // Release exceptional large responses before later memory-limited work;
    // ordinary responses retain the preallocated assembly buffer.
    if(data.capacity()>256*1024){std::string fresh;fresh.reserve(16384);data.swap(fresh);}else data.clear();
  }
  std::string_view view() const {return data;}
  Response& add(std::string_view text){if(text.size()>limit-data.size())throw std::runtime_error("response exceeds protocol size limit");data.append(text);return *this;}
  Response& string(std::string_view text){
    add("\"");size_t run=0;
    constexpr char hex[]="0123456789abcdef";
    for(size_t i=0;i<text.size();){unsigned char c=text[i];
      if(c>=0x80){
        unsigned count=0,code=0,minimum=0;
        if(c>=0xc2&&c<=0xdf){count=1;code=c&31;minimum=0x80;}
        else if(c>=0xe0&&c<=0xef){count=2;code=c&15;minimum=0x800;}
        else if(c>=0xf0&&c<=0xf4){count=3;code=c&7;minimum=0x10000;}
        else throw std::runtime_error("invalid JSON request");
        if(text.size()-i<=count)throw std::runtime_error("invalid JSON request");
        for(unsigned j=1;j<=count;++j){unsigned char next=text[i+j];if((next&0xc0)!=0x80)throw std::runtime_error("invalid JSON request");code=(code<<6)|(next&63);}
        if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff))throw std::runtime_error("invalid JSON request");
        i+=count+1;continue;
      }
      if(c=='\"'||c=='\\'||c<32){
        add(text.substr(run,i-run));
        if(c<32){char escaped[6]={'\\','u','0','0',hex[c>>4],hex[c&15]};add(std::string_view(escaped,6));}
        else {char escaped[2]={'\\',char(c)};add(std::string_view(escaped,2));}
        run=++i;
      }else ++i;
    }
    add(text.substr(run));return add("\"");
  }
  template<class Number>Response& integer(Number value){
    static_assert(std::is_integral_v<Number>);char buffer[64];auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);
    if(result.ec!=std::errc{})throw std::runtime_error("cannot serialize number");
    return add(std::string_view(buffer,size_t(result.ptr-buffer)));
  }
  Response& number(double value){
    if(!std::isfinite(value))throw std::runtime_error("non-finite result value");
    char buffer[64];auto result=std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::general,17);
    if(result.ec!=std::errc{})throw std::runtime_error("cannot serialize number");
    return add(std::string_view(buffer,size_t(result.ptr-buffer)));
  }
  Response& value(const mvdb::Value& value){
    if(auto n=std::get_if<int64_t>(&value))return integer(*n);
    if(auto n=std::get_if<double>(&value))return number(*n);
    if(auto text=std::get_if<std::string>(&value))return string(*text);
    const auto& vector=std::get<std::vector<float>>(value);
    for(float n:vector)if(!std::isfinite(n))throw std::runtime_error("non-finite vector result value");
    add("[");bool first=true;for(float n:vector){if(!first)add(",");first=false;number(static_cast<double>(n));}return add("]");
  }
  Response& begin(std::string_view id,bool legacy,bool ok=true){
    clear();if(legacy)add("{\"ok\":");else add("{\"id\":").add(id).add(",\"ok\":");
    return add(ok?"true":"false");
  }
  Response& ready(){clear();return add("{\"type\":\"ready\"}");}
  void emit(){
    // The public cap applies to JSON bytes; its line delimiter is additional.
    data.push_back('\n');
    try{emit_buffer(data);}catch(...){data.pop_back();throw;}
    data.pop_back();
  }
};
// cerr is unit-buffered: assemble telemetry before writing so each record uses
// one stream write instead of one write per key, separator and numeric value.
class Metrics {
  std::string data;
  bool first=true;
  void key(const char* name){if(!first)data+=',';first=false;data+='"';data+=name;data+="\":";}
public:
  Metrics(){data.reserve(2048);}
  void begin(const char* prefix){data.clear();data+=prefix;data+='{';first=true;}
  template<class Number>void number(const char* name,Number value){key(name);char buffer[32];auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);if(result.ec!=std::errc{})throw std::runtime_error("cannot serialize metrics");data.append(buffer,result.ptr);}
  void string(const char* name,const std::string& value){key(name);data+=mvdb::quote(value);}
  void emit(){data+="}\n";std::cerr.write(data.data(),static_cast<std::streamsize>(data.size()));}
};
void assemble_success(Response& response,const std::string& id,bool legacy,const mvdb::Result& result){
  response.begin(id,legacy);
  if(legacy||result.columns.empty())response.add(",\"affected_rows\":").integer(result.affected_rows);
  if(legacy||!result.columns.empty()){
    response.add(",\"columns\":[");bool first=true;for(const auto& name:result.columns){if(!first)response.add(",");first=false;response.string(name);}response.add("],\"rows\":[");first=true;
    result.each([&](const auto& row){if(!first)response.add(",");first=false;response.add("[");bool cell=true;for(const auto& value:row){if(!cell)response.add(",");cell=false;response.value(value);}response.add("]");});response.add("]");
  }
  response.add("}");
}
void success(Response& response,const std::string& id,bool legacy,const mvdb::Result& result){assemble_success(response,id,legacy,result);response.emit();}
std::string error_code(const std::string& message){if(message.find("duplicate primary key")!=std::string::npos)return "duplicate_key";if(message.find("dimension mismatch")!=std::string::npos)return "dimension_mismatch";if(message.find("unsupported SQL")!=std::string::npos)return "unsupported_sql";return "execution_error";}
bool json_line(const std::string& line){auto start=line.find_first_not_of(" \t\r\n");if(start==std::string::npos)return true;char first=line[start];return first=='{'||first=='['||first=='"'||first=='-'||(first>='0'&&first<='9')||line.substr(start)=="null"||line.substr(start)=="true"||line.substr(start)=="false";}
}
int main(int argc,char** argv){
  // Pipes and PID 1 must receive the handshake and every response immediately.
  static char output_buffer[64*1024];
  if(std::setvbuf(stdout,output_buffer,_IOFBF,sizeof(output_buffer))!=0){std::cerr<<"cannot buffer stdout\n";return 1;}
  try{
    install_signal_handlers();
    detect_atomic_pipe_output();
    const char* env_directory=std::getenv("VECTORDB_DATA_DIR");
    std::string directory=env_directory&&*env_directory?env_directory:"/data";auto detected=mvdb::default_cli_budget();uint64_t budget=detected.bytes;std::string budget_source=detected.source;bool positional=false;
    for(int i=1;i<argc;++i){std::string arg=argv[i];
      if(arg=="--help"||arg=="-h"){std::cout<<"usage: minivectordb [--data-dir PATH] [--memory-budget BYTES]\n       minivectordb DATA_DIR [MEMORY_BUDGET]\nDefaults: VECTORDB_DATA_DIR or /data; cgroup min(memory.max,memory.high) * 55%, clamped to 4..10 MiB; unlimited/unreadable: 10485760 bytes. JSONL protocol with legacy SQL-line compatibility.\n";return 0;}
      if(arg=="--data-dir"||arg=="--memory-budget"){if(positional||i+1==argc)throw std::runtime_error("invalid or mixed command-line arguments");auto value=std::string(argv[++i]);if(arg=="--data-dir"){if(value.empty())throw std::runtime_error("empty data directory");directory=value;}else {budget=positive_bytes(value);budget_source="explicit";}}
      else if(i==1&&!arg.starts_with("-")){positional=true;directory=arg;if(i+1<argc){budget=positive_bytes(argv[++i]);budget_source="explicit";}if(i+1<argc)throw std::runtime_error("too many positional arguments");}
      else throw std::runtime_error("unknown command-line argument: "+arg);
    }
    auto progress=[](size_t count){auto env=std::getenv("MVDB_RECOVERY_PAUSE_AFTER");if(env&&count==std::stoull(env)){std::cerr<<"RECOVERY_PAUSED "<<count<<std::endl;::raise(SIGSTOP);}};
    mvdb::Engine engine(directory,budget,progress);
    std::cerr<<"distance_kernel="<<(mvdb::avx2_available()?"avx2":"scalar")<<'\n';
    Response response;response.ready().emit();
    Metrics metrics;std::string line;bool closed=false;int exit_status=output_ok?0:3;InputLines input;
    while(output_ok&&input.next(line)){
      auto before=engine.vector_page_reads();auto spill_before=engine.spill_bytes();
      bool legacy=!json_line(line),shutdown=false;std::string id="null",code="invalid_request";
      try{
        std::string sql;
        if(!legacy){
          auto request=mvdb::protocol::Parser(line,true).parse();
          if(request.kind!='{')throw std::runtime_error("request must be a JSON object");
          auto ident=request.fields.find("id");if(ident==request.fields.end())throw std::runtime_error("request requires id");id=ident->second.raw;
          auto op=request.fields.contains("op")?mvdb::protocol::required_string(request,"op"):"execute";
          if(op=="shutdown"||op=="close"){
            code="shutdown_error";engine.close();closed=true;shutdown=true;
            response.begin(id,false).add("}").emit();
          }else if(op=="ping")response.begin(id,false).add("}").emit();
          else if(op=="execute"){
            sql=std::move(mvdb::protocol::required_string(request,"sql"));std::string mode="exact";
            if(request.fields.contains("search_mode"))mode=mvdb::protocol::required_string(request,"search_mode");
            if(mode=="approx")mode="ann";
            if(mode!="exact"&&mode!="ann")throw std::runtime_error("search_mode must be exact, ann, or approx");
            code="";engine.execute("SET SEARCH_MODE = "+mode+";");success(response,id,false,engine.execute(sql));
          }else {code="unsupported_op";throw std::runtime_error("unsupported operation");}
        }else {sql=line;code="";success(response,id,true,engine.execute(sql));}
      }catch(const std::exception& e){
        response.begin(id,legacy,false).add(",\"error\":");
        if(legacy)response.string(e.what()).add("}").emit();
        else response.add("{\"code\":").string(code.empty()?error_code(e.what()):code).add(",\"message\":").string(e.what()).add("}}").emit();
      }
      metrics.begin("METRICS ");
      metrics.number("engine_budget_bytes",budget);
      metrics.string("budget_source",budget_source);
      metrics.number("wal_syncs",engine.wal_syncs());
      metrics.number("vector_page_reads",engine.vector_page_reads()-before);
      metrics.number("budget_peak",engine.budget_peak());
      metrics.number("spill_bytes",engine.spill_bytes()-spill_before);
      metrics.number("data_page_write_bytes",engine.data_page_write_bytes());
      metrics.number("undo_spill_bytes",engine.undo_spill_bytes());
      metrics.number("wal_bytes_written",engine.wal_bytes_written());
      metrics.number("nlist",engine.last_nlist());
      metrics.number("recommended_nprobe",engine.last_recommended_nprobe());
      metrics.number("effective_nprobe",engine.effective_nprobe());
      metrics.string("ann_route",engine.last_route());
      metrics.emit();
      if(!output_ok){exit_status=3;break;}
      if(shutdown)break;
    }
    if(!closed)engine.close();
    auto read_bytes=proc_value("/proc/self/io","read_bytes"),write_bytes=proc_value("/proc/self/io","write_bytes");
    auto high_water=proc_value("/proc/self/status","VmHWM")*1024;
    metrics.begin("FINAL_METRICS ");
    metrics.number("engine_budget_bytes",budget);
    metrics.string("budget_source",budget_source);
    metrics.number("wal_syncs",engine.wal_syncs());
    metrics.number("data_page_write_bytes",engine.data_page_write_bytes());
    metrics.number("undo_spill_bytes",engine.undo_spill_bytes());
    metrics.number("wal_bytes_written",engine.wal_bytes_written());
    metrics.number("budget_peak",engine.budget_peak());
    metrics.number("process_read_bytes",read_bytes);
    metrics.number("process_write_bytes",write_bytes);
    metrics.number("vm_hwm_bytes",high_water);
    metrics.emit();
    return exit_status;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
