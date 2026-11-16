#include "../src/engine.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <unistd.h>
namespace {bool count_allocations=false;size_t vector_allocations=0;}
void* operator new(std::size_t n){if(count_allocations&&n==512)++vector_allocations;if(auto p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
int main(){
  mvdb::Column col{"v",mvdb::Type::Vector,128};std::string literal="[";
  for(unsigned i=0;i<128;++i){if(i)literal+=',';literal+="0.123456789012345";}literal+=']';
  auto oracle=std::get<std::vector<float>>(mvdb::convert(mvdb::LiteralView{literal,true,false},col));
  mvdb::Value scratch;count_allocations=true;
  for(unsigned i=0;i<10000;++i){mvdb::convert_into({literal,true,false},col,scratch);auto& v=std::get<std::vector<float>>(scratch);assert(v==oracle);v.clear();}
  count_allocations=false;assert(vector_allocations==1);
  for(auto bad:{"[NaN]","[Inf]","[1e999]","[0]","[''0]"}){
    bool failed=false;try{mvdb::convert_into({bad,true,false},col,scratch);}catch(...){failed=true;}assert(failed);
    mvdb::convert_into({literal,true,false},col,scratch);assert(std::get<std::vector<float>>(scratch)==oracle);
  }
  const auto root=std::filesystem::temp_directory_path()/("mvdb_structural_insert_"+std::to_string(::getpid()));
  assert(!std::filesystem::exists(root));
  {
    mvdb::Engine e(root,4*1024*1024);e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8),v VECTOR(128));");
    for(bool reversed:{false,true}){
      std::string sql="INSERT INTO t VALUES ";for(unsigned i=0;i<100;++i){if(i)sql+=',';unsigned id=(reversed?200-i:i+1);auto row_literal=literal;row_literal.replace(1,row_literal.find(',')-1,std::to_string(id));sql+='('+std::to_string(id)+",'ok','"+row_literal+"')";}e.execute(sql+";");
    }
    size_t count=0;e.execute("SELECT id,v FROM t ORDER BY id;").each([&](auto& row){auto& v=std::get<std::vector<float>>(row[1]);assert(v.size()==oracle.size()&&v[0]==float(std::get<int64_t>(row[0]))&&std::equal(v.begin()+1,v.end(),oracle.begin()+1));++count;});assert(count==200);
    // Non-terminated borrowed SQL must not consume the following request bytes.
    std::string framed="SELECT id,L2_DISTANCE(v,'"+literal+"') AS d FROM t ORDER BY d,id LIMIT 10;INVALID";
    auto top=e.execute(std::string_view(framed.data(),framed.size()-7));
    assert(top.numeric_count==10&&!top.memory_rows);auto saved=top.numeric_rows;
    e.execute("BEGIN");e.execute("DELETE FROM t WHERE id=1");e.execute("ROLLBACK");
    assert(top.numeric_rows[0].key==saved[0].key&&top.numeric_rows[0].distance==saved[0].distance);
    auto reverse=e.execute("SELECT L2_DISTANCE(v,'"+literal+"') AS d,id FROM t ORDER BY d,id LIMIT 10;");
    assert(reverse.numeric_count==10&&reverse.numeric_distance_column==0);
    auto wide=e.execute("SELECT id,L2_DISTANCE(v,'"+literal+"') AS d FROM t ORDER BY d,id LIMIT 11;");
    assert(wide.memory_rows&&wide.memory_rows->size()==11);
    e.execute("BEGIN;");e.execute("INSERT INTO t VALUES (1000,'ok','"+literal+"');");
    bool failed=false;try{e.execute("INSERT INTO t VALUES (1001,'ok','"+literal+"'),(1002,'ok','[NaN]');");}catch(...){failed=true;}assert(failed);
    count=0;e.execute("SELECT id FROM t WHERE id>=1000 ORDER BY id;").each([&](auto& row){assert(std::get<int64_t>(row[0])==1000);++count;});assert(count==1);e.execute("ROLLBACK;");e.close();
  }
  {mvdb::Engine e(root,4*1024*1024);size_t count=0;e.execute("SELECT id,v FROM t ORDER BY id;").each([&](auto& row){auto& v=std::get<std::vector<float>>(row[1]);assert(v.size()==oracle.size()&&v[0]==float(std::get<int64_t>(row[0]))&&std::equal(v.begin()+1,v.end(),oracle.begin()+1));++count;});assert(count==200);e.close();}
  std::filesystem::remove_all(root);std::cout<<"10000 reused vector parses: allocations="<<vector_allocations<<"; production sequential/ordered INSERT, failure,rollback,restart PASS\n";
}
