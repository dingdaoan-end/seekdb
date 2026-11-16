#pragma once
#include "sql.hpp"
#include "storage.hpp"
#include <cstdio>
#include <memory>
namespace mvdb {
// Own the two common TopK labels without allocating a vector per response.
// Larger projections retain ordinary contiguous vector storage.
class ResultColumns {
  std::array<std::string,2> small{};
  std::vector<std::string> large;
  size_t count=0;
public:
  ResultColumns()=default;
  ResultColumns(const ResultColumns&)=default;
  ResultColumns& operator=(const ResultColumns& other){if(this!=&other){ResultColumns copy(other);*this=std::move(copy);}return *this;}
  ResultColumns(ResultColumns&& other)noexcept:small(std::move(other.small)),large(std::move(other.large)),count(other.count){other.count=0;}
  ResultColumns& operator=(ResultColumns&& other)noexcept{if(this!=&other){small=std::move(other.small);large=std::move(other.large);count=other.count;other.count=0;}return *this;}
  ResultColumns(std::initializer_list<std::string> names){for(auto& n:names)push_back(n);}
  ResultColumns& operator=(std::initializer_list<std::string> names){clear();for(auto& n:names)push_back(n);return *this;}
  size_t size()const{return count;}
  bool empty()const{return !count;}
  void clear(){large.clear();for(auto& n:small)n.clear();count=0;}
  void reserve(size_t n){if(n>2){if(large.capacity()<n)large.reserve(n);if(large.empty())for(size_t i=0;i<count;++i)large.push_back(std::move(small[i]));}}
  void push_back(std::string name){if(count<2&&large.capacity()==0)small[count]=std::move(name);else{reserve(count+1);large.push_back(std::move(name));}++count;}
  std::string* begin(){return large.capacity()?large.data():small.data();}
  const std::string* begin()const{return large.capacity()?large.data():small.data();}
  std::string* end(){return begin()+count;}
  const std::string* end()const{return begin()+count;}
  std::string& operator[](size_t i){return begin()[i];}
  const std::string& operator[](size_t i)const{return begin()[i];}
  std::string& back(){return (*this)[count-1];}
  const std::string& back()const{return (*this)[count-1];}
};
namespace detail {
// A bounded table can afford one exact pass for all calibration samples even
// when its vector payload does not fit. Larger tables retain the I/O guard.
constexpr bool skip_ivf_calibration_for_payload(uint64_t active_rows,uint64_t payload_bytes,size_t budget_bytes){
  return active_rows>25000&&payload_bytes>budget_bytes/2;
}
}
struct Result {
  struct NumericRow {int64_t key;double distance;};
  std::array<NumericRow,10> numeric_rows{};
  size_t numeric_count=0,numeric_distance_column=0;
  int64_t affected_rows=0;
  ResultColumns columns;
  std::shared_ptr<FILE> rows;
  std::shared_ptr<std::vector<std::vector<Value>>> memory_rows;
  std::shared_ptr<void> memory_reservation;
  void each(const std::function<void(const std::vector<Value>&)>&) const;
};
class Engine {
  Storage storage;
  bool ann=false;
  uint32_t nprobe=0; // Zero selects the persisted calibration and cost model.
  uint32_t last_probes=0,last_lists=0,last_recommended=0;
  std::string route="none";
  uint64_t ordered_pk_batches=0,signature_rejections=0,empty_cluster_reseeds=0;
  uint64_t borrowed_insert_batches=0;
  // Cumulative executor work: PK probes, 4096-slot validity blocks,
  // and visible candidates surviving scalar signatures (before exact predicates).
  uint64_t point_lookups=0,bitmap_blocks=0,candidate_slots=0;
  struct Header {
    uint32_t magic=0,slots=0;
    uint64_t blob_end=0;
    uint32_t nlist=0,ivector=0,schema_size=0;
    char schema[4096]{};
    uint32_t active_count=0,recommended_nprobe=0,calibration_stale=0; // Legacy on-disk field; retained for offsets.
    char index_region[128]{};
    uint64_t mutations_since_calibration=0,rows_at_calibration=0;
    uint32_t calibration_skipped=0; // Empirical recommendation is not a recall certificate.
    uint32_t calibration_queries=0;
  };
  static uint64_t drift_limit(const Header& h){return h.rows_at_calibration/5>64?h.rows_at_calibration/5:64;}
  static bool stale(const Header& h){return h.calibration_stale||h.mutations_since_calibration>drift_limit(h);}
  struct Meta {int64_t key=0;uint64_t offset=0;uint32_t length=0,active=0,bucket=0,reserved=0;};
  static_assert(sizeof(Meta)==32);
  // One bounded cache: payload vectors remain exclusively in BufferPool.
  struct TableReadCache {
    std::string table;
    uint32_t slots=0;
    std::shared_ptr<void> reservation;
    std::vector<int64_t> keys;
    std::vector<uint8_t> flags; // active and extreme
    struct Dictionary {bool supported=false;std::vector<uint16_t> ids;std::vector<std::string> values;std::vector<SlotId> slot_ids;std::vector<uint32_t> offsets;};
    std::vector<Dictionary> dictionaries;
    std::vector<float> centers;
    std::vector<SlotId> postings,active_slots;
    std::vector<uint32_t> offsets;
    std::vector<BufferPool::ResidentHandle> vector_pages;
    std::optional<BufferPool::ResidencyLease> vector_lease;
    struct Neighbor {double squared;SlotId slot;};
    std::vector<std::pair<size_t,uint16_t>> filters;
    std::vector<const std::byte*> bases;
    std::vector<SlotId> candidates;
    std::vector<std::pair<double,uint32_t>> distances;
    std::vector<Neighbor> heap;
  };
  std::unique_ptr<TableReadCache> read_cache;
  struct SelectWorkspace {
    std::shared_ptr<void> reservation;
    Statement statement;
    std::string pattern;
    Header header;
    std::vector<Column> columns;
    std::vector<size_t> selected;
    std::vector<std::vector<float>> queries;
    Value query_value;
    std::vector<std::pair<size_t,bool>> order;
    std::vector<std::pair<size_t,Value>> predicates;
  };
  std::unique_ptr<SelectWorkspace> select_workspace;
  uint64_t reused_select_queries=0;
  bool prepare_select(std::string_view,Statement&);
  bool execute_select_workspace(Result&);
  std::optional<BufferPool::ResidencyLease> prepared_vectors;
  std::string prepared_table;
  bool preparation_cache_allowed=false;
  void prepare_vector_residency(const std::string&,const Header&,const std::vector<Column>&);
  uint64_t cache_builds=0;
  uint64_t bounded_attempts=0,bounded_pruned=0,bounded_fallbacks=0;
  TableReadCache* cache(const std::string&,const Header&,const std::vector<Column>&);
  bool fast_topk(const Statement&,const Header&,const std::vector<Column>&,
                 const std::vector<Expr>&,const std::vector<size_t>&,
                 const std::vector<std::vector<float>>&,
                 const std::vector<std::pair<size_t,bool>>&,
                 const std::vector<std::pair<size_t,Value>>&,Result&);
  Header header(const std::string&);
  std::vector<Column> schema(const Header&);
  Meta meta(const std::string&,SlotId);
  void put_meta(const std::string&,SlotId,const Meta&);
  std::vector<Value> row(const std::string&,SlotId,const Meta&,const std::vector<Column>&,bool);
  const float* vector_view(const std::string&,SlotId,size_t,const Column&);
  const float* center_view(const std::string&,uint32_t,size_t);
  void write_row(const std::string&,SlotId,std::vector<Value>,const std::vector<Column>&,bool);
  std::string read_blob(const std::string&,uint64_t,uint32_t);
  void write_blob(const std::string&,uint64_t,const std::string&,bool append=false);
  void bitmap(const std::string&,SlotId,bool);
  void posting(const std::string&,uint32_t,SlotId,bool);
  struct PostingList {std::string file;uint64_t offset;uint32_t count;};
  static bool packed_index(const std::string&);
  PostingList posting_list(const std::string&,uint32_t);
  uint32_t assign(const std::string&,const Header&,const std::vector<float>&);
  void create_index(const Statement&,const std::vector<Column>&);
  std::string index_base(const std::string&,const Header&) const;
  uint32_t bucket(const std::string&,const Header&,SlotId);
  void put_bucket(const std::string&,const Header&,SlotId,uint32_t);
  uint32_t calibrate(const std::string&,Header&,const std::vector<Column>&);
  Result execute_inner(const Statement&);
  Result insert_stream(std::string_view);
  bool insert_is_ascending(std::string_view);
  Result insert_sequential(std::string_view);
  Result insert_ordered(std::string_view);
public:
  Engine(std::filesystem::path root,size_t budget=8*1024*1024,std::function<void(size_t)> progress={})
    :storage(std::move(root),budget,std::move(progress)){}
  Result execute(std::string_view sql);
  void set_search_mode(bool approximate)noexcept{ann=approximate;}
  void close(){select_workspace.reset();read_cache.reset();prepared_vectors.reset();storage.close();}
  size_t resident_frame_count() const{return storage.pool.resident_page_count();}
  size_t pinned_frame_count() const{return storage.pool.pinned_page_count();}
  size_t resident_frame_bytes() const{return storage.pool.allocated_page_bytes();}
  uint64_t released_clean_pages() const{return storage.pool.clean_page_releases;}
  uint64_t data_cache_drop_count() const{return storage.pool.cache_drop_calls;}
  void index_publication_progress(std::function<void(size_t)> hook){storage.region_progress=std::move(hook);}
  size_t read_cache_slots() const{return read_cache?read_cache->slots:0;}
  uint64_t bounded_distance_attempts() const{return bounded_attempts;}
  uint64_t bounded_distance_pruned() const{return bounded_pruned;}
  uint64_t bounded_distance_fallbacks() const{return bounded_fallbacks;}
  uint64_t read_cache_build_count() const{return cache_builds;}
  uint64_t ordered_pk_batch_count() const{return ordered_pk_batches;}
  uint64_t borrowed_insert_batch_count() const{return borrowed_insert_batches;}
  uint64_t reused_select_query_count() const{return reused_select_queries;}
  uint64_t point_lookup_count() const{return point_lookups;}
  uint64_t candidate_bitmap_blocks() const{return bitmap_blocks;}
  uint64_t candidate_slots_examined() const{return candidate_slots;}
  uint64_t page_access_count() const{return storage.pool.page_accesses;}
  uint64_t buffer_page_load_count() const{return storage.pool.physical_reads;}
  uint64_t signature_rejected_rows() const{return signature_rejections;}
  uint64_t empty_cluster_reseed_count() const{return empty_cluster_reseeds;}
  uint32_t recommended_nprobe(const std::string& t){return header(t).recommended_nprobe;}
  uint32_t active_rows(const std::string& t){return header(t).active_count;}
  bool calibration_stale(const std::string& t){return stale(header(t));}
  uint64_t mutation_count(const std::string& t){return header(t).mutations_since_calibration;}
  uint64_t rows_at_calibration(const std::string& t){return header(t).rows_at_calibration;}
  bool calibration_skipped(const std::string& t){return header(t).calibration_skipped!=0;}
  uint32_t calibration_query_count(const std::string& t){return header(t).calibration_queries;}
  uint64_t drift_threshold(const std::string& t){return drift_limit(header(t));}
  uint32_t effective_nprobe() const{return last_probes;}
  uint32_t last_nlist() const{return last_lists;}
  uint32_t last_recommended_nprobe() const{return last_recommended;}
  const std::string& last_route() const{return route;}
  uint64_t vector_page_reads() const {return storage.pool.vector_reads;}
  size_t budget_peak() const {return storage.budget.peak;}
  uint64_t spill_bytes() const;
  uint64_t data_page_write_bytes() const{return storage.pool.data_page_write_bytes;}
  uint64_t undo_spill_bytes() const{return storage.undo_spill_bytes;}
  uint64_t wal_bytes_written() const{return storage.wal_bytes_written;}
  uint64_t wal_syncs() const{return storage.wal_syncs;}
};
}
