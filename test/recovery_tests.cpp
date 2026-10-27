#include "recovery_journal.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace pump::recovery;
struct Memory final : BlobStore {
  std::map<std::string,std::vector<uint8_t>> records;
  int cut=-1, calls=0;
  bool uncertain=false;
  ReadResult read(const char *key,uint8_t *out,size_t &size) override {
    if (calls++==cut) return ReadResult::Fault;
    const auto it=records.find(key); if (it==records.end()) return ReadResult::Missing;
    if (it->second.size()>size) return ReadResult::Fault;
    size=it->second.size(); memcpy(out,it->second.data(),size); return ReadResult::Found;
  }
  bool write(const char *key,const uint8_t *data,size_t size) override {
    const bool fail=calls++==cut;
    if (!fail || uncertain) records[key]={data,data+size};
    return !fail;
  }
};
int main() {
  const uint8_t old[]={1,2,3}, next[]={9,8,7,6};
  Memory base; Journal init(base); assert(init.load()==JournalState::Empty);
  assert(init.save(old,sizeof(old))); assert(init.revision()==1);
  // Every I/O boundary, including a write persisted despite a failure return.
  for (int committed=0;committed<2;++committed) for (int cut=0;cut<4;++cut) for (bool uncertain : {false,true}) {
    Memory mem; if (committed) mem.records=base.records;
    Journal live(mem); assert(live.load()==(committed?JournalState::Ready:JournalState::Empty));
    mem.calls=0; mem.cut=cut; mem.uncertain=uncertain;
    assert(!live.save(next,sizeof(next))); assert(live.state()==JournalState::Fault);
    mem.cut=-1; Journal reboot(mem); const auto state=reboot.load();
    const bool newCommitted=cut==3 || (cut==2 && uncertain);
    if (newCommitted) {
      assert(state==JournalState::Ready && reboot.size()==sizeof(next));
      assert(!memcmp(reboot.data(),next,sizeof(next)));
    } else if (committed) {
      assert(state==JournalState::Ready && reboot.size()==sizeof(old));
      assert(!memcmp(reboot.data(),old,sizeof(old)));
    } else assert(state==JournalState::Empty);
  }
  puts("PASS journal: 16 power-loss/ambiguous-commit cases retain only selected data");
  // Corrupt each byte of the selector or the active record: no stale fallback.
  for (const char *key : {"active","slot_a"}) for (size_t byte=0;byte<base.records.at(key).size();++byte) {
    auto mem=base; mem.records[key][byte]^=0x80; Journal read(mem);
    assert(read.load()==JournalState::Fault);
    assert(!read.save(next,sizeof(next)));
  }
  puts("PASS journal: every active/selector byte corruption fails closed");
  Memory orphan=base; Journal writer(orphan); assert(writer.load()==JournalState::Ready);
  orphan.calls=0; orphan.cut=2; assert(!writer.save(next,sizeof(next)));
  orphan.cut=-1; Journal read(orphan); assert(read.load()==JournalState::Ready);
  assert(read.revision()==1 && !memcmp(read.data(),old,sizeof(old)));
  assert(read.save(next,sizeof(next)) && read.revision()==2);
  std::array<uint8_t,Journal::MaxPayload> largest{}; largest.fill(0x5a);
  assert(read.save(largest.data(),largest.size()));
  Journal maxRead(orphan); assert(maxRead.load()==JournalState::Ready && maxRead.size()==largest.size());
  assert(!memcmp(maxRead.data(),largest.data(),largest.size()));
  assert(!maxRead.save(largest.data(),largest.size()+1));
  assert(!maxRead.save(nullptr,1)); assert(!maxRead.save(largest.data(),0));
  puts("PASS journal: uncommitted newer slots ignored; retries and maximum payload roundtrip");
}
