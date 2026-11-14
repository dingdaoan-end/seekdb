#include "engine.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// In-process diagnostic only: not the competition's end-to-end T. Use an
// expendable COPY of a loaded database; normal close may checkpoint that copy.
int main(int argc,char** argv){
  if(argc<4||argc>5){std::cerr<<"usage: benchmark_cpu DATA_COPY QUERY_SQL_LINES exact|ann [BUDGET_BYTES]\n";return 2;}
  try{
    size_t budget=argc==5?std::stoull(argv[4]):12*1024*1024;
    std::ifstream input(argv[2]);if(!input)throw std::runtime_error("cannot read SQL file");
    std::vector<std::string> queries;std::string line;
    while(std::getline(input,line))if(!line.empty())queries.push_back(line);
    if(queries.empty())throw std::runtime_error("no queries");
    using Clock=std::chrono::steady_clock;
    uint64_t checksum=0;auto start=Clock::now();
    for(unsigned r=0;r<5;++r)for(auto& sql:queries){auto s=mvdb::parse(sql);checksum+=s.expressions.size();}
    double parse_seconds=std::chrono::duration<double>(Clock::now()-start).count()/5;
    mvdb::Engine engine(argv[1],budget);engine.execute(std::string("SET SEARCH_MODE = ")+argv[3]+";");
    // A full pass establishes the same deterministic in-process warmup.
    for(auto& sql:queries)engine.execute(sql).each([&](const auto& row){checksum+=static_cast<uint64_t>(std::get<int64_t>(row[0]));});
    engine.reset_pruning_counters();
    auto pages=engine.vector_page_reads();start=Clock::now();
    for(unsigned r=0;r<5;++r)for(auto& sql:queries)
      engine.execute(sql).each([&](const auto& row){checksum+=static_cast<uint64_t>(std::get<int64_t>(row[0]));});
    double execute_seconds=std::chrono::duration<double>(Clock::now()-start).count()/5;
    auto reads=engine.vector_page_reads()-pages;
    const auto& pruning=engine.pruning_counters();
    const auto& radial=engine.radial_counters();
    const double trials=pruning.trials?double(pruning.trials):1;
    std::cout<<"{\"queries\":"<<queries.size()<<",\"mode\":"<<mvdb::quote(argv[3])
      <<",\"parse_seconds\":"<<parse_seconds<<",\"engine_seconds\":"<<execute_seconds
      <<",\"vector_page_loads_measured\":"<<reads<<",\"budget_bytes\":"<<budget
      <<",\"last_query_pruning_policy\":"<<mvdb::quote(engine.last_pruning_policy())
      <<",\"pruning_trials\":"<<pruning.trials<<",\"pruned32\":"<<pruning.pruned32<<",\"pruned64\":"<<pruning.pruned64
      <<",\"pruned32_rate\":"<<pruning.pruned32/trials<<",\"pruned64_rate\":"<<pruning.pruned64/trials
      <<",\"radial_tested\":"<<radial.tested<<",\"radial_rejected\":"<<radial.rejected
      <<",\"radial_late_rejected\":"<<radial.late_rejected<<",\"radial_pages_skipped\":"<<radial.pages_skipped
      <<",\"checksum\":"<<checksum<<"}\n";
    engine.close();
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
