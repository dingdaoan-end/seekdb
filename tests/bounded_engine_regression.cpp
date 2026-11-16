#include "../src/engine.hpp"
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <unistd.h>

void bounded_engine_regression(){
  const char* before=std::getenv("MVDB_PROGRESSIVE");
  std::string saved=before?before:"";bool had=before;
  auto path=std::filesystem::temp_directory_path()/("mvdb_bounded_heap_"+std::to_string(::getpid()));
  assert(!std::filesystem::exists(path));
  {
    mvdb::Engine e(path,8*1024*1024);
    e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(128));");
    auto vector=[](float x,size_t position){std::string s="[";char buf[64];
      auto r=std::to_chars(buf,buf+sizeof(buf),x);assert(r.ec==std::errc{});
      const auto number=x==std::numeric_limits<float>::max()?std::string("340282346638528859811704183484516925440"):std::string(buf,r.ptr);
      for(size_t j=0;j<128;++j){if(j)s+=',';s+=(j==position?number:"0");}return s+"]";};
    std::string insert="INSERT INTO t VALUES ";
    for(int i=0;i<80;++i){if(i)insert+=',';
      float x=i%3?1.f:std::nextafter(1.f,2.f);
      if(i==77)x=std::numeric_limits<float>::max();
      if(i==78)x=std::numeric_limits<float>::denorm_min();
      if(i==79)x=0;
      insert+='('+std::to_string(100-i)+",'"+vector(x,i%128)+"')";
    }
    e.execute(insert+";");
    auto run=[&](const char* mode,const std::string& sql){
      ::setenv("MVDB_PROGRESSIVE",mode,1);
      std::vector<std::vector<mvdb::Value>> out;
      e.execute(sql).each([&](const auto& row){out.push_back(row);});return out;
    };
    auto compare=[&]{for(float x:{0.f,1e-20f,1.f}){
      auto sql="SELECT id,L2_DISTANCE(v,'"+vector(x,0)+"') AS d FROM t ORDER BY d,id LIMIT 10;";
      auto expected=run("off",sql);assert(expected.size()==10);
      assert(run("32",sql)==expected);assert(run("64",sql)==expected);assert(run("auto",sql)==expected);
    }};
    compare();assert(e.bounded_distance_attempts()>0);
    e.execute("CREATE VECTOR INDEX vi ON t(v);");
    e.execute("SET SEARCH_MODE = ann;");e.execute("SET NPROBE = 8;");compare();
    e.execute("BEGIN;");e.execute("DELETE FROM t WHERE id=21;");compare();
    e.execute("ROLLBACK;");compare();e.close();
  }
  std::filesystem::remove_all(path);
  if(had)::setenv("MVDB_PROGRESSIVE",saved.c_str(),1);else ::unsetenv("MVDB_PROGRESSIVE");
}
