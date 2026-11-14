#pragma once
#include "sql.hpp"
#include "storage.hpp"
#include <cstdio>
#include <memory>
namespace mvdb {
namespace detail {
// A bounded table can afford one exact pass for all calibration samples even
// when its vector payload does not fit. Larger tables retain the I/O guard.
constexpr bool skip_ivf_calibration_for_payload(uint64_t active_rows,uint64_t payload_bytes,size_t budget_bytes){
  return active_rows>25000&&payload_bytes>budget_bytes/2;
}
}
struct Result {
  int64_t affected_rows=0;
  std::vector<std::string> columns;
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
    struct Dictionary {bool supported=false;std::vector<uint16_t> ids;std::vector<std::string> values;};
    std::vector<Dictionary> dictionaries;
    std::vector<float> centers;
    std::vector<SlotId> postings,active_slots;
    std::vector<uint32_t> offsets;
    std::vector<BufferPool::ResidentHandle> vector_pages;
  };
  std::unique_ptr<TableReadCache> read_cache;
  uint64_t cache_builds=0,cache_prewarms=0,cache_prewarm_vector_reads=0;
  std::string pending_index_prewarm;
  void prewarm_read_cache(const std::string&) noexcept;
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
  uint32_t assign(const std::string&,const Header&,const std::vector<float>&);
  void create_index(const Statement&,const std::vector<Column>&);
  std::string index_base(const std::string&,const Header&) const;
  uint32_t bucket(const std::string&,const Header&,SlotId);
  void put_bucket(const std::string&,const Header&,SlotId,uint32_t);
  uint32_t calibrate(const std::string&,Header&,const std::vector<Column>&);
  Result execute_inner(const Statement&);
  Result insert_stream(const std::string&);
public:
  Engine(std::filesystem::path root,size_t budget=8*1024*1024,std::function<void(size_t)> progress={})
    :storage(std::move(root),budget,std::move(progress)){}
  Result execute(const std::string& sql);
  void close(){read_cache.reset();storage.close();}
  size_t read_cache_slots() const{return read_cache?read_cache->slots:0;}
  uint64_t read_cache_build_count() const{return cache_builds;}
  uint64_t read_cache_prewarm_count() const{return cache_prewarms;}
  uint64_t read_cache_prewarm_vector_reads() const{return cache_prewarm_vector_reads;}
  uint64_t ordered_pk_batch_count() const{return ordered_pk_batches;}
  uint64_t borrowed_insert_batch_count() const{return borrowed_insert_batches;}
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
