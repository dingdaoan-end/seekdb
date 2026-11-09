#include "engine.hpp"
#include "cli_budget.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <charconv>
#include <string>
#include <unistd.h>
namespace {
uint64_t proc_value(const char* path,const std::string& key){std::ifstream file(path);std::string line;while(std::getline(file,line))if(line.starts_with(key+":")){std::istringstream value(line.substr(key.size()+1));uint64_t number=0;value>>number;return number;}return 0;}
uint64_t positive_bytes(const std::string& value){uint64_t bytes=0;auto[end,error]=std::from_chars(value.data(),value.data()+value.size(),bytes);if(error!=std::errc{}||end!=value.data()+value.size()||bytes==0)throw std::runtime_error("memory budget must be a positive integer byte count");return bytes;}
}
// Local development framing only; no competition handshake or error codes.
int main(int argc,char** argv){
  try{
    std::string directory="/data";auto detected=mvdb::default_cli_budget();uint64_t budget=detected.bytes;std::string budget_source=detected.source;bool positional=false;
    for(int i=1;i<argc;++i){std::string arg=argv[i];
      if(arg=="--help"||arg=="-h"){std::cout<<"usage: minivectordb [--data-dir PATH] [--memory-budget BYTES]\n       minivectordb DATA_DIR [MEMORY_BUDGET]\nDefaults: /data; cgroup min(memory.max,memory.high) * 55%, clamped to 4..24 MiB; unlimited/unreadable: 20971520 bytes. Local SQL-line/JSON-line adapter.\n";return 0;}
      if(arg=="--data-dir"||arg=="--memory-budget"){if(positional||i+1==argc)throw std::runtime_error("invalid or mixed command-line arguments");auto value=std::string(argv[++i]);if(arg=="--data-dir"){if(value.empty())throw std::runtime_error("empty data directory");directory=value;}else {budget=positive_bytes(value);budget_source="explicit";}}
      else if(i==1&&!arg.starts_with("-")){positional=true;directory=arg;if(i+1<argc){budget=positive_bytes(argv[++i]);budget_source="explicit";}if(i+1<argc)throw std::runtime_error("too many positional arguments");}
      else throw std::runtime_error("unknown command-line argument: "+arg);
    }
    auto progress=[](size_t count){auto env=std::getenv("MVDB_RECOVERY_PAUSE_AFTER");if(env&&count==std::stoull(env)){std::cerr<<"RECOVERY_PAUSED "<<count<<std::endl;::raise(SIGSTOP);}};
    mvdb::Engine engine(directory,budget,progress);
    std::cerr<<"distance_kernel="<<(mvdb::avx2_available()?"avx2":"scalar")<<'\n';
    std::string sql;
    while(std::getline(std::cin,sql)){
      auto before=engine.vector_page_reads();
      auto spill_before=engine.spill_bytes();
      try{auto result=engine.execute(sql);std::cout<<"{\"ok\":true,\"affected_rows\":"<<result.affected_rows<<",\"columns\":[";
        bool first=true;for(auto& name:result.columns){if(!first)std::cout<<',';first=false;std::cout<<mvdb::quote(name);}std::cout<<"],\"rows\":[";first=true;
        result.each([&](const auto& row){if(!first)std::cout<<',';first=false;std::cout<<'[';bool cell=true;for(auto& value:row){if(!cell)std::cout<<',';cell=false;std::cout<<mvdb::json(value);}std::cout<<']';});std::cout<<"]}\n"<<std::flush;
      }catch(const std::exception& e){std::cout<<"{\"ok\":false,\"error\":"<<mvdb::quote(e.what())<<"}\n"<<std::flush;}
      std::cerr<<"METRICS {\"engine_budget_bytes\":"<<budget<<",\"budget_source\":"<<mvdb::quote(budget_source)<<",\"wal_syncs\":"<<engine.wal_syncs()<<",\"vector_page_reads\":"<<(engine.vector_page_reads()-before)<<",\"budget_peak\":"<<engine.budget_peak()<<",\"spill_bytes\":"<<(engine.spill_bytes()-spill_before)<<",\"data_page_write_bytes\":"<<engine.data_page_write_bytes()<<",\"undo_spill_bytes\":"<<engine.undo_spill_bytes()<<",\"wal_bytes_written\":"<<engine.wal_bytes_written()<<",\"nlist\":"<<engine.last_nlist()<<",\"recommended_nprobe\":"<<engine.last_recommended_nprobe()<<",\"effective_nprobe\":"<<engine.effective_nprobe()<<",\"ann_route\":"<<mvdb::quote(engine.last_route())<<"}"<<std::endl;
      if(!std::cout)return 3;
    }
    engine.close();
    auto read_bytes=proc_value("/proc/self/io","read_bytes"),write_bytes=proc_value("/proc/self/io","write_bytes");
    auto high_water=proc_value("/proc/self/status","VmHWM")*1024;
    std::cerr<<"FINAL_METRICS {\"engine_budget_bytes\":"<<budget<<",\"budget_source\":"<<mvdb::quote(budget_source)<<",\"wal_syncs\":"<<engine.wal_syncs()<<",\"data_page_write_bytes\":"<<engine.data_page_write_bytes()<<",\"undo_spill_bytes\":"<<engine.undo_spill_bytes()<<",\"wal_bytes_written\":"<<engine.wal_bytes_written()<<",\"budget_peak\":"<<engine.budget_peak()<<",\"process_read_bytes\":"<<read_bytes<<",\"process_write_bytes\":"<<write_bytes<<",\"vm_hwm_bytes\":"<<high_water<<"}"<<std::endl;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
