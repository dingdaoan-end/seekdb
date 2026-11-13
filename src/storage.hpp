#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

namespace mvdb {
constexpr size_t PAGE = 16384;
using Page = std::array<std::byte, PAGE>;
using SlotId = uint32_t;
struct MemoryBudget {
  size_t limit, used = 0, peak=0;
  std::shared_ptr<char> lifetime=std::make_shared<char>();
  explicit MemoryBudget(size_t bytes) : limit(bytes) {}
  MemoryBudget(const MemoryBudget&)=delete;
  MemoryBudget& operator=(const MemoryBudget&)=delete;
  void acquire(size_t bytes);
  void release(size_t bytes) { used -= bytes; }
};

// A fixed userspace allocation: frame and replacement metadata grow only with
// frame count. Buffered I/O also uses reclaimable, externally charged kernel cache.
class BufferPool {
  struct Frame { std::string file; uint64_t page=0, age=0, version=0; bool valid=false, dirty=false, uncommitted=false, metadata=false; uint32_t pins=0; };
  std::filesystem::path directory;
  MemoryBudget& budget;
  std::vector<Frame> frames;
  std::byte* arena=nullptr;
  uint64_t clock=0;
  size_t reservation=0;
  std::unordered_map<std::string,size_t> lookup;
  std::unordered_map<std::string,int> descriptors;
  int descriptor(const std::string&);
  size_t pin(const std::string&, uint64_t);
  void flush(size_t);
public:
  struct PageGuard {
    BufferPool* owner=nullptr;size_t frame=0;const std::byte* data=nullptr;
    PageGuard()=default;
    PageGuard(BufferPool* o,size_t f,const std::byte* p):owner(o),frame(f),data(p){}
    PageGuard(const PageGuard&)=delete;
    PageGuard& operator=(const PageGuard&)=delete;
    PageGuard(PageGuard&& g) noexcept :owner(g.owner),frame(g.frame),data(g.data){g.owner=nullptr;}
    PageGuard& operator=(PageGuard&& g) noexcept {if(this!=&g){reset();owner=g.owner;frame=g.frame;data=g.data;g.owner=nullptr;}return *this;}
    ~PageGuard(){reset();}
    void reset(){if(owner){--owner->frames[frame].pins;owner=nullptr;}}
  };
  std::function<void(const std::string&,uint64_t)> before_flush;
  bool transaction_active=false;
  struct ResidentHandle { size_t frame=0; uint64_t version=0; };
  // Lookup only: never reads, ages, or admits a page.
  std::optional<ResidentHandle> find(const std::string&,uint64_t) const;
  ResidentHandle handle(const std::string&,uint64_t);
  const std::byte* resident(ResidentHandle) const;
  void append_extent(const std::string&,uint64_t,const Page&,size_t,size_t);
  void append_extent_bytes(const std::string&,uint64_t,size_t,const void*,size_t);
  void flush_page(const std::string&,uint64_t);
  void committed();
  void invalidate_prefix(const std::string&);
  void sync_all(bool release_cache=true);
  uint64_t physical_reads=0,vector_reads=0,data_page_write_bytes=0;
  BufferPool(std::filesystem::path, MemoryBudget&);
  ~BufferPool();
  BufferPool(const BufferPool&)=delete;
  Page get(const std::string&, uint64_t);
  // Borrowed until the next pool call; distance kernels consume immediately.
  const std::byte* view(const std::string&, uint64_t);
  PageGuard lease(const std::string&,uint64_t);
  void put(const std::string&, uint64_t, const Page&);
  void flush_all();
};

class Storage {
  std::filesystem::path root;
  int wal=-1, undo=-1, shadow=-1;
  void sync_wal();
  std::filesystem::path shadow_directory, shadow_index, written_directory;
  struct WrittenEntry { std::array<char,128> file{}; bool status=false; };
  std::unique_ptr<WrittenEntry[]> written_cache;
  size_t written_count=0;
  void cache_written(const std::string&,bool);
  size_t written_cursor=0, written_reservation=0;
  bool normal_written(const std::string&);
  void mark_written(const std::string&);
  std::vector<std::string> regions, sealed_regions;
  void protect(const std::string&,uint64_t);
  void restore_shadow();
  bool fresh_region(const std::string&) const;
  void discard_region(const std::string&);
  uint64_t undo_top=0, generation=0, transaction_start=0;
  uint32_t transaction_crc=0;
  bool active=false, closed=false;
  std::vector<char> undo_memory;
  size_t undo_capacity=0, redo_capacity=0;
  std::string redo_buffer;
  void flush_redo();
  void record(char, const std::string& payload={});
  void undo_write(const char*,size_t,uint64_t);
  void undo_read(char*,size_t,uint64_t);
  void write_redo(const std::string&, uint64_t, const Page&);
  void recover(const std::function<void(size_t)>&);
public:
  MemoryBudget budget;
  BufferPool pool;
  Storage(std::filesystem::path, size_t, std::function<void(size_t)> progress={});
  ~Storage();
  // Explicit close propagates checkpoint errors; repeated calls are harmless.
  void close();
  // Anonymous, unbuffered spill file on the same writable volume as the DB.
  std::shared_ptr<FILE> temporary();
  uint64_t undo_spill_bytes=0, wal_bytes_written=0, shadow_bytes_written=0;
  // Includes recovery and checkpoint epoch initialization; use deltas for commits.
  uint64_t wal_syncs=0;
  void checkpoint(const std::function<void(size_t)>& stage={});
  bool in_transaction() const { return active; }
  uint64_t savepoint() const { return undo_top; }
  void begin();
  void commit();
  void rollback_to(uint64_t);
  void rollback();
  Page read(const std::string& f,uint64_t p) { return pool.get(f,p); }
  void write(const std::string&,uint64_t,const Page&);
  // Only logically invisible new slots/tails; rollback leaves unreachable garbage.
  // Previously logged or shadowed files automatically use normal WAL/undo.
  void append(const std::string&,uint64_t,const Page&,size_t offset,size_t length);
  // One contiguous page batch of invisible slots; callers publish their
  // logical row count through normal WAL only after all payload batches finish.
  void append_bytes(const std::string&,uint64_t,size_t offset,const void*,size_t length);
  // Sync a phase boundary; release only cache already represented in frames.
  void reclaim_clean_pages();
  // Allocation markers reserve unique names permanently. Fresh payload omits
  // redo; seal syncs it before publishing metadata. Undo owns it until COMMIT.
  std::string begin_region(const std::string& prefix);
  void write_new(const std::string&,uint64_t,const Page&);
  void seal_region(const std::string&);
  template<class T> T load(const std::string& f,uint64_t p) {
    static_assert(sizeof(T)<=PAGE); auto page=read(f,p); T t{};
    std::memcpy(&t,page.data(),sizeof(t)); return t;
  }
  template<class T> void store(const std::string& f,uint64_t p,const T& t) {
    static_assert(sizeof(T)<=PAGE); Page page{};
    std::memcpy(page.data(),&t,sizeof(t)); write(f,p,page);
  }
};

class PrimaryIndex {
  Storage& storage;
  std::string file;
  struct Header { uint32_t root=0,next=1; };
  static constexpr size_t ORDER=1000;
  struct Node {
    uint32_t leaf=1,n=0;
    int64_t keys[ORDER+1]{};
    uint32_t slots[ORDER+1]{};
    uint32_t children[ORDER+2]{};
  };
  static_assert(sizeof(Node)<=PAGE,"primary index node exceeds page");
  struct Split { int64_t key; uint32_t right; };
  uint32_t allocate(const Node&);
  std::optional<Split> insert_node(uint32_t,int64_t,SlotId);
public:
  PrimaryIndex(Storage& s,std::string f):storage(s),file(std::move(f)){}
  std::optional<SlotId> find(int64_t);
  void insert(int64_t,SlotId);
  void erase(int64_t);
};
}
