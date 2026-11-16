#include "../src/engine.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <unistd.h>
namespace {bool count_allocations=false;size_t vector_allocations=0,total_allocations=0;}
void* operator new(std::size_t n){if(count_allocations){++total_allocations;if(n==512)++vector_allocations;}if(auto p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
int main(){
  // An independent encoder oracle includes binary strings and all value tags.
  std::vector<mvdb::Value> packed_row{int64_t(-123),-0.0,std::string("a\0b",3),std::vector<float>{1.25f,-0.0f,3.5f}};
  std::string expected;auto append=[&](const auto& value){expected.append(reinterpret_cast<const char*>(&value),sizeof(value));};
  append(uint32_t(4));append(uint8_t(0));append(int64_t(-123));append(uint8_t(1));append(-0.0);
  append(uint8_t(2));append(uint32_t(3));expected.append("a\0b",3);append(uint8_t(3));append(uint32_t(3));
  for(float x:{1.25f,-0.0f,3.5f})append(x);
  std::string packed="previous output";mvdb::pack_values_into(packed_row,packed);assert(packed==expected&&mvdb::pack_values(packed_row)==expected);
  auto owned=mvdb::unpack_values(packed);auto original=packed;
  const size_t before_pack_allocations=total_allocations;count_allocations=true;
  for(unsigned i=0;i<10000;++i)mvdb::pack_values_into(packed_row,packed);
  count_allocations=false;assert(total_allocations==before_pack_allocations);
  mvdb::pack_values_into({},packed);assert(packed==std::string(4,'\0')&&std::get<std::string>(owned[2])==std::string("a\0b",3));
  mvdb::pack_values_into(packed_row,packed);assert(packed==original);
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
  std::filesystem::remove_all(root);
  for(size_t budget:{size_t(64*1024),size_t(1024*1024),size_t(4*1024*1024)})for(bool reversed:{false,true}){
    mvdb::Engine e(root,budget);e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(4096));");
    const size_t width=budget==64*1024?256:3000;
    // Alternate tiny and wide records to force reuse, growth, and low-budget fallback.
    std::string sql="INSERT INTO t VALUES ";for(int i=0;i<12;++i){if(i)sql+=',';sql+='('+std::to_string(reversed?12-i:i+1)+",'"+std::string(i%3?width:1,'a'+i)+"')";}
    assert(e.execute(sql).affected_rows==12);assert(e.budget_peak()<=budget);
    size_t count=0;for(int key=1;key<=12;++key)e.execute("SELECT id,s FROM t WHERE id="+std::to_string(key)+";").each([&](const auto& row){int i=reversed?12-int(std::get<int64_t>(row[0])):int(std::get<int64_t>(row[0]))-1;assert(std::get<std::string>(row[1])==std::string(i%3?width:1,'a'+i));++count;});assert(count==12);
    bool failed=false;try{e.execute("INSERT INTO t VALUES (100,'first'),(101,'second'),(100,'duplicate');");}catch(...){failed=true;}assert(failed&&e.active_rows("t")==12);
    e.close();std::filesystem::remove_all(root);
  }
  std::cout<<"10000 reused vector parses: allocations="<<vector_allocations<<"; 10000 warmed pack calls: allocations=0; production sequential/ordered INSERT, failure,rollback,restart,wide/tiny budget PASS\n";
}
