#include "engine.hpp"
#include "cli_budget.hpp"
#include "json_protocol.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <charconv>
#include <cmath>
#include <string>
#include <unistd.h>
namespace {
uint64_t proc_value(const char* path,const std::string& key){std::ifstream file(path);std::string line;while(std::getline(file,line))if(line.starts_with(key+":")){std::istringstream value(line.substr(key.size()+1));uint64_t number=0;value>>number;return number;}return 0;}
uint64_t positive_bytes(const std::string& value){uint64_t bytes=0;auto[end,error]=std::from_chars(value.data(),value.data()+value.size(),bytes);if(error!=std::errc{}||end!=value.data()+value.size()||bytes==0)throw std::runtime_error("memory budget must be a positive integer byte count");return bytes;}
// Buffer up to the public transport limit so a serialization failure never emits
// half a JSON response. No temporary files or extra disk writes are required.
class Response {
  std::string data;
public:
  Response& add(const std::string& text){if(text.size()>16*1024*1024-data.size())throw std::runtime_error("response exceeds protocol size limit");data+=text;return *this;}
  void emit() const {std::cout<<data<<'\n'<<std::flush;}
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
  try{
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
    std::cout<<"{\"type\":\"ready\"}\n"<<std::flush;
    std::string line;bool closed=false;
    while(std::getline(std::cin,line)){
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
            std::cout<<"{\"id\":"<<id<<",\"ok\":true}\n"<<std::flush;
          }else if(op=="ping")std::cout<<"{\"id\":"<<id<<",\"ok\":true}\n"<<std::flush;
          else if(op=="execute"){
            sql=mvdb::protocol::required_string(request,"sql");std::string mode="exact";
            if(request.fields.contains("search_mode"))mode=mvdb::protocol::required_string(request,"search_mode");
            if(mode=="approx")mode="ann";
            if(mode!="exact"&&mode!="ann")throw std::runtime_error("search_mode must be exact, ann, or approx");
            code="";engine.execute("SET SEARCH_MODE = "+mode+";");success(id,false,engine.execute(sql));
          }else {code="unsupported_op";throw std::runtime_error("unsupported operation");}
        }else {code="";success(id,true,engine.execute(sql));}
      }catch(const std::exception& e){
        if(legacy)std::cout<<"{\"ok\":false,\"error\":"<<mvdb::quote(e.what())<<"}\n"<<std::flush;
        else std::cout<<"{\"id\":"<<id<<",\"ok\":false,\"error\":{\"code\":"<<mvdb::quote(code.empty()?error_code(e.what()):code)<<",\"message\":"<<mvdb::quote(e.what())<<"}}\n"<<std::flush;
      }
      std::cerr<<"METRICS {\"engine_budget_bytes\":"<<budget<<",\"budget_source\":"<<mvdb::quote(budget_source)<<",\"wal_syncs\":"<<engine.wal_syncs()<<",\"vector_page_reads\":"<<(engine.vector_page_reads()-before)<<",\"budget_peak\":"<<engine.budget_peak()<<",\"spill_bytes\":"<<(engine.spill_bytes()-spill_before)<<",\"data_page_write_bytes\":"<<engine.data_page_write_bytes()<<",\"undo_spill_bytes\":"<<engine.undo_spill_bytes()<<",\"wal_bytes_written\":"<<engine.wal_bytes_written()<<",\"nlist\":"<<engine.last_nlist()<<",\"recommended_nprobe\":"<<engine.last_recommended_nprobe()<<",\"effective_nprobe\":"<<engine.effective_nprobe()<<",\"ann_route\":"<<mvdb::quote(engine.last_route())<<"}"<<std::endl;
      if(!std::cout)return 3;
      if(shutdown)break;
    }
    if(!closed)engine.close();
    auto read_bytes=proc_value("/proc/self/io","read_bytes"),write_bytes=proc_value("/proc/self/io","write_bytes");
    auto high_water=proc_value("/proc/self/status","VmHWM")*1024;
    std::cerr<<"FINAL_METRICS {\"engine_budget_bytes\":"<<budget<<",\"budget_source\":"<<mvdb::quote(budget_source)<<",\"wal_syncs\":"<<engine.wal_syncs()<<",\"data_page_write_bytes\":"<<engine.data_page_write_bytes()<<",\"undo_spill_bytes\":"<<engine.undo_spill_bytes()<<",\"wal_bytes_written\":"<<engine.wal_bytes_written()<<",\"budget_peak\":"<<engine.budget_peak()<<",\"process_read_bytes\":"<<read_bytes<<",\"process_write_bytes\":"<<write_bytes<<",\"vm_hwm_bytes\":"<<high_water<<"}"<<std::endl;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
