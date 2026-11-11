#include "engine.hpp"
#include "cli_budget.hpp"
#include "json_protocol.hpp"
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <charconv>
#include <cmath>
#include <string>
#include <poll.h>
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
void emit_line(const std::string& text){
  // Retain partial-write progress when a signal interrupts a full stdout pipe.
  // A stream insertion can otherwise set badbit after emitting half a record.
  auto write_all=[](const char* data,size_t size){
    while(size){auto count=::write(STDOUT_FILENO,data,size);
      if(count<0&&errno==EINTR)continue;
      if(count<=0){std::cout.setstate(std::ios::badbit);return false;}
      data+=count;size-=static_cast<size_t>(count);
    }return true;
  };
  if(write_all(text.data(),text.size()))write_all("\n",1);
  std::cout<<std::flush;std::fflush(stdout);
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
public:
  Response& add(const std::string& text){if(text.size()>16*1024*1024-data.size())throw std::runtime_error("response exceeds protocol size limit");data+=text;return *this;}
  void emit() const {emit_line(data);}
};
std::string value_json(const mvdb::Value& value){
  if(auto number=std::get_if<double>(&value)){if(!std::isfinite(*number))throw std::runtime_error("non-finite result value");}
  if(auto vector=std::get_if<std::vector<float>>(&value)){for(float number:*vector)if(!std::isfinite(number))throw std::runtime_error("non-finite vector result value");}
  if(auto string=std::get_if<std::string>(&value)){mvdb::protocol::Parser(mvdb::quote(*string)).parse();}
  return mvdb::json(value);
}
void success(const std::string& id,bool legacy,const mvdb::Result& result){
  Response response;response.add(legacy?"{\"ok\":true":"{\"id\":"+id+",\"ok\":true");
  if(legacy||result.columns.empty())response.add(",\"affected_rows\":"+std::to_string(result.affected_rows));
  if(legacy||!result.columns.empty()){
    response.add(",\"columns\":[");bool first=true;for(const auto& name:result.columns){if(!first)response.add(",");first=false;response.add(mvdb::quote(name));}response.add("],\"rows\":[");first=true;
    result.each([&](const auto& row){if(!first)response.add(",");first=false;response.add("[");bool cell=true;for(const auto& value:row){if(!cell)response.add(",");cell=false;response.add(value_json(value));}response.add("]");});response.add("]");
  }
  response.add("}").emit();
}
std::string error_code(const std::string& message){if(message.find("duplicate primary key")!=std::string::npos)return "duplicate_key";if(message.find("dimension mismatch")!=std::string::npos)return "dimension_mismatch";if(message.find("unsupported SQL")!=std::string::npos)return "unsupported_sql";return "execution_error";}
bool json_line(const std::string& line){auto start=line.find_first_not_of(" \t\r\n");if(start==std::string::npos)return true;char first=line[start];return first=='{'||first=='['||first=='"'||first=='-'||(first>='0'&&first<='9')||line.substr(start)=="null"||line.substr(start)=="true"||line.substr(start)=="false";}
}
int main(int argc,char** argv){
  // Pipes and PID 1 must receive the handshake and every response immediately.
  std::setvbuf(stdout,nullptr,_IONBF,0);std::cout<<std::unitbuf;
  try{
    install_signal_handlers();
    const char* env_directory=std::getenv("VECTORDB_DATA_DIR");
    std::string directory=env_directory&&*env_directory?env_directory:"/data";auto detected=mvdb::default_cli_budget();uint64_t budget=detected.bytes;std::string budget_source=detected.source;bool positional=false;
    for(int i=1;i<argc;++i){std::string arg=argv[i];
      if(arg=="--help"||arg=="-h"){std::cout<<"usage: minivectordb [--data-dir PATH] [--memory-budget BYTES]\n       minivectordb DATA_DIR [MEMORY_BUDGET]\nDefaults: VECTORDB_DATA_DIR or /data; cgroup min(memory.max,memory.high) * 55%, clamped to 4..24 MiB; unlimited/unreadable: 20971520 bytes. JSONL protocol with legacy SQL-line compatibility.\n";return 0;}
      if(arg=="--data-dir"||arg=="--memory-budget"){if(positional||i+1==argc)throw std::runtime_error("invalid or mixed command-line arguments");auto value=std::string(argv[++i]);if(arg=="--data-dir"){if(value.empty())throw std::runtime_error("empty data directory");directory=value;}else {budget=positive_bytes(value);budget_source="explicit";}}
      else if(i==1&&!arg.starts_with("-")){positional=true;directory=arg;if(i+1<argc){budget=positive_bytes(argv[++i]);budget_source="explicit";}if(i+1<argc)throw std::runtime_error("too many positional arguments");}
      else throw std::runtime_error("unknown command-line argument: "+arg);
    }
    auto progress=[](size_t count){auto env=std::getenv("MVDB_RECOVERY_PAUSE_AFTER");if(env&&count==std::stoull(env)){std::cerr<<"RECOVERY_PAUSED "<<count<<std::endl;::raise(SIGSTOP);}};
    mvdb::Engine engine(directory,budget,progress);
    std::cerr<<"distance_kernel="<<(mvdb::avx2_available()?"avx2":"scalar")<<'\n';
    emit_line("{\"type\":\"ready\"}");
    std::string line;bool closed=false;int exit_status=0;InputLines input;
    while(input.next(line)){
      auto before=engine.vector_page_reads();auto spill_before=engine.spill_bytes();
      bool legacy=!json_line(line),shutdown=false;std::string id="null",code="invalid_request";
      try{
        std::string sql=line;
        if(!legacy){
          auto request=mvdb::protocol::Parser(line).parse();
          if(request.kind!='{')throw std::runtime_error("request must be a JSON object");
          auto ident=request.fields.find("id");if(ident==request.fields.end())throw std::runtime_error("request requires id");id=ident->second.raw;
          auto op=request.fields.contains("op")?mvdb::protocol::required_string(request,"op"):"execute";
          if(op=="shutdown"||op=="close"){
            code="shutdown_error";engine.close();closed=true;shutdown=true;
            emit_line("{\"id\":"+id+",\"ok\":true}");
          }else if(op=="ping")emit_line("{\"id\":"+id+",\"ok\":true}");
          else if(op=="execute"){
            sql=mvdb::protocol::required_string(request,"sql");std::string mode="exact";
            if(request.fields.contains("search_mode"))mode=mvdb::protocol::required_string(request,"search_mode");
            if(mode=="approx")mode="ann";
            if(mode!="exact"&&mode!="ann")throw std::runtime_error("search_mode must be exact, ann, or approx");
            code="";engine.execute("SET SEARCH_MODE = "+mode+";");success(id,false,engine.execute(sql));
          }else {code="unsupported_op";throw std::runtime_error("unsupported operation");}
        }else {code="";success(id,true,engine.execute(sql));}
      }catch(const std::exception& e){
        if(legacy)emit_line("{\"ok\":false,\"error\":"+mvdb::quote(e.what())+"}");
        else emit_line("{\"id\":"+id+",\"ok\":false,\"error\":{\"code\":"+mvdb::quote(code.empty()?error_code(e.what()):code)+",\"message\":"+mvdb::quote(e.what())+"}}");
      }
      std::cerr<<"METRICS {\"engine_budget_bytes\":"<<budget<<",\"budget_source\":"<<mvdb::quote(budget_source)<<",\"wal_syncs\":"<<engine.wal_syncs()<<",\"vector_page_reads\":"<<(engine.vector_page_reads()-before)<<",\"budget_peak\":"<<engine.budget_peak()<<",\"spill_bytes\":"<<(engine.spill_bytes()-spill_before)<<",\"data_page_write_bytes\":"<<engine.data_page_write_bytes()<<",\"undo_spill_bytes\":"<<engine.undo_spill_bytes()<<",\"wal_bytes_written\":"<<engine.wal_bytes_written()<<",\"nlist\":"<<engine.last_nlist()<<",\"recommended_nprobe\":"<<engine.last_recommended_nprobe()<<",\"effective_nprobe\":"<<engine.effective_nprobe()<<",\"ann_route\":"<<mvdb::quote(engine.last_route())<<"}"<<std::endl;
      if(!std::cout){exit_status=3;break;}
      if(shutdown)break;
    }
    if(!closed)engine.close();
    auto read_bytes=proc_value("/proc/self/io","read_bytes"),write_bytes=proc_value("/proc/self/io","write_bytes");
    auto high_water=proc_value("/proc/self/status","VmHWM")*1024;
    std::cerr<<"FINAL_METRICS {\"engine_budget_bytes\":"<<budget<<",\"budget_source\":"<<mvdb::quote(budget_source)<<",\"wal_syncs\":"<<engine.wal_syncs()<<",\"data_page_write_bytes\":"<<engine.data_page_write_bytes()<<",\"undo_spill_bytes\":"<<engine.undo_spill_bytes()<<",\"wal_bytes_written\":"<<engine.wal_bytes_written()<<",\"budget_peak\":"<<engine.budget_peak()<<",\"process_read_bytes\":"<<read_bytes<<",\"process_write_bytes\":"<<write_bytes<<",\"vm_hwm_bytes\":"<<high_water<<"}"<<std::endl;
    return exit_status;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
