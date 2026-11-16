#include "../src/engine.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
namespace {
std::string vec(float x,bool uniform=false){std::string out="[";for(unsigned j=0;j<128;++j){if(j)out+=',';out+=std::to_string(uniform||j==0?x:0.f);}return out+"]";}
using Rows=std::vector<std::vector<mvdb::Value>>;
Rows rows(mvdb::Engine& e,const std::string& sql){Rows out;e.execute(sql).each([&](auto& row){out.push_back(row);});return out;}
struct Env {std::string name,value;bool existed;Env(const char* n):name(n){auto p=std::getenv(n);existed=p;if(p)value=p;}~Env(){if(existed)::setenv(name.c_str(),value.c_str(),1);else ::unsetenv(name.c_str());}};
}
void final_query_regression(){
  Env mode("MVDB_PROGRESSIVE"),dict("MVDB_DICT_SLICES"),expand("MVDB_FILTER_EXPAND");
  ::setenv("MVDB_PROGRESSIVE","auto",1);::unsetenv("MVDB_DICT_SLICES");::unsetenv("MVDB_FILTER_EXPAND");
  auto path=std::filesystem::temp_directory_path()/("mvdb_final_query_"+std::to_string(::getpid()));assert(!std::filesystem::exists(path));
  const auto q="SELECT id,L2_DISTANCE(v,'"+vec(0)+"') AS d FROM t ";
  {
    mvdb::Engine e(path,1024*1024);
    e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,cat VARCHAR(32),tag VARCHAR(32),v VECTOR(128));");
    for(unsigned base=0;base<512;base+=16){std::string sql="INSERT INTO t VALUES ";for(unsigned j=base;j<base+16;++j){if(j!=base)sql+=',';auto cluster=j/64;sql+='('+std::to_string(j)+",'c"+std::to_string(cluster)+"','"+(j%2?"odd":"even")+"','"+vec(cluster*100+j%64)+"')";}e.execute(sql+";");}
    const auto filtered=q+"WHERE cat='c7' ORDER BY d,id LIMIT 10;";
    const auto expected=rows(e,filtered);assert(expected.size()==10);
    ::setenv("MVDB_DICT_SLICES","off",1);assert(rows(e,filtered)==expected);::unsetenv("MVDB_DICT_SLICES");
    assert(rows(e,q+"WHERE cat='c7' AND tag='even' ORDER BY d,id LIMIT 10;").size()==10);
    assert(rows(e,q+"WHERE cat='c7' AND cat='c0' ORDER BY d,id LIMIT 10;").empty());
    assert(rows(e,q+"WHERE cat='absent' ORDER BY d,id LIMIT 10;").empty());
    e.execute("BEGIN;");e.execute("UPDATE t SET cat='moved' WHERE id=448;");
    assert(rows(e,q+"WHERE cat='moved' ORDER BY d,id LIMIT 10;").size()==1);
    e.execute("DELETE FROM t WHERE id=449;");assert(rows(e,filtered).size()==10);
    e.execute("ROLLBACK;");assert(rows(e,filtered)==expected);
    for(unsigned j=512;j<520;++j)e.execute("INSERT INTO t VALUES ("+std::to_string(j)+",'rare','even','"+vec(999)+"');");
    e.execute("CREATE VECTOR INDEX vi ON t(v);");e.close();
  }
  {
    // Cold pages and new probe slices must revalidate all borrowed addresses.
    mvdb::Engine e(path,1024*1024);e.execute("SET SEARCH_MODE = ann;");e.execute("SET NPROBE = 1;");
    auto filtered=q+"WHERE cat='c7' ORDER BY d,id LIMIT 10;";
    ::setenv("MVDB_FILTER_EXPAND","off",1);assert(rows(e,filtered).empty());
    ::unsetenv("MVDB_FILTER_EXPAND");assert(rows(e,filtered).size()==10);assert(e.effective_nprobe()>1);
    e.execute("SET SEARCH_MODE = exact;");auto expected=rows(e,filtered);e.execute("SET SEARCH_MODE = ann;");assert(rows(e,filtered)==expected);
    assert(rows(e,q+"WHERE cat='rare' ORDER BY d,id LIMIT 10;").size()==8);assert(e.last_route()=="exact_fallback");assert(e.effective_nprobe()==0);
    // Projection forces the generic heap and LIMIT forces external sorting.
    auto generic="SELECT id,cat,L2_DISTANCE(v,'"+vec(0)+"') AS d FROM t WHERE cat='c7' ORDER BY d,id LIMIT ";
    assert(rows(e,generic+"10;").size()==10);assert(e.effective_nprobe()>1);
    assert(rows(e,generic+"5000;").size()==64);assert(e.effective_nprobe()==e.last_nlist());
    // A tiny AND intersection may use a larger seed slice but must verify all predicates.
    assert(rows(e,q+"WHERE cat='c7' AND tag='even' ORDER BY d,id LIMIT 40;").size()==32);
    e.close();
  }
  std::filesystem::remove_all(path);
  for(bool uniform:{false,true}){
    mvdb::Engine e(path,2*1024*1024);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(128));");
    for(unsigned base=0;base<256;base+=16){std::string sql="INSERT INTO t VALUES ";for(unsigned j=base;j<base+16;++j){if(j!=base)sql+=',';sql+='('+std::to_string(j)+",'"+vec(uniform?1.f:float(j),uniform)+"')";}e.execute(sql+";");}
    auto sql=q+"ORDER BY d,id LIMIT 10;";::setenv("MVDB_PROGRESSIVE","off",1);auto expected=rows(e,sql);rows(e,sql);
    auto before=e.bounded_distance_attempts(),fallback=e.bounded_distance_fallbacks(),pruned=e.bounded_distance_pruned();
    ::setenv("MVDB_PROGRESSIVE","auto",1);assert(rows(e,sql)==expected);assert(e.bounded_distance_attempts()>before);
    if(uniform){assert(e.bounded_distance_attempts()-before==64);assert(e.bounded_distance_fallbacks()==fallback+1);}
    else assert(e.bounded_distance_pruned()>pruned);
    if(uniform){auto attempts=e.bounded_distance_attempts();::setenv("MVDB_PROGRESSIVE","retry",1);assert(rows(e,sql)==expected);assert(e.bounded_distance_attempts()-attempts==64);}
    for(const char* policy:{"32","64","retry","strict"}){::setenv("MVDB_PROGRESSIVE",policy,1);assert(rows(e,sql)==expected);}
    e.close();std::filesystem::remove_all(path);
  }
  {
    mvdb::Engine e(path,2*1024*1024);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(128));");
    for(unsigned base=0;base<512;base+=16){std::string sql="INSERT INTO t VALUES ";for(unsigned j=base;j<base+16;++j){if(j!=base)sql+=',';sql+='('+std::to_string(j)+",'"+vec(j>=128&&j<138?0.f:10.f,true)+"')";}e.execute(sql+";");}
    auto sql=q+"ORDER BY d,id LIMIT 10;";::setenv("MVDB_PROGRESSIVE","off",1);auto expected=rows(e,sql);rows(e,sql);
    auto attempts=e.bounded_distance_attempts(),fallbacks=e.bounded_distance_fallbacks(),pruned=e.bounded_distance_pruned();
    ::setenv("MVDB_PROGRESSIVE","retry",1);assert(rows(e,sql)==expected);
    assert(e.bounded_distance_attempts()-attempts>=128);assert(e.bounded_distance_fallbacks()>fallbacks);assert(e.bounded_distance_pruned()>pruned);
    e.close();std::filesystem::remove_all(path);
  }
}
#ifdef MVDB_FINAL_QUERY_STANDALONE
int main(){final_query_regression();}
#endif
